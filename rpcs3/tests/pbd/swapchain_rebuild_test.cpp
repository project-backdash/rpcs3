#include "pbd_test.h"

#include "Emu/PBD/pbd_swapchain_rebuild.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using pbd::test::check;

namespace
{
	enum class result
	{
		success,
		suboptimal,
		out_of_date
	};

	// The renderer's use of the rule (VKPresent.cpp). Each flip rebuilds first when a rebuild is flagged, or when the window's
	// size differs from the swapchain's on a driver that polls it; it then acquires an image and presents it, and each
	// result can flag a rebuild for the next flip.
	struct renderer
	{
		pbd::swapchain_rebuild::rule rebuilds;
		bool polls_size = true; // AMD and Intel; NVIDIA reports window changes itself and is never polled
		bool should_rebuild = false, unavailable = false;
		std::uint32_t width = 1280, height = 720, monitor = 1;                      // what the swapchain was made for
		std::uint32_t window_width = 1280, window_height = 720, window_monitor = 1; // the window now
		int made = 0, held_warnings = 0;

		bool window_changed() const
		{
			return window_width != width || window_height != height || window_monitor != monitor;
		}

		void suboptimal()
		{
			const bool follow = rebuilds.suboptimal();
			if (follow || window_changed())
				should_rebuild = true;
			else if (rebuilds.report_held())
				++held_warnings;
		}

		void rebuild()
		{
			rebuilds.rebuilding(should_rebuild && !unavailable, !window_changed());
			width = window_width;
			height = window_height;
			monitor = window_monitor;
			should_rebuild = unavailable = false;
			++made;
		}

		void flip(result acquire, result present)
		{
			if (polls_size && (window_width != width || window_height != height))
				unavailable = true;
			if (unavailable || should_rebuild)
				rebuild();

			if (acquire == result::out_of_date)
			{
				// Rebuilt at once, and the next acquire succeeds.
				unavailable = true;
				rebuild();
			}
			else if (acquire == result::suboptimal)
				suboptimal();

			if (present == result::suboptimal)
				suboptimal();
			else if (present == result::out_of_date)
				unavailable = true;
			rebuilds.presented(present == result::success);
		}

		void flips(int count, result acquire, result present)
		{
			for (int i = 0; i < count; ++i)
				flip(acquire, present);
		}

		void flips(int count, result present)
		{
			flips(count, result::success, present);
		}
	};

	// A driver that keeps Vulkan's rules for oldSwapchain, for create_replacing. Swapchains are numbered from 1; 0 is none.
	struct driver
	{
		bool refuses_exclusive = false, refuses_all = false;
		int next = 1;
		std::vector<int> alive, retired;
		std::vector<std::string> calls;
		bool broke_a_rule = false;

		static bool has(const std::vector<int>& list, int swapchain)
		{
			return std::find(list.begin(), list.end(), swapchain) != list.end();
		}

		// A swapchain made earlier, the one being replaced.
		int existing()
		{
			alive.push_back(next);
			return next++;
		}

		bool create(bool exclusive, int old)
		{
			calls.push_back(std::string(exclusive ? "create exclusive" : "create") + " old " + std::to_string(old));
			// oldSwapchain is none or a live swapchain not yet retired, and the call retires it whether or not it succeeds.
			if (old && (!has(alive, old) || has(retired, old)))
				broke_a_rule = true;
			if (old)
				retired.push_back(old);
			// Stricter than Vulkan, which counts only a swapchain that is not retired: a window that still has a swapchain
			// other than oldSwapchain, retired or not, is in use, as a driver or layer may treat it.
			for (int swapchain : alive)
				if (swapchain != old)
					return false;
			if (refuses_all || (exclusive && refuses_exclusive))
				return false;
			alive.push_back(next++);
			return true;
		}

		void destroy(int swapchain)
		{
			calls.push_back("destroy " + std::to_string(swapchain));
			if (!has(alive, swapchain))
				broke_a_rule = true;
			std::erase(alive, swapchain);
		}

