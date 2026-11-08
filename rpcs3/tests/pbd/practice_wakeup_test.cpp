#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_wakeup.h"
#include "Emu/PBD/pbd_practice_wakeup_reading.h"
#include "Emu/PBD/pbd_movement_observation.h"

#include <set>
#include <map>

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

namespace
{
	// Synthetic loaded banks let the production reader see both fighters, shared reactions and every floor alias.
	struct wakeup_memory
	{
		static constexpr std::array<std::uint32_t, 2> actors{0x10090040, 0x10090910};
		static constexpr std::array<std::uint32_t, 2> banks{0x20000000, 0x20001000}, tables{0x21000000, 0x21010000};
		std::map<std::uint32_t, std::uint32_t> words;
		wakeup_memory()
		{
			words[0x1008f05c] = 100;
			for (unsigned side = 0; side < 2; ++side)
			{
				const auto actor = actors[side], bank = banks[side];
				words[actor + 0x5c] = actors[1 - side];
				words[actor + 0x50] = bank;
				words[bank + 0x1e0] = tables[side];
				words[bank + 0x1e4] = 64;
				const auto commands = 0x22000000 + side * 0x10000;
				words[bank + 0x1b0] = commands;
				words[bank + 0x1b4] = 2;
				words[commands] = 0x00002004;
				words[commands + 16] = 1;
				words[commands + 18] = 255;
				words[commands + 22] = 0x50;
				words[commands + 24] = 0x80000000;
				for (unsigned id = 1; id <= 0x17; ++id)
					words[bank + 0xac + id * 4] = id;
				pose(side, 4);
			}
		}
		void pose(unsigned side, unsigned id, unsigned owner = 2)
		{
			const auto actor = actors[side], move = tables[owner < 2 ? owner : side] + id * 0x4c;
			words[actor + 0xc4] = move;
			words[actor + 0xcc] = 0x6084;
			words[actor + 0x48] = words[actor + 0x320] = 0;
			words[actor + 0x96] = words[actor + 0x146] = 1;
			words[move + 0x24] = 60;
			words[move + 0x14] = 0x22000000 + (owner < 2 ? owner : side) * 0x10000;
		}
		std::optional<pbd_wakeup_observation> read(unsigned side)
		{
			return pbd_read_wakeup(side, [&](auto at)
				{
					return words.contains(at) ? words.at(at) : ~0u;
				},
				[&](auto at)
				{
					return words.contains(at) ? words.at(at) : ~0u;
				});
		}
	};
} // namespace

PBD_TEST(PbdPracticeWakeup, EveryFloorAliasAndSharedReactionReceivesEveryWakeupChoice)
{
	for (unsigned side = 0; side < 2; ++side)
		for (const unsigned id : {4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 0x17u, 40u})
			for (unsigned owner = 0; owner < 2; ++owner)
				for (unsigned selected = 0; selected < pbd_wakeup_option_count; ++selected)
				{
					wakeup_memory memory;
					memory.pose(side, id, owner);
					const auto now = memory.read(side);
					check(now && now->on_floor && now->actionable, "the live floor flag admits own-bank and attacker-bank floor poses");
					pbd_wakeup_controller control;
					control.arm(static_cast<option>(selected));
					const auto plan = pbd_plan_wakeup(static_cast<option>(selected), now->position, false);
					check(control.sample(*now) == (plan.count ? plan.words[0] : 0), "each choice reaches its floor input or its documented fallback");
				}
}

PBD_TEST(PbdPracticeWakeup, FloorClockDirectionAndInvalidMemoryDoNotInventAGetup)
{
	wakeup_memory memory;
	for (unsigned side = 0; side < 2; ++side)
	{
		const auto actor = memory.actors[side];
		for (const unsigned direction : {1u, 2u, 0u, 0xffffu})
		{
			memory.words[actor + 0x146] = direction;
			const auto now = memory.read(side);
			check(now && now->on_floor && now->actionable == (direction == 1 || direction == 2),
				"positive cursor steps accept floor input; a frozen or reversed cursor waits");
		}
		memory.words[actor + 0xcc] = 0x6000;
		check(memory.read(side)->knocked_down && !memory.read(side)->on_floor, "a falling launcher reaction keeps the landing-tech route");
		memory.words[actor + 0xcc] = 0x1052;
		check(!memory.read(side)->on_floor, "a rising move is not another floor pose");
		memory.words[actor + 0xc4]++;
		check(!memory.read(side), "a misaligned move pointer cannot drive either fighter");
	}
	check(!memory.read(2));
}

