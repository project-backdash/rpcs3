// Practice menu rows and their published state.
#include "stdafx.h"
#include "pbd_practice_menu_builder.h"

namespace category = pbd::practice::categories;
namespace practice_rows = pbd::practice::practice_rows;
using pbd::practice::prompt_action;
using pbd::practice::training_mode;

void pbd::rollback_session::practice_steps::native_menu::row(std::string label, std::string value, bool enabled, std::string help, bool is_default)
{
	pbd::game_overlay::row next;
	next.enabled = enabled;
	next.label = std::move(label);
	next.detail = std::move(value);
	next.help = std::move(help);
	next.is_default = is_default;
	rows.push_back(std::move(next));
}

void pbd::rollback_session::practice_steps::native_menu::choice_row(u32 index_in_page, std::string label, bool enabled, std::string help)
{
	const auto list = practice_choices(index_in_page);
	const bool known = list.current < list.options.size();
	row(std::move(label), known ? list.options[list.current].text : std::string("Unavailable"), enabled && known, std::move(help),
		known && list.options[list.current].is_default);
}

void pbd::rollback_session::practice_steps::native_menu::restore_row(bool enabled, std::string help)
{
	row("Restore Defaults", "", enabled, std::move(help));
}

void pbd::rollback_session::practice_steps::native_menu::begin()
{
	// Restore Defaults needs the game's starting options; the first menu opens before any can be changed here.
	if (!data.practice.menu.game_defaults)
		data.practice.menu.game_defaults = practice_game_values();
	panel.generation = data.practice.menu.generation;
	panel.visible = true;
	panel.close_on_action = false;
	panel.practice = true;
	panel.title = "PRACTICE";
	panel.subtitle = "Practice";
	panel.confirm = "Choose / change";
	panel.back = "Resume";
}

void pbd::rollback_session::practice_steps::native_menu::confirm_dialog()
{
	auto& bar = data.practice.view.menu_prompts;
	if (asking)
	{
		// A question over the list: Up/Down pick Yes or No, Cross answers and Circle closes it unanswered.
		const auto text = pbd::practice::confirm_message(data.practice.menu.confirm, page, data.practice.training_mode,
			pbd::practice::selection_names_stop(data.practice.training_mode, practice_selection_gate()));
		auto& dialog = data.practice.view.confirm;
		dialog.open = true;
		dialog.message = text.message;
		dialog.detail = text.detail;
		dialog.focus = std::min<u32>(data.practice.menu.confirm_choice, pbd::practice::answer_count - 1);
		bar.add("{cross}", "Confirm", prompt_action::confirm);
		bar.add("{circle}", "Back", prompt_action::back);
		bar.add("{dpad:ud}", "Move");
	}
	if (const auto* memory = session_runtime.m_practice_memory.get(); memory && memory->set_editor)
	{
		const auto& editor = *memory->set_editor;
		data.practice.view.header = "Recording set name: " + editor.text + "_";
		if (asking && data.practice.menu.confirm == pbd::practice::confirm_kind::save_recordings)
			data.practice.view.confirm.detail = "Saved set: " + pbd::practice::menu::shortened(
																	practice_store::set_name(memory->sets.find(editor.character, editor.position), editor.position), 24);
	}
}

void pbd::rollback_session::practice_steps::native_menu::focused_rows()
{
	auto& bar = data.practice.view.menu_prompts;
	if (list_page && !asking && data.practice.menu.rail_focus && !data.practice.menu.dropdown_open)
	{
		// Rail focus: Up/Down pick a category, Right or Cross move into its list.
		bar.add("{cross}", "Confirm", prompt_action::confirm);
		bar.add("{circle}/{start}", "Resume", prompt_action::resume);
		bar.add("{dpad:udr}", "Move");
	}
	if (!rows.empty())
	{
		panel.selected = std::min(panel.selected, static_cast<u32>(rows.size() - 1));
		const auto& focused = rows[panel.selected];
		const auto list = list_page ? practice_choices(data.practice.menu.row) : practice_choice_list{};
		if (!asking && list_page && data.practice.menu.dropdown_open && !list.options.empty())
		{
			// Dropdown open: only the options move; L1/R1 and START wait until it closes.
			auto& dropdown = data.practice.view.menu_dropdown;
			dropdown.open = true;
			dropdown.count = static_cast<u32>(std::min(list.options.size(), pbd::practice::max_options));
			dropdown.current = std::min(list.current, dropdown.count - 1);
			dropdown.focus = std::min(data.practice.menu.dropdown_focus, dropdown.count - 1);
			for (u32 i = 0; i < dropdown.count; ++i)
				dropdown.options[i] = list.options[i];
			bar.add("{cross}", "Confirm", prompt_action::confirm);
			bar.add("{circle}", "Cancel", prompt_action::back);
			bar.add("{dpad:ud}", "Move");
		}
		else if (!asking && list_page && !data.practice.menu.rail_focus)
		{
			// List focus: Circle or Left go back to the rail.
			if (focused.enabled)
				bar.add("{cross}", "Confirm", prompt_action::confirm);
			bar.add("{circle}", "Back", prompt_action::back);
			bar.add("{start}", "Resume", prompt_action::resume);
			if (practice_frequency_row())
				bar.add("{l1}/{r1}", "Frequency", prompt_action::frequency);
			bar.add("{dpad:udl}", "Move");
		}
		// A longer page keeps its selected row visible; mouse rows retain their index in the full page.
		const auto visible = pbd::practice::pbd_visible_rows(static_cast<u32>(rows.size()), panel.selected, pbd::game_overlay::max_rows);
		data.practice.view.mouse_row_base = visible.first;
		panel.row_count = visible.count;
		panel.selected = visible.selected;
		data.practice.view.menu_dropdown.row = panel.selected;
		for (u32 i = 0; i < panel.row_count; ++i)
			panel.rows[i] = std::move(rows[visible.first + i]);
	}
}

