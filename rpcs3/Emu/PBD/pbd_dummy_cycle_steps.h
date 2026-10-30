#pragma once

namespace movement = pbd::practice::movement;
struct pbd::rollback_session::dummy_cycle_steps : dummy_word_steps
{
	const std::optional<bool> rest;
	const movement::facing facing;
	std::optional<bool> start_rest;
	const movement::input_sequence& loop;
	bool paused = false, pass_start = false, first_pass = false, unrested = false, turning = false, hold = false;
	movement::screen_side gap_side{};
	movement::flow_plan plan{};
	movement::gap_frame frame{};
	dummy_cycle_steps(boundary_call& call, u32& tick) : dummy_word_steps{call, tick}, rest(data.practice.dummy.rest), facing(data.practice.dummy.facing.verdict), loop(data.practice.movement.input_sequence) {}
	u16 run()
	{
		// Playback cycles through action, recovery gate, gap and ready gate. Only the gap plays the movement loop, and only
		// the action plays a recorded take; a phase that ends hands over within the same frame, so no frame is skipped.
		prepare_cycle();

		for (u32 handover = 0; handover < 4; ++handover)
		{
			if (data.practice.dummy.phase == dummy_recover)
			{
				if (const auto word = recover())
					return *word;
				continue;
			}
			if (data.practice.dummy.phase == dummy_gap)
			{
				if (const auto word = gap())
					return *word;
				continue;
			}
			if (data.practice.dummy.phase == dummy_ready)
			{
				if (const auto word = ready())
					return *word;
				continue;
			}
			if (const auto word = action())
				return *word;
		}
		return give(0);
	}

	void prepare_cycle()
	{
		// A reaction to a hit or block starts on the frame the game lets the dummy act (practice_watch_contact), while the
		// dummy still plays out its block or hit reaction and the rest reading still shows it away from rest. Until the
		// reaction's first input (the gap's first pass, the ready gate) that frame counts as rest, so neither gate holds the
		// input back to the end of the reaction. Every later gate reads rest as it is.
		const bool reaction_free = data.practice.dummy.once && data.practice.boundary_counter_valid &&
		                           pbd::practice::dummy_move::current.contacts.free_next(data.practice.boundary_counter);
		start_rest = reaction_free ? std::optional<bool>(true) : rest;
	}

	u16 give(u16 word)
	{
		data.practice.dummy.neutral_run = word ? 0u : (std::min)(data.practice.dummy.neutral_run + 1, 0xffffu);
		return word;
	}

	void log_wait(const char* gate, movement::settle result, u32 frames)
	{
		const auto& sample = data.practice.movement.moves[1];
		practice_dummy_log(fmt::format("{\"type\":\"dummy_wait\",\"boundary\":%u,\"cycle\":%u,\"gate\":\"%s\",\"frames\":%u,\"result\":\"%s\",\"reader\":\"%s\",\"move\":%s,\"rest\":%s,\"neutral_run\":%u,\"moved\":%s%s}\n",
			index, data.practice.dummy.cycles, gate, frames, movement::settle_text(result),
			rest ? "move_entry" : "none", sample ? std::to_string(sample->entry) : std::string("null"), rest ? json_bool(*rest) : "null",
			data.practice.dummy.neutral_run, json_bool(data.practice.dummy.moved), practice_facing_json()));
	}

	// Facing-only caps count toward distrusting an unproven reading; facing P1 proves it and clears that count.
	void facing_gate_end(bool held_to_cap)
	{
		if (held_to_cap)
		{
			if (!data.practice.dummy.facing.proven && ++data.practice.dummy.facing.caps == storage::settle_distrust_caps)
				practice_dummy_log(fmt::format("{\"type\":\"dummy_facing_set_aside\",\"boundary\":%u,\"caps\":%u}\n",
					index, data.practice.dummy.facing.caps));
		}
		else if (facing == movement::facing::toward)
		{
			data.practice.dummy.facing.caps = 0;
			data.practice.dummy.facing.proven = true;
		}
	}

