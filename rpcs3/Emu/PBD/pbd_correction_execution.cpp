#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

void pbd::rollback_session::tick_dispatch::rearm_correction_entry(ring_entry& entry)
{
	data.previous_held = entry.previous_held;
	data.consumer_offset = entry.consumer_offset;
	data.last_fight_index = entry.last_fight_index;
	// Re-execution of tick m records a fresh undo list against the restored state.
	reset_undo_journal(entry);
	data.undo.tick = entry.index;
	{
		const auto rearm_begin = session_clock::now();
		if (data.fault.capture)
			session_runtime.fault_rearm();
		data.correction.rearm_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - rearm_begin).count();
	}
}

void pbd::rollback_session::tick_dispatch::start_corrected_execution(const ring_entry& entry)
{
	data.tick_at(index).rollback_depth = std::max(data.tick_at(index).rollback_depth, data.correction.depth);
	row = {};
	data.update_active = false;
	// The provisional (predicted) row of the window start is replaced by its corrected execution.
	auto& first = data.sample_at(entry.index);
	first = {};
	first.entry_seen = true;
	first.entry_r1 = ppu.gpr[1];
	first.before_read = true;
	first.before_counter = entry.counter;
	data.roundtrip.repeat_index = entry.index;
	data.roundtrip.replay_syscall_ns.fill(0);
	data.roundtrip.replay_syscall_count.fill(0);
	if (data.correction.withhold_exact && data.correction.replay_skip_draws)
	{
		// Third form (see pbd_replay_pacing.h): everything submitted so far lies below the current PUT position.
		if (const auto render = rsx::get_current_renderer(); render && render->ctrl)
		{
			const u32 slot = pbd::replay_pacing::marker_write.load(std::memory_order_acquire);
			pbd::replay_pacing::markers[slot % pbd::replay_pacing::markers.size()].store((u64{data.correction.depth} << 32) | static_cast<u32>(render->ctrl->put.load()), std::memory_order_release);
			pbd::replay_pacing::marker_write.store(slot + 1, std::memory_order_release);
			data.correction.withhold_flagged += data.correction.depth;
			data.correction.render.marker_slot = slot;
			data.correction.render.marker_published = true;
		}
	}
	data.roundtrip.repeat_active = true;
	g_pbd_replay_timing.store(true, std::memory_order_relaxed);
	g_pbd_replay_frame.store(!data.peer.enabled || (data.correction.withhold_exact && data.correction.replay_skip_draws), std::memory_order_relaxed);
	pbd::replay_pacing::skip_draws.store(data.correction.replay_skip_draws && !data.correction.withhold_exact ? 1u : 0u, std::memory_order_relaxed);
	data.stage = 1;
	data.correction.clock.cursor = 0;
	data.correction.clock.replay_tick = entry.index;
	data.correction.clock.replaying = true;
}

bool pbd::rollback_session::tick_dispatch::resume_correction_clock(const ring_entry& entry)
{
	// The guest finishes the CURRENT main-loop iteration after this callback returns, so its load monitor
	// would difference now(correction.at) against the start just restored from correction.at - depth
	// (several logical steps). Put the frame-start timestamp back to the value the
	// original execution held at this boundary.
	// With the battle clock the window-start tick runs under its own logical time instead
	// (docs/design/netcode-restore-clock.md): battle_clock.tick goes back to m and the frame start is the one the original
	// execution held at boundary m (the restored page already carries it; written for the excluded case).
	if (data.battle_clock.enabled && data.battle_clock.anchor_set)
	{
		data.battle_clock.tick = entry.index;
		data.battle_clock.frame_started = false;
		const u64 logical = data.battle_clock.anchor + (static_cast<u64>(entry.index) + 1) * data.battle_clock.freq / 60;
		if (!write_dword(0x1074f9f8, logical))
		{
			fail("correction_frame_start_write_failed");
			return true;
		}
		data.correction.frame_start_adapted = true;
	}
	pbd::ordinary_metrics::rewind_frames(entry.counter);
	pbd::frame_presentation::flip_base.fetch_sub(data.correction.depth, std::memory_order_release);
	pbd::frame_presentation::mark_input(entry.counter);
	pbd::frame_presentation::mark_logical(entry.counter, data.session.segments.number, false, entry.index);
	data.correction.phase = 1;
	if (data.replay.vblank)
	{
		// The re-executed window start resumes inside the frame that already passed its vblank wait;
		// this credit serves the first replayed frame after it.
		pbd::replay_pacing::grant();
		if (pbd::replay_pacing::lead.load(std::memory_order_relaxed) && data.correction.depth > 1)
		{
			// Lead: the second replayed frame's credit now, the last re-entered boundary grants none.
			pbd::replay_pacing::grant();
			pbd::replay_pacing::lead_extra.fetch_add(1, std::memory_order_relaxed);
		}
	}

	return false;
}
