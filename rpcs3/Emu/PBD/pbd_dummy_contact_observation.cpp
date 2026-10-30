#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_practice_lesson_channel.h"
#include "pbd_dummy_move_runtime.h"

namespace movement = pbd::practice::movement;

void pbd::rollback_session::boundary_call::practice_watch_contact(u32 index)
{
	namespace movement = pbd::practice::movement;
	const auto contacts = pbd::practice::dummy_move::current.contacts.take();
	const bool hit = std::exchange(data.practice.dummy.hit, false);
	practice_guard_frame(contacts, hit);
	if (hit && data.practice.dummy.enabled && data.practice.dummy.state != dummy_state::recording && data.practice.dummy.state != dummy_state::ready &&
		(!data.practice.reactions.wakeup_control.armed || data.practice.reactions.wakeup_control.active))
		data.practice.reactions.wakeup_control.arm(data.practice.reactions.wakeup,
			data.practice.reactions.wakeup == pbd::practice::pbd_wakeup_option::random ? practice_dummy_random() : 0);
	observe_dummy_playback_contact(index, hit);
	// The watch runs only while it can lead somewhere, in the fight, with the dummy left to Dummy Action.
	const bool watching = practice_offense_reacts() && data.practice.dummy.state == dummy_state::idle && data.session.in_match;
	if (!watching)
	{
		data.practice.contact.watching = false;
		data.practice.contact.ready = false;
		return;
	}
	if (!data.practice.contact.watching)
	{
		// The watch starts from this boundary's reading: whatever moved the dummy before belongs to our own input or
		// to a setting no longer in force. The count starts again with it, so a reaction, a Restart, a round start and
		// a changed setting (each of which ends the watch) all clear it.
		data.practice.contact.tracker = {};
		data.practice.contact.tracker.previous_rest = data.practice.dummy.rest;
		data.practice.contact.reaction_count.reset();
		data.practice.contact.watching = true;
		data.practice.contact.ready = false;
		return;
	}
	if (!data.practice.frame_advanced || data.practice.contact.ready)
		return;
	using event = movement::contact_tracker::event;
	data.practice.contact.reaction_count.applied(true, data.practice.offense.counted, data.practice.offense.times, contacts[0]);
	data.practice.contact.reaction_count.applied(false, data.practice.offense.counted, data.practice.offense.times, contacts[1]);
	// The game's own reading of the first frame the dummy can act (contact_batch::free_next) lets the reaction's first input
	// play on that frame; the rest reading would only see the dummy back at the end of its block or hit reaction, a dozen
	// frames later after a block. A dummy the wake-up control gets up keeps to the rest reading: that control owns its input
	// until it stands.
	const bool free_next = data.practice.boundary_counter_valid && !data.practice.reactions.wakeup_control.engaged &&
	                       pbd::practice::dummy_move::current.contacts.free_next(data.practice.boundary_counter);
	const auto seen = data.practice.contact.tracker.observe(hit, data.practice.dummy.rest, data.practice.dummy.facing.verdict, contacts[0] || contacts[1], free_next);
	if (seen == event::contact)
		practice_dummy_log(fmt::format("{\"type\":\"dummy_contact\",\"boundary\":%u,\"hit\":%s,\"rest\":%s%s}\n", index, json_bool(hit),
			data.practice.dummy.rest ? json_bool(*data.practice.dummy.rest) : "null", practice_facing_json()));
	else if (seen == event::ready || seen == event::dropped)
	{
		data.practice.contact.ready = seen == event::ready && data.practice.contact.reaction_count.ready();
		if (seen == event::dropped)
			data.practice.contact.reaction_count.reset();
		practice_dummy_log(fmt::format("{\"type\":\"dummy_contact_%s\",\"boundary\":%u,\"frames\":%u,\"series_hit\":%s,\"counted\":%u,\"due\":%s,\"game_free\":%s,\"rest\":%s%s}\n",
			seen == event::ready ? "recovered" : "dropped", index, data.practice.contact.tracker.waited, json_bool(data.practice.contact.tracker.series_hit),
			data.practice.contact.reaction_count.seen, json_bool(data.practice.contact.ready), json_bool(free_next),
			data.practice.dummy.rest ? json_bool(*data.practice.dummy.rest) : "null", practice_facing_json()));
	}
}

