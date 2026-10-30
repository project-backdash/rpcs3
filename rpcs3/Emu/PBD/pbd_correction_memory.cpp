#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

bool pbd::rollback_session::tick_dispatch::apply_correction_undo()
{
	// Re-check a renderer-idle sample at the owned memory-write point. A submit that raced the
	// earlier sample is represented by the exact-withhold marker published for this restore.
	if (data.correction.render.waited && data.correction.render.wait_ok && !data.correction.render.rsx_parked)
	{
		if (const auto render = rsx::get_current_renderer(); render && render->ctrl &&
															 !pbd::replay_pacing::handshake_parked.load(std::memory_order_acquire) &&
															 (+render->ctrl->get) != ((+render->ctrl->put) & ~3u))
		{
			data.correction.render.safe_point_stale = true;
			data.correction.render.safe_point_stale_total++;
		}
	}
	// Validate the whole window before writing; an earlier missing journal must never leave a partial rewind.
	// Keep only the earliest preimage of each page, in the same final-write order as the full rewind.
	auto& undo = data.correction.undo_restore_plan;
	for (u32 t = index - data.correction.depth; t < index; ++t)
	{
		const auto& e = data.checkpoint.ring[t % data.checkpoint.ring.size()];
		if (!e.valid || e.index != t || e.undo_overflow || e.undo_bytes.size() != e.undo_pages.size() * 4096ull ||
			e.undo_pcs.size() != e.undo_pages.size() || e.undo_lrs.size() != e.undo_pages.size() ||
			!undo.append(t, e.undo_pages, data.correction.undo_restore_unique))
			return false;
	}
	for (u32 t = index; t-- > index - data.correction.depth;)
	{
		auto& e = data.checkpoint.ring[t % data.checkpoint.ring.size()];
		if (!e.valid || e.index != t || e.undo_overflow)
			return false;
		for (u32 f : e.foreign_pages)
			data.correction.foreign_pages += fmt::format("%s[%u,%u]", data.correction.foreign_pages.empty() ? "" : ",", f, t);
		for (usz k = 0; k < e.skip_pages.size(); k++)
			data.correction.skip_pages += fmt::format("%s[%u,%u,%u]", data.correction.skip_pages.empty() ? "" : ",", e.skip_pages[k], e.skip_pcs[k], t);
	}
	const auto copies = undo.copies();
	for (auto copy = copies.rbegin(); copy != copies.rend(); ++copy)
	{
		const auto& e = data.checkpoint.ring[copy->journal % data.checkpoint.ring.size()];
		const auto k = copy->slot;
		if (!data.checkpoint.memory.write_page_except_ranges(copy->address, e.undo_bytes.data() + k * 4096, data.correction.undo_excluded))
			return false;
		data.correction.undo_pages += fmt::format("%s[%u,%u,%u,%u]", data.correction.undo_applied ? "," : "", copy->address, e.undo_pcs[k], e.undo_lrs[k], copy->journal);
		data.correction.undo_applied++;
	}
	return true;
}

