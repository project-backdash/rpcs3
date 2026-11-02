#include "pbd_test.h"

#include "Emu/PBD/pbd_host.h"
#include "Emu/PBD/pbd_host_timing.h"
#include "Emu/PBD/pbd_stall_metrics.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
	using pbd::host::evidence;
	using pbd::host::kind;
	using pbd::test::check;

	// The number after "\"name\":" in a row, or a negative value when the field is missing.
	double field(const std::string& row, std::string_view name)
	{
		const auto key = "\"" + std::string(name) + "\":";
		const auto at = row.find(key);
		return at == std::string::npos ? -1 : std::strtod(row.c_str() + at + key.size(), nullptr);
	}

	bool has(const std::string& text, std::string_view part)
	{
		return text.find(part) != std::string::npos;
	}

	constexpr std::uint64_t ms(double value)
	{
		return static_cast<std::uint64_t>(value * 1e6);
	}
} // namespace

PBD_TEST(PbdHost, OnlyWineSignsMakeAWineHost)
{
	check(pbd::host::classify({}) == std::pair{kind::other, evidence::none}, "A build that is not for Windows is neither");
	check(pbd::host::classify({true}) == std::pair{kind::windows, evidence::none}, "No sign of Wine is Windows");
	check(pbd::host::classify({true, true}) == std::pair{kind::wine, evidence::exported}, "The ntdll export is Wine");
	check(pbd::host::classify({true, false, true}) == std::pair{kind::wine, evidence::install}, "A hidden export with Wine's key and tool is Wine");
	check(pbd::host::classify({true, true, false, "wine"}) == std::pair{kind::wine, evidence::exported}, "A real export wins over the test switch");
	check(pbd::host::classify({true, false, false, "wine"}) == std::pair{kind::wine, evidence::forced}, "The test switch makes Windows act as Wine");
	check(pbd::host::classify({true, false, false, "1"}) == std::pair{kind::windows, evidence::none}, "Only the word wine forces it");
	check(pbd::host::classify({false, true, true, "wine"}) == std::pair{kind::other, evidence::none}, "Nothing makes a non-Windows build Wine");
}

PBD_TEST(PbdHost, HostTextIsSafeInJsonAndReports)
{
	check(pbd::host::printable("wine-9.0 (Proton-9.0-3)") == "wine-9.0 (Proton-9.0-3)", "Ordinary version text is kept");
	check(pbd::host::printable("a\"b\\c\nd\x7f") == "abcd", "Quotes, backslashes and control bytes are dropped");
	check(pbd::host::printable("/usr/lib/wine") == " usr lib wine", "No slash survives to read as a path");
	check(pbd::host::printable(std::string(100, 'x')).size() == 64, "Text is cut at its limit");
	check(pbd::host::printable("6.1.52-valve16", 5) == "6.1.5", "A shorter limit applies");
}

PBD_TEST(PbdHost, InfoAnswerIsOneJsonLine)
{
	pbd::host::facts found;
	found.host = kind::wine;
	found.seen = evidence::exported;
	found.version = "9.0";
	found.build = "wine-9.0";
	found.system = "Linux";
	found.release = "6.5.0-valve";
	pbd::host::probe_result measured;
	measured.thread_times_us = 12.5;
	measured.cycle_time = true;
	measured.write_fault_us = 9.25;
	const auto text = pbd::host::info_json(found, measured);
	check(text == "{\"schema\":\"pbd-host-info-1\",\"host\":\"wine\",\"seen\":\"export\",\"wine\":\"9.0\",\"build\":\"wine-9.0\",\"system\":\"Linux\","
				  "\"release\":\"6.5.0-valve\",\"probe\":{\"thread_times_us\":12.500,\"cycle_time\":true,\"protect_us\":null,"
				  "\"write_fault_us\":9.250,\"clock_ns\":null}}",
		text);
}

#ifdef _WIN32
PBD_TEST(PbdHost, ThisProcessMatchesItsNtdll)
{
	const auto exported = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
	const auto& found = pbd::host::current();
	check(&found == &pbd::host::current(), "The host is worked out once");
	if (exported)
		check(found.host == kind::wine && found.seen == evidence::exported, "Under Wine the export decides");
	else
		check(found.host == kind::windows || found.seen == evidence::forced || found.seen == evidence::install, "Windows without a sign of Wine is Windows");
	check(pbd::host::wine() == (found.host == kind::wine), "wine() follows the host");
}

PBD_TEST(PbdHost, ProbeMeasuresEveryCallAndKeepsRunning)
{
	pbd::host::probe_counts counts;
	counts.thread_times = 50;
	counts.protect = 50;
	counts.faults = 20;
	counts.clock = 100;
	const auto measured = pbd::host::probe(counts);
	check(measured.thread_times_us >= 0, "GetThreadTimes is timed");
	check(measured.cycle_time, "QueryThreadCycleTime answers");
	check(measured.protect_us >= 0, "VirtualProtect is timed");
	check(measured.write_fault_us >= 0, "Every probe write faults once and continues");
	check(measured.clock_ns >= 0, "The clock is timed");
	// The handler is gone: a second probe adds its own and measures again.
	check(pbd::host::probe(counts).write_fault_us >= 0, "A second probe faults and continues too");
}
#endif