	// Turn toward P1 on the dummy's input side, with neutral input between taps.
	u16 practice_dummy_turn_word(const char* gate, u32 waited)
	{
		if (data.practice.dummy.turn.tap_starts())
			practice_dummy_log(fmt::format("{\"type\":\"dummy_turn\",\"boundary\":%u,\"cycle\":%u,\"gate\":\"%s\",\"tap\":%u,\"waited\":%u,\"side\":\"%s\"%s}\n",
				index, data.practice.dummy.cycles, gate, data.practice.dummy.turn.taps() + 1, waited,
				movement::side_text(practice_dummy_side()), practice_facing_json()));
		return movement::play_word(data.practice.dummy.turn.next(), practice_dummy_side());
	}

	std::optional<u16> recover()
	{
		// After the action's last input: neutral until the dummy is back at rest (its move and recovery are over),
		// so the movement or the next action never lands inside the move or is read as its follow-up. While
		// playback is paused for a hit the game drives the dummy and the gate only counts: the pause has its own
		// cap, and a combo and get-up longer than the gate's cap is no sign of a wrong reading.
		if (rest == false)
			data.practice.dummy.moved = true;
		if (data.practice.movement.pause.active)
		{
			data.practice.dummy.gate++;
			return give(0);
		}
		const auto verdict = movement::recovery_verdict(rest, data.practice.dummy.gate, data.practice.dummy.moved);
		if (verdict == movement::settle::waiting)
		{
			data.practice.dummy.gate++;
			return give(0);
		}
		log_wait("recovery", verdict, data.practice.dummy.gate);
		// A gate the reading contradicted extends the run that sets it aside (practice_state::dummy_engine_state::caps); any other
		// end breaks the run. Once set aside it stays aside for this character. A hit's pause that ran the gate to its
		// cap and ended there itself (movement::movement_pause::capped) says nothing either way.
		const bool pause_capped = verdict == movement::settle::capped && data.practice.movement.pause.capped;
		if (!pause_capped && ((verdict == movement::settle::capped && rest == false) || verdict == movement::settle::unconfirmed))
		{
			if (++data.practice.dummy.caps == storage::settle_distrust_caps)
				practice_dummy_log(fmt::format("{\"type\":\"dummy_reader_set_aside\",\"boundary\":%u,\"caps\":%u,\"last\":\"%s\"}\n",
					index, data.practice.dummy.caps, movement::settle_text(verdict)));
		}
		else if (!pause_capped && data.practice.dummy.caps < storage::settle_distrust_caps)
			data.practice.dummy.caps = 0;
		if (practice_dummy_reacting())
		{
			// Offense's Dummy Action after a contact has played and the dummy has recovered from it.
			end_reaction("after_contact_done");
			return give(0);
		}
		if (!next_cycle())
			return give(0);
		return std::nullopt;
	}

