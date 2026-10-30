#include "pbd_test.h"

#include "Emu/PBD/pbd_rematch.h"
#include "Emu/PBD/pbd_rematch_native.h"
#include "Emu/PBD/pbd_character_rule_native.h"

using pbd::test::check;

using namespace pbd::rematch;

PBD_TEST(PbdRematch, NextSetLoad)
{
	sealed_state previous{1, 5700, 0, {{12, 32}, 17}, {3, 2, {100, 100}}};
	for (u32 game = 0; game < 32; ++game)
	{
		load next{1, 1, previous.end_frame, previous.epoch + 1, {{12, 32}, game % 2 ? 17u : 20u}, previous.rules, true};
		check(validate(next, previous, false, true) == error::none);
		check(validate(next, previous, false) == error::stale_game);
		native_load loader;
		check(loader.arm(next, previous, false, 1, game + 1, true) && loader.straight_to_vs());
		auto stale = next;
		++stale.epoch;
		check(validate(stale, previous, false, true) == error::stale_epoch);
		stale = next;
		++stale.end_frame;
		check(validate(stale, previous, false, true) == error::changed_end);
		previous = {1, previous.end_frame + 100, next.epoch, next.selected, next.rules};
	}
}

PBD_TEST(PbdRematch, InitialFixedPicks)
{
	const selection selected{{12, 32}, 17};
	const options rules{3, 2, {100, 100}};
	constexpr u64 generation = 0x71a11;
	pbd::character_rule::configure(generation, true);
	for (unsigned variant = 0; variant < 10; ++variant)
	{
		native_load start;
		auto picks = selected;
		auto settings = rules;
		if (variant == 0)
			picks.rows[0] = 85;
		if (variant == 1)
			picks.rows[1] = 85;
		if (variant == 2)
			picks.stage = 35;
		if (variant == 3)
			settings.rounds = 2;
		if (variant == 4)
			settings.time_index = 5;
		if (variant == 5)
			settings.handicap[1] = 90;
		if (variant == 8)
			picks.rows[0] = 81;
		if (variant == 9)
			picks.rows[1] = 84;
		check(!start.arm_start(picks, settings, variant == 6 ? 0 : generation, variant == 7 ? 1 : 0));
		check(!start.active());
	}
	native_load start;
	check(start.arm_start(selected, rules, generation, 0) && start.straight_to_vs());
	check(!start.arm_start(selected, rules, generation, 0));
	for (const u32 stage : {0u, 13u, 15u, 34u, 37u})
	{
		native_load other;
		check(other.arm_start({{12, 32}, stage}, rules, generation, 0), "an original stage or Inferno starts a game");
	}
	for (const u32 stage : {14u, 16u, 35u, 36u, 38u})
	{
		native_load other;
		check(!other.arm_start({{12, 32}, stage}, rules, generation, 0), "an id with no stage behind it is refused");
	}
	pbd::character_rule::configure(0, false);
}

static void loser_repick_checks(const load& same, const sealed_state& previous, load& changed)
{
	// Change character: the loser's side picks again. The load still freezes both previous rows and the stage.
	for (u32 side = 0; side < 2; ++side)
	{
		auto repick = same;
		repick.repick = side;
		check(valid(repick) && validate(repick, previous, false) == error::none);
		// Only a decided game has a loser, and the stage stays, so neither a draw nor a random stage combines with it.
		auto replayed = repick;
		replayed.next_game = 1;
		check(validate(replayed, previous, true) == error::repick_after_draw);
		auto random = repick;
		random.random_stage = true;
		random.selected.stage = 31;
		check(!valid(random) && validate(random, previous, false) == error::invalid_load);
		// The frozen rows still describe the last game: a load that names another row for either side is stale.
		for (u32 port = 0; port < 2; ++port)
		{
			changed = repick;
			++changed.selected.rows[port];
			check(validate(changed, previous, false) == error::changed_fighters);
		}
		// What the next game applied: any row for the chooser, the frozen row for the other side and the frozen stage.
		const u32 kept = side ^ 1u;
		selection applied = repick.selected;
		applied.rows[side] = 77;
		check(matches(repick, applied) && !matches(same, applied));
		applied.rows[kept] = 77;
		check(!matches(repick, applied));
		applied = repick.selected;
		applied.stage = 31;
		check(!matches(repick, applied));
		check(matches(repick, repick.selected) && matches(same, same.selected));
	}
	auto outside = same;
	outside.repick = 2;
	check(!valid(outside) && validate(outside, previous, false) == error::invalid_load);
}

PBD_TEST(PbdRematch, Checks)
{
	for (bool ready : {false, true})
		for (bool held : {false, true})
		{
			const auto peer = gate_resource(true, false, true, ready, held);
			check(peer.announce == ready && peer.hold == (ready && held));
			const auto stale = gate_resource(true, true, false, ready, held);
			check(!stale.announce && !stale.hold);
			const auto stock = gate_resource(false, false, true, ready, held);
			check(!stock.announce && !stock.hold);
			check(gate_resource(false, true, true, ready, held).hold);
		}
	const sealed_state previous{1, 5700, 0, {{12, 32}, 17}, {3, 2, {100, 100}}};
	const load same{1, 2, 5700, 1, previous.selected, previous.rules, false};
	check(validate(same, previous, false) == error::none);
	auto changed = same;
	changed.selected.stage = 31;
	changed.random_stage = true;
	check(validate(changed, previous, false) == error::none);
	changed.random_stage = false;
	check(validate(changed, previous, false) == error::changed_stage);
	changed = same;
	changed.random_stage = true;
	check(validate(changed, previous, false) == error::changed_stage);
	for (unsigned port = 0; port < 2; ++port)
	{
		changed = same;
		++changed.selected.rows[port];
		check(validate(changed, previous, false) == error::changed_fighters);
		changed = same;
		--changed.rules.handicap[port];
		check(validate(changed, previous, false) == error::invalid_load);
	}
	changed = same;
	changed.rules.time_index = 3;
	check(validate(changed, previous, false) == error::changed_options);
	changed = same;
	++changed.end_frame;
	check(validate(changed, previous, false) == error::changed_end);
	changed = same;
	++changed.previous_game;
	check(validate(changed, previous, false) == error::stale_game);
	changed = same;
	++changed.next_game;
	check(validate(changed, previous, false) == error::stale_game);
	changed = same;
	changed.epoch = 2;
	check(validate(changed, previous, false) == error::stale_epoch);
	changed = same;
	changed.epoch = 0;
	check(validate(changed, previous, false) == error::invalid_load);
	changed = same;
	changed.selected.rows[0] = 85;
	check(validate(changed, previous, false) == error::invalid_load);
	changed = same;
	changed.selected.stage = 38;
	check(validate(changed, previous, false) == error::invalid_load);
	changed = same;
	changed.next_game = 1;
	check(validate(changed, previous, true) == error::none);
	changed.random_stage = true;
	changed.selected.stage = 31;
	check(validate(changed, previous, true) == error::changed_stage);
	auto drawn = previous;
	drawn.epoch = 1;
	changed = same;
	changed.next_game = 1;
	changed.epoch = 2;
	check(validate(changed, drawn, true) == error::none);
	changed.epoch = 1;
	check(validate(changed, drawn, true) == error::stale_epoch);
	auto exhausted = previous;
	exhausted.epoch = max_serial;
	check(validate(same, exhausted, false) == error::invalid_state);
	loser_repick_checks(same, previous, changed);
}
