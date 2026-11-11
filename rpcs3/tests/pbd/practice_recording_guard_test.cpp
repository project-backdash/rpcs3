#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_recording_guard.h"

namespace
{
	using namespace pbd::practice;
	using pbd::test::check;

	struct recording_fighter
	{
		pbd_recording_guard control;
		pbd_recording_guard_view now{7, 100, 0x2000004c, {0x1952, 0x1952, 0x1952}};
		std::array<bool, 3> writable{true, true, true};
		unsigned writes = 0;

		auto writer()
		{
			return [&](unsigned field, std::uint32_t value)
			{
				if (!writable[field])
					return false;
				now.flags[field] = value;
				++writes;
				return true;
			};
		}
		void boundary(bool recording)
		{
			control.boundary(recording, now, writer());
		}
		void entered()
		{
			control.entered(now, writer());
		}
	};
} // namespace

PBD_TEST(PbdPracticeRecordingGuard, RecordingUsesTheExistingNoGuardBits)
{
	recording_fighter fighter;
	fighter.now.flags = {0x195a, 0x3029, 0x1052};
	const auto before = fighter.now.flags;
	fighter.boundary(true);
	for (unsigned field = 0; field < before.size(); ++field)
		check(fighter.now.flags[field] == pbd_without_guard(before[field]), "recording clears both guard bits and keeps every other flag");
	check(fighter.writes == 3 && fighter.control.active);
}

PBD_TEST(PbdPracticeRecordingGuard, StoppingRestoresGuardAtTheSameBoundary)
{
	recording_fighter fighter;
	const auto before = fighter.now.flags;
	fighter.boundary(true);
	++fighter.now.counter;
	fighter.boundary(false);
	check(fighter.now.flags == before && !fighter.control.active, "stopping restores neutral guard before another game frame runs");
	const auto writes = fighter.writes;
	fighter.entered();
	check(fighter.writes == writes, "the first move after Stop keeps its own guard");
}

PBD_TEST(PbdPracticeRecordingGuard, RepeatedBoundariesKeepTheOriginalGuard)
{
	recording_fighter fighter;
	const auto before = fighter.now.flags;
	for (unsigned frame = 0; frame < 120; ++frame)
	{
		fighter.boundary(true);
		fighter.boundary(true);
		++fighter.now.counter;
	}
	check(fighter.writes == 3, "neutral recording frames do not overwrite the saved guard with cleared bits");
	fighter.boundary(false);
	check(fighter.now.flags == before);
}

PBD_TEST(PbdPracticeRecordingGuard, NewMovesLoseGuardBeforeContactAndRestoreTheirOwnFlags)
{
	recording_fighter fighter;
	fighter.boundary(true);
	++fighter.now.counter;
	for (const std::uint32_t flags : {0x6084u, 0x1052u, 0x3029u, 0x1952u})
	{
		fighter.now.move += 0x4c;
		fighter.now.flags = {flags, flags, flags};
		fighter.entered();
		check(fighter.now.flags == pbd_guard_values{pbd_without_guard(flags), pbd_without_guard(flags), pbd_without_guard(flags)},
			"knockdown, rising, crouch and neutral entries all use the same no-guard route before contact");
	}
	fighter.boundary(false);
	check(fighter.now.flags == pbd_guard_values{0x1952, 0x1952, 0x1952}, "Stop restores the latest move, not the stance at Record");
}

PBD_TEST(PbdPracticeRecordingGuard, ReenteringTheSameMoveDoesNotRestoreAnOldGuard)
{
	recording_fighter fighter;
	fighter.boundary(true);
	fighter.now.flags = {0x6084, 0x6084, 0x6084};
	fighter.entered();
	fighter.boundary(false);
	check(fighter.now.flags == pbd_guard_values{0x6084, 0x6084, 0x6084}, "a new move entry replaces the old snapshot even when its allocation is the same");
}

PBD_TEST(PbdPracticeRecordingGuard, GameChangesWinWhenRecordingStops)
{
	recording_fighter fighter;
	fighter.boundary(true);
	fighter.now.flags[0] = 0x6084;
	fighter.now.flags[2] = 0x3029;
	fighter.boundary(false);
	check(fighter.now.flags == pbd_guard_values{0x6084, 0x1952, 0x3029}, "only fields still holding the route's exact write are restored");
}

