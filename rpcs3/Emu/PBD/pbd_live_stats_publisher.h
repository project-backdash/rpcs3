#pragma once

#include "pbd_live_stats_state.h"

namespace pbd::live_stats
{
	constexpr std::uint32_t sat32(std::uint64_t value) noexcept
	{
		return value > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(value);
	}

	constexpr std::uint64_t non_negative(std::int64_t value) noexcept
	{
		return value < 0 ? 0 : static_cast<std::uint64_t>(value);
	}

	// Microseconds from nanoseconds, rounded.
	constexpr std::uint32_t us_from_ns(std::int64_t ns) noexcept
	{
		return sat32((non_negative(ns) + 500) / 1000);
	}

	// Milliticks from a tick count, truncated toward zero like the latch's own advantage rows (static_cast<s32>(x * 1000)).
	constexpr std::int32_t milli_from_ticks(double ticks) noexcept
	{
		if (!(ticks == ticks))
			return 0; // NaN
		const double milli = ticks * 1000.0;
		if (milli >= 2147483647.0)
			return 2147483647;
		if (milli <= -2147483647.0)
			return -2147483647;
		return static_cast<std::int32_t>(milli);
	}

	// Owner-private running statistics (the latch's owner PPU only).
	struct running
	{
		std::uint64_t sum = 0, count = 0, peak = 0, last = 0;
		constexpr void add(std::uint64_t value) noexcept
		{
			sum += value;
			count++;
			peak = (std::max)(peak, value);
			last = value;
		}
		constexpr std::uint64_t mean() const noexcept
		{
			return count ? (sum + count / 2) / count : 0;
		}
	};

	// Mean of the newest min(n, 32) round trips in O(1). 32 is the window the latch already uses for its one-way estimate.
	struct rtt_window
	{
		static constexpr std::uint32_t size = 32;
		std::array<std::uint32_t, size> ring{};
		std::uint64_t count = 0, sum = 0;
		constexpr void add(std::uint32_t value) noexcept
		{
			auto& slot = ring[count % size];
			if (count >= size)
				sum -= slot;
			slot = value;
			sum += value;
			count++;
		}
		constexpr std::uint32_t mean() const noexcept
		{
			const std::uint64_t n = std::min<std::uint64_t>(count, size);
			return n ? static_cast<std::uint32_t>((sum + n / 2) / n) : 0;
		}
	};

	// Interarrival jitter in the integer form of RFC 3550 A.8, over consecutive round trips instead of transit times:
	// J += (|R_k - R_(k-1)| - J) / 16, state scaled by 16. The first sample only sets R_(k-1).
	struct jitter_filter
	{
		std::uint64_t previous = 0, j16 = 0;
		bool has_previous = false;
		constexpr void add(std::uint64_t rtt) noexcept
		{
			if (has_previous)
			{
				const std::uint64_t d = rtt > previous ? rtt - previous : previous - rtt;
				j16 = j16 - ((j16 + 8) >> 4) + d;
			}
			previous = rtt;
			has_previous = true;
		}
		constexpr std::uint32_t value() const noexcept
		{
			return sat32((j16 + 8) >> 4);
		}
	};

	// Owner side. One instance lives in the latch's storage; every call is made by the owner PPU (configure() by the configuring
	// thread, before m_active is raised). Only this class writes the block, apart from publish_end / publish_guard below.
	class publisher
	{
	public:
		struct config
		{
			std::uint32_t mode = mode_straight, role = 0, session_tag = 0, input_delay = 0, max_prediction = 0, window = 0;
			bool relayed = false;
		};

		publisher() noexcept : m_block(&stats) {}
		explicit publisher(block& target) noexcept : m_block(&target) {}

