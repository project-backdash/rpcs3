#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_wakeup.h"
#include "Emu/PBD/pbd_movement_observation.h"

#include <set>

namespace
{
	using namespace pbd::practice;
	using pbd::test::check;
	using option = pbd_wakeup_option;
	using position = pbd_ground_position;
} // namespace

PBD_TEST(PbdPracticeWakeup, StockTechValuesAndUntechableFallback)
{
	const std::array choices{option::quick_roll_down, option::quick_roll_up, option::spring_forward, option::quick_roll_back};
	for (std::uint32_t at = 0; at < choices.size(); ++at)
	{
		const auto tech = pbd_plan_wakeup(choices[at], position::face_down_feet_away, true);
		check(tech.native_action == 6 + at, "landing techs retain the game's established values 6-9");
		const auto fallback = pbd_plan_wakeup(choices[at], position::face_down_feet_away, false);
		check(fallback.option == option::stand_up && !fallback.native_action && fallback.words[0] == 0x1000,
			"a landing that cannot tech uses Stand Up from the floor");
	}
}

PBD_TEST(PbdPracticeWakeup, UnavailableFaceUpChoicesStandUp)
{
	for (const auto pose : {position::unknown, position::face_up_feet_away, position::face_down_feet_toward, position::face_down_feet_away})
		for (const auto requested : {option::toe_kick, option::spring_kick})
		{
			const auto fallback = pbd_plan_wakeup(requested, pose, true);
			check(fallback.option == option::stand_up && fallback.words[0] == 0x1000,
				"an unavailable ground attack stands up instead of substituting a rising kick");
		}
	check(pbd_resolve_wakeup(option::toe_kick, position::face_up_feet_toward) == option::toe_kick);
	check(pbd_resolve_wakeup(option::spring_kick, position::face_up_feet_toward) == option::spring_kick);
}

PBD_TEST(PbdPracticeWakeup, RandomIncludesStayDownAndOnlyAvailableChoices)
{
	for (const auto pose : {position::face_up_feet_toward, position::face_down_feet_away})
	{
		std::set<option> observed;
		for (std::uint32_t word = 0; word < 28; ++word)
		{
			const auto chosen = pbd_resolve_wakeup(option::random, pose, word);
			check(pbd_wakeup_available(chosen, pose), "Random never picks an unavailable floor attack");
			check(pbd_resolve_wakeup(option::random, pose, word) == chosen, "one knockdown's random word keeps its choice");
			observed.insert(chosen);
		}
		check(observed.contains(option::stay_down), "Stay Down remains a real Random option");
		check(observed.size() == (pose == position::face_up_feet_toward ? 14 : 12));
		for (std::uint32_t word = 0; word < 20; ++word)
		{
			const auto floor = pbd_plan_wakeup(option::random, pose, false, word);
			check(!floor.native_action && floor.option < option::quick_roll_up,
				"Random excludes techs when the knockdown cannot be teched");
		}
	}
}

PBD_TEST(PbdPracticeWakeup, FloorInputUsesTheFirstFreeFrameOnEitherSide)
{
	for (std::uint32_t at = 0; at < static_cast<std::uint32_t>(option::quick_roll_up); ++at)
	{
		const auto selected = static_cast<option>(at);
		if (selected == option::stay_down)
			continue;
		pbd_floor_wakeup sequence;
		sequence.reset(pbd_plan_wakeup(selected, position::face_up_feet_toward, false));
		check(!sequence.next(false, true) && !sequence.next(true, false), "standing and locked floor frames inject nothing");
		const auto first = sequence.next(true, true);
		check(first == sequence.plan.words[0], "the first free floor frame receives the option's opening input");
		for (const auto side : {pbd::practice::movement::screen_side::left, pbd::practice::movement::screen_side::right})
			check(pbd::practice::movement::record_word(pbd::practice::movement::play_word(*first, side), side) == *first,
				"the same floor option preserves its opponent-relative input on both sides");
		if (selected == option::spring_kick)
			check(sequence.next(false, false) == 0x0060, "3+4 follows the back-roll opening immediately");
		check(!sequence.next(true, true), "a completed option is not repeated while its animation runs");
	}
}

PBD_TEST(PbdPracticeWakeup, StayDownAndResetDoNotLeaveAQueuedAttack)
{
	pbd_floor_wakeup sequence;
	sequence.reset(pbd_plan_wakeup(option::spring_kick, position::face_up_feet_toward, false));
	check(sequence.next(true, true) == 0x8000);
	sequence.reset(pbd_plan_wakeup(option::stay_down, position::face_down_feet_away, false));
	for (unsigned frame = 0; frame < 120; ++frame)
		check(sequence.next(true, true) == 0, "Stay Down holds neutral instead of continuing the old spring kick");
	check(!sequence.next(false, true), "the floor sequence returns control after the fighter leaves the floor");
	sequence.reset();
	check(!sequence.next(false, true));
	check(sequence.next(true, true) == 0x1000, "Restart clears the previous input and restores Stand Up");
}

