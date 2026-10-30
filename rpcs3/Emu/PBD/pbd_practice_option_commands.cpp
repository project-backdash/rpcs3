#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_dummy_move_runtime.h"

namespace category = pbd::practice::categories;

bool pbd::rollback_session::practice_steps::choose_offense_reaction(u32 option)
{
	if (std::exchange(data.practice.menu.dropdown_sub, false))
	{
		// Recording's list: the slot. An empty slot is listed dimmed and cannot be chosen.
		if (option >= pbd::practice::reaction_slots || !practice_slot_playable(data.practice.dummy.slots[option]))
			return false;
		data.practice.offense.after = pbd::practice::after_contact::action;
		data.practice.offense.reaction_slot = option;
	}
	else
	{
		// Recording opens its own list (menu_navigation) and never arrives here.
		const auto reaction = pbd::practice::reaction_from_option(option);
		if (!reaction || reaction->kind == pbd::practice::after_contact::action)
			return false;
		if (reaction->kind == pbd::practice::after_contact::moves)
		{
			// The moves come from the in-game move list; the row becomes Moves once some are on.
			practice_open_move_list(pbd::practice::move_list_view::offense_target);
			return false;
		}
		data.practice.offense.after = reaction->kind;
		data.practice.offense.reaction_input = reaction->input;
	}
	// The row's choice is what plays: moves set before give way to it, so they no longer act after every contact
	// whatever Count and After say.
	pbd::practice::dummy_move::current_settings = {};
	practice_movement_refresh();
	return true;
}

bool pbd::rollback_session::practice_steps::choose_training_setting(u32 row, u32 option)
{
	if (data.practice.menu.page == category::practice_settings && row == pbd::practice::practice_rows::mode)
	{
		if (option < pbd::practice::training_mode_count)
			practice_set_training_mode(static_cast<pbd::practice::training_mode>(option));
		return true;
	}
	// Input Delay Frames: a new delay starts its line empty, so the fight's next words wait the new number of frames.
	if (data.practice.menu.page == category::practice_settings && pbd::practice::input_delay_row(data.practice.training_mode, row))
	{
		if (option > pbd::practice::max_input_delay)
			return true;
		data.practice.input_delay_frames = option;
		data.practice.input_delay.clear();
		practice_dummy_log(fmt::format("{\"type\":\"practice_input_delay\",\"boundary\":%u,\"frames\":%u}\n", index, option));
		return true;
	}
	if (const auto game = data.practice.menu.page == category::practice_settings ?
	                          pbd::practice::game_option_row(data.practice.training_mode, row) :
	                          std::nullopt)
	{
		// Attack Data stores 1 or 2 for its two choices; Dummy Action stores the game's action an option names, with Guard All's
		// guard choice kept here; Counter Hit's Random commits Off and sets the option itself before each contact. The other
		// options store the choice itself.
		u32 value = *game == 3 ? option + 1 : option;
		if (*game == 0)
		{
			if (option >= pbd::practice::pbd_dummy_action_choices.size())
				return true;
			value = pbd::practice::pbd_dummy_action_choices[option].action;
			data.practice.reactions.guard.choose(pbd::practice::pbd_dummy_action_choices[option].guard);
		}
		else if (*game == 1)
		{
			if (option > 2)
				return true;
			data.practice.reactions.counter_hit.choose(option == 2);
			value = option == 2 ? 0 : option;
		}
		queue_native(pbd::practice::native::operation::option_commit, value, static_cast<pbd::practice::native::option>(*game));
		return true;
	}
	if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
		row == pbd::practice::practice_rows::wakeup)
	{
		if (option < pbd::practice::pbd_wakeup_option_count)
		{
			data.practice.reactions.wakeup = static_cast<pbd::practice::pbd_wakeup_option>(option);
			data.practice.reactions.wakeup_control.reset();
			data.practice.reactions.wakeup_suspended = false;
		}
		return true;
	}
	if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
		row == pbd::practice::practice_rows::throw_escape)
	{
		if (option < pbd::practice::throw_escape_option_count)
		{
			data.practice.reactions.throw_escape = static_cast<pbd::practice::throw_escape_option>(option);
			data.practice.reactions.throw_escape_control.reset();
		}
		return true;
	}

	return false;
}

