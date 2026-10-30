#include "pbd_test.h"

#include "Emu/PBD/pbd_replay_viewer_menu.h"
#include "Emu/PBD/pbd_replay_control_buttons.h"
#include "Emu/PBD/pbd_replay_capture.h"

using pbd::test::check;

PBD_TEST(PbdReplayViewer, UnwatchedRecordedRoundsAreSelectableInTheLocalMatch)
{
	using namespace pbd::replay_viewer;
	std::vector<round_row> rows{{1, 1, 0, 510, -1}};
	const auto frame = [](std::uint32_t round) -> std::optional<std::uint32_t>
	{
		return round <= 5 ? std::optional<std::uint32_t>((round - 1) * 2000) : std::nullopt;
	};
	include_recorded_rounds(rows, 0, frame);
	check(rows.size() == 1);
	include_recorded_rounds(rows, 1, frame);
	include_recorded_rounds(rows, 1, frame);
	check(rows.size() == 5 && rows.front().fight == 510);
	for (std::uint32_t i = 0; i < rows.size(); ++i)
		check(rows[i].match == 1 && rows[i].round == i + 1 && rows[i].from == i * 2000);
	controller list;
	context c{false, true, 0, pbd::takeover_phase::watching, static_cast<std::uint32_t>(rows.size()), true, 0, 0};
	list.poll(0, c);
	check(list.poll(triangle, c).kind == command::pause);
	c.paused = true;
	for (unsigned n = 0; n < 4; ++n)
	{
		list.poll(0, c);
		list.poll(down, c);
	}
	list.poll(0, c);
	const auto selected = list.poll(cross, c);
	check(selected.kind == command::round && selected.value == 4 && selected.resume);
	check(rows[selected.value].match == 1 && rows[selected.value].round == 5);
}

PBD_TEST(PbdReplayViewer, RoundListAndShouldersSeekTheSameRecordedFight)
{
	using namespace pbd::replay_viewer;
	using namespace pbd::replay_rewind;
	const std::array<std::uint32_t, 5> starts{140, 2947, 5561, 8143, 10905};
	std::vector<round_row> rows;
	include_recorded_rounds(rows, 1, [&](std::uint32_t round) -> std::optional<std::uint32_t>
		{
			return round <= starts.size() ? std::optional<std::uint32_t>(starts[round - 1]) : std::nullopt;
		});
	std::vector<point> checkpoints;
	for (const auto& row : rows)
		checkpoints.push_back({row.fight + 1000, row.match, row.fight, row.round, true});
	for (std::uint32_t round = 1; round <= rows.size(); ++round)
	{
		controller list, shoulder;
		context c{false, true, 0, pbd::takeover_phase::watching, 5, true, 0, round - 1};
		list.poll(0, c);
		check(list.poll(triangle, c).kind == command::pause);
		c.paused = true;
		list.poll(0, c);
		const auto selected = list.poll(cross, c);
		check(selected.kind == command::round && rows[selected.value].round == round && selected.resume);
		const auto pinned = round_choice(checkpoints, 1, rows[selected.value].round);
		check(pinned && checkpoints[*pinned].frame == rows[selected.value].fight);
		shoulder.poll(0, c);
		check(shoulder.poll(r1, c).kind == command::next_round);
		check(!round_forward_finished(1, round, 1, true, round, false));
		check(round_forward_finished(1, round, 1, true, round, true));
		shoulder.poll(0, c);
		check(shoulder.poll(l1, c).kind == command::previous_round);
		shoulder.barrier(l1);
		check(shoulder.poll(l1, c).kind == command::none);
		shoulder.poll(0, c);
		check(shoulder.poll(l1, c).kind == command::previous_round);
		check(previous_round_target(round + 1, true) == rows[selected.value].round);
	}
	check(!round_choice(checkpoints, 2, 1) && !round_choice(checkpoints, 1, 6));
}

PBD_TEST(PbdReplayViewer, RightClickUsesBackAndRejectsOldMenus)
{
	using namespace pbd::replay_viewer;
	controller by_pad, by_mouse;
	context c;
	by_pad.poll(0, c);
	by_pad.menu = by_mouse.menu = page::display;
	const auto expected = by_pad.poll(circle, c);
	const auto actual = by_mouse.pointer(pointer_back, c, 0);
	check(actual.kind == expected.kind && by_pad.menu == by_mouse.menu);
	pointer_surface surface;
	snapshot view;
	view.enabled = true;
	view.menu = static_cast<std::uint32_t>(page::display);
	view.match = 2;
	surface.publish(view, {}, 0);
	check(surface.back());
	check(surface.take(page::display, pbd::takeover_phase::watching, 3) == pointer_discard);
	check(surface.back());
	check(surface.take(page::display, pbd::takeover_phase::watching, 2) == pointer_back);
	view.menu = static_cast<std::uint32_t>(page::none);
	surface.publish(view, {}, 0);
	check(!surface.back());
}

