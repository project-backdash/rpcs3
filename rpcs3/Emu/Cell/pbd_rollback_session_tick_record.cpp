// The tick record at each boundary, and the test modes that hold the owner at a tick and compare a repeated tick with
// its first run.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"

bool pbd::rollback_session::tick_dispatch::tick_record(bool round_reset, const std::array<be_t<u32>, 2>& scene_now,
	bool soft_mode)
{
	if (data.checkpoint.tick)
	{
		// Each input entry is a tick boundary: exactly one battle update must separate it
		// from the previous boundary. Anything else fails closed.
		auto& tick = data.tick_at(index);
		const bool pbd_round_cut = std::exchange(data.replay.round_reconstructed, false);
		tick.round_reset = round_reset || pbd_round_cut;
		tick.lockstep = phase_lockstep(index);
		if (data.rolling && tick.lockstep && index < data.history.lockstep_ticks.size())
			data.history.lockstep_ticks[index] = 1;
		tick.scene = {u32{scene_now[0]}, u32{scene_now[1]}};
		tick.session_segment = data.session.segments.number;
		tick.gate_hold = pbd::fn_hook::gate_hold.load(std::memory_order_acquire);
		read_word(0x1008f07c, tick.frame_counter);
		if (read_tick_counters(tick, soft_mode))
			return true;
		// Phase-aware hash scope (combat hash v2): the non-fight relaxation applies only under the round lifecycle,
		// decided from the same witness law the phase classification uses below.
		const bool relaxed_scope = data.round_lifecycle && (pbd_round_cut ||
															   (index > data.start_index && data.tick_at(index - 1).read &&
																   tick.battle_counters[1] != data.tick_at(index - 1).battle_counters[1] + 1));
		tick.hash_relaxed = relaxed_scope;
		hash_tick_combat(tick, relaxed_scope);
		hash_tick_generator(tick, relaxed_scope);
		if (verify_tick_phase(tick, pbd_round_cut, soft_mode))
			return true;
		capture_tick_checkpoint(tick);
		capture_replay_checkpoints(tick);
	}
	return false;
}

bool pbd::rollback_session::tick_dispatch::control_hold()
{
	if (data.checkpoint.tick && (index == data.checkpoint.tick_hold_index || index == data.checkpoint.tick_hold_index + 1) && data.roundtrip.phase < 2)
	{
		// Explicit one-tick control boundary: the owner stops at the tick entry holding no
		// VM lock; release admits exactly the next battle update, which the following
		// boundary verifies. Only the owner PPU is held; other guest threads keep running.
		auto& tick = data.tick_at(index);
		tick.held = true;
		vm::temporary_unlock(ppu);
		// Hold probe: capture the tick write set, owner context and lock ownership before and
		// after the hold. Pages that change while the owner is held were written by someone else.

		capture_held_checkpoint("before");
		const auto held_from = session_clock::now();
		while (session_clock::now() - held_from < std::chrono::milliseconds(data.checkpoint.tick_hold_ms) &&
			   !(ppu.state & (cpu_flag::exit + cpu_flag::req_exit)))
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		tick.hold_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - held_from).count();
		capture_held_checkpoint("after");

		capture_roundtrip_origin();
		if (restore_roundtrip_origin())
			return true;
	}
	return false;
}

