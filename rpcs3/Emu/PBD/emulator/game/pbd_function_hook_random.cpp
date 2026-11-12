#include "stdafx.h"
#include "pbd_function_hook_event.h"

namespace pbd::fn_hook::detail
{
	random_state& random_runtime() noexcept
	{
		static random_state instance;
		return instance;
	}

	void initialize_wolf_motion(ppu_thread& ppu)
	{
		using namespace pbd::t5dr::wolf_motion;
		const u32 actor = static_cast<u32>(ppu.gpr[4]);
		const u32 controller = static_cast<u32>(ppu.gpr[3]);
		if (ppu.gpr[2] != pbd::t5dr::toc || ppu.lr != caller || !actor || actor > 0xfffff72fu ||
			Emu.GetTitleID() != pbd::t5dr::title_id ||
			Emu.GetExecutableHash() != pbd::t5dr::executable_hash ||
			!vm::check_addr(stage_address, vm::page_readable, 4) || vm::read32(stage_address) != stage ||
			!vm::check_addr(call_site, vm::page_readable, 4) || vm::read32(call_site) != call_opcode ||
			!vm::check_addr(store_site, vm::page_readable, 4) || vm::read32(store_site) != store_opcode ||
			!vm::check_addr(request_site, vm::page_readable, 4) || vm::read32(request_site) != request_opcode ||
			!vm::check_addr(actor + actor_controller, vm::page_readable, 4) ||
			static_cast<u64>(vm::read32(actor + actor_controller)) + controller_bias != controller ||
			!vm::check_addr(actor + request, vm::page_writable, 1))
			return;

		// No movement is pending at construction. Only the stock request routine sets this byte afterward.
		vm::write8(actor + request, 0);
	}

	void random_event::inspect_mt()
	{
		if (address == 0x66950 && !replay)
		{
			// MT in fights (see random_state::mt_in_fight): owner thread, live pass, battle scene. The core is always reached through a wrapper
			// (0x66b14..0x66db4, 0x162204): its caller's return address is the LR the wrapper saved in ITS caller's frame (back chain + 0x10).
			const u32 scene = vm::check_addr(pbd::t5dr::scene_address, vm::page_readable, 4) ? +vm::read32(pbd::t5dr::scene_address) : 0u;
			if (scene == 17u || scene == 16u)
			{
				u32 caller = static_cast<u32>(ppu.lr);
				const u32 sp = static_cast<u32>(ppu.gpr[1]);
				if (((caller >= 0x66b14 && caller < 0x66db4) || (caller >= 0x162204 && caller < 0x1622c0)) && vm::check_addr(sp, vm::page_readable, 4))
				{
					const u32 back = vm::read32(sp);
					if (back && vm::check_addr(back + 0x10, vm::page_readable, 8))
						caller = static_cast<u32>(vm::read64(back + 0x10));
				}
				if (c.mt_in_fight[caller]++ == 0)
					pbd_fn_log.error("Mersenne Twister drawn in a fight (tick %u, caller 0x%x): its state differs between PCs by design", current_tick.load(), caller);
			}
		}
	}

	void random_event::inspect_keyed_replay()
	{
		if (c.keyed_draws && replay && address == 0x3afc3c && draw_statistics_active.load(std::memory_order_relaxed))
		{
			// Keyed-draw check (see pbd::fn_hook::keyed_in_replay).
			if (keyed_in_replay.fetch_add(1) == 0)
				pbd_fn_log.error("Keyed draws: rand() called inside the keyed window during a RE-EXECUTED frame (tick %u, LR 0x%x)", current_tick.load(), static_cast<u32>(ppu.lr));
		}
	}

