// Practice dummy slot selection, recording and per-frame playback input.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_practice_lesson_channel.h"
#include "../PBD/pbd_dummy_move_runtime.h"
#include "../PBD/pbd_dummy_cycle_steps.h"

bool pbd::rollback_session::boundary_call::practice_dummy_choose_slot()
{
	// Offense's reaction plays its one chosen slot, whatever Defense's play mode says, and never draws a random number.
	if (practice_dummy_reacting())
		return choose_reaction_slot();
	if (data.practice.training_mode != pbd::practice::training_mode::defense)
	{
		data.practice.dummy.have_play_slot = false;
		return false;
	}
	const auto available = [&](u32 i)
	{
		return practice_slot_playable(data.practice.dummy.slots[i]);
	};
	u32 chosen = data.practice.dummy.slot_index;
	if (data.practice.play_mode == 0)
	{
		if (!available(chosen))
		{
			data.practice.dummy.have_play_slot = false;
			return false;
		}
	}
	else if (data.practice.play_mode == 1)
	{
		const u32 start = data.practice.dummy.have_play_slot ? (data.practice.dummy.play_slot + 1) % 8 : chosen;
		bool found = false;
		for (u32 n = 0; n < 8; ++n)
			if (available((start + n) % 8))
			{
				chosen = (start + n) % 8;
				found = true;
				break;
			}
		if (!found)
		{
			data.practice.dummy.have_play_slot = false;
			return false;
		}
	}
	else
	{
		u32 total = 0;
		for (u32 i = 0; i < 8; ++i)
			if (available(i))
				total += data.practice.dummy.slots[i].frequency;
		if (!total)
		{
			data.practice.dummy.have_play_slot = false;
			return false;
		}
		u32 pick = practice_dummy_random() % total;
		for (u32 i = 0; i < 8; ++i)
		{
			if (!available(i))
				continue;
			if (pick < data.practice.dummy.slots[i].frequency)
			{
				chosen = i;
				break;
			}
			pick -= data.practice.dummy.slots[i].frequency;
		}
	}
	data.practice.dummy.play_slot = chosen;
	data.practice.dummy.play_pos = 0;
	data.practice.dummy.have_play_slot = true;
	// A slot with moves plays a move picked afresh each time it is chosen.
	practice_pick_slot_move(chosen);
	return true;
}

// Starts playback's cycle from its beginning: under Shuffle a random opening gap (which plays the movement), then the
// ready gate; otherwise the ready gate at once, so the first action also starts from a dummy at rest. The neutral run
// counts from here: what the dummy did before playback (or before a Restart) is not the host's input.
void pbd::rollback_session::boundary_call::practice_dummy_begin_cycle()
{
	data.practice.dummy.play_pos = 0;
	data.practice.movement.pos = 0;
	data.practice.dummy.neutral_run = 0;
	data.practice.dummy.gate = 0;
	data.practice.dummy.gap_over = 0;
	data.practice.dummy.gap_hit = false;
	data.practice.dummy.moved = false;
	data.practice.dummy.move_gate = 0;
	data.practice.movement.started = false;
	data.practice.dummy.turn.reset();
	data.practice.dummy.flow = {};
	data.practice.dummy.flow_given = 0;
	data.practice.dummy.play_first = 0;
	data.practice.movement.pause.reset();
	// Offense's reaction goes straight to the ready gate: Defense's Shuffle gap is not its setting.
	data.practice.dummy.wait = data.practice.interval == 3 && !practice_dummy_reacting() ? 30 + practice_dummy_random() % 91 : 0;
	// A one-shot movement comes before every action, the first one included: a gap just long enough for its motion.
	if (!data.practice.dummy.wait && !practice_dummy_reacting() && data.practice.movement.input_sequence.once)
		data.practice.dummy.wait = static_cast<u32>(data.practice.movement.input_sequence.frames.size());
	data.practice.dummy.phase = data.practice.dummy.wait ? dummy_gap : dummy_ready;
}

