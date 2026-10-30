#pragma once

#include "pbd_session_health.h"

struct pbd::rollback_session::record_writer
{
	// Created once at writer startup. Rows are read only after publication, and terminal references only after
	// the callback drain. No checkpoint, transport, Practice or replay control state is writable through this view.
	struct source
	{
		u32 start_index, extent;
		bool session_enabled, quiet, tick, practice_continuous, history_rolling;
		const u32& menu_hash_mask;
		std::filesystem::path directory;
		session_clock::time_point origin;
		const pbd::session_history::buffer<update_sample, max_updates>& samples;
		const pbd::session_history::buffer<tick_sample, max_updates>& ticks;
		const pbd::session_history::buffer<u8, max_updates>& matches;
		const pbd::practice::rolling::window<2048>& window;
		const pbd::session_runtime::practice_prefix& rewind_prefix;
		const std::atomic<u32>& published;
		std::atomic<u32>& written_through;
		pbd::session_runtime::trace_buffer* trace;
		std::atomic<u32>& reason;
		const std::atomic<bool>& in_callback;
		const std::atomic<u64>& last_callback_ns;
		const std::atomic<u64>& overlay_poll_ns;
		const pbd::peer_recovery::state& network;
		std::mutex& diagnostic_mutex;
		const pbd::session_runtime::boundary_diagnostic& diagnostic;
		const bool& update_active;
		const u32& end_index;
		const bool& taken;
		fs::file& file;
		fs::file& tick_file;
		pbd::peer_input_trace::queue<>* input_trace;
		std::unique_ptr<std::FILE, int (*)(std::FILE*)>& input_trace_file;
		std::atomic<u32>& rewind_request;
		std::atomic<u32>& rewind_ack;
		const u32& rewind_to;
		const u32& rewind_from;
		const u32& rewind_pass;
		const u64& rewind_ns;
		const u32& rewind_pages;
		const std::array<char, 16>& rewind_action;

