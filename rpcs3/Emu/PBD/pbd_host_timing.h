#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

// The host_timing rows of tick-correction.jsonl, written only under Wine (pbd_host.h). Each row covers the stretch since the
// previous one: every 600 live ticks, and the rest at the session's end. It says how fast live ticks ran and what the
// write-fault capture and the timed scopes cost meanwhile, so one problem report from a Proton PC shows whether the game kept
// 60 ticks a second. The type name does not start with "correction", so the launcher's correction readers skip these rows.
// Std-only, so a standalone test compiles it.
namespace pbd::host_timing
{
	inline constexpr std::uint32_t row_ticks = 600;
	// Live tick times are kept in 0.5 ms bins; the last bin also holds every tick of 100 ms or more.
	inline constexpr std::uint64_t bin_ns = 500'000;
	inline constexpr std::size_t bins = 200;

	// The session's cumulative counters at one moment. A row reports how much each grew since the previous row.
	struct totals
	{
		std::uint64_t replayed_ticks = 0, corrections = 0, hold_ns = 0, faults = 0, fault_handler_ns = 0, arm_calls = 0,
					  arm_ns = 0, ring_ns = 0, scopes = 0, cpu_scopes = 0, points = 0;
	};

	// The host facts every row repeats, because a report carries only the end of launcher.log. Printable text (pbd_host.h).
	struct host_text
	{
		std::string_view wine, system, release, seen;
	};

	// The time below which `fraction` of the counted ticks fell: the upper edge of that bin, in ms.
	inline double percentile_ms(const std::array<std::uint32_t, bins>& histogram, std::uint64_t count, double fraction) noexcept
	{
		if (!count)
			return 0;
		const auto wanted = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(count))));
		std::uint64_t seen = 0;
		for (std::size_t bin = 0; bin < bins; bin++)
		{
			seen += histogram[bin];
			if (seen >= wanted)
				return static_cast<double>((bin + 1) * bin_ns) / 1e6;
		}
		return static_cast<double>(bins * bin_ns) / 1e6;
	}

	class recorder
	{
		std::array<std::uint32_t, bins> histogram{};
		std::uint64_t ticks = 0, begin_ns = 0, last_ns = 0;
		totals before{};
		bool started = false;

	public:
		// A live tick completed at `now_ns`, with the session's counters `now`. The first one only starts the stretch. True when
		// a row is due.
		bool live(std::uint64_t now_ns, const totals& now) noexcept
		{
			if (!started)
			{
				started = true;
				begin_ns = last_ns = now_ns;
				before = now;
				return false;
			}
			histogram[std::min<std::uint64_t>((now_ns - last_ns) / bin_ns, bins - 1)]++;
			last_ns = now_ns;
			return ++ticks >= row_ticks;
		}

		bool pending() const noexcept
		{
			return ticks != 0;
		}

		// Replaces `text` with the row for the stretch up to `now_ns`, through boundary `through`; the next stretch starts
		// here. The caller's string is reused, so a row allocates nothing once the string has held one.
		void row(std::string& text, std::uint32_t through, bool end, std::uint64_t now_ns, const totals& now, const host_text& host)
		{
			const auto grew = [](std::uint64_t later, std::uint64_t earlier)
			{
				return static_cast<unsigned long long>(later >= earlier ? later - earlier : 0);
			};
			const double wall_ms = static_cast<double>(now_ns - begin_ns) / 1e6;
			char numbers[640];
			std::snprintf(numbers, sizeof(numbers),
				"{\"type\":\"host_timing\",\"through\":%u,\"end\":%s,\"live_ticks\":%llu,\"replayed_ticks\":%llu,\"corrections\":%llu,"
				"\"wall_ms\":%.1f,\"live_ticks_per_s\":%.2f,\"live_tick_ms_p50\":%.1f,\"live_tick_ms_p95\":%.1f,\"hold_ms\":%.1f,"
				"\"faults\":%llu,\"fault_handler_ns\":%llu,\"arm_calls\":%llu,\"arm_ns\":%llu,\"ring_ns\":%llu,"
				"\"scopes\":%llu,\"cpu_scopes\":%llu,\"points\":%llu,",
				through, end ? "true" : "false", static_cast<unsigned long long>(ticks), grew(now.replayed_ticks, before.replayed_ticks),
				grew(now.corrections, before.corrections), wall_ms, wall_ms > 0 ? static_cast<double>(ticks) * 1000.0 / wall_ms : 0.0,
				percentile_ms(histogram, ticks, 0.5), percentile_ms(histogram, ticks, 0.95),
				static_cast<double>(grew(now.hold_ns, before.hold_ns)) / 1e6, grew(now.faults, before.faults),
				grew(now.fault_handler_ns, before.fault_handler_ns), grew(now.arm_calls, before.arm_calls), grew(now.arm_ns, before.arm_ns),
				grew(now.ring_ns, before.ring_ns), grew(now.scopes, before.scopes), grew(now.cpu_scopes, before.cpu_scopes),
				grew(now.points, before.points));
			text = numbers;
			text += "\"wine\":\"";
			text += host.wine;
			text += "\",\"system\":\"";
			text += host.system;
			text += "\",\"release\":\"";
			text += host.release;
			text += "\",\"seen\":\"";
			text += host.seen;
			text += "\"}\n";
			histogram.fill(0);
			ticks = 0;
			begin_ns = now_ns;
			before = now;
		}
	};

	// The session's side, kept by the boundary owner. Off unless the emulator runs under Wine.
	struct state
	{
		bool enabled = false, ended = false;
		std::uint64_t replayed_ticks = 0, ring_ns = 0;
		recorder stretch;
	};
} // namespace pbd::host_timing