	std::optional<u16> gap()
	{
		// The Action Interval: the movement loop plays here and nowhere else; with no movement it stays neutral.
		// With no action to play the loop is all the dummy does, so the interval never runs out. An action that
		// becomes playable meanwhile (a slot or the play mode changed in the menu) is picked up when the pass in
		// progress ends.
		if (!data.practice.dummy.have_play_slot)
		{
			if (practice_dummy_available() && practice_dummy_choose_slot())
				data.practice.dummy.wait = 0;
			else if (!data.practice.dummy.once)
				data.practice.dummy.wait = (std::max)(data.practice.dummy.wait, 1u);
		}
		// A one-shot movement plays only before an action (or once after a contact): with none, playback ends and the
		// dummy follows the game's Dummy Action.
		if (!data.practice.dummy.have_play_slot && !practice_movement_alone())
		{
			data.practice.dummy.state = dummy_state::idle;
			data.practice.movement.pos = 0;
			data.practice.dummy.wait = 0;
			practice_dummy_log(fmt::format("{\"type\":\"dummy_play_stop\",\"boundary\":%u,\"reason\":\"nothing_to_play\"}\n", index));
			return give(0);
		}
		prepare_gap_frame();
		if (!frame.done)
		{
			if (hold)
			{
				if (!paused)
					data.practice.dummy.move_gate++;
				if (turning)
					return give(practice_dummy_turn_word("movement", data.practice.dummy.move_gate));
			}
			else if (pass_start)
			{
				const bool capped = data.practice.dummy.move_gate >= movement::settle_cap_frames;
				if (first_pass || data.practice.dummy.move_gate)
					log_wait("movement", capped ? movement::settle::capped : movement::settle::actionable, data.practice.dummy.move_gate);
				// Only a cap the facing alone caused counts against the facing reading: the rest reading may have held
				// the dummy too.
				facing_gate_end(capped && movement::faces_away(facing) && !unrested);
				data.practice.dummy.move_gate = 0;
				data.practice.movement.started = true;
				data.practice.dummy.turn.reset();
			}
			return give(frame.word);
		}
		if (frame.flow)
		{
			begin_flow();
			return std::nullopt;
		}
		if (data.practice.dummy.gap_over)
			practice_dummy_log(fmt::format("{\"type\":\"dummy_movement\",\"boundary\":%u,\"cycle\":%u,\"finished_pass_frames\":%u}\n",
				index, data.practice.dummy.cycles, data.practice.dummy.gap_over));
		if (data.practice.dummy.once)
		{
			// The movement after a contact has had its time: the dummy goes back to the game's Dummy Action
			// (practice_dummy_route restores it for this frame) and the contact watch resumes.
			data.practice.dummy.state = dummy_state::idle;
			data.practice.dummy.once = false;
			data.practice.dummy.have_play_slot = false;
			data.practice.movement.pos = 0;
			data.practice.dummy.wait = 0;
			practice_dummy_log(fmt::format("{\"type\":\"dummy_play_stop\",\"boundary\":%u,\"reason\":\"after_contact_done\"}\n", index));
			return give(0);
		}
		data.practice.dummy.gate = 0;
		data.practice.dummy.phase = dummy_ready;
		return std::nullopt;
	}

	std::optional<u16> ready()
	{
		// Before the action: neutral until the dummy is at rest, has had a clean neutral lead-in and faces P1, so the
		// take's first inputs are fresh presses from a standing dummy (its directions follow DR's input side frame by
		// frame below). A dummy with its back to P1 is turned around first with forward taps (turn_around), each
		// followed by a settle longer than the lead-in, until it faces P1 or the gate reaches its cap. While playback
		// is paused for a hit the game drives the dummy, so no action starts; the gate counts from the pause's end.
		if (data.practice.movement.pause.active)
			return give(0);
		if (data.practice.dummy.turn.active(facing, data.practice.dummy.gate < movement::settle_cap_frames && rest != false))
		{
			data.practice.dummy.gate++;
			return give(practice_dummy_turn_word("ready", data.practice.dummy.gate));
		}
		const auto verdict = movement::with_facing(movement::ready_verdict(start_rest, data.practice.dummy.gate, data.practice.dummy.neutral_run),
			facing, data.practice.dummy.gate);
		if (verdict == movement::settle::waiting)
		{
			data.practice.dummy.gate++;
			return give(0);
		}
		log_wait("ready", verdict, data.practice.dummy.gate);
		facing_gate_end((verdict == movement::settle::capped || verdict == movement::settle::skipped) && movement::faces_away(facing) &&
						movement::ready_verdict(rest, 0, data.practice.dummy.neutral_run) != movement::settle::waiting);
		if (verdict == movement::settle::skipped)
		{
			// Still backturned at the cap: the take would come out as the back-turned move, so this cycle has no
			// action. Not through the recovery gate, which would count the missing move against the rest reading.
			data.practice.dummy.gate = 0;
			data.practice.dummy.moved = false;
			data.practice.dummy.turn.reset();
			if (practice_dummy_reacting())
			{
				end_reaction("backturned");
				return give(0);
			}
			if (!next_cycle())
				return give(0);
			return std::nullopt;
		}
		data.practice.dummy.gate = 0;
		data.practice.dummy.play_pos = 0;
		data.practice.dummy.moved = false;
		data.practice.dummy.turn.reset();
		data.practice.dummy.flow = {};
		data.practice.dummy.flow_given = 0;
		data.practice.dummy.play_first = 0;
		data.practice.dummy.phase = dummy_action;
		return std::nullopt;
	}

