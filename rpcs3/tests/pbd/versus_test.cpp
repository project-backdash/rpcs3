#include "pbd_test.h"

#include "Emu/Cell/pbd_versus.h"
#include "Emu/PBD/pbd_versus_input.h"

using pbd::test::check;

namespace
{
	using namespace pbd::versus;
	// Named here so a system select() can never make it ambiguous.
	using pbd::versus::select;
	namespace hs = pbd::hidden_selection;

	hs::observation at_cursor(u32 cursor)
	{
		hs::observation o;
		o.scene = select_scene;
		o.subscene = select_subscene;
		o.mode = 4;
		o.grid_mode = 1;
		o.state = choosing;
		o.cursor = cursor;
		o.handicap = 100;
		o.verified = true;
		return o;
	}

	frame select_frame()
	{
		frame f;
		f.versus = true;
		f.scene = select_scene;
		f.subscene = select_subscene;
		f.players = {at_cursor(14), at_cursor(22)};
		f.devices = {"pad-a", "pad-b"};
		f.names = {"Stick A", "Pad B"};
		f.layouts = {default_layout(), default_layout()};
		return f;
	}

	void run(director& d, const frame& f, unsigned count)
	{
		for (unsigned i = 0; i < count; ++i)
			d.step(f);
	}

} // namespace