// Starts playback's routine afresh: the cycle of the next action when one can play, otherwise the movement on its own,
// which loops from the gap's start until playback stops (or, played once after a contact, for after_contact_frames, a
// one-shot just once). False when there is neither (Neutral or a one-shot, and no action).
bool pbd::rollback_session::boundary_call::practice_dummy_begin_routine()
{
	data.practice.dummy.have_play_slot = false;
	const bool reacting = practice_dummy_reacting();
	const bool action = (reacting || practice_dummy_available()) && practice_dummy_choose_slot();
	// A reaction whose slot cannot play has nothing else to play: the Movement is not part of it.
	if (!action && (reacting || !practice_movement_alone()))
		return false;
	practice_dummy_begin_cycle();
	if (!action)
	{
		data.practice.dummy.wait = data.practice.dummy.once ? pbd::practice::movement::after_contact_wait(data.practice.movement.input_sequence) : 1;
		data.practice.dummy.phase = dummy_gap;
	}
	return true;
}

bool pbd::rollback_session::boundary_call::practice_dummy_start_playback(u32 index)
{
	if (practice_move_reactions_on())
	{
		data.practice.view.toast = "Set Action After a Hit or Block to None to play the dummy continuously";
		data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(4);
		return false;
	}
	// Every recorded action at Frequency 0: say so, and play the movement alone if there is one.
	const bool all_zero = !practice_dummy_reacting() && practice_dummy_all_zero();
	if (all_zero)
	{
		data.practice.view.toast = practice_movement_alone() ? "Every Dummy Action is at Frequency 0\tOnly the movement plays; {l1}/{r1} raises a Frequency" :
		                                                       "Every Dummy Action is at Frequency 0\t{l1}/{r1} on a Dummy Action raises its Frequency";
		data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(4);
		practice_dummy_log(fmt::format("{\"type\":\"dummy_all_frequency_zero\",\"boundary\":%u,\"movement_alone\":%s}\n", index,
			json_bool(practice_movement_alone())));
		if (!practice_movement_alone())
			return false;
	}
	if (!practice_dummy_reacting() && !practice_dummy_available() && !practice_movement_alone())
	{
		// A one-shot movement has nothing to come before, so the dummy stays with the game's Dummy Action.
		const char* empty = data.practice.play_mode == 0 ? "That Dummy Action is empty" : "Record an action first";
		data.practice.view.toast = !data.practice.movement.input_sequence.empty() ?
		                               fmt::format("%s\t%s plays only before an action", empty, practice_movement_label()) :
		                           data.practice.play_mode == 0 ? "That Dummy Action is empty\tChoose a movement to play it alone" :
		                                                          "Record an action first\tor choose a Movement Between Actions";
		data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(4);
		return false;
	}
	data.practice.dummy.preview = false;
	if (!practice_dummy_begin_routine())
	{
		data.practice.dummy.state = dummy_state::idle;
		return false;
	}
	data.practice.dummy.state = dummy_state::playing;
	data.practice.dummy.plays++;
	practice_dummy_log(fmt::format("{\"type\":\"dummy_play_start\",\"boundary\":%u,\"slot\":%u,\"saved\":%u,\"rng\":%u,\"opening_gap\":%u,\"rest_reader\":%s,\"movement\":%u,\"mode\":%u,\"once\":%s}\n",
		index, data.practice.dummy.have_play_slot ? data.practice.dummy.play_slot + 1 : 0u, practice_dummy_saved(), data.practice.dummy.rng,
		data.practice.dummy.wait, json_bool(data.practice.dummy.rest.has_value()), static_cast<u32>(practice_active_movement()),
		static_cast<u32>(data.practice.training_mode), json_bool(data.practice.dummy.once)));
	return true;
}

// Starts Offense's own dummy routine when it is due while the fight runs: a Movement that plays all the time (Action
// After a Hit or Block: None; a one-shot never plays alone, so the dummy keeps to the game's Dummy Action), or the
// reaction once the dummy has recovered from the counted contacts. Run by the controls, and again right after the
// contact watch so the reaction starts on the frame the dummy can act.
void pbd::rollback_session::boundary_call::practice_offense_begin(u32 index)
{
	if (!(practice_offense_movement_on() || practice_offense_reacts()) || data.practice.dummy.state != dummy_state::idle ||
		!data.session.in_match ||
		data.practice.menu.open || data.practice.native.screen != 0 || !data.practice.native.pause_valid || data.practice.native.paused ||
		data.practice.restart.reset_request || data.practice.pause.requested || data.practice.pause.resume_native)
		return;
	const bool after_contact = data.practice.offense.after != pbd::practice::after_contact::none;
	if (after_contact ? !data.practice.contact.ready : !pbd::practice::movement::plays_alone(data.practice.movement.input_sequence))
		return;
	data.practice.dummy.once = after_contact;
	data.practice.dummy.reaction = data.practice.offense.after == pbd::practice::after_contact::action;
	data.practice.contact.ready = false;
	if (!practice_dummy_start_playback(index))
	{
		data.practice.dummy.once = false;
		return;
	}
	// While the watch ran none of our input reached the dummy (its port gave neutral), so the reaction's lead-in is
	// already clean and its take starts on the first frame the dummy can act, which the watch has just seen.
	if (after_contact)
		data.practice.dummy.neutral_run = (std::max)(data.practice.dummy.neutral_run, pbd::practice::movement::action_lead_neutral_frames);
}

