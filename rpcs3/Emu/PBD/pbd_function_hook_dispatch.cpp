#include "stdafx.h"
#include "pbd_function_hook_event.h"
#include "../Cell/pbd_versus_native.h"
#include "pbd_practice_paint.h"

namespace pbd::fn_hook::detail
{
	std::optional<hook_action> hook_event::admit()
	{
		if (!pbd::versus::frame_boundary(ppu, address))
		{
			ppu.state += cpu_flag::exit;
			return hook_action::fatal;
		}
		pbd::practice::native::frame_event(ppu, address);
		if (address == pbd::pick_transition::hook_pc)
			pbd::pick_transition::event(ppu);
		if (address == pbd::rank_screens::after_battle_setup || address == pbd::rank_screens::after_match_bookkeeping)
			pbd::rank_screens::event(ppu, address);
		const auto entry = pbd::replay_entry::enter(pbd::replay_entry::context{ppu, address},
			pbd_direct_match::function_event, pbd_live_replay::function_event, pbd::boot_request::event);
		if (entry == pbd::replay_entry::refusal::direct_replay)
		{
			pbd_fn_log.fatal("Direct replay match entry refused at 0x%x", address);
			ppu.state += cpu_flag::exit;
			return hook_action::fatal;
		}
		if (entry == pbd::replay_entry::refusal::boot_request)
		{
			pbd_fn_log.fatal("Native boot request refused at 0x%x; inspect its bounded witness", address);
			ppu.state += cpu_flag::exit;
			return hook_action::fatal;
		}
		if (voice_runtime().fatal_armed.load() && voice_runtime().fatal_generation.load() == static_cast<u64>(Emu.GetEmulationIdentifier()) &&
			voice_runtime().fatal_thread.load() == ppu.id)
		{
			voice_runtime().fatal_late_events.fetch_add(1);
			pbd_fn_log.fatal("fatal containment observed a late probe on thread 0x%x at 0x%x", ppu.id, address);
			ppu.state += cpu_flag::exit;
			return hook_action::fatal;
		}
		return {};
	}

	void hook_event::trace_render()
	{
		// Command identity: at the cellGcmSetFlip thunk (EBOOT import stub, LLE target 0x6d23b4), r3 is the GCM
		// context and r4 the buffer. The LLE's first command is GCM_DRIVER_QUEUE+1 written at context->current (after
		// an optional segment-switch callback), so the write pointer read here, on the writing thread, is the
		// command's address or the start of a NOP/JUMP chain leading to it. The RSX proves which (pop_command).
		if (address == 0x3c53a0 && pbd::frame_presentation::queue_tags_enabled())
		{
			const u32 gcm = static_cast<u32>(ppu.gpr[3]);
			const bool readable = gcm && vm::check_addr(gcm, vm::page_readable, 12);
			const u32 begin = readable ? +vm::read32(gcm) : 0u;
			const u32 end = readable ? +vm::read32(gcm + 4) : 0u;
			const u32 current = readable ? +vm::read32(gcm + 8) : pbd::frame_presentation::frame_tag_queue::no_address;
			const u32 callback = gcm && vm::check_addr(gcm + 12, vm::page_readable, 4) ? +vm::read32(gcm + 12) : 0u;
			const auto tag = pbd::frame_presentation::render_tokens.register_flip(ppu.id, ppu.id == pbd::t5dr::main_ppu_id,
				static_cast<u32>(ppu.gpr[4]), current, begin, end, gcm, callback);
			c.latest_flip_tag_sequence.store(tag.sequence, std::memory_order_release);
			const auto render = rsx::get_current_renderer();
			const u32 get = render && render->ctrl ? +render->ctrl->get : 0u;
			const u32 put = render && render->ctrl ? +render->ctrl->put : 0u;
			pbd::frame_presentation::tag_trace::get().produce(tag, ppu.id, pbd::t5dr::main_ppu_id,
				begin, end, current, get, put);
		}
		if ((address == 0x34b9d8 || address == 0x34bb1c) && pbd::frame_presentation::queue_tags_enabled())
		{
			const u64 context = ppu.id == pbd::t5dr::main_ppu_id ? pbd::frame_presentation::logical_context.load(std::memory_order_acquire) : ~u64{0};
			const u32 holder = vm::check_addr(0x46ff28, vm::page_readable, 4) ? +vm::read32(0x46ff28) : 0u;
			const u32 gcm = holder && vm::check_addr(holder, vm::page_readable, 4) ? +vm::read32(holder) : 0u;
			const u32 begin = gcm && vm::check_addr(gcm, vm::page_readable, 12) ? +vm::read32(gcm) : 0u;
			const u32 end = gcm && vm::check_addr(gcm, vm::page_readable, 12) ? +vm::read32(gcm + 4) : 0u;
			const u32 current = gcm && vm::check_addr(gcm, vm::page_readable, 12) ? +vm::read32(gcm + 8) : 0u;
			const auto render = rsx::get_current_renderer();
			const u32 get = render && render->ctrl ? +render->ctrl->get : 0u;
			const u32 put = render && render->ctrl ? +render->ctrl->put : 0u;
			pbd::frame_presentation::tag_trace::get().phase(address == 0x34b9d8 ? "flip_tail" : "present_end",
				ppu.id, pbd::t5dr::main_ppu_id, pbd::frame_presentation::logical_boundary.load(std::memory_order_acquire),
				context, begin, end, current, get, put);
		}
	}

