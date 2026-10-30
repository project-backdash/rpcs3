#pragma once

#include "../PBD/pbd_live_stats_publisher.h"

namespace pbd::live_stats
{
	struct snapshot
	{
		std::uint32_t generation = 0, mode = mode_idle, role = 0, session_tag = 0, boundary = 0;
		std::uint32_t input_delay = 0, delay_now = 0, max_prediction = 0, window = 0;
		std::uint32_t rtt_last_us = 0, rtt_avg_us = 0, rtt_jitter_us = 0, rtt_samples = 0;
		std::int32_t adv_milli = 0, error_milli = 0;
		std::uint32_t adv_valid = 0, predicting = 0;
		std::uint32_t corrections = 0, depth_last = 0, depth_max = 0;
		std::uint32_t restore_last_us = 0, restore_avg_us = 0, restore_max_us = 0;
		std::uint32_t tick_last_us = 0, tick_avg_us = 0, tick_max_us = 0, ticks_replayed = 0;
		std::uint32_t correction_last_us = 0, correction_avg_us = 0, correction_max_us = 0;
		std::uint32_t hash_checks = 0;
		std::int32_t hash_tick = -1, mismatch_tick = -1;
		std::uint32_t in_match = 0, depth_recent = 0, loss_permille = 0;
		std::uint32_t relayed = 0;
		std::uint32_t leave_select = 0, leave_hold_ms = 0;
		std::uint32_t replay_state = 0, replay_speed = 2, replay_control = 0, replay_match = 0, replay_frame = 0;
		std::uint32_t replay_round = 0, replay_round_frame = 0;
		std::uint32_t takeover_on = 0, takeover_state = 0, takeover_port = 0, takeover_frames_left = 0, takeover_choice = 0;
		std::uint32_t practice_dummy_on = 0, practice_dummy_state = 0, practice_dummy_slot = 0, practice_dummy_frames = 0;
		std::uint32_t practice_dummy_choice = 0, practice_dummy_row = 0, practice_dummy_frequency = 1, practice_dummy_interval = 1;
		std::uint32_t practice_dummy_slots = 0;
		const char* end_reason = nullptr;
		const char* guard = nullptr;
	};

	inline snapshot load(const block& b = stats) noexcept
	{
		constexpr auto r = std::memory_order_relaxed;
		snapshot s;
		s.generation = b.generation.load(r);
		s.mode = b.mode.load(r);
		s.role = b.role.load(r);
		s.session_tag = b.session_tag.load(r);
		s.boundary = b.boundary.load(r);
		s.input_delay = b.input_delay.load(r);
		s.delay_now = b.delay_now.load(r);
		s.max_prediction = b.max_prediction.load(r);
		s.window = b.window.load(r);
		s.rtt_last_us = b.rtt_last_us.load(r);
		s.rtt_avg_us = b.rtt_avg_us.load(r);
		s.rtt_jitter_us = b.rtt_jitter_us.load(r);
		s.rtt_samples = b.rtt_samples.load(r);
		s.adv_milli = b.adv_milli.load(r);
		s.error_milli = b.error_milli.load(r);
		s.adv_valid = b.adv_valid.load(r);
		s.predicting = b.predicting.load(r);
		s.corrections = b.corrections.load(r);
		s.depth_last = b.depth_last.load(r);
		s.depth_max = b.depth_max.load(r);
		s.restore_last_us = b.restore_last_us.load(r);
		s.restore_avg_us = b.restore_avg_us.load(r);
		s.restore_max_us = b.restore_max_us.load(r);
		s.tick_last_us = b.tick_last_us.load(r);
		s.tick_avg_us = b.tick_avg_us.load(r);
		s.tick_max_us = b.tick_max_us.load(r);
		s.ticks_replayed = b.ticks_replayed.load(r);
		s.correction_last_us = b.correction_last_us.load(r);
		s.correction_avg_us = b.correction_avg_us.load(r);
		s.correction_max_us = b.correction_max_us.load(r);
		s.hash_checks = b.hash_checks.load(r);
		s.hash_tick = b.hash_tick.load(r);
		s.mismatch_tick = b.mismatch_tick.load(r);
		s.in_match = b.in_match.load(r);
		s.depth_recent = b.depth_recent.load(r);
		s.loss_permille = b.loss_permille.load(r);
		s.relayed = b.relayed.load(r);
		s.leave_select = b.leave_select.load(r);
		s.leave_hold_ms = b.leave_hold_ms.load(r);
		s.replay_state = b.replay_state.load(r);
		s.replay_speed = b.replay_speed.load(r);
		s.replay_control = b.replay_control.load(r);
		s.replay_match = b.replay_match.load(r);
		s.replay_frame = b.replay_frame.load(r);
		s.replay_round = b.replay_round.load(r);
		s.replay_round_frame = b.replay_round_frame.load(r);
		s.takeover_on = b.takeover_on.load(r);
		s.takeover_state = b.takeover_state.load(r);
		s.takeover_port = b.takeover_port.load(r);
		s.takeover_frames_left = b.takeover_frames_left.load(r);
		s.takeover_choice = b.takeover_choice.load(r);
		s.practice_dummy_on = b.practice_dummy_on.load(r);
		s.practice_dummy_state = b.practice_dummy_state.load(r);
		s.practice_dummy_slot = b.practice_dummy_slot.load(r);
		s.practice_dummy_frames = b.practice_dummy_frames.load(r);
		s.practice_dummy_choice = b.practice_dummy_choice.load(r);
		s.practice_dummy_row = b.practice_dummy_row.load(r);
		s.practice_dummy_frequency = b.practice_dummy_frequency.load(r);
		s.practice_dummy_interval = b.practice_dummy_interval.load(r);
		s.practice_dummy_slots = b.practice_dummy_slots.load(r);
		s.end_reason = b.end_reason.load(r);
		s.guard = b.guard.load(r);
		return s;
	}

