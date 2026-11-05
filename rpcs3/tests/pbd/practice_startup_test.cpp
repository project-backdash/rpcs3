#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_frame_calculator.h"
#include "Emu/PBD/pbd_practice_punish.h"

namespace
{
	using namespace pbd::practice::frames;
	using pbd::test::check;

	readiness_reading free_on(std::uint32_t cursor)
	{
		readiness_reading out;
		out.free = cursor;
		return out;
	}

	void carried_jump(std::uint32_t first, std::uint32_t last)
	{
		calculator sample;
		sample.commit_reading(0, 1, 101, 1, free_on(46), false, {}, {}, false, true);
		sample.commit_reading(0, 2, 102, 2, free_on(50), true, last, first, false, true);
		check(sample.latest[0].startup == first, "the jump's first frame counts when the attack enters on cursor 2");
	}
} // namespace

PBD_TEST(PbdPracticeStartup, NinaUfOnePlusTwo)
{
	// Thrmv_A7_00 has an inclusive 12..12 attack window; its 0x401 entry carries the jump cursor.
	carried_jump(12, 12);
}

PBD_TEST(PbdPracticeStartup, DevilJinUfFour)
{
	// Jz_jprk00F has an inclusive 18..18 attack window.
	carried_jump(18, 18);
}

PBD_TEST(PbdPracticeStartup, KingUfFour)
{
	// Co_jprk00F has an inclusive 15..17 attack window.
	carried_jump(15, 17);
}

PBD_TEST(PbdPracticeStartup, PaulUfThree)
{
	// pol_sjk00F has an inclusive 15..16 attack window.
	carried_jump(15, 16);
}

PBD_TEST(PbdPracticeStartup, PaulUfFour)
{
	carried_jump(15, 17);
	calculator sample;
	sample.commit_reading(0, 2, 101, 1, free_on(46), true, 17, 15);
	check(sample.latest[0].startup == 15u, "a direct cursor-1 entry retains its ordinary startup");
}

PBD_TEST(PbdPracticeStartup, JinUfThree)
{
	// Co_jplk00F has no attack window and hands over to Jz_spinlk, whose window is 7..9. The loaded animation's
	// duration supplies the windup, so exercise several durations rather than substituting a length for its zero on disk.
	for (std::uint32_t windup : {12u, 13u, 14u, 20u})
	{
		calculator sample;
		sample.commit_reading(0, 1, 101, 1, free_on(46), false, {}, {}, false, true);
		sample.commit_reading(0, 2, 102, 2, free_on(60), false, {}, {}, false, true);
		sample.commit_reading(0, 3, 101 + windup, 1, free_on(40), true, 9, 7);
		check(sample.latest[0].startup == windup + 7, "the separate windup adds to the final part's seven frames");
	}
}

PBD_TEST(PbdPracticeStartup, AWindowSkippedByAnEarlyBranchIsNotTheFirstHit)
{
	calculator sample;
	sample.commit_reading(0, 1, 101, 1, free_on(46), false, {}, {}, false, true);
	sample.commit_reading(0, 2, 102, 2, free_on(50), true, 17, 15, false, true);
	sample.commit_reading(0, 3, 103, 1, free_on(30), false);
	check(!sample.latest[0].startup, "a cancelled window has no active frame to display");
	sample.commit_reading(0, 4, 110, 1, free_on(40), true, 12, 10);
	check(sample.latest[0].startup == 19u, "a skipped fifteen-frame window is replaced by the actual later hit");
}

PBD_TEST(PbdPracticeStartup, GroundedAutomaticWindupAndRestoredSequence)
{
	calculator sample;
	sample.commit_reading(0, 1, 101, 1, free_on(60), false, {}, {}, false, true);
	const auto saved = sample;
	sample.commit_reading(0, 2, 111, 1, free_on(40), true, 10, 10);
	check(sample.latest[0].startup == 20u, "a grounded automatic prelude contributes its elapsed frames");
	sample = saved;
	sample.commit_reading(0, 2, 111, 1, free_on(40), true, 10, 10);
	check(sample.latest[0].startup == 20u, "a restored calculator retains the pending startup origin");
}

PBD_TEST(PbdPracticeStartup, SameTickAndDeferredHandoverUseTheGameCounter)
{
	calculator sample;
	sample.commit_reading(0, 1, 101, 1, free_on(46), false, {}, {}, false, true);
	sample.commit_reading(0, 2, 101, 2, free_on(50), true, 18, 18);
	check(sample.latest[0].startup == 17u, "two commits in one tick do not add an elapsed frame");
	sample.reset();
	sample.commit_reading(0, 1, 101, 1, free_on(46), false, {}, {}, false, true);
	sample.commit_reading(0, 2, 110, 1, free_on(50), true, 9, 9);
	check(sample.latest[0].startup == 18u, "a deferred jump attack includes the time before its cursor restarts");
}