bool pbd::rollback_session::practice_steps::choose_offense_movement(u32 row, u32 option)
{
	if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
		(row == pbd::practice::practice_rows::movement || row == pbd::practice::practice_rows::wavedash_speed ||
			row == pbd::practice::practice_rows::after_contact || row == pbd::practice::practice_rows::counted ||
			row == pbd::practice::practice_rows::times))
	{
		if (row == pbd::practice::practice_rows::movement)
		{
			if (option >= pbd::practice::movement::technique_count)
				return true;
			data.practice.offense.movement = static_cast<pbd::practice::movement::preset>(option);
			practice_movement_refresh();
		}
		else if (row == pbd::practice::practice_rows::wavedash_speed)
		{
			const auto speed = pbd::practice::wavedash_speed_from_option(option);
			if (!speed)
				return true;
			data.practice.offense.wavedash_speed = *speed;
			practice_movement_refresh();
		}
		else if (row == pbd::practice::practice_rows::after_contact)
		{
			if (!choose_offense_reaction(option))
				return true;
		}
		else if (row == pbd::practice::practice_rows::counted)
		{
			if (option >= pbd::practice::contact_counted_count)
				return true;
			data.practice.offense.counted = static_cast<pbd::practice::contact_counted>(option);
		}
		else
		{
			if (option > pbd::practice::max_reaction_times - pbd::practice::min_reaction_times)
				return true;
			data.practice.offense.times = pbd::practice::min_reaction_times + option;
		}
		data.practice.offense.movement_off = false;
		if (data.practice.dummy.state == dummy_state::playing)
			practice_dummy_stop_playback(index, "offense_setting");
		data.practice.contact.watching = false;
		data.practice.contact.ready = false;
		data.practice.contact.reaction_count.reset();
		practice_dummy_log(fmt::format("{\"type\":\"dummy_offense_movement\",\"boundary\":%u,\"on\":true,\"movement\":%u,\"after_contact\":%u,\"reaction_slot\":%u,\"reaction_input\":%u,\"counted\":%u,\"times\":%u,\"tape_frames\":%u,\"loop_start\":%u}\n",
			index, static_cast<u32>(data.practice.offense.movement), static_cast<u32>(data.practice.offense.after),
			data.practice.offense.reaction_slot + 1, static_cast<u32>(data.practice.offense.reaction_input), static_cast<u32>(data.practice.offense.counted), data.practice.offense.times,
			static_cast<u32>(data.practice.movement.input_sequence.frames.size()), data.practice.movement.input_sequence.loop_start));
		return true;
	}
	return false;
}

bool pbd::rollback_session::practice_steps::choose_restart_setting(u32 row, u32 option)
{
	if (data.practice.menu.page == category::restart_settings && row == pbd::practice::restart_rows::position)
	{
		if (option < practice_restart_choices.size())
			data.practice.restart.position = practice_restart_choices[option];
		return true;
	}
	// Dummy Direction and Switch Positions: each lists its default first and its change second.
	if (data.practice.menu.page == category::restart_settings &&
		(row == pbd::practice::restart_rows::dummy_direction || row == pbd::practice::restart_rows::switch_positions))
	{
		if (option > 1)
			return true;
		auto& variant = data.practice.restart.variant;
		(row == pbd::practice::restart_rows::dummy_direction ? variant.dummy_back : variant.switched) = option == 1;
		practice_dummy_log(fmt::format("{\"type\":\"practice_restart_variant\",\"boundary\":%u,\"switched\":%s,\"dummy_back\":%s}\n",
			index, json_bool(variant.switched), json_bool(variant.dummy_back)));
		return true;
	}

	return false;
}