PBD_TEST(PbdPracticeWakeup, RepeatedFaceUpLandingsKeepToeAndSpringKicks)
{
	for (const auto selected : {option::toe_kick, option::spring_kick})
	{
		wakeup_memory memory;
		pbd_wakeup_controller control;
		for (const unsigned id : {4u, 6u, 7u, 4u, 6u, 7u})
		{
			memory.pose(1, id);
			memory.words[0x1008f05c]++;
			const auto now = memory.read(1);
			check(now && now->on_floor && now->position == position::face_up_feet_toward,
				"each phase of a face-up landing admits its available ground attacks");
			if (!control.armed)
				control.arm(selected);
			const auto plan = pbd_plan_wakeup(selected, position::face_up_feet_toward, false);
			check(control.sample(*now) == plan.words[0] && control.floor.plan.option == selected,
				"a later face-up knockdown cannot replace the selected attack with Stand Up");
			memory.pose(1, 1);
			memory.words[memory.actors[1] + 0xcc] = 0x1000;
			memory.words[0x1008f05c]++;
			check(!control.sample(*memory.read(1)) && !control.armed, "standing releases the old opening before another knockdown");
		}
	}
}

PBD_TEST(PbdPracticeWakeup, ChangingFloorMovesDoesNotAcknowledgeAnIgnoredOpening)
{
	pbd_wakeup_controller control;
	control.arm(option::stand_up);
	pbd_wakeup_observation now{34, 100, true, true, false, position::face_up_feet_toward};
	check(control.sample(now) == 0x1000);
	for (unsigned frame = 1; frame <= 7; ++frame)
	{
		now.counter++;
		now.allocation++;
		check(control.sample(now) == 0, "floor-to-floor transitions do not repeat an accepted action");
	}
	now.counter++;
	check(control.sample(now) == 0x1000, "an ignored get-up retries without needing another hit");
	now.counter++;
	now.on_floor = false;
	check(control.sample(now) == 0);
	for (unsigned frame = 0; frame < 30; ++frame)
	{
		now.counter++;
		now.on_floor = true;
		check(control.sample(now) == 0, "a get-up that left the floor is not restarted on a later floor pose");
	}
}

PBD_TEST(PbdPracticeWakeup, LockedLandingPreservesEveryOpeningUntilTheGetupWindow)
{
	for (unsigned selected = 0; selected < pbd_wakeup_option_count; ++selected)
	{
		wakeup_memory memory;
		const auto actor = memory.actors[1], commands = 0x22010000u;
		const auto landing = memory.words[actor + 0xc4], floor = memory.tables[1] + 7 * 0x4c;
		memory.words[landing + 0x14] = commands + 24;
		memory.words[commands + 24 + 8] = 0x8007;
		memory.words[commands + 24 + 16] = 60;
		memory.words[commands + 24 + 18] = 255;
		memory.words[floor + 0x14] = commands;
		memory.words[floor + 0x24] = 60;
		pbd_wakeup_controller control;
		control.arm(static_cast<option>(selected));
		for (unsigned cursor = 1; cursor < 60; ++cursor)
		{
			memory.words[actor + 0x96] = cursor;
			memory.words[0x1008f05c]++;
			const auto now = memory.read(1);
			check(now && now->on_floor && !now->actionable, "a grounded landing without an input window cannot consume a wake-up");
			check(control.sample(*now) == 0 && control.floor.given == 0);
			check(control.native_action() == pbd_wakeup_native_action(static_cast<option>(selected)),
				"a landing tech stays offered while the fighter lands, before the floor's input window");
		}
		memory.words[actor + 0x96] = 60;
		memory.words[0x1008f05c]++;
		const auto now = memory.read(1);
		check(now && now->actionable, "the end-command window admits the floor target's inputs on the same frame");
		const auto plan = pbd_plan_wakeup(static_cast<option>(selected), now->position, false);
		check(control.sample(*now) == (plan.count ? plan.words[0] : 0), "the requested opening survives the normal floor time");
		check(!control.native_action(), "the floor's input window withdraws a tech that was not taken");
		memory.words[commands + 24 + 8] = 0x8030;
		check(!memory.read(1)->actionable, "an invalid terminal target is never followed");
	}
}