		// A new latch generation: everything starts clean, then generation + 1 tells the overlay a latch is configured.
		void configure(const config& c) noexcept
		{
			block& b = *m_block;
			*this = publisher(b); // owner-private sums, windows and the tick interval start over
			b.mode.store(c.mode, r);
			b.role.store(c.role, r);
			b.session_tag.store(c.session_tag, r);
			b.boundary.store(0, r);
			b.input_delay.store(c.input_delay, r);
			b.delay_now.store(0, r);
			b.max_prediction.store(c.max_prediction, r);
			b.window.store(c.window, r);
			b.rtt_last_us.store(0, r);
			b.rtt_avg_us.store(0, r);
			b.rtt_jitter_us.store(0, r);
			b.rtt_samples.store(0, r);
			b.adv_milli.store(0, r);
			b.error_milli.store(0, r);
			b.adv_valid.store(0, r);
			b.predicting.store(0, r);
			b.corrections.store(0, r);
			b.depth_last.store(0, r);
			b.depth_max.store(0, r);
			b.restore_last_us.store(0, r);
			b.restore_avg_us.store(0, r);
			b.restore_max_us.store(0, r);
			b.tick_last_us.store(0, r);
			b.tick_avg_us.store(0, r);
			b.tick_max_us.store(0, r);
			b.ticks_replayed.store(0, r);
			b.correction_last_us.store(0, r);
			b.correction_avg_us.store(0, r);
			b.correction_max_us.store(0, r);
			b.hash_checks.store(0, r);
			b.hash_tick.store(-1, r);
			b.mismatch_tick.store(-1, r);
			b.in_match.store(0, r);
			b.depth_recent.store(0, r);
			b.loss_permille.store(0, r);
			b.relayed.store(c.relayed ? 1 : 0, r);
			b.leave_select.store(0, r);
			b.leave_hold_ms.store(0, r);
			b.replay_state.store(0, r);
			b.replay_speed.store(2, r);
			b.replay_control.store(0, r);
			b.replay_match.store(0, r);
			b.replay_frame.store(0, r);
			b.replay_round.store(0, r);
			b.replay_round_frame.store(0, r);
			b.takeover_on.store(0, r);
			b.takeover_state.store(0, r);
			b.takeover_port.store(0, r);
			b.takeover_frames_left.store(0, r);
			b.takeover_choice.store(0, r);
			b.practice_dummy_on.store(0, r);
			b.practice_dummy_state.store(0, r);
			b.practice_dummy_slot.store(0, r);
			b.practice_dummy_frames.store(0, r);
			b.practice_dummy_choice.store(0, r);
			b.practice_dummy_row.store(0, r);
			b.practice_dummy_frequency.store(1, r);
			b.practice_dummy_interval.store(1, r);
			b.practice_dummy_slots.store(0, r);
			b.end_reason.store(nullptr, r);
			b.guard.store(nullptr, r);
			b.generation.fetch_add(1, r);
		}

		// peer_poll, after the round-trip ring write.
		void rtt(std::uint64_t rtt_us) noexcept
		{
			const std::uint32_t v = sat32(rtt_us);
			m_rtt.add(v);
			m_jitter.add(v);
			block& b = *m_block;
			b.rtt_last_us.store(v, r);
			b.rtt_avg_us.store(m_rtt.mean(), r);
			b.rtt_jitter_us.store(m_jitter.value(), r);
			b.rtt_samples.store(sat32(m_rtt.count), r);
		}

		// The negotiated (or forced) input delay applied at roll_begin - 4 (session: start + 40).
		void input_delay(std::uint32_t delay) noexcept
		{
			m_block->input_delay.store(delay, r);
		}
		// The delay the newest assign row used (lockstep phases run at max(D, L)).
		void delay_now(std::uint32_t delay) noexcept
		{
			m_block->delay_now.store(delay + 1, r);
		}

		// One advantage step per boundary: the smoothed own advantage and, when valid, the halved difference of both peers'.
		void advantage(double smoothed_ticks, double error_ticks, bool error_valid) noexcept
		{
			block& b = *m_block;
			b.adv_milli.store(milli_from_ticks(smoothed_ticks), r);
			b.error_milli.store(error_valid ? milli_from_ticks(error_ticks) : 0, r);
			b.adv_valid.store(error_valid ? 3u : 1u, r);
		}

