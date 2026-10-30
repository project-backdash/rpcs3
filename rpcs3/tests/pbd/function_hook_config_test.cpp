#include "pbd_test.h"

#include "Emu/Cell/pbd_function_hook_config.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/PBD/pbd_json_reader.h"
#include "Emu/PBD/pbd_session_policy.h"
#include "Emu/PBD/pbd_voice_health.h"
#include "Emu/PBD/pbd_function_hook_state.h"
#include "Emu/PBD/pbd_function_hook_event.h"
#include "Emu/PBD/pbd_rank_screens.h"
#include <cstdlib>
#include <fstream>
#include <map>

using pbd::test::check;

#if defined(_WIN32) && GTEST_HAS_DEATH_TEST
PBD_TEST(PbdHookConfigurationDeathTest, MalformedSettingsUseControlledLaunchRefusal)
{
	// Windows exit tests restart this test in a fresh process, before the cached configuration has been read.
	// The inherited directory lets each child publish its refusal where the parent can inspect it.
	const auto inherited = pbd::env::path("PBD_TEST_HOOK_REFUSAL");
	const auto directory = inherited.empty() ? std::filesystem::temp_directory_path() /
	                                               ("pbd-hook-refusal-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) :
	                                           inherited;
	struct restore
	{
		std::filesystem::path directory, inherited;
		~restore()
		{
			pbd::env::set_wide("PBD_TEST_HOOK_REFUSAL", inherited.c_str());
			if (inherited.empty())
				std::filesystem::remove_all(directory);
		}
	} saved{directory, inherited};
	std::filesystem::create_directories(directory);
	check(pbd::env::set_wide("PBD_TEST_HOOK_REFUSAL", directory.c_str()));
	struct setting
	{
		const wchar_t* gates;
		const wchar_t* unless;
		int exit_code;
		const wchar_t* slots = L"";
		const wchar_t* rng_offset = L"";
	};
	for (const auto& value : {setting{L"", L"", 0}, setting{L"unknown", L"", 78},
			 setting{L"player_draw,effects_draw,stage_camera", L"0x2649a8:0x469dbc", 0}, setting{L"", L"0x2649a8", 78},
			 setting{L"", L"", 0, L"0,0x8; fffc", L"0xffffffff"}, setting{L"", L"", 78, L"0,2,8"},
			 setting{L"", L"", 78, L"10000"}, setting{L"", L"", 78, L"8junk"},
			 setting{L"", L"", 78, L"", L"100000000"}, setting{L"", L"", 78, L"", L"0xjunk"}})
	{
		if (inherited.empty())
			std::filesystem::remove(directory / "boot-refusal.json");
		EXPECT_EXIT(
			{
				if (!pbd::env::set_wide("PBD_FN_SKIP_GATE", value.gates) || !pbd::env::set_wide("PBD_FN_SKIP_UNLESS", value.unless) ||
					!pbd::env::set_wide("PBD_FN_DISPATCH_SLOTS", value.slots) || !pbd::env::set_wide("PBD_FN_RNG_OFFSET", value.rng_offset) ||
					!pbd::env::set_wide("PBD_ORDINARY_OUTPUT", directory.c_str()))
					std::_Exit(1);
				if (!pbd::fn_hook::validate_configuration({{0x10000, 0x400000}}))
					pbd::fn_hook::reject_configuration_launch();
				std::_Exit(0);
			},
			::testing::ExitedWithCode(value.exit_code), "");
		if (!inherited.empty())
			continue;
		if (!value.exit_code)
		{
			check(!std::filesystem::exists(directory / "boot-refusal.json"));
			continue;
		}
		std::ifstream refusal(directory / "boot-refusal.json");
		const std::string text{std::istreambuf_iterator<char>(refusal), {}};
		const auto decoded = pbd::json::detail::json_parser(text).parse();
		check(decoded.value);
		std::uint32_t exit_code = 0;
		check(pbd::json::detail::number32(decoded.value->find("exit_code"), 0, 0xffffffffu, exit_code) && exit_code == 78);
		const auto* reason = decoded.value->find("reason");
		check(reason && reason->text == "probe_configuration_rejected");
	}
}
#endif

#ifdef _WIN32
PBD_TEST(PbdHookConfiguration, DirectReplayRetainsRequiredCameraUpdates)
{
	using namespace pbd::fn_hook::detail;
	struct restore_environment
	{
		std::vector<std::pair<std::string, std::wstring>> saved;
		std::vector<std::string> errors = configuration_errors;
		std::vector<std::pair<std::string, std::vector<u32>>> addresses = supplied_addresses;
		~restore_environment()
		{
			for (auto entry = saved.rbegin(); entry != saved.rend(); ++entry)
				pbd::env::set_wide(entry->first, entry->second.c_str());
			configuration_errors = std::move(errors);
			supplied_addresses = std::move(addresses);
		}
		void set(const char* name, const wchar_t* value)
		{
			const auto previous = pbd::env::find_wide(name);
			saved.emplace_back(name, previous ? previous : L"");
			check(pbd::env::set_wide(name, value));
		}
	} environment;
	environment.set("PBD_SESSION_POLICY", L"direct-v1");
	for (const char* name : {"PBD_FN_SKIP", "PBD_FN_SKIPPABLE", "PBD_FN_SKIP_GATE", "PBD_TEST_STAGE_CAMERA_FORCE",
			 "PBD_SET_CHANNEL", "PBD_MATCH_STANDBY_CHANNEL", "PBD_PREPARE_FOR"})
		environment.set(name, L"");

	Emu.SetTestMode();
	vm::init();
	struct close_memory
	{
		~close_memory()
		{
			vm::close();
		}
	} memory;
	constexpr u32 base = 0x10050000, size = 0x10000, camera_mode = 0x10053694, stage_draw = 0x260d44;
	constexpr u32 stage_base = 0x10080000, stage = 0x1008f088;
	check(vm::map(base, size, vm::page_size_64k));
	check(vm::map(stage_base, size, vm::page_size_64k));
	struct construction_id
	{
		u32 previous = std::exchange(id_manager::g_id, ppu_thread::id_base);
		~construction_id()
		{
			id_manager::g_id = previous;
		}
	} identity;
	ppu_thread owner(ppu_thread_params{}, "camera-gate-test", 100);

	// Cold, held and preparation launches use the real configuration without diagnostic skip or gate switches.
	for (const auto& [name, value] : {std::pair{"PBD_SET_CHANNEL", L""}, {"PBD_SET_CHANNEL", L"set"},
			 {"PBD_MATCH_STANDBY_CHANNEL", L"standby"}, {"PBD_PREPARE_FOR", L"lobby"}})
	{
		environment.set(name, value);
		config configured;
		const auto allowed = [&]
		{
			hook_event event{configured, owner, stage_draw, true,
				std::binary_search(configured.skip.begin(), configured.skip.end(), stage_draw)};
			event.camera_gate();
			return event.skip;
		};
		check(!vm::check_addr(camera_mode, vm::page_readable, 4));
		check(!allowed());
		check(vm::falloc(base, size));
		vm::write32(camera_mode, 0);
		check(!allowed(), "An unreadable stage cannot authorize a render-state skip");
		check(vm::falloc(stage_base, size));
		for (const u32 mode : {0u, 1u, 2u, 3u})
		{
			vm::write32(camera_mode, mode);
			for (const u32 selected_stage : {0u, 18u, 32u, 34u})
			{
				vm::write32(stage, selected_stage);
				check(allowed() == (mode != 2 && selected_stage != 34), "Corrections must retain Ominous Mist's fog updates");
			}
		}
		// Live execution always runs the game's own stage layer.
		vm::write32(stage, 34);
		hook_event live{configured, owner, stage_draw, false, false};
		live.camera_gate();
		check(!live.skip);
		check(vm::dealloc(stage_base) == size);
		check(vm::dealloc(base) == size);
		environment.set(name, L"");
	}
}

PBD_TEST(PbdHookConfiguration, ReportsOnlyAfterAllThreadsHaveJoined)
{
	auto& config = pbd::fn_hook::detail::cfg();
	using outcome = pbd::fn_hook::detail::config::report_outcome;
	const auto directory = std::filesystem::temp_directory_path() /
	                       ("pbd-shutdown-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directory(directory);
	struct restore
	{
		pbd::fn_hook::detail::config& config;
		std::vector<u32> probes = config.observed_sites;
		outcome report = config.report.load();
		u64 calls = config.calls;
		std::wstring output = pbd::env::path("PBD_ORDINARY_OUTPUT").native();
		std::filesystem::path directory;
		~restore()
		{
			config.observed_sites = std::move(probes);
			config.report.store(report);
			config.calls = calls;
			pbd::env::set_wide("PBD_ORDINARY_OUTPUT", output.c_str());
			std::filesystem::remove_all(directory);
		}
	} saved{config, config.observed_sites, config.report.load(), config.calls,
		pbd::env::path("PBD_ORDINARY_OUTPUT").native(), directory};
	check(pbd::env::set_wide("PBD_ORDINARY_OUTPUT", directory.c_str()));
	config.observed_sites = {0x15a88};
	config.report.store(outcome::unattempted);
	pbd::rollback_session session;
	std::thread invalidation([&]
		{
			session.invalidate();
		});
	invalidation.join();
	check(config.report.load() == outcome::unattempted);
	std::thread stopping([&]
		{
			session = thread_state::aborting;
		});
	stopping.join();
	session = thread_state::finished;
	check(config.report.load() == outcome::unattempted);
	check(!std::filesystem::exists(directory / "fn-probe-summary.json"));
	// Function probes outside the boundary may finish between the session join and the final CPU join.
	config.calls = 123;
	session = thread_state::destroying_context;
	check(config.report.load() == outcome::succeeded);
	std::ifstream summary(directory / "fn-probe-summary.json");
	const std::string text{std::istreambuf_iterator<char>(summary), {}};
	const auto decoded = pbd::json::detail::json_parser(text).parse();
	check(decoded.value);
	std::uint32_t calls = 0;
	check(pbd::json::detail::number32(decoded.value->find("calls"), 0, 0xffffffffu, calls) && calls == 123);
	config.calls = 456;
	session = thread_state::destroying_context;
	check(config.report.load() == outcome::succeeded);
}
#endif

PBD_TEST(PbdVoiceHealth, DetectsSustainedAllocationLossWithoutClaimLock)
{
	pbd::voice_health::monitor health;
	health.observe(true, 0);
	for (unsigned i = 0; i < 8; ++i)
		health.observe(false, i * 1000000u);
	check(health.successes == 1 && health.failures == 8 && health.exhausted == 1);
	health.observe(false, 8000000);
	check(health.exhausted == 1);
	health.observe(true, 9000000);
	check(health.streak == 0);
	for (unsigned i = 0; i < 8; ++i)
		health.observe(false, 10000000u + i * 1000000u);
	check(health.exhausted == 2 && health.max_streak == 9);
}

PBD_TEST(PbdVoiceHealth, BriefSaturationAndSuccessfulAllocationsDoNotReportExhaustion)
{
	pbd::voice_health::monitor health;
	for (unsigned i = 0; i < 1000; ++i)
	{
		health.observe(false, i * 10000u);
		health.observe(true, i * 10000u);
	}
	check(health.exhausted == 0 && health.max_streak == 1);
	for (unsigned i = 0; i < 8; ++i)
		health.observe(false, 10000000u + i);
	check(health.exhausted == 0);
}

PBD_TEST(PbdHookConfiguration, UnboundWatcherSeed)
{
	using pbd::fn_hook::pbd_stage_sync_seed_missing;
	const auto channel = (std::filesystem::temp_directory_path() / "pbd-spectator-start").string();
	std::map<std::string_view, std::string_view> request{
		{"PBD_SESSION", "1"}, {"PBD_SPECTATOR_STANDBY", "1"}, {"PBD_SPECTATOR_CHANNEL", channel}};
	const auto missing = [&](bool seeded)
	{
		const auto read = [&](const char* key)
		{
			const auto found = request.find(key);
			return found == request.end() ? std::string_view{} : found->second;
		};
		return pbd_stage_sync_seed_missing(seeded, read, [&](const char* key)
			{
				return std::filesystem::path(read(key));
			});
	};
	check(!missing(false));
	for (const auto marker : {"", "0", "2", "true", "01", " 1"})
	{
		request["PBD_SESSION"] = marker;
		check(missing(false));
		request["PBD_SESSION"] = "1";
		request["PBD_SPECTATOR_STANDBY"] = marker;
		check(missing(false));
		request["PBD_SPECTATOR_STANDBY"] = "1";
	}
	for (const auto relative : {"", "channel", "./channel", "../channel"})
	{
		request["PBD_SPECTATOR_CHANNEL"] = relative;
		check(missing(false));
	}
	request.clear();
	check(missing(false));
	check(!missing(true));
}

PBD_TEST(PbdHookConfiguration, UnboundMatchSeed)
{
	const auto channel = (std::filesystem::temp_directory_path() / "pbd-match-start").string();
	std::map<std::string_view, std::string_view> request{
		{"PBD_SESSION", "1"}, {"PBD_PREPARE_FOR", "lobby"}, {"PBD_MATCH_STANDBY_CHANNEL", channel}};
	const auto missing = [&](bool seeded = false)
	{
		const auto read = [&](const char* key)
		{
			const auto found = request.find(key);
			return found == request.end() ? std::string_view{} : found->second;
		};
		return pbd::fn_hook::pbd_stage_sync_seed_missing(seeded, read, [&](const char* key)
			{
				return std::filesystem::path(read(key));
			});
	};
	check(!missing());
	for (const auto marker : {"", "0", "2", "true", "01", " 1", "1 "})
	{
		request["PBD_SESSION"] = marker;
		check(missing());
	}
	request["PBD_SESSION"] = "1";
	for (const auto marker : {"", "Lobby", "practice", "replay", " lobby", "lobby "})
	{
		request["PBD_PREPARE_FOR"] = marker;
		check(missing());
	}
	request["PBD_PREPARE_FOR"] = "lobby";
	for (const auto relative : {"", "channel", "./channel", "../channel", "C:channel"})
	{
		request["PBD_MATCH_STANDBY_CHANNEL"] = relative;
		check(missing());
	}
	request.clear();
	check(missing());
	check(!missing(true));
}

#ifdef _WIN32
PBD_TEST(PbdHookConfiguration, UnboundStandbyPathsRetainNativeText)
{
	struct restore_environment
	{
		std::vector<std::pair<std::string, std::wstring>> saved;
		~restore_environment()
		{
			for (const auto& [name, value] : saved)
				pbd::env::set_wide(name, value.c_str());
		}
		void set(const char* name, const wchar_t* value)
		{
			const auto previous = pbd::env::find_wide(name);
			saved.emplace_back(name, previous ? previous : L"");
			check(pbd::env::set_wide(name, value));
		}
	} environment;
	const auto channel = std::filesystem::temp_directory_path() / L"pbd-\u00dcbung-\u65e5\u672c";
	environment.set("PBD_SESSION", L"1");
	environment.set("PBD_PREPARE_FOR", L"lobby");
	environment.set("PBD_SPECTATOR_STANDBY", L"0");
	environment.set("PBD_MATCH_STANDBY_CHANNEL", channel.c_str());
	environment.set("PBD_SPECTATOR_CHANNEL", channel.c_str());
	const auto read_switch = [](const char* name)
	{
		check(std::string_view(name) != "PBD_MATCH_STANDBY_CHANNEL" && std::string_view(name) != "PBD_SPECTATOR_CHANNEL");
		const auto value = pbd::env::find(name);
		return std::string_view(value ? value : "");
	};
	const auto missing = [&]
	{
		return pbd::fn_hook::pbd_stage_sync_seed_missing(false, read_switch, pbd::env::path);
	};
	check(pbd::env::path("PBD_MATCH_STANDBY_CHANNEL") == channel && !missing());
	check(pbd::env::set_wide("PBD_PREPARE_FOR", L""));
	check(pbd::env::set_wide("PBD_SPECTATOR_STANDBY", L"1"));
	check(pbd::env::path("PBD_SPECTATOR_CHANNEL") == channel && !missing());
	check(pbd::env::set_wide("PBD_SPECTATOR_CHANNEL", L"relative-\u00dcbung"));
	check(missing());
}
#endif

PBD_TEST(PbdHookConfiguration, Checks)
{
	using namespace pbd::fn_hook;
	const auto valid = parse_hook_addresses("probes", "0x15a88,0x52298; 0xcb96c\n0x3afc3c 0x15a88");
	check(valid.errors.empty() && valid.values.size() == 5 && valid.values.back() == 0x15a88);
	for (const auto token : {"336536", "0X52298", "0x", "0x52299", "0x52298junk", "0x100000000", "-0x4", "0x-4", "0x+4"})
	{
		const auto bad = parse_hook_addresses("probes", std::string("0x15a88,") + token + ",0xcb96c");
		check(bad.errors.size() == 1 && bad.values.size() == 2);
	}
	const auto original = parse_hook_addresses("probes", "0x15a88,336536,833900,3865660,951708,420176,1187300,951752,951924,420628,420664,420796,420928,421076,1450500");
	check(original.errors.size() == 14 && original.values.size() == 1);
	const std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges{{0x10000, 0x400000}, {0x500000, 0x501000}};
	check(address_in_code(0x10000, ranges) && address_in_code(0x3ffffc, ranges));
	check(!address_in_code(0, ranges) && !address_in_code(0x400000, ranges));
	check(!address_in_code(0x10001, ranges) && !address_in_code(0xfffffffc, ranges));
	check(!address_in_code(0x4ffffc, ranges) && address_in_code(0x500ffc, ranges));
	check(parse_hook_addresses("probes", "").errors.empty());
}

PBD_TEST(PbdHookConfiguration, NamedSessionPolicyRejectsUnknownModes)
{
	using namespace pbd::session_policy;
	check(parse("") == mode::diagnostic);
	check(parse("direct-v1") == mode::direct);
	for (const auto name : {"direct", "direct-v2", "Direct-v1", " direct-v1", "direct-v1 "})
		check(parse(name) == mode::invalid);
}

PBD_TEST(PbdHookConfiguration, HeldMatchBindsTheSameSeedAsAColdPeerBeforeStageLoading)
{
	using namespace pbd::fn_hook;
	auto& state = detail::random_runtime();
	struct restore
	{
		detail::random_state& target;
		detail::random_state saved = target;
		u32 tick = current_tick.load(), pass = current_pass.load();
		std::vector<u32> controllers = stage_controllers;
		~restore()
		{
			target = std::move(saved);
			current_tick = tick;
			current_pass = pass;
			stage_controllers = std::move(controllers);
		}
	} saved{state};
	state = {};
	current_tick = current_pass = 0;
	stage_controllers.clear();
	check(!bind_match_seed(0) && !state.match_seed_set);
	constexpr u32 seed = 3034615787u;
	check(bind_match_seed(seed) && state.match_seed_set && state.match_seed_base == seed);
	check(bind_match_seed(seed));
	check(!bind_match_seed(seed + 1) && state.match_seed_base == seed);
	check(pbd::stage_sync::screen_seed(state.match_seed_base, state.epoch.match_seeds) ==
		  pbd::stage_sync::screen_seed(seed, 0));
	check(pbd::stage_sync::mob_batch_seed(state.match_seed_base, state.epoch.match_seeds, state.epoch.mobs.open(20)) ==
		  pbd::stage_sync::mob_batch_seed(seed, 0, 1));
	check(!bind_match_seed(seed));
	state.epoch = {};
	state.epoch.match_seeds = 1;
	check(!bind_match_seed(seed));
	state.epoch = {};
	current_tick = 1;
	check(!bind_match_seed(seed));
	current_tick = 0;
	current_pass = 1;
	check(!bind_match_seed(seed));
	current_pass = 0;
	stage_controllers = {0x10000000};
	check(!bind_match_seed(seed));
}

PBD_TEST(PbdHookConfiguration, ReturnedGameBindsItsNextSetLikeAFreshGame)
{
	using namespace pbd::fn_hook;
	auto& state = detail::random_runtime();
	auto& voice = detail::voice_runtime();
	struct restore
	{
		detail::random_state& target;
		detail::voice_state& voice;
		detail::random_state saved = target;
		u32 tick = current_tick.load(), pass = current_pass.load(), ready = gate_ready_tick.load(), hold = gate_hold.load();
		u32 spectator = spectator_vs_init_gate.load(), active = draw_statistics_active.load(), segment = keyed_segment.load();
		u32 detaches = thread_detaches.load();
		u64 in_replay = keyed_in_replay.load();
		bool rematch = rematch_vs_init_gate.load();
		std::vector<u32> controllers = stage_controllers;
		std::map<u32, u32> counts = keyed_counts;
		std::map<std::pair<u32, u32>, std::pair<u32, u64>> draws = draw_statistics;
		u32 activity = voice.activity.load(), owner = voice.owner.load(), foreign = voice.foreign_active;
		u64 sequence = voice.foreign_sequence;
		std::vector<detail::voice_state::marker> events = voice.foreign_events;
		pbd::voice_health::monitor health = voice.health;
		pbd::rank_stakes::game_values armed = pbd::rank_screens::armed();
		u32 gate_node = detail::cfg().gate_node;
		~restore()
		{
			target = std::move(saved);
			current_tick = tick;
			current_pass = pass;
			gate_ready_tick = ready;
			gate_hold = hold;
			spectator_vs_init_gate = spectator;
			draw_statistics_active = active;
			keyed_segment = segment;
			thread_detaches = detaches;
			keyed_in_replay = in_replay;
			rematch_vs_init_gate = rematch;
			stage_controllers = std::move(controllers);
			keyed_counts = std::move(counts);
			draw_statistics = std::move(draws);
			voice.activity = activity;
			voice.owner = owner;
			voice.foreign_active = foreign;
			voice.foreign_sequence = sequence;
			voice.foreign_events = std::move(events);
			voice.health = health;
			pbd::rank_screens::arm(armed, {0u, 0u});
			detail::cfg().gate_node = gate_node;
		}
	} saved{state, voice};
	// Settings read at process start.
	state = {};
	state.rng_repair = 0x9dc04;
	state.rng_exit = 0x9dc80;
	state.rng_offset = 0x6000;
	state.fence = {{0x9dc04, 0x9dc80}};
	state.stage_sync = true;
	state.draw_diagnostics = true;
	state.keyed_draws = true;
	state.rng_audit = {0x15a88};
	// Populate per-game state, including an outstanding voice claim, before the resident reset.
	current_tick = current_pass = 0;
	stage_controllers.clear();
	constexpr u32 first = 1060522137u, next = 4155720893u;
	check(bind_match_seed(first));
	state.epoch.match_seeds = 1;
	state.epoch.mobs.open(20);
	current_tick = 12936;
	current_pass = 1;
	stage_controllers = {0x10000000};
	gate_ready_tick = 12000;
	gate_hold = 1;
	spectator_vs_init_gate = 2;
	rematch_vs_init_gate = true;
	draw_statistics_active = 0;
	keyed_segment = 12;
	keyed_counts = {{0x121de4, 3}};
	keyed_in_replay = 1;
	thread_detaches = 4;
	draw_statistics = {{{0x3afc3c, 0x121de4}, {6, 0x1234}}};
	voice.activity = 1;
	voice.owner = 0x01000003;
	voice.foreign_active = 1;
	voice.foreign_sequence = 9;
	voice.foreign_events = {{9, 0x01000003, 12936, 0, 1, 2}};
	voice.health.observe(false, 0);
	// Armed rank screens and an exit-gate node belong to the stopped guest.
	pbd::rank_screens::arm({{2, 1}, {-1, 6}}, {5u, 7u});
	detail::cfg().gate_node = 0x10a00000;
	check(!bind_match_seed(next));

	check(reset_for_next_game());
	check(state.rng_repair == 0x9dc04 && state.rng_exit == 0x9dc80 && state.rng_offset == 0x6000);
	check(state.fence == std::vector<std::pair<u32, u32>>{{0x9dc04, 0x9dc80}} && state.rng_audit == std::vector<u32>{0x15a88});
	check(state.stage_sync && state.draw_diagnostics && state.keyed_draws);
	check(!state.match_seed_set && !state.direct_match_epoch_applied && !state.epoch.match_seeds && !state.epoch.mobs.count());
	check(!current_tick && !current_pass && !gate_ready_tick && !gate_hold && !spectator_vs_init_gate && !rematch_vs_init_gate);
	check(stage_controllers.empty() && keyed_counts.empty() && draw_statistics.empty() && draw_statistics_active == 1);
	check(!keyed_segment && !keyed_in_replay && !thread_detaches);
	check(!voice.activity && !voice.owner && !voice.foreign_active && !voice.foreign_sequence && voice.foreign_events.empty());
	check(!voice.health.failures && !voice.health.streak);
	// Reset leaves rank screens unarmed and drops the old guest's exit-gate node.
	check(pbd::rank_screens::armed() == pbd::rank_stakes::game_values{} && !detail::cfg().gate_node);
	// The next seed starts from the same epoch as a fresh peer.
	check(bind_match_seed(next) && state.match_seed_base == next);
	check(pbd::stage_sync::screen_seed(state.match_seed_base, state.epoch.match_seeds) == pbd::stage_sync::screen_seed(next, 0));
}

PBD_TEST(PbdHookConfiguration, DirectModeSnapshotsTheLaunchPolicy)
{
	using namespace pbd::session_policy;
	std::map<std::string, std::string> fields{{"PBD_SESSION_POLICY", "direct-v1"}, {"PBD_SET_CHANNEL", "set"}};
	const auto load = [&]
	{
		return read([&](const char* key) -> const char*
			{
				const auto found = fields.find(key);
				return found == fields.end() ? nullptr : found->second.c_str();
			});
	};
	const auto admitted = load();
	check(admitted.direct() && admitted.stage_sync && admitted.exit_gate);
	fields["PBD_SESSION_STAGE_SYNC"] = fields["PBD_SESSION_EXIT_GATE"] = "0";
	const auto next = load();
	check(next.direct() && !next.stage_sync && !next.exit_gate);
	check(admitted.stage_sync && admitted.exit_gate);
	fields.clear();
	check(!load().direct());
}

PBD_TEST(PbdHookConfiguration, EffectiveDirectRecipeIgnoresDiagnosticOverrides)
{
	using namespace pbd::session_policy;
	for (const auto* text : {L"1", L"16", L"0", L"-1", L"9junk", L"4294967296"})
	{
		check(recipe_number(true, text, ring_size, 1, 16) == ring_size);
		check(recipe_number(true, text, gate_lag, 1, 60) == gate_lag);
	}
	check(recipe_number(false, L"4", ring_size, 1, 16) == 4);
	check(!recipe_number(false, L"4junk", ring_size, 1, 16));
	check(!recipe_number(false, L"17", ring_size, 1, 16));
}