PBD_TEST(PbdPracticeWakeup, PaulBackThreeKnockdownWaitsForTheFloorCommandWindow)
{
	// Paul's Pl_Swlk reaction sDm_ASI10LD ends at alias 4; dwDWN_00U then opens alias 6 at cursor 60.
	// The phase lengths before that window are modelled; the command window and floor aliases come from the move data.
	for (unsigned selected = 0; selected < pbd_wakeup_option_count; ++selected)
	{
		wakeup_memory memory;
		const auto actor = memory.actors[1], bank = memory.banks[1], table = memory.tables[1], commands = 0x22010000u;
		memory.words[bank + 0x1e4] = 700;
		memory.words[bank + 0xac + 4 * 4] = 35;
		memory.words[bank + 0xac + 6 * 4] = 40;
		memory.words[bank + 0xac + 7 * 4] = 39;
		pbd_wakeup_controller control;
		control.arm(static_cast<option>(selected));
		memory.pose(1, 667);
		memory.words[actor + 0xcc] = 0x7402;
		check(!control.sample(*memory.read(1)) && control.engaged && !control.active, "the falling reaction retains landing techs");
		check(control.native_action() == pbd_wakeup_native_action(static_cast<option>(selected)), "landing-tech choices retain their native action before the floor");
		memory.pose(1, 35);
		memory.words[table + 35 * 0x4c + 0x14] = commands + 24;
		memory.words[commands + 24 + 8] = 0x8006;
		memory.words[commands + 24 + 16] = 60;
		memory.words[commands + 24 + 18] = 255;
		memory.words[table + 40 * 0x4c + 0x14] = commands;
		memory.words[table + 40 * 0x4c + 0x24] = 60;
		for (unsigned cursor = 1; cursor < 60; ++cursor)
		{
			memory.words[actor + 0x96] = cursor;
			memory.words[0x1008f05c]++;
			check(control.sample(*memory.read(1)) == 0 && control.floor.given == 0, "the normal floor time cannot spend the requested action");
		}
		memory.words[actor + 0x96] = 60;
		memory.words[0x1008f05c]++;
		const auto expected = control.floor.plan.count ? control.floor.plan.words[0] : 0;
		check(control.sample(*memory.read(1)) == expected, "the selected floor action runs when the game opens get-up input");
	}
}

PBD_TEST(PbdPracticeWakeup, GroundedSideRollAndFaceDownGetupAcknowledgeTheirInputTarget)
{
	for (const auto selected : {option::side_roll_up, option::side_roll_down, option::rising_low_kick, option::stand_up})
	{
		wakeup_memory memory;
		const auto commands = 0x22010000u;
		memory.pose(1, 7);
		memory.words[commands + 8] = 43;
		pbd_wakeup_controller control;
		control.arm(selected);
		const auto opening = control.sample(*memory.read(1));
		check(opening == control.floor.plan.words[0]);
		memory.pose(1, 43);
		memory.words[0x1008f05c]++;
		const auto rolling = memory.read(1);
		check(rolling && rolling->on_floor && rolling->recovering, "a grounded input target is an accepted roll or get-up");
		check(control.sample(*rolling) == 0 && control.floor.responded);
		memory.pose(1, 7);
		for (unsigned frame = 0; frame < 120; ++frame)
		{
			memory.words[0x1008f05c]++;
			check(control.sample(*memory.read(1)) == 0, "a side roll ending on the floor is not replaced by Stand Up");
		}
	}
}

PBD_TEST(PbdPracticeWakeup, LandingPoseChoosesAvailableGroundAttacksWhenTheWindowOpens)
{
	for (const auto selected : {option::toe_kick, option::spring_kick})
	{
		wakeup_memory memory;
		const auto actor = memory.actors[1], table = memory.tables[1], commands = 0x22010000u;
		memory.pose(1, 40);
		memory.words[table + 40 * 0x4c + 0x14] = commands + 24;
		memory.words[commands + 24 + 8] = 0x8007;
		memory.words[commands + 24 + 16] = 60;
		memory.words[commands + 24 + 18] = 255;
		memory.words[table + 7 * 0x4c + 0x14] = commands;
		memory.words[table + 7 * 0x4c + 0x24] = 60;
		pbd_wakeup_controller control;
		control.arm(selected);
		check(control.sample(*memory.read(1)) == 0 && control.floor.plan.option == option::stand_up, "an unidentified locked landing has not spent its opening");
		memory.words[actor + 0x96] = 60;
		memory.words[0x1008f05c]++;
		const auto now = memory.read(1);
		check(now && now->actionable && now->position == position::face_up_feet_toward, "the terminal's floor alias identifies the available get-up before the hand-over");
		const auto opening = control.sample(*now);
		check(control.floor.plan.option == selected && opening == (selected == option::toe_kick ? 0x4040 : 0x8000),
			"toe and spring kicks are chosen from the pose that admits input, including after a locked throw landing");
	}
}
