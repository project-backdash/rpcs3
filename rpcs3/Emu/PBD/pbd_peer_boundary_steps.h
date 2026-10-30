#pragma once

struct pbd::rollback_session::peer_boundary_steps : tick_dispatch
{
	bool& roll_trigger;
	bool lockstep = false, wait_counted = false, held = false, ahead_reported = false;
	bool stop_seen = false, emu_stopping = false, left_waiting = false, lobby_ended = false;
	u32 hold_polls = 0, ahead0 = 0;
	u64 pause_credit = 0;
	double one_way_us = -1.0, sync_error = 0.0;
	session_clock::time_point hold_from{}, stall_until{}, align_until{}, last_send{};
	peer_boundary_steps(tick_dispatch& call, bool& trigger) : tick_dispatch(call), roll_trigger(trigger) {}
	bool run()
	{
		if (data.peer.enabled && !data.roundtrip.repeat_active && !data.correction.clock.replaying)
		{
			if (auto_input_delay())
				return true;
			// Send the owned inputs, receive, hold (lockstep outside the window; backpressure inside it so a
			// correction never needs more than ring - 1 ticks), then correct from the earliest newly usable mismatch.
			lockstep = index < data.roll_begin || (data.roll_end && index >= data.roll_end + data.peer.delay) || phase_lockstep(index);
			assign_phase_delay(lockstep);
			peer_send(index);
			if (lockstep && index < data.history.lockstep_ticks.size())
				data.history.lockstep_ticks[index] = 1;
			hold_from = session_clock::now();
			pause_credit = peer_pause_credit();
			// Balance may stall one tick; actions are at least 30 boundaries apart (peer_state::sync_state::balance).
			stall_until = hold_from;
			sync_timing(hold_from, lockstep, one_way_us, stall_until, sync_error);
			align_opening();
			wait_inputs();
			if (finish_peer_hold())
				return true;
			if (resolve_peer_inputs())
				return true;
		}
		return false;
	}

	void align_opening()
	{
		// Opening alignment uses the filtered symmetric error once per boundary, at least 16 boundaries apart.
		// Hold for two vblank periods because the game's banked semaphore can absorb one period.
		align_until = hold_from;
		if (lockstep && data.peer.sync.enabled && index < data.roll_begin && index >= data.start_index + 8 && data.peer.sync.remote_adv_valid && data.peer.rtt.ring_count >= 8 &&
			sync_error > 0.6 && (!data.peer.sync.last_align || index >= data.peer.sync.last_align + 16))
		{
			align_until = hold_from + std::chrono::microseconds(33334);
			data.peer.sync.last_align = index;
			data.peer.sync.align_holds++;
			const auto text = fmt::format("{\"type\":\"align_hold\",\"boundary\":%u,\"error_milliticks\":%d}\n", index, static_cast<s32>(sync_error * 1000));
			data.peer.file.write(text.data(), text.size());
		}
	}

