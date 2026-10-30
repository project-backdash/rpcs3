// Practice's pad reading: the menu's controls, the mouse on the menu, the take's question and the dummy chords.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_dummy_move_runtime.h"

namespace category = pbd::practice::categories;

void pbd::rollback_session::practice_steps::read_pad()
{
	update_pause_requests();
	// Read once per frame, so a tap shorter than a frame still opens, moves or records (pbd::input_taps::tap_buffer).
	const u16 host_pad = pbd_local_pad_word(pbd::input_taps::reader::practice);
	// The in-game Settings menu holds this controller while it is open and until its buttons are released after it closes,
	// so Practice reads a neutral pad: no menu action, chord or leave hold, and the press that closed Settings acts on neither.
	const bool settings_claim = pbd::settings_menu::claimed();
	const u16 pad = data.game_overlay.claim || settings_claim ? 0 : host_pad;
	u16 pressed = static_cast<u16>(pad & ~data.practice.dummy.pad_prev);
	trace_pad(host_pad, pad, pressed);
	data.practice.dummy.pad = pad;
	data.practice.dummy.pad_prev = pad;
	// Cross pressed up to two frames before SELECT still counts as SELECT+Cross for the restart chords.
	const u16 chord_pressed = data.practice.chord_window.update(pad, pressed, index);
	const auto now = session_clock::now();
	const bool select = (pad & session_select_bit) != 0, start = (pad & session_start_bit) != 0;
	if (practice_leave_held(select, start, now))
		return;
	const auto [start_press, settings_pause, settings_resume, takeover] = requested_practice_actions(pressed, select, start);
	if (wait_practice_reset(pad, select, chord_pressed))
		return;
	// SELECT+Circle in either order (pbd::practice::controls::record_stop), followed at every boundary so a Circle that
	// went down just before SELECT is known when SELECT arrives.
	std::size_t stop_chord_tail = 0;
	const bool stop_chord = data.practice.record_stop.update(pad, pressed, index, data.practice.dummy.pending.words.size(), stop_chord_tail) &&
	                        data.practice.dummy.state == dummy_state::recording;
	// While the dummy waits to record or is recording, START belongs to the take: it ends a recording (the "Record
	// this movement?" dialog follows once DR acknowledges pause) or, before SELECT+Circle, returns to the Dummy
	// Recording list without recording anything. SELECT+Circle ends a recording too. Both act on their press
	// whatever the dummy is doing, ahead of the wait for a released pad below. A pause taken over acts as START.
	if (stop_recording_action(start_press, stop_chord, takeover, stop_chord_tail))
		return;
	open_requested_panel(pad, start_press, settings_pause, takeover);
	stop_unreadable_dummy(pad, now);
	if (release_practice_input(pad, select, chord_pressed))
		return;
	if (read_without_pause(pressed))
		return;
	menu_pointer(pad, pressed, settings_claim);
	if (confirm_dialog(pressed))
		return;
	if (menu_navigation(pressed, select, settings_resume))
		return;
	if (!data.session.in_match)
		return;
	record_chord(pressed, select, chord_pressed, now);
	data.replay.pad_prev = pad;
	data.replay.pad = pad;
}

