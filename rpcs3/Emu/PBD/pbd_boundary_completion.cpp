#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

void pbd::rollback_session::boundary_call::complete_corrected_row(update_sample& row)
{
	write_corrected_row(row);
	if (row.completed && data.roundtrip.repeat_index + 1 < data.correction.at)
	{
		data.roundtrip.repeat_index++;
		data.sample_at(data.roundtrip.repeat_index) = {}; // Provisional row replaced by the next corrected execution.
	}
	else
	{
		// The whole window is now authoritative: publish it. Replay of the last window tick stays
		// active until boundary T closes it.
		data.roundtrip.repeat_active = false;
		g_pbd_replay_timing.store(false, std::memory_order_relaxed);
		pbd::replay_pacing::skip_draws.store(0u, std::memory_order_relaxed);
		data.correction.phase = 2;
		if (row.completed)
			publish_through(data.rolling || data.peer.enabled ? data.correction.at - data.correction.depth + 1 : data.correction.at);
		if (data.rolling || data.peer.enabled)
			data.correction.phase = 0; // Ready for the next learned mismatch.
		complete_takeover_bridge(row);
	}
}

void pbd::rollback_session::boundary_call::write_corrected_row(const update_sample& row)
{
	// Corrected re-execution of one window tick: recorded separately, never published.
	// Clock replay for this tick is closed and counted at the next boundary (correction_clock row).
	const auto port_text = [&](u32 port)
	{
		const auto& p = row.ports[port];
		return fmt::format("{\"port\":%u,\"injected\":%s,\"posted\":%s,\"injected_r3\":%u,\"raw\":%u,\"effective\":%u,\"held\":%u,\"rising\":%u,\"stored_held\":%u,\"stored_rising\":%u}",
			port, json_bool(p.injected), json_bool(p.posted), p.injected_r3, p.raw, p.effective, p.held, p.rising, p.stored_held, p.stored_rising);
	};
	// Syscall wall time inside this replayed tick (owner thread), total and the top six by time.
	u64 syscall_total = 0, syscall_count = 0;
	std::vector<std::pair<u64, u32>> by_time;
	for (u32 code = 0; code < data.roundtrip.replay_syscall_ns.size(); code++)
	{
		if (!data.roundtrip.replay_syscall_count[code])
			continue;
		syscall_total += data.roundtrip.replay_syscall_ns[code];
		syscall_count += data.roundtrip.replay_syscall_count[code];
		by_time.emplace_back(data.roundtrip.replay_syscall_ns[code], code);
	}
	std::sort(by_time.begin(), by_time.end(), std::greater<>());
	std::string syscalls;
	for (usz i = 0; i < by_time.size() && i < 6; i++)
		syscalls += fmt::format("%s[\"%s\",%u,%u]", i ? "," : "", ppu_get_syscall_name(by_time[i].second), data.roundtrip.replay_syscall_count[by_time[i].second], by_time[i].first);
	// With live_stats the elapsed_ns below is this one value (off: the expression is evaluated there).
	const std::chrono::nanoseconds::rep pbd_live_elapsed_ns = data.live_stats ? std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - data.correction.begin).count() : 0;
	if (data.live_stats)
		data.live_pub.replayed_tick(pbd_live_elapsed_ns);
	const auto text = fmt::format(
		"{\"type\":\"corrected_update\",\"index\":%u,\"elapsed_ns\":%u,\"completed\":%s,\"failure\":%s,\"raw\":[%u,%u],\"before_counter\":%u,\"after_counter\":%u,"
		"\"syscall_total_ns\":%u,\"syscall_count\":%u,\"syscalls\":[%s],"
		"\"ports\":[%s,%s]}\n",
		data.roundtrip.repeat_index, data.live_stats ? pbd_live_elapsed_ns : std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - data.correction.begin).count(),
		json_bool(row.completed), nullable_quoted(row.failure), row.raw[0], row.raw[1], row.before_counter, row.after_counter,
		syscall_total, syscall_count, syscalls,
		port_text(0), port_text(1));
	if (data.correction.file)
		data.correction.file.write(text.data(), text.size());
	if (row.completed)
		pbd::replay_pacing::replay_completed.fetch_add(1, std::memory_order_relaxed); // record only
	data.roundtrip.replay_syscall_ns.fill(0);
	data.roundtrip.replay_syscall_count.fill(0);
}