PBD_TEST(PbdVersus, ControllerSetup)
{
	using pbd::versus::select;
	auto start_layout = default_layout();
	start_layout[5] = "1+2";
	setup s;
	open_setup(s, "Stick A", start_layout, triangle);
	check(step(s, triangle) == setup_effect::none && s.open && s.focus == 0, "the Triangle that opened the panel does nothing in it");
	step(s, 0);
	// Left Punch on Cross: Cross's Left Kick goes to Square, so neither attack loses its button.
	check(step(s, cross) == setup_effect::changed && s.current[2] == "1" && s.current[0] == "3", "two attacks trade buttons");
	step(s, 0);
	check(step(s, select) == setup_effect::none && s.current[2] == "1", "an attack keeps its button");
	step(s, 0);
	for (int i = 0; i < 4; ++i)
	{
		step(s, down);
		step(s, 0);
	}
	check(s.focus == 4 && action_of(4) == "1+2", "Down moves to the next action");
	check(step(s, select) == setup_effect::changed && s.current[5] == "off", "Select leaves a combination with no button");
	step(s, 0);
	check(step(s, r1) == setup_effect::changed && s.current[5] == "1+2", "a shoulder takes a combination");
	step(s, 0);
	check(step(s, up | cross) == setup_effect::changed && s.focus == 3 && s.current[2] == "4", "a press on a row gives it that button");
	step(s, 0);
	s.focus = 0;
	check(step(s, up) == setup_effect::none && s.focus == static_cast<u32>(setup_row::done), "Up from the first row wraps to the last");
	step(s, 0);
	s.focus = static_cast<u32>(setup_row::restore_default);
	check(step(s, cross) == setup_effect::changed && s.current == default_layout(), "Restore Default Settings");
	step(s, 0);
	s.focus = static_cast<u32>(setup_row::restore_previous);
	check(step(s, cross) == setup_effect::changed && s.current == start_layout, "Restore Previous Settings returns to the layout at opening");
	step(s, 0);
	s.focus = static_cast<u32>(setup_row::check);
	check(step(s, cross) == setup_effect::none && s.checking, "Check Button Input opens");
	step(s, 0);
	check(step(s, cross | circle | start) == setup_effect::none && s.checking && s.open, "every button is only shown while checking");
	step(s, 0);
	check(step(s, select) == setup_effect::none && !s.checking, "Select quits Check Button Input");
	step(s, 0);
	check(attacks_of(default_layout(), square | cross) == 0b0101, "Square and Cross are Left Punch and Left Kick");
	check(attacks_of(start_layout, r1) == 0b0011, "R1 does 1+2 where it is given it");
	check(step(s, circle) == setup_effect::closed && !s.open, "Circle leaves from the rows below the actions");
	open_setup(s, "Stick A", start_layout, 0);
	check(step(s, start) == setup_effect::closed, "START leaves from anywhere");
}
PBD_TEST(PbdVersus, NextGameMenu)
{
	menu m{true, 0, {cross, 0}};
	check(!step(m, {cross, 0}) && m.open, "a Cross held when the menu opens does not choose");
	step(m, {0, 0});
	step(m, {0, down});
	step(m, {0, 0});
	check(m.focus == 0, "the winner cannot move the loser's menu");
	check(!step(m, {0, cross}) && m.open, "the winner cannot choose");
	step(m, {down, 0});
	step(m, {0, 0});
	check(step(m, {cross, 0}) == next::rematch_random_stage && !m.open, "the loser moves and chooses");
	menu last{true, next_count - 1, {}, 1};
	check(step(last, {0, cross}) == next::exit_to_launcher, "Exit to Launcher is the last choice");
	menu wrap{true, 0, {}};
	step(wrap, {up, 0});
	check(wrap.focus == next_count - 1, "Up from Rematch wraps to Exit to Launcher");
}
PBD_TEST(PbdVersus, Stages)
{
	for (u32 random = 0; random < 100; ++random)
	{
		check(pbd::stage_rule::loadable(draw_stage(random)));
		check(draw_stage(random, 0x12) != 0x12);
	}
}
PBD_TEST(PbdVersus, Director)
{
	using pbd::versus::select;
	director d;
	auto f = select_frame();
	auto out = d.step(f);
	check(!out.words[0] && !out.words[1] && out.hidden[0] == (triangle | r1 | select), "the select plays its own players, less Triangle, R1 and Select");
	check(d.snapshot(f).select && !d.snapshot(f).menu, "the select's prompts show");
	f.words[0] = r1;
	check(d.step(f).added[0] == triangle, "R1 picks the costume Triangle did");
	f.words[0] = 0;
	d.step(f);

	// Triangle opens that player's Controller Setup; the panel has the controller until it is closed and let go of.
	f.words[1] = triangle;
	out = d.step(f);
	check(out.words[1] == u16{0} && d.snapshot(f).setup[1].open && d.snapshot(f).setup[1].name == "Pad B", "Triangle opens Controller Setup");
	check(!d.snapshot(f).setup[0].open && !out.words[0], "the other player plays on");
	f.words[1] = cross;
	check(d.step(f).changed[1].has_value(), "a change in the panel reaches the controller");
	f.words[1] = start;
	d.step(f);
	check(!d.snapshot(f).setup[1].open, "START closes the panel");
	f.words[1] = start;
	check(d.step(f).words[1] == u16{0}, "the closing button stays out of the game");
	f.words[1] = 0;
	check(!d.step(f).words[1], "let go, the player has the select back");

	// Locked player choices request one direct load without handicap or stage presses.
	f.players[0].state = handicap;
	f.players[0].committed_row = 12;
	check(!d.step(f).direct_start);
	f.players[1].state = handicap;
	f.players[1].committed_row = 31;
	out = d.step(f);
	check(out.direct_start && out.direct_start->rows == std::array<u32, 2>{12, 31});
	check(pbd::stage_rule::loadable(out.direct_start->stage));
	check(out.words == std::array<std::optional<u16>, 2>{u16{0}, u16{0}});
	check(!d.step(f).direct_start);
	f.scene = load_scene;
	d.step(f);

	// A game, then the select it returns to: the menu, and Rematch plays the same fighters and stage again.
	f = select_frame();
	f.scene = game_scene;
	f.players[0].applied_verified = true;
	f.players[0].applied_left = 12;
	f.players[0].applied_right = 31;
	f.players[0].applied_stage = 0x12;
	d.step(f);
	f = select_frame();
	f.wins = {1, 0};
	out = d.step(f);
	check(d.snapshot(f).menu && out.words[0] == u16{0} && out.words[1] == u16{0}, "after a game the menu holds both controllers");
	f.words[1] = cross;
	d.step(f);
	check(d.snapshot(f).menu, "a press as the menu appears does not choose");
	f.words[1] = 0;
	run(d, f, menu_settle);
	f.words[1] = cross;
	d.step(f);
	check(!d.snapshot(f).menu);
	f.words[1] = 0;
	out = d.step(f);
	check(out.direct_start == std::optional<pbd::rematch::selection>{{{12, 31}, 0x12}});
	check(out.words[0] == u16{0} && out.words[1] == u16{0});
	check(!d.step(f).direct_start);
	f.scene = load_scene;
	out = d.step(f);
	check(out.words[0] == u16{0});

	f.scene = game_scene;
	out = d.step(f);
	check(!out.words[0], "the game is the players' own");
	check(out.hidden[0] == select && out.hidden[1] == select, "START skips the intros, replays and win poses");
	f.phase = fight_phase;
	out = d.step(f);
	check(out.hidden[0] == (start | select) && out.hidden[1] == (start | select), "a fight cannot be paused or quit by the game");
	f.words[1] = start | select;
	unsigned leaves = 0;
	for (unsigned i = 0; i < leave_frames + 10; ++i)
		leaves += d.step(f).leave ? 1u : 0u;
	check(leaves == 1, "START + SELECT held leaves once");
	f.words[1] = 0;
	check(!d.step(f).leave && d.snapshot(f).banner.empty(), "let go, nothing more");

	// Leaving the VS mode closes everything.
	director fresh;
	f = select_frame();
	f.words[0] = triangle;
	fresh.step(f);
	f.versus = false;
	f.words[0] = 0;
	fresh.step(f);
	f.versus = true;
	check(!fresh.snapshot(f).setup[0].open, "a panel does not outlive the VS mode");
	director first;
	f = select_frame();
	f.words[0] = cross;
	first.step(f);
	check(first.snapshot(f).select, "the first select has no menu");
}