void pbd::rollback_session::practice_steps::menu_pointer(u16 pad, u16& pressed, bool settings_claim)
{
	// Pad ownership wins while a physical control is held. Mouse actions then use the same menu
	// branches as confirm/navigation, with the renderer's page identity and absolute row checked here.
	if (!pad && !data.game_overlay.claim && !settings_claim && data.practice.native.paused && (data.practice.menu.open || data.practice.dummy.state == dummy_state::confirm))
	{
		if (const auto mouse = pbd::practice::mouse::take({data.practice.menu.generation, pointer_context(), static_cast<u64>(Emu.GetEmulationIdentifier()), data.practice.menu.mouse_revision}))
		{
			bool accepted = false;
			const bool list_menu = data.practice.dummy.state != dummy_state::confirm;
			// Over the move list only the wheel, the control bar and a click beside it (which closes it, as Circle does) act.
			const bool move_list = data.practice.menu.move_list.open;
			const bool beside_list = move_list && mouse->kind != pbd::practice::mouse::action::scroll &&
			                         mouse->kind != pbd::practice::mouse::action::prompt && mouse->kind != pbd::practice::mouse::action::dismiss &&
			                         mouse->kind != pbd::practice::mouse::action::back;
			if (beside_list)
				accepted = false;
			else if (mouse->kind == pbd::practice::mouse::action::back)
			{
				pressed |= 0x0020;
				accepted = true;
			}
			else if (mouse->kind == pbd::practice::mouse::action::choose_page)
			{
				// A rail click focuses that category and shows its list.
				if (list_menu && !data.practice.menu.dropdown_open && data.practice.menu.confirm == pbd::practice::confirm_kind::none && mouse->value < category::count)
				{
					data.practice.menu.page = mouse->value;
					data.practice.menu.row = 0;
					data.practice.menu.rail_focus = true;
					queue_sound(pbd::practice::native::sound::cursor);
					accepted = true;
					practice_dummy_log(fmt::format("{\"type\":\"practice_menu_open\",\"boundary\":%u,\"page\":%u,\"pause\":1,\"mouse\":true}\n", index, data.practice.menu.page));
				}
			}
			else if (mouse->kind == pbd::practice::mouse::action::scroll)
			{
				// A stage-list wheel chooses a stage even when the category rail still owns the pad focus.
				if (data.practice.menu.page == category::stage_selection && !data.practice.menu.dropdown_open &&
					data.practice.menu.confirm == pbd::practice::confirm_kind::none)
					data.practice.menu.rail_focus = false;
				pressed |= mouse->value ? 0x4000 : 0x1000;
				accepted = true;
			}
			else if (mouse->kind == pbd::practice::mouse::action::hover_option || mouse->kind == pbd::practice::mouse::action::choose_option)
			{
				mouse_option(*mouse, pressed, list_menu, accepted);
			}
			else if (mouse->kind == pbd::practice::mouse::action::dismiss)
			{
				// A click outside an open dropdown or question closes it without a change, as Circle does.
				if (list_menu && (data.practice.menu.dropdown_open || data.practice.menu.confirm != pbd::practice::confirm_kind::none || move_list))
				{
					pressed |= 0x0020;
					accepted = true;
				}
			}
			else if (mouse->kind == pbd::practice::mouse::action::frequency)
			{
				if (list_menu && !data.practice.menu.dropdown_open && practice_frequency_row())
				{
					pressed |= mouse->value ? 0x0008 : 0x0004;
					accepted = true;
				}
			}
			else if (mouse->kind == pbd::practice::mouse::action::prompt)
			{
				mouse_prompt(*mouse, pressed, list_menu, accepted);
			}
			else
			{
				mouse_row(*mouse, pressed, list_menu, accepted);
			}
			practice_dummy_log(fmt::format("{\"type\":\"practice_mouse\",\"boundary\":%u,\"generation\":%u,\"context\":%u,\"action\":%u,\"value\":%u,\"accepted\":%s}\n",
				index, mouse->owner.generation, mouse->owner.context, static_cast<u32>(mouse->kind), mouse->value, json_bool(accepted)));
		}
	}
}

bool pbd::rollback_session::practice_steps::confirm_dialog(u16 pressed)
{
	if (data.practice.dummy.state == dummy_state::confirm)
	{
		// "Record this movement?": Yes / Record again / Exit without recording. Only Confirm acts.
		// Yes and Discard return to the Defense rows of Practice Settings with the slot
		// focused, so a slot's frequency can be set straight away; Record again returns to the fight, ready to
		// record the same one.
		const auto back_to_slot = [&]()
		{
			data.practice.dummy.state = dummy_state::idle;
			if (data.practice.training_mode != pbd::practice::training_mode::defense)
			{
				data.practice.training_mode = pbd::practice::training_mode::defense;
				practice_movement_refresh();
			}
			data.practice.menu.open = true;
			data.practice.menu.page = category::practice_settings;
			data.practice.menu.row = practice_record_row();
			data.practice.menu.rail_focus = false;
			data.practice.menu.dropdown_open = false;
			data.practice.dummy.ignore_until_release = true;
			data.practice.pause.requested = false;
		};
		// The dialog is drawn only over DR's acknowledged pause; until then Confirm must not act on it unseen.
		if (!data.practice.native.paused || data.practice_resume_settling())
			return true;
		if (pressed & 0x5000)
		{
			data.practice.dummy.choice = pbd::practice::wrap_step(data.practice.dummy.choice, 3, (pressed & 0x4000) != 0);
			queue_sound(pbd::practice::native::sound::cursor);
		}
		else if (pressed & 0x0040)
		{
			if (data.practice.dummy.choice == 0 && !data.practice.dummy.pending.words.empty())
			{
				queue_sound(pbd::practice::native::sound::confirm);
				practice_dummy_accept(index, false);
				back_to_slot();
			}
			else if (data.practice.dummy.choice == 1)
			{
				queue_sound(pbd::practice::native::sound::confirm);
				practice_dummy_arm(index);
				resume_native();
			}
			else if (data.practice.dummy.choice == 2)
			{
				queue_sound(pbd::practice::native::sound::cancel);
				practice_clear_pending();
				practice_dummy_log(fmt::format("{\"type\":\"dummy_record_discard\",\"boundary\":%u,\"slot\":%u}\n", index, practice_record_log_slot()));
				back_to_slot();
			}
		}
		return true;
	}
	return false;
}

