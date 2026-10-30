#pragma once

struct pbd::rollback_session::takeover_restore_steps : boundary_call
{
	takeover_anchor& a;
	std::span<takeover_anchor* const> journals;
	const char* action;
	u32 side, from;
	ring_entry& entry;
	u64 requested_ns = 0, journal_bytes = 0, excluded = 0, restore_core_ns = 0;
	u32 audit_pages = 0;
	std::vector<std::pair<const takeover_anchor*, usz>> plan;
	checkpoint_restore_result restored{};
	session_clock::time_point restore_begin{};
	takeover_restore_steps(boundary_call& call, takeover_anchor& anchor, std::span<takeover_anchor* const> history,
		const char* operation, u32 port, u32 boundary)
		: boundary_call(call), a(anchor), journals(history), action(operation), side(port), from(boundary), entry(anchor.checkpoint) {}
	bool run()
	{
		requested_ns = pbd::frame_presentation::tag_trace::now_ns();

		if (!pbd::undo_restore::restore_anchor(*this))
			return false;
		const auto text = fmt::format("{\"type\":\"takeover_restore\",\"pass\":%u,\"to\":%u,\"from\":%u,\"action\":\"%s\",\"restore_ns\":%u,\"pages\":%u,\"static_bytes\":%u,\"journal_bytes\":%u,\"audit_pages\":%u,\"bridge\":\"session_rollback\",\"bridge_frames\":1,\"readback_ns\":%u,\"suspend_ns\":%u,\"undo_excluded_bytes\":%u,\"requested_steady_ns\":%u,\"completed_steady_ns\":%u}\n",
			data.takeover.restore_pass, data.takeover.rewind_to, from, action, data.takeover.rewind_ns, data.takeover.rewind_pages,
			entry.pages.size(), journal_bytes, audit_pages,
			restored.readback_ns, restored.suspend_ns, excluded, requested_ns, pbd::frame_presentation::tag_trace::now_ns());
		data.replay.control_file.write(text.data(), text.size());
		data.takeover.redispatch = true;
		return true;
	}

	bool refuse(const char* reason)
	{
		data.takeover.refusal = reason;
		const u64 lifecycle_now = g_pbd_takeover_lifecycle_syscalls.load(std::memory_order_relaxed);
		const u64 lifecycle_code = g_pbd_takeover_last_lifecycle_syscall.load(std::memory_order_relaxed);
		const auto text = fmt::format("{\"type\":\"takeover_refused\",\"action\":\"%s\",\"reason\":\"%s\",\"match\":%u,\"frame\":%u,\"side\":%u,\"boundary\":%u,\"lifecycle_anchor\":%llu,\"lifecycle_now\":%llu,\"lifecycle_code\":%llu}\n",
			action, reason, a.match, a.frame, side, from, a.lifecycle_syscalls, lifecycle_now, lifecycle_code);
		data.replay.control_file.write(text.data(), text.size());
		return false;
	}

	bool wait_until(const auto& predicate)
	{
		const auto begin = session_clock::now();
		while (!predicate())
		{
			if (pbd_emulation_stopping(ppu) || session_clock::now() - begin >= std::chrono::milliseconds(500))
				return false;
			std::this_thread::yield();
		}
		return true;
	}

	bool apply_journal()
	{
		for (const auto& [journal, k] : plan)
			if (!data.checkpoint.memory.write_page_except_ranges(journal->journal_pages[k], journal->journal_bytes.data() + k * 4096, excluded))
				return false;
		return true;
	}