	std::optional<hook_action> hook_event::owner_entry()
	{
		if (ppu.id != pbd::t5dr::main_ppu_id)
		{
			c.foreign.fetch_add(1, std::memory_order_relaxed);
			return hook_action::continue_guest;
		}
		// This owner-scoped live paint suppression is independent of replay skipping and RNG repair.
		pbd::game_lifecycle::placement_phase(ppu, address);
		if (pbd::practice::paint::suppress_paint(ppu, address))
			return hook_action::skip;
		replay = g_pbd_replay_frame.load(std::memory_order_relaxed); // the whole re-executed frame
#ifdef PBD_DIAGNOSTIC_BUILD
		if (c.steer_stage && !replay && address == 0xb0cf0 && static_cast<u32>(ppu.lr) == 0x5b3ec)
		{
			c.select_work = static_cast<u32>(ppu.gpr[3]);
			c.select_players = static_cast<u32>(ppu.gpr[4]);
		}
#endif
		skip = replay && !c.skip.empty() && std::binary_search(c.skip.begin(), c.skip.end(), address);
		return {};
	}

	std::optional<hook_action> hook_event::character_policy()
	{
		const auto character_action = pbd::character_rule::event(ppu, address, can_skip);
		if (character_action == pbd::character_rule::action::refused)
		{
			ppu.state += cpu_flag::exit;
			return hook_action::fatal;
		}
		if (character_action == pbd::character_rule::action::skip)
			return hook_action::skip;
		return {};
	}