// A Restart or a rewind ends a movement or Dummy Action played once after a contact (the contact belonged to the old
// situation) and starts the contact watch, and with it the count, again, as it does the guard choices and Counter Hit's Random.
void pbd::rollback_session::boundary_call::practice_restart_contact()
{
	pbd::practice::dummy_move::reset_playback();
	pbd::practice::dummy_move::current.contacts = {};
	data.practice.contact.reaction_count.reset();
	data.practice.reactions.wakeup_control.reset();
	data.practice.reactions.wakeup_suspended = false;
	data.practice.reactions.guard.restart();
	data.practice.reactions.counter_hit.restart();
	data.practice.reactions.throw_escape_control.reset();
	if (data.practice.dummy.state == dummy_state::playing && data.practice.dummy.once)
		data.practice.dummy.state = dummy_state::idle;
	data.practice.dummy.once = false;
	data.practice.dummy.reaction = false;
	data.practice.contact.watching = false;
	data.practice.contact.ready = false;
	data.practice.dummy.hit = false;
	// Words read before the restart never run after it.
	data.practice.input_delay.clear();
}

void pbd::rollback_session::boundary_call::practice_restart_after_restore(u32 from)
{
	const u32 old_position = data.practice.dummy.play_pos;
	data.practice.history.reset();
	data.practice.health_valid = false;
	data.practice.view.fighters = {};
	data.practice.combos = {};
	data.practice.streak.reset();
	data.practice.precision.reset();
	data.practice.punish.reset();
	data.practice.view.fight_live = false;
	data.practice.dummy.play_pos = 0;
	data.practice.dummy.wait = 0;
	data.practice.movement.pos = 0;
	data.practice.dummy.have_play_slot = false;
	data.practice.dummy.neutral_run = 0;
	practice_capture_mark("rewind", from);
	practice_restart_contact();
	if (data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview)
	{
		// Restart starts the configured selection afresh instead of advancing Cycle from the previous slot.
		if (!practice_dummy_begin_routine())
			data.practice.dummy.state = dummy_state::idle;
	}
	// Preview retains the pending take and begins immediately; kept tapes and host random/cycle counters survive the restore.
	data.practice.pause.resume_native = false;
	data.practice.pause.resume_injected = false;
	data.practice.dummy.ignore_until_release = true;
	data.practice.dummy.mask_ui = true;
	data.practice.play_before.valid = false;
	data.practice.record_word_valid = false;
	const u32 shown_slot = !data.practice.dummy.preview && data.practice.dummy.state == dummy_state::playing &&
	                               data.practice.dummy.have_play_slot ?
	                           data.practice.dummy.play_slot :
	                           data.practice.dummy.slot_index;
	practice_dummy_log(fmt::format("{\"type\":\"dummy_restart\",\"boundary\":%u,\"from\":%u,\"to\":%u,\"pass\":%u,\"state\":%u,\"preview\":%s,\"slot\":%u,\"position_before\":%u,\"position\":%u,\"wait\":%u,\"ignore\":true,\"pending_frames\":%u}\n",
		from, from, data.takeover.keyframe.index, data.takeover.restore_pass, static_cast<u32>(data.practice.dummy.state),
		json_bool(data.practice.dummy.preview), shown_slot + 1, old_position, data.practice.dummy.play_pos,
		data.practice.dummy.wait, static_cast<u32>(data.practice.dummy.pending.words.size())));
}