	bool admit_anchor()
	{
		if (data.history.practice_continuous && (!data.history.practice_anchor.valid || !data.history.practice_window.can_branch()))
			return refuse("rolling_anchor_metadata_unavailable");
		if (!data.replay.takeover_fast || !a.valid || !entry.valid || entry.index != a.index)
			return refuse("anchor_missing");
		if (data.lobby.direct_match.enabled() && !data.lobby.direct_match.pbd_can_restore_cursor(a.host.pbd_cursor))
			return refuse("direct_replay_cursor_unavailable");
		std::array<be_t<u32>, 2> scene{};
		if (!vm::try_access(pbd::t5dr::scene_address, scene.data(), sizeof(scene), false))
			return refuse("scene_unreadable");
		if (data.session.matches != a.match || data.session.segments.number != a.segment || u32{scene[0]} != a.scene[0] || u32{scene[1]} != a.scene[1])
			return refuse("session_changed");
		if (data.battle_clock.anchor != a.battle_anchor || initial_state().battle_counter != a.battle_counter)
			return refuse("battle_clock_changed");
		if (data.fault.remaps != a.fault_remaps)
			return refuse("mapping_changed");
		if (g_pbd_takeover_lifecycle_syscalls.load(std::memory_order_relaxed) != a.lifecycle_syscalls)
			return refuse("lifecycle_syscall");
		if (pbd::fn_hook::takeover_counters() != a.fn_counters)
			return refuse("generator_epoch_changed");

		return true;
	}

	bool plan_journal()
	{
		// The pages to write back, earliest journal first; a page a newer journal also holds keeps the earlier copy.
		try
		{
			std::unordered_set<u32> seen;
			for (const auto* journal : journals)
			{
				if (journal->overflow)
					return refuse("journal_full");
				journal_bytes += journal->journal_bytes.size();
				for (usz k = 0; k < journal->journal_pages.size(); k++)
				{
					const u32 page = journal->journal_pages[k];
					if (!seen.insert(page).second)
						continue;
					if (!vm::check_addr(page, vm::page_readable | vm::page_writable, 4096))
						return refuse("journal_page_unmapped");
					plan.emplace_back(journal, k);
				}
			}
		}
		catch (...)
		{
			return refuse("journal_plan_allocation_failed");
		}

		return true;
	}

	bool wait_restore_ready()
	{
		if (!wait_until([&]
				{
					return data.written_through.load(std::memory_order_acquire) == data.published.load(std::memory_order_acquire);
				}))
			return refuse("writer_busy");
		if (!wait_until([&]
				{
					const auto render = rsx::get_current_renderer();
					return !render || !render->ctrl || pbd::replay_pacing::handshake_parked.load(std::memory_order_acquire) ||
			               (+render->ctrl->get) == ((+render->ctrl->put) & ~3u);
				}))
			return refuse("render_busy");
		if (!wait_until([&]
				{
					return roundtrip_locks_free();
				}))
			return refuse("guest_locks_busy");

		return true;
	}