	// ---- The switch: decided once per process, shared by the latch (publish) and the overlay (show) ----

	enum class overlay_mode : std::uint8_t
	{
		off,
		on,
		ab
	};
	enum class note : std::uint8_t
	{
		none,         // unset, empty or "0": off, no log line
		on,           // "1": on
		ab_exception, // "ab" in a flip-selected identity capture run: on, with a warning (overlay A/B frame check only)
		bad_value,    // any other value: off, one warning
		refused,      // a frame-gate capture variable is set: off, one error naming it
		ab_refused,   // "ab" without the capture it is an exception for: off, one error naming the missing variable
	};

	struct decision
	{
		overlay_mode mode = overlay_mode::off;
		note why = note::none;
		const char* variable = nullptr; // the variable a refusal names
		constexpr bool show() const noexcept
		{
			return mode != overlay_mode::off;
		}
	};

	// What the decision reads from the process environment.
	struct environment
	{
		std::string_view overlay;      // the switch ("" = unset or empty; a value that is not short printable ASCII reads "?")
		bool queue_tag = false;        // PBD_PRESENT_IDENTITY_QUEUE_TAG == "1" (pbd::frame_presentation::queue_tags_enabled())
		bool output = false;           // PBD_PRESENT_OUTPUT non-empty: legacy capture; every ordinary measurement run sets it
		bool identity_flip = false;    // PBD_PRESENT_IDENTITY_FLIP non-empty (the LAN launcher sets ticks + 2 in every session)
		bool identity_counter = false; // PBD_PRESENT_IDENTITY_COUNTER non-empty: counter-selected identity capture
		bool identity_segment = false; // PBD_PRESENT_IDENTITY_SEGMENT non-empty
		bool identity_anytime = false; // PBD_PRESENT_IDENTITY_ANYTIME == "1": presented-frame check inside a correction window
		bool target_offsets = false;   // PBD_PRESENT_TARGET_OFFSETS non-empty: target capture
		bool target_segment = false;   // PBD_PRESENT_TARGET_SEGMENT non-empty
	};

