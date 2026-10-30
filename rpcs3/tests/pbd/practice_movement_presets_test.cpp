#include "pbd_test.h"

#include "Emu/PBD/pbd_movement_observation.h"
#include "Emu/PBD/pbd_practice_presentation.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
	namespace movement = pbd::practice::movement;
	using movement::preset;
	using movement::screen_side;

	using pbd::test::check;

	struct run
	{
		std::uint16_t word;
		std::uint32_t frames;
	};

	std::vector<std::uint16_t> expand(std::initializer_list<run> runs)
	{
		std::vector<std::uint16_t> out;
		for (const auto& each : runs)
			out.insert(out.end(), each.frames, each.word);
		return out;
	}

	// Plays a whole pass of `kind` through gap_word for a dummy on `side`, as the latch does, and returns the screen words.
	std::vector<std::uint16_t> played(preset kind, screen_side side, std::uint32_t speed = movement::default_wavedash_speed)
	{
		const auto loop = movement::generate(kind, speed);
		std::vector<std::uint16_t> out;
		std::uint32_t position = 0, remaining = static_cast<std::uint32_t>(loop.frames.size()), overrun = 0;
		for (;;)
		{
			const auto frame = movement::gap_word(loop, position, remaining, overrun, side);
			if (frame.done)
				break;
			out.push_back(frame.word);
		}
		return out;
	}

	constexpr std::uint16_t n = 0, f = 0x2000, b = 0x8000, d = 0x4000, u = 0x1000;

	void continuous_and_contact_playback()
	{
		// Both ways the latch plays a movement. After a hit or block the gap is after_contact_wait long: a continuous movement
		// plays for after_contact_frames and finishes the pass it is in, a one-shot plays its motion once. Played continuously
		// the latch keeps the interval from running out, so a continuous tape simply repeats.
		for (std::uint32_t at = static_cast<std::uint32_t>(preset::backdash); at < movement::preset_count; ++at)
		{
			const auto kind = static_cast<preset>(at);
			const auto loop = movement::generate(kind);
			for (const auto side : {screen_side::left, screen_side::right})
			{
				std::uint32_t position = 0, remaining = movement::after_contact_wait(loop), overrun = 0;
				std::vector<std::uint16_t> after;
				for (;;)
				{
					const auto frame = movement::gap_word(loop, position, remaining, overrun, side);
					if (frame.done)
						break;
					after.push_back(frame.word);
				}
				check(position == 0, "the gap ends between two passes");
				if (loop.once)
				{
					check(after == played(kind, side), "after a contact a one-shot plays once");
					continue;
				}
				if (loop.held)
					check(after.size() == movement::after_contact_frames, "a held movement plays the second exactly");
				std::uint32_t cursor = 0, wait = 1, over = 0;
				std::size_t at_sequence = 0;
				for (std::uint32_t frame_index = 0; frame_index < 4 * movement::after_contact_frames; ++frame_index)
				{
					wait = (std::max)(wait, 1u);
					const auto frame = movement::gap_word(loop, cursor, wait, over, side);
					check(!frame.done && frame.word == movement::sequence_word(loop, loop.frames[at_sequence], side),
						"played continuously the movement repeats");
					at_sequence = at_sequence + 1 < loop.frames.size() ? at_sequence + 1 : loop.loop_start;
				}
			}
		}
	}

} // namespace

