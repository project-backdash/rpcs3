#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

bool pbd::rollback_session::boundary_call::choose_inputs(u32 index, update_sample& row, session_clock::time_point start)
{
	if (!data.update_active)
	{
		pbd::stall_metrics::boundary(index);
		pbd::stall_metrics::scope measure_entry("update_entry");
		data.update_active = true;
		if (data.session.enabled && !data.roundtrip.repeat_active)
		{
			// Session mode: this tick's phase, from the scene selectors at its first dispatch (before its inputs are chosen). A
			// re-execution keeps the live pass's flag (corrections never reach menu ticks: those are lockstep).
			std::array<be_t<u32>, 2> scene_at{};
			const bool read_ok = vm::try_access(pbd::t5dr::scene_address, scene_at.data(), sizeof(scene_at), false);
			data.match_at(index) = read_ok && (scene_at[0] == 17u || scene_at[0] == 16u) ? 1 : 0;
			data.neutral_at(index) = read_ok && !data.match_at(index) && scene_at[0] != 3u ? 1 : 0;
		}
		row.host_start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(start - data.origin).count();
		row.raw = data.history.practice_continuous ? std::array<u16, 2>{0, 0} : std::array<u16, 2>{data.input_at(index).p1, data.input_at(index).p2};
		if (data.spectator.frame)
			row.raw = data.spectator.frame->pads;
		if (choose_direct_input(index, row))
			return true;
		choose_practice_input(index, row);
		choose_session_replay_input(index, row);
		// Branch pass: the re-executed tick N consumes a deliberately different port-0 input.
		if (data.roundtrip.repeat_active && data.roundtrip.branch && data.roundtrip.pass == 1)
			row.raw[0] = data.roundtrip.branch_raw;
		// Peer: the owned port from the local tape, the other port only from the transport ledger.
		choose_peer_input(index, row);
		// The first pass of the correction window uses the predicted port-0 input.
		if (!data.roundtrip.repeat_active && data.correction.active && data.correction.phase == 0 && index + data.correction.depth >= data.correction.at && index < data.correction.at)
			row.raw[0] = data.correction.predict_raw;
		// Rolling: a tick not yet known at execution time "now" (the live boundary, or T during a correction) runs with
		// the last confirmed input, tape[now - delay]; known ticks run with the authoritative tape input.
		if (data.rolling && index >= data.roll_begin && index < data.roll_end && !phase_lockstep(index))
		{
			const u32 now = data.roundtrip.repeat_active ? data.correction.at : index;
			if (index + data.roll_delay > now && !all_known_at(now))
			{
				pbd::update_inputs confirmed = data.input_at(now - data.roll_delay);
				if (data.replay.session_replay && !data.peer.enabled && now - data.roll_delay < data.replay.values.size())
					confirmed = {static_cast<u16>(data.replay.values[now - data.roll_delay]), static_cast<u16>(data.replay.values[now - data.roll_delay] >> 16)};
				row.raw = {confirmed.p1, confirmed.p2};
			}
		}
	}
	return false;
}