	// Frame gates force the overlay off. A captured image never contains it either way (VKPresent.cpp reads image_to_flip back
	// before any overlay composition and excludes every PBD capture from record-with-overlays), but a gate run must not carry
	// the overlay's host work either. The legacy capture (PBD_PRESENT_OUTPUT) and a flip-selected identity capture alone are
	// NOT refusals for "1": ordinary measurement runs set the first and the LAN launcher sets the second in every session, so
	// refusing them would make the overlay unavailable in every netplay session.
	constexpr decision decide(const environment& e) noexcept
	{
		if (e.overlay.empty() || e.overlay == "0")
			return {};
		if (e.overlay == "1")
		{
			if (e.queue_tag)
				return {overlay_mode::off, note::refused, "PBD_PRESENT_IDENTITY_QUEUE_TAG"};
			if (e.target_offsets)
				return {overlay_mode::off, note::refused, "PBD_PRESENT_TARGET_OFFSETS"};
			if (e.target_segment)
				return {overlay_mode::off, note::refused, "PBD_PRESENT_TARGET_SEGMENT"};
			if (e.identity_counter)
				return {overlay_mode::off, note::refused, "PBD_PRESENT_IDENTITY_COUNTER"};
			if (e.identity_segment)
				return {overlay_mode::off, note::refused, "PBD_PRESENT_IDENTITY_SEGMENT"};
			if (e.identity_anytime)
				return {overlay_mode::off, note::refused, "PBD_PRESENT_IDENTITY_ANYTIME"};
			return {overlay_mode::on, note::on, nullptr};
		}
		return {overlay_mode::off, note::bad_value, "PBD_OVERLAY"};
	}

	// The one log line of a decision (none for note::none). `variable` is appended by the caller where the text ends in ": ".
	constexpr const char* describe(note why) noexcept
	{
		switch (why)
		{
		case note::on: return "PBD_OVERLAY=1: on-screen stats on (host-side only; frame-gate captures force it off)";
		case note::ab_exception: return "PBD_OVERLAY=ab: overlay ON in a capture run: A/B exception, not frame-gate evidence";
		case note::bad_value: return "PBD_OVERLAY is not 0, 1 or ab: overlay off";
		case note::refused: return "PBD_OVERLAY refused (frame-gate capture run, overlay off): ";
		case note::ab_refused: return "PBD_OVERLAY=ab refused (only for a flip-selected identity capture run), missing: ";
		default: return "";
		}
	}

	namespace detail
	{
		template <typename Char>
		std::string_view short_ascii(const Char* value, std::array<char, 8>& buffer) noexcept
		{
			if (!value || !*value)
				return {};
			std::size_t n = 0;
			for (; value[n]; n++)
			{
				if (n + 1 >= buffer.size() || value[n] < 0x20 || value[n] > 0x7e)
				{
					buffer[0] = '?';
					return {buffer.data(), 1};
				}
				buffer[n] = static_cast<char>(value[n]);
			}
			return {buffer.data(), n};
		}

		template <typename Char>
		constexpr bool non_empty(const Char* value) noexcept
		{
			return value && *value;
		}

		template <typename Char>
		constexpr bool is_one(const Char* value) noexcept
		{
			return value && value[0] == '1' && !value[1];
		}

#ifdef _WIN32
		inline const wchar_t* env(const char* name) noexcept
		{
			return pbd::env::find_wide(name);
		}
#else
		inline const char* env(const char* name) noexcept
		{
			return pbd::env::find(name);
		}
#endif
	} // namespace detail

	// Reads the frame-gate variables now (not cached); the caller sets the switch.
	inline environment read_environment() noexcept
	{
		using namespace detail;
		environment e;
		e.queue_tag = is_one(env("PBD_PRESENT_IDENTITY_QUEUE_TAG"));
		e.output = non_empty(env("PBD_PRESENT_OUTPUT"));
		e.identity_flip = non_empty(env("PBD_PRESENT_IDENTITY_FLIP"));
		e.identity_counter = non_empty(env("PBD_PRESENT_IDENTITY_COUNTER"));
		e.identity_segment = non_empty(env("PBD_PRESENT_IDENTITY_SEGMENT"));
		e.identity_anytime = is_one(env("PBD_PRESENT_IDENTITY_ANYTIME"));
		e.target_offsets = non_empty(env("PBD_PRESENT_TARGET_OFFSETS"));
		e.target_segment = non_empty(env("PBD_PRESENT_TARGET_SEGMENT"));
		return e;
	}