PBD_TEST(PbdReplayViewer, PublicCapturePreservesTheQualifiedMemoryPolicy)
{
	using namespace pbd::replay_capture;
	const auto pages = pbd_pages();
	check(!pages.empty());
	for (std::size_t i = 0; i < pages.size(); ++i)
		check(!(pages[i] & 4095) && (!i || pages[i - 1] < pages[i]));
	for (const char* mode : {"", "fast", "1", "relaunch", "invalid"})
		for (const char* controls : {"0", "1"})
			for (const char* fast : {"0", "1"})
			{
				const auto find = [&](std::string_view key) -> const char*
				{
					return key == "PBD_REPLAY_CONTROLS" ? controls : key == "PBD_REPLAY_TAKEOVER"  ? mode :
					                                             key == "PBD_REPLAY_TAKEOVER_FAST" ? fast :
					                                                                                 nullptr;
				};
				const std::string_view value(mode);
				check(pbd_requested(find) == (std::string_view(controls) == "1" &&
												 (value == "fast" || ((value == "1" || value == "relaunch") && std::string_view(fast) == "1"))));
			}
}

PBD_TEST(PbdReplayViewer, WatchControlsUseOneMapAndPreserveUnavailableActions)
{
	using namespace pbd::replay_controls;
	check(pbd_watch_command(pbd_buttons.start, 0, false, true, false, false) == pbd_command::pause);
	check(pbd_watch_command(pbd_buttons.start, pbd_buttons.start, false, true, false, false) == pbd_command::none);
	check(pbd_watch_command(pbd_buttons.start, 0, false, false, false, false) == pbd_command::none);
	check(pbd_watch_command(pbd_buttons.start, 0, false, false, true, false) == pbd_command::skip_scene);
	check(pbd_watch_command(pbd_buttons.step_forward, 0, true, true, false, false) == pbd_command::step);
	check(pbd_watch_command(pbd_buttons.step_forward, pbd_buttons.step_forward, true, true, false, false) == pbd_command::step);
	check(pbd_watch_command(pbd_buttons.step_forward, 0, false, true, false, false) == pbd_command::none);
	check(pbd_watch_command(pbd_buttons.faster, 0, true, true, false, false) == pbd_command::faster);
	check(pbd_watch_command(pbd_buttons.slower, 0, false, true, false, false) == pbd_command::slower);
	for (const auto unavailable : {pbd_buttons.step_back, pbd_buttons.jump_back, pbd_buttons.jump_forward})
		check(pbd_watch_command(unavailable, 0, true, true, true, false) == pbd_command::none);
	check(pbd_watch_command(pbd_buttons.take, 0, false, true, false, false) == pbd_command::none);
	check(pbd_watch_command(pbd_buttons.take, 0, false, true, false, true) == pbd_command::take);
	check(pbd_watch_command(pbd_buttons.select | pbd_buttons.start, 0, true, true, true, true) == pbd_command::leave);
	check(pbd_keyboard[0].second == pbd_buttons.start && pbd_keyboard[1].second == pbd_buttons.start);
	// A second controller and keyboard shortcuts can control Watch without becoming the taken fighter's input.
	const std::array<std::uint16_t, 2> pads{0x1040, 0x4020};
	check(pbd_taken_word(0x50e4, pads, 1) == 0x1040);
	check(pbd_taken_word(0x50e4, pads, 2) == 0x4020);
	check(pbd_taken_word(0, pads, 1) == 0);
	check(pbd_taken_word(0xffff, pads, 0) == 0 && pbd_taken_word(0xffff, pads, 3) == 0);
	for (const auto game : {pbd_buttons.slower, pbd_buttons.faster, pbd_buttons.step_forward,
			 pbd_buttons.step_back, pbd_buttons.jump_forward, pbd_buttons.jump_back, pbd_buttons.take})
		check(pbd_taken_command(game, 0) == pbd_command::none);
	check(pbd_taken_command(pbd_buttons.start, 0) == pbd_command::pause);
	check(pbd_taken_command(pbd_buttons.start, pbd_buttons.start) == pbd_command::none);
	check(pbd_taken_command(pbd_buttons.select | pbd_buttons.start, 0) == pbd_command::leave);
}

PBD_TEST(PbdReplayViewer, AttemptModifierChordsNeverLeaveOrReachTheFighter)
{
	using namespace pbd::replay_controls;
	constexpr std::uint16_t cross = 0x0040, circle = 0x0020, select = pbd_buttons.select;
	check(pbd_attempt_command(select, 0) == pbd_command::none);
	check(pbd_attempt_command(select | cross, select) == pbd_command::retry);
	check(pbd_attempt_command(select | cross, cross) == pbd_command::retry);
	check(pbd_attempt_command(select | cross, 0) == pbd_command::retry);
	check(pbd_attempt_command(select | cross, select | cross) == pbd_command::none);
	check(pbd_attempt_command(select | circle, select) == pbd_command::return_to_replay);
	check(pbd_attempt_command(select | circle, circle) == pbd_command::return_to_replay);
	check(pbd_attempt_command(select | circle, 0) == pbd_command::return_to_replay);
	check(pbd_attempt_command(select | cross | circle, select) == pbd_command::none);
	check(pbd_attempt_command(cross, 0) == pbd_command::none);
	check(pbd_attempt_command(circle, 0) == pbd_command::none);
	check(pbd_attempt_command(0, select) == pbd_command::leave);
	check(pbd_attempt_command(pbd_buttons.start, 0) == pbd_command::pause);
	const std::array<std::uint16_t, 2> pads{select | cross, select | circle};
	check(pbd_taken_word(select | cross | circle, pads, 1) == 0);
	check(pbd_taken_word(select | cross | circle, pads, 2) == 0);
	check(pbd_taken_word(pbd_buttons.start | cross, {pbd_buttons.start | cross, 0}, 1) == cross);
}