PBD_TEST(PbdHost, ForcedWineScopesCountWithoutAClock)
{
	const bool forced = pbd::host::classify({true, false, false, "wine"}).first == kind::wine;
	pbd::stall_metrics::host_counter counter(forced);
	check(counter.counted(pbd::stall_metrics::sampling::cpu), "A CPU scope returns at once");
	check(counter.counted(pbd::stall_metrics::sampling::cpu));
	check(counter.counted(pbd::stall_metrics::sampling::elapsed), "An elapsed scope returns at once");
	check(counter.counted_point(), "A point returns at once");
	const auto counts = counter.read();
	check(counts.scopes == 3 && counts.cpu_scopes == 2 && counts.points == 1, "Scopes, CPU scopes and points are counted apart");

	pbd::stall_metrics::host_counter windows(false);
	check(!windows.counted(pbd::stall_metrics::sampling::cpu) && !windows.counted_point(), "On Windows every scope is measured");
	const auto none = windows.read();
	check(none.scopes == 0 && none.cpu_scopes == 0 && none.points == 0, "On Windows nothing is counted");
}

PBD_TEST(PbdHostTiming, RowEvery600LiveTicks)
{
	pbd::host_timing::recorder stretch;
	pbd::host_timing::totals now;
	std::uint64_t at = ms(1000);
	check(!stretch.live(at, now), "The first live tick only starts the stretch");
	check(!stretch.pending());
	for (std::uint32_t tick = 1; tick < pbd::host_timing::row_ticks; tick++)
	{
		at += ms(16.68);
		now.faults += 160;
		now.cpu_scopes += 100;
		now.scopes += 150;
		check(!stretch.live(at, now), "No row before 600 live ticks");
	}
	at += ms(16.68);
	now.replayed_ticks = 30;
	now.corrections = 6;
	now.hold_ns = ms(2.5);
	check(stretch.live(at, now), "The 600th live tick asks for a row");
	std::string row;
	stretch.row(row, 1234, false, at, now, {"9.0", "Linux", "6.5.0", "export"});
	check(row.starts_with("{\"type\":\"host_timing\",") && row.ends_with("}\n"), row);
	check(field(row, "through") == 1234 && field(row, "live_ticks") == 600, row);
	check(field(row, "live_ticks_per_s") > 59.9 && field(row, "live_ticks_per_s") < 60.0, row);
	check(field(row, "live_tick_ms_p50") == 17.0 && field(row, "live_tick_ms_p95") == 17.0, row);
	check(field(row, "wall_ms") > 10007 && field(row, "wall_ms") < 10009, row);
	check(field(row, "replayed_ticks") == 30 && field(row, "corrections") == 6 && field(row, "hold_ms") == 2.5, row);
	check(field(row, "faults") == 599 * 160 && field(row, "cpu_scopes") == 599 * 100 && field(row, "scopes") == 599 * 150, row);
	check(has(row, "\"end\":false") && has(row, "\"wine\":\"9.0\",\"system\":\"Linux\",\"release\":\"6.5.0\",\"seen\":\"export\""), row);
	check(!stretch.pending(), "A row starts the next stretch");
}

PBD_TEST(PbdHostTiming, SlowTicksShowInP95AndRowsReportGrowthOnly)
{
	pbd::host_timing::recorder stretch;
	pbd::host_timing::totals now{100, 10, 0, 5000};
	std::uint64_t at = 0;
	stretch.live(at, now);
	for (std::uint32_t tick = 1; tick <= 100; tick++)
	{
		at += tick % 10 == 0 ? ms(33.4) : ms(16.6);
		stretch.live(at, now);
	}
	now.replayed_ticks += 12;
	now.faults += 800;
	std::string row;
	stretch.row(row, 101, true, at + ms(5), now, {"", "", "", "forced"});
	check(field(row, "live_ticks") == 100 && field(row, "replayed_ticks") == 12 && field(row, "faults") == 800, row);
	check(field(row, "live_tick_ms_p50") == 17.0, row);
	check(field(row, "live_tick_ms_p95") == 33.5, "One tick in ten at 33 ms is the 95th percentile");
	check(has(row, "\"end\":true") && has(row, "\"seen\":\"forced\""), row);

	at += ms(16.6);
	stretch.live(at, now);
	std::string next;
	stretch.row(next, 102, true, at, now, {"", "", "", "forced"});
	check(field(next, "replayed_ticks") == 0 && field(next, "faults") == 0, "The next row counts from the previous one");
}

PBD_TEST(PbdHostTiming, VeryLongTicksLandInTheLastBin)
{
	std::array<std::uint32_t, pbd::host_timing::bins> histogram{};
	histogram[pbd::host_timing::bins - 1] = 1;
	check(pbd::host_timing::percentile_ms(histogram, 1, 0.95) == 100.0, "A tick of 100 ms or more reads as 100");
	check(pbd::host_timing::percentile_ms(histogram, 0, 0.95) == 0, "No ticks read as zero");
}
