#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

void pbd::rollback_session::tick_dispatch::sync_timing(session_clock::time_point hold_from, bool lockstep,
	double& one_way_us, session_clock::time_point& stall_until, double& sync_error)
{
	if (data.peer.sync.remote_boundary_us && data.peer.rtt.ring_count)
	{
		// Half the SMALLEST of the last 32 round trips: the samples include the time a datagram sat in the socket
		// until the next boundary's poll, the smallest has the least of it.
		const u32 n = std::min<u32>(data.peer.rtt.ring_count, 32u);
		u32 smallest = ~u32{0};
		for (u32 k = 0; k < n; k++)
			smallest = std::min(smallest, data.peer.rtt.ring[(data.peer.rtt.ring_count - 1 - k) % data.peer.rtt.ring.size()]);
		one_way_us = smallest / 2.0;
		const u64 now_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(hold_from.time_since_epoch()).count());
		const double remote_now = data.peer.sync.remote_boundary + (static_cast<double>(now_us - data.peer.sync.remote_boundary_us) + one_way_us) / frame_period_us;
		const double advantage = static_cast<double>(index) - remote_now;
		if (data.peer.sync.adv_sampled_at != index) // one step per boundary: the pass after a correction re-enters with the same index
		{
			data.peer.sync.adv_ewma = data.peer.sync.adv_valid ? data.peer.sync.adv_ewma + (advantage - data.peer.sync.adv_ewma) / (data.peer.sync.enabled ? 8.0 : 32.0) : advantage;
			data.peer.sync.adv_sampled_at = index;
		}
		data.peer.sync.adv_valid = true;
		// Time sync (see peer_state::sync_state::enabled): act on half the difference of the two peers' estimates.
		const bool error_valid = data.peer.sync.enabled && data.peer.sync.remote_adv_valid && data.peer.rtt.ring_count >= 8;
		const double error = error_valid ? (data.peer.sync.adv_ewma - data.peer.sync.remote_adv_milli / 1000.0) / 2.0 : 0.0;
		sync_error = error;
		if (data.live_stats)
			data.live_pub.advantage(data.peer.sync.adv_ewma, error, error_valid);
		if (index % 60 == 0)
		{
			const auto text = fmt::format("{\"type\":\"advantage\",\"boundary\":%u,\"milliticks\":%d,\"smoothed_milliticks\":%d,\"remote_milliticks\":%d,\"error_milliticks\":%d,"
										  "\"slew_pending_us\":%d,\"slewed_us\":%d,\"one_way_us\":%u}\n",
				index, static_cast<s32>(advantage * 1000), static_cast<s32>(data.peer.sync.adv_ewma * 1000), data.peer.sync.remote_adv_milli, static_cast<s32>(error * 1000),
				static_cast<s32>(pbd::replay_pacing::slew_us.load()), static_cast<s32>(pbd::replay_pacing::slewed_us.load()), static_cast<u32>(one_way_us));
			data.peer.file.write(text.data(), text.size());
		}
		adjust_peer_timeline(error_valid, error, lockstep);
		balance_peer_timeline(hold_from, lockstep, stall_until);
	}
}