void pbd::rollback_session::practice_steps::native_menu::move_list()
{
	auto& bar = data.practice.view.menu_prompts;
	// The move list over the menu: the rows in view, the highlighted move's help and the list's own control bar.
	auto& moves_view = data.practice.view.move_list;
	moves_view = {};
	if (data.practice.menu.move_list.open && !decision)
	{
		namespace view = pbd::practice::move_list_view;
		const auto& list = data.practice.menu.move_list;
		moves_view.open = true;
		moves_view.title = list.target == view::offense_target ? std::string("Action After a Hit or Block") : practice_slot_name(list.target);
		moves_view.detail = list.chosen.empty()     ? std::string("No moves on") :
		                    list.chosen.size() == 1 ? std::string("1 move on") :
		                                              fmt::format("%u moves on \xC2\xB7 one at random", static_cast<u32>(list.chosen.size()));
		u32 position = 0, total = 0;
		for (u32 at = 0; at < list.rows.size(); ++at)
			if (!list.rows[at].header)
			{
				++total;
				if (at <= list.focus)
					position = total;
			}
		moves_view.position = fmt::format("%u / %u", position, total);
		for (u32 at = list.first; at < list.rows.size() && moves_view.count < moves_view.lines.size(); ++at)
		{
			const auto& each = list.rows[at];
			std::string name = each.name, used;
			u32 uses = 0;
			if (!each.header && list.single_choice())
				for (u32 slot = 0; slot < data.practice.dummy.slots.size(); ++slot)
				{
					const auto& moves = data.practice.dummy.slots[slot].moves;
					if (slot != list.target && std::find(moves.begin(), moves.end(), each.id) != moves.end())
					{
						used += (uses++ ? ", " : "") + std::to_string(slot + 1);
					}
				}
			if (uses)
				name = std::string(uses == 1 ? "In slot " : "In slots ") + used + ": " + name;
			moves_view.lines[moves_view.count++] = {name, each.command, each.header, each.selectable, !each.header && list.on(each.id), at == list.focus,
				each.glyphs};
		}
		moves_view.more_above = list.first > 0;
		moves_view.more_below = list.first + moves_view.count < list.rows.size();
		const bool focused = list.focusable(list.focus);
		const auto& row_in_focus = focused ? list.rows[list.focus] : view::row{};
		moves_view.help = !focused                 ? std::string() :
		                  !row_in_focus.selectable ? std::string("The dummy can't do this move from where it stands, so it can't be chosen.") :
		                  list.on(row_in_focus.id) ? std::string("{cross} turns it off. {circle} keeps the moves that are on.") :
		                  list.single_choice()     ? std::string("{cross} chooses this move, replacing the slot's previous move. {circle} keeps it.") :
		                                             std::string("{cross} turns it on. With several on, the dummy picks one at random each time.");
		bar = {};
		bar.add("{cross}", "On/Off", prompt_action::confirm);
		bar.add("{circle}", "Done", prompt_action::back);
		bar.add("{dpad:ud}", "Move");
		bar.add("{dpad:l}/{dpad:r}", "Page");
		bar.add("{l1}/{r1}", "Category");
	}
}

void pbd::rollback_session::practice_steps::native_menu::build()
{
	begin();
	if (data.practice.dummy.state == dummy_state::confirm)
		decision_rows();
	else if (page == category::practice_settings)
		training_rows();
	else if (page == category::restart_settings)
		restart_rows();
	else if (page == category::display_settings)
		display_rows();
	else
		selection_rows();
	if (data.practice.dummy.state == dummy_state::recording && data.practice.view.header.empty())
		data.practice.view.header = fmt::format("Recording %s \xC2\xB7 %.1f s", practice_record_name(), timer_frames / 60.0);
	// Menu toasts replace the header text while they last.
	if (!data.practice.view.toast.empty())
		data.practice.view.header = data.practice.view.toast;
	list_page = !decision;
	asking = list_page && data.practice.menu.confirm != pbd::practice::confirm_kind::none;
	confirm_dialog();
	focused_rows();
	move_list();
	pbd::game_overlay::publish(panel);
}

void pbd::rollback_session::practice_steps::publish_native_menu(bool decision, bool remembered_ready, bool situation_ready, u32 timer_frames)
{
	const bool r3_remembers = pbd::practice::controls::r3_remembers(data.practice.button_map);
	if (data.practice.view.native_menu)
	{
		native_menu menu{*this, decision, remembered_ready, situation_ready, timer_frames, r3_remembers};
		menu.build();
	}
}
