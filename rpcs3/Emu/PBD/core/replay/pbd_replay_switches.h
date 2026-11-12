#pragma once

#include <array>
#include <string_view>

// A replay a player watches plays its file and nothing else. These switches paste saved memory into the game, dump or
// snapshot it, probe its pages or change its timing for a test, so a replay refuses to start while one is set unless
// its run opts in (pbd_anchor_dump.cpp replay_refuses_test_switches). The launcher and tools/replay_watch.py pass none
// of them.
namespace pbd::replay_switches
{
	inline constexpr std::string_view opt_in = "PBD_TEST_REPLAY_DIAGNOSTICS";

	// Every test switch the emulator reads, and the dumps and probes without a PBD_TEST_ name, each also read under its
	// older name (pbd_env.h). A test replay's own file, PBD_TEST_REPLAY_V2_DIRECT_GAME, is not one. A new PBD_TEST_
	// switch belongs here.
	inline constexpr std::array<std::string_view, 19> refused{
		"PBD_TEST_ANCHOR_PASTE",
		"PBD_ANCHOR_DUMP",
		"PBD_SESSION_MATCH_SNAPSHOT",
		"PBD_SESSION_DUMP_MATCH",
		"PBD_TICK_PROBE_PAGES",
		"PBD_TICK_PROBE_OUTPUT",
		"PBD_TEST_REPLAY_V2_RECORD_SOURCE",
		"PBD_TEST_STEER_STAGE",
		"PBD_TEST_STAGE_CAMERA_FORCE",
		"PBD_TEST_EARLY_FLUSH",
		"PBD_TEST_SKIP_GUARD_TAINT",
		"PBD_TEST_SKIP_GUARD_MISSING",
		"PBD_TEST_VOICE_CLAIM_DELAY_US",
		"PBD_TEST_VOICE_RENDEZVOUS_US",
		"PBD_TEST_VOICE_RENDEZVOUS_TICK",
		"PBD_TEST_VOICE_DROP_MAIN_STOP",
		"PBD_TEST_FLIP_RESET_DELAY_US",
		"PBD_TEST_RANK_STAKE",
		"PBD_TEST_RANK_PROMOTE",
	};

	// The first refused switch with a non-empty value, or nullptr when there is none or the run opted in (exactly "1").
	// `find` looks a PBD_ name up as pbd::env::find does.
	template <typename Find>
	const char* first_refused(Find&& find) noexcept
	{
		if (const char* in = find(opt_in); in && std::string_view(in) == "1")
			return nullptr;
		for (const std::string_view name : refused)
			if (const char* value = find(name); value && *value)
				return name.data();
		return nullptr;
	}
} // namespace pbd::replay_switches