PBD_TEST(PbdReplayViewer, PublicWatchControllerCanTakeEitherFighter)
{
	using namespace pbd::replay_controls;
	constexpr std::uint16_t cross = 0x0040, left = 0x8000;
	// Watch leaves the menu channel on port 1 and moves the configured live controller to port 2.
	const std::array<std::uint16_t, 2> pads{0, cross | left};
	for (std::uint32_t side : {1u, 2u})
	{
		check(pbd_taken_input_port(side, 2) == 2);
		check(pbd_taken_word(cross | left, pads, pbd_taken_input_port(side, 2)) == (cross | left));
		check(pbd_taken_word(pbd_buttons.select | cross, pads, pbd_taken_input_port(side, 2)) == 0);
		check(pbd_taken_input_port(side, 0) == side);
	}
	check(pbd_taken_input_port(0, 2) == 0 && pbd_taken_input_port(3, 2) == 0);
	check(pbd_take_side({0, pbd_buttons.take}, 2) == 1);
	check(pbd_take_side({pbd_buttons.take, 0}, 2) == 1);
	check(pbd_take_side({0, pbd_buttons.take}, 0) == 2);
	check(pbd_take_side({0, pbd_buttons.take}, 2, 2) == 2);
	check(pbd_take_side({pbd_buttons.take, 0}, 2, 2) == 2);
	check(pbd_take_side({0, pbd_buttons.take}, 0, 1) == 1);
	check(pbd_take_side({0, pbd_buttons.take}, 2, 3) == 1);
}
PBD_TEST(PbdReplayViewer, ConfirmationSideChoicePreservesYesAndNo)
{
	using namespace pbd::replay_controls;
	using namespace pbd::replay_viewer;
	for (const auto initial_side : {1u, 2u})
	{
		check(pbd_prompt_side(right, 0, initial_side) == 2);
		check(pbd_prompt_side(left, 0, initial_side) == 1);
		check(pbd_prompt_side(right, right, initial_side) == initial_side);
		check(pbd_prompt_side(left | right, 0, initial_side) == initial_side);
		check(pbd_prompt_side(select | right, 0, initial_side) == initial_side);
	}
	for (const auto choice : {0u, 1u})
	{
		controller prompt;
		context cx{true, true, 0, pbd::takeover_phase::enable_prompt, 0, true};
		prompt.poll(0, cx);
		prompt.take_choice = choice;
		check(prompt.poll(right, cx).kind == command::none && prompt.take_choice == choice);
		prompt.poll(0, cx);
		check(prompt.poll(cross, cx).kind == (choice ? command::cancel_arm : command::arm));
		check(prompt.poll(cross | right, cx).game == 0);
	}
	snapshot view{};
	view.takeover = pbd::takeover_phase::enable_prompt;
	view.port = pbd_prompt_side(right, 0, 1);
	const auto layout = menu_layout(view);
	check(layout.count == 2);
	check(take_prompt_title(2, "Devil Jin\nYes").find('\n') == std::string::npos);
}

#include "replay_viewer_model.h"
// namespace

static void round_coordinates_and_seek_targets()
{
	std::vector<round_row> rounds;
	check(parse_rounds("2:1:0:512:-1;1:2:2200:2520:4000;1:1:0:510:2200", rounds));
	check(rounds.size() == 3 && rounds[0].round == 1 && rounds[1].from == 2200 && rounds[2].match == 2);
	for (const auto text : {"1:1:0:2:-2", "1:1:3:2:-1", "1:0:0:2:-1", "1:1:0:2:1", "1:1:0:2:-1;1:1:0:2:-1", "1:1:0:2", "1:1:0:2:-1x", "1:1:0:2:-1;;", "1:1:0:99999999999:-1"})
		check(!parse_rounds(text, rounds) && rounds.empty());
	check(!parse_rounds(std::string(16385, '0'), rounds));
	check(parse_rounds("", rounds) && rounds.empty());
	check(!parse_rounds("1:2:0:512:-1", rounds));
	check(!parse_rounds("1:1:0:512:3000;1:2:2000:2500:-1", rounds));
	check(!parse_rounds("1:1:0:512:-1;1:2:0:514:-1", rounds));
	check(!parse_rounds("1:1:0:216000:-1", rounds));
	check(parse_rounds("1:1:0:215999:-1", rounds));
	check(previous_round_target(2, true) == 1 && previous_round_target(2, false) == 2 && previous_round_target(0, false) == 1);
	check(!seek_finished(2, 500, 1, false, 0));
	check(!seek_finished(2, 500, 2, true, 499));
	check(seek_finished(2, 500, 2, true, 500));
	check(seek_finished(2, 500, 2, false, 0));
	check(seek_finished(2, 500, 3, true, 0));
	check(!round_forward_finished(1, 4, 1, true, 3, false));
	check(round_forward_finished(1, 4, 1, false, 0, false)); // Final KO cannot fast-forward through the next match.
	check(round_forward_finished(1, 4, 2, true, 1, true));
	check(!round_forward_finished(2, 1, 1, false, 0, false)); // Initial skip still reaches the intended match.
	check(!round_forward_finished(1, 2, 1, true, 1, true));
	check(round_forward_finished(1, 2, 1, true, 2, true));
}