	void random_event::draw_diagnostics()
	{
		if ((c.draw_diagnostics || c.keyed_draws) && !replay && draw_statistics_active.load(std::memory_order_relaxed))
		{
			// Draw census (see pbd::fn_hook::census). Record only.
			static constexpr std::array<std::pair<u32, u32>, 9> wrappers{{{0x121de4, 0x121e70}, {0xe85c8, 0xe8674}, {0xe8674, 0xe8718}, {0x66b14, 0x66b38},
				{0x66b38, 0x66bbc}, {0x66bbc, 0x66c40}, {0x66c40, 0x66cd4}, {0x66cd4, 0x66db4}, {0x162204, 0x1622c0}}};
			const u32 r1 = static_cast<u32>(ppu.gpr[1]);
			const u32 lr = static_cast<u32>(ppu.lr);
			while (!c.draw_stack.empty() && r1 >= c.draw_stack.back().r1)
				c.draw_stack.pop_back();
			bool wrapper = false;
			for (const auto& [start, end] : wrappers)
				if (address == start)
				{
					if (c.draw_stack.size() < 16)
						c.draw_stack.push_back({start, end, lr, r1});
					wrapper = true;
					break;
				}
			if (!wrapper && (address == 0x3afc3c || address == 0xe859c || address == 0x66950))
			{
				u32 caller = lr;
				for (auto it = c.draw_stack.rbegin(); it != c.draw_stack.rend() && caller >= it->start && caller < it->end; ++it)
					caller = it->lr;
				u32 generator = address == 0x3afc3c ? 1u : address == 0x66950 ? 3u :
				                                                                static_cast<u32>(ppu.gpr[3]);
				std::array<u8, 8> state{};
				const u32 r13 = static_cast<u32>(ppu.gpr[13]);
				keyed_draw(caller, r13);
				if (c.draw_diagnostics)
				{
					if (address == 0x3afc3c)
					{
						vm::try_access(r13 - pbd::t5dr::random_tls::seed_before_r13, state.data(), 4, false);
						vm::try_access(r13 + pbd::t5dr::random_tls::flag_after_r13, state.data() + 4, 1, false);
					}
					else if (address == 0xe859c)
						vm::try_access(generator, state.data(), 4, false);
					auto& entry = draw_statistics[{generator, caller}];
					entry.first++;
					if (!entry.second)
						entry.second = 0xcbf29ce484222325ull;
					for (u8 b : state)
						entry.second = (entry.second ^ b) * 0x100000001b3ull;
				}
			}
		}
	}

	void random_event::random_audit()
	{
		if (!c.rng_audit.empty() && !replay)
		{
			// RNG audit (see random_state::rng_audit). Record only: nothing here writes guest memory or changes the answer.
			const u32 r1 = static_cast<u32>(ppu.gpr[1]);
			const u32 r13 = static_cast<u32>(ppu.gpr[13]);
			while (!c.audit_stack.empty())
			{
				const auto& top = c.audit_stack.back();
				if (address == top.lr && r1 == top.r1)
				{
					close_audit(top, true, r13);
					c.audit_stack.pop_back();
					break;
				}
				if (r1 > top.r1 || r1 == top.r1)
				{
					close_audit(top, false, r13);
					c.audit_stack.pop_back();
					continue;
				}
				break;
			}
			if (address == 0x3afc3c || address == 0xe859c || address == 0x66950)
			{
				const u32 bit = address == 0x3afc3c ? 1u : address == 0xe859c ? 2u :
				                                                                4u;
				for (auto& f : c.audit_stack)
					f.drew |= bit;
			}
			if (std::binary_search(c.rng_audit.begin(), c.rng_audit.end(), address))
			{
				c.audit_stats[{address, c.epoch.match_seeds}].calls++;
				const u32 lr = static_cast<u32>(ppu.lr);
				if (auto& set = c.audit_returns[address]; set.size() < 32)
					set.insert(lr);
				if (c.audit_stack.size() < 64)
				{
					random_state::audit_frame f;
					f.entry = address;
					f.r1 = r1;
					f.lr = lr;
					f.tick = current_tick.load(std::memory_order_relaxed);
					f.read = read_audit_tls(r13, f.tls);
					c.audit_stack.push_back(f);
				}
				else
					c.audit_overflow++;
			}
		}
	}