bool pbd::rollback_session::boundary_call::practice_offense_reacts()
{
	if (practice_move_reactions_on())
		return false;
	if (!data.practice.dummy.enabled || data.practice.training_mode != pbd::practice::training_mode::offense || data.practice.offense.movement_off)
		return false;
	if (data.practice.offense.after == pbd::practice::after_contact::movement)
		return !data.practice.movement.input_sequence.empty();
	return data.practice.offense.after == pbd::practice::after_contact::action &&
	       data.practice.offense.reaction_slot < data.practice.dummy.slots.size() &&
	       practice_slot_playable(data.practice.dummy.slots[data.practice.offense.reaction_slot]);
}

bool pbd::rollback_session::boundary_call::practice_offense_movement_on()
{
	return data.practice.dummy.enabled && data.practice.training_mode == pbd::practice::training_mode::offense &&
	       !practice_move_reactions_on() && !data.practice.movement.input_sequence.empty() && !data.practice.offense.movement_off;
}

void pbd::rollback_session::boundary_call::observe_dummy_playback_contact(u32 index, bool hit)
{
	// While playback runs, a hit pauses it and hands the dummy to the game's own Dummy Action until the dummy can act
	// again (movement::movement_pause, practice_dummy_route), so the game gets it up and no dash, roll or held crouch
	// comes out of a combo. A hit while the dummy plays its movement (Defense's gap, Offense's Movement, or the Movement
	// after a contact) also puts the loop back at its first pass, which once the pause is over waits for the dummy at
	// rest and facing P1 like any first pass. Without a rest reading the gap no longer hands over to its action either:
	// the ready gate starts it.
	const bool playing = data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview;
	if (!playing)
		data.practice.movement.pause.reset();
	else if (data.practice.frame_advanced)
	{
		const bool was_paused = data.practice.movement.pause.active;
		data.practice.movement.pause.observe(hit, data.practice.dummy.rest);
		if (!was_paused && data.practice.movement.pause.active)
			practice_dummy_log(fmt::format("{\"type\":\"dummy_game_getup\",\"boundary\":%u,\"cycle\":%u,\"phase\":%u,\"rest\":%s%s}\n",
				index, data.practice.dummy.cycles, data.practice.dummy.phase,
				data.practice.dummy.rest ? json_bool(*data.practice.dummy.rest) : "null", practice_facing_json()));
		if (was_paused && !data.practice.movement.pause.active)
			practice_dummy_log(fmt::format("{\"type\":\"dummy_movement_resume\",\"boundary\":%u,\"cycle\":%u,\"frames\":%u,\"seen_away\":%s,\"rest\":%s%s}\n",
				index, data.practice.dummy.cycles, data.practice.movement.pause.waited, json_bool(data.practice.movement.pause.left),
				data.practice.dummy.rest ? json_bool(*data.practice.dummy.rest) : "null", practice_facing_json()));
	}
	// A hit ends the action it lands in: the rest of the take would come out of the hit as a fragment of it.
	if (hit && playing && data.practice.dummy.phase == dummy_action)
	{
		practice_dummy_log(fmt::format("{\"type\":\"dummy_action_hit\",\"boundary\":%u,\"cycle\":%u,\"slot\":%u,\"position\":%u}\n",
			index, data.practice.dummy.cycles, data.practice.dummy.play_slot + 1, data.practice.dummy.play_pos));
		data.practice.dummy.phase = dummy_recover;
		data.practice.dummy.gate = 0;
		data.practice.dummy.moved = true;
		data.practice.dummy.play_pos = 0;
		data.practice.dummy.play_first = 0;
		data.practice.dummy.flow = {};
		data.practice.dummy.flow_given = 0;
	}
	if (hit && playing && data.practice.dummy.phase == dummy_gap)
	{
		if (data.practice.movement.started)
			practice_dummy_log(fmt::format("{\"type\":\"dummy_movement_hit\",\"boundary\":%u,\"cycle\":%u,\"position\":%u,\"rest\":%s%s}\n",
				index, data.practice.dummy.cycles, data.practice.movement.pos,
				data.practice.dummy.rest ? json_bool(*data.practice.dummy.rest) : "null", practice_facing_json()));
		if (!data.practice.dummy.rest)
			data.practice.dummy.gap_hit = true;
		data.practice.movement.pos = 0;
		data.practice.movement.started = false;
		data.practice.dummy.move_gate = 0;
		data.practice.dummy.turn.reset();
	}
}