// The menu's order (one Wavedash, its speed a row of its own) and the dashes at frame-perfect timing.
PBD_TEST(PbdPracticeMovementPresets, ExistingSequencesUnchanged)
{
	check(static_cast<std::uint32_t>(preset::none) == 0 && static_cast<std::uint32_t>(preset::wavedash) == 1 &&
			  static_cast<std::uint32_t>(preset::backdash_cancel) == 2 && static_cast<std::uint32_t>(preset::forward_dash) == 3,
		"Neutral, Wavedash, then the dashes");
	check(movement::generate(preset::none).empty(), "Neutral still has no tape");

	const auto cancel = movement::generate(preset::backdash_cancel);
	check(cancel.loop_start == 2 && cancel.flow_at == 15 && !cancel.sideways && !cancel.once && !cancel.held &&
			  cancel.frames == expand({{b, 1}, {n, 1}, {b, 10}, {static_cast<std::uint16_t>(d | b), 1}, {b, 1}, {n, 1}}),
		"Continuous Backdash is b 1, n 1, then b 10, d/b 1, b 1, n 1");

	// A wavedash pass is f, n, d, df, the release forward, n. The single-pass speeds, step by step: the relaxed capture's
	// shape at the slow end, the fast capture's at 4 a second, one-frame steps toward 6.
	const auto wave = [](std::uint32_t speed, std::uint32_t fwd, std::uint32_t neutral, std::uint32_t down, std::uint32_t df,
						  std::uint32_t release, std::uint32_t rest)
	{
		const auto made = movement::generate(preset::wavedash, speed);
		return made.loop_start == 0 && !made.sideways && made.frames.size() == 60 / speed && made.pass_ends.empty() &&
		       made.flow_at == fwd + neutral + down + df && made.flow_neutral_frames == neutral && made.flow_return_frames == release &&
		       made.frames == expand({{f, fwd}, {n, neutral}, {d, down}, {static_cast<std::uint16_t>(d | f), df}, {f, release}, {n, rest}});
	};
	check(wave(2, 6, 3, 6, 4, 3, 8), "2 a second is f 6, n 3, d 6, df 4, f 3, n 8 (30)");
	check(wave(3, 5, 2, 3, 3, 2, 5), "3 a second is f 5, n 2, d 3, df 3, f 2, n 5 (20)");
	check(wave(4, 4, 2, 3, 1, 2, 3), "4 a second is f 4, n 2, d 3, df 1, f 2, n 3 (15)");
	check(wave(5, 3, 2, 2, 1, 1, 3), "5 a second is f 3, n 2, d 2, df 1, f 1, n 3 (12)");
	check(wave(6, 2, 1, 2, 1, 1, 3), "6 a second is f 2, n 1, d 2, df 1, f 1, n 3 (10)");
	// 7 and 8 a second alternate passes a frame apart so the average is exact: 60 frames for 7, 15 for 2.
	const auto seven = movement::generate(preset::wavedash, 7), eight = movement::generate(preset::wavedash, 8);
	check(seven.frames.size() == 60 && seven.pass_ends == std::vector<std::uint32_t>{9, 17, 26, 34, 43, 51, 60} && seven.flow_at == 5,
		"7 a second: 9, 8, 9, 8, 9, 8, 9 frames");
	check(eight.frames.size() == 15 && eight.pass_ends == std::vector<std::uint32_t>{8, 15} && eight.flow_at == 4 &&
			  eight.frames == expand({{f, 1}, {n, 1}, {d, 1}, {static_cast<std::uint16_t>(d | f), 1}, {f, 1}, {n, 3},
								  {f, 1}, {n, 1}, {d, 1}, {static_cast<std::uint16_t>(d | f), 1}, {f, 1}, {n, 2}}),
		"8 a second: the shortest valid passes, 8 and 7 frames");

	const auto dash = movement::generate(preset::forward_dash);
	check(dash.loop_start == 0 && dash.flow_at == 8 && dash.dash_way == f && !dash.sideways && dash.frames == expand({{f, 4}, {n, 4}}),
		"Continuous Forward Dash is forward taps, f 4, n 4, handed over after a tap's neutral");

	// Played on either side, a wavedash only swaps forward and back: Down stays Down.
	for (std::uint32_t speed = movement::min_wavedash_speed; speed <= movement::max_wavedash_speed; ++speed)
	{
		if (speed < movement::max_wavedash_speed)
			check(movement::generate(preset::wavedash, speed).frames.size() * movement::wavedash_passes(speed + 1).count >
					  movement::generate(preset::wavedash, speed + 1).frames.size() * movement::wavedash_passes(speed).count,
				"each speed is faster than the one before");
		const auto r = movement::wavedash_passes(speed).passes[0];
		for (const auto side : {screen_side::left, screen_side::right})
		{
			const std::uint16_t toward = side == screen_side::left ? f : b;
			const auto first = played(preset::wavedash, side, speed);
			check(std::vector<std::uint16_t>(first.begin(), first.begin() + r.cycle()) == expand({{toward, r.forward_frames}, {n, r.neutral_frames},
																							  {d, r.down_frames}, {static_cast<std::uint16_t>(d | toward), r.down_forward_frames}, {toward, r.release_frames}, {n, r.rest_frames}}),
				"a wavedash pass plays toward the opponent with its crouch on Down, on either side");
		}
	}
}

