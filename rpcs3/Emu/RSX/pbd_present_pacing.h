#pragma once

#include "util/types.hpp"
#include "Emu/Cell/timers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>

// Early present's steady rhythm. A live frame reaches the renderer as soon as the game finishes it (hook_event::
// early_flush), and the renderer would show it at once. How long a frame takes varies, so frames shown at once reach
// the screen unevenly (online sets measured it: 3-61 % of fight frames on time). Instead the renderer shows frames on a
// steady grid at the game's vblank period that follows the frames themselves: each frame waits for its slot, a frame
// that misses its slot goes at once, and the grid moves later as soon as recent frames would miss it and earlier a small
// step per frame while they all make it. A frame is never held past the game's next vblank, so the game never waits for
// its own flip.
namespace pbd::present_pacing
{
	// The last scheduled guest vblank and the scheduled period (get_system_time, microseconds); catch-up vblanks are not
	// part of the rhythm.
	inline std::atomic<u64> vblank_us{0};
	inline std::atomic<u32> period_us{16'667};
	// Set when early flush hands a live frame to the renderer: the vblank it ran after and when its flip was written.
	inline std::atomic<u64> frame_vblank_us{0};
	inline std::atomic<u64> frame_written_us{0};

	/** Recent values of one measurement, for percentiles. */
	template <u32 Size>
	class sample_window
	{
	public:
		void add(s32 value)
		{
			m_values[m_next] = value;
			m_next = (m_next + 1) % Size;
			m_count = std::min(m_count + 1, Size);
		}
		void shift(s32 by)
		{
			for (u32 i = 0; i < m_count; ++i)
				m_values[i] -= by;
		}
		s32 percentile(u32 percent) const
		{
			if (!m_count)
				return 0;
			std::array<s32, Size> sorted{};
			std::copy_n(m_values.begin(), m_count, sorted.begin());
			const u32 at = std::min(m_count - 1, (m_count * percent) / 100);
			std::nth_element(sorted.begin(), sorted.begin() + at, sorted.begin() + m_count);
			return sorted[at];
		}
		u32 count() const
		{
			return m_count;
		}
		u32 within(s32 low, s32 high) const
		{
			return static_cast<u32>(std::count_if(m_values.begin(), m_values.begin() + m_count, [&](s32 value)
				{
					return value >= low && value <= high;
				}));
		}
		void clear()
		{
			m_next = m_count = 0;
		}

	private:
		std::array<s32, Size> m_values{};
		u32 m_next = 0, m_count = 0;
	};

	class cadence
	{
	public:
		static constexpr u32 window = 120; // two seconds of frames
		static constexpr s32 margin_us = 1'000;
		static constexpr s32 decay_us = 50; // the grid comes earlier by at most this much per frame

		/**
		 * When to show a frame ready at `ready`: its slot on the grid, at once when it missed the slot, and never after
		 * `latest`. `period` is the game's vblank period.
		 */
		u64 target(u64 ready, u64 latest, u32 period)
		{
			s64 slot = static_cast<s64>(m_slot) + period;
			const s64 at = static_cast<s64>(ready);
			// The first frame, or one far off the grid (a pause, a loading screen, a stall), starts a new rhythm.
			if (!m_slot || at + period < slot || at > slot + period / 2)
			{
				slot = at;
				m_offsets.clear();
			}
			m_offsets.add(static_cast<s32>(at - slot));
			// Recent frames should be ready a margin before their slots: the grid moves later at once when they would not
			// be, earlier by a step while they are.
			const s32 want = m_offsets.percentile(95) + margin_us;
			const s32 move = want > 0 ? want : std::max(want, -decay_us);
			slot += move;
			m_offsets.shift(move);
			m_slot = static_cast<u64>(slot);
			const u64 show = std::max(ready, m_slot);
			return std::min(show, std::max(ready, latest));
		}

	private:
		sample_window<window> m_offsets;
		u64 m_slot = 0;
	};

	/** What the renderer reports about the rhythm: where the time went and how evenly frames left. */
	struct stats
	{
		u64 paced = 0, late = 0, held_us = 0, last_shown_us = 0;
		sample_window<600> after_vblank, to_renderer, intervals;
	};

	/**
	 * Called by the renderer just before it shows a guest flip: holds a frame that early flush handed over until its
	 * slot. A frame whose vblank has already been followed by another goes at once: the game already waits for its flip.
	 */
	inline void hold(cadence& rhythm, stats& counts)
	{
		const u64 vblank = frame_vblank_us.exchange(0, std::memory_order_acq_rel);
		const u64 written = frame_written_us.exchange(0, std::memory_order_acq_rel);
		if (!vblank || !written)
			return;
		const u64 now = get_system_time();
		const u64 current = vblank_us.load(std::memory_order_acquire);
		const u32 period = period_us.load(std::memory_order_relaxed);
		counts.paced++;
		counts.after_vblank.add(static_cast<s32>(static_cast<s64>(written) - static_cast<s64>(vblank)));
		counts.to_renderer.add(static_cast<s32>(static_cast<s64>(now) - static_cast<s64>(written)));
		const bool waited_on = current != vblank;
		const u64 until = rhythm.target(now, waited_on ? now : current + period - 1'000, period);
		if (until <= now)
			counts.late++;
		else
		{
			counts.held_us += until - now;
			// Sleep to within a millisecond of the slot, then yield until it: the host's sleep is coarser than that.
			for (u64 at = now; at < until; at = get_system_time())
			{
				if (until - at > 1'500)
					std::this_thread::sleep_for(std::chrono::microseconds(until - at - 1'000));
				else
					std::this_thread::yield();
			}
		}
		const u64 shown = std::max(now, until);
		if (counts.last_shown_us)
			counts.intervals.add(static_cast<s32>(std::min<u64>(shown - counts.last_shown_us, 1'000'000)));
		counts.last_shown_us = shown;
	}
} // namespace pbd::present_pacing