bool pbd::rollback_session::tick_dispatch::auto_input_delay()
{
	// Automatic input delay (see peer_state::delay_auto). Role 0 decides from the round trips of the
	// opening lockstep phase; both peers apply the announced value at the same boundary, still in lockstep.
	if (data.peer.delay_auto && !data.peer.delay_applied)
	{
		// Session mode: decided early in character select (frames 30 / 40): fast picks can reach the loading screens and their
		// alignment barriers (index jumps) around the default frames 90..110, and a peer that jumped past roll_begin before the
		// announcement arrived would fail closed.
		const u32 decide_at = data.session.enabled ? data.start_index + 30 : data.roll_begin > 20 ? data.roll_begin - 20 :
		                                                                                            0;
		const u32 apply_at = data.session.enabled ? data.start_index + 40 : data.roll_begin > 4 ? data.roll_begin - 4 :
		                                                                                          0;
		if (data.peer.role == 0 && !data.peer.delay_announce && index >= decide_at)
		{
			const u32 n = std::min<u32>(data.peer.rtt.ring_count, static_cast<u32>(data.peer.rtt.ring.size()));
			u32 decided = std::min<u32>(1, data.peer.delay_auto_max), median = 0, smallest = 0;
			if (n >= 8)
			{
				std::vector<u32> sorted(data.peer.rtt.ring.begin(), data.peer.rtt.ring.begin() + n);
				std::sort(sorted.begin(), sorted.end());
				median = sorted[n / 2];
				smallest = sorted[0];
				// From the SMALLEST round trip. Datagrams are read once per boundary, so every sample carries up
				// to a frame of waiting in the socket on each side (a 1 ms LAN path shows a median of about 16.5 ms), and
				// median or p95 overestimate the delay. Ticks of one-way transit to
				// cover = (smallest / 2 + 4 ms of jitter allowance) / tick, minus the one tick of prediction this game can
				// afford inside a frame: 0 on a LAN, 1 up to about 58 ms of round trip, 2 up
				// to about 92 ms, 3 up to about 125 ms, then the cap (PBD_PEER_AUTO_MAX_DELAY, default 3, at most 5).
				const double need = (smallest / 2.0 + 4000.0) / frame_period_us;
				// Never below 1: at delay 0 every frame waits for the same frame's remote input, which stutters even on a LAN;
				// at delay 1 nothing waits. With the entry sample, delay 1 is one real frame.
				decided = static_cast<u32>(std::clamp(static_cast<s32>(std::ceil(need)) - 1, static_cast<s32>(std::min<u32>(1, data.peer.delay_auto_max)), static_cast<s32>(data.peer.delay_auto_max)));
			}
			if (data.peer.delay_forced >= 0)
				decided = static_cast<u32>(data.peer.delay_forced); // a configured delay
			data.peer.delay_announce = decided + 1;
			const auto text = fmt::format("{\"type\":\"input_delay_decided\",\"boundary\":%u,\"rtt_smallest_us\":%u,\"rtt_median_us\":%u,\"samples\":%u,\"input_delay\":%u}\n",
				index, smallest, median, n, decided);
			data.peer.file.write(text.data(), text.size());
		}
		if (index >= apply_at && data.peer.delay_announce)
		{
			data.peer.input_delay = data.peer.delay_announce - 1;
			if (data.live_stats)
				data.live_pub.input_delay(data.peer.input_delay);
			data.peer.delay_applied = true;
			const auto text = fmt::format("{\"type\":\"input_delay_applied\",\"boundary\":%u,\"input_delay\":%u}\n", index, data.peer.input_delay);
			data.peer.file.write(text.data(), text.size());
		}
		else if (index >= (data.session.enabled ? data.start_index + 600 : data.roll_begin))
		{
			fail("peer_delay_announce_missing");
			return true;
		}
	}
	return false;
}