		bool make(int old, bool exclusive)
		{
			const auto create_one = [this](bool with_exclusive, int replaced)
			{
				return create(with_exclusive, replaced);
			};
			const auto destroy_one = [this](int swapchain)
			{
				destroy(swapchain);
			};
			return pbd::swapchain_rebuild::create_replacing(old, exclusive, create_one, destroy_one);
		}
	};

	// flip()'s retries for a new swapchain (VKPresent.cpp). Attempt n succeeds when `answers[n]` is true and is refused
	// otherwise, also once the answers run out; the emulator stops after `stop_after` refusals.
	struct retries
	{
		std::vector<bool> answers;
		int stop_after = -1;
		int attempts = 0, refusals = 0, waits = 0;

		bool run()
		{
			const auto attempt = [this]()
			{
				const bool made = attempts < static_cast<int>(answers.size()) && answers[attempts];
				++attempts;
				refusals += made ? 0 : 1;
				return made;
			};
			const auto stopped = [this]()
			{
				return refusals == stop_after;
			};
			const auto wait = [this]()
			{
				++waits;
			};
			return pbd::swapchain_rebuild::rebuild_with_retries(10, attempt, stopped, wait);
		}
	};
} // namespace

PBD_TEST(PbdSwapchainRebuild, SameWindowSuboptimalRebuildsTwiceThenKeepsTheSwapchain)
{
	renderer r;
	r.flips(200, result::suboptimal);
	check(r.made == 2, "two rebuilds for a driver that calls every new swapchain suboptimal");
	check(r.held_warnings == 1, "one warning that the swapchain is kept");

	renderer both;
	both.flips(200, result::suboptimal, result::suboptimal);
	check(both.made == 2, "an acquire and a present both suboptimal still count as one rebuild per flip");
	check(both.held_warnings == 1);
}

PBD_TEST(PbdSwapchainRebuild, ASuboptimalAcquireWithASuccessfulPresentIsHeldToo)
{
	renderer r;
	r.flips(200, result::suboptimal, result::success);
	check(r.made == 2, "the present's success does not undo the acquire's report");
	check(r.held_warnings == 1);
}

PBD_TEST(PbdSwapchainRebuild, AlternatingResultsAreHeldToo)
{
	// Each swapchain presents once successfully, then suboptimal.
	renderer r;
	for (int i = 0; i < 100; ++i)
	{
		r.flip(result::success, result::success);
		r.flip(result::success, result::suboptimal);
	}
	check(r.made == 2, "a single success between reports does not let them rebuild again");
	check(r.held_warnings == 1);

	// Acquire suboptimal and present success, then the reverse.
	renderer swapped;
	for (int i = 0; i < 100; ++i)
	{
		swapped.flip(result::suboptimal, result::success);
		swapped.flip(result::success, result::suboptimal);
	}
	check(swapped.made == 2);
}

PBD_TEST(PbdSwapchainRebuild, ARunOfCleanFramesLetsALaterReportRebuildAgain)
{
	renderer r;
	r.flips(20, result::suboptimal);
	check(r.made == 2);
	r.flips(pbd::swapchain_rebuild::clean_frames_to_follow_again - 1, result::success);
	r.flip(result::success, result::suboptimal);
	check(r.made == 2, "one frame short of the run: still held");
	r.flips(pbd::swapchain_rebuild::clean_frames_to_follow_again, result::success);
	r.flips(20, result::suboptimal);
	check(r.made == 4, "after a full run a report is followed twice again");
	check(r.held_warnings == 2, "and its warning is given again");

	// A frame whose acquire was suboptimal is not clean even when its present succeeded.
	renderer acquire;
	acquire.flips(20, result::suboptimal);
	for (std::uint32_t i = 0; i < 3 * pbd::swapchain_rebuild::clean_frames_to_follow_again; ++i)
		acquire.flip(i % 30 == 0 ? result::suboptimal : result::success, result::success);
	acquire.flips(20, result::suboptimal);
	check(acquire.made == 2);
}

