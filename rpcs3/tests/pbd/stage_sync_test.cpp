#include "Emu/PBD/pbd_stage_sync.h"

#include "pbd_test.h"

#include <cstdint>

using namespace pbd::stage_sync;

namespace
{
	// A three-game set: City Lights (no mobs), then Wolves' Den, then Ominous Mist.
	constexpr std::uint32_t session_seed = 0x2e851713;
	constexpr std::uint32_t wolves_den_seed = 2352259158u;   // game 2, mob batch 1
	constexpr std::uint32_t ominous_mist_seed = 1095366307u; // game 3, mob batch 1
} // namespace

PBD_TEST(PbdStageSync, ScreenAndMobSeedsFollowTheSessionSeedAndTheMatchNumber)
{
	pbd::test::check(screen_seed(session_seed, 0) == 2876169592u, "Match 1 screen seed differs from the recording");
	pbd::test::check(screen_seed(session_seed, 1) == 626164677u, "Match 2 screen seed differs from the recording");
	pbd::test::check(screen_seed(session_seed, 2) == 3209054290u, "Match 3 screen seed differs from the recording");
	pbd::test::check(mob_batch_seed(session_seed, 1, 1) == wolves_den_seed, "Wolves' Den batch seed differs");
	pbd::test::check(mob_batch_seed(session_seed, 2, 1) == ominous_mist_seed, "Ominous Mist batch seed differs");
	pbd::test::check(mob_batch_seed(session_seed, 1, 2) != wolves_den_seed, "Batch 2 reused batch 1's seed");
}

PBD_TEST(PbdStageSync, ControllersCreatedInOneTickShareOneBatch)
{
	mob_batches live;
	pbd::test::check(live.open(12003) == 1, "The first controller did not open batch 1");
	pbd::test::check(live.open(12003) == 0, "A second controller in the same tick opened another batch");
	pbd::test::check(live.open(12004) == 2, "A controller in a later tick did not open batch 2");
	pbd::test::check(live.count() == 2, "Two batches were not counted");
}

PBD_TEST(PbdStageSync, EveryMatchStartRestartsTheBatchNumbers)
{
	mob_batches live;
	live.begin_match();
	pbd::test::check(live.open(12003) == 1, "Game 2's stage load did not open batch 1");
	pbd::test::check(live.count() == 1, "The recorder did not see the load's batch before game 2's initializer");
	live.begin_match();
	pbd::test::check(live.count() == 0, "Game 2's start kept the batch count");
	pbd::test::check(live.open(27700) == 1, "Game 3's stage load did not open batch 1");
}

// The recorder saves the count it sees at the match initializer, after the stage load. A replay boots fresh and runs
// that load again, so it reaches the initializer with the recorded count only when it starts from none.
PBD_TEST(PbdStageSync, AReplayNumbersItsStageLoadsMobBatchesAsTheRecordingDid)
{
	auto replay = epoch::replay(1);
	pbd::test::check(replay.match_seeds == 1, "The replay did not continue the recording's match numbers");
	const std::uint32_t batch = replay.mobs.open(63);
	pbd::test::check(batch == 1, "The replay numbered its wolves' batch differently from the recording");
	pbd::test::check(mob_batch_seed(session_seed, replay.match_seeds, batch) == wolves_den_seed,
		"The replay reseeded its wolves differently from the recording");
	pbd::test::check(replay.reached(1), "The replay did not reach the recorded mob batch count");
	pbd::test::check(!replay.reached(2), "A count other than the recorded one was accepted");
}