void pbd::rollback_session::tick_dispatch::adjust_peer_timeline(bool error_valid, double error, bool lockstep)
{
	// A correction that takes more than two periods loses a vblank for good (the
	// game's vblank semaphore banks one); the peer then trails by a tick and the other predicts a tick deeper. The
	// slew is too slow for that (0.3 tick per 45 boundaries): a peer 0.8 tick or more behind takes a catch-up frame.
	if (error_valid && index >= data.start_index + 16 && error <= -0.8 && !lockstep && data.replay.vblank && index >= data.peer.sync.last_stall + 16)
	{
		pbd::replay_pacing::grant();
		data.peer.sync.last_stall = index;
		data.peer.sync.last_catchup = index; // the filter has not absorbed this tick yet; no slew for 16 boundaries
		data.peer.sync.catchups++;
		const auto text = fmt::format("{\"type\":\"sync_catchup\",\"boundary\":%u,\"smoothed_milliticks_before\":%d,\"error_milliticks\":%d}\n",
			index, static_cast<s32>(data.peer.sync.adv_ewma * 1000), static_cast<s32>(error * 1000));
		data.peer.file.write(text.data(), text.size());
	}
	else if (error_valid && index >= data.start_index + 16 && index >= data.peer.sync.evaluated + 45 && (!data.peer.sync.last_catchup || index >= data.peer.sync.last_catchup + 16) &&
			 pbd::replay_pacing::slew_us.load(std::memory_order_acquire) == 0)
	{
		if (std::fabs(error) > 0.12)
		{
			// 0.4 of the error from each peer (0.8 together), at most 0.3 tick per step; the vblank thread spreads the
			// shift over the next frames. Ahead (error > 0): the grid moves later.
			const s64 shift_us = static_cast<s64>(0.4 * std::clamp(error, -0.75, 0.75) * frame_period_us);
			pbd::replay_pacing::slew_us.fetch_add(shift_us, std::memory_order_acq_rel);
			data.peer.sync.evaluated = index;
			data.peer.sync.slews++;
			data.peer.sync.requested_us += shift_us;
			const auto text = fmt::format("{\"type\":\"sync_slew\",\"boundary\":%u,\"smoothed_milliticks\":%d,\"remote_milliticks\":%d,\"error_milliticks\":%d,\"shift_us\":%d}\n",
				index, static_cast<s32>(data.peer.sync.adv_ewma * 1000), data.peer.sync.remote_adv_milli, static_cast<s32>(error * 1000), static_cast<s32>(shift_us));
			data.peer.file.write(text.data(), text.size());
		}
		else
			data.peer.sync.evaluated = index >= 30 ? index - 30 : 0; // inside the dead band: look again in 15 boundaries
	}
}

void pbd::rollback_session::tick_dispatch::balance_peer_timeline(session_clock::time_point hold_from, bool lockstep, session_clock::time_point& stall_until)
{
	// Guard against extremes only (stalling a peer that is only 0.75 tick ahead causes frequent hitches, and symmetric
	// catch-up gives the slower peer, which rolls back at a higher cost, many more rollbacks; it is
	// better off trailing a little). The peer more than 2 ticks BEHIND runs one catch-up frame (one virtual vblank
	// credit: its vblank wait ends at once, nothing visible); the peer more than 3 ticks AHEAD, about to take
	// depth 4-5 rollbacks and then hold at the prediction bound, stalls one frame. At most every 30 boundaries.
	if (data.peer.sync.balance && !data.peer.sync.enabled && !lockstep && index >= data.roll_begin + 60 && index >= data.peer.sync.last_stall + 30)
	{
		if (data.peer.sync.adv_ewma < -2.0 && data.replay.vblank)
		{
			pbd::replay_pacing::grant();
			data.peer.sync.last_stall = index;
			data.peer.sync.catchups++;
			const auto text = fmt::format("{\"type\":\"sync_catchup\",\"boundary\":%u,\"smoothed_milliticks_before\":%d}\n", index, static_cast<s32>(data.peer.sync.adv_ewma * 1000));
			data.peer.file.write(text.data(), text.size());
			data.peer.sync.adv_ewma += 1.0;
		}
		else if (data.peer.sync.adv_ewma > 3.0)
		{
			// TWO periods. The game's vblank semaphore banks one post (max count 1), so a stall of
			// one period is absorbed by the vblank that fires during it and moves this peer back 0 ticks; of two
			// vblanks fired during the stall only one is banked: exactly one tick back.
			stall_until = hold_from + std::chrono::microseconds(33334);
			data.peer.sync.last_stall = index;
			data.peer.sync.stalls++;
			const auto text = fmt::format("{\"type\":\"sync_stall\",\"boundary\":%u,\"smoothed_milliticks_before\":%d}\n", index, static_cast<s32>(data.peer.sync.adv_ewma * 1000));
			data.peer.file.write(text.data(), text.size());
			data.peer.sync.adv_ewma -= 1.0;
		}
	}
}
