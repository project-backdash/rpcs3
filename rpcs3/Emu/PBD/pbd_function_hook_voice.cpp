#include "stdafx.h"
#include "pbd_function_hook_state.h"
#include "pbd_rank_screens.h"

namespace pbd::fn_hook
{
	using namespace detail;

	namespace detail
	{
		voice_state& voice_runtime() noexcept
		{
			static voice_state instance;
			return instance;
		}

		constexpr u32 voice_restore_bit = 0x80000000u;
		thread_local u32 voice_depth = 0;
		thread_local bool voice_owned = false;
		thread_local u32 voice_slot = 0;
		thread_local u32 allocation_depth = 0;
		thread_local bool allocation_succeeded = false;

		bool voice_yield(ppu_thread& ppu) noexcept
		{
			sys_timer_usleep(ppu, 100);
			return !ppu.check_state();
		}

		void voice_fault_stop(voice_state& c, const char* reason, u32 thread, u32 owner) noexcept
		{
			c.faults.fetch_add(1);
			pbd_fn_log.fatal("voice fault (%s): thread 0x%x owner 0x%x tick %u; scheduling no-save termination",
				reason, thread, owner, current_tick.load());
#ifdef PBD_DIAGNOSTIC_BUILD
			if (c.test_drop_main_stop)
			{
				c.main_stop_dropped.fetch_add(1);
				pbd_fn_log.warning("TEST-ONLY: intentionally dropping the main-thread no-save stop; fatal watchdog must exit the process");
				return;
			}
#endif
			try
			{
				const u64 emulation = static_cast<u64>(Emu.GetEmulationIdentifier());
				Emu.CallFromMainThread([emulation]
					{
						if (static_cast<u64>(Emu.GetEmulationIdentifier()) == emulation)
							Emu.Kill(true, false);
					},
					nullptr, false);
				c.fail_stops.fetch_add(1);
			}
			catch (...)
			{
				pbd_fn_log.fatal("could not schedule the no-save voice fault stop");
			}
		}

		void voice_fatal_record(voice_state& c, const char* phase, u64 generation, u32 thread, u32 instruction, u32 owner) noexcept
		{
			try
			{
				const auto directory = pbd::env::path("PBD_ORDINARY_OUTPUT");
				if (directory.empty())
					return;
				const auto path = (directory / "voice-fatal-fault.json").u8string();
				const auto text = fmt::format(
					"{\"schema\":1,\"phase\":\"%s\",\"generation\":%u,\"thread\":%u,\"instruction\":\"0x%x\","
					"\"owner\":%u,\"tick\":%u,\"deadline_ms\":%u,\"late_events\":%u,\"exit_code\":79}\n",
					phase, generation, thread, instruction, owner, current_tick.load(), c.fatal_deadline_ms,
					c.fatal_late_events.load());
				if (fs::file out(std::string(path.begin(), path.end()), fs::rewrite); out)
				{
					out.write(text.data(), text.size());
					out.sync();
				}
			}
			catch (...)
			{
				pbd_fn_log.error("fatal containment could not retain voice-fatal-fault.json");
			}
		}