PBD_TEST(PbdPracticeMovementPresets, SideMapping)
{
	// Up is into the screen and Down toward it on both sides. On the left (facing right) the dummy's left is Up; on the
	// right (facing left) it is Down.
	check(movement::lateral(movement::own_left, screen_side::left) == u && movement::lateral(movement::own_right, screen_side::left) == d,
		"on the left of the screen the dummy's left is Up and its right is Down");
	check(movement::lateral(movement::own_left, screen_side::right) == d && movement::lateral(movement::own_right, screen_side::right) == u,
		"on the right of the screen the dummy's left is Down and its right is Up");
	for (std::uint32_t word = 0; word < 0x10000; word += 0x0101)
		for (const auto side : {screen_side::left, screen_side::right})
		{
			const auto w = static_cast<std::uint16_t>(word);
			check(movement::lateral(movement::lateral(w, side), side) == w, "lateral is its own inverse");
			check((movement::lateral(w, side) & ~(u | d)) == (w & ~(u | d)), "lateral leaves every other bit alone");
		}
	check(movement::lateral(static_cast<std::uint16_t>(u | f), screen_side::right) == (d | f), "a diagonal swaps only its vertical half");

	// oriented() alone never touches Up and Down, which is why the sideways tapes need lateral().
	check(movement::oriented(u, screen_side::right) == u && movement::oriented(d, screen_side::right) == d,
		"the forward / back conversion leaves Up and Down as they are");

	movement::input_sequence plain;
	movement::input_sequence sideways;
	sideways.sideways = true;
	check(movement::sequence_word(plain, static_cast<std::uint16_t>(u | b), screen_side::right) == (u | f),
		"a plain tape swaps only forward and back");
	check(movement::sequence_word(sideways, static_cast<std::uint16_t>(u | b), screen_side::right) == (d | f),
		"a sideways tape swaps both");
	check(movement::sequence_word(sideways, u, screen_side::left) == u, "on the left a sideways tape plays as written");
}

