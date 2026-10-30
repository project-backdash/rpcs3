#include "pbd_test.h"

#include "Emu/PBD/pbd_replay_rewind.h"
#include "Emu/PBD/pbd_replay_checkpoint.h"

#include <array>
#include <vector>

using pbd::test::check;
using namespace pbd::replay_rewind;

PBD_TEST(PbdReplayRewind, CapturesOneCheckpointPerSecondOfFightAndEveryRoundStart)
{
	check(capture_due(true, true, false, false, 0, 500));          // the first fight boundary
	check(!capture_due(false, true, true, false, 0, 500));         // a taken side: the replay no longer plays the recording
	check(!capture_due(true, false, true, false, 0, 500));         // between rounds the render thread owns the frame object
	check(!capture_due(true, true, false, true, 500, 500 + spacing - 1));
	check(capture_due(true, true, false, true, 500, 500 + spacing));
	check(capture_due(true, true, true, true, 500, 510));          // a round start is always kept
	check(!capture_due(true, true, true, true, 900, 600));         // behind the newest after a restore: wait until past it
	check(!capture_due(true, true, false, true, 900, 900));
}

PBD_TEST(PbdReplayRewind, RestoresTheNewestCheckpointAtOrBeforeTheTarget)
{
	const std::vector<point> points{{100, 1, 0, 1, true}, {160, 1, 60, 1}, {220, 1, 120, 1}, {700, 1, 600, 2, true}, {760, 1, 660, 2}};
	check(restore_choice(points, 1, 0) == 0u);
	check(restore_choice(points, 1, 59) == 0u);
	check(restore_choice(points, 1, 60) == 1u);
	check(restore_choice(points, 1, 599) == 2u);
	check(restore_choice(points, 1, 5000) == 4u);
	check(!restore_choice(points, 2, 100));
	check(!restore_choice({}, 1, 100));
	const std::vector<point> later{{300, 1, 200, 1}, {360, 1, 260, 1}};
	check(restore_choice(later, 1, 50) == 0u); // before the first: the earliest moment still held
	check(round_choice(points, 1, 2) == 3u && round_choice(points, 1, 1) == 0u);
	check(!round_choice(points, 1, 3) && !round_choice(later, 1, 1)); // only a pinned start counts
}

PBD_TEST(PbdReplayRewind, ThinsEvenlyAndKeepsTheEndsAndRoundStarts)
{
	std::vector<point> points{{0, 1, 0, 1, true}, {60, 1, 60, 1}, {120, 1, 120, 1}, {150, 1, 150, 2, true}, {210, 1, 210, 2}, {270, 1, 270, 2}};
	check(!eviction_choice(points, 100, 100));
	// Unpinned middles: 60 (span 120), 120 (span 90), 210 (span 120). The closest neighbours go first.
	check(eviction_choice(points, 101, 100) == 2u);
	points.erase(points.begin() + 2);
	check(eviction_choice(points, 101, 100) == 3u); // now 60 spans 150 and 210 spans 120
	const std::vector<point> pinned{{0, 1, 0, 1, true}, {60, 1, 60, 2, true}, {90, 1, 90, 3, true}, {120, 1, 120, 3}};
	check(eviction_choice(pinned, 2, 1) == 2u); // only pinned middles left: still the closest pair
	check(!eviction_choice(std::vector<point>{{0, 1}, {60, 1}}, 2, 1));
}

PBD_TEST(PbdReplayRewind, StepsAddUpFromThePendingTargetAndStayInTheMatch)
{
	check(step_frames == 60 * 10);
	check(step_target(1000, -static_cast<s32>(step_frames), 5000) == 400);
	check(step_target(100, -static_cast<s32>(step_frames), 5000) == 0);
	check(step_target(4900, static_cast<s32>(step_frames), 5000) == 5000);
	check(step_target(1600, -static_cast<s32>(step_frames), 5000) == 1000);
	check(step_target(1000, -static_cast<s32>(step_frames), 5000) == 400); // a second press from the pending 1000
	check(budget_from_mib(0) == default_budget && budget_from_mib(256) == (256ull << 20) && budget_from_mib(10) == default_budget);
	check(budget_from_mib(1u << 20) == default_budget);
}