static void intro_and_armed_controls()
{
	controller intro;
	context intro_context;
	// Cross stays free while watching; R1 moves on to the next round instead.
	check(press(intro, cross, intro_context).kind == command::none);
	intro_context.fighting = true;
	check(press(intro, cross, intro_context).kind == command::none);
	controller armed_intro;
	context armed_context{false, true, 0, pbd::takeover_phase::armed, 0, false};
	check(press(armed_intro, cross, armed_context).kind == command::none);
	armed_context.fighting = true;
	check(armed_intro.poll(cross, armed_context).kind == command::none); // Held intro press cannot start the clock.
	check(press(armed_intro, cross, armed_context).kind == command::start_take);
	snapshot armed_view;
	armed_view.takeover = pbd::takeover_phase::armed;
	armed_view.frames_left = 600;
}

static void playback_menu_actions(controller& c, context& cx)
{
	// A replay opened while Circle is held cannot accidentally pause or confirm.
	check(c.poll(circle, cx).kind == command::none);
	check(c.poll(circle, cx).kind == command::none);
	check(press(c, circle, cx).kind == command::pause);
	check(c.poll(circle, cx).kind == command::none);
	// Opening Options takes ownership and requires a complete release, including stick directions.
	check(press(c, start, cx).kind == command::pause && c.menu == page::playback);
	cx.paused = true;
	check(c.poll(start | cross, cx).kind == command::none);
	check(c.poll(cross, cx).kind == command::none);
	check(press(c, down, cx).kind == command::none && c.choice == 1);
	// Resume, Take Control: Player 1 / Player 2, Playback Speed, Round Select, Display, Return to Launcher, Exit Game.
	press(c, down, cx);
	press(c, down, cx);
	check(c.choice == 3);
	press(c, cross, cx);
	check(c.menu == page::speed && c.choice == 2 && c.parent == page::playback);
	check(c.poll(cross | down, cx).kind == command::none && c.choice == 2);
	press(c, down, cx);
	check(c.choice == 3);
	const auto faster = press(c, cross, cx);
	// A picked speed goes back to the Options row that opened the list.
	check(faster.kind == command::speed && faster.value == 1 && c.menu == page::playback && c.choice == 3);
	check(press(c, circle, cx).kind == command::pause && c.menu == page::none);
	// Resume plays; Options closes the menu from any page.
	check(press(c, start, cx).kind == command::none && c.menu == page::playback);
	check(press(c, cross, cx).kind == command::pause && c.menu == page::none);
}

static void deep_menu_and_round_list()
{
	{
		controller deep;
		context dx{false, true, 0, pbd::takeover_phase::watching, 3, true};
		check(press(deep, start, dx).kind == command::pause);
		dx.paused = true;
		for (unsigned n = 0; n < 5; ++n)
			press(deep, down, dx);
		press(deep, cross, dx);
		check(deep.menu == page::display && deep.parent == page::playback);
		check(press(deep, circle, dx).kind == command::none && deep.menu == page::playback && deep.choice == 5);
		press(deep, cross, dx);
		check(press(deep, start, dx).kind == command::pause && deep.menu == page::none && deep.parent == page::none);
		// The two leaving rows close the menu and ask for the launcher (1) or the game's end (2).
		press(deep, start, dx);
		press(deep, up, dx);
		const auto quit = press(deep, cross, dx);
		check(quit.kind == command::leave && quit.value == 2 && deep.menu == page::none);
		press(deep, start, dx);
		press(deep, up, dx);
		press(deep, up, dx);
		const auto launcher = press(deep, cross, dx);
		check(launcher.kind == command::leave && launcher.value == 1 && deep.menu == page::none);
	}
	// Triangle (Switch Rounds) opens the round list on the current round; a pick from a playing replay plays on.
	{
		controller list;
		context lx{false, true, 0, pbd::takeover_phase::watching, 3, true, 0, 2};
		check(press(list, triangle, lx).kind == command::pause && list.menu == page::rounds && list.choice == 2);
		lx.paused = true;
		press(list, up, lx);
		const auto picked = press(list, cross, lx);
		check(picked.kind == command::round && picked.value == 1 && picked.resume && list.menu == page::none);
		check(press(list, triangle, lx).kind == command::none && list.menu == page::rounds);
		const auto still = press(list, cross, lx);
		check(still.kind == command::round && still.value == 2 && !still.resume);
		check(press(list, triangle, lx).kind == command::none);
		check(press(list, circle, lx).kind == command::none && list.menu == page::none);
	}
}

static void display_menu_actions(controller& c, context& cx)
{
	c = {};
	cx = {true, true, 0, pbd::takeover_phase::watching, 3};
	press(c, select, cx);
	check(c.menu == page::display);
	press(c, cross, cx);
	check(c.history);
	press(c, down, cx);
	press(c, cross, cx);
	check(!c.frame_panel);
	press(c, circle, cx);
	check(c.menu == page::none);
	// The replay buttons: Left / Right rewind and fast forward, L1 / R1 the previous / next round, L3 / R3 take a side.
	check(press(c, right, cx).kind == command::seek_forward);
	check(press(c, left, cx).kind == command::seek_back);
	check(press(c, l1, cx).kind == command::previous_round);
	check(press(c, r1, cx).kind == command::next_round);
	check(press(c, l3, cx).kind == command::take_over && press(c, l3, cx).value == 1);
	check(press(c, r3, cx).kind == command::take_over && press(c, r3, cx).value == 2);
	// A held direction repeats after the delay, then at the interval; a second direction or a release stops it.
}

