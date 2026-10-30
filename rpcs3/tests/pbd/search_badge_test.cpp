#include "pbd_test.h"

#include "Emu/Cell/pbd_search_badge.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using pbd::test::check;

PBD_TEST(PbdSearchBadge, ParsesOnlyWellFormedFiles)
{
	using namespace pbd::search_badge;
	const auto ranked = parse("PBD-SEARCH-1\ngeneration=7\nactive=1\nmode=ranked\nstarted=1760000000000\n");
	check(ranked.value && ranked.value->active && ranked.value->mode == search_mode::ranked);
	check(ranked.value->generation == 7 && ranked.value->started_ms == 1760000000000ull);
	const auto ended = parse("PBD-SEARCH-1\r\ngeneration=8\r\nactive=0\r\n");
	check(ended.value && !ended.value->active);

	check(parse("PBD-MENU-1\ngeneration=1\nactive=0\n").error == "bad_header");
	check(parse("PBD-SEARCH-1\nactive=0\n").error == "missing_required");
	check(parse("PBD-SEARCH-1\ngeneration=0\nactive=0\n").error == "bad_line");
	check(parse("PBD-SEARCH-1\ngeneration=1\nactive=0\nactive=0\n").error == "bad_line");
	check(parse("PBD-SEARCH-1\ngeneration=1\nactive=1\nmode=casual\nstarted=5\n").error == "bad_line");
	check(parse("PBD-SEARCH-1\ngeneration=1\nactive=1\nmode=quick\nstarted=5\nopponent=x\n").error == "bad_line");
	// A running search names its mode and start; an ended one names neither.
	check(parse("PBD-SEARCH-1\ngeneration=1\nactive=1\nmode=quick\n").error == "bad_search");
	check(parse("PBD-SEARCH-1\ngeneration=1\nactive=0\nmode=quick\nstarted=5\n").error == "bad_search");
	check(parse("PBD-SEARCH-1\ngeneration=1\nactive=0\n" + std::string(max_file_bytes, '\n')).error == "file_too_large");
}

// placing=<done>|<total> (ranked-notices-1): only a running Ranked search, for a fighter still placing.
PBD_TEST(PbdSearchBadge, RankedSearchNamesThePlacement)
{
	using namespace pbd::search_badge;
	const std::string ranked = "PBD-SEARCH-1\ngeneration=7\nactive=1\nmode=ranked\nstarted=1760000000000\n";
	const auto placing_search = parse(ranked + "placing=2|5\n");
	check(placing_search.value && placing_search.value->placement == placing{2, 5});
	check(!parse(ranked).value->placement && parse(ranked + "placing=0|4\n").value->placement == placing{0, 4});
	check(parse(ranked + "placing=10|10\n").value);
	for (const auto bad : {"6|5", "0|0", "1|11", "2", "2|5|1", "x|5", "2|", "|5"})
		check(parse(ranked + "placing=" + bad + "\n").error == "bad_line", bad);
	check(parse(ranked + "placing=2|5\nplacing=2|5\n").error == "bad_line");
	check(parse("PBD-SEARCH-1\ngeneration=7\nactive=1\nmode=quick\nstarted=5\nplacing=2|5\n").error == "bad_search");
	check(parse("PBD-SEARCH-1\ngeneration=7\nactive=0\nplacing=2|5\n").error == "bad_search");
}

PBD_TEST(PbdSearchBadge, ShowsTheModeAndTimeSearched)
{
	using namespace pbd::search_badge;
	check(elapsed(10000, 10000) == "0:00");
	check(elapsed(10000, 19999) == "0:09");
	check(elapsed(10000, 93000) == "1:23");
	check(elapsed(10000, 10000 + 754000) == "12:34");
	check(elapsed(10000, 5000) == "0:00");
	check(elapsed(0, 1000ull * 60 * 60 * 5) == "99:59");
}

PBD_TEST(PbdSearchBadge, StaysClearOfTheHudAndGivesWayToLauncherPanels)
{
	using namespace pbd::search_badge;
	// In play: right-aligned under the dummy's life bar, above P2's history (first row centred at 166, 22 apart).
	check(placement(false).right_aligned && placement(false).x == 1228.f && placement(false).centre + 12.f <= 166.f - 11.f);
	// Over Practice's pause menu: above its panel (top 112), left-aligned with it.
	check(!placement(true).right_aligned && placement(true).x == 56.f && placement(true).centre + 12.f <= 112.f);
	search running{1, true, search_mode::quick, 5};
	check(shows(running, false));
	check(!shows(running, true));
	check(!shows(search{2, false}, false));
}

PBD_TEST(PbdSearchBadge, PollReadsNewFilesAndKeepsTheNewestGeneration)
{
	using namespace pbd::search_badge;
	const auto dir = std::filesystem::temp_directory_path() / ("pbd-search-badge-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(dir);
	// Each write gets its own write time, as each launcher publish (a new file renamed over the old) does.
	const auto base = std::filesystem::file_time_type::clock::now();
	int writes = 0;
	const auto write = [&](const std::string& text)
	{
		std::ofstream(dir / "search.pbd", std::ios::binary | std::ios::trunc) << text;
		std::filesystem::last_write_time(dir / "search.pbd", base + std::chrono::seconds(++writes));
	};
	reader from;
	from.file = dir / "search.pbd";
	std::chrono::steady_clock::time_point now{};
	publish({});
	check(poll(from, now).empty() && !load().active); // no file yet
	write("PBD-SEARCH-1\ngeneration=5\nactive=1\nmode=ranked\nstarted=42\n");
	check(poll(from, now).empty() && !load().active); // within 250 ms of the last read
	now += std::chrono::milliseconds(250);
	check(poll(from, now).empty() && load().active && load().mode == search_mode::ranked && load().started_ms == 42);
	write("PBD-SEARCH-1\ngeneration=4\nactive=0\n");
	now += std::chrono::milliseconds(250);
	check(poll(from, now).empty() && load().active); // an older generation never replaces a newer one
	write("broken");
	now += std::chrono::milliseconds(250);
	check(poll(from, now) == "bad_header");
	now += std::chrono::milliseconds(250);
	check(poll(from, now).empty() && load().active); // logged once
	write("PBD-SEARCH-1\ngeneration=6\nactive=0\n");
	now += std::chrono::milliseconds(250);
	check(poll(from, now).empty() && !load().active);
	publish({});
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}