PBD_TEST(PbdVersus, RematchShortcutsRequestDirectLoads)
{
	for (const auto choice : {next::rematch_random_stage, next::select_stage, next::select_character, next::exit_to_launcher})
	{
		director d;
		auto f = select_frame();
		f.scene = game_scene;
		f.players[0].applied_verified = true;
		f.players[0].applied_left = 12;
		f.players[0].applied_right = 31;
		f.players[0].applied_stage = 0x12;
		d.step(f);
		f = select_frame();
		run(d, f, menu_settle + 1);
		for (u32 row = 0; row < static_cast<u32>(choice); ++row)
		{
			f.words = {down, 0};
			d.step(f);
			f.words = {};
			d.step(f);
		}
		f.words = {cross, 0};
		const auto chosen = d.step(f);
		check(!chosen.direct_start);
		if (choice == next::exit_to_launcher)
		{
			check(chosen.leave && chosen.words[0] == u16{0} && chosen.words[1] == u16{0});
			f.words = {};
			check(!d.step(f).direct_start);
			continue;
		}
		f.words = {};
		auto out = d.step(f);
		if (choice == next::select_character)
		{
			check(d.snapshot(f).select && !out.direct_start && !out.words[0]);
			continue;
		}
		if (choice == next::select_stage)
		{
			check(!out.direct_start && out.words[0] == u16{0} && out.words[1] == u16{0});
			f.words = {right, 0};
			check(!d.step(f).direct_start);
			f.words = {};
			d.step(f);
			f.words = {cross, 0};
			check(!d.step(f).direct_start);
			f.words = {};
			out = d.step(f);
			check(out.direct_start && out.direct_start->stage == 0x13);
		}
		else
			check(out.direct_start && out.direct_start->stage != 0x12 && pbd::stage_rule::loadable(out.direct_start->stage));
		check(out.direct_start->rows == std::array<u32, 2>{12, 31});
		check(!d.step(f).direct_start);
		f.versus = false;
		check(!d.step(f).direct_start);
		f = select_frame();
		check(d.snapshot(f).banner.empty() && d.snapshot(f).select);
	}
}

PBD_TEST(PbdVersus, FinishedGameGivesOnlyTheLoserTheMenu)
{
	for (unsigned loser = 0; loser < sides; ++loser)
	{
		director d;
		auto f = select_frame();
		f.scene = game_scene;
		d.step(f);
		f = select_frame();
		f.wins[1 - loser] = 1;
		run(d, f, menu_settle + 1);
		check(d.snapshot(f).menu_player == loser);
		f.wins = {}; // The native select clears streaks after the first read.
		f.words[1 - loser] = up | cross;
		d.step(f);
		check(d.snapshot(f).menu && d.snapshot(f).menu_focus == 0);
		f.words = {};
		d.step(f);
		f.words[loser] = up;
		d.step(f);
		check(d.snapshot(f).menu_focus == next_count - 1);
		f.words = {};
		d.step(f);
		f.words[loser] = cross;
		check(d.step(f).leave);
	}
}

PBD_TEST(PbdVersus, NewPlayersReturnsToSelectAndReleasesBothSeats)
{
	director d;
	auto f = select_frame();
	f.scene = game_scene;
	d.step(f);
	f = select_frame();
	f.wins = {0, 1};
	run(d, f, menu_settle + 1);
	for (u32 i = 0; i < static_cast<u32>(next::new_players); ++i)
	{
		f.words = {down, 0};
		d.step(f);
		f.words = {};
		d.step(f);
	}
	f.words = {cross, 0};
	const auto out = d.step(f);
	check(out.new_players && !out.leave && !out.direct_start);
	check(out.words[0] == u16{0} && out.words[1] == u16{0});
	f.words = {};
	f.devices = {};
	check(!d.step(f).new_players);
	const auto shown = d.snapshot(f);
	check(shown.select && !shown.menu && shown.empty[0] && shown.empty[1]);
}

PBD_TEST(PbdVersus, HomeActsAsStart)
{
	using namespace pbd::controllers;
	state pad;
	pad.buttons = bit(home) | bit(pbd::controllers::square);
	const auto word = canonical(pbd::versus::input::home_as_start(pad));
	check(word == (pbd::versus::start | pbd::versus::square));
	check(canonical(pbd::versus::input::home_as_start({})) == 0);
}
