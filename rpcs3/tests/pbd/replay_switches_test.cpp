#include "pbd_test.h"

#include "Emu/PBD/pbd_env.h"
#include "Emu/PBD/pbd_replay_switches.h"

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace
{
	using ::pbd::test::check;
	namespace switches = ::pbd::replay_switches;

	// A process environment held in a map, looked up the way pbd::env::find is.
	struct environment
	{
		std::map<std::string, std::string, std::less<>> values;

		const char* operator()(std::string_view name) const
		{
			const auto found = values.find(name);
			return found == values.end() ? nullptr : found->second.c_str();
		}
	};

	bool names(const char* found, std::string_view expected)
	{
		return found && std::string_view(found) == expected;
	}

	// Sets a variable for this process, or removes it when value is null.
	void put(const char* name, const char* value)
	{
#ifdef _WIN32
		_putenv_s(name, value ? value : "");
#else
		if (value)
			setenv(name, value, 1);
		else
			unsetenv(name);
#endif
	}

} // namespace

PBD_TEST(PbdReplaySwitches, ListChecks)
{
	std::set<std::string_view> seen;
	bool prefixed = true;
	for (const auto name : switches::refused)
	{
		prefixed = prefixed && name.starts_with("PBD_");
		seen.insert(name);
	}
	check(prefixed, "a refused switch is not named with PBD_, the only spelling the emulator reads");
	check(seen.size() == switches::refused.size(), "a switch is listed twice");
	check(!seen.contains(switches::opt_in), "the opt-in refuses its own replay");
}

PBD_TEST(PbdReplaySwitches, RefusalChecks)
{
	check(!switches::first_refused(environment{}), "an empty environment refuses a replay");
	bool each = true;
	for (const auto name : switches::refused)
		each = each && names(switches::first_refused(environment{{{std::string(name), "1"}}}), name);
	check(each, "a test switch set alone is not the one named");
	check(!switches::first_refused(environment{{{"PBD_ANCHOR_DUMP", ""}}}), "a switch set empty is off, so it refuses nothing");
	check(names(switches::first_refused(environment{{{"PBD_TICK_PROBE_OUTPUT", "out"}, {"PBD_TEST_ANCHOR_PASTE", "dump;0-1"}}}),
			  "PBD_TEST_ANCHOR_PASTE"),
		"the first switch in the list is not the one named");
	for (const auto release : {"PBD_REPLAY_V2_GAME", "PBD_TEST_REPLAY_V2_DIRECT_GAME", "PBD_TICK_TRACE",
			 "PBD_FIXTURE_SCENES", "PBD_LDD_PADS", "PBD_SESSION", "PBD_UPDATE_TAPE"})
		check(!switches::first_refused(environment{{{release, "1"}}}), "a switch that is not a test switch refuses a replay");
}

PBD_TEST(PbdReplaySwitches, OptInChecks)
{
	const std::string opt_in(switches::opt_in);
	check(!switches::first_refused(environment{{{"PBD_ANCHOR_DUMP", "dumps"}, {opt_in, "1"}}}), "a test replay that opts in is refused");
	check(names(switches::first_refused(environment{{{"PBD_ANCHOR_DUMP", "dumps"}, {opt_in, "0"}}}), "PBD_ANCHOR_DUMP"),
		"an opt-in of 0 lets a test switch through");
	check(names(switches::first_refused(environment{{{"PBD_ANCHOR_DUMP", "dumps"}, {opt_in, ""}}}), "PBD_ANCHOR_DUMP"),
		"an empty opt-in lets a test switch through");
	check(!switches::first_refused(environment{{{opt_in, "1"}}}), "the opt-in alone refuses a replay");
}

PBD_TEST(PbdReplaySwitches, ProcessChecks)
{
	put("PBD_TEST_STEER_STAGE", "1");
	const bool refused = switches::first_refused(::pbd::env::find) != nullptr;
	put("PBD_TEST_REPLAY_DIAGNOSTICS", "1");
	const bool opted_in = switches::first_refused(::pbd::env::find) == nullptr;
	put("PBD_TEST_REPLAY_DIAGNOSTICS", nullptr);
	put("PBD_TEST_STEER_STAGE", nullptr);
	check(refused, "a test switch set in the process is not refused");
	check(opted_in, "the process opt-in is not read");
}