PBD_TEST(PbdPracticeMovementPresets, NewPresets)
{
	check(movement::preset_count == 11 && static_cast<std::uint32_t>(preset::backdash) == 4 &&
			  static_cast<std::uint32_t>(preset::sidestep_left) == 5 && static_cast<std::uint32_t>(preset::sidestep_right) == 6 &&
			  static_cast<std::uint32_t>(preset::sidewalk_left) == 7 && static_cast<std::uint32_t>(preset::sidewalk_right) == 8 &&
			  static_cast<std::uint32_t>(preset::crouch) == 9 && static_cast<std::uint32_t>(preset::jump_back) == 10 &&
			  static_cast<std::uint32_t>(preset::jump_back) + 1 == movement::preset_count,
		"the movements in menu order, Jump Back last");
	// The Movement row lists every preset as one option of the menu's option list, which keeps only its first
	// max_options: a preset past that could never be shown or chosen. The same holds for the Wavedash End row.
	check(movement::preset_count <= pbd::practice::max_options, "every movement fits the menu's option list");
	check(movement::wavedash_end_count <= pbd::practice::max_options, "every Wavedash End fits the option list");
	for (std::uint32_t at = 0; at < movement::preset_count; ++at)
	{
		const auto kind = static_cast<preset>(at);
		check(movement::is_wavedash(kind) == (at == 1), "one Wavedash");
		check(movement::is_sideways(kind) == (at >= 5 && at <= 8), "only the sidesteps and sidewalks are sideways");
		check(movement::is_one_shot(kind) == (at >= 4 && at <= 6), "Backdash and the sidesteps are one-shot");
		const auto made = movement::generate(kind);
		check(kind == preset::none || (!made.empty() && (made.flow_at != 0) == !made.rest_each_pass && made.rest_each_pass == (kind == preset::jump_back)),
			"every movement but Jump Back can hand over to an action; Jump Back waits for each landing");
		check(made.frames.size() <= movement::gap_overrun_limit_frames, "no pass is cut at the overrun limit");
		check(made.sideways == movement::is_sideways(kind) && made.once == movement::is_one_shot(kind) && movement::plays_alone(made) == !made.once && made.tap == (kind == preset::sidestep_left || kind == preset::sidestep_right), "the presets are marked so");
		check(made.held == (kind == preset::sidewalk_left || kind == preset::sidewalk_right || kind == preset::crouch),
			"only the sidewalks and Crouch hold one input");
	}

	// Backdash: one b 1, n 1, b 10.
	const auto backdash = expand({{b, 1}, {n, 1}, {b, 10}});
	check(movement::generate(preset::backdash).frames == backdash && movement::generate(preset::backdash).loop_start == 0,
		"Backdash is one b, n, b");
	check(played(preset::backdash, screen_side::left) == backdash, "on the left Back is the Left bit");
	check(played(preset::backdash, screen_side::right) == expand({{f, 1}, {n, 1}, {f, 10}}), "on the right Back is the Right bit");

	// Sidesteps: one 3-frame tap.
	check(played(preset::sidestep_left, screen_side::left) == expand({{u, 3}}), "Sidestep Left on the left taps Up");
	check(played(preset::sidestep_left, screen_side::right) == expand({{d, 3}}), "Sidestep Left on the right taps Down");
	check(played(preset::sidestep_right, screen_side::left) == expand({{d, 3}}), "Sidestep Right on the left taps Down");
	check(played(preset::sidestep_right, screen_side::right) == expand({{u, 3}}), "Sidestep Right on the right taps Up");

	// Sidewalks: tap 3, release 3, then one frame of the held input per pass.
	check(played(preset::sidewalk_left, screen_side::left) == expand({{u, 3}, {n, 3}, {u, 1}}), "Sidewalk Left on the left is u, U");
	check(played(preset::sidewalk_left, screen_side::right) == expand({{d, 3}, {n, 3}, {d, 1}}), "Sidewalk Left on the right is d, D");
	check(played(preset::sidewalk_right, screen_side::left) == expand({{d, 3}, {n, 3}, {d, 1}}), "Sidewalk Right on the left is d, D");
	check(played(preset::sidewalk_right, screen_side::right) == expand({{u, 3}, {n, 3}, {u, 1}}), "Sidewalk Right on the right is u, U");

	// Crouch holds Down on both sides.
	check(played(preset::crouch, screen_side::left) == expand({{d, 1}}) && played(preset::crouch, screen_side::right) == expand({{d, 1}}),
		"Crouch holds Down on either side");
	// Jump Back: up-back away from the opponent, then the landing.
	check(played(preset::jump_back, screen_side::left) == expand({{static_cast<std::uint16_t>(u | b), 10}, {n, 45}}) &&
			  played(preset::jump_back, screen_side::right) == expand({{static_cast<std::uint16_t>(u | f), 10}, {n, 45}}),
		"Jump Back is Up and away from the opponent on either side");

	continuous_and_contact_playback();
	// Readable text: the sidesteps read as Tekken notation for the side they play on.
	check(movement::direction_text(played(preset::sidestep_left, screen_side::right).front(), screen_side::right) == "d",
		"a left sidestep on the right reads as d");
}