bool pbd::rollback_session::boundary_call::choose_direct_input(u32 index, update_sample& row)
{
	if (data.lobby.direct_match.enabled() && !data.lobby.direct_match.pbd_live_inputs())
	{
		u32 scene = 0, counter = 0, segment = 0;
		if (!read_word(pbd::t5dr::scene_address, scene) || !read_word(counter_address, counter))
		{
			pbd_session_log.error("Direct replay input owner read failed.");
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
		const auto result = data.lobby.direct_match.sample(index, scene, counter, row.raw, segment);
		if (result == pbd_direct_match::sample_result::refused)
		{
			pbd_session_log.error("Direct replay input alignment refused.");
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
		if (result == pbd_direct_match::sample_result::ended)
			data.lobby.direct_match_end_pending = true;
		if (result == pbd_direct_match::sample_result::input)
		{
			data.session.segments.number = segment;
			pbd::fn_hook::keyed_segment.store(segment);
			if (data.replay.controls && data.replay.control_port && !data.takeover.null_input)
			{
				const auto pad = data.replay.test.word_set ? data.replay.test.word : data.replay.pad;
				row.raw[data.replay.control_port - 1] = static_cast<u16>(pad & ~(session_start_bit | session_select_bit));
			}
			if (data.replay.viewer_enabled)
				for (usz side = 0; side < 2; ++side)
					data.replay.viewer_history[side].update(index, row.raw[side]);
		}
	}
	return false;
}

void pbd::rollback_session::boundary_call::choose_practice_input(u32 index, update_sample& row)
{
	if (data.practice.dummy.enabled && !data.roundtrip.repeat_active)
	{
		// The scheduler advances once per boundary, before the two port hooks. P1 is still replaced by its ordinary live
		// device read below; P2 consumes this exact recorded word. Recording, and the ready state before it, neutralise P1
		// so one controller drives only P2.
		row.raw[1] = practice_dummy_word(index);
		if (data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::ready)
			row.raw[0] = 0;
		// The word's frame runs with the route its playback state now asks for: a pause for a hit that began or ended
		// at this boundary, or playback that started or ended in it, reaches this frame rather than the next. A route
		// refused here ends playback, and the word chosen for it then reaches no one.
		data.practice.dummy.driver = practice_dummy_route(index);
		practice_getup_guard();
		if (data.practice.dummy.state != dummy_state::recording && data.practice.dummy.state != dummy_state::playing &&
			data.practice.dummy.state != dummy_state::ready && !data.practice.reactions.wakeup_control.armed &&
			!data.practice.reactions.throw_escape_control.owns())
			row.raw[1] = 0;
	}
}

void pbd::rollback_session::boundary_call::choose_session_replay_input(u32 index, update_sample& row)
{
	if (data.replay.session_replay && !data.peer.enabled)
	{
		// Single-instance session replay (replay_state::session_replay): the value in force at this tick's offset in its segment.
		u32 p1 = 0, p2 = 0;
		if (const auto it = data.replay.inputs.find(data.session.segments.number); it != data.replay.inputs.end() && index >= data.session.segments.index)
		{
			const u32 offset = index - data.session.segments.index;
			const auto& changes = it->second;
			auto at = std::upper_bound(changes.begin(), changes.end(), offset, [](u32 o, const std::array<u32, 3>& c)
				{
					return o < c[0];
				});
			if (at != changes.begin())
			{
				--at;
				p1 = (*at)[1];
				p2 = (*at)[2];
			}
		}
		if (data.replay.control_port)
		{
			// A side this PC's controller took (replay controls): its input from the controller, without START and SELECT (the
			// replay's own buttons); the other side keeps the recording.
			if (!data.takeover.null_input)
			{
				u32 word = (data.replay.test.word_set ? data.replay.test.word : data.replay.pad) & ~u32{session_start_bit | session_select_bit};
				if (data.replay.takeover && (data.replay.pad & session_select_bit))
					word &= ~u32{0x0040 | 0x0020}; // SELECT + CROSS / CIRCLE are the takeover's
				(data.replay.control_port == 1 ? p1 : p2) = word;
			}
		}
		row.raw = {static_cast<u16>(p1), static_cast<u16>(p2)};
		if (data.replay.viewer_enabled)
			for (usz side = 0; side < 2; side++)
				data.replay.viewer_history[side].update(index, row.raw[side]);
		if (data.replay.values.empty())
			data.replay.values.assign(data.extent(), 0);
		if (!data.roundtrip.repeat_active && index < data.replay.values.size())
			data.replay.values[index] = p1 | (p2 << 16);
	}
}

void pbd::rollback_session::boundary_call::choose_peer_input(u32 index, update_sample& row)
{
	if (data.peer.enabled)
	{
		const u32 now = data.roundtrip.repeat_active ? data.correction.at : index;
		row.raw[data.peer.role] = data.peer.live.enabled ? (data.peer.role == 0 ? data.input_at(index).p1 : data.input_at(index).p2) : scripted_local(index, index);
		if (data.peer.live.enabled)
		{
			// Live controller: without the entry sample the owned input of tick t is what the game read from the device during
			// tick t-1 (its original_r3 at the latch site), fixed at the first dispatch of t before the boundary send, injected
			// like a tape value and reused by every re-execution (declared input delay: one frame).
			// The entry sample of this boundary (see peer_state::live_state::entry_sample), taken once, before anything uses it.
			if (!data.roundtrip.repeat_active && data.peer.live.entry_sample && !data.history.entry_sampled[index])
			{
				// The newest word of the owned port, plus a button tapped into a ring slot written since the previous sample that
				// this one would skip (read_input_ring). The sample is frozen below, so what is sent is what both games execute.
				u32 producer = 0;
				pbd::stall_metrics::scope measure_ring("local_input_ring", index, 0, true, 1'000'000, pbd::stall_metrics::sampling::elapsed);
				const auto ring_word = read_input_ring(data.input_rings[data.peer.role & 1], data.peer.role & 1, std::nullopt, &producer);
				measure_ring.finish();
				const bool read_ok = ring_word.has_value();
				u16 local_sample = ring_word.value_or(u16{0});
				{
					pbd::stall_metrics::scope measure_admission("local_input_admission", index, 0, true, 1'000'000, pbd::stall_metrics::sampling::elapsed);
					data.history.entry_sample[index] = filter_local_sample(local_sample, index);
				}
				data.history.entry_sampled[index] = 1;
				// Poll-age instrument: how old the word is. getdata_age = boundary entry - the game's newest cellPadGetData of the owned port
				// (the keyread thread publishes the ring word right after it); poll_age = boundary entry - the host pad handlers'
				// newest completed poll. Microseconds, -1 = never seen.
				const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now().time_since_epoch()).count();
				const s64 got_ns = g_pbd_pad_getdata_ns[data.peer.role & 1].load(std::memory_order_relaxed);
				const s64 poll_ns = g_pbd_pad_poll_ns.load(std::memory_order_relaxed);
				const auto text = fmt::format("{\"type\":\"sample\",\"boundary\":%u,\"value\":%u,\"for_tick\":%u,\"read\":%s,\"producer\":%u,\"getdata_age_us\":%d,\"poll_age_us\":%d}\n",
					index, data.history.entry_sample[index], index + data.peer.input_delay, json_bool(read_ok), static_cast<u32>(producer),
					got_ns ? static_cast<s32>((now_ns - got_ns) / 1000) : -1, poll_ns ? static_cast<s32>((now_ns - poll_ns) / 1000) : -1);
				data.peer.file.write(text.data(), text.size());
			}
			if (!data.roundtrip.repeat_active && !data.history.live_fixed[index])
			{
				// With a scheduled input delay D the owned input of tick t is the device read of tick t-1-D,
				// or with the entry sample the entry sample of boundary t-D.
				const u32 back = (data.peer.live.entry_sample ? 0 : 1) + data.peer.input_delay;
				if (data.peer.live.entry_sample)
					data.history.live_inputs[index] = entry_value(index, back);
				else
					data.history.live_inputs[index] = index >= data.start_index + back ? filter_local_sample(static_cast<u16>(data.sample_at(index - back).ports[data.peer.role].original_r3 & 0xffff), index - back) : u16{0};
				data.history.live_fixed[index] = 1;
			}
			row.raw[data.peer.role] = data.history.live_inputs[index];
		}
		row.raw[1 - data.peer.role] = peer_remote_for(index, now);
	}
}

bool pbd::rollback_session::boundary_call::update_replay_round(u32 index, bool fighting)
{
	bool round_start = false;
	const u32 recorded_round = data.lobby.direct_match.pbd_recorded_round(index);
	if (data.session.in_match && recorded_round)
		data.replay.round = recorded_round;
	if (!data.session.in_match)
		data.replay.round = 0;
	else if (fighting && !data.replay.was_fighting)
	{
		round_start = true;
		if (!recorded_round)
			data.replay.round++;
		data.replay.round_start = index;
		const u32 frame = match_frame(index);
		data.takeover.rounds.push_back({data.session.matches, data.replay.round, frame});
		if (data.replay.viewer_enabled)
		{
			const auto found = std::find_if(data.replay.viewer_rounds.begin(), data.replay.viewer_rounds.end(), [&](const auto& r)
				{
					return r.match == data.session.matches && r.round == data.replay.round;
				});
			if (found == data.replay.viewer_rounds.end() && data.replay.viewer_rounds.size() < 256)
				data.replay.viewer_rounds.push_back({data.session.matches, data.replay.round, frame > 320 ? frame - 320 : 0, frame, -1});
			else if (found != data.replay.viewer_rounds.end())
				found->fight = frame;
			std::sort(data.replay.viewer_rounds.begin(), data.replay.viewer_rounds.end(), [](const auto& a, const auto& b)
				{
					return a.match == b.match ? a.round < b.round : a.match < b.match;
				});
		}
		const auto text = fmt::format("{\"type\":\"round\",\"match\":%u,\"round\":%u,\"match_frame\":%u,\"boundary\":%u}\n",
			data.session.matches, data.replay.round, frame, index);
		data.replay.control_file.write(text.data(), text.size());
		if (data.replay.takeover_fast && data.takeover.phase == pbd::takeover_phase::watching)
			request_takeover_capture(index);
		if (pbd::replay_viewer::round_forward_finished(data.takeover.next_match, data.takeover.next_round,
				data.session.matches, data.session.in_match, data.replay.round, true))
		{
			data.takeover.next_round = 0;
			end_replay_fast(data, index);
		}
	}

	return round_start;
}
