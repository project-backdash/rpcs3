#pragma once

// Reads P1's wavedash and backdash-cancel input patterns frame by frame. Completed runs of min_streak or more report
// pace and cancel timing; individual backdash cancels also report a timing grade. Practice shows wavedash streaks only
// for fighters with a crouch dash.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include "Emu/PBD/pbd_movement_observation.h"

namespace pbd::practice::streak
{
	enum class kind : std::uint8_t
	{
		wavedash,
		backdash_cancel,
		// One backdash cancel, as soon as it is done.
		single_cancel
	};

	struct streak
	{
		kind what = kind::wavedash;
		// A single cancel: 1, with cancel_frames its frames from the d/b to the b after it.
		std::uint32_t count = 0;
		// Moves a second over the run, from the first move to the last.
		double per_second = 0.0;
		// Backdash cancels only. backdash_frames: the average time the backdash's back was held before the d/b cancel.
		// cancel_frames: the average time from a d/b to the held back of the next backdash (d/b, b, n), over the cancels that
		// led into another backdash; perfect_cancels of them took perfect_cancel_frames or fewer.
		double backdash_frames = 0.0, cancel_frames = 0.0;
		std::uint32_t cancels = 0, perfect_cancels = 0;
	};

	// The fastest cancel: one frame each of d/b, b and neutral, so the next backdash's held back is the fourth frame.
	inline constexpr std::uint32_t perfect_cancel_frames = 3;

	// A single cancel's grade, from its d/b to the b after it: the d/b held one frame (the b on the very next frame, as
	// Continuous Backdash does it) is frame-perfect, and each frame later drops one grade, down to Slow from 4 frames late.
	// A b more than step_frames after the d/b is not a cancel.
	enum class grade : std::uint8_t
	{
		perfect,
		great,
		good,
		fair,
		slow
	};
	inline constexpr std::uint32_t perfect_single_frames = 1;
	inline constexpr grade grade_of(std::uint32_t frames) noexcept
	{
		const std::uint32_t late = frames > perfect_single_frames ? frames - perfect_single_frames : 0;
		return late >= 4 ? grade::slow : static_cast<grade>(late);
	}
	inline constexpr const char* grade_name(grade value) noexcept
	{
		switch (value)
		{
		case grade::perfect: return "PERFECT";
		case grade::great: return "GREAT";
		case grade::good: return "GOOD";
		case grade::fair: return "FAIR";
		default: return "SLOW";
		}
	}

	// A run is min_streak moves or more, each within link_frames of the one before (a crouch dash 30 frames apart, the
	// slowest Practice wavedash, still links). Each step of a move is quick: f to n to d/f within step_frames for a crouch
	// dash, the second b of a backdash within step_frames of the first, and the d/b cancel within step_frames of it.
	inline constexpr std::uint32_t min_streak = 3, link_frames = 40, step_frames = 16;
	// How long the pop-up stays, about as long as the game shows a combo's damage.
	inline constexpr std::uint32_t shown_frames = 120;

	// A number with at most `decimals` decimals and no trailing zeros ("3.35", "3.4", "3").
	inline std::string trimmed(double value, int decimals)
	{
		char digits[32]{};
		std::snprintf(digits, sizeof(digits), "%.*f", decimals, value);
		std::string out = digits;
		if (out.find('.') != std::string::npos)
		{
			while (out.back() == '0')
				out.pop_back();
			if (out.back() == '.')
				out.pop_back();
		}
		return out;
	}

	// The pop-up's text: "WAVEDASH x31 - 3.35/s", the pace with two decimals at most. A backdash cancel run adds a detail
	// line after a tab (the toast's title/detail split): "Backdash 9.2f, cancel 3.6f (perfect 3f), 7 of 11 perfect".
	inline std::string text(const streak& value)
	{
		if (value.what == kind::single_cancel)
		{
			// "BACKDASH CANCEL - GREAT" over "1 frame late"; a frame-perfect one needs no detail.
			const auto frames = static_cast<std::uint32_t>(value.cancel_frames);
			const std::uint32_t late = frames > perfect_single_frames ? frames - perfect_single_frames : 0;
			std::string out = std::string("BACKDASH CANCEL - ") + grade_name(grade_of(frames));
			if (late)
				out += "\t" + std::to_string(late) + (late == 1 ? " frame late" : " frames late");
			return out;
		}
		std::string out = std::string(value.what == kind::wavedash ? "WAVEDASH" : "BACKDASH CANCEL") + " x" + std::to_string(value.count) + " - " + trimmed(value.per_second, 2) + "/s";
		if (value.what == kind::backdash_cancel)
		{
			out += "\tBackdash " + trimmed(value.backdash_frames, 1) + "f";
			if (value.cancels)
				out += ", cancel " + trimmed(value.cancel_frames, 1) + "f (perfect " + std::to_string(perfect_cancel_frames) + "f), " +
				       std::to_string(value.perfect_cancels) + " of " + std::to_string(value.cancels) + " perfect";
		}
		return out;
	}

	class tracker
	{
	public:
		// One frame of P1's input, opponent-relative (pbd::practice::movement::oriented with P1's side). Returns the run that
		// ended on this frame, when it was long enough to show.
		std::optional<streak> frame(std::uint16_t relative) noexcept
		{
			namespace movement = pbd::practice::movement;
			const auto dirs = static_cast<std::uint16_t>(relative & (movement::up | movement::down | movement::forward | movement::back));
			const std::uint32_t now = m_frame++;
			std::optional<streak> ended;
			track_wavedash(relative, dirs, now, ended);
			const auto single = track_backdash(dirs, now, ended);
			m_previous = dirs;
			// A run nothing has extended for link_frames is over.
			for (auto* which : {&m_wave, &m_cancel})
				if (!ended && which->count && now - which->last > link_frames)
					ended = which->finish(which == &m_wave ? kind::wavedash : kind::backdash_cancel);
			// One pop-up a frame: a single cancel that comes with the end of a run shows on the next frame.
			if (single && ended)
				m_waiting = single;
			else if (single)
				ended = single;
			else if (!ended && m_waiting)
			{
				ended = m_waiting;
				m_waiting.reset();
			}
			return ended;
		}