PBD_TEST(PbdPracticeMovementPresets, CursorAndPreparation)
{
	// A discarded preparation restores the cursor and the interval; the replayed frame hands over the same way.
	for (const auto& loop : {movement::generate(preset::wavedash, 4), movement::generate(preset::wavedash, 7), movement::generate(preset::backdash_cancel), movement::generate(preset::crouch), movement::generate(preset::backdash)})
	{
		std::uint32_t position = loop.flow_at, remaining = 0, overrun = 0;
		const auto first = movement::gap_word(loop, position, remaining, overrun, screen_side::right, false, loop.flow_at);
		check(first.done && first.flow && position == 0 && remaining == 0, "the hand-over point ends the gap");
		position = loop.flow_at;
		remaining = 0;
		const auto again = movement::gap_word(loop, position, remaining, overrun, screen_side::right, false, loop.flow_at);
		check(again.done && again.flow && again.word == first.word, "restoring the saved cursor replays the same hand-over");
	}
	const auto fast = movement::generate(preset::wavedash, 4);
	std::uint32_t position = fast.flow_at, remaining = 10, overrun = 0;
	const auto on = movement::gap_word(fast, position, remaining, overrun, screen_side::right, false, fast.flow_at);
	check(!on.done && on.word == movement::play_word(f, screen_side::right) && position == fast.flow_at + 1 && remaining == 9,
		"with time left the pass goes on with its release forward");
	// A tape of several passes hands over at any pass's df, and a gap whose time is up ends at any pass boundary.
	const auto alternating = movement::generate(preset::wavedash, 8);
	check(alternating.pass_ends == std::vector<std::uint32_t>{8, 15} && alternating.flow_at == 4, "8 a second: passes of 8 and 7 frames");
	std::uint32_t second = 8 + 4, second_left = 3, second_over = 0;
	check(movement::gap_word(alternating, second, second_left, second_over, screen_side::left, false, alternating.flow_at).flow && second == 0,
		"the second pass hands over after its own df");
	std::uint32_t too_soon = 8 + 4, too_soon_left = 4, too_soon_over = 0;
	check(!movement::gap_word(alternating, too_soon, too_soon_left, too_soon_over, screen_side::left, false, alternating.flow_at).done,
		"with more time than the pass has left, it goes on");
	std::uint32_t boundary = 8, boundary_left = 0, boundary_over = 0;
	check(movement::gap_word(alternating, boundary, boundary_left, boundary_over, screen_side::left).done && boundary == 0,
		"with no time left the gap ends between two passes");
	check(movement::at_pass_start(alternating, 8) && !movement::at_pass_start(alternating, 4) && movement::at_pass_start(alternating, 15),
		"each pass boundary is a pass start");
	check(movement::pass_at(alternating, 3).start == 0 && movement::pass_at(alternating, 8).start == 8 && movement::pass_at(alternating, 15).end == 15,
		"a cursor at the end belongs to the last pass");
	// Pass starts: the tape's start and each pass end, never inside a held input; a one-shot's is due with its time.
	const auto crouch = movement::generate(preset::crouch), kbd = movement::generate(preset::backdash_cancel), bd = movement::generate(preset::backdash);
	check(movement::at_pass_start(crouch, 0) && !movement::at_pass_start(crouch, 1) && !movement::at_pass_start(movement::input_sequence{}, 0),
		"a held crouch has one pass start, before it");
	check(movement::at_pass_start(kbd, 0) && movement::at_pass_start(kbd, 15) && !movement::at_pass_start(kbd, 2) && movement::at_pass_start(kbd, 99),
		"a dash loop's pass starts are its start and each loop's end, not inside its b, n, b");
	check(!movement::pass_due(bd, 0, 13) && movement::pass_due(bd, 0, 12) && movement::pass_due(bd, 0, 0), "a one-shot is due when its motion ends the interval");
	// Held at a loop end, a dash loop goes back to its lead-in.
	std::uint32_t wrap = 15, frames_left = 5, over = 0;
	const auto held = movement::gap_word(kbd, wrap, frames_left, over, screen_side::right, true);
	check(held.word == 0 && !held.done && wrap == 0 && frames_left == 4, "a dash loop held at its end goes back to the lead-in");
	// A held one-shot whose interval is over keeps waiting: it never skips its motion.
	std::uint32_t start = 0, none = 0, none_over = 0;
	for (int frame = 0; frame < 300; ++frame)
	{
		const auto waiting = movement::gap_word(bd, start, none, none_over, screen_side::right, true);
		check(!waiting.done && waiting.word == 0 && start == 0, "a held one-shot waits");
	}
	check(movement::gap_word(bd, start, none, none_over, screen_side::right, false).word == movement::play_word(b, screen_side::right),
		"released, it plays its motion");
}