		void predicting(std::uint32_t ticks) noexcept
		{
			m_block->predicting.store(ticks, r);
		}
		void boundary(std::uint32_t next_index) noexcept
		{
			m_block->boundary.store(next_index, r);
			// The recent rollback maximum covers the current and the previous 120-boundary window.
			if (++m_depth_window_ticks >= 120)
			{
				m_depth_previous = m_depth_current;
				m_depth_current = 0;
				m_depth_window_ticks = 0;
				m_block->depth_recent.store(m_depth_previous, r);
			}
		}

		// The newest boundary is inside a match (the overlay shows only then).
		void match(bool in_match) noexcept
		{
			m_block->in_match.store(in_match ? 1u : 0u, r);
		}

		// The local player's leave prompt (filter_local_sample): Select held, and the Start + Select hold so far.
		void leave(bool select_held, std::uint32_t hold_ms) noexcept
		{
			m_block->leave_select.store(select_held ? 1u : 0u, r);
			m_block->leave_hold_ms.store(hold_ms, r);
		}

		// A controlled replay's state (see block::replay_state); speed_step is -2..2.
		void replay(bool paused, int speed_step, std::uint32_t control, std::uint32_t match, std::uint32_t frame, std::uint32_t round = 0,
			std::uint32_t round_frame = 0) noexcept
		{
			m_block->replay_round.store(round, r);
			m_block->replay_round_frame.store(round_frame, r);
			m_block->replay_speed.store(static_cast<std::uint32_t>(speed_step + 2), r);
			m_block->replay_control.store(control, r);
			m_block->replay_match.store(match, r);
			m_block->replay_frame.store(frame, r);
			m_block->replay_state.store(paused ? 2u : 1u, r);
		}

		// The replay takeover's state (see block::takeover_state).
		void takeover(bool on, std::uint32_t state, std::uint32_t port, std::uint32_t frames_left, std::uint32_t choice) noexcept
		{
			m_block->takeover_port.store(port, r);
			m_block->takeover_frames_left.store(frames_left, r);
			m_block->takeover_choice.store(choice, r);
			m_block->takeover_state.store(state, r);
			m_block->takeover_on.store(on ? 1u : 0u, r);
		}

		void practice_dummy(bool on, std::uint32_t state, std::uint32_t slot, std::uint32_t frames, std::uint32_t choice,
			std::uint32_t row, std::uint32_t frequency, std::uint32_t interval, std::uint32_t slots) noexcept
		{
			m_block->practice_dummy_slot.store(slot, r);
			m_block->practice_dummy_frames.store(frames, r);
			m_block->practice_dummy_choice.store(choice, r);
			m_block->practice_dummy_row.store(row, r);
			m_block->practice_dummy_frequency.store(frequency, r);
			m_block->practice_dummy_interval.store(interval, r);
			m_block->practice_dummy_slots.store(slots, r);
			m_block->practice_dummy_state.store(state, r);
			m_block->practice_dummy_on.store(on ? 1u : 0u, r);
		}

		// The other peer's boundary carried by a received datagram. The first datagram of each boundary is sent at its
		// start; a boundary that advanced by more than one means the first datagrams of the boundaries in between never arrived
		// (hold resends and post-correction passes carry an older or equal boundary and are ignored).
		void remote_boundary(std::uint32_t boundary) noexcept
		{
			if (m_remote_seen && boundary > m_remote_last)
			{
				const std::uint32_t gap = (std::min)(boundary - m_remote_last - 1, 60u);
				m_loss_missed += gap;
				m_loss_total += gap + 1;
				if (m_loss_total >= 300)
				{
					m_block->loss_permille.store(sat32(m_loss_missed * 1000ull / m_loss_total), r);
					m_loss_missed = m_loss_total = 0;
				}
			}
			if (!m_remote_seen || boundary > m_remote_last)
			{
				m_remote_last = boundary;
				m_remote_seen = true;
			}
		}