	void random_event::seed_mobs()
	{
		if (address == 0xcb96c && c.stage_sync && c.match_seed_set)
			initialize_wolf_motion(ppu);
		if (address == 0xcb96c && c.stage_sync && c.match_seed_set && !replay)
		{
			// Mob batch sync (see random_state::epoch). Only outside the battle.
			const u32 scene = vm::check_addr(pbd::t5dr::scene_address, vm::page_readable, 4) ? +vm::read32(pbd::t5dr::scene_address) : 17u;
			const u32 tick = current_tick.load();
			const u32 r13 = static_cast<u32>(ppu.gpr[13]);
			if (scene != 17u && scene != 16u && stage_controllers.size() < 4096)
				stage_controllers.push_back(static_cast<u32>(ppu.gpr[3]));
			const bool writable = vm::check_addr(r13 - pbd::t5dr::random_tls::seed_before_r13, vm::page_writable, 4) && vm::check_addr(r13 + pbd::t5dr::random_tls::flag_after_r13, vm::page_writable, 1);
			if (const u32 batch = scene != 17u && scene != 16u && writable ? c.epoch.mobs.open(tick) : 0u)
			{
				const u32 seed = pbd::stage_sync::mob_batch_seed(c.match_seed_base, c.epoch.match_seeds, batch);
				vm::write32(r13 - pbd::t5dr::random_tls::seed_before_r13, seed);
				vm::write8(r13 + pbd::t5dr::random_tls::flag_after_r13, 0);
				pbd_fn_log.warning("session stage sync: libc rand() reseeded to %u before mob batch %u at tick %u (scene %u)", seed, batch, tick, scene);
			}
		}
	}

	void random_event::seed_stage()
	{
		if (address == 0x52298 && c.stage_sync && c.match_seed_set && !replay)
		{
			// Stage sync (see random_state::stage_sync). Only on scene 12's exit frame.
			const u32 scene = vm::check_addr(pbd::t5dr::scene_address, vm::page_readable, 4) ? +vm::read32(pbd::t5dr::scene_address) : 0u;
			const u32 r13 = static_cast<u32>(ppu.gpr[13]);
			if (scene == 12u && vm::check_addr(r13 - pbd::t5dr::random_tls::seed_before_r13, vm::page_writable, 4) && vm::check_addr(r13 + pbd::t5dr::random_tls::flag_after_r13, vm::page_writable, 1))
			{
				const u32 seed = pbd::stage_sync::screen_seed(c.match_seed_base, c.epoch.match_seeds);
				vm::write32(r13 - pbd::t5dr::random_tls::seed_before_r13, seed);
				vm::write8(r13 + pbd::t5dr::random_tls::flag_after_r13, 0);
				pbd_fn_log.warning("session stage sync: libc rand() reseeded to %u at tick %u (match %u)", seed, current_tick.load(), c.epoch.match_seeds + 1);
			}
		}
	}

	void random_event::reseed_match()
	{
		if (address == 0x15a88)
		{
			// Match seed (see random_state::match_seed_set). r26 = the reseed kind (0x15a80 compares it with 9).
			const u32 kind = static_cast<u32>(ppu.gpr[26]);
			const u32 before = vm::check_addr(pbd::t5dr::frame_counter, vm::page_readable, 4) ? +vm::read32(pbd::t5dr::frame_counter) : 0u;
			u32 after = before;
			if (c.match_seed_set && (kind == 2 || kind == 8) && !replay && vm::check_addr(pbd::t5dr::frame_counter, vm::page_writable, 4))
			{
				// Match n of the session. High bit set and 2^24 apart per match: always above any real counter (frames since power-on) and above
				// the previous match's counter, so the game's comparisons with cached frame numbers stay monotone.
				after = 0x80000000u | ((c.match_seed_base & 0x00ffffffu) + (std::min<u32>(c.epoch.match_seeds, 126u) + 1u) * 0x01000000u);
				vm::write32(pbd::t5dr::frame_counter, after);
				c.epoch.match_seeds++;
				draw_statistics_active.store(0); // draw census: the match-start reseed ends the census window until the next menu barrier
				c.epoch.mobs.begin_match();
			}
			pbd_fn_log.warning("session reseed: kind %u, tick %u, replay %d, frame counter %u -> %u (match seeds %u)", kind, current_tick.load(), replay ? 1 : 0, before, after, c.epoch.match_seeds);
		}
	}