	std::optional<u16> action()
	{
		// The lead-in of a movement that handed over to this action, before the take's first input: a wavedash's
		// Wavedash End, a release.
		if (data.practice.dummy.flow_given < data.practice.dummy.flow.frames())
			return give(movement::play_word(data.practice.dummy.flow.word(data.practice.dummy.flow_given++), practice_dummy_side()));
		if (!data.practice.dummy.have_play_slot || data.practice.dummy.play_slot >= data.practice.dummy.slots.size())
		{
			if (!practice_dummy_choose_slot())
			{
				// No action left: a continuous movement carries on alone, or with Neutral or a one-shot playback ends. A
				// reaction whose slot cannot play ends too (the Movement is not part of it).
				data.practice.dummy.have_play_slot = false;
				if (practice_dummy_reacting())
				{
					data.practice.dummy.once = false;
					data.practice.dummy.reaction = false;
					data.practice.dummy.state = dummy_state::idle;
					return give(0);
				}
				if (!practice_movement_alone())
				{
					data.practice.dummy.state = dummy_state::idle;
					return give(0);
				}
				data.practice.movement.pos = 0;
				data.practice.dummy.gap_over = 0;
				data.practice.dummy.gap_hit = false;
				data.practice.dummy.gate = 0;
				data.practice.dummy.move_gate = 0;
				data.practice.movement.started = false;
				data.practice.dummy.wait = 1;
				data.practice.dummy.phase = dummy_gap;
				return std::nullopt;
			}
		}
		auto& slot = data.practice.dummy.slots[data.practice.dummy.play_slot];
		const auto input_sequence = practice_slot_frames(slot);
		if (data.practice.dummy.play_pos >= input_sequence.size())
		{
			// The take's last input has been given (Trim Neutral leaves no trailing wait in the take).
			data.practice.dummy.gate = 0;
			data.practice.dummy.phase = dummy_recover;
			return std::nullopt;
		}
		if (data.practice.dummy.play_pos > data.practice.dummy.play_first && rest == false)
			data.practice.dummy.moved = true;
		const u16 recorded_word = input_sequence[data.practice.dummy.play_pos++];
		// The take is relative and is turned into pad directions frame by frame with the side DR reads this frame's word
		// with, the same way the recording turned each frame into a relative word: f stays toward the opponent and b away
		// from it whichever side the dummy stands on, right after a throw-break side swap included.
		const auto side = practice_dummy_side();
		const u16 word = movement::play_word(recorded_word, side);
		if (data.practice.dummy.play_pos == data.practice.dummy.play_first + 1)
			practice_dummy_log(fmt::format("{\"type\":\"dummy_play_sample\",\"boundary\":%u,\"slot\":%u,\"word\":%u,\"frame\":0,\"pass\":%u,\"recorded_side\":\"%s\",\"side\":\"%s\",\"side_read\":%s%s}\n",
				index, data.practice.dummy.play_slot + 1, word, data.takeover.restore_pass, movement::side_text(slot.side),
				movement::side_text(side), json_bool(practice_side_of(1).has_value()), practice_facing_json()));
		return give(word);
	}

