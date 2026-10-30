#include "stdafx.h"
#include "pbd_function_hook_event.h"

namespace pbd::fn_hook::detail
{
	// Resolve a task's function descriptor at node+0x2c for slot 8, falling back to its vtable slot.
	// Return the descriptor's guest entry address, or zero if any required word is unreadable.
	static u32 resolve_task(u32 node, u32 slot) noexcept
	{
		if (!node || !vm::check_addr(node, vm::page_readable, 0x30))
			return 0;
		u32 descriptor = slot == 8 ? +vm::read32(node + 0x2c) : 0u;
		if (!descriptor)
		{
			const u32 table = vm::read32(node);
			if (!table || !vm::check_addr(table, vm::page_readable, slot + 4))
				return 0;
			descriptor = vm::read32(table + slot);
		}
		if (!descriptor || !vm::check_addr(descriptor, vm::page_readable, 8))
			return 0;
		return vm::read32(descriptor);
	}
	void hook_event::player_draw_gate()
	{
		// Verdicts use the corrected guest state: 0 = eligible, 1 = r24 == 4 (source value 3), 2 = unknown vtable,
		// 3 = unreadable data. Live frames count would-be vetoes without suppressing the draw.
		if (address == 0x2649a8 && c.gate_player_draw && (skip || !replay))
		{
			const auto word = [](u32 a, u32& v)
			{
				if (!a || !vm::check_addr(a, vm::page_readable, 4))
					return false;
				v = vm::read32(a);
				return true;
			};
			const u32 self = static_cast<u32>(ppu.gpr[3]);
			u32 vptr = 0, value = 0, p1 = 0, p2 = 0;
			u32 verdict = 3;
			if (!word(self, vptr))
				verdict = 3;
			else if (vptr == 0x100841b4u)
				verdict = word(self + 0x50, value) ? (static_cast<s32>(value) != 3 ? 0 : 1) : 3;
			else if (vptr == 0x100841d8u)
				verdict = word(0x469dbc, p1) && word(p1 + 4, p2) && vm::check_addr(p2 + 0x12, vm::page_readable, 2) ? (static_cast<s16>(static_cast<u16>(vm::read16(p2 + 0x12))) != 3 ? 0 : 1) : 3;
			else
				verdict = 2;
			if (!replay)
			{
				c.gate_player_live_calls++;
				if (verdict)
				{
					c.gate_player_live_would_veto[verdict]++;
					if (c.gate_player_first_live_veto_tick == 0xffffffffu)
						c.gate_player_first_live_veto_tick = current_tick.load(std::memory_order_relaxed);
				}
			}
			else if (!verdict)
				c.gate_player_skipped++;
			else
			{
				skip = false;
				c.gate_player_vetoed++;
				c.skip_vetoed++;
				c.gate_player_veto_reason[verdict]++;
			}
		}
	}

	void hook_event::effects_draw_gate()
	{
		if (address == 0x9f8d0 && c.gate_effects_draw && (skip || !replay))
		{
			const auto word = [](u32 a, u32& v)
			{
				if (!a || !vm::check_addr(a, vm::page_readable, 4))
					return false;
				v = vm::read32(a);
				return true;
			};
			const u32 self = static_cast<u32>(ppu.gpr[3]);
			u32 vptr = 0, i = 0, w1 = 0, w2 = 0;
			u32 verdict = 3;
			if (!word(self, vptr))
				verdict = 3;
			else if (vptr != 0x100649ecu)
				verdict = 2;
			else if (!word(self + 0x5c, i) || i > 15)
				verdict = 3;
			else if (!word(0x100537fcu + 16 * i, w1) || !word(0x100537fcu + 16 * i + 0x100, w2))
				verdict = 3;
			else
				verdict = (w1 == 2u || w2 == 2u) ? 1 : 0;
			if (!replay)
			{
				c.gate_effects_live_calls++;
				if (verdict)
					c.gate_effects_live_would_veto[verdict]++;
			}
			else if (!verdict)
				c.gate_effects_skipped++;
			else
			{
				skip = false;
				c.gate_effects_vetoed++;
				c.skip_vetoed++;
				c.gate_effects_veto_reason[verdict]++;
			}
		}
	}