bool pbd::rollback_session::boundary_call::choose_reaction_slot()
{
	const u32 slot = data.practice.offense.reaction_slot;
	if (slot >= data.practice.dummy.slots.size() || !practice_slot_playable(data.practice.dummy.slots[slot]))
	{
		data.practice.dummy.have_play_slot = false;
		return false;
	}
	data.practice.dummy.play_slot = slot;
	data.practice.dummy.play_pos = 0;
	data.practice.dummy.have_play_slot = true;
	practice_pick_slot_move(slot);
	return true;
}

std::optional<u16> pbd::rollback_session::boundary_call::dummy_throw_escape(u32 index)
{
	// Throw Escape: while the dummy is held in a throw whose escape window is open, the break that throw asks for, through
	// Controller (practice_dummy_route), ahead of the wake-up control and playback: a thrown dummy reads as knocked down, and
	// a take's own buttons pressed first would decide the break. It drives the dummy for a few frames at most.
	if (data.practice.reactions.throw_escape == pbd::practice::throw_escape_option::none)
		data.practice.reactions.throw_escape_control.reset();
	else
	{
		auto& escape = data.practice.reactions.throw_escape_control;
		const auto held = pbd::practice::native::read_throw_escape();
		if (escape.fresh(held))
		{
			escape.begin(*held, pbd::practice::throw_escape_breaks(data.practice.reactions.throw_escape,
									data.practice.reactions.throw_escape == pbd::practice::throw_escape_option::random ? practice_dummy_random() : 0));
			practice_dummy_log(fmt::format("{\"type\":\"throw_escape\",\"boundary\":%u,\"move\":%u,\"wanted\":%u,\"breaks\":%s}\n",
				index, held->allocation, held->wanted, json_bool(escape.breaks)));
		}
		const bool owned = escape.owns();
		const auto word = escape.next(held);
		if (owned && !escape.owns() && held)
			practice_dummy_log(fmt::format("{\"type\":\"throw_escape_given\",\"boundary\":%u,\"given\":%u,\"presses\":%u}\n",
				index, held->given, escape.presses));
		if (word)
			return *word;
	}

	return std::nullopt;
}

std::optional<u16> pbd::rollback_session::boundary_call::dummy_wakeup_word(u32 index)
{
	if (const auto floor = pbd::practice::native::read_wakeup())
	{
		auto& control = data.practice.reactions.wakeup_control;
		if (floor->on_floor && !control.armed)
			control.arm(data.practice.reactions.wakeup, data.practice.reactions.wakeup == pbd::practice::pbd_wakeup_option::random ? practice_dummy_random() : 0);
		const bool was_active = control.active;
		auto word = control.sample(*floor);
		if (floor->on_floor && !control.armed)
		{
			control.arm(data.practice.reactions.wakeup, data.practice.reactions.wakeup == pbd::practice::pbd_wakeup_option::random ? practice_dummy_random() : 0);
			word = control.sample(*floor);
		}
		if (was_active != control.active)
			practice_dummy_log(fmt::format("{\"type\":\"practice_wakeup\",\"boundary\":%u,\"active\":%s,\"choice\":%u,\"floor_choice\":%u,\"move\":%u,\"position\":%u}\n",
				index, json_bool(control.active), static_cast<u32>(control.choice), static_cast<u32>(control.floor.plan.option),
				floor->allocation, static_cast<u32>(floor->position)));
		if (word)
			return pbd::practice::movement::play_word(*word, practice_dummy_side());
		if (control.armed && control.engaged)
			return 0;
	}
	else
		data.practice.reactions.wakeup_control.reset();

	return std::nullopt;
}