	bool restore_memory()
	{
		restore_begin = session_clock::now();
		if (!restore_checkpoint_memory(entry, from, [&]()
				{
					return apply_journal();
				},
				{true, true, data.correction.withhold_exact && data.correction.replay_skip_draws}, restored))
		{
			if (restored.voice_refused)
				return refuse("voice_restore_refused");
			if (restored.voice_history_refused)
				return refuse("voice_history_changed");
			if (!restored.undo_ok || !restored.write_ok)
				return refuse("restore_write_failed");
			return refuse("static_readback_failed");
		}

		restore_core_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - restore_begin).count();
		return true;
	}

	void stop_failed_restore()
	{
		data.session.pending_end = guard_failed;
		// The observer's end scope parks the gameplay owner after this callback's temporary restore leases unwind.
		data.session.failstop = true;
		data.peer.fault = data.takeover.refusal.c_str();
		data.replay.paused = false;
		data.replay.step = false;
		session_runtime.finish(guard_failed);
	}

	void audit_restore()
	{
		if (data.takeover.audit)
		{
			const bool first = std::find(data.takeover.audit_restored.begin(), data.takeover.audit_restored.end(), a.index) == data.takeover.audit_restored.end();
			const auto stem = first ? fmt::format("memdump-restored-%u", a.index) :
			                          fmt::format("memdump-restored-%u-pass%u", a.index, data.takeover.restore_pass + 1);
			audit_pages = write_memdump(data.checkpoint.capture_directory / stem);
			if (first)
				data.takeover.audit_restored.push_back(a.index);
		}
	}

	bool restore_host_state()
	{
		if (data.lobby.direct_match.enabled())
		{
			if (!data.lobby.direct_match.pbd_restore_cursor(a.host.pbd_cursor) ||
				(side && !data.lobby.direct_match.set_continuation(data.generation, static_cast<pbd_replay_continuation>(side))))
				return refuse("direct_replay_cursor_restore_failed");
		}
		load_takeover_host(a.host);
		// These three values belong to the production checkpoint point inside the tick, after the entry preamble and
		// fight witness. The broader replay-control slot is intentionally earlier so it can undo the takeover action.
		data.consumer_offset = entry.consumer_offset;
		data.last_fight_index = entry.last_fight_index;
		data.previous_held = entry.previous_held;
		data.correction.render.marker_published = false;
		if (data.correction.withhold_exact && data.correction.replay_skip_draws)
		{
			if (const auto render = rsx::get_current_renderer(); render && render->ctrl)
			{
				const u32 slot = pbd::replay_pacing::marker_write.load(std::memory_order_acquire);
				// Only the anchor tick is re-executed before live rendering resumes, irrespective of rewind distance.
				pbd::replay_pacing::markers[slot % pbd::replay_pacing::markers.size()].store((u64{1} << 32) | render->ctrl->put.load(), std::memory_order_release);
				pbd::replay_pacing::marker_write.store(slot + 1, std::memory_order_release);
				data.correction.withhold_flagged++;
				data.correction.render.marker_slot = slot;
				data.correction.render.marker_published = true;
			}
		}
		data.battle_clock.tick = a.index;
		data.battle_clock.frame_started = false;
		if (data.battle_clock.enabled && data.battle_clock.anchor_set)
		{
			const u64 logical = data.battle_clock.anchor + (static_cast<u64>(a.index) + 1) * data.battle_clock.freq / 60;
			if (!write_dword(0x1074f9f8, logical))
				return refuse("frame_start_write_failed");
		}

		return true;
	}

	bool rewind_record_writer()
	{
		const u32 request = data.takeover.rewind_request.load(std::memory_order_relaxed) + 1;
		data.takeover.rewind_to = a.index;
		data.takeover.rewind_from = from;
		data.takeover.rewind_pass = ++data.takeover.restore_pass;
		data.takeover.rewind_pages = static_cast<u32>(plan.size());
		data.takeover.rewind_ns = restore_core_ns;
		data.takeover.rewind_action.fill(0);
		std::strncpy(data.takeover.rewind_action.data(), action, data.takeover.rewind_action.size() - 1);
		if (data.history.practice_continuous)
			data.history.practice_rewind_prefix = data.history.practice_anchor.prefix;
		data.published.store(a.index, std::memory_order_release);
		data.takeover.rewind_request.store(request, std::memory_order_release);
		if (!wait_until([&]
				{
					return data.takeover.rewind_ack.load(std::memory_order_acquire) == request;
				}))
			return refuse("writer_rewind_timeout");

		return true;
	}

	bool discard_future_history()
	{
		// Production correction retains the window-start tick observation: its remaining latch stages replace only
		// the input row, and the following boundary compares against that original tick witness. Discard only rows
		// after the anchor; clearing ticks[a.index] makes the next live boundary see a zero witness/fight count.
		if (data.history.practice_continuous)
		{
			if (!data.history.practice_window.begin_branch() || !data.history.practice_window.claim(a.index, a.index))
				return refuse("rolling_branch_refused");
			data.sample_at(a.index) = {};
			data.tick_at(a.index) = data.history.practice_anchor.tick;
			data.match_at(a.index) = data.history.practice_anchor.match;
			data.neutral_at(a.index) = data.history.practice_anchor.neutral;
			data.start_executed_at(a.index) = data.history.practice_anchor.start;
			data.history.practice_produced = data.history.practice_anchor.prefix;
		}
		for (u32 t = a.index + 1; !data.history.practice_continuous && t < from && t < data.extent(); t++)
		{
			data.sample_at(t) = {};
			data.tick_at(t) = {};
			if (t < data.replay.values.size())
				data.replay.values[t] = 0;
		}

		return true;
	}

	void resume_input_capture()
	{
		data.next_index = a.index + 1;
		data.update_active = false;
		data.battle_clock.frame_started = false;
		// A capture asked for at the boundary being left would describe a moment that no longer exists.
		data.takeover.capture_pending = false;
		data.rewind.capture_pending = false;
		if (!data.fault.armed_all)
			session_runtime.fault_arm_all();
		else
			session_runtime.fault_rearm();
		data.takeover.fault_armed = true;
		data.undo.tick = a.index;
		data.undo.ready = true;
		data.takeover.ignore_pad_until_release = true;
		if (data.replay.viewer_enabled)
			data.replay.viewer.barrier(pbd_local_pad_word());
		if (data.practice.rewind)
		{
			data.practice.native.controlled = a.practice_native_controlled;
			data.practice.native.getup = a.practice_native_getup;
			data.practice.native.action = a.practice_native_action;
			data.practice.native.written = a.practice_native_written;
			data.practice.reactions.wakeup_control.reset();
			data.practice.reactions.wakeup_suspended = false;
			data.practice.reactions.throw_escape_control.reset();
			data.practice.history_pending = false;
			data.practice.record_word_valid = false;
			data.practice.play_before.valid = false;
			data.practice.boundary_counter_valid = false;
		}
		data.replay.paused = false;
		if (data.replay.viewer_enabled)
			pbd::practice::native::frame_restore(a.frame_state);
		data.replay.step = false;
		data.replay.speed = 0;
		g_cfg.video.vblank_rate.set(data.replay.fast.saved_vblank);
		pbd::replay_pacing::vblank_request.store(static_cast<u32>(data.replay.fast.saved_vblank), std::memory_order_release);
		pbd::replay_pacing::pbd_player_rate.store(0, std::memory_order_release);
		pbd::player_volume::restore();
	}

	void start_restore_bridge()
	{
		// Resume through the correction path's representation seam. The current hook returns exactly like a session
		// correction restore: the saved entry stage is already admitted, and the remaining three latch stages execute
		// as the authoritative replacement row. The selected recording/controller source becomes live at T+1.
		auto& first = data.sample_at(a.index);
		first = {};
		first.entry_seen = true;
		first.entry_r1 = ppu.gpr[1];
		first.before_read = true;
		first.before_counter = entry.counter;
		data.roundtrip.repeat_index = a.index;
		data.roundtrip.repeat_active = true;
		data.takeover.bridge.active = true;
		data.takeover.bridge.from = from;
		data.takeover.bridge.side = side;
		data.takeover.bridge.taken = a.host.replay_taken;
		data.takeover.bridge.saved_correct = data.correction.active;
		data.correction.active = true;
		data.correction.at = a.index + 1;
		data.correction.depth = 1;
		data.correction.all_known = true;
		data.correction.phase = 1;
		data.correction.clock.mismatches = 0;
		data.correction.clock.replaying = false;
		data.correction.begin = session_clock::now();
		data.roundtrip.replay_syscall_ns.fill(0);
		data.roundtrip.replay_syscall_count.fill(0);
		data.correction.render.restore_steady_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		data.stage = 1;
		g_pbd_replay_timing.store(true, std::memory_order_relaxed);
		g_pbd_replay_frame.store(true, std::memory_order_relaxed);
		pbd::replay_pacing::skip_draws.store(data.correction.replay_skip_draws && !data.correction.withhold_exact ? 1u : 0u, std::memory_order_relaxed);
		pbd::ordinary_metrics::rewind_frames(entry.counter);
		pbd::frame_presentation::flip_base.fetch_sub(from - a.index, std::memory_order_release);
		pbd::frame_presentation::mark_input(entry.counter);
		pbd::frame_presentation::mark_logical(entry.counter, a.segment, false, a.index);
		if (data.replay.vblank)
			pbd::replay_pacing::grant();
	}
};
