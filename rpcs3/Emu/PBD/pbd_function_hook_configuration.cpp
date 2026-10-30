#include "stdafx.h"
#include "pbd_function_hook_state.h"
#include "pbd_session_policy.h"
#include "pbd_env_parse.h"
#include "Emu/pbd_session_mode.h"
#include <charconv>

namespace pbd::fn_hook::detail
{
	std::vector<std::string> configuration_errors;
	std::string env_text(const char* name)
	{
		std::string text;
#ifdef _WIN32
		if (const auto value = pbd::env::find_wide(name); value && *value)
		{
			const std::wstring_view wide(value);
			for (const wchar_t c : wide)
			{
				if (c > 127)
				{
					configuration_errors.emplace_back(std::string(name) + ": expected ASCII configuration text");
					return {};
				}
				text.push_back(static_cast<char>(c));
			}
		}
#else
		if (const auto value = pbd::env::find(name); value && *value)
			text = value;
#endif
		return text;
	}

	std::vector<std::pair<std::string, std::vector<u32>>> supplied_addresses;

	std::vector<u32> parse_raw(const char* name)
	{
		auto parsed = parse_hook_addresses(name, env_text(name));
		configuration_errors.insert(configuration_errors.end(), parsed.errors.begin(), parsed.errors.end());
		supplied_addresses.emplace_back(name, parsed.values);
		return parsed.values;
	}

	std::vector<u32> parse_list(const char* name)
	{
		auto out = parse_raw(name);
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}

	// Dispatch slot byte offsets are scalars, not code addresses; retain their documented hex syntax.
	std::vector<u32> parse_slots(const char* name)
	{
		std::vector<u32> out;
		const auto text = env_text(name);
		for (usz pos = text.find_first_not_of(",; "); pos != text.npos;)
		{
			const auto end = text.find_first_of(",; ", pos);
			const auto token = std::string_view(text).substr(pos, end == text.npos ? end : end - pos);
			u64 value = 0;
			if (!pbd::env::parse_unsigned(token, value, 0xfffc, 16) || value % 4)
				configuration_errors.emplace_back(std::string(name) + ": invalid aligned slot offset");
			else
				out.push_back(static_cast<u32>(value));
			pos = end == text.npos ? end : text.find_first_not_of(",; ", end);
		}
		return out;
	}