static void direction_repeat_navigation()
{
	{
		controller held;
		context clock{false, true, 0, pbd::takeover_phase::watching, 3, true, 1000};
		held.poll(0, clock);
		check(held.poll(left, clock).kind == command::seek_back);
		clock.now_ms = 1000 + pbd::replay_rewind::repeat_delay_ms - 1;
		check(held.poll(left, clock).kind == command::none);
		clock.now_ms = 1000 + pbd::replay_rewind::repeat_delay_ms;
		check(held.poll(left, clock).kind == command::seek_back);
		clock.now_ms += pbd::replay_rewind::repeat_interval_ms;
		check(held.poll(left, clock).kind == command::seek_back);
		clock.now_ms += pbd::replay_rewind::repeat_interval_ms;
		check(held.poll(left | right, clock).kind == command::seek_forward); // the fresh Right acts once
		clock.now_ms += 10 * pbd::replay_rewind::repeat_interval_ms;
		check(held.poll(left | right, clock).kind == command::none);
		check(held.poll(0, clock).kind == command::none);
	}
}

static void round_choice_and_pause(controller& c, context& cx)
{
	c.menu = page::rounds;
	c.choice = 0;
	const auto chosen = press(c, cross, cx);
	check(chosen.kind == command::round && chosen.value == 0 && c.menu == page::none);
	snapshot listed;
	listed.menu = static_cast<std::uint32_t>(page::rounds);
	listed.count = 1;
	listed.rounds[0] = 1;
	const auto rounds_view = menu_layout(listed);
	check(rounds_view.kind == menu_view::shape::list && rounds_view.count == 1);
	c.menu = page::none;
	check(press(c, r2, cx).kind == command::step);
	check(press(c, l2, cx).kind == command::step_back);
	cx.paused = false;
	check(press(c, l2, cx).kind == command::speed);
	cx.paused = true;

	// Display: P1 / P2 Command History (both Off at first) and Frame Data; a menu confirm never leaks into the game.
}

static void display_preferences_per_side()
{
	controller per_side;
	context display_context{true, true, 0, pbd::takeover_phase::watching, 3, true};
	check(!per_side.history && !per_side.history_sides[0] && !per_side.history_sides[1]);
	check(!history_visible(display_flags(per_side, false), 0) && !history_visible(display_flags(per_side, false), 1));
	press(per_side, select, display_context);
	check(per_side.poll(select | cross, display_context).game == 0 && !per_side.history);
	check(press(per_side, cross, display_context).game == 0 && per_side.history && per_side.history_sides[0] && !per_side.history_sides[1]);
	check(history_visible(display_flags(per_side, true), 0) && !history_visible(display_flags(per_side, true), 1));
	press(per_side, down, display_context);
	check(per_side.choice == 1 && press(per_side, cross, display_context).game == 0);
	check(per_side.history_sides[1] && history_visible(display_flags(per_side, true), 1));
	check(per_side.poll(cross, display_context).game == 0 && per_side.history_sides[1]);
	press(per_side, cross, display_context);
	press(per_side, up, display_context);
	press(per_side, cross, display_context);
	check(!per_side.history && !history_visible(display_flags(per_side, true), 0) && !history_visible(display_flags(per_side, true), 1));
	// Frame Data toggles both players' panels; three rows wrap; Circle closes a page Select opened.
	press(per_side, down, display_context);
	press(per_side, down, display_context);
	check(per_side.choice == 2 && press(per_side, cross, display_context).kind == command::none && per_side.frame_panel);
	check((display_flags(per_side, true) & 2u) != 0);
	press(per_side, cross, display_context);
	check(!per_side.frame_panel && !(display_flags(per_side, true) & 2u));
	press(per_side, down, display_context);
	check(per_side.choice == 0);
	check(press(per_side, circle, display_context).kind == command::none && per_side.menu == page::none);
}

static void display_preferences_survive_navigation()
{
	// Publish every side combination through the exact helper used by the latch, then filter both renderer paths.
	for (const bool p1 : {false, true})
		for (const bool p2 : {false, true})
		{
			controller preferences;
			preferences.history_sides = {p1, p2};
			preferences.history = p1 || p2;
			preferences.frame_panel = true;
			preferences.attack_panel = true;
			snapshot mapped;
			mapped.flags = display_flags(preferences, true);
			mapped.menu = static_cast<std::uint32_t>(page::display);
			mapped.choice = 1;
			mapped.history[0][0] = {static_cast<word>(right | cross), 7};
			mapped.history[1][0] = {static_cast<word>(left | circle), 11};
			mapped.fighters[0].startup = 13;
			mapped.fighters[0].on_block = -8;
			mapped.fighters[1].startup = 17;
			mapped.fighters[1].on_hit = 4;
			presentation.store(mapped);
			const auto received = presentation.load();
			check(received.fighters[0].startup == 13u && received.fighters[0].on_block == -8);
			check(received.fighters[1].startup == 17u && received.fighters[1].on_hit == 4);
			check((received.flags & 15) == ((p1 || p2) ? 15u : 14u));
			check(history_visible(received.flags, 0) == p1);
			check(history_visible(received.flags, 1) == p2);
			check(!history_visible(received.flags, 2));
			check(received.history[0][0].input == (right | cross) && received.history[0][0].held == 7);
			check(received.history[1][0].input == (left | circle) && received.history[1][0].held == 11);
			const auto layout = menu_layout(received);
			check(layout.kind == menu_view::shape::list && layout.count == 3 && layout.focus == 1);
		}
}

