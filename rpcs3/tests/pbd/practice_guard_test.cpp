#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_guard.h"
#include "Emu/PBD/pbd_dummy_move.h"

#include <vector>

namespace
{
	using namespace pbd::practice;
	using pbd::test::check;
	using guard = pbd_guard_option;

	// Hands out the given random words in turn and counts how many were drawn.
	struct words
	{
		std::vector<std::uint32_t> values;
		std::size_t taken = 0;
		std::uint32_t operator()()
		{
			return values[taken++ % values.size()];
		}
	};
	constexpr std::uint32_t on = 0x80000000u, off = 0x7fffffffu;
	const pbd_guard_frame quiet{false, false, true}, busy{false, false, false}, hit{true, false, false}, blocked{false, true, false};
} // namespace

PBD_TEST(PbdPracticeGuard, DummyActionListKeepsTheGameValuesAndAddsGuardAllChoices)
{
	constexpr std::array<std::uint32_t, 9> actions{0, 1, 2, 3, 4, 4, 4, 5, 10};
	for (std::uint32_t at = 0; at < pbd_dummy_action_choices.size(); ++at)
	{
		const auto& choice = pbd_dummy_action_choices[at];
		check(choice.action == actions[at], "each option commits the game's own action");
		check(pbd_dummy_action_option(choice.action, choice.guard) == at, "the action in force and the guard choice name the option again");
		check((choice.guard != guard::game) == !choice.help.empty(), "only the added choices explain themselves");
	}
	check(pbd_dummy_action_option(4, guard::game) == 4u && pbd_dummy_action_option(4, guard::after_first_hit) == 5u &&
		pbd_dummy_action_option(4, guard::random) == 6u);
	for (const auto choice : {guard::after_first_hit, guard::random})
		check(pbd_dummy_action_option(1, choice) == 1u && pbd_dummy_action_option(10, choice) == 8u,
			"a guard choice left from Guard All never renames another action");
	for (std::uint32_t action = 6; action <= 9; ++action)
		check(!pbd_dummy_action_option(action, guard::game), "the landing techs belong to Wake-up");
	check(!pbd_dummy_action_option(11, guard::game));
}

PBD_TEST(PbdPracticeGuard, OnlyGuardAllGivesWayToStanding)
{
	pbd_guard_state state;
	state.choose(guard::after_first_hit);
	check(state.lifted(), "After First Hit starts without a guard");
	check(pbd_guard_action(4, state) == 0, "Standing stands in for Guard All");
	for (const std::uint32_t action : {0u, 1u, 2u, 3u, 5u, 10u, 0xffffffffu})
		check(pbd_guard_action(action, state) == action, "any other action keeps its own guard rules");
	state.choose(guard::game);
	check(!state.lifted() && pbd_guard_action(4, state) == 4, "the game's Guard All guards as it always has");
}

PBD_TEST(PbdPracticeGuard, AfterFirstHitGuardsUntilTheDummyHasBeenNeutralForAMoment)
{
	pbd_guard_state state;
	state.choose(guard::after_first_hit);
	words draw{{on}};
	for (int frame = 0; frame < 100; ++frame)
		state.frame(quiet, draw);
	check(state.lifted(), "no hit, no guard, however long the dummy stands");
	state.frame(blocked, draw);
	check(state.lifted(), "only a hit brings the guard");
	state.frame(hit, draw);
	check(!state.lifted(), "the first hit brings the guard");
	for (int frame = 0; frame < 50; ++frame)
		state.frame(busy, draw);
	check(!state.lifted(), "a combo or a knockdown keeps it");
	for (std::uint32_t frame = 1; frame < pbd_guard_reset_frames; ++frame)
		state.frame(quiet, draw);
	state.frame(blocked, draw);
	check(!state.lifted(), "a block before the moment is up starts the moment again");
	for (std::uint32_t frame = 1; frame < pbd_guard_reset_frames; ++frame)
		state.frame(quiet, draw);
	state.frame(busy, draw);
	for (std::uint32_t frame = 1; frame < pbd_guard_reset_frames; ++frame)
		state.frame(quiet, draw);
	check(!state.lifted(), "leaving neutral starts the moment again");
	state.frame(quiet, draw);
	check(state.lifted(), "after the moment in neutral the dummy waits for a new first hit");
	check(draw.taken == 0, "After First Hit draws no random words");
}

