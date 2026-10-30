// The correction at a boundary: the replayed interval's close, the peer boundary with its input delay, phase delay
// and timing sync, and the restore of the window.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_peer_boundary_steps.h"
#include "Emu/PBD/pbd_phase_input_steps.h"

bool pbd::rollback_session::tick_dispatch::correction()
{
	if (data.correction.active)
	{
		close_replayed_interval();
		if (!data.correction.clock.replaying)
		{
			data.checkpoint.ring[index % data.checkpoint.ring.size()].clock.clear();
			data.correction.clock.tick = index;
			data.correction.clock.recording = true;
		}
		bool roll_trigger = false;
		if (data.rolling && !data.roundtrip.repeat_active && !data.correction.clock.replaying && index >= data.roll_begin + data.roll_delay)
		{
			// The authoritative input of tick T - delay is learned at this boundary; at a lockstep-entry boundary every
			// tick below it is known. Scan the unpublished known ticks for the earliest mismatch.
			const u32 first = std::max(index - data.roll_delay, data.published.load());
			const u32 known_end = phase_lockstep(index) ? index : index - data.roll_delay + 1;
			u32 m = known_end;
			for (u32 t = first; t < known_end; t++)
			{
				const auto& executed = data.sample_at(t).raw;
				pbd::update_inputs truth = data.input_at(t);
				if (data.replay.session_replay && !data.peer.enabled && t < data.replay.values.size())
					truth = {static_cast<u16>(data.replay.values[t]), static_cast<u16>(data.replay.values[t] >> 16)};
				if (t < data.roll_end && (executed[0] != truth.p1 || executed[1] != truth.p2))
				{
					m = t;
					break;
				}
			}
			roll_trigger = m < known_end;
			// Ticks before m (all known ticks when they matched) can never be re-executed again.
			publish_through(roll_trigger ? m : known_end);
			if (roll_trigger)
			{
				data.correction.at = index;
				data.correction.depth = index - m;
				data.correction.all_known = phase_lockstep(index);
				data.correction.phase = 0;
				data.correction.clock.mismatches = 0;
				data.roll_corrections++;
			}
		}
		if (peer_boundary(roll_trigger))
			return true;
		if (restore_window(roll_trigger))
			return true;
	}
	return false;
}