void pbd::rollback_session::tick_dispatch::write_correction_summary(const char* reason)
{
	const auto text = fmt::format(
		"{\"type\":\"correction\",\"correct_at\":%u,\"depth\":%u,\"predict_raw\":%u,\"restored\":%s,\"reason\":%s,\"locks_free\":%s,\"locks_wait_ns\":%u,\"locks_busy\":[%s],\"locks_forced\":%s,\"suspend_ns\":%u,\"suspend_skipped\":%s,"
		"\"restored_bytes\":%u,\"readback_ok\":%s,\"readback_differing_bytes\":%u,\"readback_differing_excluded_bytes\":%u,\"readback_spans\":[%s],\"restore_ns\":%u,\"rewind_ns\":%u,\"readback_ns\":%u,\"rearm_ns\":%u,\"readback_checked\":%s,\"exclusion_file_ranges\":%u,"
		"\"undo_pages_applied\":%u,\"undo_pages_available\":%u,\"undo_duplicates_omitted\":%u,\"undo_restore\":\"%s\",\"undo_excluded_bytes\":%u,\"undo_saves_total\":%u,\"undo_hook\":\"interpreter_store_sites_hle_untracked\",\"undo_overflows\":%u,\"undo_pages\":[%s],\"undo_library_skipped\":[%s],\"published_before\":%u,\"capture\":\"%s\",\"fault_captures\":%u,\"fault_foreign\":%u,\"fault_foreign_pages\":%u,\"fault_armed_pages\":%u,\"fault_capture_ns\":%u,\"fault_arm_ns\":%u,\"fault_arm_calls\":%u,\"fault_remaps\":%u,\"undo_foreign_pages\":[%s],"
		"\"cid\":%u,\"marker\":%d,\"rsx_ended\":%u,\"rsx_skipped\":%u,\"rsx_parked\":%u,\"rsx_get\":%u,\"rsx_put\":%u,\"render_waited\":%s,\"render_wait_ok\":%s,\"render_wait_ns\":%u,"
		"\"safe_point_stale\":%s}\n",
		data.correction.at, data.correction.depth, data.correction.predict_raw, json_bool(!reason), nullable_quoted(reason), json_bool(data.correction.locks.free), data.correction.locks.wait_ns, data.correction.locks.busy, json_bool(data.correction.locks.forced), data.correction.suspend_ns, nullable_quoted(data.correction.suspend_skipped),
		data.correction.written, json_bool(data.correction.readback.ok), data.correction.readback.diff, data.correction.readback.excluded,
		data.correction.readback.spans, data.correction.restore_ns, data.correction.rewind_ns, data.correction.readback.ns, data.correction.rearm_ns, json_bool(data.correction.readback.checked), data.checkpoint.exclusion_file_ranges,
		data.correction.undo_applied, data.correction.undo_restore_plan.candidates(), data.correction.undo_restore_plan.candidates() - data.correction.undo_restore_plan.copies().size(),
		data.correction.undo_restore_unique ? "unique" : "all", data.correction.undo_excluded, data.undo.saves, data.undo.overflows, data.correction.undo_pages, data.correction.skip_pages, data.published.load(),
		data.fault.capture ? "fault" : "store", data.fault.captures, data.fault.foreign, data.fault.foreign_pages, data.fault.armed_pages, data.fault.capture_ns, data.fault.arm_ns, data.fault.arm_calls, data.fault.remaps, data.correction.foreign_pages,
		data.peer.enabled ? data.peer.corrections : data.roll_corrections, data.correction.render.marker_published ? static_cast<s32>(data.correction.render.marker_slot) : -1,
		data.correction.render.rsx_ended, data.correction.render.rsx_skipped, data.correction.render.rsx_parked, data.correction.render.rsx_get, data.correction.render.rsx_put,
		json_bool(data.correction.render.waited), json_bool(data.correction.render.wait_ok), data.correction.render.wait_ns,
		json_bool(data.correction.render.safe_point_stale));
	data.correction.file.write(text.data(), text.size());
}