bool pbd::rollback_session::practice_steps::choose_display_setting(u32 row, u32 option)
{
	if (data.practice.menu.page == category::display_settings)
	{
		namespace display_rows = pbd::practice::display_rows;
		const bool display = option == 0;
		auto& view = data.practice.view;
		switch (row)
		{
		case display_rows::p1_history: view.command_history[0] = display; break;
		case display_rows::p2_history: view.command_history[1] = display; break;
		case display_rows::attack_info: view.attack_info = display; break;
		case display_rows::frame_data: view.frame_data = display; break;
		case display_rows::control_hints: view.control_hints = display; break;
		case display_rows::movement_streaks: view.movement_streaks = display; break;
		default: return true;
		}
		log_display();
		return true;
	}
	return false;
}

void pbd::rollback_session::practice_steps::choose_defense_setting(u32 row, u32 option)
{
	if (choose_defense_movement(row, option))
		return;
	if (row < 8 && option == 0)
	{
		// Record: back to the fight, ready to record this slot. SELECT+Circle starts the take and START ends it.
		if (!data.practice.native.pause_valid)
			return;
		data.practice.dummy.slot_index = row;
		practice_dummy_arm(index);
		resume_native();
		return;
	}
	if (row < 8 && option == 1)
	{
		// Choose from Move List: the in-game move list opens over the menu for this slot.
		practice_open_move_list(row);
		return;
	}
	if (row < 8)
	{
		if (!clear_dummy_slot(row))
			return;
	}
	else if (row == pbd::practice::dummy_rows::interval)
		data.practice.interval = std::min<u32>(option, 3);
	else if (row == pbd::practice::dummy_rows::trim)
	{
		for (auto& each : data.practice.dummy.slots)
			each.trim_neutral = option == 0;
	}
	std::array<unsigned, 8> weights{};
	for (u32 i = 0; i < 8; ++i)
		if (practice_slot_playable(data.practice.dummy.slots[i]))
			weights[i] = data.practice.dummy.slots[i].frequency;
	const auto pct = pbd::practice::controls::percentages(weights);
	const u32 shown = row < 8 ? row : data.practice.dummy.slot_index;
	practice_dummy_log(fmt::format("{\"type\":\"dummy_settings\",\"slot\":%u,\"enabled\":%s,\"weight\":%u,\"percent\":%u,\"mode\":%u,\"interval\":%u,\"trim\":%s}\n",
		shown + 1, json_bool(data.practice.dummy.slots[shown].enabled), data.practice.dummy.slots[shown].frequency, pct[shown],
		data.practice.play_mode, data.practice.interval, json_bool(data.practice.dummy.slots[0].trim_neutral)));
}

bool pbd::rollback_session::practice_steps::choose_defense_movement(u32 row, u32 option)
{
	if (row == pbd::practice::dummy_rows::save_recordings || row == pbd::practice::dummy_rows::load_recordings)
	{
		recording_set_choose(row == pbd::practice::dummy_rows::load_recordings, option);
		return true;
	}
	if (row == pbd::practice::dummy_rows::movement)
	{
		if (option >= pbd::practice::movement::preset_count)
			return true;
		data.practice.movement.preset = static_cast<pbd::practice::movement::preset>(option);
		practice_movement_refresh();
		practice_dummy_log(fmt::format("{\"type\":\"dummy_movement\",\"boundary\":%u,\"movement\":%u,\"tape_frames\":%u,\"loop_start\":%u}\n",
			index, option, static_cast<u32>(data.practice.movement.input_sequence.frames.size()), data.practice.movement.input_sequence.loop_start));
		// Playback of the movement alone has nothing left to do once Neutral or a one-shot is chosen.
		if (data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview && !practice_movement_alone() && !practice_dummy_available())
			practice_dummy_stop_playback(index, "movement_neutral");
		return true;
	}
	if (row == pbd::practice::dummy_rows::wavedash_speed)
	{
		const auto speed = pbd::practice::wavedash_speed_from_option(option);
		if (!speed)
			return true;
		data.practice.movement.wavedash_speed = *speed;
		practice_movement_refresh();
		practice_dummy_log(fmt::format("{\"type\":\"dummy_wavedash_speed\",\"boundary\":%u,\"speed\":%u,\"tape_frames\":%u}\n", index, *speed,
			static_cast<u32>(data.practice.movement.input_sequence.frames.size())));
		return true;
	}
	if (row == pbd::practice::dummy_rows::movement_end && pbd::practice::end_row_for(data.practice.movement.preset) == pbd::practice::end_row::dash)
	{
		if (option >= pbd::practice::movement::dash_end_count)
			return true;
		data.practice.movement.dash_end = static_cast<pbd::practice::movement::dash_end>(option);
		practice_dummy_log(fmt::format("{\"type\":\"dummy_dash_end\",\"boundary\":%u,\"end\":%u}\n", index, option));
		return true;
	}
	if (row == pbd::practice::dummy_rows::movement_end)
	{
		if (option >= pbd::practice::movement::wavedash_end_count)
			return true;
		data.practice.movement.wavedash_end = static_cast<pbd::practice::movement::wavedash_end>(option);
		practice_dummy_log(fmt::format("{\"type\":\"dummy_wavedash_end\",\"boundary\":%u,\"end\":%u}\n", index, option));
		return true;
	}

	return false;
}