bool pbd::rollback_session::tick_dispatch::roundtrip_compare()
{
	if (data.roundtrip.active && index == data.checkpoint.tick_hold_index + 1 && data.roundtrip.phase == 2 && !data.roundtrip.repeat_active && data.roundtrip.repeat_completed)
	{
		// Second arrival at boundary N+1 after re-executing tick N with the same input: compare S2 with S1.
		// Requires the completed repeat row, so it can never run in the callback that performed the restore.
		if (repeat_branch_input())
			return true;
		data.roundtrip.phase = 3;
		const bool s2_ok = read_pages(data.checkpoint.checkpoint_pages, data.roundtrip.pages_s2);
		const auto context_s2 = read_context(ppu);
		const auto& current_battle_counters = data.tick_at(index).battle_counters;
		u64 s1_s2 = 0, s1_s2_excluded = 0, n_s1 = 0, n_s1_excluded = 0;
		const auto spans = s2_ok ? data.checkpoint.memory.byte_differences(data.checkpoint.checkpoint_pages, data.roundtrip.pages_s1, data.roundtrip.pages_s2, s1_s2, s1_s2_excluded) : std::string();
		static_cast<void>(data.checkpoint.memory.byte_differences(data.checkpoint.checkpoint_pages, data.roundtrip.pages_n, data.roundtrip.pages_s1, n_s1, n_s1_excluded));
		const auto context_diff = context_differences(data.roundtrip.context_s1, context_s2);
		const auto& w1 = data.roundtrip.saved_battle_counters;
		const auto& wb = data.roundtrip.battle_counters_mismatch;
		u64 s1_branch = 0, s1_branch_excluded = 0;
		u32 player_branch = 0;
		std::string branch_spans, branch_context;
		if (data.roundtrip.branch && data.roundtrip.pass == 2)
		{
			branch_spans = data.checkpoint.memory.byte_differences(data.checkpoint.checkpoint_pages, data.roundtrip.pages_s1, data.roundtrip.pages_wrong, s1_branch, s1_branch_excluded);
			branch_context = context_differences(data.roundtrip.context_s1, data.roundtrip.context_wrong);
			for (usz i = 0; i < data.checkpoint.checkpoint_pages.size(); i++)
				for (u32 o = 0; o < 4096; o++)
				{
					const u32 address = data.checkpoint.checkpoint_pages[i] + o;
					if (address >= 0x10090040 && address < 0x100911e0 && data.roundtrip.pages_s1[i * 4096 + o] != data.roundtrip.pages_wrong[i * 4096 + o])
						player_branch++;
				}
		}
		const auto text = fmt::format(
			"{\"schema\":1,\"source\":\"tick_roundtrip_stage_b\",\"restored\":true,\"hold_index\":%u,"
			"\"identity_restore_ok\":%s,\"identity_written_bytes\":%u,\"identity_readback_differing_bytes\":%u,\"identity_readback_differing_excluded_bytes\":%u,\"identity_context_equal\":%s,"
			"\"restore_readback_ok\":%s,\"restore_readback_differing_bytes\":%u,\"restore_readback_differing_excluded_bytes\":%u,"
			"\"restored_bytes\":%u,\"restore_ns\":%u,"
			"\"locks_free_before_restore\":%s,\"clock_recorded\":%u,\"clock_replayed\":%u,\"clock_error\":%s,"
			"\"repeat_completed\":%s,\"repeat_input_equal\":%s,\"s2_read\":%s,\"tick_changed_bytes_n_to_s1\":%u,\"tick_changed_excluded_bytes\":%u,"
			"\"s1_s2_differing_bytes\":%u,\"s1_s2_differing_excluded_bytes\":%u,\"s1_s2_spans\":[%s],\"context_s1_s2_differences\":[%s],"
			"\"branch\":%s,\"branch_raw\":%u,\"branch_repeat_completed\":%s,\"branch_injected\":[%u,%u],\"s1_branch_differing_bytes\":%u,"
			"\"s1_branch_player_bytes\":%u,\"s1_branch_spans\":[%s],\"branch_context_differences\":[%s],\"witness_branch\":[%u,%u,%u,%u,%u],"
			"\"witness_s1\":[%u,%u,%u,%u,%u],\"witness_s2\":[%u,%u,%u,%u,%u]}\n",
			data.checkpoint.tick_hold_index, json_bool(data.roundtrip.identity_ok), data.roundtrip.identity_written, data.roundtrip.identity_diff, data.roundtrip.identity_diff_excluded,
			json_bool(data.roundtrip.identity_context_equal), json_bool(data.roundtrip.readback_ok), data.roundtrip.readback_diff, data.roundtrip.readback_diff_excluded,
			data.roundtrip.restored_bytes, data.roundtrip.restore_ns, json_bool(data.roundtrip.locks_free), data.roundtrip.clock.size(), data.roundtrip.clock_replay,
			json_bool(data.roundtrip.clock_error), json_bool(data.roundtrip.repeat_completed), json_bool(data.roundtrip.repeat_equal), json_bool(s2_ok), n_s1, n_s1_excluded,
			s1_s2, s1_s2_excluded, spans, context_diff,
			json_bool(data.roundtrip.branch), data.roundtrip.branch_raw, json_bool(data.roundtrip.wrong_repeat_completed), data.roundtrip.wrong_injected[0], data.roundtrip.wrong_injected[1],
			s1_branch, player_branch, branch_spans, branch_context, wb[0], wb[1], wb[2], wb[3], wb[4], w1[0], w1[1], w1[2], w1[3], w1[4], current_battle_counters[0], current_battle_counters[1], current_battle_counters[2], current_battle_counters[3], current_battle_counters[4]);
		const auto path = (data.checkpoint.capture_directory / "tick-roundtrip.json").u8string();
		fs::file out(std::string(path.begin(), path.end()), fs::write_new + fs::isfile);
		if (out)
			out.write(text.data(), text.size());
	}
	return false;
}