void pbd::rollback_session::tick_dispatch::wait_correction_renderer()
{
	pbd::stall_metrics::scope measure_wait("correction_renderer_wait");
	// Renderer state at the restore (record only) and the optional bounded renderer wait (OFF by default).
	// The wait runs before the voice gate, the rewind and any suspend (other PPU/SPU threads keep running) and never
	// waits for the marker itself: the renderer parks in the frame handshake until the NEXT present, which the owner
	// only issues after this callback returns.
	data.correction.render.waited = false;
	data.correction.render.wait_ok = false;
	data.correction.render.wait_ns = 0;
	data.correction.render.marker_published = false;
	data.correction.render.safe_point_stale = false;
	pbd::replay_pacing::replay_completed.store(0, std::memory_order_relaxed);
	if (const auto render = rsx::get_current_renderer(); render && render->ctrl)
	{
		const auto render_idle = [&]()
		{
			return pbd::replay_pacing::handshake_parked.load(std::memory_order_acquire) != 0 ||
			       (+render->ctrl->get) == ((+render->ctrl->put) & ~3u);
		};
		if (data.correction.render.wait_ms && data.correction.withhold_exact && data.correction.replay_skip_draws)
		{
			data.correction.render.waited = true;
			const auto wait_begin = session_clock::now();
			const auto wait_limit = std::chrono::milliseconds(data.correction.render.wait_ms);
			while (!(data.correction.render.wait_ok = render_idle()))
			{
				if ((ppu.state & (cpu_flag::exit + cpu_flag::req_exit)) || Emu.IsStopped() || session_clock::now() - wait_begin >= wait_limit)
					break;
				std::this_thread::yield();
			}
			data.correction.render.wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - wait_begin).count();
		}
		data.correction.render.rsx_parked = pbd::replay_pacing::handshake_parked.load(std::memory_order_acquire);
		data.correction.render.rsx_get = +render->ctrl->get;
		data.correction.render.rsx_put = +render->ctrl->put;
	}
	data.correction.render.rsx_ended = pbd::replay_pacing::frames_ended.load(std::memory_order_acquire);
	data.correction.render.rsx_skipped = pbd::replay_pacing::frames_skipped.load(std::memory_order_relaxed);
	data.correction.render.restore_steady_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void pbd::rollback_session::tick_dispatch::wait_correction_locks()
{
	pbd::stall_metrics::scope measure_wait("correction_guest_locks_wait");
	// The guest locks (probe lwmutexes, probed lv2 mutex) must be free before memory is restored. Other guest
	// threads hold them only briefly, so the sample is repeated for up to 30 ms (a holder can be descheduled
	// while the host is busy). The lwmutex control blocks are not restored, so their words are the truth, and a
	// lock can be held (or have a sleeper) for longer than any wait. In PEER mode the correction then proceeds
	// (locks_forced) with the busy locks recorded: the restore writes no lock word, the data those locks protect
	// belongs to other threads (excluded spans), and the confirmed-state hashes remain the check. A busy set
	// already forced once is not waited for again. Rolling (single emulator, a test mode) still fails closed.
	data.correction.locks.free = roundtrip_locks_free();
	data.correction.locks.wait_ns = 0;
	data.correction.locks.busy.clear();
	data.correction.locks.forced = false;
	if (!data.correction.locks.free && (data.rolling || data.peer.enabled))
	{
		const auto wait_begin = session_clock::now();
		data.correction.locks.busy = roundtrip_locks_busy_text();
		const auto limit = data.peer.enabled && data.correction.locks.busy == data.correction.locks.forced_signature ? std::chrono::milliseconds(0) : std::chrono::milliseconds(30);
		while (!data.correction.locks.free && session_clock::now() - wait_begin < limit)
		{
			std::this_thread::yield();
			data.correction.locks.free = roundtrip_locks_free();
		}
		data.correction.locks.wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - wait_begin).count();
		if (!data.correction.locks.free)
		{
			data.correction.locks.busy = roundtrip_locks_busy_text();
			pbd_session_log.warning("Guest locks still busy before the restore at boundary %u after %u us: [%s]%s.", index,
				data.correction.locks.wait_ns / 1000, data.correction.locks.busy, data.peer.enabled ? "; peer correction proceeds" : "");
			if (data.peer.enabled)
			{
				data.correction.locks.forced = true;
				data.correction.locks.forced_signature = data.correction.locks.busy;
			}
		}
	}
}

bool pbd::rollback_session::tick_dispatch::restore_correction_memory(ring_entry& entry)
{
	pbd::stall_metrics::scope measure_restore("checkpoint_restore", entry.pages.size());
	data.correction.readback.diff = data.correction.readback.excluded = 0; // Counters are per correction.
	const u32 corrections_so_far = data.peer.enabled ? data.peer.corrections : data.roll_corrections;
	data.correction.readback.checked = data.correction.readback.policy == 0 || (data.correction.readback.policy == 1 && (corrections_so_far <= 4 || corrections_so_far % 32 == 0)) ||
	                                   (data.correction.readback.policy == 3 && corrections_so_far <= 4);
	checkpoint_restore_result restored{};
	const bool memory_ok = restore_checkpoint_memory(entry, index, [&]()
		{
			return apply_correction_undo();
		},
		{data.correction.readback.checked, false, data.correction.withhold_exact && data.correction.replay_skip_draws}, restored);
	data.correction.written = restored.written;
	data.correction.rewind_ns = restored.rewind_ns;
	data.correction.readback.ns = restored.readback_ns;
	data.correction.readback.ok = restored.readback_ok;
	data.correction.readback.diff = restored.readback_diff;
	data.correction.readback.excluded = restored.readback_excluded;
	data.correction.readback.spans = std::move(restored.readback_spans);
	data.correction.suspend_ns = restored.suspend_ns;
	data.correction.suspend_skipped = restored.suspend_skipped;
	if (!memory_ok)
	{
		data.correction.phase = 2;
		const char* reason = restored.voice_refused         ? "voice_restore_refused" :
		                     restored.voice_history_refused ? "voice_foreign_claim_rewound" :
		                     restored.undo_ok               ? "restore_write_failed" :
		                                                      "undo_restore_failed";
		write_correction_summary(reason);
		fail(restored.voice_refused         ? "voice_restore_refused" :
			 restored.voice_history_refused ? "voice_foreign_claim_rewound" :
			 restored.undo_ok               ? "correction_restore_write_failed" :
											  "correction_undo_failed");
		return true;
	}

	return false;
}