	// The net overlay (one line, bottom right, matches only) has its switch, PBD_NET_OVERLAY=1, with the frame-gate refusals
	// above ("ab" is not accepted). Decided once per process by whichever side asks first (latch configure or the first
	// reset_performance_overlay); the environment does not change in between, and settings changes cannot flip it.
	inline const decision& net_process_decision() noexcept
	{
		static const decision value = []
		{
			std::array<char, 8> buffer{};
			environment e = read_environment();
			e.overlay = detail::short_ascii(detail::env("PBD_NET_OVERLAY"), buffer);
			if (e.overlay == "ab")
				return decision{overlay_mode::off, note::bad_value, "PBD_NET_OVERLAY"};
			return decide(e);
		}();
		return value;
	}

	// The latch publishes exactly when an overlay may show (latch configure; storage::live_stats).
	inline bool publishing() noexcept
	{
		return net_process_decision().show();
	}

	// Connection quality bars: 4 green, 3 or 2 orange, 1 red.
	constexpr std::uint32_t net_bars(std::uint32_t rtt_ms, std::uint32_t jitter_ms, std::uint32_t loss_permille) noexcept
	{
		if (rtt_ms <= 90 && jitter_ms <= 12 && loss_permille <= 10)
			return 4;
		if (rtt_ms <= 150 && jitter_ms <= 25 && loss_permille <= 30)
			return 3;
		if (rtt_ms <= 220 && jitter_ms <= 40 && loss_permille <= 60)
			return 2;
		return 1;
	}

	inline std::uint32_t net_bars(const snapshot& s) noexcept
	{
		if (!s.rtt_samples)
			return 0; // no measurement yet: all bars dim
		return net_bars((s.rtt_avg_us + 500) / 1000, (s.rtt_jitter_us + 500) / 1000, s.loss_permille);
	}

	// The small tag in front of the connection line: "Relay" only while the session runs through the relay, nothing when
	// the two PCs reach each other directly.
	inline const char* route_tag(const snapshot& s) noexcept
	{
		return s.relayed ? "Relay" : nullptr;
	}

	// Leave prompt: shown instead of the net line while Select is held, anywhere in a session. The hold that ends the session
	// lasts leave_hold_total_ms.
	inline constexpr std::uint32_t leave_hold_total_ms = 2000;

	inline std::string net_text(const snapshot& s)
	{
		const auto rtt = s.rtt_samples ? std::to_string((s.rtt_avg_us + 500) / 1000) + " ms" : std::string("-- ms");
		return rtt + "  \xC2\xB7  Delay " + std::to_string(s.input_delay) + "f  \xC2\xB7  Rollback " + std::to_string(s.depth_recent) + "f";
	}

	// Keep the connection text readable when an older installation lacks the notice fonts.
	template <typename Font>
	constexpr Font* connection_face(Font* preferred, Font* fallback) noexcept
	{
		return preferred ? preferred : fallback;
	}

	// Replay line (pure, testable): shown instead of the net line through a controlled replay; while SELECT is held it lists the
	// SELECT combinations instead (the leave hold's countdown once START joins).
	inline const char* replay_speed_text(std::uint32_t stored) noexcept
	{
		constexpr const char* names[5] = {"1/4x", "1/2x", "1x", "2x", "4x"};
		return stored < 5 ? names[stored] : "1x";
	}