void pbd::rollback_session::boundary_call::practice_dummy_stop_playback(u32 index, const char* reason)
{
	const bool preview = data.practice.dummy.preview;
	data.practice.dummy.preview = false;
	data.practice.dummy.state = preview ? dummy_state::confirm : dummy_state::idle;
	data.practice.dummy.choice = 0;
	data.practice.dummy.have_play_slot = false;
	data.practice.dummy.play_pos = 0;
	data.practice.dummy.wait = 0;
	data.practice.movement.pos = 0;
	data.practice.dummy.once = false;
	data.practice.play_before.valid = false;
	data.practice.dummy.ignore_until_release = true;
	data.practice.dummy.mask_ui = true;
	if (preview)
		pbd::practice::input_claim.store(true, std::memory_order_release);
	practice_dummy_log(fmt::format("{\"type\":\"%s\",\"boundary\":%u,\"reason\":\"%s\"}\n", preview ? "dummy_preview_end" : "dummy_play_stop", index, reason));
}

void pbd::rollback_session::boundary_call::practice_dummy_start_record(u32 index)
{
	data.practice.dummy.preview = false;
	practice_clear_pending();
	data.practice.dummy.state = dummy_state::recording;
	// Set with the take's first kept word (for the logs; every word carries its own side).
	data.practice.dummy.pending.side.reset();
	data.practice.dummy.choice = 0;
	data.practice.dummy.recordings++;
	data.practice.dummy.mask_ui = true;
	data.practice.dummy.ignore_until_release = true;
	practice_dummy_log(fmt::format("{\"type\":\"dummy_record_start\",\"boundary\":%u,\"slot\":%u,\"cap_frames\":1800}\n",
		index, practice_record_log_slot()));
}

// Record from a slot: back in the fight with the dummy under the player's control, waiting for SELECT+Circle.
// Any playback ends here; the take itself starts from an empty buffer.
void pbd::rollback_session::boundary_call::practice_dummy_arm(u32 index)
{
	data.practice.dummy.preview = false;
	practice_clear_pending();
	data.practice.dummy.have_play_slot = false;
	data.practice.dummy.play_pos = 0;
	data.practice.dummy.wait = 0;
	data.practice.movement.pos = 0;
	data.practice.play_before.valid = false;
	data.practice.dummy.once = false;
	data.practice.dummy.state = dummy_state::ready;
	data.practice.dummy.choice = 0;
	data.practice.dummy.mask_ui = true;
	data.practice.dummy.ignore_until_release = true;
	practice_dummy_log(fmt::format("{\"type\":\"dummy_record_ready\",\"boundary\":%u,\"slot\":%u}\n",
		index, practice_record_log_slot()));
}

// chord_tail: the frames the SELECT+Circle stop's early Circle reached (record_stop); 0 for any other stop.
void pbd::rollback_session::boundary_call::practice_dummy_stop_record(u32 index, const char* reason, u32 chord_tail)
{
	data.practice.dummy.pending.chord = (std::min)(chord_tail, static_cast<u32>(data.practice.dummy.pending.words.size()));
	practice_measure_pending();
	data.practice.dummy.state = dummy_state::confirm;
	data.practice.dummy.choice = 0;
	data.practice.dummy.mask_ui = true;
	data.practice.dummy.ignore_until_release = true;
	practice_dummy_log(fmt::format("{\"type\":\"dummy_record_stop\",\"boundary\":%u,\"slot\":%u,\"reason\":\"%s\",\"frames\":%u,\"stop_chord_frames\":%u,\"digest\":\"%016x\"}\n",
		index, practice_record_log_slot(), reason, static_cast<u32>(data.practice.dummy.pending.words.size()), data.practice.dummy.pending.chord,
		practice_dummy_digest(data.practice.dummy.pending.words)));
}

