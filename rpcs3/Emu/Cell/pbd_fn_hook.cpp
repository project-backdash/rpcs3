#include "stdafx.h"
#include "../PBD/pbd_function_hook_event.h"

namespace pbd::fn_hook
{
	using namespace detail;

	bool keyed_draws_enabled() noexcept
	{
		return random_runtime().keyed_draws;
	}

	bool validate_configuration(const std::vector<std::pair<u32, u32>>& code_ranges)
	{
		const auto& c = cfg();
		auto errors = configuration_errors;
		const auto initial_errors = configuration_errors.size();
		const auto emitted = [&](u32 address)
		{
			return std::binary_search(c.observed_sites.begin(), c.observed_sites.end(), address) ||
			       std::binary_search(c.skippable.begin(), c.skippable.end(), address);
		};
		const auto require = [&](const char* feature, u32 address)
		{
			if (!emitted(address))
				errors.push_back(fmt::format("%s requires emitted probe 0x%x", feature, address));
		};
		check_code_addresses(c, code_ranges, errors);
		if (random_runtime().match_seed_set || !pbd_stage_sync_seed_missing(false, env_text, pbd::env::path))
			require("PBD_MATCH_SEED", 0x15a88);
		if (!pbd::env::path("PBD_BOOT_WITNESS").empty() || !env_text("PBD_BOOT_TARGET").empty())
			for (u32 address : {0x12d74u, 0x56258u, 0x56f20u, 0x1e9e8u, 0xafc64u, 0x5b0f0u, 0x5b0f8u, 0x159f0u, 0x15a9cu, 0x15ac4u,
					 0x3afc3cu, 0x3afd48u, 0xe859cu, 0xe85c4u, 0x66950u, 0x66af8u, 0xcb96cu, 0x52298u})
				require("PBD_BOOT_WITNESS", address);
		if (random_runtime().stage_sync)
		{
			require("PBD_SESSION_STAGE_SYNC", 0x52298);
			require("PBD_SESSION_STAGE_SYNC", 0xcb96c);
			if (pbd_stage_sync_seed_missing(random_runtime().match_seed_set, env_text, pbd::env::path))
				errors.emplace_back("PBD_SESSION_STAGE_SYNC requires nonzero PBD_MATCH_SEED");
		}
		// The loading-screen exit gate holds a ready screen (scenes 8 and 12) by masking its slot's state word for one call, which
		// only these four probes do. Without them the gate changes nothing in the game yet still records a hold every frame: the
		// other player waits until session_exit_gate_timeout and every watcher of the set fails. So the launch is refused.
		if (env_text("PBD_SESSION_EXIT_GATE") == "1")
		{
			std::string missing;
			for (u32 address : {0x52288u, 0x52290u, 0x117c70u, 0x117c78u})
				if (!emitted(address))
					missing += fmt::format("%s0x%x", missing.empty() ? "" : ", ", address);
			if (!missing.empty())
				errors.push_back(fmt::format("PBD_SESSION_EXIT_GATE=1 needs the loading-screen gate probes 0x52288, 0x52290, 0x117c70 and "
											 "0x117c78 in PBD_FN_PROBES; missing: %s. Add them, or set PBD_SESSION_EXIT_GATE=0.",
					missing));
		}
		if (random_runtime().draw_diagnostics || random_runtime().keyed_draws)
			for (u32 address : {0x3afc3cu, 0xe859cu, 0x66950u, 0x121de4u, 0xe85c8u, 0xe8674u,
					 0x66b14u, 0x66b38u, 0x66bbcu, 0x66c40u, 0x66cd4u, 0x162204u})
				require("Draw census/keyed draws", address);
		if (c.blocking_loads)
		{
			require("Blocking loads", 0x9a884);
			require("Blocking loads", 0x1ebe1c);
		}
		if (c.early_flush_live || c.test_early_flush)
			require("Early flush", 0x34b9d8);
		for (const auto& [entry, leave] : random_runtime().fence)
		{
			require("RNG fence", entry);
			require("RNG fence exit", leave);
		}
		for (const auto& [entry, slot] : c.dispatch)
			require("Task dispatch", entry);
		for (u32 address : random_runtime().rng_audit)
			require("RNG audit", address);
		if (random_runtime().rng_repair)
			require("RNG repair exit", random_runtime().rng_exit);
		if (voice_runtime().fatal_timeout && g_cfg.core.ppu_decoder != ppu_decoder_type::llvm)
			errors.emplace_back("PBD_VOICE_FATAL_TIMEOUT supports only the LLVM PPU decoder");
		errors.insert(errors.end(), configuration_errors.begin() + initial_errors, configuration_errors.end());
		for (const auto& error : errors)
			pbd_fn_log.error("Probe configuration rejected: %s", error);
		if (!errors.empty())
			return false;
		if (requested())
			pbd_fn_log.notice("Probe configuration accepted: %u probes, %u skippable; executable ranges checked", c.observed_sites.size(), c.skippable.size());
		return true;
	}

