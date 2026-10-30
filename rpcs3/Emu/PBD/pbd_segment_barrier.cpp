#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_rank_screens.h"

namespace
{
	bool battle_scene(u32 scene)
	{
		return scene == 17u || scene == 16u;
	}
} // namespace

bool pbd::rollback_session::boundary_call::session_segment_barrier()
{
	if (data.session.enabled && pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active)
	{
		std::array<be_t<u32>, 2> scene_at{};
		if (vm::try_access(pbd::t5dr::scene_address, scene_at.data(), sizeof(scene_at), false))
		{
			const std::array<u32, 2> now_scene{u32{scene_at[0]}, u32{scene_at[1]}};

			const bool change = data.session.segments.last_scene_valid && now_scene[0] != data.session.segments.last_scene[0] &&
			                    !(battle_scene(now_scene[0]) && battle_scene(data.session.segments.last_scene[0]));
			data.session.segments.last_scene = now_scene;
			data.session.segments.last_scene_valid = true;
			// The Ranked notices and the VS cards follow the scene at every fresh boundary (the cards: shown on the VS screens,
			// hidden on any other).
			publish_set_notices(now_scene[0]);
			publish_vs_cards(now_scene[0]);
			// A saved replay's players, as its file recorded them, in the game's records (pbd::game_profile), so the replayed game
			// draws their names and recorded ranks under the life bars. Display only, as in the
			// recorded game; repeats write nothing, and after a scrub to an earlier frame the next fresh boundary writes them again.
			// The game's own Ranked screens write what the recording's start names (pbd::rank_screens).
			if (!data.lobby.channel && data.lobby.direct_match.enabled() && !data.lobby.direct_match.pbd_live_inputs())
			{
				const auto& recorded = data.lobby.direct_match.pbd_player_names();
				const auto ranks = data.lobby.direct_match.pbd_player_ranks();
				store_game_profiles({recorded[0], recorded[1]}, with_kept_promotions(ranks));
				if (pbd::rank_screens::configured().screens && pbd::rank_stakes::values_may_change(now_scene[0]))
					pbd::rank_screens::arm(data.lobby.direct_match.pbd_rank_screens().value_or(pbd::rank_stakes::game_values{}), ranks);
			}
			if (change && !data.peer.enabled && (!data.lobby.direct_match.enabled() || data.lobby.direct_match.pbd_live_inputs()))
			{
				if (replay_segment_change(now_scene))
					return true;
			}
			if (change && data.peer.enabled)
			{
				if (peer_segment_change(now_scene))
					return true;
			}
		}
	}
	return false;
}

