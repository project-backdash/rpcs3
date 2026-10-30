// Practice's menu rows: the choices of each row, choosing one, Restore Defaults and the confirmation dialogs.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_dummy_move_runtime.h"

namespace category = pbd::practice::categories;
#include "../PBD/pbd_practice_choice_options.h"

// The pad and the pointer reach only the options an open list shows, so a Wake-up choice past them could never be chosen.
static_assert(pbd::practice::pbd_wakeup_option_count <= pbd::practice::max_options, "every Wake-up choice fits the option list");
static_assert(pbd::practice::pbd_dummy_action_choices.size() <= pbd::practice::max_options, "every Dummy Action fits the option list");

auto pbd::rollback_session::practice_steps::practice_choices(u32 row) -> practice_choice_list
{
	practice_choice_list out;
	practice_choice_steps{*this, row, out, {}}.run();
	return out;
}

// The option of a row's dropdown that opens a list of its own in place of the dropdown's: Offense's Action After a Hit or
// Block opens the recorded Dummy Actions from its Recording option. Circle there goes back to that option.
std::optional<u32> pbd::rollback_session::practice_steps::practice_sub_list_opener(u32 row)
{
	if (data.practice.menu.page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense &&
		row == pbd::practice::practice_rows::after_contact)
		return pbd::practice::reaction_recording_option;
	return std::nullopt;
}

// Opens the move list over the menu for a Defense slot (0..7) or Offense's Action After a Hit or Block, with the moves
// already chosen for it on.
void pbd::rollback_session::practice_steps::practice_open_move_list(u32 target)
{
	namespace view = pbd::practice::move_list_view;
	auto rows = pbd::practice::dummy_move::list_rows();
	if (rows.empty())
	{
		queue_sound(pbd::practice::native::sound::cancel);
		data.practice.view.toast = "The move list is available once both fighters have loaded";
		data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
		return;
	}
	std::vector<u32> chosen;
	if (target == view::offense_target)
	{
		const auto& settings = pbd::practice::dummy_move::current_settings;
		for (const auto* reaction : {&settings.after_guard, &settings.after_hit})
			if (reaction->kind == pbd::practice::dummy_move::source::moves)
				chosen.insert(chosen.end(), reaction->ids.begin(), reaction->ids.end());
	}
	else if (target < data.practice.dummy.slots.size())
		chosen = data.practice.dummy.slots[target].moves;
	else
		return;
	data.practice.menu.move_list.open_on(target, std::move(rows), std::move(chosen));
	practice_dummy_log(fmt::format("{\"type\":\"practice_move_list_open\",\"boundary\":%u,\"target\":%u,\"rows\":%u,\"on\":%u}\n", index, target,
		static_cast<u32>(data.practice.menu.move_list.rows.size()), static_cast<u32>(data.practice.menu.move_list.chosen.size())));
}

// Circle closes the move list with the moves that are on: they become the slot's action or the reaction. With none on, a
// slot plays its recording again (or is empty) and the reaction is None.
void pbd::rollback_session::practice_steps::practice_close_move_list()
{
	namespace view = pbd::practice::move_list_view;
	auto& list = data.practice.menu.move_list;
	list.open = false;
	const auto chosen = list.chosen;
	const auto name = list.chosen_name();
	const std::string what = chosen.empty() ? std::string("No moves") : chosen.size() == 1 ? pbd::practice::menu::shortened(name, 40) :
	                                                                                         fmt::format("%u moves, one at random", static_cast<u32>(chosen.size()));
	if (list.target == view::offense_target)
	{
		using pbd::practice::dummy_move::source;
		if (!chosen.empty())
		{
			pbd::practice::dummy_move::current_settings = {{source::moves, 0, chosen}, {source::moves, 0, chosen}};
			data.practice.offense.after = pbd::practice::after_contact::moves;
		}
		else if (data.practice.offense.after == pbd::practice::after_contact::moves)
		{
			pbd::practice::dummy_move::current_settings = {};
			data.practice.offense.after = pbd::practice::after_contact::none;
		}
		data.practice.offense.movement_off = false;
		if (data.practice.dummy.state == dummy_state::playing)
			practice_dummy_stop_playback(index, "offense_setting");
		data.practice.contact.watching = false;
		data.practice.contact.ready = false;
		data.practice.contact.reaction_count.reset();
		practice_movement_refresh();
		data.practice.view.toast = "Action After a Hit or Block\t" + what;
	}
	else if (list.target < data.practice.dummy.slots.size())
	{
		auto& slot = data.practice.dummy.slots[list.target];
		slot.moves = chosen;
		slot.enabled = true;
		slot.move_sequence.clear();
		practice_pick_slot_move(list.target);
		data.practice.view.toast = practice_slot_name(list.target) + "\t" + what;
	}
	data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
	practice_dummy_log(fmt::format("{\"type\":\"practice_move_list_close\",\"boundary\":%u,\"target\":%u,\"on\":%u}\n", index, list.target,
		static_cast<u32>(chosen.size())));
	list.rows.clear();
}