void pbd::rollback_session::boundary_call::practice_dummy_accept(u32 index, bool autoplay)
{
	auto& slot = practice_record_slot();
	// A failed native read can lead straight to Keep, without passing the ordinary Stop path.
	practice_measure_pending();
	// A kept take is the slot's action again: moves chosen for it from the move list give way to it.
	slot.enabled = true;
	slot.moves.clear();
	slot.move_sequence.clear();
	slot.frames.assign(data.practice.dummy.pending.words.begin(), data.practice.dummy.pending.words.end());
	slot.side = data.practice.dummy.pending.side;
	slot.leading_neutral_frames = data.practice.dummy.pending.leading;
	slot.trailing_neutral_frames = data.practice.dummy.pending.trailing;
	slot.stop_chord_frames = data.practice.dummy.pending.chord;
	practice_clear_pending();
	practice_dummy_log(fmt::format("{\"type\":\"dummy_record_accept\",\"boundary\":%u,\"slot\":%u,\"frames\":%u,\"playback_frames\":%u,\"leading_neutral_frames\":%u,\"trailing_neutral_frames\":%u,\"stop_chord_frames\":%u,\"trim\":%s,\"playable\":%s,\"frequency\":%u,\"interval\":%u}\n",
		index, practice_record_log_slot(), static_cast<u32>(slot.frames.size()), static_cast<u32>(practice_slot_frames(slot).size()),
		slot.leading_neutral_frames, slot.trailing_neutral_frames, slot.stop_chord_frames, json_bool(slot.trim_neutral),
		json_bool(pbd::practice::controls::has_input(practice_slot_frames(slot))), slot.frequency, slot.interval));
	practice_save_input_sequence(slot, practice_record_log_slot(), practice_record_name(), index);
	if (autoplay)
		practice_dummy_start_playback(index);
}

u16 pbd::rollback_session::boundary_call::practice_dummy_word(u32 index)
{
	if (const auto result = dummy_word_without_cycle(index))
		return *result;
	return dummy_cycle_steps{*this, index}.run();
}

std::optional<u16> pbd::rollback_session::boundary_call::dummy_word_without_cycle(u32 index)
{
	data.practice.record_word_valid = false;
	data.practice.play_before.valid = false;
	// The final port latch withholds input during a restored chord's release; that neutral frame cannot consume a tape word.
	if (!data.practice.dummy.enabled || data.takeover.ignore_pad_until_release || data.practice.native.paused ||
		data.practice.dummy.ignore_until_release || data.practice.dummy.mask_ui || data.game_overlay.claim)
	{
		data.practice.reactions.wakeup_control.reset();
		data.practice.reactions.wakeup_suspended = true;
		data.practice.reactions.throw_escape_control.reset();
		if (data.practice.dummy.enabled)
			if (const auto floor = pbd::practice::native::read_wakeup(); floor && !floor->standing)
			{
				// Releasing a restored chord cannot let the stock dummy get up or consume a queued floor attack.
				data.practice.reactions.wakeup_control.arm(pbd::practice::pbd_wakeup_option::stay_down);
				data.practice.reactions.wakeup_control.sample(*floor);
			}
		return 0;
	}
	if (std::exchange(data.practice.reactions.wakeup_suspended, false))
		data.practice.reactions.wakeup_control.reset();
	// Ready to record drives the dummy exactly as recording does; only recording keeps the words.
	if (data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::ready)
	{
		data.practice.reactions.wakeup_control.reset();
		data.practice.reactions.throw_escape_control.reset();
		u16 word = data.practice.dummy.test.started && !data.practice.dummy.test.stopped ?
		               data.practice.dummy.test.word :
		               data.practice.dummy.pad;
		word &= static_cast<u16>(~(session_start_bit | session_select_bit));
		// The live controller drives P2 here, so SELECT is the same modifier as on P1: nothing under it reaches the take.
		word = data.practice.record_select.filter(word, (data.practice.dummy.pad & session_select_bit) != 0, data.practice.dummy.pad);
		// A stick click with an action records that action's attacks; the clicks themselves are never part of a take.
		word = pbd::practice::controls::map_attacks(word, data.practice.button_map);
		word &= static_cast<u16>(~pbd::controllers::settings::stick_clicks);
		// Input conversion precedes gameplay; only the next boundary's counter can confirm this word consumed a frame.
		// The side DR reads it with is kept beside it, so the take stores what the player meant (toward / away).
		data.practice.record_word = word;
		data.practice.record_side = practice_dummy_side();
		data.practice.record_word_valid = data.practice.dummy.state == dummy_state::recording && data.practice.native.pause_valid && data.practice.boundary_counter_valid;
		return word;
	}
	if (const auto word = dummy_throw_escape(index))
		return *word;
	if (const auto word = dummy_wakeup_word(index))
		return *word;
	if (data.practice.dummy.state != dummy_state::playing)
		return 0;
	data.practice.play_before = {true, data.practice.dummy.preview, data.practice.dummy.have_play_slot,
		data.practice.dummy.play_slot, data.practice.dummy.play_pos, data.practice.dummy.wait, data.practice.dummy.rng, data.practice.dummy.cycles,
		data.practice.movement.pos, data.practice.dummy.phase, data.practice.dummy.gate,
		data.practice.dummy.gap_over, data.practice.dummy.neutral_run, data.practice.dummy.caps, data.practice.dummy.facing.caps,
		data.practice.dummy.move_gate, data.practice.dummy.moved, data.practice.movement.started, data.practice.dummy.facing.proven,
		data.practice.dummy.turn.frames, data.practice.dummy.flow_given, data.practice.dummy.play_first, data.practice.dummy.flow};
	if (data.practice.dummy.preview)
	{
		const auto input_sequence = practice_pending_frames();
		if (data.practice.dummy.play_pos < input_sequence.size())
		{
			// The take is relative: each word becomes the pad directions DR reads as that input on the dummy's side now.
			const u16 recorded_word = input_sequence[data.practice.dummy.play_pos++];
			const auto side = practice_dummy_side();
			const u16 word = pbd::practice::movement::play_word(recorded_word, side);
			if (data.practice.dummy.play_pos == 1)
				practice_dummy_log(fmt::format("{\"type\":\"dummy_preview_sample\",\"boundary\":%u,\"slot\":%u,\"word\":%u,\"frame\":0,\"pass\":%u,\"recorded_side\":\"%s\",\"side\":\"%s\",\"side_read\":%s}\n",
					index, practice_record_log_slot(), word, data.takeover.restore_pass, pbd::practice::movement::side_text(data.practice.dummy.pending.side),
					pbd::practice::movement::side_text(side), json_bool(practice_side_of(1).has_value())));
			return word;
		}
		practice_dummy_stop_playback(index, "complete");
		return 0;
	}
	return std::nullopt;
}