bool pbd::rollback_session::practice_steps::menu_navigation(u16 pressed, bool select, bool settings_resume)
{
	if (data.practice.menu.open)
	{
		if (settings_resume && data.practice.menu.by_escape && data.practice.menu.confirm == pbd::practice::confirm_kind::none &&
			!data.practice.menu.dropdown_open)
		{
			resume_native();
			return true;
		}
		// Like the dialog, the menu acts only while it is drawn: a game running under it is paused again first.
		if (!data.practice.native.paused)
			return true;
		// The move list, open over the menu, takes every control until Circle closes it.
		if (data.practice.menu.move_list.open)
		{
			practice_move_list_input(pressed, select);
			return true;
		}
		if (data.practice.menu.confirm != pbd::practice::confirm_kind::none)
		{
			// A question over the list: Up/Down move between Yes and No, Cross answers, Circle closes it as No.
			// START, L1/R1 and Left wait until it closes.
			if (pressed & 0x5000)
			{
				data.practice.menu.confirm_choice = pbd::practice::wrap_step(data.practice.menu.confirm_choice, pbd::practice::answer_count, (pressed & 0x4000) != 0);
				queue_sound(pbd::practice::native::sound::cursor);
			}
			else if (pressed & 0x0040)
				answer_confirm(data.practice.menu.confirm_choice == pbd::practice::answer_yes);
			else if (pressed & 0x0020)
				answer_confirm(false);
			return true;
		}
		if (navigate_dropdown(pressed))
			return true;
		if (!select && (pressed & session_start_bit))
		{
			queue_sound(pbd::practice::native::sound::cancel);
			resume_native();
			return true;
		}
		if (navigate_categories(pressed))
			return true;
		if (pressed & 0x000c)
		{
			// L1/R1 only set a filled slot's frequency; pages change from the rail.
			if (practice_frequency_row())
			{
				queue_sound(pbd::practice::native::sound::cursor);
				const u32 slot_index = *practice_focused_slot();
				auto& slot = data.practice.dummy.slots[slot_index];
				slot.frequency = pbd::practice::step_frequency(slot.frequency, (pressed & 0x0008) != 0);
				practice_dummy_log(fmt::format("{\"type\":\"dummy_frequency\",\"boundary\":%u,\"slot\":%u,\"weight\":%u}\n", index, slot_index + 1, slot.frequency));
			}
		}
		else if (pbd::practice::list_back(pressed))
		{
			// Circle or Left: back to the category rail.
			queue_sound(pbd::practice::native::sound::cancel);
			data.practice.menu.rail_focus = true;
		}
		else if (pressed & 0x5000)
		{
			queue_sound(pbd::practice::native::sound::cursor);
			data.practice.menu.row = practice_row_step((pressed & 0x4000) != 0);
		}
		else if (pressed & 0x0040)
		{
			activate_practice_row();
		}
		return true;
	}
	return false;
}

void pbd::rollback_session::practice_steps::record_chord(u16 pressed, bool select, u16 chord_pressed,
	session_clock::time_point now)
{
	// SELECT+Circle: in the ready state it starts the take for the row that chose Record (START or SELECT+Circle
	// ends it, both handled above); in Defense it starts playback and, pressed again, stops it; in Offense with a
	// Movement chosen it turns the dummy's movement off and on again.
	if (select && (pressed & 0x0020))
	{
		select_circle_chord(now);
	}
	else
	{
		const bool test_remember = !data.practice.test.remembered && data.practice.test.remember != umax && index >= data.session.match_start + data.practice.test.remember;
		const bool test_restart = !data.practice.test.restarted && data.practice.test.restart != umax && index >= data.session.match_start + data.practice.test.restart;
		if (practice_can_change_situation() && !data.takeover.ignore_pad_until_release)
		{
			if (select && (chord_pressed & 0xf440))
				restart_chord(chord_pressed);
			else if (test_restart)
			{
				data.practice.test.restarted |= test_restart;
				restored = restore_takeover("practice_restart", 3, index);
				if (restored)
					practice_restart_after_restore(index);
				else
				{
					data.practice.history.reset();
					data.practice.health_valid = false;
					data.practice.view.fighters = {};
					data.practice.combos = {};
					data.practice.streak.reset();
					data.practice.precision.reset();
					data.practice.punish.reset();
					data.practice.view.fight_live = false;
				}
				if (!restored && data.takeover.keyframe.valid)
					data.session.pending_end = takeover_relaunch;
			}
			// R3 remembers unless it has an action: then the press is that action, for the fight.
			else if ((pressed & 0x0400) && pbd::practice::controls::r3_remembers(data.practice.button_map))
			{
				if (pbd::game_lifecycle::requested())
					remember_position();
				else
				{
					data.practice.view.toast = "Remember Position isn't available";
					data.practice.menu.toast_until = now + std::chrono::seconds(3);
				}
				data.takeover.ignore_pad_until_release = true;
			}
			else if (test_remember)
			{
				data.practice.test.remembered |= test_remember;
				request_takeover_capture(index);
				const auto event = fmt::format("{\"type\":\"practice_remember\",\"boundary\":%u,\"match\":%u,\"match_frame\":%u}\n", index, data.session.matches, match_frame(index));
				data.replay.control_file.write(event.data(), event.size());
			}
		}
	}
}
