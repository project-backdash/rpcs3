#pragma once

// Movement tapes use opponent-relative directions; conversion to screen directions follows the dummy's input side.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pbd::practice::movement
{
	// Directions in the canonical pad word the game reads (the bits the command history and the wall-restart chords use).
	inline constexpr std::uint16_t pad_up = 0x1000, pad_right = 0x2000, pad_down = 0x4000, pad_left = 0x8000;
	// Opponent-relative words reuse the screen bits of a dummy standing on the left: Right is toward the opponent.
	inline constexpr std::uint16_t forward = pad_right, back = pad_left, down = pad_down, up = pad_up;

	enum class screen_side : std::uint8_t
	{
		left,
		right
	};

	// Turns an opponent-relative word into the screen word for a dummy on `side`. Only the horizontal bits change, and the
	// conversion is its own inverse, so it also turns a recorded screen word back into a relative one.
	inline std::uint16_t oriented(std::uint16_t word, screen_side side) noexcept
	{
		if (side == screen_side::left)
			return word;
		const auto mirrored = static_cast<std::uint16_t>(((word & pad_left) ? pad_right : 0) | ((word & pad_right) ? pad_left : 0));
		return static_cast<std::uint16_t>((word & ~(pad_left | pad_right)) | mirrored);
	}

	// Swaps Left and Right (diagonals follow, since Up and Down are untouched): a screen word recorded on one side becomes the
	// same opponent-relative input for the other side.
	inline std::uint16_t mirrored(std::uint16_t word) noexcept
	{
		return oriented(word, screen_side::right);
	}

	// A recorded take (an action slot) is kept opponent-relative, frame by frame. DR converts a pad word's Left / Right into
	// back / forward with the fighter's input side of that frame (pbd::fighter_side::read_input), so recording converts
	// each word with the side DR read it with, and playback converts each word with the side DR will read it with: f stays
	// toward the opponent and b away from it whichever side the dummy stands on, a side swap in between included. oriented()
	// is its own inverse, so both directions are the same conversion; the names say which way it runs.
	inline std::uint16_t record_word(std::uint16_t screen, screen_side side) noexcept
	{
		return oriented(screen, side);
	}
	inline std::uint16_t play_word(std::uint16_t relative, screen_side side) noexcept
	{
		return oriented(relative, side);
	}

	// The dummy's own left and right, for the sideways presets. The stick's Up is a step into the screen and Down a step toward
	// it whichever side a fighter stands on, so they are the fighter's left and right only on the left of the screen (facing
	// right, its left hand points into the screen); on the right, facing left, Up is its right. A sideways tape is written for
	// a dummy on the left (own_left on the Up bit, own_right on the Down bit) and lateral() swaps the two for the right side.
	// Like oriented() it is its own inverse.
	inline constexpr std::uint16_t own_left = pad_up, own_right = pad_down;
	inline std::uint16_t lateral(std::uint16_t word, screen_side side) noexcept
	{
		if (side == screen_side::left)
			return word;
		const auto swapped = static_cast<std::uint16_t>(((word & pad_up) ? pad_down : 0) | ((word & pad_down) ? pad_up : 0));
		return static_cast<std::uint16_t>((word & ~(pad_up | pad_down)) | swapped);
	}

	// Menu order of "Movement Between Actions". Neutral is the default and leaves the dummy idle. Wavedash is one choice; its
	// speed is a setting of its own (wavedash_speed). The menu's option list holds pbd::practice::max_options choices (15),
	// and every preset is one of them.
	enum class preset : std::uint8_t
	{
		none,
		wavedash,
		backdash_cancel,
		forward_dash,
		backdash,
		sidestep_left,
		sidestep_right,
		sidewalk_left,
		sidewalk_right,
		crouch,
		jump_back
	};
	inline constexpr std::uint32_t preset_count = 11;

	inline constexpr bool is_wavedash(preset kind) noexcept
	{
		return kind == preset::wavedash;
	}

	// The menu's name for each choice.
	inline constexpr const char* preset_name(preset kind) noexcept
	{
		switch (kind)
		{
		case preset::none: return "Neutral";
		case preset::wavedash: return "Wavedash";
		case preset::backdash_cancel: return "Continuous Backdash";
		case preset::forward_dash: return "Continuous Forward Dash";
		case preset::backdash: return "Backdash";
		case preset::sidestep_left: return "Sidestep Left";
		case preset::sidestep_right: return "Sidestep Right";
		case preset::sidewalk_left: return "Sidewalk Left";
		case preset::sidewalk_right: return "Sidewalk Right";
		case preset::crouch: return "Crouch";
		case preset::jump_back: return "Jump Back";
		}
		return "Neutral";
	}

	// Offense's Movement Technique lists the movement techniques only, the presets before the plain inputs: Neutral, Wavedash,
	// Continuous Backdash, Continuous Forward Dash and Backdash. A sidestep, a sidewalk, a crouch or a jump is a plain input,
	// not a technique.
	inline constexpr std::uint32_t technique_count = 5;
	static_assert(static_cast<std::uint32_t>(preset::backdash) + 1 == technique_count && static_cast<std::uint32_t>(preset::sidestep_left) == technique_count,
		"the techniques are the presets before the first plain input");
	inline constexpr bool is_technique(preset kind) noexcept
	{
		return static_cast<std::uint32_t>(kind) < technique_count;
	}

	// The presets whose Up and Down are the dummy's own left and right (lateral()) rather than the stick's.
	inline constexpr bool is_sideways(preset kind) noexcept
	{
		return kind == preset::sidestep_left || kind == preset::sidestep_right || kind == preset::sidewalk_left || kind == preset::sidewalk_right;
	}

	// Two kinds of movement. A continuous movement runs without a break until an action is due, starts again once the
	// dummy has recovered from it, and runs on for ever with no action to play: Continuous Backdash, Continuous Forward Dash,
	// Wavedash, Sidewalk Left / Right, Crouch and Jump Back. A one-shot movement plays once, straight before each action, and with
	// no action to come before it does not play: the dummy follows the game's own Dummy Action. Backdash, Sidestep Left / Right.
	inline constexpr bool is_one_shot(preset kind) noexcept
	{
		return kind == preset::backdash || kind == preset::sidestep_left || kind == preset::sidestep_right;
	}

	// Tuning, in frames at 60 per second.
	// The backdashes are b, n, b at frame-perfect timing. Continuous Backdash is the Korean backdash cancel: a lead-in b 1,
	// n 1, then a loop of the backdash's own b 10, d/b 1 to cancel it, b 1 and n 1 on the way back, which with the next b 10
	// make the next b, n, b. Backdash is one b 1, n 1, b 10.
	inline constexpr std::uint32_t dash_tap_frames = 1, dash_gap_frames = 1, dash_hold_frames = 10;
	inline constexpr std::uint32_t dash_cancel_frames = 1, dash_release_frames = 1, dash_neutral_frames = 1;
	inline constexpr std::uint32_t dash_lead_frames = dash_tap_frames + dash_gap_frames;
	inline constexpr std::uint32_t dash_cycle_frames = dash_hold_frames + dash_cancel_frames + dash_release_frames + dash_neutral_frames;
	inline constexpr std::uint32_t backdash_frames = dash_lead_frames + dash_hold_frames;
	// Continuous Forward Dash is forward taps, f 4, n 4, again and again: a player's "ff, ff, ff" capture taps f 3-4 with
	// n 3-5 between, one tap every 7 to 8 frames.
	inline constexpr std::uint32_t forward_dash_tap_frames = 4, forward_dash_gap_frames = 4;
	// A wavedash is a chain of whole crouch dashes. Each pass is f, n, d, df, then the forward that releasing df gives, then
	// a short neutral: f, n, d, df, f, n. A player's captured passes: relaxed f 5, n 3, d 4, df 3, f 2, n 6 (23 frames), fast
	// f 4, n 2, d 3, df 1, f 3, n 4 (17). The step's neutral and release lengths also shape the Wavedash End into an action
	// (plan_flow).
	struct crouch_dash_rhythm
	{
		std::uint32_t forward_frames = 1, neutral_frames = 1, down_frames = 1, down_forward_frames = 1, release_frames = 1, rest_frames = 1;
		constexpr std::uint32_t cycle() const noexcept
		{
			return forward_frames + neutral_frames + down_frames + down_forward_frames + release_frames + rest_frames;
		}
		// Where the crouch dash is entered: the frames up to and including its df.
		constexpr std::uint32_t entry() const noexcept
		{
			return forward_frames + neutral_frames + down_frames + down_forward_frames;
		}
		// Every step is an input of at least one frame, so each pass ends on neutral.
		constexpr bool whole() const noexcept
		{
			return forward_frames && neutral_frames && down_frames && down_forward_frames && release_frames && rest_frames;
		}
		// No step is shorter than the same step of `faster`.
		constexpr bool no_shorter_than(const crouch_dash_rhythm& faster) const noexcept
		{
			return forward_frames >= faster.forward_frames && neutral_frames >= faster.neutral_frames && down_frames >= faster.down_frames &&
			       down_forward_frames >= faster.down_forward_frames && release_frames >= faster.release_frames && rest_frames >= faster.rest_frames;
		}
	};

	// Wavedash speed in crouch dashes a second, 2 to 8; one pass lasts 60 / speed frames. The relaxed capture shapes the slow
	// speeds (every step of 2 a second inside the captured ranges, its rest at their longest, 8), the fast one 4 a second,
	// and toward 8 every step shortens to one frame: 8 a second is the shortest valid f, n, d, df, f, n with the rest one or
	// two frames longer. Where 60 / speed is not whole (7 and 8 a second) the tape holds several passes whose lengths differ
	// by one frame in the rest only, so their average is exact and every pass is entered at the same point.
	inline constexpr std::uint32_t min_wavedash_speed = 2, max_wavedash_speed = 8, default_wavedash_speed = 3;
	inline constexpr std::uint32_t wavedash_speed_count = max_wavedash_speed - min_wavedash_speed + 1;
	inline constexpr std::size_t max_wavedash_passes = 7;
	struct wavedash_rhythm
	{
		std::array<crouch_dash_rhythm, max_wavedash_passes> passes{};
		std::size_t count = 0;
		constexpr std::uint32_t frames() const noexcept
		{
			std::uint32_t total = 0;
			for (std::size_t i = 0; i < count; ++i)
				total += passes[i].cycle();
			return total;
		}
	};
	inline constexpr std::uint32_t clamp_wavedash_speed(std::uint32_t speed) noexcept
	{
		return speed < min_wavedash_speed ? min_wavedash_speed : speed > max_wavedash_speed ? max_wavedash_speed :
		                                                                                      speed;
	}
	inline constexpr wavedash_rhythm wavedash_passes(std::uint32_t speed) noexcept
	{
		constexpr crouch_dash_rhythm nine{2, 1, 1, 1, 1, 3}, eight{2, 1, 1, 1, 1, 2};
		switch (clamp_wavedash_speed(speed))
		{
		case 2: return {{{{6, 3, 6, 4, 3, 8}}}, 1};
		case 3: return {{{{5, 2, 3, 3, 2, 5}}}, 1};
		case 4: return {{{{4, 2, 3, 1, 2, 3}}}, 1};
		case 5: return {{{{3, 2, 2, 1, 1, 3}}}, 1};
		case 6: return {{{{2, 1, 2, 1, 1, 3}}}, 1};
		case 7: return {{{nine, eight, nine, eight, nine, eight, nine}}, 7};
		default: return {{{{1, 1, 1, 1, 1, 3}, {1, 1, 1, 1, 1, 2}}}, 2};
		}
	}
	namespace detail
	{
		inline constexpr bool wavedash_speeds_valid() noexcept
		{
			for (std::uint32_t speed = min_wavedash_speed; speed <= max_wavedash_speed; ++speed)
			{
				const auto made = wavedash_passes(speed);
				if (!made.count || made.frames() * speed != 60 * made.count)
					return false;
				for (std::size_t i = 0; i < made.count; ++i)
				{
					const auto& pass = made.passes[i];
					if (!pass.whole() || pass.entry() != made.passes[0].entry() || pass.neutral_frames != made.passes[0].neutral_frames ||
						pass.release_frames != made.passes[0].release_frames || pass.cycle() > made.passes[0].cycle())
						return false;
				}
				if (speed < max_wavedash_speed && !made.passes[0].no_shorter_than(wavedash_passes(speed + 1).passes[0]))
					return false;
			}
			return true;
		}
	} // namespace detail
	static_assert(detail::wavedash_speeds_valid(), "every wavedash speed is whole passes at exactly 60 / speed frames each, entered alike, "
												   "and no step of a slower speed is shorter");
	// The sideways presets were not captured from a player, so their tap is chosen: 3 frames, long enough to be a real press
	// rather than a one-frame flick and short enough that Up is never held into a jump. A sidestep is that tap alone. A
	// sidewalk is u, n, U: the tap, a release as long, then Up (or Down) held for as long as it runs.
	inline constexpr std::uint32_t sideways_tap_frames = 3, sidewalk_gap_frames = 3;
	// Jump Back holds up-back long enough to be a jump rather than a sidestep, then neutral while the jump lands, and again.
	inline constexpr std::uint32_t jump_back_hold_frames = 10, jump_back_cycle_frames = 55;
	// When the gap's interval ends in the middle of a loop pass, the loop finishes that pass so no motion is cut in half (a
	// lone forward or back would join the action's first inputs into a dash or a different command). A pass longer than this
	// is cut here instead; the ready gate's neutral frames still separate it from the action.
	inline constexpr std::uint32_t gap_overrun_limit_frames = 60;
	static_assert(dash_lead_frames + dash_cycle_frames <= gap_overrun_limit_frames && 60 / min_wavedash_speed <= gap_overrun_limit_frames &&
					  forward_dash_tap_frames + forward_dash_gap_frames <= gap_overrun_limit_frames &&
					  backdash_frames <= gap_overrun_limit_frames && sideways_tap_frames + sidewalk_gap_frames + 1 <= gap_overrun_limit_frames &&
					  jump_back_cycle_frames <= gap_overrun_limit_frames,
		"no generated pass is long enough to be cut at the overrun limit");

	// One opponent-relative word per frame. Frames from loop_start to the end repeat; any before it play once at the start of
	// each run (the lead-in b, n of a backdash cancel, the u, n of a sidewalk).
	struct input_sequence
	{
		std::vector<std::uint16_t> frames;
		std::uint32_t loop_start = 0;
		// Up and Down are the dummy's own left and right (lateral()), written for a dummy on the left of the screen.
		bool sideways = false;
		// One-shot: the tape plays once, straight before the action (is_one_shot).
		bool once = false;
		// The tape ends with a short tap (a sidestep): before an action whose first input presses the same direction, the
		// tap's last frame is a release instead (plan_flow), so the action's press is a new one.
		bool tap = false;
		// The loop is one input held (Crouch's d, a sidewalk's U): it has no pass boundary inside, so the dummy is checked for
		// readiness only before it starts, and it is never released and pressed again.
		bool held = false;
		// Every pass waits for the dummy to be back at rest, not only the first: a jump must land before the next one (and
		// before the action, which the ready gate starts, as such a tape has no hand-over).
		bool rest_each_pass = false;
		// Where each pass of a tape of several passes ends (the last at the tape's end); empty: the tape is one pass.
		std::vector<std::uint32_t> pass_ends;
		// Where the movement hands straight over to the action after it (plan_flow, gap_word), counted from the start of the
		// pass: a wavedash right after each pass's df, every other movement at the end of its tape (after a dash loop's n, after
		// any frame of a held input, after a one-shot's last input). 0: no hand-over (Neutral, Jump Back, or a tape made by hand).
		std::uint32_t flow_at = 0;
		// A wavedash's own neutral after the df and its release forward's length, which the Wavedash End plays into the action.
		std::uint32_t flow_neutral_frames = 0, flow_return_frames = 0;
		// A dash loop's own direction (forward or back; 0 on any other tape): its hand-over follows the Dash End (plan_flow).
		std::uint16_t dash_way = 0;
		bool empty() const noexcept
		{
			return frames.empty();
		}
	};

	// A tape word as the screen word for a dummy on `side`: forward and back always follow the side, and Up and Down too on a
	// sideways tape.
	inline std::uint16_t sequence_word(const input_sequence& movement, std::uint16_t word, screen_side side) noexcept
	{
		const auto screen = oriented(word, side);
		return movement.sideways ? lateral(screen, side) : screen;
	}

	struct step
	{
		std::uint16_t word = 0;
		std::uint32_t frames = 1;
	};

	inline void append(std::vector<std::uint16_t>& out, std::initializer_list<step> steps)
	{
		for (const auto& each : steps)
			out.insert(out.end(), each.frames, each.word);
	}
} // namespace pbd::practice::movement