static void takeover_menu_selection()
{
	// Options must carry the original pause state into the takeover confirmation, just like direct stick clicks.
	for (const bool paused : {false, true})
		for (const bool right_side : {false, true})
		{
			controller menu_controller;
			context menu_context{paused, true, 0, pbd::takeover_phase::watching, 3, true};
			check(press(menu_controller, start, menu_context).kind == (paused ? command::none : command::pause));
			menu_context.paused = true;
			for (unsigned n = 0; n < (right_side ? 2u : 1u); ++n)
				press(menu_controller, down, menu_context);
			const auto ask = press(menu_controller, cross, menu_context);
			check(ask.kind == command::take_over && ask.value == (right_side ? 2 : 1) && menu_controller.menu == page::none);
			menu_context.takeover = pbd::takeover_phase::enable_prompt;
			check(menu_controller.poll(cross, menu_context).kind == command::none);
			check(press(menu_controller, circle, menu_context).kind == command::cancel_arm);
			controller direct_controller;
			menu_context.takeover = pbd::takeover_phase::watching;
			menu_context.paused = paused;
			const auto direct = press(direct_controller, right_side ? r3 : l3, menu_context);
			check(direct.kind == ask.kind && direct.value == ask.value);
		}
}

static void takeover_controller_transitions(controller& c, context& cx)
{
	c = {};
	cx = {true, true, 0, pbd::takeover_phase::enable_prompt, 3};
	check(c.poll(cross, cx).kind == command::none);
	check(press(c, cross, cx).kind == command::arm);
	cx.takeover = pbd::takeover_phase::armed;
	check(c.poll(cross, cx).kind == command::none);
	check(c.poll(cross | right, cx).game == 0);
	cx.fighting = true;
	const auto take = press(c, cross | right, cx);
	check(take.kind == command::start_take && take.game == (cross | right));
	// The native start_takeover handler resumes before publishing the active controller context.
	cx.paused = false;
	cx.takeover = pbd::takeover_phase::controlling;
	check(c.poll(cross | right, cx).game == (cross | right));
	check(press(c, select | right | cross, cx).kind == command::retry && c.poll(select | right | cross, cx).game == 0);
	check(press(c, select | circle, cx).kind == command::return_replay);
	check(press(c, start, cx).kind == command::pause && c.menu == page::takeover_options);
	cx.paused = true;
	check(c.poll(start | cross, cx).game == 0 && c.poll(cross, cx).game == 0);
	check(press(c, circle, cx).kind == command::pause && c.menu == page::none);
	cx.paused = false;
	check(c.poll(circle, cx).game == 0);
	check(press(c, right, cx).game == right);
	press(c, select | right, cx);
	check(c.poll(right, cx).game == 0 && c.poll(right, cx).game == 0);
	check(press(c, cross, cx).game == cross);
	// Time-up opening on a held attack never chooses Retry; Bookmark cannot claim a Practice snapshot.
	cx.takeover = pbd::takeover_phase::end_prompt;
	check(c.poll(cross, cx).kind == command::none);
	press(c, down, cx);
	press(c, down, cx);
	check(press(c, cross, cx).kind == command::bookmark);
}

static void takeover_prompt_publication(controller& c, context& cx, snapshot& s)
{
	s.takeover = pbd::takeover_phase::end_prompt;
	s.take_choice = 2;
	const auto prompt_view = menu_layout(s);
	check(prompt_view.kind == menu_view::shape::dialog && prompt_view.count == 3 && prompt_view.focus == 2);
	check(prompt_view.count == 3);
	s.fast = 1;
	check(menu_layout(s).kind == menu_view::shape::none);
	c.barrier(cross);
	check(c.poll(cross, cx).game == 0);
	check(press(c, circle, cx).kind == command::return_replay);
	// Leave wins when multiple buttons are held, including while controlling the fighter.
	cx.takeover = pbd::takeover_phase::controlling;
	check(press(c, start | select | cross, cx).kind == command::none && c.poll(start | select | cross, cx).game == 0);

	// Armed/active Options and nested Display preserve ownership and the earlier pause state.
	// Retry/Return stay unavailable while armed because no first-fight-input anchor exists yet.
}

#include "replay_viewer_takeover_checks.h"

