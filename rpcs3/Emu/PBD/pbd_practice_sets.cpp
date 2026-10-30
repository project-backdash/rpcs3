#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

auto pbd::rollback_session::practice_steps::recording_set_choices(bool loading) -> practice_choice_list
{
	practice_choice_list out;
	const auto* memory = session_runtime.m_practice_memory.get();
	if (!memory || !memory->dummy)
		return out;
	const auto character = *memory->dummy;
	const auto selected = memory->set_position.find(character);
	out.current = selected == memory->set_position.end() ? 0 : selected->second;
	const bool busy = data.practice.dummy.state != dummy_state::idle || memory->sets.pending(character);
	for (u32 position = 0; position < practice_store::recording_set_count; ++position)
	{
		pbd::practice::menu_option next;
		const auto* saved = memory->sets.find(character, position);
		const auto name = practice_store::set_name(saved, position);
		next.text = pbd::practice::menu::shortened(name, 22);
		next.note = saved ? fmt::format("%u actions", practice_store::action_count(saved->content)) : "Empty";
		next.help = loading ? "Replace the eight current actions and their playback settings with this set." :
		                      "Choose a name, then save the eight current actions and their playback settings.";
		if (saved)
			next.help = name + " | Saved " + saved->saved_at.substr(0, 10) + ". " + next.help;
		next.enabled = !busy && (!loading || saved);
		out.options.push_back(std::move(next));
	}
	return out;
}

void pbd::rollback_session::practice_steps::recording_set_choose(bool loading, u32 position)
{
	auto* memory = session_runtime.m_practice_memory.get();
	if (!memory || !memory->dummy || data.practice.dummy.state != dummy_state::idle || memory->sets.pending(*memory->dummy) ||
		!practice_store::valid_set_position(*memory->dummy, position))
		return;
	practice_store::set_content live{memory_slots(*memory->dummy), memory_rows().defense};
	if (loading)
	{
		if (!memory->sets.find(*memory->dummy, position))
			return;
		memory->load_set_requested = std::pair{*memory->dummy, position};
		if (memory->sets.changed(live))
			open_confirm(pbd::practice::confirm_kind::load_recordings);
		else
			recording_set_apply();
		return;
	}
	const auto* saved = memory->sets.find(*memory->dummy, position);
	memory->set_editor = practice_store::recording_name_editor{*memory->dummy, position, 0, 0,
		saved ? practice_store::set_name(saved, position) : std::string{}};
	data.practice.menu.dropdown_open = true;
	data.practice.menu.dropdown_focus = 0;
}

bool pbd::rollback_session::practice_steps::recording_set_name_input(u16 pressed)
{
	auto* memory = session_runtime.m_practice_memory.get();
	if (!memory || !memory->set_editor)
		return false;
	auto& editor = *memory->set_editor;
	if (memory->dummy != editor.character || !data.practice.menu.dropdown_open)
	{
		memory->set_editor.reset();
		return false;
	}
	const auto cancel = [&]
	{
		data.practice.menu.dropdown_focus = editor.position;
		memory->set_editor.reset();
	};
	if (pressed & 0x5000)
	{
		data.practice.menu.dropdown_focus = pbd::practice::wrap_step(data.practice.menu.dropdown_focus,
			static_cast<u32>(editor.choices().size()), (pressed & 0x4000) != 0);
		queue_sound(pbd::practice::native::sound::cursor);
	}
	else if (pressed & 0x0020)
	{
		queue_sound(pbd::practice::native::sound::cancel);
		if (editor.back())
			data.practice.menu.dropdown_focus = editor.focus;
		else
			cancel();
	}
	else if (pressed & 0x0040)
	{
		const auto options = editor.choices();
		if (data.practice.menu.dropdown_focus >= options.size() || !options[data.practice.menu.dropdown_focus].enabled)
			return true;
		queue_sound(pbd::practice::native::sound::confirm);
		const auto result = editor.choose(data.practice.menu.dropdown_focus);
		data.practice.menu.dropdown_focus = editor.focus;
		if (result == practice_store::recording_name_editor::result::cancel)
			cancel();
		else if (result == practice_store::recording_name_editor::result::save)
		{
			data.practice.menu.dropdown_open = false;
			if (memory->sets.find(editor.character, editor.position))
				open_confirm(pbd::practice::confirm_kind::save_recordings);
			else
				recording_set_save();
		}
	}
	return true;
}

void pbd::rollback_session::practice_steps::recording_set_save()
{
	auto* memory = session_runtime.m_practice_memory.get();
	if (!memory || !memory->set_editor)
		return;
	const auto editor = *memory->set_editor;
	if (memory->dummy != editor.character || data.practice.dummy.state != dummy_state::idle)
	{
		memory->set_editor.reset();
		return;
	}
	const auto name = editor.name();
	const bool queued = memory->sets.submit(memory->files, memory->root.parent_path(), editor.position,
		{memory_slots(editor.character), memory_rows().defense}, pbd::set_channel::utc_now(), name);
	if (queued)
	{
		memory->set_position[editor.character] = editor.position;
		memory->set_editor.reset();
	}
	data.practice.menu.dropdown_open = !queued;
	data.practice.menu.dropdown_focus = 0;
	data.practice.view.toast = queued ? fmt::format("Saving %s...", name) : "Could not save recordings";
	data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
}

void pbd::rollback_session::practice_steps::recording_set_apply()
{
	auto* memory = session_runtime.m_practice_memory.get();
	if (!memory || !memory->load_set_requested)
		return;
	const auto [character, position] = *memory->load_set_requested;
	memory->load_set_requested.reset();
	if (memory->dummy != character || data.practice.dummy.state != dummy_state::idle)
		return;
	const auto chosen = memory->sets.select(character, position);
	if (!chosen)
		return;
	auto rows = memory_rows();
	rows.defense = chosen->playback;
	practice_store::drop_empty_reaction(rows.offense, chosen->actions);
	memory_apply(rows, chosen->actions);
	memory_save();
	memory->set_position[character] = position;
	data.practice.view.toast = practice_store::set_name(memory->sets.find(character, position), position) + " loaded";
	data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
	practice_dummy_log(fmt::format("{\"type\":\"practice_set_loaded\",\"boundary\":%u,\"character\":%u,\"set\":%u}\n",
		index, character, position + 1));
}

void pbd::rollback_session::practice_steps::recording_set_poll()
{
	auto* memory = session_runtime.m_practice_memory.get();
	if (!memory)
		return;
	for (const auto& done : memory->sets.poll())
	{
		practice_dummy_log(fmt::format("{\"type\":\"practice_set_saved\",\"boundary\":%u,\"character\":%u,\"set\":%u,\"saved\":%s}\n",
			index, done.character, done.position + 1, done.saved ? "true" : "false"));
		if (memory->dummy != done.character)
			continue;
		data.practice.view.toast = done.saved ? practice_store::set_name(memory->sets.find(done.character, done.position), done.position) + " saved" : "Could not save recordings";
		data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
	}
}