	void wait_inputs()
	{
		pbd::stall_metrics::scope measure_wait("peer_input_wait", index, lockstep, true, 200'000);
		last_send = hold_from;
		// The session's first ticks (the long first hold below) can wait for the other PC to finish loading; later holds
		// are pacing and draw nothing. Leave is offered there after 30 s, as at every other sync point.
		session_wait_modal waiting{session_wait_leave()};
		for (;;)
		{
			peer_poll(index);
			if (data.peer.fault)
				break;
			const u32 u = peer_first_unusable(index);
			observe_prediction(u);
			const bool want_current = wait_for_fresh_input();
			const bool stalling = session_clock::now() < stall_until;
			const bool aligning = session_clock::now() < align_until;
			// The segment barrier drained earlier player inputs. Loading phases inject zero on both ports, so
			// their physical button words cannot change execution and need not stall the guest before arriving.
			const bool neutral = data.session.enabled && data.neutral_at(index);
			const bool blocked = lockstep ? ((!neutral && !(u >= index && (data.history.peer_remote_arrival[index] >= 0 || remote_left(index)))) || aligning) : (index - u >= data.peer.max_prediction || want_current || stalling);
			if (!blocked)
				break;
			if (hold_and_poll(waiting))
				break;
		}
		waiting.end();
	}

	void observe_prediction(u32 u)
	{
		if (!ahead_reported)
		{
			// How many ticks this boundary would predict, AFTER the poll (datagrams the receive thread queued count).
			ahead_reported = true;
			ahead0 = index >= u ? index - u : 0;
			if (data.live_stats)
				data.live_pub.predicting(ahead0);
			if (!lockstep && index != data.peer.wait_corrected_at)
				data.peer.sync.wait_shallow_boundaries++;
		}
	}

	bool wait_for_fresh_input()
	{
		bool want_current = data.peer.wait_ns && data.history.peer_remote_arrival[index] < 0 &&
		                    session_clock::now() - hold_from < std::chrono::nanoseconds(data.peer.wait_ns);
		// Newer inputs shorten prediction even when this tick's input cannot arrive yet.
		// Arrival-phase filtering waits only when the expected datagram and its 3 ms allowance fit the remaining budget.
		if (want_current && data.peer.wait_due && data.peer.sync.enabled && !lockstep && one_way_us >= 0 && data.peer.sync.remote_boundary_us)
		{
			const auto now_tp = session_clock::now();
			const u64 t_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(now_tp.time_since_epoch()).count());
			// Repeat the arrival phase across lost datagrams. A fresh arrival resets the phase to zero;
			// only a stamp at least one period old can represent a late datagram.
			const u64 since = t_us - data.peer.sync.remote_boundary_us;
			const u64 phase = since % frame_period_us;
			const u64 until = (since >= frame_period_us && phase <= 3000) ? 0 : frame_period_us - phase;
			const u64 spent = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(now_tp - hold_from).count());
			const u64 budget = data.peer.wait_ns / 1000;
			want_current = spent < budget && until + 3000 <= budget - spent;
		}
		// A peer running behind needs the frame time for catch-up instead of waiting for fresher input.
		if (want_current && data.peer.sync.enabled && !lockstep && sync_error <= -0.3)
			want_current = false;
		if (want_current && !lockstep)
		{
			// Reserve frame time for a pending correction and skip a second wait after it re-executes this boundary.
			bool pending = index == data.peer.wait_corrected_at;
			for (u32 t = data.peer.scan_from; t < index && !pending; t++)
				pending = peer_usable(t, index) && data.sample_at(t).raw[1 - data.peer.role] != data.history.peer_remote_value[t];
			if (pending)
			{
				want_current = false;
				if (!wait_counted)
				{
					data.peer.wait_skipped++;
					wait_counted = true;
				}
			}
		}