	void random_event::random_fence()
	{
		for (const auto& [entry, exit] : c.fence)
		{
			if (address != entry && address != exit)
				continue;
			const u32 base = static_cast<u32>(ppu.gpr[13]);
			const auto mapped = [&](u8 flags)
			{
				return vm::check_addr(base - random_state::gen_seed_at, flags, 4) && vm::check_addr(base + random_state::gen_flag_at, flags, 0x88);
			};
			if (address == entry)
			{
				if (skip)
					break; // the fenced code does not run at all
				if (c.fence_depth++)
				{
					c.fence_faults++;
					break;
				} // re-entered: the outer bracket owns it
				if (!mapped(vm::page_readable))
				{
					c.fence_depth = 0;
					c.fence_faults++;
					break;
				}
				c.fence_entry_open = entry;
				c.fence_sp = ppu.gpr[1]; // r1 before the prologue: the same value at the blr
				c.saved_seed = vm::read32(base - random_state::gen_seed_at);
				c.saved_flag = vm::read8(base + random_state::gen_flag_at);
				c.saved_last = vm::read32(base + random_state::gen_flag_at + 4);
				for (u32 i = 0; i < 32; i++)
					c.saved_table[i] = vm::read32(base + random_state::gen_flag_at + 8 + i * 4);
				c.fence_opened++;
				break;
			}
			if (!c.fence_depth || c.fence_entry_open != entry)
			{
				c.fence_faults++;
				break;
			}
			if (--c.fence_depth)
				break; // inner return: the outer bracket closes it
			if (ppu.gpr[1] != c.fence_sp || !mapped(vm::page_writable))
			{
				c.fence_faults++;
				break;
			} // mispaired: leave the guest alone
			restore_random_fence(base);
			break;
		}
	}

	void random_event::repair_random()
	{
		if (c.rng_repair && c.fence.empty())
		{
			const u32 tick = current_tick.load(std::memory_order_relaxed);
			const u32 seed_at = static_cast<u32>(ppu.gpr[13]) - c.rng_offset;
			if (task == c.rng_repair && task_slot == 8 && !replay)
			{
				// Live pass: this task is starting. Its seed is closed at the next rng_exit probe.
				c.rng_inside = true;
				c.rng_inside_tick = tick;
			}
			else if (address == c.rng_exit && c.rng_inside)
			{
				c.rng_inside = false;
				if (vm::check_addr(seed_at, vm::page_readable, 4))
				{
					auto& into = c.rng_after[c.rng_inside_tick & 255];
					into.tick = c.rng_inside_tick;
					into.seed = vm::read32(seed_at);
					if (!vm::try_access(static_cast<u32>(ppu.gpr[13]) + random_state::rng_tail_offset, into.tail.data(), random_state::rng_tail_size, false))
						into.tick = 0xffffffffu;
					c.rng_recorded++;
				}
			}
			else if (task == c.rng_repair && task_slot == 8 && replay && skip)
			{
				// Restore the live state only for a unique reported tick: repeated indices cannot identify another frame.
				const u32 want = tick;
				const bool repeated = (tick == c.rng_window_tick);
				c.rng_window_tick = tick;
				const auto& slot = c.rng_after[want & 255];
				if (!repeated && slot.tick == want && vm::check_addr(seed_at, vm::page_writable, 4))
				{
					// The ordinary guest alias triggers rollback write capture; vm::try_access's SUDO alias bypasses it.
					vm::write32(seed_at, slot.seed);
					const u32 tail_at = static_cast<u32>(ppu.gpr[13]) + random_state::rng_tail_offset;
					for (u32 o = 0; o + 4 <= random_state::rng_tail_size; o += 4)
					{
						u32 word = 0;
						std::memcpy(&word, slot.tail.data() + o, 4);
						vm::write32(tail_at + o, std::byteswap(word)); // tail bytes are guest order; write32 swaps again
					}
					c.rng_applied++;
				}
				else
				{
					c.rng_missing++;
				}
			}
		}
	}