		// An admitted restore (after correct_restore_ns is measured). A new correction also closes any open tick interval.
		void restore(std::uint32_t depth, std::int64_t restore_ns) noexcept
		{
			m_corrections++;
			m_depth_max = (std::max)(m_depth_max, depth);
			m_depth_current = (std::max)(m_depth_current, depth);
			m_block->depth_recent.store((std::max)(m_depth_current, m_depth_previous), r);
			m_restore.add(us_from_ns(restore_ns));
			m_tick_open = false;
			block& b = *m_block;
			b.corrections.store(m_corrections, r);
			b.depth_last.store(depth, r);
			b.depth_max.store(m_depth_max, r);
			b.restore_last_us.store(sat32(m_restore.last), r);
			b.restore_avg_us.store(sat32(m_restore.mean()), r);
			b.restore_max_us.store(sat32(m_restore.peak), r);
		}

		// corrected_update row: elapsed_ns is cumulative from the restore start. The k-th replayed tick costs
		// elapsed[k+1] - elapsed[k] and the last one wall - elapsed[last].
		void replayed_tick(std::int64_t elapsed_ns) noexcept
		{
			const std::uint64_t e = non_negative(elapsed_ns);
			if (m_tick_open)
				add_tick(e > m_tick_previous_ns ? e - m_tick_previous_ns : 0);
			m_tick_previous_ns = e;
			m_tick_open = true;
		}

		// correction_end row: the complete affected frame (restore + resume + every replayed tick).
		void correction_end(std::int64_t wall_ns) noexcept
		{
			const std::uint64_t w = non_negative(wall_ns);
			if (m_tick_open)
				add_tick(w > m_tick_previous_ns ? w - m_tick_previous_ns : 0);
			m_tick_open = false;
			m_correction.add(us_from_ns(static_cast<std::int64_t>(std::min<std::uint64_t>(w, 0x7fffffffffffffffull))));
			block& b = *m_block;
			b.correction_last_us.store(sat32(m_correction.last), r);
			b.correction_avg_us.store(sat32(m_correction.mean()), r);
			b.correction_max_us.store(sat32(m_correction.peak), r);
		}

		// A confirmed-state hash comparison (immediate in peer_poll or deferred at confirmation).
		void hash_check(std::int32_t tick, bool equal) noexcept
		{
			m_hash_checks++;
			block& b = *m_block;
			b.hash_checks.store(m_hash_checks, r);
			b.hash_tick.store(tick, r);
			if (!equal)
				b.mismatch_tick.store(tick, r);
		}

	private:
		static constexpr auto r = std::memory_order_relaxed;
		block* m_block;
		running m_restore{}, m_tick{}, m_correction{};
		rtt_window m_rtt{};
		jitter_filter m_jitter{};
		std::uint64_t m_tick_previous_ns = 0;
		bool m_tick_open = false;
		std::uint32_t m_corrections = 0, m_depth_max = 0, m_hash_checks = 0;
		std::uint32_t m_depth_current = 0, m_depth_previous = 0, m_depth_window_ticks = 0, m_remote_last = 0;
		std::uint64_t m_loss_missed = 0, m_loss_total = 0;
		bool m_remote_seen = false;

		void add_tick(std::uint64_t ns) noexcept
		{
			m_tick.add(us_from_ns(static_cast<std::int64_t>(std::min<std::uint64_t>(ns, 0x7fffffffffffffffull))));
			block& b = *m_block;
			b.tick_last_us.store(sat32(m_tick.last), r);
			b.tick_avg_us.store(sat32(m_tick.mean()), r);
			b.tick_max_us.store(sat32(m_tick.peak), r);
			b.ticks_replayed.store(sat32(m_tick.count), r);
		}
	};

	// finish() (owner or thread-state thread): the latch's end reason, a reason_text literal.
	inline void publish_end(const char* reason, block& b = stats) noexcept
	{
		b.end_reason.store(reason, std::memory_order_relaxed);
	}
	// fail() (owner): the failed guard, a string literal.
	inline void publish_guard(const char* message, block& b = stats) noexcept
	{
		b.guard.store(message, std::memory_order_relaxed);
	}

	// Reader side: one relaxed load per field.
} // namespace pbd::live_stats