void pbd::rollback_session::boundary_call::complete_takeover_bridge(const update_sample& row)
{
	if (data.takeover.bridge.active)
	{
		const u32 completed = data.roundtrip.repeat_index;
		const u32 side = data.takeover.bridge.side;
		data.next_index = completed + 1;
		data.correction.clock.replaying = false;
		data.correction.active = data.takeover.bridge.saved_correct;
		data.takeover.bridge.active = false;
		if (data.practice.rewind && side == 3)
		{
			data.takeover.phase = pbd::takeover_phase::watching;
			data.takeover.port = 0;
			data.replay.control_port = 0;
			data.replay.taken = false;
			data.takeover.null_input = false;
			data.practice.history.reset();
			data.practice.health_valid = false;
			data.practice.view.fighters = {};
			data.practice.view.fight_live = false;
		}
		else if (side)
		{
			data.takeover.phase = pbd::takeover_phase::controlling;
			data.takeover.port = side;
			data.replay.control_port = side;
			data.replay.taken = true;
			data.takeover.start = completed;
			data.takeover.start_frame = data.takeover.keyframe.frame;
			data.takeover.match = data.takeover.keyframe.match;
			data.takeover.deadline = completed + data.takeover.frames;
		}
		else
		{
			data.takeover.phase = pbd::takeover_phase::watching;
			data.takeover.port = 0;
			data.replay.control_port = 0;
			data.replay.taken = data.takeover.bridge.taken;
			data.takeover.null_input = false;
			data.takeover.keyframe.valid = false;
			// Rewind checkpoints still compare the lifecycle count, so it keeps counting while they exist.
			g_pbd_takeover_anchor_live.store(!data.rewind.points.empty(), std::memory_order_release);
		}
		const auto bridge_text = fmt::format("{\"type\":\"takeover_bridge\",\"to\":%u,\"from\":%u,\"completed\":%s,\"failure\":%s,\"next\":%u}\n",
			completed, data.takeover.bridge.from, json_bool(row.completed), nullable_quoted(row.failure), data.next_index);
		data.replay.control_file.write(bridge_text.data(), bridge_text.size());
		if (!data.practice.rewind && row.completed && !row.failure)
		{
			const bool fighting = data.session.in_match && data.last_fight_index + 1 == data.next_index;
			replay_steps steps{*this, data.next_index, fighting};
			steps.log_event("take", static_cast<s32>(data.replay.control_port));
			steps.publish();
		}
	}
}

void pbd::rollback_session::boundary_call::complete_live_row(u32 index, update_sample& row)
{
	if (data.practice.rewind && row.completed)
	{
		data.practice.history_pending = data.session.in_match && data.practice.native.pause_valid;
		data.practice.history_index = index;
		data.practice.history_words = row.raw;
		data.practice.history_sides = {practice_side_of(0), practice_side_of(1)};
	}
	data.next_index = index + 1;
	if (data.live_stats)
	{
		data.live_pub.boundary(data.next_index);
		data.live_pub.match(!data.session.enabled || (data.history_contains(index) && data.match_at(index)));
	}
	// Rolling rows in [begin, end + delay - 1) may still be re-executed by a later correction.
	// Peer rows inside [begin, end + delay) are published only from boundary confirmation; lockstep rows ran with received input.
	const bool provisional = data.peer.enabled ? index >= data.roll_begin && (!data.roll_end || index < data.roll_end + data.peer.delay) :
	                         data.rolling      ? index >= data.roll_begin && index + 1 < data.roll_end + data.roll_delay :
	                                             data.correction.active && data.correction.phase == 0 && index + data.correction.depth >= data.correction.at && index < data.correction.at;
	if (provisional)
	{
		// Predicted first-pass row: kept out of the frozen trace until corrected; evidence goes to the correction file.
		const auto text = fmt::format(
			"{\"type\":\"predicted_update\",\"index\":%u,\"completed\":%s,\"failure\":%s,\"raw\":[%u,%u],\"after_counter\":%u,"
			"\"held\":[%u,%u],\"rising\":[%u,%u]}\n",
			index, json_bool(row.completed), nullable_quoted(row.failure), row.raw[0], row.raw[1], row.after_counter,
			row.ports[0].held, row.ports[1].held, row.ports[0].rising, row.ports[1].rising);
		data.correction.file.write(text.data(), text.size());
	}
	else
	{
		publish_through(index + 1);
	}
	write_peer_end(index, row);
	if (!data.history.practice_continuous && row.completed && index + 1 == data.extent())
		session_runtime.finish(update_limit);
	// A controlled replay's tape runs past the recording for a taken side; untouched, it ends where the recording does.
	else if (row.completed && data.replay.end_index && !data.replay.taken && index + 1 >= data.replay.end_index)
		session_runtime.finish(update_limit);
	// Graceful player stop (LAN test): the records are written exactly as after a completed tape.
	else if (row.completed && !data.stop_file.empty() && index % 16 == 0 && fs::is_file(data.stop_file))
		session_runtime.finish(user_stopped);
	// An end asked for in game (the Start + Select hold) or by leaving VS mode ends the same way.
	else if (row.completed && data.session.pending_end != collecting)
		session_runtime.finish(data.session.pending_end);
}