		return want_current;
	}

	bool hold_and_poll(session_wait_modal& waiting)
	{
		if (lobby_wait_end())
		{
			lobby_ended = true;
			return true;
		}
		if (!held)
		{
			held = true;
			data.peer.holds++;
			data.correction.peer_held_index = index;
			vm::temporary_unlock(ppu);
		}
		if ((index <= data.roll_begin || data.peer.link.recovery.traffic().recovering) && waiting.poll())
		{
			left_waiting = true;
			return true;
		}
		// Authenticated sets use the absolute 90 s blocked-wait lease. Legacy LAN startup/later holds use 300 s/60 s.
		if (peer_wait_expired(hold_from, index <= data.roll_begin ? 300 : 60, pause_credit))
		{
			if (!data.peer.fault)
				data.peer.fault = "peer_hold_timeout";
			return true;
		}
		// Graceful player stop while holding (the other peer stopped first and sends nothing more): checked about
		// once a second; ends the latch exactly as the row-completed stop check does.
		if (!data.stop_file.empty() && ++hold_polls % 1000 == 0 && fs::is_file(data.stop_file))
		{
			stop_seen = true;
			return true;
		}
		if (data.session.pending_end != collecting)
		{
			stop_seen = true;
			return true;
		}
		if (pbd_emulation_stopping(ppu))
		{
			emu_stopping = true;
			return true;
		}
		if (session_clock::now() - last_send > std::chrono::milliseconds(10))
		{
			peer_send(index);
			last_send = session_clock::now();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

		return false;
	}

	bool finish_peer_hold()
	{
		if (held)
		{
			const u64 held_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - hold_from).count();
			data.peer.hold_ns += held_ns;
			const auto text = fmt::format("{\"type\":\"hold\",\"boundary\":%u,\"lockstep\":%s,\"ns\":%u,\"ahead0\":%u,\"deep\":false}\n", index, json_bool(lockstep), held_ns, ahead0);
			data.peer.file.write(text.data(), text.size());
		}
		if (lobby_ended)
			return true;
		if (emu_stopping)
		{
			pbd_session_log.warning("Emulation stopped while holding at boundary %u.", index);
			session_runtime.finish(emulation_stopped);
			return true;
		}
		if (left_waiting)
		{
			leave_session_wait(index);
			return true;
		}
		if (stop_seen)
		{
			pbd_session_log.warning("Stop seen while holding at boundary %u: session ended by the player.", index);
			session_runtime.finish(data.session.pending_end != collecting ? data.session.pending_end : user_stopped);
			return true;
		}
		if (data.peer.fault)
		{
			fail(data.peer.fault);
			return true;
		}

		return false;
	}

	bool resolve_peer_inputs()
	{
		const u32 u = peer_first_unusable(index);
		u32 m = index;
		for (u32 t = data.peer.scan_from; t < index; t++)
		{
			if (!peer_usable(t, index))
				continue;
			if (data.session.enabled && data.neutral_at(t))
			{
				// Only unpublished rows remain in this scan. Retain the arrived transport word for recording;
				// the game consumed neutral regardless of that word, so there is no misprediction to replay.
				data.sample_at(t).raw[1 - data.peer.role] = data.history.peer_remote_value[t];
			}
			else if (data.sample_at(t).raw[1 - data.peer.role] != data.history.peer_remote_value[t])
			{
				m = t;
				break;
			}
		}
		if (m < index)
		{
			if (index - m >= data.checkpoint.ring.size() || index - m > data.correction.clock.replay_counts.size())
			{
				fail("peer_correction_beyond_ring");
				return true;
			}
			roll_trigger = true;
			data.correction.at = index;
			data.correction.depth = index - m;
			data.correction.all_known = phase_lockstep(index);
			data.correction.phase = 0;
			data.correction.clock.mismatches = 0;
			data.peer.corrections++;
			data.peer.wait_corrected_at = index;
			publish_through(m);
		}
		else
		{
			// Every usable tick executed with its received remote value: confirm and publish them.
			data.peer.scan_from = u;
			if (u > 0)
				data.peer.confirmed = static_cast<s32>(u - 1);
			publish_through(u);
			if (check_confirmed_hashes())
				return true;
			row.raw[1 - data.peer.role] = peer_remote_for(index, index);
		}

		return false;
	}

	bool check_confirmed_hashes()
	{
		// Hashes received earlier for ticks confirmed by now, checked in tick order.
		for (s32 t = data.peer.hash_checked + 1; t <= data.peer.confirmed; t++)
		{
			const u32 slot = static_cast<u32>(t) % data.peer.pending_hash_tick.size();
			if (data.peer.pending_hash_tick[slot] != static_cast<u32>(t) + 1u)
				continue;
			bool equal = true;
			const u32 mask = hash_mask(t);
			for (u32 k = 0; k < 3; k++)
				equal = equal && (!((mask >> k) & 1u) || data.peer.pending_hash[slot][k] == data.tick_at(t).combat[k]);
			data.peer.pending_hash_tick[slot] = 0;
			data.peer.hash_checked = t;
			data.peer.hash_deferred++;
			if (data.live_stats)
				data.live_pub.hash_check(t, equal);
			if (!equal || data.peer.hash_deferred <= 3 || t % 60 == 0)
			{
				const auto text = fmt::format("{\"type\":\"hash\",\"boundary\":%u,\"tick\":%d,\"equal\":%s,\"deferred\":true}\n", index, t, json_bool(equal));
				data.peer.file.write(text.data(), text.size());
			}
			if (!equal)
			{
				data.peer.fault = "peer_confirmed_state_mismatch";
				write_mismatch_bundle(t);
				fail("peer_confirmed_state_mismatch");
				return true;
			}
		}

		return false;
	}
};
