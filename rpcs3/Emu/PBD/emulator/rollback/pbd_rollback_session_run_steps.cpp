// run()'s later steps: the session segment barrier, the exit gate, the replay controls, the choice of the tick's
// inputs and the completion of its row.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"

bool pbd::rollback_session::boundary_call::exit_gate(u32 index)
{
	if (data.session.enabled && data.session.gate.enabled && data.peer.enabled && pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active)
	{
		bool hold = false;
		if (data.session.gate.seg && data.session.gate.seg == data.session.segments.number)
		{
			const u32 s = data.session.gate.seg, b = index, lag = data.session.gate.lag;
			if (!data.session.gate.ready)
				data.session.gate.ready = pbd::fn_hook::gate_ready_tick.load();
			data.session.gate.through = b; // every test of an index below b has run (the handler of tick b runs after this boundary)

			if (!(data.session.gate.ready && data.session.gate.ready - 1 + lag <= b))
				hold = true;
			else
			{
				// Open at b only if the other peer was ready at b - lag too. Undecided while its first ready index is unknown and its tests are
				// not yet recorded through b - lag (with lockstep menus and lag > input delay this does not wait in practice).

				if (wait_exit_gate(s, b, lag))
					return true;
				const u32 r = data.session.gate.peer_ready_seg[s % 8] == s ? data.session.gate.peer_ready[s % 8] : 0u;
				hold = !(r && r - 1 + lag <= b);
				if (!hold && !data.session.gate.opened)
				{
					data.session.gate.opened = true;
					const auto text = fmt::format("{\"type\":\"session_gate\",\"segment\":%u,\"scene\":%u,\"local_ready\":%u,\"remote_ready\":%u,\"open\":%u,\"lag\":%u}\n",
						s, data.session.segments.scene, data.session.gate.ready - 1, r - 1, b, lag);
					data.peer.file.write(text.data(), text.size());
				}
			}
		}
		pbd::fn_hook::gate_hold.store(hold ? 1u : 0u);
	}
	return false;
}

bool pbd::rollback_session::boundary_call::apply_replay_controls(u32 index)
{
	// Replay controls: pause, frame step, speed and the controller taking a side, before this boundary's inputs are chosen.
	if (data.replay.controls && !data.update_active && !data.roundtrip.repeat_active && pc == pcs[0])
	{
		if (data.replay.takeover_fast)
			save_takeover_host();
		if (data.rewind.enabled)
			data.rewind.dispatch_host = data.takeover.dispatch_slot;
		const bool fighting = data.session.in_match && index && data.last_fight_index + 1 == index;
		const bool round_start = update_replay_round(index, fighting);
		data.replay.was_fighting = fighting;
		rewind_schedule(index, fighting, round_start);
		if (data.replay.fast.active)
		{
			if (data.live_stats)
				data.live_pub.replay(false, 2, 0, 0, 0);
			if (data.replay.viewer_enabled)
			{
				auto view = pbd::replay_viewer::presentation.load();
				view.enabled = 1;
				view.fast = 1;
				view.menu = 0;
				view.takeover = pbd::takeover_phase::watching;
				view.flags = 0;
				view.match = data.session.matches;
				pbd::replay_viewer::presentation.store(view);
				// The in-game Settings menu holds this controller here too, so a hold made in the menu never leaves the replay.
				const u16 pad = pbd::settings_menu::claimed() ? 0 : pbd_local_pad_word();
				const bool leaving = (pad & (session_select_bit | session_start_bit)) == (session_select_bit | session_start_bit);
				const auto now = session_clock::now();
				if (leaving && !data.replay.leave_held)
					data.replay.leave_from = now;
				data.replay.leave_held = leaving;
				const u32 hold_ms = leaving ? static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(now - data.replay.leave_from).count()) : 0;
				if (data.live_stats)
					data.live_pub.leave(leaving, std::min(hold_ms, pbd::live_stats::leave_hold_total_ms));
				if (hold_ms >= pbd::live_stats::leave_hold_total_ms)
					data.session.pending_end = user_stopped;
				// A running seek keeps reading Left / Right, so the newest target wins and holding either keeps going.
				if (data.rewind.seeking && data.session.pending_end == collecting)
				{
					replay_steps steps{*this, index, fighting};
					if (steps.seek_poll())
					{
						data.takeover.redispatch = false;
						data.end_callback();
						return true;
					}
				}
			}
		}
		else
		{
			const auto result = replay_controls_at(index);
			if (result == replay_control_result::restored)
			{
				data.takeover.redispatch = false;
				data.end_callback();
				return true;
			}
			if (result == replay_control_result::stopped)
			{
				if (data.session.pending_end != guard_failed)
					pbd_session_log.warning("Emulation stopped while the replay was held at boundary %u.", index);
				session_runtime.finish(data.session.pending_end == guard_failed ? guard_failed : emulation_stopped);
				data.end_callback();
				return true;
			}
		}
		if (data.session.pending_end == guard_failed)
		{
			session_runtime.finish(guard_failed);
			data.end_callback();
			return true;
		}
	}
	return false;
}

void pbd::rollback_session::boundary_call::complete_row(u32 index, update_sample& row)
{
	if (row.completed || row.failure)
	{
		data.update_active = false;
		if (data.roundtrip.repeat_active && data.correction.active)
		{
			complete_corrected_row(row);
		}
		else if (data.roundtrip.repeat_active)
		{
			// The verification re-execution of tick N: compared with the first execution, never published.
			const auto& first = data.sample_at(data.roundtrip.repeat_index);
			data.roundtrip.repeat_completed = row.completed;
			data.roundtrip.repeat_equal = row.completed && first.after_counter == row.after_counter;
			for (u32 port = 0; port < 2; port++)
			{
				const auto& a = first.ports[port];
				const auto& b = row.ports[port];
				data.roundtrip.repeat_equal = data.roundtrip.repeat_equal && a.injected_r3 == b.injected_r3 && a.raw == b.raw && a.effective == b.effective &&
				                              a.held == b.held && a.rising == b.rising && a.stored_held == b.stored_held && a.stored_rising == b.stored_rising;
			}
			data.roundtrip.repeat_active = false;
			g_pbd_replay_timing.store(false, std::memory_order_relaxed);
			pbd::replay_pacing::skip_draws.store(0u, std::memory_order_relaxed);
		}

		else
		{
			complete_live_row(index, row);
		}
	}
}