void pbd::rollback_session::boundary_call::write_peer_end(u32 index, const update_sample& row)
{
	if (data.peer.enabled && row.completed && index + 1 == data.extent())
	{
		const auto text = fmt::format(
			"{\"type\":\"peer_end\",\"sent\":%u,\"received\":%u,\"duplicates\":%u,\"holds\":%u,\"hold_ns\":%u,\"corrections\":%u,\"confirmed\":%d,\"hash_checked\":%d,\"published\":%u,"
			"\"rtt_us\":{\"last\":%u,\"min\":%u,\"max\":%u,\"mean\":%u,\"count\":%u},\"endpoint_changes\":%u,\"dropped_foreign\":%u,\"input_delay\":%u,\"hash_deferred\":%u,"
			"\"wait_skipped\":%u,\"sync_stalls\":%u,\"sync_catchups\":%u,\"sync_slews\":%u,\"sync_requested_us\":%d,\"slewed_us\":%d,\"rx_overflow\":%u,\"wait_shallow_boundaries\":%u,\"samples_merged\":%u}\n",
			data.peer.link.sent, data.peer.link.received, data.peer.link.duplicates, data.peer.holds, data.peer.hold_ns, data.peer.corrections,
			data.peer.confirmed, data.peer.hash_checked, data.published.load(),
			data.peer.rtt.last_us, data.peer.rtt.count ? data.peer.rtt.min_us : 0, data.peer.rtt.max_us, data.peer.rtt.count ? data.peer.rtt.sum_us / data.peer.rtt.count : 0, data.peer.rtt.count,
			data.peer.link.endpoint_changes, data.peer.link.dropped_foreign, data.peer.input_delay, data.peer.hash_deferred,
			data.peer.wait_skipped, data.peer.sync.stalls, data.peer.sync.catchups,
			data.peer.sync.slews, static_cast<s32>(data.peer.sync.requested_us), static_cast<s32>(pbd::replay_pacing::slewed_us.load()), data.peer.rx.overflow.load(),
			data.peer.sync.wait_shallow_boundaries, data.peer.live.entry_merged);
		data.peer.file.write(text.data(), text.size());
		// The push model's totals (phase delay and fight-start prediction).
		const auto pushed_end = fmt::format("{\"type\":\"phase_delay_end\",\"fight_start\":%u,\"fight_mode\":%u,\"fills\":%u,\"redundant\":%u,\"deferred\":%u,\"forced\":%u,\"bootstrap_withheld\":%u,\"bootstrap_admitted\":%u,\"predrops\":%u,\"predrops_changed\":%u,\"suppressed_values\":%u,\"edges_deferred\":%u,\"beyond_tape\":%u,\"align_holds\":%u}\n",
			data.peer.fight_start, data.peer.fight_mode, data.peer.live.fills, data.peer.live.redundant, data.peer.live.deferred, data.peer.live.forced,
			data.peer.live.bootstrap_withheld, data.peer.live.bootstrap_admitted, data.peer.live.predrops, data.peer.live.predrops_changed, data.peer.live.suppressed_values,
			data.peer.live.edges_deferred, data.peer.live.beyond_script, data.peer.sync.align_holds);
		data.peer.file.write(pushed_end.data(), pushed_end.size());
		// Closing linger: the other peer may still be holding at this last (lockstep) boundary for
		// this tick, and no later boundary carries it again: a single lost datagram would end its run with
		// peer_hold_timeout. Repeat the final datagram for 50 ms.
		for (u32 k = 0; k < 10; k++)
		{
			peer_send(index);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
}