	inline std::string replay_text(const snapshot& s)
	{
		std::string out = s.replay_state == 2 ? std::string("PAUSED") : std::string("REPLAY ") + replay_speed_text(s.replay_speed);
		if (s.replay_match)
		{
			out += "   match " + std::to_string(s.replay_match);
			if (s.replay_round)
			{
				// Time into the round's fight, m:ss.t at 60 frames a second.
				const std::uint32_t tenths = s.replay_round_frame / 6;
				const std::uint32_t seconds = tenths / 10;
				out += "   round " + std::to_string(s.replay_round) + "  " + std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") +
				       std::to_string(seconds % 60) + "." + std::to_string(tenths % 10);
			}
			if (s.replay_frame)
				out += "   frame " + std::to_string(s.replay_frame);
		}
		if (s.takeover_on)
		{
			const char* side = s.takeover_port == 2 ? "P2" : "P1";
			if (s.takeover_state == 4)
				return std::string(s.takeover_port == 2 ? "ENABLE CONTROLS FOR RIGHT-SIDE CHARACTER?   " : "ENABLE CONTROLS FOR LEFT-SIDE CHARACTER?   ") +
				       (s.takeover_choice == 0 ? "> YES      NO" : "  YES    > NO") + "      UP/DOWN, X: choose   O: cancel";
			if (s.takeover_state == 3)
				return std::string(s.takeover_choice == 0 ? "> TRY AGAIN      RETURN TO REPLAY" : "  TRY AGAIN    > RETURN TO REPLAY") + "      UP/DOWN, X: choose   O: return";
			if (s.takeover_state == 1 || s.takeover_state == 2)
			{
				const std::uint32_t tenths = (s.takeover_frames_left + 5) / 6;
				if (s.takeover_state == 1)
					return std::string(side) + ": READY " + std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) +
					       " s   press any button or direction";
				return std::string(s.replay_state == 2 ? "PAUSED   " : "") + side + ": YOU " + std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) +
				       " s   SELECT+X: retry   SELECT+O: return" + (s.replay_state == 2 ? "   START: go" : "");
			}
			return out + (s.replay_state == 2 ? "   R2: next frame   START: play" : "   L3/R3: play P1/P2   L1/R1: round   L2/R2: speed");
		}
		if (s.replay_control)
			out += s.replay_control == 1 ? "   you: P1" : "   you: P2";
		out += s.replay_state == 2 ? "   R1: next frame   START: play" : "   START: pause   L1/R1: speed";
		return out;
	}

	inline const char* practice_dummy_interval_text(std::uint32_t interval) noexcept
	{
		constexpr const char* names[4] = {"SHORT", "NORMAL", "SLOW", "SHUFFLE"};
		return interval < 4 ? names[interval] : "NORMAL";
	}

	// Practice dummy status and controls. The host consumes these actions before they reach the guest's P1 word.
	inline std::string practice_dummy_text(const snapshot& s)
	{
		const std::string slot = "SLOT " + std::to_string(s.practice_dummy_slot + 1);
		if (s.practice_dummy_state == 1)
		{
			const std::uint32_t tenths = s.practice_dummy_frames / 6;
			return "RECORDING " + slot + "   " + std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) +
			       " / 30.0 s   control the dummy   SELECT: stop";
		}
		if (s.practice_dummy_state == 2)
		{
			constexpr const char* choices[3] = {"> YES      RECORD AGAIN      EXIT", "  YES    > RECORD AGAIN      EXIT", "  YES      RECORD AGAIN    > EXIT"};
			return std::string("RECORD THIS ACTION?   ") + choices[s.practice_dummy_choice < 3 ? s.practice_dummy_choice : 0] +
			       "   UP/DOWN, X: choose   O: exit";
		}
		if (s.practice_dummy_state == 3)
			return "DUMMY PLAYBACK   " + slot + "   " + std::to_string(s.practice_dummy_slots) + " saved   SELECT+SQUARE: stop   SELECT+TRIANGLE: settings";
		if (s.practice_dummy_state == 4)
		{
			switch (s.practice_dummy_row)
			{
			case 0: return "DUMMY SETTINGS   > " + slot + "   LEFT/RIGHT: choose";
			case 1: return "DUMMY SETTINGS   > PLAYBACK   X: start/stop";
			case 2: return "DUMMY SETTINGS   > FREQUENCY " + std::to_string(s.practice_dummy_frequency) + "   LEFT/RIGHT: 1-10";
			case 3: return std::string("DUMMY SETTINGS   > INTERVAL ") + practice_dummy_interval_text(s.practice_dummy_interval) + "   LEFT/RIGHT: choose";
			default: return "DUMMY SETTINGS   > RECORD " + slot + "   X: begin";
			}
		}
		return "DUMMY READY   " + slot + "   " + std::to_string(s.practice_dummy_slots) +
		       " saved   SELECT+CIRCLE: record   SELECT+TRIANGLE: settings";
	}
} // namespace pbd::live_stats