void pbd::rollback_session::tick_dispatch::close_replayed_interval()
{
	if (data.correction.clock.replaying)
	{
		// This boundary closes the replayed interval of the previous window tick.
		const auto& journal = data.checkpoint.ring[data.correction.clock.replay_tick % data.checkpoint.ring.size()].clock;
		const u32 slot = data.correction.clock.replay_tick - (data.correction.at - data.correction.depth);
		if (slot < data.correction.clock.replay_counts.size())
		{
			data.correction.clock.replay_counts[slot] = data.correction.clock.cursor;
			data.correction.clock.journal_sizes[slot] = static_cast<u32>(journal.size());
		}
		data.correction.clock.mismatches += data.correction.clock.cursor != journal.size();
		if (data.roundtrip.repeat_active)
		{
			data.correction.clock.replay_tick = index;
			data.correction.clock.cursor = 0;
		}
		else
		{
			// Boundary T after the corrected window: report replay per tick, then record live again.
			data.correction.clock.replaying = false;
			std::string replayed, sizes;
			for (u32 i = 0; i < data.correction.depth; i++)
			{
				replayed += fmt::format("%s%u", i ? "," : "", data.correction.clock.replay_counts[i]);
				sizes += fmt::format("%s%u", i ? "," : "", data.correction.clock.journal_sizes[i]);
			}
			std::string hook_by_pc;
			for (const auto& e : data.correction.window.hook_by_pc)
				if (e[0])
					fmt::append(hook_by_pc, "%s[%u,%u,%u]", hook_by_pc.empty() ? "" : ",", e[0], e[1], e[2]);
			// With live_stats the correction_wall_ns below is this one value (off: the expression is evaluated there).
			const std::chrono::nanoseconds::rep pbd_live_wall_ns = data.live_stats ? std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - data.correction.begin).count() : 0;
			if (data.live_stats)
				data.live_pub.correction_end(pbd_live_wall_ns);
			// The RSX published this window's marker hit (lag in frames at the restore, replay progress, time).
			const bool pbd_marker_hit = data.correction.render.marker_published &&
			                            pbd::replay_pacing::marker_hit_slot[data.correction.render.marker_slot % 8].load(std::memory_order_acquire) == data.correction.render.marker_slot + 1;
			const auto text = fmt::format(
				"{\"type\":\"correction_clock\",\"first_index\":%u,\"journal\":[%s],\"replayed\":[%s],\"mismatches\":%u,\"clock_error\":%s}\n"
				"{\"type\":\"correction_end\",\"boundary\":%u,\"clock_mismatches\":%u,\"clock_error\":%s,\"correction_wall_ns\":%u,"
				"\"replay_vblank\":%s,\"virtual_vblanks_granted\":%u,\"virtual_vblanks_posted\":%u,\"virtual_vblanks_pending\":%u,\"virtual_vblanks_notified\":%u,"
				"\"replay_skip_draws\":%s,\"frames_skipped_total\":%u,\"withhold_exact\":%s,\"withhold_flagged_total\":%u,\"withhold_estimated\":%u,\"window_hook_ns\":%u,\"window_hook_by_pc\":[%s],\"flip_absorbed\":%u,\"flip_redelivered\":%u,\"flip_dropped\":%u,\"flip_resets\":%u,"
				"\"audio_underruns_begin\":%u,\"audio_underruns\":%u,\"audio_advances_begin\":%u,\"audio_advances\":%u,\"audio_silence_begin\":%u,\"audio_silence\":%u,"
				"\"cid\":%u,\"marker\":%d,\"marker_hit\":%s,\"marker_lag_frames\":%u,\"marker_hit_replayed\":%u,\"marker_hit_after_us\":%d,"
				"\"rsx_ended_end\":%u,\"rsx_skipped_end\":%u,\"rsx_parked_end\":%u,\"handshake_waits\":%u,\"handshake_passed\":%u,\"withheld_at_risk_total\":%u,"
				"\"safe_point_stale_total\":%u,\"expired_withheld_total\":%u,"
				"\"peer_dropped_foreign\":%u,\"peer_endpoint_changes\":%u}\n",
				data.correction.at - data.correction.depth, sizes, replayed, data.correction.clock.mismatches, json_bool(data.correction.clock.error),
				index, data.correction.clock.mismatches, json_bool(data.correction.clock.error),
				data.live_stats ? pbd_live_wall_ns : std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - data.correction.begin).count(),
				json_bool(data.replay.vblank), pbd::replay_pacing::granted.load(), pbd::replay_pacing::posted.load(), pbd::replay_pacing::credits.load(), pbd::replay_pacing::notified.load(),
				json_bool(data.correction.replay_skip_draws), pbd::replay_pacing::frames_skipped.load(),
				json_bool(data.correction.withhold_exact), data.correction.withhold_flagged, pbd::replay_pacing::markers_missed.load(),
				data.correction.window.hook_ns + static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - data.correction.window.scope_start).count()), hook_by_pc,
				g_pbd_flip_double_done.load(), g_pbd_flip_redelivered.load(), g_pbd_flip_dropped.load(), g_pbd_flip_resets.load(),
				data.correction.audio_begin[0], g_pbd_audio_underruns.load(std::memory_order_relaxed),
				data.correction.audio_begin[1], g_pbd_audio_time_advances.load(std::memory_order_relaxed),
				data.correction.audio_begin[2], g_pbd_audio_silence_blocks.load(std::memory_order_relaxed),
				data.peer.enabled ? data.peer.corrections : data.roll_corrections, data.correction.render.marker_published ? static_cast<s32>(data.correction.render.marker_slot) : -1,
				json_bool(pbd_marker_hit), pbd_marker_hit ? pbd::replay_pacing::marker_hit_lag[data.correction.render.marker_slot % 8].load(std::memory_order_relaxed) : 0u,
				pbd_marker_hit ? pbd::replay_pacing::marker_hit_replayed[data.correction.render.marker_slot % 8].load(std::memory_order_relaxed) : 0u,
				pbd_marker_hit ? static_cast<s32>((static_cast<s64>(pbd::replay_pacing::marker_hit_ns[data.correction.render.marker_slot % 8].load(std::memory_order_relaxed)) - static_cast<s64>(data.correction.render.restore_steady_ns)) / 1000) : -1,
				pbd::replay_pacing::frames_ended.load(std::memory_order_acquire), pbd::replay_pacing::frames_skipped.load(std::memory_order_relaxed),
				pbd::replay_pacing::handshake_parked.load(std::memory_order_acquire), pbd::replay_pacing::handshake_waits.load(std::memory_order_relaxed), pbd::replay_pacing::handshake_passed.load(std::memory_order_relaxed),
				pbd::replay_pacing::frames_withheld_at_risk.load(std::memory_order_relaxed),
				data.correction.render.safe_point_stale_total, pbd::replay_pacing::markers_expired_withheld.load(std::memory_order_relaxed), data.peer.link.dropped_foreign, data.peer.link.endpoint_changes);
			data.correction.window.open = false;
			data.correction.file.write(text.data(), text.size());
		}
	}
}