		void reset() noexcept
		{
			*this = {};
		}

	private:
		struct run
		{
			std::uint32_t count = 0, first = 0, last = 0;
			// Backdash cancels: the frames each backdash's back was held, and each measured cancel.
			std::uint32_t held = 0, cancel_total = 0, cancels = 0, perfect = 0;
			void add_cancel(std::uint32_t frames) noexcept
			{
				cancel_total += frames;
				++cancels;
				if (frames <= perfect_cancel_frames)
					++perfect;
			}
			// The run as shown when it was long enough; either way it starts again.
			std::optional<streak> finish(kind what) noexcept
			{
				std::optional<streak> out;
				if (count >= min_streak && last > first)
				{
					out = streak{what, count, static_cast<double>(count - 1) * 60.0 / static_cast<double>(last - first)};
					out->backdash_frames = static_cast<double>(held) / count;
					out->cancels = cancels;
					out->perfect_cancels = perfect;
					if (cancels)
						out->cancel_frames = static_cast<double>(cancel_total) / cancels;
				}
				*this = {};
				return out;
			}
		};
		static void counted(run& which, kind what, std::uint32_t now, std::optional<streak>& ended) noexcept
		{
			if (which.count && now - which.last > link_frames)
				ended = which.finish(what);
			if (!which.count)
				which.first = now;
			++which.count;
			which.last = now;
		}

		void track_wavedash(std::uint16_t relative, std::uint16_t dirs, std::uint32_t now, std::optional<streak>& ended) noexcept
		{
			namespace movement = pbd::practice::movement;
			// Crouch dash: f, n, (d), d/f. A d/f with a button is an attack out of the crouch dash (an electric), not movement.
			const bool f = dirs == movement::forward, n = dirs == 0, d = dirs == movement::down;
			const bool buttons = (relative & ~(movement::up | movement::down | movement::forward | movement::back)) != 0;
			const bool df = dirs == (movement::down | movement::forward);
			if (f)
				m_dash = {1, now};
			else if (n)
			{
				if (m_dash.stage == 1)
					m_dash = {2, now};
				else if (m_dash.stage != 2)
					m_dash = {};
			}
			else if (d)
			{
				if (m_dash.stage >= 2)
					m_dash.stage = 3;
				else
					m_dash = {};
			}
			else
			{
				if (df && !buttons && m_dash.stage >= 2 && now - m_dash.at <= step_frames)
					counted(m_wave, kind::wavedash, now, ended);
				m_dash = {};
			}
		}

		std::optional<streak> track_backdash(std::uint16_t dirs, std::uint32_t now, std::optional<streak>& ended) noexcept
		{
			namespace movement = pbd::practice::movement;
			// Backdash cancel: b, n, b (the backdash), then d/b while it runs (the cancel). Leaving d/b gives the next b.
			const bool n = dirs == 0;
			const bool b = dirs == movement::back, db = dirs == (movement::down | movement::back);
			if (b && m_previous != movement::back)
			{
				m_backdash_at = m_released && now - m_released_at <= step_frames ? std::optional<std::uint32_t>(now) : std::nullopt;
				m_released = false;
				// The held back of a backdash that a cancel led into: that cancel took from its d/b to here.
				if (m_backdash_at && m_cancel_at && m_cancel.count && now - *m_cancel_at <= link_frames)
					m_cancel.add_cancel(now - *m_cancel_at);
				if (m_backdash_at)
					m_cancel_at.reset();
			}
			else if (n && (m_previous == movement::back || m_previous == (movement::down | movement::back)))
			{
				m_released = true;
				m_released_at = now;
			}
			else if (!n && !b && !db)
				m_released = false;
			// A single cancel ends on the first b after its d/b, held or released to neutral first; anything else drops it.
			std::optional<streak> single;
			if (m_single_at)
			{
				if (b && m_previous != movement::back)
				{
					if (now - *m_single_at <= step_frames)
					{
						single = streak{kind::single_cancel, 1};
						single->cancel_frames = static_cast<double>(now - *m_single_at);
					}
					m_single_at.reset();
				}
				else if (!db && !n)
					m_single_at.reset();
			}
			if (db && m_backdash_at && now - *m_backdash_at <= step_frames)
			{
				counted(m_cancel, kind::backdash_cancel, now, ended);
				m_cancel.held += now - *m_backdash_at;
				m_cancel_at = now;
				m_single_at = now;
				m_backdash_at.reset();
			}
			return single;
		}

		struct dash_step
		{
			std::uint32_t stage = 0; // 1: f, 2: n after it, 3: d after that
			std::uint32_t at = 0;    // the frame the n came (stage 2 on), or the f
		};
		std::uint32_t m_frame = 0;
		dash_step m_dash;
		run m_wave, m_cancel;
		std::uint16_t m_previous = 0;
		bool m_released = false;
		std::uint32_t m_released_at = 0;
		std::optional<std::uint32_t> m_backdash_at;
		// The frame of the last counted d/b cancel, until the next backdash's held back measures it.
		std::optional<std::uint32_t> m_cancel_at;
		// The same d/b, until the b after it completes the single cancel; and a single cancel held back for a frame.
		std::optional<std::uint32_t> m_single_at;
		std::optional<streak> m_waiting;
	};
} // namespace pbd::practice::streak