		usz slot(u32 index) const noexcept
		{
			return practice_continuous ? window.slot(index) : index;
		}
		const update_sample& sample_at(u32 index) const noexcept
		{
			return samples[slot(index)];
		}
		const tick_sample& tick_at(u32 index) const noexcept
		{
			return ticks[slot(index)];
		}
		u8 match_at(u32 index) const noexcept
		{
			return matches[slot(index)];
		}
		bool history_contains(u32 index) const noexcept
		{
			return practice_continuous ? window.owns(index) : index < extent && (!history_rolling || pbd::session_history::buffer<u8, max_updates>::retained(index, published.load(std::memory_order_acquire)));
		}
		u64 now_ns() const noexcept
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - origin).count());
		}
	} data;
	std::function<void()> fail_output;
	std::function<void(u32)> publish_end;
	u32 written = 0;
	u64 completed = 0, injected_total = 0;
	fs::file health;
	fs::file health_snapshot;
	u64 health_sequence = 0, health_next_ns = 0;
	u64 health_network_sequence = ~u64{0};
	bool health_battle = false, freeze_pause_seen = false;
	pbd::session_health::activity health_activity;
	u64 freeze_quiet_from = 0;
	bool output_bad = false, freeze_dumped = false;
	pbd::session_runtime::boundary_diagnostic last_diagnostic{};
	u64 diagnostic_next_ns = 0;

	void refresh_diagnostic()
	{
		std::unique_lock lock(data.diagnostic_mutex, std::try_to_lock);
		if (lock)
			last_diagnostic = data.diagnostic;
	}

	void run()
	{
		written = data.start_index;
		// Liveness observations use published host facts and never inspect live guest memory.
		if (data.session_enabled)
		{
			const auto name = (data.directory / "session-health.jsonl").u8string();
			health.open(std::string(name.begin(), name.end()), fs::rewrite);
			const auto snapshot_name = (data.directory / "session-health.snapshot").u8string();
			health_snapshot.open(std::string(snapshot_name.begin(), snapshot_name.end()), fs::rewrite);
			// Write the whole page before play, allocating its disk extent while capacity is available.
			observe_health();
		}

		for (;;)
		{
			pbd::stall_metrics::drain(false);
			drain_input_trace(false);
			const u32 available = data.published.load(std::memory_order_acquire);
			if (data.trace)
			{
				data.trace->consume([&](const pbd::session_runtime::trace_snapshot& captured)
					{
						try
						{
							return write_row(captured.index, nullptr, &captured);
						}
						catch (...)
						{
							return false;
						}
					});
				if (data.trace->disabled() && !output_bad)
				{
					output_bad = true;
					pbd_session_log.error("Raw update/tick trace disabled after a write failure; the set continues.");
				}
				written = available;
				data.written_through.store(written, std::memory_order_release);
			}
			else
				for (; !output_bad && written < available; written++)
				{
					if (!write_row(written, nullptr))
					{
						fail_output();
						output_bad = true;
						pbd_session_log.error("Update trace short write; injection disabled, output incomplete.");
						break;
					}
					data.written_through.store(written + 1, std::memory_order_release);
				}
			const u32 rewind_request = data.rewind_request.load(std::memory_order_acquire);
			if (!output_bad && rewind_request != data.rewind_ack.load(std::memory_order_relaxed))
			{
				if (!rewind_rows(rewind_request))
					output_bad = true;
				continue;
			}
			const u32 reason = data.reason.load();
			if (reason != collecting && !data.in_callback.load())
			{
				if (!output_bad && written != data.published.load(std::memory_order_acquire))
					continue;
				const bool end_written = !output_bad && write_end(reason);
				const u32 final_reason = data.trace ? reason : output_bad || !end_written ? output_failed :
				                                                                            reason;
				if (final_reason == output_failed)
					data.reason.store(output_failed);
				drain_input_trace(true);
				pbd::stall_metrics::drain(true);
				publish_end(final_reason);
				return;
			}
			observe_health();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	void drain_input_trace(bool ending) noexcept
	{
		if (!data.input_trace || !data.input_trace_file)
			return;
		try
		{
			for (u32 count = 0; count < 4096; ++count)
			{
				const auto value = data.input_trace->pop();
				if (!value)
					break;
				const auto& row = *value;
				std::string text;
				if (row.type == pbd::peer_input_trace::kind::assign)
					text = fmt::format("{\"type\":\"assign\",\"boundary\":%u,\"value\":%u,\"physical\":%u,\"suppress\":%u,\"edge_deferred\":%u,\"edge_carried\":%u,\"for_tick\":%d,\"delay\":%u,\"lockstep\":%s,\"predicted\":%s,\"how\":\"%s\"}\n",
						row.boundary, row.value, row.physical, row.suppress, row.deferred, row.carried, row.tick == ~u32{0} ? -1 : static_cast<s32>(row.tick),
						row.delay, json_bool(row.lockstep), json_bool(row.predicted), row.how);
				else
					text = fmt::format("{\"type\":\"%s\",\"boundary\":%u,\"tick\":%u,\"value\":%u}\n",
						row.type == pbd::peer_input_trace::kind::receive ? "recv" : "fill", row.boundary, row.tick, row.value);
				if (std::fwrite(text.data(), 1, text.size(), data.input_trace_file.get()) != text.size())
				{
					data.input_trace_file.reset();
					pbd_session_log.error("Peer input trace write failed; diagnostic output incomplete.");
					return;
				}
			}
			if (ending && data.input_trace_file)
			{
				const auto text = fmt::format("{\"type\":\"trace_end\",\"dropped\":%u}\n", data.input_trace->dropped());
				if (std::fwrite(text.data(), 1, text.size(), data.input_trace_file.get()) != text.size() ||
					std::fflush(data.input_trace_file.get()) != 0)
					pbd_session_log.error("Peer input trace could not be completed.");
				data.input_trace_file.reset();
			}
		}
		catch (...)
		{
			data.input_trace_file.reset();
			pbd_session_log.error("Peer input trace disabled after diagnostic publication failed.");
		}
	}

	bool write_row(u32 index, const char* partial_reason, const pbd::session_runtime::trace_snapshot* captured = nullptr)
	{
		if (!captured && !data.history_contains(index))
			return false;
		const auto& row = captured ? captured->row : data.sample_at(index);
		health_battle = data.session_enabled && (captured ? captured->in_match : data.match_at(index) != 0);
		const u32 injected = u32{row.ports[0].injected} + u32{row.ports[1].injected};
		const u32 posted = u32{row.ports[0].posted} + u32{row.ports[1].posted};
		if (data.quiet)
		{
			completed += row.completed;
			injected_total += injected;
			return true;
		}
		std::string line = fmt::format(
			"{\"type\":\"update\",\"index\":%u,\"entry_seen\":%s,\"stage\":%u,\"last_pc\":%u,"
			"\"host_start_ns\":%u,\"host_end_ns\":%u,\"hook_duration_ns\":%u,\"before_counter\":%s,\"after_counter\":%s,\"entry_r1\":%s,"
			"\"raw\":[%u,%u],\"injected_ports\":%u,\"posted_ports\":%u,\"completed\":%s,\"failure\":%s,\"soft\":%s,\"ports\":[",
			index, json_bool(row.entry_seen), row.stage, row.last_pc, row.host_start_ns, row.host_end_ns, row.hook_duration_ns,
			number_or_null(row.before_read, row.before_counter), number_or_null(row.after_read, row.after_counter), number_or_null(row.entry_seen, row.entry_r1),
			row.raw[0], row.raw[1], injected, posted, json_bool(row.completed), nullable_quoted(row.failure ? row.failure : row.completed ? nullptr :
																																			partial_reason),
			nullable_quoted(row.soft));
		for (u32 port = 0; port < 2; port++)
		{
			const auto& sample = row.ports[port];
			if (port)
				line += ',';
			line += fmt::format(
				"{\"port\":%u,\"injected\":%s,\"posted\":%s,\"original_r3\":%s,\"injected_r3\":%s,\"raw\":%s,"
				"\"effective\":%s,\"held\":%s,\"rising\":%s,\"stored_held\":%s,\"stored_rising\":%s}",
				port, json_bool(sample.injected), json_bool(sample.posted), number_or_null(sample.input_read_seen, sample.original_r3),
				number_or_null(sample.injected, sample.injected_r3), number_or_null(sample.post_seen, sample.raw),
				number_or_null(sample.post_seen, sample.effective), number_or_null(sample.post_seen, sample.held), number_or_null(sample.post_seen, sample.rising),
				number_or_null(sample.stored_held_read, sample.stored_held), number_or_null(sample.stored_rising_read, sample.stored_rising));
		}
		line += "]}\n";
		if (data.file.write(line.data(), line.size()) != line.size())
			return false;
		if (!write_tick_row(index, row, partial_reason, captured ? &captured->tick : nullptr))
			return false;
		completed += row.completed;
		injected_total += injected;
		return true;
	}

	bool write_tick_row(u32 index, const update_sample& row, const char* partial_reason, const tick_sample* captured = nullptr)
	{
		if (data.tick && row.entry_seen)
		{
			const auto& tick = captured ? *captured : data.tick_at(index);
			const u32 mask = data.session_enabled && !(captured ? health_battle : data.match_at(index) != 0) ? data.menu_hash_mask : 7u;
			const auto tick_line = fmt::format(
				"{\"type\":\"tick\",\"index\":%u,\"consumer\":%u,\"session_segment\":%u,\"sync_hash_mask\":%u,\"gate_hold\":%u,"
				"\"witness_read\":%s,\"witness\":[%u,%u,%u,%u,%u],\"checked\":%s,\"one_tick\":%s,"
				"\"held\":%s,\"hold_ns\":%u,\"ring_captured\":%s,\"ring_capture_ns\":%u,\"ring_pages_ns\":%u,\"ring_context_ns\":%u,\"ring_calls\":%u,\"faults\":%u,\"fault_ns\":%u,\"combat_read\":%s,\"combat\":[\"%016x\",\"%016x\",\"%016x\"],\"audio_bookkeeping\":\"%016x\",\"hash_relaxed\":%s,"
				"\"ring_bytes\":%u,\"rollback_depth\":%u,\"rtt_us\":%u,\"generator_read\":%s,\"generator_base\":%u,\"generator_seed\":\"%08x\",\"generator_digest\":\"%016x\",\"countdown_read\":%s,\"countdown\":%u,\"battle_time\":%u,\"battle_reads\":%u,\"battle_clock_version\":%u,\"battle_frame_samples\":%u,"
				"\"monitor_read\":%s,\"monitor_state\":%u,\"monitor_start\":%u,\"monitor_end\":%u,\"monitor_index\":%u,\"frame_start\":%u,\"frame_start_adapted\":%s,"
				"\"effect_read\":%s,\"effect\":[\"%016x\",\"%016x\"],\"effect_pages\":[\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\","
				"\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\","
				"\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\",\"%016x\"],"
				"\"round_reset\":%s,\"scene\":[%u,%u],\"frame_counter\":%u,\"fight\":%s,\"fight_count\":%u,\"lockstep\":%s,\"update_completed\":%s,\"failure\":%s}\n",
				index, row.before_counter, tick.session_segment, mask, tick.gate_hold,
				json_bool(tick.read), tick.battle_counters[0], tick.battle_counters[1], tick.battle_counters[2], tick.battle_counters[3], tick.battle_counters[4],
				json_bool(tick.checked), tick.checked ? json_bool(tick.one_tick) : "null", json_bool(tick.held), tick.hold_ns, json_bool(tick.ring_captured), tick.ring_capture_ns, tick.ring_pages_ns, tick.ring_context_ns, tick.ring_calls, tick.faults, tick.fault_ns,
				json_bool(tick.combat_read), tick.combat[0], tick.combat[1], tick.combat[2], tick.audio_bookkeeping, json_bool(tick.hash_relaxed), tick.ring_bytes, tick.rollback_depth, tick.rtt_us, json_bool(tick.generator_read), tick.generator_base, tick.generator_seed, tick.generator_digest,
				json_bool(tick.countdown_read), tick.countdown, tick.battle_time, tick.battle_reads, tick.battle_clock_version, tick.battle_frame_samples,
				json_bool(tick.monitor_read), tick.monitor_state, tick.monitor_start, tick.monitor_end, tick.monitor_index, tick.frame_start, json_bool(tick.frame_start_adapted),
				json_bool(tick.effect_read), tick.effect[0], tick.effect[1],
				tick.effect_pages[0], tick.effect_pages[1], tick.effect_pages[2], tick.effect_pages[3],
				tick.effect_pages[4], tick.effect_pages[5], tick.effect_pages[6], tick.effect_pages[7],
				tick.effect_pages[8], tick.effect_pages[9], tick.effect_pages[10], tick.effect_pages[11],
				tick.effect_pages[12], tick.effect_pages[13], tick.effect_pages[14], tick.effect_pages[15],
				tick.effect_pages[16], tick.effect_pages[17], tick.effect_pages[18], tick.effect_pages[19],
				tick.effect_pages[20], tick.effect_pages[21], tick.effect_pages[22], tick.effect_pages[23],
				tick.effect_pages[24], tick.effect_pages[25], tick.effect_pages[26], tick.effect_pages[27],
				tick.effect_pages[28], tick.effect_pages[29], tick.effect_pages[30], tick.effect_pages[31],
				json_bool(tick.round_reset), tick.scene[0], tick.scene[1], tick.frame_counter, json_bool(tick.fight), tick.fight_count, json_bool(tick.lockstep), json_bool(row.completed), nullable_quoted(row.failure ? row.failure : row.completed ? nullptr :
																																																														partial_reason));
			if (data.tick_file.write(tick_line.data(), tick_line.size()) != tick_line.size())
				return false;
		}
		return true;
	}

	bool rewind_rows(u32 rewind_request)
	{
		const u32 to = data.rewind_to, from = data.rewind_from;
		if (data.practice_continuous)
		{
			completed = data.rewind_prefix.completed;
			injected_total = data.rewind_prefix.injected;
		}
		for (u32 t = to; !data.practice_continuous && t < from && t < data.extent; t++)
		{
			const auto& old = data.sample_at(t);
			completed -= old.completed;
			injected_total -= u32{old.ports[0].injected} + u32{old.ports[1].injected};
		}
		const auto marker = fmt::format("{\"type\":\"takeover_restore\",\"pass\":%u,\"to\":%u,\"from\":%u,\"action\":\"%s\",\"restore_ns\":%u,\"pages\":%u}\n",
			data.rewind_pass, to, from, data.rewind_action.data(), data.rewind_ns, data.rewind_pages);
		if (!data.quiet && (data.file.write(marker.data(), marker.size()) != marker.size() || (data.tick && data.tick_file.write(marker.data(), marker.size()) != marker.size())))
		{
			fail_output();
			return false;
		}
		written = to;
		data.written_through.store(to, std::memory_order_release);
		data.rewind_ack.store(rewind_request, std::memory_order_release);
		return true;
	}

	bool write_end(u32 reason)
	{
		if (reason == output_failed)
			return false;
		// Stop can arrive between port hooks. After the gate drains, the
		// inactive producer cannot touch its unfinished row; emit it explicitly.
		if (!data.trace && data.update_active && (data.practice_continuous || written < data.extent))
		{
			if (!write_row(written, reason_text(reason)))
			{
				pbd_session_log.error("Partial-port update trace write failed; output incomplete.");
				return false;
			}
			written++;
		}
		const u32 requested = data.end_index && !data.taken ? data.end_index : static_cast<u32>(data.extent);
		const bool complete = reason == update_limit && completed == requested - data.start_index && (!data.trace || !data.trace->dropped());
		const auto end = fmt::format(
			"{\"type\":\"end\",\"observed_updates\":%u,\"completed_updates\":%u,\"injected_ports\":%u,\"status\":\"%s\",\"reason\":\"%s\"}\n",
			written - data.start_index, completed, injected_total, complete ? "complete" : "incomplete", reason_text(reason));
		bool success = true;
		if (!data.quiet && data.file.write(end.data(), end.size()) != end.size())
		{
			pbd_session_log.error("Update trace end write failed; output incomplete.");
			success = false;
		}
		if (data.tick && !data.quiet)
		{
			const auto tick_end = fmt::format("{\"type\":\"end\",\"rows\":%u,\"status\":\"%s\",\"reason\":\"%s\",\"flip_absorbed\":%u,\"flip_redelivered\":%u,\"flip_dropped\":%u,\"flip_resets\":%u}\n",
				written - data.start_index, complete ? "complete" : "incomplete", reason_text(reason),
				g_pbd_flip_double_done.load(), g_pbd_flip_redelivered.load(), g_pbd_flip_dropped.load(), g_pbd_flip_resets.load());
			if (data.tick_file.write(tick_end.data(), tick_end.size()) != tick_end.size())
			{
				pbd_session_log.error("Tick trace end write failed; output incomplete.");
				success = false;
			}
		}
		if (!complete)
			pbd_session_log.error("Update latch ended incomplete (%s); %u updates completed, %u ports injected.", reason_text(reason), completed, injected_total);
		return success;
	}

	void observe_health()
	{
		if (const u64 now = data.now_ns(); now >= diagnostic_next_ns)
		{
			diagnostic_next_ns = now + 1'000'000'000ull;
			refresh_diagnostic();
		}
		// Freeze probe (diagnostic, once per run): the owner reached its first boundary, is not inside the
		// callback (so not holding for the other peer) and no boundary has been entered for 4 s. Record the last
		// owner-published boundary and renderer facts; absence of a boundary does not establish a guest suspension.
		// in_callback is read BEFORE the stamp: the callback exit stores the stamp and then clears in_callback, so
		// idle == true guarantees the stamp read next is that exit's (or newer), never a long hold's entry stamp. A paused
		// emulator is not a freeze: the quiet time restarts when it resumes.
		const bool idle = !data.in_callback.load();
		const u64 menu_poll = data.overlay_poll_ns.load();
		if (Emu.IsPaused() || (!idle && pbd::game_overlay::hold_live(data.now_ns(), menu_poll)))
			freeze_pause_seen = true;
		else if (freeze_pause_seen)
		{
			freeze_pause_seen = false;
			freeze_quiet_from = data.now_ns();
		}
		const auto network_sequence = data.network.traffic().sequence;
		if (const u64 now = data.now_ns(); (health || health_snapshot) && (now >= health_next_ns || network_sequence != health_network_sequence))
		{
			const auto network = data.network.get();
			health_network_sequence = network.sequence;
			health_next_ns = now + 1'000'000'000ull;
			const u64 last = std::max(data.last_callback_ns.load(), freeze_quiet_from);
			const auto at_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
			const auto details = network.enabled ? ",\"network\":" + pbd::peer_recovery::status_json(network, at_us) +
			                                           pbd::session_health::progress_json(network.progress) :
			                                       std::string{};
			const auto line = fmt::format("{\"type\":\"health\",\"schema\":1,\"sequence\":%u,\"state\":\"%s\",\"idle_ms\":%u%s}\n",
				++health_sequence, health_activity.observe(data.published.load(std::memory_order_acquire), now, health_battle, freeze_pause_seen, network.recovering),
				last && now > last ? (now - last) / 1000000 : 0, details);
			if (health && health.write(line.data(), line.size()) != line.size())
				health.close();
			if (health_snapshot)
			{
				const auto page = pbd::session_health::snapshot(line);
				if (!page || health_snapshot.seek(0) != 0 || health_snapshot.write(page->data(), page->size()) != page->size())
					health_snapshot.close(); // No stale-file/PID fallback: the launcher still requires a fresh valid sequence.
			}
		}
		if (const u64 last = data.last_callback_ns.load(); idle && last && !freeze_dumped && !freeze_pause_seen)
		{
			const u64 now_ns = data.now_ns();
			if (now_ns > std::max(last, freeze_quiet_from) + 4'000'000'000ull)
			{
				dump_freeze(now_ns, last);
			}
		}
	}

	void dump_freeze(u64 now_ns, u64 last)
	{
		freeze_dumped = true;
		std::string text = fmt::format("PBD freeze probe: no tick boundary for %u ms\n", (now_ns - last) / 1000000);
		// A publisher stalled while holding its lock cannot prevent the last successful snapshot from being reported.
		refresh_diagnostic();
		const auto& snapshot = last_diagnostic;
		text += fmt::format("Last published boundary: generation %u sequence %u next_index %u repeat_active %u correct_at %u depth %u\n",
			snapshot.generation, snapshot.sequence, snapshot.next_index, u32{snapshot.repeat_active}, snapshot.correction_at, snapshot.correction_depth);
		if (const auto render = rsx::get_current_renderer())
			text += fmt::format("RSX %s\npacing: marker_write %u markers_missed %u frames_ended %u credits %u granted %u posted %u skip_draws %u exact %u\n",
				render->pbd_debug_state(),
				pbd::replay_pacing::marker_write.load(), pbd::replay_pacing::markers_missed.load(), static_cast<u32>(pbd::replay_pacing::frames_ended.load()),
				static_cast<u32>(pbd::replay_pacing::credits.load()), static_cast<u32>(pbd::replay_pacing::granted.load()), static_cast<u32>(pbd::replay_pacing::posted.load()),
				static_cast<u32>(pbd::replay_pacing::skip_draws.load()), static_cast<u32>(pbd::replay_pacing::exact.load()));
		const auto path = (data.directory / "freeze-probe.txt").u8string();
		if (fs::file out(std::string(path.begin(), path.end()), fs::rewrite); out)
			out.write(text.data(), text.size());
		pbd_session_log.error("Freeze probe: no tick boundary for 4 s; published diagnostic facts written to freeze-probe.txt.");
		// A missing boundary establishes no cause. Keep this diagnostic observational; peer/session liveness owns failure policy.
	}
};