PBD_TEST(PbdPracticeRecordingGuard, MovePropertiesAreClearedAtTheNextBoundary)
{
	recording_fighter fighter;
	fighter.boundary(true);
	++fighter.now.counter;
	fighter.now.flags = {0x3029, 0x3029, 0x3029};
	fighter.boundary(true);
	check(fighter.now.flags == pbd_guard_values{0x3021, 0x3021, 0x3021});
	fighter.boundary(false);
	check(fighter.now.flags == pbd_guard_values{0x3029, 0x3029, 0x3029}, "a property change supersedes the older guard snapshot");
}

PBD_TEST(PbdPracticeRecordingGuard, ReadyReviewPlaybackAndOrdinaryPracticeKeepGuard)
{
	recording_fighter fighter;
	const auto before = fighter.now.flags;
	for (unsigned boundary = 0; boundary < 8; ++boundary)
	{
		fighter.boundary(false);
		fighter.entered();
		++fighter.now.counter;
	}
	check(fighter.now.flags == before && fighter.writes == 0 && !fighter.control.active,
		"only the recording state enables the player's no-guard override");
}

PBD_TEST(PbdPracticeRecordingGuard, StoppingWhileDownDoesNotGiveTheFloorMoveGuard)
{
	recording_fighter fighter;
	fighter.boundary(true);
	++fighter.now.counter;
	fighter.now.move += 0x4c;
	fighter.now.flags = {0x6084, 0x6084, 0x6084};
	fighter.entered();
	fighter.boundary(false);
	check(fighter.now.flags == pbd_guard_values{0x6084, 0x6084, 0x6084}, "Stop does not turn a non-guarding hit reaction into a guard");
}

PBD_TEST(PbdPracticeRecordingGuard, LifetimeAndClockChangesDiscardOldWrites)
{
	for (unsigned change = 0; change < 4; ++change)
	{
		recording_fighter fighter;
		fighter.boundary(true);
		if (change == 0)
			++fighter.now.generation;
		else if (change == 1)
			fighter.now.counter -= 2;
		else if (change == 2)
			fighter.now.counter += 2;
		else
			fighter.now.move += 0x4c;
		const auto writes = fighter.writes;
		fighter.boundary(false);
		check(fighter.writes == writes && !fighter.control.active, "restored images and different moves never receive stale guard bits");
	}
}

PBD_TEST(PbdPracticeRecordingGuard, MoveHooksExpireAfterOneFrameAndAcrossLifetimes)
{
	for (unsigned change = 0; change < 2; ++change)
	{
		recording_fighter fighter;
		fighter.boundary(true);
		if (change == 0)
			fighter.now.counter += 2;
		else
			++fighter.now.generation;
		fighter.now.flags = {0x1952, 0x1952, 0x1952};
		const auto writes = fighter.writes;
		fighter.entered();
		check(fighter.writes == writes, "an old recording cannot clear a later frame or another boot");
	}
}

PBD_TEST(PbdPracticeRecordingGuard, MoveHooksCannotRenewAnOldRecording)
{
	recording_fighter fighter;
	fighter.boundary(true);
	++fighter.now.counter;
	fighter.now.flags = {0x1952, 0x1952, 0x1952};
	fighter.entered();
	check(fighter.now.flags == pbd_guard_values{0x1942, 0x1942, 0x1942}, "the next frame may still clear its move entry");
	++fighter.now.counter;
	fighter.now.flags = {0x1952, 0x1952, 0x1952};
	const auto writes = fighter.writes;
	fighter.entered();
	check(fighter.writes == writes && !fighter.control.active, "only a recording boundary can renew the override, never a move hook");
}

PBD_TEST(PbdPracticeRecordingGuard, UnreadableAndUnwrittenFlagsAreNeverRestored)
{
	recording_fighter fighter;
	fighter.writable[1] = false;
	fighter.boundary(true);
	check(fighter.now.flags == pbd_guard_values{0x1942, 0x1952, 0x1942});
	fighter.writable[1] = true;
	fighter.now.flags[1] = 0x6084;
	fighter.boundary(false);
	check(fighter.now.flags == pbd_guard_values{0x1952, 0x6084, 0x1952}, "a failed write never acquires restoration ownership");
	fighter.boundary(true);
	fighter.control.boundary(false, std::nullopt, fighter.writer());
	check(!fighter.control.active, "an unreadable Practice scene drops the override without writing memory");
	const auto writes = fighter.writes;
	fighter.boundary(false);
	check(fighter.writes == writes, "discarded snapshots cannot write on a later readable scene");
}