	void random_event::keyed_draw(u32 caller, u32 r13)
	{
		if (c.keyed_draws && c.match_seed_set && address == 0x3afc3c && vm::check_addr(r13 - pbd::t5dr::random_tls::seed_before_r13, vm::page_writable, 4) && vm::check_addr(r13 + pbd::t5dr::random_tls::flag_after_r13, vm::page_writable, 1))
		{
			// Keyed draw (see pbd::fn_hook::keyed_segment): srand(key) state, then the stock rand() runs (cold init, draw).
			const u32 count = keyed_counts[caller]++;
			u64 h = 0x9e3779b97f4a7c15ull ^ c.match_seed_base;
			for (const u32 part : {keyed_segment.load(std::memory_order_relaxed), caller, count})
			{
				h ^= part;
				h *= 0xbf58476d1ce4e5b9ull;
				h ^= h >> 31;
			}
			const u32 key = static_cast<u32>(h ^ (h >> 32)) | 1u;
			vm::write32(r13 - pbd::t5dr::random_tls::seed_before_r13, key);
			vm::write8(r13 + pbd::t5dr::random_tls::flag_after_r13, 0);
			c.keyed++;
		}
	}

	bool random_event::read_audit_tls(u32 r13, std::array<u8, 137>& out)
	{
		return vm::try_access(r13 - pbd::t5dr::random_tls::seed_before_r13, out.data(), 4, false) && vm::try_access(r13 + pbd::t5dr::random_tls::flag_after_r13, out.data() + 4, 1, false) &&
		       vm::try_access(r13 + pbd::t5dr::random_tls::words_after_r13, out.data() + 5, 132, false);
	}

	void random_event::close_audit(const random_state::audit_frame& f, bool matched, u32 r13)
	{
		auto& s = c.audit_stats[{f.entry, c.epoch.match_seeds}];
		if (!matched)
		{
			s.unmatched++;
			return;
		}
		s.matched++;
		if (f.drew & 1)
			s.drew_rand++;
		if (f.drew & 2)
			s.drew_lcg++;
		if (f.drew & 4)
			s.drew_mt++;
		if (f.drew && s.first_advanced_tick == ~0u)
			s.first_advanced_tick = f.tick;
		std::array<u8, 137> now{};
		if (!f.read || !read_audit_tls(r13, now))
		{
			c.audit_unreadable++;
			return;
		}
		if (now == f.tls && !f.drew)
			return;
		s.advanced++;
		if (now == f.tls)
			return;
		u32 before = 0, after = 0;
		for (u32 i = 0; i < 4; i++)
		{
			before = (before << 8) | f.tls[i];
			after = (after << 8) | now[i];
		}
		u64 steps = 0;
		for (u32 v = before; v != after && steps < 65536; steps++)
			v = 1664525u * v + 1013904223u;
		s.steps += steps;
		s.max_steps = std::max(s.max_steps, steps);
		if (s.first_advanced_tick == ~0u)
			s.first_advanced_tick = f.tick;
	}

	void random_event::restore_random_fence(u32 base)
	{
		const u32 seed_now = vm::read32(base - random_state::gen_seed_at);
		if (seed_now == c.saved_seed)
		{
			c.fence_idle++;
			return;
		} // nothing was drawn
		{
			u32 v = c.saved_seed, n = 0;
			while (n < 64 && v != seed_now)
			{
				v = 1664525u * v + 1013904223u;
				n++;
			}
			c.fence_draws[v == seed_now ? n : 65]++;
		}
		// Ordinary guest alias (never vm::try_access, which bypasses the write-fault capture of the rollback layer).
		vm::write32(base - random_state::gen_seed_at, c.saved_seed);
		vm::write8(base + random_state::gen_flag_at, c.saved_flag);
		vm::write32(base + random_state::gen_flag_at + 4, c.saved_last);
		for (u32 i = 0; i < 32; i++)
			vm::write32(base + random_state::gen_flag_at + 8 + i * 4, c.saved_table[i]);
		c.fence_restored++;
	}

} // namespace pbd::fn_hook::detail