		void voice_arm_fatal_containment(voice_state& c, ppu_thread& ppu, u32 instruction, u32 owner) noexcept
		{
			c.fatal_outcomes.fetch_add(1);
			ppu.state += cpu_flag::exit;
			const u64 generation = static_cast<u64>(Emu.GetEmulationIdentifier());
			if (c.fatal_armed.exchange(true))
				return;
			c.fatal_thread.store(ppu.id);
			c.fatal_instruction.store(instruction);
			c.fatal_generation.store(generation);
			voice_fatal_record(c, "armed", generation, ppu.id, instruction, owner);
			const u64 deadline_ms = c.fatal_deadline_ms;
			std::thread{[&c, generation, thread = ppu.id, instruction, owner, deadline_ms]
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(deadline_ms));
					if (static_cast<u64>(Emu.GetEmulationIdentifier()) != generation || Emu.IsStopped(true))
					{
						voice_fatal_record(c, "main_stop_completed", generation, thread, instruction, owner);
						return;
					}
					c.fatal_watchdog_exits.fetch_add(1);
					pbd_fn_log.fatal("fatal containment deadline expired for generation %u; forcing authenticated process exit 79", generation);
					voice_fatal_record(c, "watchdog_process_exit", generation, thread, instruction, owner);
					logs::listener::sync_all();
					std::_Exit(79);
				}}
				.detach();
		}

		// Caller holds voice_lease_mutex. A sequence is host history and is never rewound with a game tick.
		void voice_marker(voice_state& c, ppu_thread& ppu, u32 kind, u32 slot)
		{
			const voice_state::marker row{++c.foreign_sequence, ppu.id, current_tick.load(), current_pass.load(), kind, slot};
			if (c.foreign_events.size() < 4096)
				c.foreign_events.push_back(row);
			pbd_fn_log.notice("foreign voice %s: seq %u thread 0x%x tick %u replay %u slot 0x%x", kind ? "leave" : "enter", row.sequence, row.thread, row.tick, row.replay, row.slot);
		}

		hook_action voice_enter(ppu_thread& ppu) noexcept
		{
			auto& c = voice_runtime();
			if (voice_depth++)
			{
				if (ppu.id != pbd::t5dr::main_ppu_id)
				{
					std::lock_guard lock(c.lease_mutex);
					voice_marker(c, ppu, 0, 0);
				}
				return hook_action::continue_guest; // Nested claims share the outer lease, but each event is still recorded.
			}
			voice_slot = 0;
			u32 state = c.activity.load();
			for (;;)
			{
				{
					std::lock_guard lock(c.lease_mutex);
					if (!(state & voice_restore_bit) && c.activity.compare_exchange_weak(state, state + 1))
					{
						if (ppu.id != pbd::t5dr::main_ppu_id)
						{
							c.foreign_active++;
							voice_marker(c, ppu, 0, 0);
						}
						break;
					}
				}
				// A restore contains only bounded memory writes; it must finish before another claim can enter.
				voice_yield(ppu);
				state = c.activity.load();
			}
			c.claims.fetch_add(1);
			const auto until = hook_clock::now() + std::chrono::microseconds(c.timeout_us);
			bool contended = false;
			for (;;)
			{
				u32 free = 0;
				if (c.owner.compare_exchange_strong(free, ppu.id))
				{
					voice_owned = true;
					break;
				}
				if (!contended)
				{
					c.contentions.fetch_add(1);
					contended = true;
				}
				if (hook_clock::now() >= until || !voice_yield(ppu))
				{
					c.timeouts.fetch_add(1);
					const u32 owner = c.owner.load();
					voice_fault_stop(c, "claim_timeout", ppu.id, owner);
					if (c.fatal_timeout)
					{
						voice_arm_fatal_containment(c, ppu, 0x342044, owner);
						return hook_action::fatal;
					}
					break; // lease stays counted until safe termination; no restore may overlap it
				}
			}
#ifdef PBD_DIAGNOSTIC_BUILD
			if (voice_owned && ppu.id != pbd::t5dr::main_ppu_id && c.test_rendezvous_us &&
				current_tick.load() >= c.test_rendezvous_tick)
			{
				bool armed = true;
				if (c.test_rendezvous_armed.compare_exchange_strong(armed, false))
				{
					c.test_rendezvous_entered.fetch_add(1);
					pbd_fn_log.warning("TEST-ONLY: holding foreign voice thread 0x%x inside claim critical section at tick %u for %u us; audio stall is intentional",
						ppu.id, current_tick.load(), c.test_rendezvous_us);
					sys_timer_usleep(ppu, c.test_rendezvous_us);
					c.test_rendezvous_released.fetch_add(1);
					pbd_fn_log.warning("TEST-ONLY: released foreign voice rendezvous thread 0x%x at tick %u", ppu.id, current_tick.load());
				}
			}
#endif
			return hook_action::continue_guest;
		}

		void voice_leave(ppu_thread& ppu) noexcept
		{
			auto& c = voice_runtime();
			if (!voice_depth)
			{
				c.balance_errors.fetch_add(1);
				return;
			}
			if (--voice_depth)
			{
				if (ppu.id != pbd::t5dr::main_ppu_id)
				{
					std::lock_guard lock(c.lease_mutex);
					voice_marker(c, ppu, 1, voice_slot);
				}
				return;
			}
			if (voice_owned)
			{
				c.owner.store(0);
				voice_owned = false;
			}
			std::lock_guard lock(c.lease_mutex);
			if (ppu.id != pbd::t5dr::main_ppu_id)
			{
				voice_marker(c, ppu, 1, voice_slot);
				if (c.foreign_active)
					c.foreign_active--;
				else
					c.balance_errors.fetch_add(1);
			}
			c.activity.fetch_sub(1);
		}
		std::optional<hook_action> voice_actions(ppu_thread& ppu, u32 address)
		{
			auto& c = voice_runtime();
			trace_voice(ppu, address);
			if (address == 0x342044)
			{
				if (!allocation_depth++)
					allocation_succeeded = false;
			}
			else if (address == 0x3428b8 && allocation_depth)
				allocation_succeeded = true;
			else if ((address == 0x3420d8 || address == 0x342104) && allocation_depth && !--allocation_depth)
			{
				const auto now = std::chrono::duration_cast<std::chrono::microseconds>(hook_clock::now().time_since_epoch()).count();
				std::lock_guard lock(c.lease_mutex);
				const auto exhausted = c.health.exhausted;
				c.health.observe(allocation_succeeded, static_cast<u64>(now));
				if (c.health.exhausted != exhausted)
					pbd_fn_log.error("Sound voice pool exhausted: %u consecutive failed allocations, tick %u", c.health.streak, current_tick.load());
			}

			if (c.lock)
			{
				if (address == 0x342044)
				{
					const auto action = voice_enter(ppu);
					if (action == hook_action::fatal)
						return action;
				}
				else if (address == 0x3420d8 || address == 0x342104)
					voice_leave(ppu);
				else if (address == 0x3428b8)
					voice_slot = static_cast<u32>(ppu.gpr[3]);
			}
			// All claimers participate, including the music feeder (before the owner-only filter).
#ifdef PBD_DIAGNOSTIC_BUILD
			if (address == 0x3428b8 && c.test_delay_us)
			{
				const u32 voice = static_cast<u32>(ppu.gpr[3]);
				const auto n = c.test_delays.fetch_add(1) + 1;
				// A guest-aware sleep releases the PPU scheduler slot; a host mutex/sleep alone can starve the other claimer.
				sys_timer_usleep(ppu, c.test_delay_us);
				if (!ppu.check_state() && vm::check_addr(voice + 0x40, vm::page_readable, 1) && vm::read8(voice + 0x40))
				{
					c.test_stale.fetch_add(1);
					pbd_fn_log.error("stale voice claim: thread 0x%x voice 0x%x tick %u after free-check delay %u us", ppu.id, voice, current_tick.load(), c.test_delay_us);
				}
				if (n <= 4)
					pbd_fn_log.warning("voice delay: thread 0x%x voice 0x%x tick %u (%u us)", ppu.id, voice, current_tick.load(), c.test_delay_us);
			}
#endif
			return {};
		}

	} // namespace detail

	u64 voice_checkpoint_token() noexcept
	{
		auto& c = voice_runtime();
		if (!c.lock)
			return 0;
		std::lock_guard lock(c.lease_mutex);
		// A capture that overlaps a foreign lease is conservatively unsafe even if its enter preceded the capture.
		return c.foreign_sequence | (c.foreign_active ? (u64{1} << 63) : 0);
	}

	pbd::stage_sync::epoch reseed_epoch() noexcept
	{
		return random_runtime().epoch;
	}

	bool bind_match_seed(u32 seed) noexcept
	{
		auto& c = random_runtime();
		if (!seed || current_tick.load() || current_pass.load() || c.epoch.match_seeds || c.epoch.mobs.count() ||
			c.direct_match_epoch_applied || !stage_controllers.empty() || (c.match_seed_set && c.match_seed_base != seed))
			return false;
		c.match_seed_base = seed;
		c.match_seed_set = true;
		return true;
	}

	bool reset_for_next_game() noexcept
	{
		try
		{
			// Keep process settings; the next set supplies its seed through bind_match_seed.
			auto& random = random_runtime();
			auto fresh = std::make_unique<random_state>();
			fresh->rng_repair = random.rng_repair;
			fresh->rng_exit = random.rng_exit;
			fresh->rng_offset = random.rng_offset;
			fresh->fence = std::move(random.fence);
			fresh->stage_sync = random.stage_sync;
			fresh->draw_diagnostics = random.draw_diagnostics;
			fresh->keyed_draws = random.keyed_draws;
			fresh->rng_audit = std::move(random.rng_audit);
			random = std::move(*fresh);
			// A thread stopped inside a voice claim never released it.
			auto& voice = voice_runtime();
			std::lock_guard lock(voice.lease_mutex);
			voice.activity = 0;
			voice.owner = 0;
			voice.foreign_active = 0;
			voice.foreign_sequence = 0;
			voice.foreign_events.clear();
			voice.health = {};
		}
		catch (...)
		{
			return false;
		}
		current_tick = 0;
		current_pass = 0;
		gate_ready_tick = 0;
		gate_hold = 0;
		// The old guest's exit-gate node went with its memory.
		cfg().gate_node = 0;
		// A new lobby command must arm rank screens so a resident game starts with the same values as a fresh peer.
		pbd::rank_screens::arm({}, {0u, 0u});
		spectator_vs_init_gate = 0;
		rematch_vs_init_gate = false;
		stage_controllers.clear();
		draw_statistics_active = 1;
		keyed_segment = 0;
		keyed_counts.clear();
		keyed_in_replay = 0;
		thread_detaches = 0;
		draw_statistics.clear();
		return true;
	}

	bool begin_direct_match_epoch(ppu_thread& ppu, u32 seed, u32 prior_matches) noexcept
	{
		auto& c = random_runtime();
		be_t<u32> scene{}, pending{};
		if (!pbd_direct_match::requested() || current_tick.load() || current_pass.load() ||
			get_current_cpu_thread() != &ppu || ppu.id != pbd::t5dr::main_ppu_id || ppu.cia != pbd::t5dr::boundary_pc ||
			ppu.gpr[2] != pbd::t5dr::toc ||
			!pbd::replay_carry::owner_ready(ppu, static_cast<u64>(Emu.GetEmulationIdentifier())) || ppu.current_function ||
			Emu.GetTitleID() != pbd::t5dr::title_id || Emu.GetExecutableHash() != pbd::t5dr::executable_hash ||
			!vm::try_access(pbd::t5dr::scene_address, &scene, 4, false) ||
			!vm::try_access(pbd::t5dr::subscene_address, &pending, 4, false) ||
			scene != 3u || pending != 44u || !seed ||
			c.epoch.match_seeds || c.epoch.mobs.count() || c.direct_match_epoch_applied || prior_matches > 125)
			return false;
		if (!c.match_seed_set && pbd::replay_standby::pbd_requested())
		{
			c.match_seed_set = true;
			c.match_seed_base = seed;
		}
		if (!c.match_seed_set || seed != c.match_seed_base)
			return false;
		c.epoch = pbd::stage_sync::epoch::replay(prior_matches);
		c.direct_match_epoch_applied = true;
		return true;
	}

	bool voice_restore_history_ok(u64 token, u32 target, u32 from) noexcept
	{
		auto& c = voice_runtime();
		if (!c.lock)
			return true;
		std::lock_guard lock(c.lease_mutex);
		if (token == c.foreign_sequence)
			return true;
		c.foreign_claim_rewound.fetch_add(1);
		pbd_fn_log.error("voice_foreign_claim_rewound: refusing restore %u -> %u, snapshot token 0x%x current sequence %u", from, target, token, c.foreign_sequence);
		return false; // A detector is not permission to rewrite/exclude foreign-owned slots.
	}

	bool voice_boundary_ok() noexcept
	{
		auto& c = voice_runtime();
		if (!c.lock)
			return true;
		std::lock_guard lock(c.lease_mutex);
		const u32 active = c.activity.load() & ~voice_restore_bit;
		const bool ok = active >= c.foreign_active && active - c.foreign_active == (voice_depth ? 1u : 0u) && voice_depth == 0;
		if (!ok && c.balance_errors.fetch_add(1) == 0)
			pbd_fn_log.error("voice lease imbalance: activity %u foreign %u owner depth %u tick %u", active, c.foreign_active, voice_depth, current_tick.load());
		return ok;
	}

	struct voice_trace_file
	{
		fs::file output;
		std::mutex mutex;
		std::atomic<u64> sequence{0};
		std::atomic<bool> enabled{false};
		voice_trace_file()
		{
			static_cast<void>(open_environment_path());
		}
		bool open_environment_path() noexcept
		{
			try
			{
#ifdef _WIN32
				const wchar_t* path = pbd::env::find_wide("PBD_VOICE_TRACE");
				if (!path || !*path)
					return true;
				if (!std::filesystem::path(path).is_absolute())
					return false;
				const auto utf8 = std::filesystem::path(path).u8string();
				const bool opened = output.open(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile);
				enabled.store(opened, std::memory_order_relaxed);
				return opened;
#else
				return true;
#endif
			}
			catch (...)
			{
				enabled.store(false, std::memory_order_relaxed);
				return false;
			}
		}
		u64 next() noexcept
		{
			return ++sequence;
		}
		void write(const std::string& text) noexcept
		{
			std::lock_guard lock(mutex);
			if (enabled.load(std::memory_order_relaxed) && output.write(text.data(), text.size()) != text.size())
				enabled.store(false, std::memory_order_relaxed);
		}
	};
	voice_trace_file& voice_trace() noexcept
	{
		static voice_trace_file value;
		return value;
	}

	bool voice_restore_begin(ppu_thread& ppu) noexcept
	{
		auto& c = voice_runtime();
		if (!c.lock)
			return true;
		const auto until = hook_clock::now() + std::chrono::microseconds(c.timeout_us);
		bool waited = false;
		for (;;)
		{
			u32 idle = 0;
			if (c.activity.compare_exchange_strong(idle, voice_restore_bit))
			{
				c.restores.fetch_add(1);
				return true;
			}
			if (!waited)
			{
				c.restore_waits.fetch_add(1);
				waited = true;
			}
			if (hook_clock::now() >= until || !voice_yield(ppu))
			{
				c.restore_refused.fetch_add(1);
				voice_fault_stop(c, "restore_timeout", ppu.id, c.owner.load());
				return false;
			}
		}
	}

	void voice_restore_end() noexcept
	{
		auto& c = voice_runtime();
		if (c.lock)
			c.activity.fetch_and(~voice_restore_bit);
	}
	void detail::trace_voice(ppu_thread& ppu, u32 address)
	{
		// Trace only (PBD_VOICE_TRACE=<absolute path>): every voice-claim probe event by every thread,
		// with the latch tick and whether the main thread is re-executing, so a missing or repeated voice can be located.
		if ((address == 0x342044 || address == 0x3420d8 || address == 0x342104 || address == 0x3428b8) && voice_trace().enabled.load(std::memory_order_relaxed))
		{
			const bool replaying = g_pbd_replay_frame.load(std::memory_order_relaxed);
			voice_trace().write(fmt::format("{\"seq\":%u,\"pc\":\"0x%x\",\"thread\":\"0x%x\",\"main\":%s,\"tick\":%u,\"replay\":%s,\"r3\":\"0x%x\",\"r4\":\"0x%x\",\"r5\":\"0x%x\",\"lr\":\"0x%x\"}\n",
				voice_trace().next(), address, ppu.id, ppu.id == pbd::t5dr::main_ppu_id ? "true" : "false", current_tick.load(),
				replaying ? "true" : "false", static_cast<u32>(ppu.gpr[3]), static_cast<u32>(ppu.gpr[4]), static_cast<u32>(ppu.gpr[5]),
				static_cast<u32>(ppu.lr)));
		}
	}

} // namespace pbd::fn_hook