bool pbd::rollback_session::tick_dispatch::peer_boundary(bool& roll_trigger)
{
	return peer_boundary_steps{*this, roll_trigger}.run();
}

void pbd::rollback_session::tick_dispatch::assign_phase_delay(bool lockstep)
{
	phase_input_steps{*this, lockstep}.run();
}

bool pbd::rollback_session::tick_dispatch::restore_window(bool roll_trigger)
{
	if (!data.roundtrip.repeat_active && data.correction.phase == 0 && (data.rolling || data.peer.enabled ? roll_trigger : index == data.correction.at))
	{
		// The authoritative inputs for the window are now known; restore the ring entry of the
		// window start in place and re-execute the window with the tape inputs.
		const auto restore_begin = session_clock::now();
		data.correction.undo_restore_plan.clear();
		data.correction.undo_applied = 0;
		data.correction.undo_excluded = 0;
		data.correction.undo_pages.clear();
		data.correction.skip_pages.clear();
		data.correction.foreign_pages.clear();
		auto& entry = data.checkpoint.ring[(index - data.correction.depth) % data.checkpoint.ring.size()];
		data.correction.begin = restore_begin;
		data.correction.audio_begin[0] = g_pbd_audio_underruns.load(std::memory_order_relaxed);
		data.correction.audio_begin[1] = g_pbd_audio_time_advances.load(std::memory_order_relaxed);
		data.correction.audio_begin[2] = g_pbd_audio_silence_blocks.load(std::memory_order_relaxed);
		data.correction.window.scope_start = restore_begin;
		data.correction.window.hook_ns = 0;
		data.correction.window.hook_by_pc = {};
		data.correction.window.open = true;
		wait_correction_renderer();
		wait_correction_locks();
		if (!entry.valid || entry.index != index - data.correction.depth || (!data.correction.locks.free && !data.correction.locks.forced))
		{
			data.correction.phase = 2;
			write_correction_summary(!entry.valid || entry.index != index - data.correction.depth ? "ring_entry_missing" : "locks_busy_before_restore");
			// A rolling correction that cannot be admitted would leave a mispredicted tick in history: fail closed.
			if (data.rolling || data.peer.enabled)
			{
				fail("rolling_correction_not_admitted");
				return true;
			}
		}
		else
		{
			if (restore_correction_memory(entry))
				return true;
			rearm_correction_entry(entry);
			start_corrected_execution(entry);
			if (resume_correction_clock(entry))
				return true;
			data.correction.restore_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - restore_begin).count();
			if (data.live_stats)
				data.live_pub.restore(data.correction.depth, static_cast<s64>(data.correction.restore_ns));
			write_correction_summary(nullptr);
		}
		return true;
	}
	return false;
}