	std::optional<hook_action> hook_event::vs_gate()
	{
		const u32 spectator_init = address == 0x2739f4 ? spectator_vs_init_gate.load(std::memory_order_acquire) : 0u;
		const bool rematch_init = address == 0x2739f4 && rematch_vs_init_gate.load(std::memory_order_acquire);
		if (address == 0x2739f4 && (spectator_init || rematch_init) && static_cast<u32>(ppu.lr) == 0x5b240)
		{
			// Natural continuation runs 0x5af04: 0x5b1ec/0x5b1fc choose resource slots,
			// then this last weak-handle readiness test precedes the scene-2 request.
			// Resource state 7 is set by a guest callback on this PPU, so keep that
			// callback running and hold only this last test while the source stays in scene 1.
			const u32 id = static_cast<u32>(ppu.gpr[4]);
			const bool pinned = can_skip && id <= 0xc6 && vm::check_addr(0x2739f4, vm::page_readable, 0xcc) &&
			                    +vm::read32(0x2739f4) == 0x3800ffb0 && +vm::read32(0x273a30) == 0x8002e0fc &&
			                    +vm::read32(0x273abc) == 0x4bfffa04 &&
			                    vm::check_addr(0x2734c0, vm::page_readable, 20) && +vm::read32(0x2734c0) == 0x80030048 &&
			                    +vm::read32(0x2734c4) == 0x2f800007 && +vm::read32(0x2734d0) == 0x4e800020 &&
			                    vm::check_addr(0x5b234, vm::page_readable, 0x18) && +vm::read32(0x5b23c) == 0x482187b9 &&
			                    +vm::read32(0x5b244) == 0x2fa30000 && +vm::read32(0x5b248) == 0x419e02d8 &&
			                    vm::check_addr(pbd::t5dr::scene_address, vm::page_readable, 8) &&
			                    +vm::read32(pbd::t5dr::scene_address) == 1 && +vm::read32(pbd::t5dr::subscene_address) == 44 &&
			                    vm::check_addr(pbd::t5dr::game_mode_address, vm::page_readable, 4) &&
			                    +vm::read32(pbd::t5dr::game_mode_address) == 4;
			if (!pinned)
			{
				pbd_fn_log.error("VS init gate: continuation readiness call or VS scene changed");
				ppu.state += cpu_flag::exit;
				return hook_action::fatal;
			}
			if (spectator_init == 1 || rematch_init)
			{
				// An absent/stale weak handle follows the ordinary predicate, including its cleanup.
				if (!vm::check_addr(0x46e63c, vm::page_readable, 4))
				{
					ppu.state += cpu_flag::exit;
					return hook_action::fatal;
				}
				const u32 table = +vm::read32(0x46e63c);
				if (!table || table > 0xfffffff8u - 8u * id || !vm::check_addr(table + 8u * id, vm::page_readable, 8))
				{
					ppu.state += cpu_flag::exit;
					return hook_action::fatal;
				}
				const u32 node = +vm::read32(table + 8u * id);
				const bool valid_handle = node && vm::check_addr(node, vm::page_readable, 0x4c) &&
				                          +vm::read32(node + 0xc) == +vm::read32(table + 8u * id + 4);
				const auto gate = pbd::rematch::gate_resource(rematch_init, spectator_init == 1, valid_handle,
					valid_handle && +vm::read32(node + 0x48) == 7u, gate_hold.load(std::memory_order_acquire));
				if (gate.announce)
				{
					u32 none = 0;
					gate_ready_tick.compare_exchange_strong(none, current_tick.load() + 1);
				}
				if (gate.hold)
				{
					// Hold only the verified, side-effect-free readiness result; guest resource callbacks keep running.
					ppu.gpr[3] = 0;
					return hook_action::skip;
				}
			}
			// Release through the original predicate; never supply a ready result or resource state.
		}
		return {};
	}

	void hook_event::record_event()
	{
		c.calls++;
		c.skips += skip;
		if (c.max_events)
		{
			const auto now = hook_clock::now();
			if (!c.origin_set)
			{
				c.origin = now;
				c.origin_set = true;
			}
			if (c.used < c.events.size())
			{
				auto& row = c.events[c.used++];
				row.address = address;
				row.tick = current_tick.load(std::memory_order_relaxed);
				row.flags = (replay ? 1u : 0u) | (skip ? 2u : 0u);
				row.ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - c.origin).count());
				row.reg0 = task ? task : (c.reg0 >= 0 ? (c.reg0 == 32 ? ppu.lr : ppu.gpr[c.reg0]) : 0);
				row.reg1 = c.reg1 >= 0 ? (c.reg1 == 32 ? ppu.lr : ppu.gpr[c.reg1]) : 0;
				row.cyc = pbd::owner_timeline::enabled ? pbd::owner_timeline::thread_cycles() : 0;
			}
			else
			{
				c.overflow = true;
			}
		}
	}

	hook_action hook_event::run()
	{
		if (const auto action = admit())
			return *action;
		trace_render();
		if (const auto action = detail::voice_actions(ppu, address))
			return *action;
		if (const auto action = owner_entry())
			return *action;
		if (address == pbd::t5dr::gpu_load_probe && g_fxo->is_init<pbd::rollback_session>())
			ppu.gpr[3] = g_fxo->get<pbd::rollback_session>().gpu_elapsed_time(ppu, ppu.gpr[3]);
		player_draw_gate();
		effects_draw_gate();
		camera_gate();
		skip_unless();
		can_skip = std::binary_search(c.skippable.begin(), c.skippable.end(), address);
		if (const auto action = character_policy())
			return *action;
		if (const auto action = vs_gate())
			return *action;
		task_dispatch();
		skip_guard();
		random_event random{random_runtime(), ppu, address, replay, skip, task, task_slot};
		random.inspect_mt();
		random.inspect_keyed_replay();
		random.draw_diagnostics();
		random.random_audit();
		wait_resources();
		session_exit_gate();
		random.seed_mobs();
		random.seed_stage();
		clear_exit_gate();
		random.reseed_match();
		early_flush();
		random.random_fence();
		random.repair_random();
		record_event();
		return skip ? hook_action::skip : hook_action::continue_guest;
	}

} // namespace pbd::fn_hook::detail
