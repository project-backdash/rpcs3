#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_dummy_move_runtime.h"

namespace category = pbd::practice::categories;

void pbd::rollback_session::practice_steps::mouse_option(const pbd::practice::mouse::command& mouse, u16& pressed, bool list_menu, bool& accepted)
{
	// A question's Yes / No are its options, as a dropdown's are.
	if (list_menu && data.practice.menu.confirm != pbd::practice::confirm_kind::none && mouse.value < pbd::practice::answer_count)
	{
		if (data.practice.menu.confirm_choice != mouse.value)
		{
			data.practice.menu.confirm_choice = mouse.value;
			queue_sound(pbd::practice::native::sound::cursor);
		}
		if (mouse.kind == pbd::practice::mouse::action::choose_option)
			pressed |= 0x0040;
		accepted = true;
	}
	else if (list_menu && data.practice.menu.dropdown_open && mouse.value < practice_choices(data.practice.menu.row).options.size())
	{
		if (data.practice.menu.dropdown_focus != mouse.value)
		{
			data.practice.menu.dropdown_focus = mouse.value;
			queue_sound(pbd::practice::native::sound::cursor);
		}
		if (mouse.kind == pbd::practice::mouse::action::choose_option)
			pressed |= 0x0040;
		accepted = true;
	}
}

void pbd::rollback_session::practice_steps::mouse_prompt(const pbd::practice::mouse::command& mouse, u16& pressed, bool list_menu, bool& accepted)
{
	// A control-bar click presses that group's button. Focus can move between rail and list without a
	// new pointer scope, and Circle means Back in one and Resume in the other, so the group must still
	// be on the bar the player is looking at.
	using pbd::practice::prompt_action;
	const auto action = static_cast<prompt_action>(mouse.value & 0xff);
	const bool second = (mouse.value & pbd::practice::prompt_second_key) != 0;
	if (data.practice.view.menu_prompts.offers(action))
	{
		accepted = true;
		if (action == prompt_action::confirm)
			pressed |= 0x0040;
		else if (action == prompt_action::back)
			pressed |= 0x0020;
		else if (action == prompt_action::resume)
			pressed |= session_start_bit;
		else if (action == prompt_action::frequency && list_menu && !data.practice.menu.dropdown_open && practice_frequency_row())
			pressed |= second ? 0x0008 : 0x0004;
		else
			accepted = false;
	}
}

void pbd::rollback_session::practice_steps::mouse_row(const pbd::practice::mouse::command& mouse, u16& pressed, bool list_menu, bool& accepted)
{
	const auto panel = pbd::game_overlay::load();
	const u32 base = data.practice.view.mouse_row_base;
	const bool visible_row = panel.visible && panel.generation == data.practice.menu.generation &&
	                         mouse.value >= base && mouse.value - base < panel.row_count;
	// While the rail has focus, hovering the list leaves it alone; a click on a row moves focus into the list.
	const bool rail_hover = list_menu && data.practice.menu.rail_focus && mouse.kind == pbd::practice::mouse::action::hover_row;
	// An open question owns the pointer; the list behind it does not answer.
	const bool behind_question = list_menu && data.practice.menu.confirm != pbd::practice::confirm_kind::none;
	if (visible_row && !rail_hover && !behind_question)
	{
		const bool entering = list_menu && data.practice.menu.rail_focus;
		if (entering)
			data.practice.menu.rail_focus = false;
		u32& selected = data.practice.dummy.state == dummy_state::confirm ? data.practice.dummy.choice :
		                                                                    data.practice.menu.row;
		if (selected != mouse.value || entering)
		{
			selected = mouse.value;
			queue_sound(pbd::practice::native::sound::cursor);
		}
		accepted = mouse.kind == pbd::practice::mouse::action::hover_row || panel.rows[mouse.value - base].enabled;
		if (mouse.kind == pbd::practice::mouse::action::choose_row && accepted)
			pressed |= 0x0040;
	}
}

bool pbd::rollback_session::practice_steps::navigate_dropdown(u16 pressed)
{
	if (data.practice.menu.dropdown_open)
	{
		if (recording_set_name_input(pressed))
			return true;
		const auto list = practice_choices(data.practice.menu.row);
		const u32 count = static_cast<u32>(std::min(list.options.size(), pbd::practice::max_options));
		// The row can stop offering choices while the list is open (a native value became unreadable).
		if (!count)
		{
			data.practice.menu.dropdown_open = false;
			return true;
		}
		if (pressed & 0x5000)
		{
			data.practice.menu.dropdown_focus = pbd::practice::wrap_step(data.practice.menu.dropdown_focus, count, (pressed & 0x4000) != 0);
			queue_sound(pbd::practice::native::sound::cursor);
		}
		else if (pressed & 0x0040)
		{
			// An unavailable option stays highlighted and the list stays open.
			const u32 option = std::min(data.practice.menu.dropdown_focus, count - 1);
			if (!list.options[option].enabled)
				return true;
			// An option that opens a list of its own (Recording) shows that list in place of this one.
			if (const auto opener = practice_sub_list_opener(data.practice.menu.row); opener && *opener == option && !data.practice.menu.dropdown_sub)
			{
				queue_sound(pbd::practice::native::sound::confirm);
				data.practice.menu.dropdown_sub = true;
				const auto sub = practice_choices(data.practice.menu.row);
				data.practice.menu.dropdown_focus = sub.options.empty() ? 0 : std::min<u32>(sub.current, static_cast<u32>(sub.options.size() - 1));
				return true;
			}
			// A game option plays its confirm sound once the game accepts the commit; queuing one here too
			// would play it twice.
			if (!(data.practice.menu.page == category::practice_settings &&
					pbd::practice::game_option_row(data.practice.training_mode, data.practice.menu.row)))
				queue_sound(pbd::practice::native::sound::confirm);
			data.practice.menu.dropdown_open = false;
			practice_choose(data.practice.menu.row, option);
		}
		else if (pressed & 0x0020)
		{
			queue_sound(pbd::practice::native::sound::cancel);
			// Circle in a list of its own goes back to the first list, on the option that opened it.
			if (const auto opener = practice_sub_list_opener(data.practice.menu.row); opener && data.practice.menu.dropdown_sub)
			{
				data.practice.menu.dropdown_sub = false;
				data.practice.menu.dropdown_focus = *opener;
			}
			else
				data.practice.menu.dropdown_open = false;
		}
		return true;
	}
	return false;
}