bool pbd::rollback_session::practice_steps::clear_dummy_slot(u32 row)
{
	auto& slot = data.practice.dummy.slots[row];
	if (slot.frames.empty() && slot.moves.empty())
		return false;
	slot.frames.clear();
	slot.moves.clear();
	slot.move_sequence.clear();
	slot.leading_neutral_frames = 0;
	slot.trailing_neutral_frames = 0;
	slot.stop_chord_frames = 0;
	slot.side.reset();
	slot.frequency = 1;
	// A cleared slot that is playing ends its turn; the scheduler then picks among the slots still filled, and
	// a take cut off in the middle (or in the lead-in before it) waits for the dummy to recover.
	if (data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview && data.practice.dummy.have_play_slot && data.practice.dummy.play_slot == row)
	{
		data.practice.dummy.have_play_slot = false;
		if (data.practice.dummy.phase == dummy_action && (data.practice.dummy.play_pos || data.practice.dummy.flow_given))
		{
			data.practice.dummy.phase = dummy_recover;
			data.practice.dummy.gate = 0;
		}
		data.practice.dummy.play_pos = 0;
		data.practice.dummy.play_first = 0;
		data.practice.dummy.flow = {};
		data.practice.dummy.flow_given = 0;
	}
	// With no action left to play, playback ends now instead of finishing the gap's movement first, unless a
	// continuous movement is chosen: that then plays on alone.
	if (data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview && !practice_dummy_available() && !practice_movement_alone())
		practice_dummy_stop_playback(index, "slot_cleared");
	data.practice.view.toast = fmt::format("Dummy Action %u cleared", row + 1);
	data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
	practice_dummy_log(fmt::format("{\"type\":\"dummy_slot_clear\",\"boundary\":%u,\"slot\":%u}\n", index, row + 1));
	return true;
}

void pbd::rollback_session::practice_steps::restore_offense_defaults()
{
	data.practice.reactions.guard.choose(pbd::practice::pbd_guard_option::game);
	data.practice.reactions.counter_hit.choose(false);
	data.practice.offense.movement = pbd::practice::movement::preset::none;
	data.practice.reactions.wakeup = pbd::practice::pbd_wakeup_option::stand_up;
	data.practice.reactions.throw_escape = pbd::practice::throw_escape_option::none;
	data.practice.offense.wavedash_speed = pbd::practice::movement::default_wavedash_speed;
	data.practice.offense.after = pbd::practice::after_contact::none;
	data.practice.offense.reaction_slot = 0;
	data.practice.offense.reaction_input = pbd::practice::movement::reaction_input::backdash;
	pbd::practice::dummy_move::current_settings = {};
	data.practice.offense.counted = pbd::practice::contact_counted::either;
	data.practice.offense.times = pbd::practice::min_reaction_times;
	data.practice.contact.watching = false;
	data.practice.contact.ready = false;
	data.practice.contact.reaction_count.reset();
	data.practice.offense.movement_off = false;
	practice_movement_refresh();
	data.practice.input_delay_frames = 0;
}