PBD_TEST(PbdPracticeMovementPresets, DashEndFlowPlans)
{
	using words = std::vector<std::uint16_t>;
	using movement::wavedash_end;
	const words f4{static_cast<std::uint16_t>(f | 0x20)}, b2{static_cast<std::uint16_t>(b | 0x10)}, button2{0x10}, df1{static_cast<std::uint16_t>(d | f | 0x80)};
	constexpr std::uint16_t four = 0x20;
	const auto fd_loop = movement::generate(preset::forward_dash), kbd_loop = movement::generate(preset::backdash_cancel);
	check(fd_loop.dash_way == f && kbd_loop.dash_way == b && !movement::generate(preset::backdash).dash_way &&
			  !movement::generate(preset::wavedash).dash_way,
		"only the dash loops take a Dash End");
	for (const auto* take : {&f4, &button2, &b2})
		for (const auto dash : {movement::dash_end::into_move, movement::dash_end::stop_first})
		{
			const auto plan = movement::plan_flow(fd_loop, *take, screen_side::left, wavedash_end::forward, dash);
			check(plan.at == fd_loop.flow_at && plan.lead_frames == 0 && plan.release_frames == 0 &&
					  plan.neutral_frames == (dash == movement::dash_end::stop_first ? movement::dash_stop_neutral_frames : 0u),
				"the Dash End alone shapes a dash loop's hand-over, whatever the take starts with");
		}
	check(movement::plan_flow(movement::generate(preset::wavedash), f4, screen_side::left, wavedash_end::crouch_dash, movement::dash_end::stop_first).neutral_frames ==
			  movement::generate(preset::wavedash).flow_neutral_frames,
		"the Dash End leaves a wavedash alone");
	// Before a take that starts with d/f, Into Move hands over after the loop's last forward, which rolls into it: f, n, d/f
	// would be a crouch dash, and a recorded d/f+2 an electric. Continuous Backdash does the same with d/b; Stop First keeps
	// its neutral.
	const words db4 = expand({{static_cast<std::uint16_t>(d | b | four), 3}, {n, 2}});
	check(movement::plan_flow(fd_loop, df1, screen_side::left).at == movement::forward_dash_tap_frames &&
			  movement::plan_flow(kbd_loop, db4, screen_side::left).at == kbd_loop.flow_at - movement::dash_neutral_frames &&
			  movement::plan_flow(fd_loop, db4, screen_side::left).at == fd_loop.flow_at,
		"Into Move rolls the dash's own direction into a d/f (d/b) take");
	const auto stop_df = movement::plan_flow(fd_loop, df1, screen_side::left, wavedash_end::crouch_dash, movement::dash_end::stop_first);
	check(stop_df.at == fd_loop.flow_at && stop_df.neutral_frames == movement::dash_stop_neutral_frames, "Stop First keeps its neutral");
}