PBD_TEST(PbdPracticeGuard, AfterFirstHitLiftsWithoutAReadingAndOnARestart)
{
	pbd_guard_state state;
	state.choose(guard::after_first_hit);
	words draw{{on}};
	state.frame(hit, draw);
	const pbd_guard_frame unread{false, false, std::nullopt};
	for (std::uint32_t frame = 1; frame < pbd_guard_unread_frames; ++frame)
		state.frame(unread, draw);
	check(!state.lifted());
	state.frame(unread, draw);
	check(state.lifted(), "without a neutral reading the guard still lifts in the end");
	state.frame(hit, draw);
	check(!state.lifted());
	state.restart();
	check(state.lifted(), "a Restart waits for a new first hit");
}

PBD_TEST(PbdPracticeGuard, RandomGuardDrawsOnTheFirstFrameAndAfterEachContact)
{
	pbd_guard_state state;
	state.choose(guard::random);
	check(!state.lifted(), "before its first draw Random keeps Guard All's guard");
	words draw{{off, on, on, off}};
	state.frame(quiet, draw);
	check(draw.taken == 1 && state.lifted(), "the first frame draws");
	for (int frame = 0; frame < 60; ++frame)
		state.frame(quiet, draw);
	state.frame(busy, draw);
	check(draw.taken == 1 && state.lifted(), "a choice holds until a hit or block");
	state.frame(hit, draw);
	check(draw.taken == 2 && !state.lifted(), "a hit draws again");
	state.frame(blocked, draw);
	check(draw.taken == 3 && !state.lifted(), "so does a block");
	state.frame(hit, draw);
	check(draw.taken == 4 && state.lifted());
	state.restart();
	check(!state.lifted(), "after a Restart the dummy guards until the next draw");
	state.frame(quiet, draw);
	check(draw.taken == 5);
}

PBD_TEST(PbdPracticeGuard, CounterHitRandomDrawsOnTheFirstFrameAndAfterEachContact)
{
	pbd_counter_hit_draw counter;
	words draw{{on, off}};
	counter.frame(hit, draw);
	check(!counter.value && draw.taken == 0, "Off and On draw nothing");
	counter.choose(true);
	check(!counter.value, "nothing is set before the first frame");
	counter.frame(quiet, draw);
	check(counter.value == true && draw.taken == 1);
	for (int frame = 0; frame < 30; ++frame)
		counter.frame(busy, draw);
	check(counter.value == true && draw.taken == 1, "the value holds until the next hit or block");
	counter.frame(blocked, draw);
	check(counter.value == false && draw.taken == 2);
	counter.restart();
	check(!counter.value);
	counter.frame(quiet, draw);
	check(draw.taken == 3);
	counter.choose(false);
	check(!counter.value && !counter.random);
}

PBD_TEST(PbdPracticeGuard, TheHostGeneratorGivesBothChoices)
{
	std::uint32_t rng = 0x9e3779b9u;
	const auto draw = [&] { return pbd::practice::dummy_move::random(rng); };
	pbd_guard_state state;
	state.choose(guard::random);
	pbd_counter_hit_draw counter;
	counter.choose(true);
	std::uint32_t guarded = 0, counters = 0;
	for (int contact = 0; contact < 1000; ++contact)
	{
		state.frame(hit, draw);
		counter.frame(hit, draw);
		guarded += state.lifted() ? 0 : 1;
		counters += counter.value == true ? 1 : 0;
	}
	check(guarded > 400 && guarded < 600 && counters > 400 && counters < 600, "about half of each, from the host's own generator");
}