// Offense's reaction is over: the dummy goes back to the game's Dummy Action (practice_dummy_route restores it for
// this frame) and the contact watch, with a fresh count, resumes. No interval and no next action follow.
void pbd::rollback_session::dummy_word_steps::end_reaction(const char* reason)
{
	data.practice.dummy.state = dummy_state::idle;
	data.practice.dummy.once = false;
	data.practice.dummy.reaction = false;
	data.practice.dummy.have_play_slot = false;
	data.practice.dummy.play_pos = 0;
	data.practice.dummy.wait = 0;
	data.practice.dummy.cycles++;
	practice_dummy_log(fmt::format("{\"type\":\"dummy_play_stop\",\"boundary\":%u,\"reason\":\"%s\",\"slot\":%u}\n",
		index, reason, data.practice.dummy.play_slot + 1));
}

// The next cycle after an action, or after one skipped: the Action Interval counts from here. False when playback
// ends.
bool pbd::rollback_session::dummy_word_steps::next_cycle()
{
	const u32 interval = data.practice.interval == 0 ? 6 : data.practice.interval == 1 ? 60 :
	                                                   data.practice.interval == 2     ? 120 :
	                                                                                     30 + practice_dummy_random() % 91;
	data.practice.dummy.wait = interval;
	data.practice.dummy.cycles++;
	// With no action left to play a continuous movement carries on alone (the gap below does not end); with
	// Neutral or a one-shot there is nothing left and playback ends, leaving the dummy to the game's Dummy Action.
	if (!practice_dummy_choose_slot())
	{
		if (!practice_movement_alone())
		{
			data.practice.dummy.state = dummy_state::idle;
			return false;
		}
		data.practice.dummy.have_play_slot = false;
	}
	data.practice.movement.pos = 0;
	data.practice.dummy.gap_over = 0;
	data.practice.dummy.gap_hit = false;
	data.practice.dummy.gate = 0;
	data.practice.dummy.move_gate = 0;
	data.practice.movement.started = false;
	data.practice.dummy.phase = dummy_gap;
	practice_dummy_log(fmt::format("{\"type\":\"dummy_cycle\",\"boundary\":%u,\"cycle\":%u,\"next_slot\":%u,\"interval_frames\":%u,\"rng\":%u,\"movement\":%u}\n",
		index, data.practice.dummy.cycles, data.practice.dummy.have_play_slot ? data.practice.dummy.play_slot + 1 : 0u, interval,
		data.practice.dummy.rng, static_cast<u32>(practice_active_movement())));
	return true;
}