bool pbd::rollback_session::practice_steps::navigate_categories(u16 pressed)
{
	if (data.practice.menu.rail_focus)
	{
		// Category rail: Up/Down show another category's list, Right or Cross move into it, Circle resumes.

		pbd::practice::column_focus focus{true, data.practice.menu.page, data.practice.menu.row};
		switch (pbd::practice::rail_step(focus, pressed, category::count))
		{
		case pbd::practice::rail_action::resume:
			queue_sound(pbd::practice::native::sound::cancel);
			resume_native();
			break;
		case pbd::practice::rail_action::moved:
			queue_sound(pbd::practice::native::sound::cursor);
			data.practice.menu.page = focus.page;
			data.practice.menu.row = focus.row;
			practice_dummy_log(fmt::format("{\"type\":\"practice_menu_open\",\"boundary\":%u,\"page\":%u,\"pause\":%u}\n", index, data.practice.menu.page, data.practice.native.paused ? 1u : 0u));
			break;
		case pbd::practice::rail_action::entered:
			queue_sound(pbd::practice::native::sound::confirm);
			data.practice.menu.rail_focus = false;
			data.practice.menu.row = 0;
			break;
		case pbd::practice::rail_action::none:
			break;
		}
		return true;
	}
	return false;
}

void pbd::rollback_session::practice_steps::activate_practice_row()
{
	const u32 row = data.practice.menu.row;
	data.practice.menu.dropdown_sub = false;
	const auto list = practice_choices(row);
	if (!list.options.empty())
	{
		// Value rows open their dropdown on the option in effect; the shown row must be available.
		if (shown_row_enabled(row))
		{
			queue_sound(pbd::practice::native::sound::confirm);
			data.practice.menu.dropdown_open = true;
			data.practice.menu.dropdown_focus = std::min<u32>(list.current, static_cast<u32>(list.options.size() - 1));
		}
	}
	// Restore Defaults is the last row of every settings category and asks first (Practice Settings'
	// Training Mode row is never it, even in a mode with no rows of its own).
	else if (data.practice.menu.page <= category::display_settings && row + 1 == practice_row_count() &&
			 !(data.practice.menu.page == category::practice_settings && row == pbd::practice::practice_rows::mode))
	{
		if (shown_row_enabled(row))
			open_confirm(pbd::practice::confirm_kind::restore_defaults);
	}
	else if (data.practice.menu.page == category::restart_settings)
	{
		namespace restart_rows = pbd::practice::restart_rows;
		if (row == restart_rows::restart)
			queue_reset(data.practice.restart.position, data.practice.restart.variant);
		else if (row == restart_rows::remember && pbd::game_lifecycle::requested() && practice_can_change_situation())
		{
			queue_sound(pbd::practice::native::sound::confirm);
			remember_position();
		}
	}
	// Selection stops playback before loading; a recording in progress stays in its current arena.
	else if (data.practice.menu.page == category::character_selection || data.practice.menu.page == category::stage_selection)
	{
		if (practice_selection_gate() != pbd::practice::selection_gate::refuse)
			open_confirm(data.practice.menu.page == category::character_selection ? pbd::practice::confirm_kind::character_selection :
																					pbd::practice::confirm_kind::stage_selection);
		else
		{
			queue_sound(pbd::practice::native::sound::cancel);
			practice_dummy_log(fmt::format("{\"type\":\"practice_row_refused\",\"boundary\":%u,\"page\":%u,\"state\":%u,\"preview\":%s,\"at\":\"row\"}\n",
				index, data.practice.menu.page, static_cast<u32>(data.practice.dummy.state),
				json_bool(data.practice.dummy.preview)));
		}
	}
	else if (data.practice.menu.page == category::move_list && shown_row_enabled(row))
	{
		if (practice_selection_gate() == pbd::practice::selection_gate::stop_then_go)
			practice_dummy_stop_playback(index, "command_list");
		practice_restart_contact();
		queue_sound(pbd::practice::native::sound::confirm);
		queue_native(pbd::practice::native::operation::command_list);
	}
	else if (data.practice.menu.page == category::exit_practice)
		open_confirm(pbd::practice::confirm_kind::exit_practice);
	// Settings opens over this menu; the window's GUI thread picks the request up within a few frames.
	else if (data.practice.menu.page == category::settings)
	{
		queue_sound(pbd::practice::native::sound::confirm);
		pbd::settings_menu::request_open();
		practice_dummy_log(fmt::format("{\"type\":\"practice_settings_open\",\"boundary\":%u}\n", index));
	}
}