	void hook_event::camera_gate()
	{
		if (!replay && address == 0x260d44 && c.gate_stage_camera && (!vm::check_addr(0x10053694, vm::page_readable, 4) || vm::read32(0x10053694) == 2u))
		{
			if (!c.gate_stage_live_mode2++)
				c.gate_stage_first_mode2_tick = current_tick.load(std::memory_order_relaxed);
		}
		if (skip && address == 0x260d44 && c.gate_stage_camera)
		{
#ifdef PBD_DIAGNOSTIC_BUILD
			const bool forced = c.test_stage_camera_force && (++c.test_stage_camera_calls % c.test_stage_camera_force) == 0;
			if (forced)
				c.test_stage_camera_forced++;
#else
			constexpr bool forced = false;
#endif
			// Ominous Mist's stage layer sets fog before drawing and clears it afterward (0xa645c / 0xa6398).
			// Keep those render-state updates through corrections so the next visible frame has the same fog state.
			constexpr u32 stage = 0x1008f088, ominous_mist = 34;
			if (!forced && vm::check_addr(0x10053694, vm::page_readable, 4) && vm::read32(0x10053694) != 2u &&
				vm::check_addr(stage, vm::page_readable, 4) && vm::read32(stage) != ominous_mist)
				c.gate_stage_skipped++;
			else
			{
				skip = false;
				c.gate_stage_vetoed++;
				c.skip_vetoed++;
			}
		}
	}

	void hook_event::skip_unless()
	{
		if (skip && !c.skip_unless.empty())
		{
			for (const auto& [entry, word] : c.skip_unless)
				if (entry == address && (!vm::check_addr(word, vm::page_readable, 4) || vm::read32(word) != 0u))
				{
					skip = false;
					c.skip_vetoed++;
					break;
				}
		}
	}

	void hook_event::task_dispatch()
	{
		for (const auto& site : c.dispatch)
		{
			if (site.first != address)
				continue;
			task = resolve_task(static_cast<u32>(ppu.gpr[3]), site.second);
			task_slot = site.second;
			if (!skip && replay && can_skip && task && !c.skip_tasks.empty() &&
				std::binary_search(c.skip_tasks.begin(), c.skip_tasks.end(), task))
				skip = true;
			else if (!skip && replay && !can_skip && task && !c.skip_tasks.empty() &&
					 std::binary_search(c.skip_tasks.begin(), c.skip_tasks.end(), task))
				c.skip_ignored++;
			break;
		}
	}

	void hook_event::skip_guard()
	{
		if (c.skip_guard_record)
		{
			const bool candidate = std::binary_search(universal_skip_entries.begin(), universal_skip_entries.end(), address);
			const u32 sp = static_cast<u32>(ppu.gpr[1]), lr = static_cast<u32>(ppu.lr), node = static_cast<u32>(ppu.gpr[3]);
			if (!replay)
			{
				skip_guard_history::tls_state tls{};
				const u32 r13 = static_cast<u32>(ppu.gpr[13]);
				const bool read = vm::try_access(r13 - 0x7000, tls.data(), 4, false) &&
				                  vm::try_access(r13 + 0x9094, tls.data() + 4, 1, false) && vm::try_access(r13 + 0x9098, tls.data() + 5, 132, false) &&
				                  (!candidate || (node && vm::check_addr(node, vm::page_readable, 4)));
				const u32 core = address == 0x3afc3c ? 1u : address == 0xe859c ? 2u :
				                                        address == 0x66950     ? 4u :
				                                                                 0u;
				c.guard_history.observe(address, lr, node, sp, candidate, core, read, tls,
#ifdef PBD_DIAGNOSTIC_BUILD
					c.skip_guard_test_taint
#else
					false
#endif
				);
			}
			if (c.skip_guard)
			{
				// This mode authorizes ONLY its own guarded entry set, and never bypasses a refusal
				// through the FN_SKIP/FN_SKIP_TASKS lists. Live execution always stays unmodified.
				skip = false;
				if (replay && can_skip && candidate && std::binary_search(c.guard_entries.begin(), c.guard_entries.end(), address))
				{
					const auto why = c.guard_history.allow(current_tick.load(), {address, lr, node}, sp,
#ifdef PBD_DIAGNOSTIC_BUILD
						c.skip_guard_test_missing
#else
						false
#endif
					);
					skip = why == skip_guard_history::allowed;
				}
			}
		}
	}