PBD_TEST(PbdSwapchainRebuild, ASizeChangeRebuildsAndStartsTheCountAgain)
{
	renderer r;
	r.flips(20, result::suboptimal);
	check(r.made == 2);
	r.window_width = 1920;
	r.window_height = 1080;
	r.flip(result::success, result::suboptimal);
	check(r.made == 3, "the new size is always rebuilt for");
	check(r.rebuilds.same_window_rebuilds() == 0);
	r.flips(20, result::suboptimal);
	check(r.made == 5, "a suboptimal report at the new size is followed twice again");
	check(r.held_warnings == 2, "the warning is given again for the new size");
}

PBD_TEST(PbdSwapchainRebuild, ADriverThatIsNeverPolledStillRebuildsForAChangedWindow)
{
	// Without the size poll only the driver's report says the window changed; a held swapchain still follows it.
	renderer r;
	r.polls_size = false;
	r.flips(20, result::suboptimal);
	check(r.made == 2);
	r.window_width = 1920;
	r.window_height = 1080;
	r.flip(result::success, result::suboptimal);
	r.flip(result::success, result::success);
	check(r.made == 3 && r.width == 1920 && r.height == 1080, "rebuilt for the new size");
	check(r.rebuilds.same_window_rebuilds() == 0);

	// The same for the window moved to another monitor of the same size, through an acquire report.
	r.flips(20, result::suboptimal);
	check(r.made == 5);
	r.window_monitor = 2;
	r.flip(result::suboptimal, result::success);
	r.flip(result::success, result::success);
	check(r.made == 6 && r.monitor == 2, "rebuilt for the new monitor");
	check(r.rebuilds.same_window_rebuilds() == 0);

	// A changed window the driver never reports is left alone, as before.
	renderer quiet;
	quiet.polls_size = false;
	quiet.window_width = 1920;
	quiet.flips(20, result::success);
	check(quiet.made == 0);
}

PBD_TEST(PbdSwapchainRebuild, OutOfDateAndUnavailableAreNeverHeldBack)
{
	renderer r;
	r.flips(50, result::out_of_date);
	check(r.made == 49, "every out-of-date swapchain is rebuilt on the next flip");
	check(r.held_warnings == 0);

	renderer acquire;
	acquire.flips(50, result::out_of_date, result::success);
	check(acquire.made == 50, "every out-of-date acquire is rebuilt at once");

	// Out of date while suboptimal reports are held: still rebuilt, and the count starts again.
	renderer held;
	held.flips(20, result::suboptimal);
	check(held.made == 2);
	held.flip(result::success, result::out_of_date);
	held.flip(result::success, result::suboptimal);
	check(held.made == 3);
	held.flip(result::success, result::suboptimal);
	check(held.made == 4, "a report after the out-of-date rebuild is followed again");

	// The rule itself: a rebuild for anything but a suboptimal report alone never counts toward holding.
	pbd::swapchain_rebuild::rule rule;
	rule.rebuilding(true, true);
	rule.rebuilding(true, true);
	check(!rule.suboptimal());
	rule.rebuilding(false, true);
	check(rule.same_window_rebuilds() == 0 && rule.suboptimal());
	rule.rebuilding(true, false);
	check(rule.same_window_rebuilds() == 0, "a suboptimal rebuild for a changed window does not count");
}

PBD_TEST(PbdSwapchainRebuild, StatusLinesAreTheFirstEightThenEveryHundredth)
{
	std::vector<std::uint64_t> logged;
	for (std::uint64_t count = 0; count <= 1000; ++count)
		if (pbd::swapchain_rebuild::log_status(count))
			logged.push_back(count);
	const std::vector<std::uint64_t> expected{1, 2, 3, 4, 5, 6, 7, 8, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000};
	check(logged == expected);
}