bool pbd::rollback_session::boundary_call::replay_segment_change(const std::array<u32, 2>& now_scene)
{
	// Single-instance session replay (replay_state::session_replay): the segment starts here, no alignment; the same menu sync as the barrier.
	const u32 k = data.next_index;
	data.session.segments.prev_index = data.session.segments.index;
	data.session.segments.prev_scene = data.session.segments.scene;
	data.session.segments.number++;
	data.session.segments.index = k;
	data.session.segments.scene = now_scene[0];
	const u32 n = data.session.segments.number;
	if (const auto it = data.replay.sync.find(n); it != data.replay.sync.end())
	{
		be_t<u32> word = it->second;
		vm::try_access(0x1008f07c, &word, sizeof(word), true);
	}
	if (!battle_scene(now_scene[0]) && data.session.seed_set)
	{
		const u32 r13 = static_cast<u32>(ppu.gpr[13]);
		be_t<u32> seed_word = data.session.seed ^ (n * 0x9e3779b1u);
		u8 flag = 0;
		if (vm::try_access(r13 - 0x7000, &seed_word, sizeof(seed_word), true) && vm::try_access(r13 + 0x9094, &flag, 1, true))
			data.session.generator_syncs++;
		if (now_scene[0] == 3u && !sync_random_panel(n))
		{
			data.peer.fault = "character_rule_panel_sync";
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
	}
	if (data.session.seed_set)
		take_segment_digest(n);
	pbd::fn_hook::draw_statistics.clear();
	pbd::fn_hook::draw_statistics_active.store(!battle_scene(now_scene[0]) ? 1u : 0u);
	pbd::fn_hook::keyed_segment.store(n);
	pbd::fn_hook::keyed_counts.clear();
	if (data.session.gate.enabled && (now_scene[0] == 12u || now_scene[0] == 8u))
		pbd::fn_hook::gate_ready_tick.store(0);
	pbd_session_log.warning("Session (one PC): segment %u starts at boundary %u in scene %u (counter %s)", n, k, now_scene[0],
		data.replay.sync.count(n) ? "synced from the replay file" : "not synced");
	replay_segment(k);
	return false;
}

bool pbd::rollback_session::boundary_call::peer_segment_change(const std::array<u32, 2>& now_scene)
{
	const u32 k = data.next_index;
	u32 counter_now = 0;
	const u32 n = prepare_peer_segment(k, now_scene, counter_now);
	u32 target = k;
	bool held = false;
	u64 waited = 0;
	if (wait_peer_segment(n, k, target, held, waited))
		return true;
	if (target > k)
	{
		// This peer got here first: skip to the other's index. The skipped indices never run here; the laws compare with the boundary
		// that really ran last, and the game's input counter did not move with the index.
		const u32 jump = target - k;
		if (k > data.start_index)
			data.tick_at(target - 1) = data.tick_at(k - 1);
		// Re-anchor the logical battle clock so the index jump advances guest time by exactly one frame.
		// A longer frame would trigger the game's CPU overload monitor and prune active effects on this peer.
		if (data.battle_clock.enabled && data.battle_clock.anchor_set)
			data.battle_clock.anchor = data.battle_clock.anchor + (static_cast<u64>(k) + 1) * data.battle_clock.freq / 60 - (static_cast<u64>(target) + 1) * data.battle_clock.freq / 60;
		data.consumer_offset -= static_cast<s32>(jump);
		data.next_index = target;
	}
	if (sync_segment_state(n, k, counter_now, now_scene))
		return true;
	// The players' names and ranks in the game's records (write_game_profiles), here where both peers stand aligned on
	// boundary `target` and nothing earlier can be re-executed, and the Ranked stakes both peers hold, from which the next game's
	// command plans the game's own Ranked screens (settle_rank_stakes).
	write_game_profiles(target, now_scene[0]);
	settle_rank_stakes(n);
	// Settle aligned peers before the configured loading scene so host loading latency does not change its executed frames.
	// PBD_SESSION_SETTLE_MS defaults to 1500; PBD_SESSION_SETTLE_SCENE defaults to 12, the last screen before battle.
	if (now_scene[0] == data.session.gate.settle_scene && data.session.gate.settle_ms)
	{
		if (!held)
		{
			held = true;
			vm::temporary_unlock(ppu);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(data.session.gate.settle_ms));
	}
	// Nothing before the segment's first boundary can be re-executed, awaited or hash-checked any more, on either peer.
	publish_through(target);
	data.peer.scan_from = std::max(data.peer.scan_from, target);
	data.peer.confirmed = std::max<s32>(data.peer.confirmed, static_cast<s32>(target) - 1);
	data.peer.hash_checked = std::max(data.peer.hash_checked, data.peer.confirmed);
	const auto text = fmt::format("{\"type\":\"session_barrier\",\"segment\":%u,\"scene\":[%u,%u],\"local_first\":%u,\"remote_first\":%u,\"aligned\":%u,\"jump\":%u,\"wait_ns\":%u}\n",
		n, now_scene[0], now_scene[1], k, data.session.segments.peer_index[n % 8], target, target - k, waited);
	data.peer.file.write(text.data(), text.size());
	replay_segment(target);
	return false;
}

u32 pbd::rollback_session::boundary_call::prepare_peer_segment(u32 k, const std::array<u32, 2>& now_scene, u32& counter_now)
{
	read_word(0x1008f07c, counter_now);
	data.session.segments.prev_index = data.session.segments.index;
	data.session.segments.prev_scene = data.session.segments.scene;
	data.session.segments.prev_counter = data.session.segments.counter;
	data.session.segments.prev_rank_stakes = data.session.segments.rank_stakes;
	data.session.segments.number++;
	data.session.segments.index = k;
	data.session.segments.scene = now_scene[0];
	data.session.segments.counter = counter_now;
	data.session.segments.rank_stakes = pbd::rank_screens::configured().screens && data.lobby.rank_stakes ?
	                                        pbd::rank_stakes::stakes_word(*data.lobby.rank_stakes) :
	                                        0u;
	const u32 n = data.session.segments.number;
	if (!pbd::fn_hook::draw_statistics.empty() || pbd::fn_hook::draw_statistics_active.load())
	{
		// DRAW CENSUS (log only; pbd::fn_hook::census): the draws of the segment that just ended, per (generator, effective
		// caller): count and a digest of the generator state each draw started from. Comparing both peers' rows locates a divergent draw.
		std::string list;
		for (const auto& [key, value] : pbd::fn_hook::draw_statistics)
			fmt::append(list, "%s[%u,%u,%u,%u]", list.empty() ? "" : ",", key.first, key.second, value.first, static_cast<u32>(value.second ^ (value.second >> 32)));
		const auto text = fmt::format("{\"type\":\"draw_census\",\"segment\":%u,\"scene\":%u,\"boundary\":%u,\"draws\":[%s]}\n",
			n - 1, data.session.segments.prev_scene, k, list);
		data.peer.file.write(text.data(), text.size());
		pbd::fn_hook::draw_statistics.clear();
	}
	pbd::fn_hook::draw_statistics_active.store(now_scene[0] != 17u && now_scene[0] != 16u ? 1u : 0u);
	pbd::fn_hook::keyed_segment.store(n);
	pbd::fn_hook::keyed_counts.clear();
	// A repick's VS loader runs after its select, as in any in-game continuation: only a straight load is gated.
	if (data.session.gate.enabled && (now_scene[0] == 12u || now_scene[0] == 8u ||
										 (now_scene[0] == 1u && data.lobby.rematch.straight_to_vs())))
	{
		// A gated screen starts: nothing of it has been tested yet (the fn hook reports its first ready index from now on).
		data.session.gate.seg = n;
		data.session.gate.ready = 0;
		data.session.gate.opened = false;
		pbd::fn_hook::gate_ready_tick.store(0);
	}

	return n;
}

bool pbd::rollback_session::boundary_call::peer_segment_ready(u32 n, u32 k, u32& target)
{
	if (data.session.segments.peer_number[n % 8] == n)
	{
		if (data.session.segments.peer_scene[n % 8] != data.session.segments.scene)
		{
			data.peer.fault = "session_segment_mismatch";
			return true;
		}
		// A segment edge is announced from confirmed execution only: every earlier tick must be known and
		// have run with its received input. In practice both edges of a match lie in long lockstep stretches.
		// Only the frames BEFORE the other peer's exit from the previous segment count: from there on it held at the barrier
		// and this peer ran its input as neutral by design, so comparing those frames with the values the other peer
		// filled in after its jump would fail.
		const u32 check_end = std::min(k, data.session.segments.peer_index[n % 8]);
		bool mispredicted = false;
		for (u32 t = data.peer.scan_from; t < check_end && !mispredicted; t++)
		{
			if (!peer_usable(t, k) || remote_left(t))
				continue;
			if (data.session.enabled && data.neutral_at(t))
				data.sample_at(t).raw[1 - data.peer.role] = data.history.peer_remote_value[t];
			else
				mispredicted = data.sample_at(t).raw[1 - data.peer.role] != data.history.peer_remote_value[t];
		}
		if (mispredicted)
		{
			data.peer.fault = "session_barrier_after_misprediction";
			return true;
		}
		if (peer_first_unusable(k) >= check_end)
		{
			target = std::max(k, data.session.segments.peer_index[n % 8]);
			return true;
		}
	}
	return false;
}

bool pbd::rollback_session::boundary_call::abort_segment_wait(u32 n, u32 k, u32 target, bool emu_stopping, bool left_waiting, bool stop_seen,
	bool lobby_ended)
{
	if (emu_stopping || left_waiting || stop_seen || lobby_ended || data.peer.fault || target >= data.extent())
	{
		if (emu_stopping)
		{
			pbd_session_log.warning("Emulation stopped at session barrier %u.", n);
			session_runtime.finish(emulation_stopped);
		}
		else if (left_waiting)
			leave_session_wait(k);
		else if (stop_seen)
		{
			pbd_session_log.warning("Stop seen at session barrier %u: session ended by the player.", n);
			session_runtime.finish(data.session.pending_end != collecting ? data.session.pending_end : user_stopped);
		}
		else if (lobby_ended)
			pbd_session_log.warning("Lobby set ended at session barrier %u (lobby_set_ended): the other PC is no longer awaited.", n);
		else if (data.peer.fault)
		{
			pbd_session_log.error("Guard '%s' failed at session barrier %u (boundary %u).", data.peer.fault, n, k);
			session_runtime.finish(guard_failed);
		}
		else
			session_runtime.finish(update_limit);
		data.end_callback();
		return true;
	}
	return false;
}

bool pbd::rollback_session::boundary_call::wait_peer_segment(u32 n, u32 k, u32& target, bool& held, u64& waited)
{
	const auto barrier_from = session_clock::now();
	const auto pause_credit = peer_pause_credit();
	bool stop_seen = false, emu_stopping = false, lobby_ended = false;
	u32 polls = 0;
	const u32 announce = k > 0 ? k - 1 : 0;
	// Between screens (and so between games) the other PC may still be loading: "Waiting for <name>'s game to load..."
	// until it arrives, with Leave after 30 s.
	session_wait_modal waiting{session_wait_leave()};
	bool left_waiting = false;
	peer_send(announce);
	auto last_send = session_clock::now();
	for (;;)
	{
		peer_poll(announce);
		if (data.peer.fault)
			break;
		if (peer_segment_ready(n, k, target))
			break;
		if (data.session.segments.peer_newest > n)
		{
			data.peer.fault = "session_segment_lost";
			break;
		}
		if (pbd_emulation_stopping(ppu))
		{
			emu_stopping = true;
			break;
		}
		if (!held)
		{
			held = true;
			vm::temporary_unlock(ppu);
		}
		if (waiting.poll())
		{
			left_waiting = true;
			break;
		}
		if (session_clock::now() - last_send > std::chrono::milliseconds(10))
		{
			peer_send(announce);
			last_send = session_clock::now();
		}
		if (peer_wait_expired(barrier_from, 120, pause_credit))
		{
			if (!data.peer.fault)
				data.peer.fault = "session_barrier_timeout";
			break;
		}
		if (!data.stop_file.empty() && ++polls % 1000 == 0 && fs::is_file(data.stop_file))
		{
			stop_seen = true;
			break;
		}
		// An end asked for in game (the Start + Select hold) just before this wait ends it the same way.
		if (data.session.pending_end != collecting)
		{
			stop_seen = true;
			break;
		}
		// The lobby may end the set meanwhile, as when the other PC refused its game: that PC never reaches this barrier, and
		// without the ending this wait would last until the stop file or the barrier's timeout.
		if (lobby_wait_end())
		{
			lobby_ended = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	waiting.end();
	peer_send(announce); // the other peer may be the one waiting
	waited = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - barrier_from).count();
	if (abort_segment_wait(n, k, target, emu_stopping, left_waiting, stop_seen, lobby_ended))
		return true;

	return false;
}

bool pbd::rollback_session::boundary_call::sync_segment_state(u32 n, u32 k, u32 counter_now, const std::array<u32, 2>& now_scene)
{
	// Menu sync (see session_state::segments_state::counter): the same counter on both peers (the larger of the two announced values, never
	// lower than either), and into a MENU scene the same generator state (srand equivalent: S, flag 0; the next draw re-fills the table).
	{
		const u32 remote_counter = data.session.segments.peer_counter[n % 8];
		const u32 synced = std::max(data.session.segments.counter, remote_counter);
		be_t<u32> word = synced;
		if (synced != counter_now)
			vm::try_access(0x1008f07c, &word, sizeof(word), true);
		if (!battle_scene(now_scene[0]) && data.session.seed_set)
		{
			const u32 r13 = static_cast<u32>(ppu.gpr[13]);
			be_t<u32> seed_word = data.session.seed ^ (n * 0x9e3779b1u);
			u8 flag = 0;
			if (vm::try_access(r13 - 0x7000, &seed_word, sizeof(seed_word), true) && vm::try_access(r13 + 0x9094, &flag, 1, true))
				data.session.generator_syncs++;
			// Into character select both peers also set the random-panel characters (see session_state::random_panel_syncs).
			if (now_scene[0] == 3u)
				sync_random_panel(n);
		}
		const auto sync_text = fmt::format("{\"type\":\"session_sync\",\"segment\":%u,\"counter_local\":%u,\"counter_remote\":%u,\"counter\":%u,\"generator\":%s}\n",
			n, data.session.segments.counter, remote_counter, synced, json_bool(!battle_scene(now_scene[0]) && data.session.seed_set));
		data.peer.file.write(sync_text.data(), sync_text.size());
		// The segment state digest, taken after the syncs; the parser compares it with the other peer's.
		if (data.session.seed_set)
			take_segment_digest(n);
		if (data.peer.fault)
		{
			pbd_session_log.error("Guard '%s' failed at session barrier %u (boundary %u).", data.peer.fault, n, k);
			session_runtime.finish(guard_failed);
			data.end_callback();
			return true;
		}
	}

	return false;
}