PBD_TEST(PbdReplayRewind, ReconstructedRoundsKeepTheirRecordedTimeOnMonotoneBoundaries)
{
	check(recorded_frame(120, 100, 0, 20) == 0);
	check(recorded_frame(150, 100, 0, 20) == 30);
	check(recorded_frame(200, 200, 3600, 20) == 3580); // a jump ahead of the physical boundary
	check(recorded_frame(9000, 9000, 100, 20) == 80);  // a jump back without rewinding the physical boundary
	check(recorded_frame(9010, 9000, 100, 20) == 90);
	check(recorded_frame(50, 100, 100, 20) == 0);
	check(recorded_frame(50, ~0u, 100, 20) == 0);
}

PBD_TEST(PbdReplayRewind, BackStepsUseAnEarlierCheckpointWithoutFallingForward)
{
	const std::vector<point> points{{100, 1, 0, 1, true}, {160, 1, 60, 1}, {220, 1, 120, 1}};
	// Paused at 61: target 60. The bridge executes once, so restoring 60 would land at 61 again.
	const auto target = step_target(61, -1, 500);
	check(target == 60 && restore_choice(points, 1, target - 1, true) == 0u);
	check(restore_choice(points, 1, 60, true) == 1u);
	const std::vector<point> later{{300, 1, 200, 1}};
	check(!restore_choice(later, 1, 199, true));
	check(restore_choice(later, 1, 199) == 0u);
}

PBD_TEST(PbdReplayRewind, WorkerCompressionRetainsExactPagesAndFirstWriteImages)
{
	using namespace pbd::replay_checkpoint;
	std::vector<std::uint8_t> pages(616 * 4096, 0), journal(97 * 4096, 0);
	for (std::size_t i = 0; i < pages.size(); i += 317) pages[i] = static_cast<std::uint8_t>(i >> 4);
	for (std::size_t i = 0; i < journal.size(); ++i) journal[i] = static_cast<std::uint8_t>((i / 4096) ^ (i & 31));
	const auto expected_pages = pages, expected_journal = journal;
	std::shared_ptr<image> sealed;
	{
		compressor worker;
		sealed = worker.submit(pages, journal);
		check(sealed && pages.empty() && journal.empty());
	} // Destruction drains pending work, even after the session discarded its checkpoint.
	sealed->wait();
	check(sealed->ready.load() && sealed->pages.packed && sealed->journal.packed);
	check(sealed->bytes() < expected_pages.size() + expected_journal.size());
	check(sealed->pages.expand(pages) && sealed->journal.expand(journal));
	check(pages == expected_pages && journal == expected_journal);
	sealed->pages.bytes.resize(3);
	check(!sealed->pages.expand(pages));
}

PBD_TEST(PbdReplayRewind, RepeatedPageImagesDoNotRequireDecodingEveryCheckpoint)
{
	journal_coverage coverage;
	check(coverage.include(std::array<u32, 3>{1, 2, 3}));
	for (u32 checkpoint = 0; checkpoint < 180; ++checkpoint)
		check(!coverage.include(std::array<u32, 3>{3, 1, 2}));
	check(coverage.include(std::array<u32, 3>{2, 4, 5}));
	check(!coverage.include(std::array<u32, 3>{5, 4, 1}));
	check(coverage.seen.size() == 5);
}

PBD_TEST(PbdReplayRewind, HeldDirectionRepeats)
{
	constexpr u16 left = 0x8000, right = 0x2000;
	repeat r;
	check(r.poll(left, left, left | right, 1000) == left);
	check(r.poll(left, 0, left | right, 1000 + repeat_delay_ms - 1) == 0);
	check(r.poll(left, 0, left | right, 1000 + repeat_delay_ms) == left);
	check(r.poll(left, 0, left | right, 1000 + repeat_delay_ms + repeat_interval_ms) == left);
	check(r.poll(0, 0, left | right, 5000) == 0);
	check(r.poll(left, 0, left | right, 6000) == 0); // a release ends the hold; only a fresh press starts it again
	repeat clockless;
	check(clockless.poll(right, right, left | right, 0) == right);
	check(clockless.poll(right, 0, left | right, 0) == 0);
}