PBD_TEST(PbdPracticeStartup, ATransitionOnTheFirstActiveFrameReplacesThatWindow)
{
	calculator sample;
	sample.commit_reading(0, 1, 101, 1, free_on(40), true, 15, 15);
	sample.commit_reading(0, 2, 115, 1, free_on(40), true, 7, 7);
	check(sample.latest[0].startup == 21u, "the old allocation left before the contact check on its first active frame");
}

PBD_TEST(PbdPracticeStartup, AReachedFirstHitSurvivesAStringAndItsMovementCancel)
{
	calculator sample;
	sample.commit_reading(0, 1, 101, 1, free_on(40), true, 15, 15);
	sample.commit_reading(0, 2, 120, 1, free_on(30), false);
	sample.commit_reading(0, 3, 125, 1, free_on(40), true, 12, 12);
	check(sample.latest[0].startup == 15u);
	sample.commit_reading(0, 4, 170, 1, free_on(40), true, 10, 10);
	check(sample.latest[0].startup == 10u, "an attack after recovery starts a fresh count");
}

PBD_TEST(PbdPracticeStartup, EarlyStanceAttackDiscardsAnAbandonedWindow)
{
	calculator sample;
	sample.commit_reading(0, 1, 101, 1, free_on(60), true, 17, 15);
	auto stance = free_on(30);
	stance.state = ready_state::stance;
	sample.commit_reading(0, 2, 110, 1, stance, false);
	check(!sample.latest[0].startup, "the stance skips the original active window");
	sample.commit_reading(0, 3, 112, 1, free_on(40), true, 10, 10);
	check(sample.latest[0].startup == 10u, "the early stance attack starts after freedom, not from the abandoned window");
}

PBD_TEST(PbdPracticeStartup, LandingInterruptionAndResetDiscardTheJumpOrigin)
{
	calculator sample;
	sample.commit_reading(0, 1, 101, 1, free_on(46), false, {}, {}, false, true);
	sample.commit_reading(0, 2, 140, 1, free_on(1), false);
	sample.commit_reading(0, 3, 150, 1, free_on(40), true, 10, 10);
	check(sample.latest[0].startup == 10u);
	sample.reset();
	sample.commit_reading(0, 1, 201, 1, free_on(46), false, {}, {}, false, true);
	sample.applied_contact(1, contact::hit, 205);
	sample.commit_reading(0, 4, 206, 1, free_on(20), false);
	sample.commit_reading(0, 3, 250, 1, free_on(40), true, 10, 10);
	check(sample.latest[0].startup == 10u);
	sample.reset();
	sample.commit_reading(0, 3, 300, 2, free_on(40), true, 10, 10);
	check(!sample.latest[0].startup, "an unobserved input cannot be inferred from a partial cursor");
}

PBD_TEST(PbdPracticeStartup, SkippedActiveFramesAndIndependentSides)
{
	calculator sample;
	sample.commit_reading(0, 1, 101, 1, free_on(46), false, {}, {}, false, true);
	sample.commit_reading(1, 5, 105, 1, free_on(40), true, 11, 10);
	sample.commit_reading(0, 2, 120, 20, free_on(40), true, 17, 15);
	check(!sample.latest[0].startup && sample.latest[1].startup == 10u);
	sample.reset();
	sample.commit_reading(0, 1, 101, 1, free_on(46), false, {}, {}, false, true);
	sample.commit_reading(0, 2, 116, 16, free_on(40), true, 17, 15);
	check(sample.latest[0].startup == 16u, "entry inside an active window counts its first playable active frame");
}

PBD_TEST(PbdPracticeStartup, PunishVerdictUsesTheWholeJumpStartup)
{
	calculator sample;
	sample.commit_reading(1, 5, 80, 1, free_on(60), true, 10, 10);
	sample.applied_contact(1, contact::block, 90);
	sample.commit_reading(0, 6, 91, 1, free_on(10), false);
	pbd::practice::punish::watch judge;
	judge.boundary(sample.latest[0], sample.latest[1], 91);
	sample.commit_reading(0, 1, 101, 1, free_on(46), false, {}, {}, false, true);
	sample.commit_reading(0, 2, 102, 2, free_on(60), false, {}, {}, false, true);
	sample.commit_reading(0, 3, 114, 1, free_on(40), true, 9, 7);
	sample.applied_contact(0, contact::hit, 120);
	const auto result = judge.boundary(sample.latest[0], sample.latest[1], 120);
	check(result && result->startup == 20u && result->outcome == pbd::practice::punish::verdict::punished);
}