	void hook_event::wait_resources()
	{
		if ((address == 0x9a884 || address == 0x1ebe1c) && c.blocking_loads && !replay)
		{
			// Deterministic loading (see config::blocking_loads). Only outside the battle (16 / 17).
			const u32 scene = vm::check_addr(pbd::t5dr::scene_address, vm::page_readable, 4) ? +vm::read32(pbd::t5dr::scene_address) : 17u;
			if (scene != 17u && scene != 16u)
			{
				const auto from = std::chrono::steady_clock::now();
				const auto until = from + std::chrono::seconds(3);
				bool unlocked = false, timed_out = false;
				for (;;)
				{
					bool ready = true;
					if (address == 0x9a884)
					{
						const auto sema = idm::check_unlocked<lv2_obj, lv2_sema>(static_cast<u32>(ppu.gpr[3]));
						ready = !sema || sema->val.load() > 0;
					}
					else
					{
						const u32 cmd = static_cast<u32>(ppu.gpr[3]);
						ready = !vm::check_addr(cmd + 1, vm::page_readable, 1) || vm::read8(cmd + 1) != 2;
					}
					if (ready)
						break;
					if (std::chrono::steady_clock::now() >= until)
					{
						timed_out = true;
						break;
					}
					if (!unlocked)
					{
						vm::temporary_unlock(ppu);
						unlocked = true;
					}
					std::this_thread::sleep_for(std::chrono::microseconds(200));
				}
				if (timed_out)
					pbd_fn_log.error("session blocking load: wait at 0x%x timed out (scene %u, tick %u)", address, scene, current_tick.load());
			}
		}
	}

	void hook_event::session_exit_gate()
	{
		if (address == 0x52288 || address == 0x117c70)
		{
			// Session exit gate (latch storage::session_exit_gate): the LAST readiness test of scene 12 (0x52288, third slot) or scene 8
			// (0x117c70, slot 0x32; 0x117b68 is only trusted in scene 8). r4 = the slot id. The test 0x2739f4 reads the weak handle {node,
			// serial} at *(r2 - 0x1f04) + 8 * id (valid when node+0xc == serial) and 0x2734c0 answers node+0x48 == 7. Reaching this call means
			// every earlier test of the screen passed, so "this slot ready" = "the screen would end in this tick".
			const u32 scene = vm::check_addr(pbd::t5dr::scene_address, vm::page_readable, 4) ? +vm::read32(pbd::t5dr::scene_address) : 0u;
			const u32 id = static_cast<u32>(ppu.gpr[4]);
			if (scene == (address == 0x52288 ? 12u : 8u) && id <= 0xc6 && vm::check_addr(0x46e63c, vm::page_readable, 4))
			{
				const u32 entry = +vm::read32(0x46e63c) + 8 * id;
				const u32 node = vm::check_addr(entry, vm::page_readable, 8) ? +vm::read32(entry) : 0u;
				if (node && vm::check_addr(node, vm::page_readable, 0x4c) && +vm::read32(node + 0xc) == +vm::read32(entry + 4) && +vm::read32(node + 0x48) == 7u)
				{
					u32 none = 0;
					gate_ready_tick.compare_exchange_strong(none, current_tick.load() + 1);
					if (gate_hold.load())
					{
						// Only the owner thread runs the screen and every state transition (0x272340), so masking the word for this one call
						// is invisible to anything else; the probe after the call puts 7 back.
						vm::write32(node + 0x48, 0x107u);
						c.gate_node = node;
						pbd_fn_log.warning("session exit gate: scene %u held at tick %u (slot 0x%x ready since tick %u)", scene, current_tick.load(), id, gate_ready_tick.load() - 1);
					}
				}
			}
		}
	}

	void hook_event::clear_exit_gate()
	{
		if ((address == 0x52290 || address == 0x117c78) && c.gate_node)
		{
			vm::write32(c.gate_node + 0x48, 7u);
			c.gate_node = 0;
		}
	}

	void hook_event::early_flush()
	{
#ifdef PBD_DIAGNOSTIC_BUILD
		if (address == 0x34b9d8 && ((c.test_early_flush && replay) || (c.early_flush_live && !replay)))
		{
			// Publish PUT after the flip tail, before the guest polls and resets its status. Re-execution requires counted
			// flip completions in sys_rsx.cpp because the renderer can finish before that poll.
			const u32 holder = vm::check_addr(0x46ff28, vm::page_readable, 4) ? +vm::read32(0x46ff28) : 0u;
			const u32 ctx = holder && vm::check_addr(holder, vm::page_readable, 4) ? +vm::read32(holder) : 0u;
			const u32 cur = ctx && vm::check_addr(ctx, vm::page_readable, 12) ? +vm::read32(ctx + 8) : 0u;
			if (const auto render = rsx::get_current_renderer(); render && render->ctrl && cur >= 0x40100000u && cur < 0x40500000u && !(cur & 3) &&
																 render->iomap_table.get_addr(cur - 0x40100000u) == cur)
			{
				render->ctrl->put = cur - 0x40100000u;
				c.test_early_flushes++;
			}
			else
				c.test_early_flush_refused++;
		}
#endif
	}

} // namespace pbd::fn_hook::detail