// The move list's controls (pbd_practice_move_list.h): Up/Down a move, Left/Right a page, L1/R1 a category, Cross a move
// on or off, Circle or START closes it with the moves that are on.
void pbd::rollback_session::practice_steps::practice_move_list_input(u16 pressed, bool select)
{
	auto& list = data.practice.menu.move_list;
	if (pressed & 0x5000)
	{
		list.step((pressed & 0x4000) != 0);
		queue_sound(pbd::practice::native::sound::cursor);
	}
	else if (pressed & 0xa000)
	{
		list.page((pressed & 0x2000) != 0);
		queue_sound(pbd::practice::native::sound::cursor);
	}
	else if (pressed & 0x000c)
	{
		list.category((pressed & 0x0008) != 0);
		queue_sound(pbd::practice::native::sound::cursor);
	}
	else if (pressed & 0x0040)
		queue_sound(list.toggle() ? pbd::practice::native::sound::confirm : pbd::practice::native::sound::cancel);
	else if ((pressed & 0x0020) || (!select && (pressed & session_start_bit)))
	{
		queue_sound(pbd::practice::native::sound::confirm);
		practice_close_move_list();
	}
}

void pbd::rollback_session::practice_steps::log_display()
{
	const auto& view = data.practice.view;
	practice_dummy_log(fmt::format("{\"type\":\"practice_display\",\"history\":%s,\"history_p1\":%s,\"history_p2\":%s,\"attack_info\":%s,\"frame_data\":%s,\"hints\":%s,\"streaks\":%s}\n",
		json_bool(view.command_history[0] || view.command_history[1]), json_bool(view.command_history[0]), json_bool(view.command_history[1]),
		json_bool(view.attack_info), json_bool(view.frame_data), json_bool(view.control_hints), json_bool(view.movement_streaks)));
}

// Stop playback before switching the dummy to the chosen mode's movement or stock Dummy Action.
// Defense leaves it idle until SELECT+Circle starts playback.
void pbd::rollback_session::practice_steps::practice_set_training_mode(pbd::practice::training_mode mode)
{
	if (mode == pbd::practice::training_mode::cpu || mode == data.practice.training_mode)
		return;
	const bool was_playing = data.practice.dummy.state == dummy_state::playing;
	if (was_playing)
		practice_dummy_stop_playback(index, "training_mode");
	data.practice.training_mode = mode;
	data.practice.offense.movement_off = false;
	practice_movement_refresh();
	// Changing modes changes row meanings; invalidate queued pointer actions.
	++data.practice.menu.mouse_revision;
	practice_dummy_log(fmt::format("{\"type\":\"practice_training_mode\",\"boundary\":%u,\"mode\":%u,\"stopped_playback\":%s}\n",
		index, static_cast<u32>(mode), json_bool(was_playing)));
}

void pbd::rollback_session::practice_steps::practice_choose(u32 row, u32 option)
{
	if (choose_training_setting(row, option))
		return;
	// Offense's Movement and Action After a Hit or Block. A change takes effect when the fight resumes: a movement
	// that is playing stops here, and the controls start the new choice in the fight (always, or after the next
	// contact). Choosing any of these rows turns the Offense movement back on if SELECT+Circle had turned it off, and
	// starts the contact watch and its count again.
	if (choose_offense_movement(row, option))
		return;
	if (choose_restart_setting(row, option))
		return;
	if (choose_display_setting(row, option))
		return;
	// The rest are Defense rows; from here `row` is the dummy_rows value.
	const auto defense = practice_defense_row(row);
	if (!defense)
		return;
	choose_defense_setting(*defense, option);
}