PBD_TEST(PbdSwapchainCreate, ANewSwapchainReplacesTheOldOneWhichIsThenDestroyed)
{
	driver d;
	const int old = d.existing();
	check(d.make(old, true));
	check(d.calls == std::vector<std::string>{"create exclusive old 1", "destroy 1"});
	check(d.alive == std::vector<int>{2} && !d.broke_a_rule);

	driver first;
	check(first.make(0, false), "the first swapchain replaces none");
	check(first.calls == std::vector<std::string>{"create old 0"});
	check(first.alive == std::vector<int>{1} && !first.broke_a_rule);
}

PBD_TEST(PbdSwapchainCreate, ARefusedExclusiveRequestIsAskedAgainWithoutItOnceTheRetiredSwapchainIsGone)
{
	driver d;
	d.refuses_exclusive = true;
	const int old = d.existing();
	check(d.make(old, true), "made without the full screen exclusive request");
	check(d.calls == std::vector<std::string>{"create exclusive old 1", "destroy 1", "create old 0"});
	check(d.alive == std::vector<int>{2} && !d.broke_a_rule);

	// Why: the refused call retired the old swapchain, so passing it again breaks the rules, and passing none while it is
	// still alive leaves the window in use for a driver that counts retired swapchains.
	driver again;
	again.refuses_exclusive = true;
	again.create(true, again.existing());
	again.create(false, 1);
	check(again.broke_a_rule);
	driver kept;
	kept.refuses_exclusive = true;
	kept.create(true, kept.existing());
	check(!kept.create(false, 0) && !kept.broke_a_rule, "refused as in use");

	driver first;
	first.refuses_exclusive = true;
	check(first.make(0, true));
	check(first.calls == std::vector<std::string>{"create exclusive old 0", "create old 0"});
	check(first.alive == std::vector<int>{1} && !first.broke_a_rule);
}

PBD_TEST(PbdSwapchainCreate, AFailureLeavesNoSwapchainAndTheNextAttemptStartsFromNothing)
{
	driver d;
	d.refuses_all = true;
	const int old = d.existing();
	check(!d.make(old, true));
	check(d.calls == std::vector<std::string>{"create exclusive old 1", "destroy 1", "create old 0"});
	check(d.alive.empty() && !d.broke_a_rule, "the retired swapchain is destroyed once and nothing is left");

	d.refuses_all = false;
	d.calls.clear();
	check(d.make(0, true), "the next attempt replaces none");
	check(d.calls == std::vector<std::string>{"create exclusive old 0"});
	check(d.alive == std::vector<int>{2} && !d.broke_a_rule);

	driver plain;
	plain.refuses_all = true;
	check(!plain.make(plain.existing(), false), "without the exclusive request there is no second call");
	check(plain.calls == std::vector<std::string>{"create old 1", "destroy 1"});
	check(plain.alive.empty() && !plain.broke_a_rule);
}

PBD_TEST(PbdSwapchainRetries, ADriverThatRefusesForAMomentGetsItsSwapchainOnALaterAttempt)
{
	retries r;
	r.answers = {false, false, true};
	check(r.run(), "made on the third attempt");
	check(r.attempts == 3 && r.waits == 2);

	retries first;
	first.answers = {true};
	check(first.run(), "a swapchain made, or frame contexts kept, at once");
	check(first.attempts == 1 && first.waits == 0);
}

PBD_TEST(PbdSwapchainRetries, ADriverThatKeepsRefusingIsAskedTenTimesThenTheRendererGivesUp)
{
	retries r;
	check(!r.run());
	check(r.attempts == 10 && r.refusals == 10, "bounded, never asked an eleventh time");
}

PBD_TEST(PbdSwapchainRetries, AStoppingEmulatorEndsTheAttempts)
{
	retries r;
	r.stop_after = 2;
	check(!r.run());
	check(r.attempts == 2 && r.waits == 1, "no attempt and no wait after the stop");

	retries made;
	made.answers = {false, true};
	made.stop_after = 5;
	check(made.run() && made.attempts == 2, "a stop that comes later does not matter");
}