	[[noreturn]] void reject_configuration_launch() noexcept
	{
		// Returning a generic ELF failure makes RPCS3 spawn a fatal GUI during savestate boot.
		// This is a controlled pre-execution configuration refusal, not a crash.
		try
		{
			const auto directory = pbd::env::path("PBD_ORDINARY_OUTPUT");
			if (!directory.empty())
			{
				const auto path = (directory / "boot-refusal.json").u8string();
				if (fs::file out(std::string(path.begin(), path.end()), fs::rewrite); out)
				{
					constexpr std::string_view text = "{\"schema\":1,\"reason\":\"probe_configuration_rejected\",\"exit_code\":78}\n";
					out.write(text.data(), text.size());
					out.sync();
				}
			}
		}
		catch (...)
		{
			pbd_fn_log.error("Could not write boot-refusal.json");
		}
		std::fputs("PBD: probe configuration rejected (exit 78); see native log\n", stderr);
		std::fflush(stderr);
		logs::listener::sync_all();
		std::_Exit(78);
	}

	bool requested() noexcept
	{
		const auto& c = cfg();
		return !c.observed_sites.empty() || !c.skippable.empty();
	}

	u16 steer_stage_sample(u32 role, u32 tick, u16 raw) noexcept
	{
#ifdef PBD_DIAGNOSTIC_BUILD
		auto& c = cfg();
		if (!c.steer_stage || role > 1)
			return raw;
		// Test input starts neutral while the options handshake prepublishes its bootstrap ticks.
		if (tick == 0)
			return 0;
		if (role != 1 || !vm::check_addr(pbd::t5dr::scene_address, vm::page_readable, 4) || vm::read32(pbd::t5dr::scene_address) != 3)
			return raw;
		const u32 player = c.select_players + 0x78 * role;
		if (!c.select_work || !c.select_players || !vm::check_addr(player, vm::page_readable, 0x78) ||
			!vm::check_addr(c.select_work, vm::page_readable, 0x1b0) || vm::read8(player + 0x20) != 13)
			return raw;
		// State 13 dispatches to 0xb13a4, the stage choice input handler. Mode 1 is roulette,
		// mode 0 the new stages, mode 2 the old stages. Only published local button words change.
		const u32 work = c.select_work, mode = vm::read32(work + 0x104), cursor = vm::read32(work + 0xfc);
		const u32 count = vm::read32(work + 0x110);
		if (!count || count > 21 || mode > 2)
			return 0;
		u32 wanted = ~0u;
		for (u32 i = 0; i < count; i++)
			if (vm::read32(work + 0x118 + 4 * i) == c.steer_stage)
				wanted = i;
		// Final stage 0x23 normalizes to 0x25 in VS when the final boss is absent.
		if (wanted == ~0u && c.steer_stage == 0x25)
			for (u32 i = 0; i < count; i++)
				if (vm::read32(work + 0x118 + 4 * i) == 0x23)
					wanted = i;
		if (cursor != c.steer_last_cursor || mode != c.steer_last_mode)
		{
			pbd_fn_log.warning("Stage steering tick %u: target 0x%x work 0x%x mode %u cursor %u wanted %u count %u", tick, c.steer_stage, work, mode, cursor, wanted, count);
			c.steer_last_cursor = cursor;
			c.steer_last_mode = mode;
		}
		if (wanted == ~0u || tick % 24 >= 6)
			return 0; // fail closed; six-tick press, eighteen-tick release
		if (mode != 0)
			return 0x1000; // Up from roulette -> new list; repeats from old -> roulette -> new
		if (cursor >= count)
			return 0;
		if (cursor == wanted)
			return 0x40; // Cross
		return 0x2000;   // Right, wrapping through the stock list
#else
		return raw;
#endif
	}

	bool observe_at(u32 address) noexcept
	{
		const auto& c = cfg();
		return std::binary_search(c.observed_sites.begin(), c.observed_sites.end(), address);
	}

	bool skippable_at(u32 address) noexcept
	{
		const auto& c = cfg();
		return std::binary_search(c.skippable.begin(), c.skippable.end(), address);
	}

	const std::string& variant() noexcept
	{
		return cfg().tag;
	}

	const std::vector<u32>& emitted_observers() noexcept
	{
		return cfg().observed_sites;
	}

	const std::vector<u32>& emitted_skippable() noexcept
	{
		return cfg().skippable;
	}

	void note_boundary(u32 tick, bool replay) noexcept
	{
		// A fence never spans a tick boundary: one still open here lost its exit probe. Drop it (the guest keeps the advance)
		// and count it, rather than writing a stale state back later.
		if (auto& random = random_runtime(); random.fence_depth)
		{
			random.fence_depth = 0;
			random.fence_faults++;
		}
		current_tick.store(tick, std::memory_order_relaxed);
		current_pass.store(replay ? 1u : 0u, std::memory_order_relaxed);
		if (auto& c = cfg(); c.skip_guard_record)
			c.guard_history.boundary(tick, replay);
	}

	hook_action event(ppu_thread& ppu, u32 address) noexcept
	{
		hook_event call{cfg(), ppu, address};
		return call.run();
	}

} // namespace pbd::fn_hook

extern "C" u32 ppu_pbd_fn_observe(ppu_thread* ppu, u32 address) noexcept
{
	return static_cast<u32>(pbd::fn_hook::event(*ppu, address));
}