// The dummy neither records nor plays, or plays only Offense's own Movement, which is a setting rather than a
// session and stops for Offense's Restore Defaults.
bool pbd::rollback_session::practice_steps::practice_dummy_settled()
{
	return data.practice.dummy.state == dummy_state::idle ||
	       (data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview &&
			   data.practice.training_mode == pbd::practice::training_mode::offense);
}

void pbd::rollback_session::practice_steps::practice_dummy_settle(const char* reason)
{
	data.practice.reactions.wakeup_control.reset();
	data.practice.reactions.wakeup_suspended = false;
	data.practice.reactions.throw_escape_control.reset();
	if (data.practice.dummy.state == dummy_state::playing && practice_dummy_settled())
		practice_dummy_stop_playback(index, reason);
}

// Character and Stage Selection's gate (pbd::practice::selection_gate_for). A take awaiting its decision (state 2)
// and any state not named here count as recording, so they refuse.
pbd::practice::selection_gate pbd::rollback_session::practice_steps::practice_selection_gate()
{
	using pbd::practice::dummy_phase;
	const auto phase = data.practice.dummy.state == dummy_state::idle    ? dummy_phase::idle :
	                   data.practice.dummy.state == dummy_state::ready   ? dummy_phase::ready :
	                   data.practice.dummy.state == dummy_state::playing ? (data.practice.dummy.preview ? dummy_phase::preview : dummy_phase::playing) :
	                                                                       dummy_phase::recording;
	return pbd::practice::selection_gate_for(phase);
}

// Restore Defaults for the category in view (in Practice Settings, the rows of the training mode in view; the mode
// itself stays). Recordings and a remembered position are kept: they are the player's
// work, not settings. The game's options are written back in one native commit, which
// plays its own confirm sound and toast once the game accepts it.
void pbd::rollback_session::practice_steps::restore_defaults()
{
	const u32 page = data.practice.menu.page;
	bool native_pending = false;
	if (page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::offense)
	{
		// Offense restores the game's options and its own Movement rows. The game's options stay locked while the
		// dummy records or plays, as their rows are; the Offense movement stops for the restore instead.
		practice_dummy_settle("restore_defaults");
		const auto current = data.practice.dummy.state == dummy_state::idle ? practice_game_values() : std::nullopt;
		if (!data.practice.menu.game_defaults || !current)
		{
			queue_sound(pbd::practice::native::sound::cancel);
			data.practice.view.toast = "The game's practice options can't be read right now";
			data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
			return;
		}
		// Counter Hit's Random reads as Off but may have left On in the game, so it always takes the restore.
		if (*current != *data.practice.menu.game_defaults || data.practice.reactions.counter_hit.random)
		{
			queue_native(pbd::practice::native::operation::option_restore, pbd::practice::pack_game_options(*data.practice.menu.game_defaults));
			native_pending = true;
		}
		restore_offense_defaults();
	}
	else if (page == category::restart_settings)
	{
		data.practice.restart.position = pbd::game_lifecycle::placement::centre;
		data.practice.restart.variant = {};
	}
	else if (page == category::practice_settings && data.practice.training_mode == pbd::practice::training_mode::defense)
	{
		data.practice.interval = 3;
		for (auto& slot : data.practice.dummy.slots)
		{
			slot.trim_neutral = true;
			slot.enabled = true;
			slot.frequency = 1;
		}
		data.practice.movement.preset = pbd::practice::movement::preset::none;
		data.practice.movement.wavedash_speed = pbd::practice::movement::default_wavedash_speed;
		data.practice.movement.wavedash_end = pbd::practice::movement::wavedash_end::crouch_dash;
		data.practice.movement.dash_end = pbd::practice::movement::dash_end::into_move;
		practice_movement_refresh();
		data.practice.input_delay_frames = 0;
		if (data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview)
			practice_dummy_stop_playback(index, "restore_defaults");
	}
	else if (page == category::display_settings)
	{
		auto& view = data.practice.view;
		view.command_history = {true, true};
		view.attack_info = true;
		view.frame_data = true;
		view.control_hints = true;
		view.movement_streaks = true;
		log_display();
	}
	else
		return;
	practice_dummy_log(fmt::format("{\"type\":\"practice_restore_defaults\",\"boundary\":%u,\"page\":%u,\"native\":%s}\n",
		index, page, json_bool(native_pending)));
	if (native_pending)
		return;
	queue_sound(pbd::practice::native::sound::confirm);
	data.practice.view.toast = "Defaults restored";
	data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
}