	u64 parse_number(const char* name, u64 fallback)
	{
		const std::string text = env_text(name);
		if (text.empty())
			return fallback;
		const bool hex = text.starts_with("0x");
		const auto digits = std::string_view(text).substr(hex ? 2 : 0);
		u64 value = 0;
		const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
		if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size())
		{
			configuration_errors.emplace_back(std::string(name) + ": invalid unsigned number");
			return fallback;
		}
		return value;
	}

	void config::configure_observed_sites()
	{
		observed_sites = parse_list("PBD_FN_PROBES");
		if (pbd::session_mode::offline_play())
			observed_sites.push_back(pbd::t5dr::boundary_pc);
		if (launch.selected == pbd::session_policy::mode::invalid)
			configuration_errors.emplace_back("PBD_SESSION_POLICY: unknown policy");
		if (launch.direct() || env_text("PBD_BATTLE_CLOCK") == "2")
			observed_sites.push_back(pbd::t5dr::gpu_load_probe);
		if (launch.direct())
		{
			observed_sites.push_back(pbd::session_policy::reseed_probe);
			if (launch.stage_sync)
				observed_sites.insert(observed_sites.end(), pbd::session_policy::stage_probes.begin(), pbd::session_policy::stage_probes.end());
			if (launch.exit_gate)
				observed_sites.insert(observed_sites.end(), pbd::session_policy::gate_probes.begin(), pbd::session_policy::gate_probes.end());
		}
		if (pbd_live_replay::requested())
			observed_sites.insert(observed_sites.end(), {0x159f0u, 0x5a554u});
		if (pbd_direct_match::requested())
			observed_sites.insert(observed_sites.end(), {0x159f0u, 0x5a554u, 0x53ee4u});
#ifdef PBD_DIAGNOSTIC_BUILD
		steer_stage = static_cast<u32>(parse_number("PBD_TEST_STEER_STAGE", 0));
		if (steer_stage)
			observed_sites.push_back(0xb0cf0);
		voice_runtime().test_delay_us = std::min<u64>(parse_number("PBD_TEST_VOICE_CLAIM_DELAY_US", 0), 1000000);
		voice_runtime().test_rendezvous_us = std::min<u64>(parse_number("PBD_TEST_VOICE_RENDEZVOUS_US", 0), 2000000);
		voice_runtime().test_rendezvous_tick = static_cast<u32>(parse_number("PBD_TEST_VOICE_RENDEZVOUS_TICK", 0));

#endif
		voice_runtime().lock = parse_number("PBD_VOICE_CLAIM_LOCK", 0) == 1;
		voice_runtime().timeout_us = std::clamp<u64>(parse_number("PBD_VOICE_CLAIM_TIMEOUT_US", 250000), 100, 2000000);
		voice_runtime().fatal_timeout = parse_number("PBD_VOICE_FATAL_TIMEOUT", 0) == 1;
#ifdef PBD_DIAGNOSTIC_BUILD
		voice_runtime().test_drop_main_stop = parse_number("PBD_TEST_VOICE_DROP_MAIN_STOP", 0) == 1;

#endif
		voice_runtime().fatal_deadline_ms = std::clamp<u64>(parse_number("PBD_VOICE_FATAL_DEADLINE_MS", 2000), 100, 10000);
		if (voice_runtime().fatal_timeout && !voice_runtime().lock)
			configuration_errors.emplace_back("PBD_VOICE_FATAL_TIMEOUT requires PBD_VOICE_CLAIM_LOCK=1");
#ifdef PBD_DIAGNOSTIC_BUILD
		if (voice_runtime().test_drop_main_stop && !voice_runtime().fatal_timeout)
			configuration_errors.emplace_back("PBD_TEST_VOICE_DROP_MAIN_STOP requires PBD_VOICE_FATAL_TIMEOUT=1");
#endif
		// 0x3428b8 is reached only after the byte at voice+0x40 was read as free.
		// Allocation health remains observable when the optional claim lock is disabled.
		observed_sites.insert(observed_sites.end(), {0x342044u, 0x3420d8u, 0x342104u, 0x3428b8u});
		observed_sites.push_back(pbd::pick_transition::hook_pc);
		if (const auto error = pbd::rank_screens::configure(); !error.empty())
			configuration_errors.push_back(error);
		if (pbd::rank_screens::configured().enabled())
			observed_sites.insert(observed_sites.end(), {pbd::rank_screens::after_battle_setup, pbd::rank_screens::after_match_bookkeeping});
		std::sort(observed_sites.begin(), observed_sites.end());
		observed_sites.erase(std::unique(observed_sites.begin(), observed_sites.end()), observed_sites.end());
	}

	void config::configure_skippable()
	{
		skippable = parse_list("PBD_FN_SKIPPABLE");
		const auto replay = pbd::session_policy::replay_presentation_of(launch);
		skippable.insert(skippable.end(), replay.skips.begin(), replay.skips.end());
		std::sort(skippable.begin(), skippable.end());
		skippable.erase(std::unique(skippable.begin(), skippable.end()), skippable.end());
		// A hidden preparation (PBD_PREPARE_FOR=lobby or watch) opens no channel, yet it must emit exactly the code a lobby set
		// or a watcher emits: the objects are named after the emitted set, and any difference makes the real start compile again.
		const std::string prepare_for = env_text("PBD_PREPARE_FOR");
		const bool lobby_code = !pbd::env::path("PBD_SET_CHANNEL").empty() || prepare_for == "lobby";
		const bool watch_code = !pbd::env::path("PBD_SPECTATOR_CHANNEL").empty() || prepare_for == "watch";
		if (lobby_code && (launch.exit_gate || parse_number("PBD_SESSION_EXIT_GATE", 0) == 1))
		{
			// The lobby's natural Rematch loader has a final weak-handle test before scene 2.
			skippable.push_back(0x2739f4u);
		}
		if (lobby_code || watch_code)
		{
			skippable.insert(skippable.end(), {0x59000u, 0x5aa18u});
			std::sort(skippable.begin(), skippable.end());
			skippable.erase(std::unique(skippable.begin(), skippable.end()), skippable.end());
		}
		if (pbd::practice::requested() || pbd::replay_viewer::requested())
		{
			observed_sites.insert(observed_sites.end(), {0xf0600u, 0xf26f4u, 0xf2c84u, 0xf0764u, 0xf07a4u, 0x16d9cu});
			std::sort(observed_sites.begin(), observed_sites.end());
			observed_sites.erase(std::unique(observed_sites.begin(), observed_sites.end()), observed_sites.end());
		}
		if (pbd::practice::requested())
		{
			// Only Practice emits this narrow pause paint scope; native resource creation still runs in full.
			observed_sites.insert(observed_sites.end(), {0x1eed14u, 0x1eedd0u});
			skippable.insert(skippable.end(), {0x139d44u, 0x108248u});
			if (pbd::game_lifecycle::requested())
				observed_sites.insert(observed_sites.end(), {0x15f5cu, 0x16018u});
			std::sort(observed_sites.begin(), observed_sites.end());
			observed_sites.erase(std::unique(observed_sites.begin(), observed_sites.end()), observed_sites.end());
			std::sort(skippable.begin(), skippable.end());
			skippable.erase(std::unique(skippable.begin(), skippable.end()), skippable.end());
		}
		// A lobby set times its fights (pbd_fight_timing.h: a live session with a set channel). Its hidden preparation is
		// neither live nor bound to a channel, and emits the same probes so it compiles the set's own objects.
		if (pbd::frame_presentation::queue_tags_enabled() || (prepare_for == "lobby" && env_text("PBD_SESSION") == "1"))
		{
			observed_sites.push_back(0x3c53a0);
			observed_sites.push_back(0x34b9d8);
			observed_sites.push_back(0x34bb1c);
			std::sort(observed_sites.begin(), observed_sites.end());
			observed_sites.erase(std::unique(observed_sites.begin(), observed_sites.end()), observed_sites.end());
		}
	}

	void config::configure_skip_guard()
	{
		skip_guard = parse_number("PBD_SKIP_GUARD", 0) == 1;
		skip_guard_record = skip_guard || parse_number("PBD_SKIP_GUARD_RECORD", 0) == 1;
#ifdef PBD_DIAGNOSTIC_BUILD
		skip_guard_test_taint = parse_number("PBD_TEST_SKIP_GUARD_TAINT", 0) == 1;
		skip_guard_test_missing = parse_number("PBD_TEST_SKIP_GUARD_MISSING", 0) == 1;

#endif
		skip_guard_subset = parse_number("PBD_SKIP_GUARD_SUBSET", 0) == 1;
		guard_entries.assign(universal_skip_entries.begin(), universal_skip_entries.end());
		if (skip_guard_subset)
		{
			// Empty is an intentional zero-skip control. Keep all emitted hooks and
			// live recording unchanged, so every subset uses the same guest code.
			guard_entries = parse_list("PBD_SKIP_GUARD_ENTRIES");
			for (u32 a : guard_entries)
				if (!std::binary_search(universal_skip_entries.begin(), universal_skip_entries.end(), a))
					configuration_errors.push_back(fmt::format("PBD_SKIP_GUARD_ENTRIES: 0x%x is outside the candidate set", a));
		}
		if (skip_guard_record)
		{
			skippable.insert(skippable.end(), universal_skip_entries.begin(), universal_skip_entries.end());
			// Three generator cores and all observed return sites. Unknown return sites refuse the skip.
			for (u32 a : {0x3afc3cu, 0xe859cu, 0x66950u, 0x25b5fcu, 0x25b86cu, 0x1f0b54u})
				observed_sites.push_back(a);
			std::sort(skippable.begin(), skippable.end());
			skippable.erase(std::unique(skippable.begin(), skippable.end()), skippable.end());
			std::sort(observed_sites.begin(), observed_sites.end());
			observed_sites.erase(std::unique(observed_sites.begin(), observed_sites.end()), observed_sites.end());
		}
	}

	void config::configure_runtime_skips()
	{
		random_runtime().rng_audit = parse_list("PBD_FN_RNG_AUDIT");
		random_runtime().draw_diagnostics = parse_number("PBD_SESSION_DRAW_CENSUS", 0) == 1;
		random_runtime().keyed_draws = parse_number("PBD_SESSION_KEYED_DRAWS", 0) == 1;
		// A skippable address is also a probe; keep the two lists disjoint so probe_at/skippable_at answer once.
		observed_sites.erase(std::remove_if(observed_sites.begin(), observed_sites.end(), [this](u32 a)
								 {
									 return std::binary_search(skippable.begin(), skippable.end(), a);
								 }),
			observed_sites.end());
		skip = parse_list("PBD_FN_SKIP");
		const auto replay = pbd::session_policy::replay_presentation_of(launch);
		skip.insert(skip.end(), replay.skips.begin(), replay.skips.end());
		std::sort(skip.begin(), skip.end());
		skip.erase(std::unique(skip.begin(), skip.end()), skip.end());
		gate_player_draw = replay.player_draw_gate;
		gate_stage_camera = replay.stage_camera_gate;
		{
			// PBD_FN_SKIP_GATE: per-call gates (player_draw, effects_draw, stage_camera); other nonempty tokens are setup errors.
			const std::string gates = env_text("PBD_FN_SKIP_GATE");
			for (usz pos = 0; pos < gates.size();)
			{
				const usz end = std::min(gates.find(',', pos), gates.size());
				const std::string g = gates.substr(pos, end - pos);
				if (g == "player_draw")
					gate_player_draw = true;
				else if (g == "effects_draw")
					gate_effects_draw = true;
				else if (g == "stage_camera")
					gate_stage_camera = true;
				else if (!g.empty())
					configuration_errors.push_back(fmt::format("PBD_FN_SKIP_GATE: unknown gate '%s'", g));
				pos = end + 1;
			}
#ifdef PBD_DIAGNOSTIC_BUILD
			test_stage_camera_force = parse_number("PBD_TEST_STAGE_CAMERA_FORCE", 0);
#endif
		}
		if (const auto text = env_text("PBD_FN_SKIP_UNLESS"); !text.empty())
		{
			for (usz pos = 0; pos < text.size();)
			{
				const usz end = std::min(text.find(',', pos), text.size());
				const std::string item = text.substr(pos, end - pos);
				const usz colon = item.find(':');
				if (colon == std::string::npos)
				{
					configuration_errors.push_back(fmt::format("PBD_FN_SKIP_UNLESS: expected entry:address, got '%s'", item));
					pos = end + 1;
					continue;
				}
				u64 entry = 0, address = 0;
				if (!pbd::env::parse_unsigned(std::string_view(item).substr(0, colon), entry, 0xffffffffu, 16) ||
					!pbd::env::parse_unsigned(std::string_view(item).substr(colon + 1), address, 0xffffffffu, 16) || entry % 4 || address % 4)
					configuration_errors.emplace_back("PBD_FN_SKIP_UNLESS: invalid aligned entry:address");
				else
					skip_unless.emplace_back(static_cast<u32>(entry), static_cast<u32>(address));
				pos = end + 1;
			}
		}
		// Runtime skips require a conditional return emitted at the same entry.
		for (u32 a : skip)
			if (!std::binary_search(skippable.begin(), skippable.end(), a))
				configuration_errors.push_back(fmt::format("PBD_FN_SKIP: 0x%x is not skippable", a));
	}

	void config::configure_registers()
	{
		const auto text = env_text("PBD_FN_REGS");
		if (text.empty())
			return;
		std::string_view rest(text);
		for (unsigned slot = 0; slot < 2; ++slot)
		{
			const auto comma = rest.find(',');
			u64 value = 0;
			if (!pbd::env::parse_unsigned(rest.substr(0, comma), value, 32, 10) || (slot == 1 && comma != rest.npos))
			{
				configuration_errors.emplace_back("PBD_FN_REGS: expected one or two decimal register indices (0..32)");
				return;
			}
			(slot == 0 ? reg0 : reg1) = static_cast<int>(value);
			if (comma == rest.npos)
				return;
			rest.remove_prefix(comma + 1);
		}
	}

	void config::configure_dispatch()
	{
		{
			// parse_list sorts, so read the two lists in their raw order instead.
			const auto sites = parse_raw("PBD_FN_DISPATCH");
			const auto slots = parse_slots("PBD_FN_DISPATCH_SLOTS");
			for (usz i = 0; i < sites.size(); i++)
			{
				const u32 slot = i < slots.size() ? slots[i] : 8u;
				dispatch.emplace_back(sites[i], slot);
			}
			// Compatibility with the first form of this switch.
			for (u32 a : parse_list("PBD_FN_TASK_DISPATCH"))
				if (std::find_if(dispatch.begin(), dispatch.end(), [a](const auto& d)
						{
							return d.first == a;
						}) == dispatch.end())
					dispatch.emplace_back(a, 8u);
		}
		skip_tasks = parse_list("PBD_FN_SKIP_TASKS");
		if (const auto r = parse_list("PBD_FN_RNG_REPAIR"); !r.empty())
			random_runtime().rng_repair = r[0];
		if (const auto r = parse_list("PBD_FN_RNG_EXIT"); !r.empty())
			random_runtime().rng_exit = r[0];
		if (const auto text = env_text("PBD_FN_RNG_OFFSET"); !text.empty())
		{
			u64 value = 0;
			if (!pbd::env::parse_unsigned(std::string_view(text), value, 0xffffffffu, 16))
				configuration_errors.emplace_back("PBD_FN_RNG_OFFSET: invalid 32-bit hexadecimal offset");
			else
				random_runtime().rng_offset = static_cast<u32>(value);
		}
	}

	void config::configure_random_state()
	{
		{
#ifdef PBD_DIAGNOSTIC_BUILD
			test_early_flush = parse_number("PBD_TEST_EARLY_FLUSH", 0) == 1;
			early_flush_live = parse_number("PBD_EARLY_FLUSH_LIVE", 0) == 1;

#endif
			if (const u64 seed = parse_number("PBD_MATCH_SEED", 0))
			{
				random_runtime().match_seed_set = true;
				random_runtime().match_seed_base = static_cast<u32>(seed);
			}
			blocking_loads = parse_number("PBD_SESSION_BLOCKING_LOADS", 0) == 1;
			random_runtime().stage_sync = launch.stage_sync || parse_number("PBD_SESSION_STAGE_SYNC", 0) == 1;
			const auto entries = parse_raw("PBD_FN_RNG_FENCE");
			const auto exits = parse_raw("PBD_FN_RNG_FENCE_EXIT");
			if (entries.size() != exits.size())
				configuration_errors.emplace_back("RNG fence entry/exit counts differ");
			for (usz i = 0; i < entries.size(); i++)
				if (entries[i] && i < exits.size() && exits[i])
					random_runtime().fence.emplace_back(entries[i], exits[i]);
		}
		max_events = parse_number("PBD_FN_MAX_EVENTS", 0);
		if (!observed_sites.empty() || !skippable.empty())
		{
			// Only emitted hooks affect the object tag; changing PBD_FN_SKIP reuses the same compiled code.
			tag = pbd::ppu_object_name::hook_tag(observed_sites, skippable);
			if (max_events)
			{
				try
				{
					events.resize(static_cast<usz>(max_events));
				}
				catch (...)
				{
					pbd_fn_log.error("Hook event capture disabled: %u records could not be allocated.", max_events);
					events.clear();
					max_events = 0;
				}
			}
		}
	}

	config::config()
	{
		configure_observed_sites();
		configure_skippable();
		configure_skip_guard();
		configure_runtime_skips();
		configure_registers();
		configure_dispatch();
		configure_random_state();
	}

	config& cfg() noexcept
	{
		static config instance;
		return instance;
	}
	void check_code_addresses(const config& c, const std::vector<std::pair<u32, u32>>& code_ranges, std::vector<std::string>& errors)
	{
		for (const auto& [name, addresses] : supplied_addresses)
		{
			std::string parsed;
			for (u32 address : addresses)
			{
				parsed += fmt::format("%s0x%x", parsed.empty() ? "" : ",", address);
				if (!address_in_code(address, code_ranges))
					errors.push_back(fmt::format("%s: 0x%x outside executable ELF LOAD bytes", name, address));
			}
			if (!addresses.empty())
				pbd_fn_log.notice("Probe configuration %s = %s", name, parsed);
		}
		for (const auto* addresses : {&c.observed_sites, &c.skippable})
			for (u32 address : *addresses)
				if (!address_in_code(address, code_ranges))
					errors.push_back(fmt::format("Emitted hook 0x%x outside executable ELF LOAD bytes", address));
	}

} // namespace pbd::fn_hook::detail
