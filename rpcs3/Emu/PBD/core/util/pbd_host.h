#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

// The system this emulator runs on. A Windows build also runs under Wine (Proton, the Steam Deck), which reports itself as
// Windows 10 but answers some calls through its server process, so a call such as GetThreadTimes costs far more there.
// The one test is the export that only Wine's ntdll has: wine_get_version. Real Windows never has it.
namespace pbd::host
{
	enum class kind
	{
		windows,
		wine,
		other, // not a Windows build
	};
	// How a Wine host was recognised.
	enum class evidence
	{
		none,
		exported, // ntdll exports wine_get_version
		install,  // the export is hidden, but HKCU\Software\Wine and system32\winecfg.exe exist
		forced,   // PBD_HOST_FORCE=wine, for test runs on Windows; the launcher never passes it to a player's game
	};
	// What detection finds; the decision is classify().
	struct signs
	{
		bool windows_build = false, wine_export = false, wine_install = false;
		std::string_view force; // PBD_HOST_FORCE
	};
	struct facts
	{
		kind host = kind::other;
		evidence seen = evidence::none;
		// Wine's version, build and host system (wine_get_build_id, wine_get_host_version) as printable(); empty when Wine
		// hides them or on Windows.
		std::string version, build, system, release;
	};

	constexpr std::pair<kind, evidence> classify(const signs& found) noexcept
	{
		if (!found.windows_build)
			return {kind::other, evidence::none};
		if (found.wine_export)
			return {kind::wine, evidence::exported};
		if (found.wine_install)
			return {kind::wine, evidence::install};
		if (found.force == "wine")
			return {kind::wine, evidence::forced};
		return {kind::windows, evidence::none};
	}

	constexpr const char* name(kind value) noexcept
	{
		return value == kind::windows ? "windows" : value == kind::wine ? "wine" :
		                                                                  "other";
	}

	constexpr const char* name(evidence value) noexcept
	{
		return value == evidence::exported ? "export" : value == evidence::install ? "install" :
		                                            value == evidence::forced      ? "forced" :
		                                                                             "none";
	}

	// Text from the host, safe inside a JSON string and a problem report: printable ASCII only, without quotes or backslashes,
	// and with each slash made a space so no part of it reads as a path. At most `limit` characters.
	inline std::string printable(std::string_view text, std::size_t limit = 64)
	{
		std::string out;
		for (const char c : text)
		{
			if (out.size() == limit)
				break;
			if (c == '/')
				out += ' ';
			else if (c >= 0x20 && c <= 0x7e && c != '"' && c != '\\')
				out += c;
		}
		return out;
	}

	// This process's host, worked out once.
	const facts& current() noexcept;

	inline bool wine() noexcept
	{
		return current().host == kind::wine;
	}

	// The --pbd-host-info probe: what the calls the game makes often cost on this host. A value that was not measured is
	// negative.
	struct probe_counts
	{
		std::uint32_t thread_times = 1000, protect = 1000, faults = 200, clock = 10000;
	};
	struct probe_result
	{
		double thread_times_us = -1; // one GetThreadTimes call
		double protect_us = -1;      // one VirtualProtect call on a 4 KiB private page
		double write_fault_us = -1;  // one write to a read-only private page, through a vectored handler that unprotects it
		double clock_ns = -1;        // one steady_clock read
		bool cycle_time = false;     // QueryThreadCycleTime succeeds
	};
	// Runs in its own short process (rpcs3.cpp), never inside a game: it adds a vectored handler for the time it measures.
	probe_result probe(const probe_counts& counts = {}) noexcept;

	// The answer to --pbd-host-info, one line of JSON.
	inline std::string info_json(const facts& found, const probe_result& measured)
	{
		const auto number = [](double value, const char* format)
		{
			char text[32];
			if (value < 0)
				return std::string("null");
			std::snprintf(text, sizeof(text), format, value);
			return std::string(text);
		};
		return std::string("{\"schema\":\"pbd-host-info-1\",\"host\":\"") + name(found.host) + "\",\"seen\":\"" + name(found.seen) +
		       "\",\"wine\":\"" + found.version + "\",\"build\":\"" + found.build + "\",\"system\":\"" + found.system +
		       "\",\"release\":\"" + found.release + "\",\"probe\":{\"thread_times_us\":" + number(measured.thread_times_us, "%.3f") +
		       ",\"cycle_time\":" + (measured.cycle_time ? "true" : "false") + ",\"protect_us\":" + number(measured.protect_us, "%.3f") +
		       ",\"write_fault_us\":" + number(measured.write_fault_us, "%.3f") + ",\"clock_ns\":" + number(measured.clock_ns, "%.1f") + "}}";
	}
} // namespace pbd::host