// Whether the row as the player sees it (the published panel of this menu visit) is available.
bool pbd::rollback_session::practice_steps::shown_row_enabled(u32 row)
{
	const auto shown = pbd::game_overlay::load();
	const u32 base = data.practice.view.mouse_row_base;
	return shown.generation == data.practice.menu.generation && row >= base && row - base < shown.row_count && shown.rows[row - base].enabled;
}

// Questions over the list open with No highlighted; Yes acts and No or Circle only close the dialog.
void pbd::rollback_session::practice_steps::open_confirm(pbd::practice::confirm_kind kind)
{
	queue_sound(pbd::practice::native::sound::confirm);
	data.practice.menu.confirm = kind;
	data.practice.menu.confirm_choice = pbd::practice::answer_no;
	practice_dummy_log(fmt::format("{\"type\":\"practice_confirm_open\",\"boundary\":%u,\"page\":%u,\"kind\":%u}\n",
		index, data.practice.menu.page, static_cast<u32>(kind)));
}

void pbd::rollback_session::practice_steps::queue_selection(bool characters)
{
	const auto gate = practice_selection_gate();
	if (gate == pbd::practice::selection_gate::refuse)
	{
		queue_sound(pbd::practice::native::sound::cancel);
		return;
	}
	const auto stage = pbd::practice::stage::resolve(data.practice.menu.row,
		!characters && data.practice.menu.row == pbd::practice::stage::random_row ? host_random_word() : 0);
	if (!characters && !stage)
	{
		queue_sound(pbd::practice::native::sound::cancel);
		return;
	}
	if (gate == pbd::practice::selection_gate::stop_then_go)
		practice_dummy_stop_playback(index, "selection");
	// The game's call clears its practice data: Practice's memory saves what is shown now, and reads the game's options again
	// only once the next fight has its saved ones back.
	if (auto* memory = session_runtime.m_practice_memory.get())
	{
		memory_save();
		memory->applied_match = ~0u;
	}
	queue_sound(pbd::practice::native::sound::confirm);
	queue_native(characters ? pbd::practice::native::operation::character_select :
							  pbd::practice::native::operation::stage_load,
		characters ? 0 : *stage);
	practice_dummy_log(fmt::format("{\"type\":\"practice_selection_queued\",\"boundary\":%u,\"characters\":%s,\"stage\":%u}\n",
		index, json_bool(characters), characters ? 0 : *stage));
}

void pbd::rollback_session::practice_steps::answer_confirm(bool yes)
{
	const auto kind = data.practice.menu.confirm;
	data.practice.menu.confirm = pbd::practice::confirm_kind::none;
	practice_dummy_log(fmt::format("{\"type\":\"practice_confirm_answer\",\"boundary\":%u,\"page\":%u,\"kind\":%u,\"yes\":%s}\n",
		index, data.practice.menu.page, static_cast<u32>(kind), json_bool(yes)));
	if (!yes)
	{
		if (kind == pbd::practice::confirm_kind::load_recordings && session_runtime.m_practice_memory)
			session_runtime.m_practice_memory->load_set_requested.reset();
		if (kind == pbd::practice::confirm_kind::save_recordings && session_runtime.m_practice_memory && session_runtime.m_practice_memory->set_editor)
		{
			data.practice.menu.dropdown_open = true;
			data.practice.menu.dropdown_focus = 0;
		}
		queue_sound(pbd::practice::native::sound::cancel);
		return;
	}
	using pbd::practice::confirm_kind;
	switch (kind)
	{
	case confirm_kind::restore_defaults: restore_defaults(); break;
	case confirm_kind::character_selection:
	case confirm_kind::stage_selection:
		queue_selection(kind == confirm_kind::character_selection);
		break;
	case confirm_kind::exit_practice: leave(); break;
	case confirm_kind::load_recordings: recording_set_apply(); break;
	case confirm_kind::save_recordings: recording_set_save(); break;
	case confirm_kind::none: break;
	}
}