PBD_TEST(PbdPracticeWakeup, AnUnavailableCharacterAttackFallsBackToStandUp)
{
	pbd_floor_wakeup sequence;
	sequence.reset(pbd_plan_wakeup(option::toe_kick, position::face_up_feet_toward, false));
	check(sequence.next(true, true, false) == 0x4040);
	for (unsigned frame = 0; frame < 7; ++frame)
		check(!sequence.next(true, true, false));
	check(sequence.next(true, true, false) == 0x1000, "an unaccepted shared floor attack falls back to Stand Up");
	sequence.reset(pbd_plan_wakeup(option::side_roll_up, position::face_up_feet_toward, false));
	check(sequence.next(true, true, false) == 0x80);
	check(!sequence.next(true, true, true));
	for (unsigned frame = 0; frame < 120; ++frame)
		check(!sequence.next(true, true, false), "a completed side roll stays down instead of triggering fallback");
}

PBD_TEST(PbdPracticeWakeup, FloorOwnershipEndsAtStandingAndResetsAcrossClockGaps)
{
	pbd_wakeup_controller control;
	control.arm(option::stay_down);
	pbd_wakeup_observation now{34, 100, true, true, false, position::face_up_feet_toward};
	check(control.sample(now) == 0 && control.active, "Stay Down owns neutral input on the floor");
	check(control.sample(now) == 0 && control.floor.given == 0, "a repeated counter consumes no extra input");
	now.counter++;
	check(control.sample(now) == 0);
	now.standing = true;
	now.on_floor = false;
	check(!control.sample(now) && !control.active && !control.armed, "standing restores the neutral dummy action");
	control.arm(option::spring_kick);
	now = {34, 200, true, true, false, position::face_up_feet_toward};
	check(control.sample(now) == 0x8000);
	now.counter = 202;
	check(!control.sample(now) && !control.active, "a rewind or missed frame cannot leave a spring follow-up queued");
	control.arm(option::quick_roll_up);
	now = {159, 299, false, false, false, position::unknown, true};
	check(!control.sample(now) && control.native_action() == 7u, "stock tech input owns an airborne knockdown");
	now = {34, 300, true, true, false, position::face_up_feet_toward, true};
	now.counter = 300;
	check(control.sample(now) == 0x1000 && !control.native_action(), "an unteched landing switches from stock tech to Stand Up");
	control.arm(option::quick_roll_up);
	now = {799, 400, false, false, false, position::unknown, false};
	check(!control.sample(now) && !control.engaged && !control.native_action(), "standing hit stun keeps its ordinary native route and frame observations");
	control.arm(option::random, 8);
	now = {46, 500, true, true, false, position::face_down_feet_toward, true};
	check(control.sample(now) == 0x1000 && control.floor.plan.option == option::stand_up,
		"Random reselects an available option when the landing excludes its first choice, using the same random word");
}

PBD_TEST(PbdPracticeWakeup, OnlyStandingCrouchingAndTechsNeverGuard)
{
	for (const std::uint32_t action : {0u, 2u, 6u, 7u, 8u, 9u})
		check(!pbd_dummy_action_guards(action), "Standing, Crouching and the landing techs never guard");
	for (const std::uint32_t action : {1u, 3u, 4u, 5u, 10u, 11u, 0xffffffffu})
		check(pbd_dummy_action_guards(action), "the guard settings, Controller and unknown values keep the game's guard");
}

PBD_TEST(PbdPracticeWakeup, GetUpFlagsLoseOnlyTheirGuardBits)
{
	check(pbd_without_guard(0x1052) == 0x1042, "the shared stand-up's standing guard loses 0x10 and nothing else");
	check(pbd_without_guard(0x1952) == 0x1942, "a standing stance entered with its guard on loses it, as the game drops it");
	check(pbd_without_guard(0x3029) == 0x3021, "the rise's low guard goes too");
	check(pbd_without_guard(0x6084) == 0x6084, "lying flags have no guard to remove");
}

PBD_TEST(PbdPracticeWakeup, GetUpGuardLastsFromTheKnockdownUntilTheDummyStands)
{
	pbd_getup_guard guard;
	constexpr std::uint64_t boot = 7;
	check(!guard.boundary(false, true, 0, boot, 100) && !guard.entered(boot, 100), "a standing dummy keeps the game's guard rules");
	check(guard.boundary(true, false, 0, boot, 101), "a knockdown starts the get-up");
	check(guard.entered(boot, 101) && guard.entered(boot, 102) && !guard.entered(boot, 103),
		"moves entered in the frame after the boundary lose their guard; a later frame waits for its own boundary");
	check(!guard.entered(boot + 1, 102), "another boot never inherits a get-up");
	check(guard.boundary(false, false, 0, boot, 140) && guard.entered(boot, 140), "rising off the floor is still the get-up");
	check(guard.boundary(false, true, 0, boot, 160) && !guard.entered(boot, 160),
		"the boundary that sees the dummy standing clears the stance its last frame entered, and the get-up is over");
	check(!guard.boundary(false, true, 0, boot, 161), "from then on the stance's own guard rules apply");
}

PBD_TEST(PbdPracticeWakeup, GuardSettingsKeepTheStockGetUp)
{
	for (const std::uint32_t action : {1u, 3u, 4u, 5u, 10u, 0xffffffffu})
	{
		pbd_getup_guard guard;
		check(!guard.boundary(true, false, action, 1, 10) && !guard.entered(1, 10), "a guard setting keeps the stock get-up guard");
		check(!guard.boundary(false, false, action, 1, 20) && !guard.boundary(false, true, action, 1, 30));
	}
	pbd_getup_guard changed;
	check(changed.boundary(true, false, 0, 1, 10), "Standing removes the get-up guard");
	check(!changed.boundary(false, false, 1, 1, 11) && !changed.entered(1, 11),
		"Standing Guard chosen during a get-up gives the rest of it the stock guard at once");
}