static void command_history_and_control_prompts(snapshot& s)
{
	command_history history;
	history.update(10, cross);
	history.update(10, cross);
	history.update(11, cross);
	check(history.rows[0].held == 2);
	history.update(12, circle);
	check(history.rows[1].input == cross && history.rows[1].held == 2);
	history.update(5, right);
	check(history.rows[0].input == right && history.rows[1].held == 0);
	s.enabled = 1;
	s.flags = 7;
	s.history[0] = history.rows;
	s.count = 1;
	s.rounds[0] = 4;
	presentation.store(s);
	const auto loaded = presentation.load();
	check(loaded.history[0][0].input == right && loaded.flags == 7 && loaded.rounds[0] == 4);
	// Sixteen rows survive publication, the same depth as the Practice history.
	for (unsigned n = 0; n < 20; ++n)
		history.update(20 + n, (n & 1) ? cross : circle);
	check(history.rows[history_rows - 1].held == 1 && history.rows[history_rows - 1].input == circle);
	s.history[1] = history.rows;
	presentation.store(s);
	check(presentation.load().history[1][history_rows - 1].held == 1);
	s.takeover = pbd::takeover_phase::watching;
	s.menu = 0;
	s.fast = 0;
	s.paused = 0;
	const auto playing = prompts(s);
	check(playing.count > 0 && playing.items[0].keys == "{circle}");
	bool options_prompt = false, exit_prompt = false;
	for (std::uint32_t i = 0; i < playing.count; ++i)
	{
		options_prompt |= playing.items[i].pointer == pointer_options;
		exit_prompt |= playing.items[i].pointer == pointer_exit && playing.items[i].keys == "{start}+{select}";
	}
	check(options_prompt && exit_prompt);
	// The control bar's order, with the leave hold last (the first to go when the bar runs out of room).
	{
		const std::array<std::string_view, 8> keys{"{circle}", "{l3}/{r3}", "{triangle}", "{l1}/{r1}", "{l2}/{r2}", "{start}", "{dpad:lr}", "{start}+{select}"};
		check(playing.count == keys.size());
		for (std::size_t i = 0; i < keys.size(); ++i)
			check(playing.items[i].keys == keys[i]);
		check(playing.items[1].label == "Take control P1/P2");
		snapshot paused_view = s;
		paused_view.paused = 1;
		check(prompts(paused_view).items[4].keys == "{l2}/{r2}");
		snapshot control_view = s;
		control_view.takeover = pbd::takeover_phase::controlling;
		const auto in_control = prompts(control_view);
		check(in_control.items[0].keys == "{start}" && in_control.items[1].keys == "{select}+{cross}" && in_control.items[2].keys == "{select}+{circle}");
	}
	s.takeover = pbd::takeover_phase::end_prompt;
	check(prompts(s).items[0].keys == "{cross}");
}

static void pointer_targets_and_scaled_coordinates()
{
	{
		controller mouse;
		context mx{false, true, 0, pbd::takeover_phase::watching, 4, true};
		check(mouse.pointer(pointer_options, mx, 0).kind == command::pause && mouse.menu == page::playback);
		mx.paused = true;
		check(mouse.pointer(4, mx, cross).kind == command::none && mouse.menu == page::rounds); // Round Select opens its list
		check(mouse.poll(cross, mx).kind == command::none && mouse.poll(cross, mx).game == 0);
		mouse.poll(0, mx);
		mouse.pointer(pointer_options, mx, 0); // Back: from the list to its Options row.
		check(mouse.menu == page::playback && mouse.choice == 4);
		check(mouse.pointer(3, mx, 0).kind == command::none && mouse.menu == page::speed);
		for (unsigned speed = 0; speed < 5; ++speed)
		{
			const auto a = mouse.pointer(speed, mx, 0);
			check(a.kind == command::speed && a.value == static_cast<int>(speed) - 2 && a.game == 0 && mouse.menu == page::playback);
			mouse.pointer(3, mx, 0);
		}
		mouse.pointer(pointer_options, mx, 0); // Back from the speeds to the Options menu.
		check(mouse.menu == page::playback);
		check(mouse.pointer(7, mx, 0).kind == command::leave && mouse.menu == page::none);
		mouse.pointer(pointer_options, mx, 0);
		const auto taken = mouse.pointer(2, mx, 0);
		check(taken.kind == command::take_over && taken.value == 2 && mouse.menu == page::none);
		mx.takeover = pbd::takeover_phase::enable_prompt;
		check(mouse.pointer(0, mx, cross).kind == command::arm);
		check(mouse.poll(cross, mx).kind == command::none); // The mouse confirm cannot become the first fight input.
		mx.takeover = pbd::takeover_phase::controlling;
		check(mouse.pointer(pointer_options, mx, cross).kind == command::none && mouse.menu == page::takeover_options);
		check(mouse.pointer(1, mx, cross).kind == command::retry);
		check(mouse.pointer(pointer_options, mx, 0).kind == command::none);
		check(mouse.pointer(2, mx, 0).kind == command::return_replay);
		check(mouse.pointer(pointer_options, mx, 0).kind == command::none);
		const auto launcher = mouse.pointer(4, mx, 0);
		check(launcher.kind == command::leave && launcher.value == 1 && mouse.menu == page::none);
		mx.takeover = pbd::takeover_phase::end_prompt;
		check(mouse.pointer(0, mx, cross).kind == command::retry);
		check(mouse.pointer(1, mx, cross).kind == command::return_replay);
		check(mouse.pointer(pointer_exit, mx, cross).kind == command::exit);
		check(mouse.poll(cross, mx).game == 0);

		pointer_surface surface;
		snapshot shown;
		shown.enabled = 1;
		shown.match = 2;
		shown.menu = static_cast<unsigned>(page::playback);
		std::array<pointer_rectangle, 12> rects{};
		rects[0] = {100, 200, 300, 24, 1};
		rects[1] = {0, 600, 100, 30, pointer_exit};
		surface.publish(shown, rects, 2);
		check(surface.hit(100, 200) == 1 && surface.hit(399, 223) == 1 && surface.hit(400, 200) == pointer_none);
		check(surface.press(101, 201) && !surface.press(10, 610)); // A pending click is never replaced by Exit.
		check(surface.take(page::playback, pbd::takeover_phase::watching, 2) == 1 && surface.take(page::playback, pbd::takeover_phase::watching, 2) == pointer_none);
		check(surface.press(101, 201) && surface.take(page::speed, pbd::takeover_phase::watching, 2) == pointer_discard);
		check(surface.press(101, 201) && surface.take(page::playback, pbd::takeover_phase::end_prompt, 2) == pointer_discard);
		check(surface.press(101, 201) && surface.take(page::playback, pbd::takeover_phase::watching, 3) == pointer_discard);
		check(surface.press(800, 500) && surface.take(page::playback, pbd::takeover_phase::watching, 2) == pointer_discard); // Outside clicks cannot become game input.
		check(surface.press(10, 610) && surface.take(page::playback, pbd::takeover_phase::watching, 2) == pointer_exit);
		check(surface.press(101, 201));
		surface.clear();
		check(surface.take(page::playback, pbd::takeover_phase::watching, 2) == pointer_none && surface.hit(101, 201) == pointer_none);
		// Identical drawn targets remain clickable with letterboxing and 125/150/175% physical scaling.
		for (const auto scale : {1.f, 1.25f, 1.5f, 1.75f})
		{
			const pointer_rectangle viewport{static_cast<int>(100 * scale), static_cast<int>(50 * scale),
				static_cast<int>(1280 * scale), static_cast<int>(720 * scale)};
			const auto target = pointer_coordinates(201 * scale, 251 * scale, viewport);
			surface.publish(shown, rects, 2);
			check(surface.press(target.x, target.y) && surface.take(page::playback, pbd::takeover_phase::watching, 2) == 1);
			const auto black_bar = pointer_coordinates(50 * scale, 251 * scale, viewport);
			check(surface.press(black_bar.x, black_bar.y) && surface.take(page::playback, pbd::takeover_phase::watching, 2) == pointer_discard);
		}
		const auto empty = pointer_coordinates(0, 0, {});
		check(empty.x < 0 && empty.y < 0);
	}
}

