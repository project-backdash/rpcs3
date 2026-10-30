#pragma once

struct pbd::rollback_session::phase_input_steps : tick_dispatch
{
	const bool lockstep;
	bool predicted = false, bootstrap = false;
	u32 delay_here = 0, limit = 0, target = 0, for_tick = 0;
	u16 physical = 0, sample = 0, newest = 0, edge_deferred = 0, edge_carried = 0;
	const char* how = "assigned";
	phase_input_steps(tick_dispatch& call, bool locked) : tick_dispatch(call), lockstep(locked) {}
	void run()
	{
		// Phase delay (see peer_state::phase_delay): fix the tick this boundary's entry sample feeds, from
		// the authoritative lockstep classification, before the datagram of this boundary goes out.
		if (data.peer.live.enabled && data.peer.phase_delay && data.peer.input_delay >= 1 && data.history.entry_sampled[index] && !data.history.entry_assigned[index])
		{
			data.history.entry_assigned[index] = 1;
			delay_here = data.peer.input_delay;
			adapt_lockstep_delay();
			data.peer.lock_phase = lockstep;
			// The shrink to D is finished BEFORE the predicted first actionable tick X.
			predicted = data.peer.fight_mode && index < data.peer.fight_start + 8 && data.last_fight_index < data.peer.fight_start &&
			            delay_here > data.peer.input_delay && index + delay_here >= data.peer.fight_start;
			if (predicted)
				delay_here = data.peer.input_delay;
			limit = static_cast<u32>(data.extent());
			target = index + delay_here;
			if (data.live_stats)
				data.live_pub.delay_now(delay_here);
			physical = data.history.entry_sample[index];
			data.peer.live.suppress &= physical; // a masked button is free again once it has been released
			sample = physical & ~data.peer.live.suppress;
			if (sample != physical)
				data.peer.live.suppressed_values++;

			bootstrap = data.peer.live.bootstrap_sample && !data.peer.live.assigned_any;
			newest = data.peer.live.assigned_any ? data.history.live_inputs[data.peer.live.assigned_through] : u16{0};

			for_tick = target;
			fill_unassigned_inputs();
			assign_sample();
			if (for_tick != ~u32{0} && (!data.peer.live.assigned_any || for_tick > data.peer.live.assigned_through))
			{
				data.peer.live.assigned_through = for_tick;
				data.peer.live.assigned_any = true;
			}
			if (data.peer.input_trace)
				data.peer.input_trace->push({pbd::peer_input_trace::kind::assign, index, for_tick, sample, physical,
					data.peer.live.suppress, edge_deferred, edge_carried, delay_here, lockstep, predicted, how});
		}
	}

	// Lockstep delay covers transit time so each tick need not wait for its input. Round intros and transitions
	// cap it at 6 ticks to keep the first actionable inputs close to the player's presses;
	// character select permits up to 30 ticks because it has no actionable fight inputs.
	static constexpr u32 max_lockstep_delay = 6;
	static constexpr u32 max_select_delay = 30;

	static bool at_character_select() noexcept
	{
		u32 scene = 0, subscene = 0;
		return pbd::session_runtime::read_word(pbd::t5dr::scene_address, scene) &&
		       pbd::session_runtime::read_word(pbd::t5dr::subscene_address, subscene) &&
		       scene == 3u && subscene == 44u;
	}

	void adapt_lockstep_delay()
	{
		if (lockstep)
		{
			// Leaving character select starts a new phase, so its larger delay does not carry into the round intro.
			const bool select = at_character_select();
			if (!data.peer.lock_phase || data.peer.lock_at_select != select)
				data.peer.lock_delay = 0;
			data.peer.lock_at_select = select;
			u32 wanted = delay_here;
			if (data.peer.rtt.ring_count >= 8)
			{
				const u32 n = std::min<u32>(data.peer.rtt.ring_count, 32u);
				u32 smallest = ~u32{0};
				for (u32 k = 0; k < n; k++)
					smallest = std::min(smallest, data.peer.rtt.ring[(data.peer.rtt.ring_count - 1 - k) % data.peer.rtt.ring.size()]);
				wanted = std::clamp<u32>(static_cast<u32>(std::ceil((smallest / 2.0 + 8000.0) / frame_period_us)), 1u, select ? max_select_delay : max_lockstep_delay);
			}
			data.peer.lock_delay = std::max(data.peer.lock_delay, wanted);
			delay_here = std::max(delay_here, data.peer.lock_delay);
		}
	}

	void fill_unassigned_inputs()
	{
		for (u32 t = data.peer.live.assigned_any ? data.peer.live.assigned_through + 1 : index; t < target && t < limit; t++)
		{
			if (data.history.live_fixed[t])
				continue;
			data.history.live_inputs[t] = newest;
			data.history.live_fixed[t] = 1;
			data.peer.live.fills++;
			if (data.peer.input_trace)
				data.peer.input_trace->push({pbd::peer_input_trace::kind::fill, index, t, newest});
		}
	}

	void assign_sample()
	{
		if (target >= limit)
		{
			// No tick of the tape is left for this sample.
			how = "beyond_tape";
			data.peer.live.beyond_script++;
			for_tick = ~u32{0};
		}
		else if (!data.history.live_fixed[target] && (!data.peer.live.assigned_any || target > data.peer.live.assigned_through))
		{
			if (bootstrap)
			{
				how = "bootstrap";
				data.peer.live.bootstrap_admitted++;
			}
			if (data.peer.live.edge_carry)
			{
				edge_carried = data.peer.live.edge_carry & ~sample & 0x0fff;
				sample |= edge_carried;
				data.peer.live.edge_carry = 0;
			}
			if (data.peer.live.released)
			{
				edge_deferred = data.peer.live.released & sample & ~edge_carried;
				sample &= ~edge_deferred;
				data.peer.live.edge_carry = edge_deferred;
				data.peer.live.released = 0;
				if (edge_deferred)
					data.peer.live.edges_deferred++;
			}
			data.history.live_inputs[target] = sample;
			data.history.live_fixed[target] = 1;
			data.peer.live.shrinking = 0;
		}
		else if (predicted && target < data.peer.fight_start)
		{
			// Dropped before the round: its tick is below X and already fixed. Buttons that went down here stay masked;
			// buttons of the newest fixed tick that went up here must not lose the edge of a re-press (peer.live.released).
			how = "predrop";
			data.peer.live.predrops++;
			for_tick = ~u32{0};
			if (physical != newest)
				data.peer.live.predrops_changed++;
			data.peer.live.suppress |= (physical & ~newest) & 0x0fff;
			data.peer.live.released = static_cast<u16>((data.peer.live.released | (newest & ~physical)) & 0x0fff);
		}
		else if (sample == newest)
		{
			how = "redundant";
			data.peer.live.redundant++;
			for_tick = ~u32{0};
		}
		else if (++data.peer.live.shrinking > 60)
		{
			how = "forced";
			data.peer.live.forced++;
			for_tick = ~u32{0};
		}
		else if (data.peer.live.assigned_any && data.peer.live.assigned_through + 1 < limit && !data.history.live_fixed[data.peer.live.assigned_through + 1])
		{
			for_tick = data.peer.live.assigned_through + 1;
			data.history.live_inputs[for_tick] = sample;
			data.history.live_fixed[for_tick] = 1;
			how = "deferred";
			data.peer.live.deferred++;
		}
		else
		{
			how = "forced";
			data.peer.live.forced++;
			for_tick = ~u32{0};
		}
	}
};