	void prepare_gap_frame()
	{
		// Readiness before movement: its first pass in this gap starts only with the dummy at rest and facing P1, and
		// every later pass only while it faces P1 (passes run into each other, so the dummy is never back at rest
		// between them; a held crouch or sidewalk is one pass). Jump Back's every pass waits for rest too, so each jump
		// lands first (tape::rest_each_pass). The dummy is held at the pass start meanwhile, for at most the gates' cap:
		// neutral, or forward taps that turn it around while it has its back to P1 (turn_around); an unknown reading does
		// not hold it. While playback is paused after a hit it is held there (the game drives the dummy meanwhile) until
		// the pause ends, which has its own cap.
		paused = data.practice.movement.pause.active;
		pass_start = movement::pass_due(loop, data.practice.movement.pos, data.practice.dummy.wait);
		first_pass = !data.practice.movement.started;
		unrested = (first_pass || loop.rest_each_pass) && (first_pass ? start_rest : rest) == false;
		turning = pass_start && !paused && !unrested &&
		          data.practice.dummy.turn.active(facing, data.practice.dummy.move_gate < movement::settle_cap_frames && rest != false);
		hold = pass_start && (paused || turning || ((unrested || movement::faces_away(facing)) && data.practice.dummy.move_gate < movement::settle_cap_frames));
		// The movement hands straight over to the action after it (movement::may_flow): the gap ends at the plan's
		// hand-over point and the action starts in this same frame, with no ready gate. Only Defense's gaps lead to an
		// action: Offense's movement plays alone, and its reaction starts from the game's Dummy Action through the
		// ready gate.
		const bool action_follows = loop.flow_at && data.practice.dummy.have_play_slot && !data.practice.dummy.once &&
		                            data.practice.dummy.play_slot < data.practice.dummy.slots.size() &&
		                            practice_slot_playable(data.practice.dummy.slots[data.practice.dummy.play_slot]);
		gap_side = practice_dummy_side();
		plan = action_follows ? movement::plan_flow(loop, practice_slot_frames(data.practice.dummy.slots[data.practice.dummy.play_slot]),
									gap_side, data.practice.movement.wavedash_end, data.practice.movement.dash_end) :
		                        movement::flow_plan{};
		frame = movement::gap_word(loop, data.practice.movement.pos, data.practice.dummy.wait, data.practice.dummy.gap_over,
			gap_side, hold, movement::may_flow(loop, action_follows, facing, data.practice.dummy.gap_hit) ? plan.at : 0u);
	}

	void begin_flow()
	{
		if (frame.flow)
		{
			practice_dummy_log(fmt::format("{\"type\":\"dummy_flow\",\"boundary\":%u,\"cycle\":%u,\"slot\":%u,\"movement\":%u,\"at\":%u,\"end_neutral_frames\":%u,\"end_frames\":%u,\"end\":\"%s\",\"release_frames\":%u,\"skipped_neutral\":%u,\"finished_pass_frames\":%u,\"side\":\"%s\"%s}\n",
				index, data.practice.dummy.cycles, data.practice.dummy.play_slot + 1, static_cast<u32>(practice_active_movement()), plan.at,
				plan.neutral_frames, plan.lead_frames, plan.lead_frames ? movement::direction_text(plan.lead_word, movement::screen_side::left) : std::string(),
				plan.release_frames, plan.first, data.practice.dummy.gap_over, movement::side_text(gap_side), practice_facing_json()));
			facing_gate_end(false);
			data.practice.dummy.gate = 0;
			data.practice.dummy.moved = false;
			data.practice.dummy.turn.reset();
			data.practice.dummy.play_pos = plan.first;
			data.practice.dummy.play_first = plan.first;
			data.practice.dummy.flow = plan;
			data.practice.dummy.flow_given = 0;
			data.practice.dummy.phase = dummy_action;
			return;
		}
	}
};