PBD_TEST(PbdReplayViewer, TakeoverPromptUsesDeadlineAndFightTail)
{
	check(!takeover_prompt_due(pbd::takeover_phase::armed, 1600, 1600, true, 1599, 1000));
	check(!takeover_prompt_due(pbd::takeover_phase::end_prompt, 1600, 1600, true, 1599, 1000));
	check(!takeover_prompt_due(pbd::takeover_phase::controlling, 1599, 1600, true, 1598, 1000));
	check(takeover_prompt_due(pbd::takeover_phase::controlling, 1600, 1600, true, 1599, 1000));
	check(!takeover_prompt_due(pbd::takeover_phase::controlling, 1507, 1600, false, 1500, 1000));
	check(takeover_prompt_due(pbd::takeover_phase::controlling, 1508, 1600, false, 1500, 1000));
	check(!takeover_prompt_due(pbd::takeover_phase::controlling, 1508, 1600, false, 999, 1000));
}

PBD_TEST(PbdReplayViewer, Checks)
{
	round_coordinates_and_seek_targets();
	intro_and_armed_controls();
	controller c;
	context cx{false, true, 0, pbd::takeover_phase::watching, 3};
	playback_menu_actions(c, cx);
	deep_menu_and_round_list();

	display_menu_actions(c, cx);
	direction_repeat_navigation();
	round_choice_and_pause(c, cx);
	display_preferences_per_side();
	display_preferences_survive_navigation();
	takeover_menu_selection();
	// Takeover confirmation owns Cross until release. The fresh first fight input reaches the game exactly once.
	takeover_controller_transitions(c, cx);
	snapshot s;
	takeover_prompt_publication(c, cx, s);
	takeover_prompt_owns_input();
	takeover_menu_close_and_resume();
	retry_and_return_commands();
	takeover_display_and_prompt_routing();

	command_history_and_control_prompts(s);
	pointer_targets_and_scaled_coordinates();
}

// A stick click the player gave an action: watching, it still takes control (the viewer reads the button itself); a taken
// fighter's click is that action, but the click that took control is no fight input until it is let go.
PBD_TEST(PbdReplayViewer, StickClickWithAnActionFightsOnceControlIsTaken)
{
	using namespace pbd::replay_viewer;
	controller c;
	context watching{false, true, 0, pbd::takeover_phase::watching, 3, true};
	watching.clicks = r3;
	c.poll(0, watching);
	const auto taken = c.poll(r3, watching);
	check(taken.kind == command::take_over && taken.value == 2 && taken.game == 0);
	context fighting = watching;
	fighting.takeover = pbd::takeover_phase::controlling;
	check(c.poll(r3, fighting).game == 0 && c.poll(r3 | right, fighting).game == right);
	check(c.poll(0, fighting).game == 0);
	check(c.poll(r3 | cross, fighting).game == (r3 | cross));
	check(c.poll(l3, fighting).game == 0);
}
