#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_dummy_move_runtime.h"
#include "pbd_practice_paint.h"

namespace category = pbd::practice::categories;

void pbd::rollback_session::practice_steps::update_pause_requests()
{
	const auto current_pause = read_practice_pause();
	data.practice.native.pause_valid = current_pause.valid;
	data.practice.native.flag = current_pause.paused;
	data.practice.native.menu_word = current_pause.valid && current_pause.menu != 0;
	data.practice.native.paused = pbd::practice::controls::native_paused(current_pause.paused, data.practice.native.ran);
	// DR's pause acknowledgement satisfies the take's question or open menu request.
	// Clear it here because recording can resume without passing through open_panel.
	if (current_pause.valid && data.practice.native.paused)
	{
		data.practice.pause.injected_at = umax;
		if (data.practice.dummy.state == dummy_state::confirm || data.practice.menu.open)
			data.practice.pause.requested = false;
	}
	else if (data.practice.pause.requested && pbd::practice::controls::pause_press_lost(data.practice.pause.injected,
												  data.practice.pause.injected_at, index, current_pause.valid, data.practice.native.paused))
	{
		practice_dummy_log(fmt::format("{\"type\":\"practice_pause_retry\",\"boundary\":%u,\"injected_at\":%u,\"state\":%u,\"menu\":%s}\n",
			index, data.practice.pause.injected_at, static_cast<u32>(data.practice.dummy.state),
			json_bool(data.practice.menu.open)));
		data.practice.pause.injected = false;
	}
	// A resume START DR has not answered is sent again while Practice still wants it, else given up so the pause is
	// taken over below. A resume not sent yet is dropped once the game runs: a START then would pause it.
	using resume_outcome = pbd::practice::controls::resume_watch::outcome;
	const bool resume_wanted = data.session.in_match && data.practice.native.screen == 0 && !data.practice.menu.open &&
	                           data.practice.dummy.state != dummy_state::confirm && !data.practice.pause.requested;
	const u32 resume_sent_at = data.practice.pause.resume_watch.sent_at, resume_presses = data.practice.pause.resume_watch.presses;
	const auto resume_result = data.practice.pause.resume_watch.update(index, current_pause.valid, data.practice.native.paused, resume_wanted);
	if (resume_result == resume_outcome::resend)
	{
		data.practice.pause.resume_native = true;
		data.practice.pause.resume_injected = false;
	}
	if (resume_result == resume_outcome::resend || resume_result == resume_outcome::given_up)
		practice_dummy_log(fmt::format("{\"type\":\"practice_resume_%s\",\"boundary\":%u,\"sent_at\":%u,\"presses\":%u,\"state\":%u,\"menu\":%s,\"requested\":%s,\"flag\":%s}\n",
			resume_result == resume_outcome::resend ? "retry" : "given_up", index, resume_sent_at, resume_presses, static_cast<u32>(data.practice.dummy.state),
			json_bool(data.practice.menu.open), json_bool(data.practice.pause.requested), json_bool(data.practice.native.flag)));
	if (current_pause.valid && !data.practice.native.paused && data.practice.pause.resume_native)
		data.practice.pause.resume_native = false;
}

bool pbd::rollback_session::practice_steps::practice_leave_held(bool select, bool start, session_clock::time_point now)
{
	if (select && start)
	{
		if (!data.replay.leave_held)
			data.replay.leave_from = now;
		data.replay.leave_held = true;
	}
	else
		data.replay.leave_held = false;
	const u32 hold_ms = data.replay.leave_held ? static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(now - data.replay.leave_from).count()) : 0;
	if (data.live_stats)
		data.live_pub.leave(select, std::min(hold_ms, pbd::live_stats::leave_hold_total_ms));
	if (hold_ms >= pbd::live_stats::leave_hold_total_ms && data.session.pending_end == collecting)
	{
		leave();
		return true;
	}

	return false;
}

std::array<bool, 4> pbd::rollback_session::practice_steps::requested_practice_actions(u16 pressed, bool select, bool start)
{
	// Updated at every boundary, before the pending Restart returns below: a START pressed while the Restart is
	// still pending, or with SELECT still down from its chord, opens the menu once the menu can take it.
	if (!select)
		data.practice.chord_select_held = false;
	const bool start_press = data.practice.menu.start.update(start, (pressed & session_start_bit) != 0, select,
		data.practice.restart.modifier_held && data.practice.chord_select_held,
		data.session.in_match && !data.practice.menu.open && data.practice.native.screen == 0 &&
			data.practice.dummy.state != dummy_state::confirm,
		!data.practice.restart.reset_request && data.practice.native.pause_valid);
	// Esc opened the in-game Settings menu over the fight: pause as START does, with the pause menu on its Settings category
	// under it. Taken at every boundary, so a request this boundary cannot act on (a pending restart, a take being recorded
	// or reviewed) is dropped rather than pausing the game after Settings has closed.
	const bool settings_pause = pbd::settings_menu::take_practice_pause() && pbd::settings_menu::shown() &&
	                            data.practice.dummy.state != dummy_state::recording &&
	                            data.practice.dummy.state != dummy_state::confirm &&
	                            data.practice.dummy.state != dummy_state::ready;
	// Return to Game in that menu's top level. Left waiting while the pause Esc asked for has not opened its menu yet,
	// so a quick Esc then Return to Game still resumes; acted on below only for a pause menu Esc opened.
	const bool settings_resume = (!data.practice.pause.requested || data.practice.menu.open) && pbd::settings_menu::take_practice_resume();
	// DR's pause that nothing in Practice opened, shows or is leaving is answered below as a START press would be
	// (pbd::practice::controls::pause_takeover): DR's own pause menu is never left to the player.
	const bool takeover = data.practice.pause.takeover.update(!data.practice.restart.reset_request &&
															  pbd::practice::controls::pause_unowned(data.practice_pause_scene()));
	if (takeover)
		practice_dummy_log(fmt::format("{\"type\":\"practice_pause_takeover\",\"boundary\":%u,\"state\":%u,\"counter\":%u}\n",
			index, static_cast<u32>(data.practice.dummy.state), data.practice.boundary_counter));

	return {start_press, settings_pause, settings_resume, takeover};
}

bool pbd::rollback_session::practice_steps::wait_practice_reset(u16 pad, bool select, u16 chord_pressed)
{
	if (data.practice.restart.reset_request)
	{
		data.practice.dummy.mask_ui = true;
		if (!pad)
			data.practice.dummy.ignore_until_release = false;
		// Another restart chord while one is still pending replaces it: the latest choice is the one that runs.
		if (select && (chord_pressed & 0xf440) && !data.practice.menu.open)
			restart_chord(chord_pressed);
		return true;
	}
	return false;
}

bool pbd::rollback_session::practice_steps::stop_recording_action(bool start_press, bool stop_chord, bool takeover, std::size_t stop_chord_tail)
{
	if ((data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::ready) &&
		!data.practice.menu.open &&
		data.practice.native.screen == 0 && data.session.in_match && data.practice.native.pause_valid && (start_press || stop_chord || takeover))
	{
		const bool by_chord = stop_chord && !start_press;
		if (data.practice.dummy.state == dummy_state::recording)
			practice_dummy_stop_record(index, start_press ? "start" : by_chord ? "select_circle" :
																				 "native_pause",
				by_chord ? static_cast<u32>(stop_chord_tail) : 0u);
		else
		{
			data.practice.dummy.state = dummy_state::idle;
			data.practice.menu.return_slot = practice_record_row();
			data.practice.dummy.ignore_until_release = true;
			data.practice.dummy.mask_ui = true;
			// START soon after Record can arrive while DR is still paused: drop the resume not yet sent so the
			// menu does not open over a game that is about to unpause.
			if (data.practice.native.paused && !data.practice_resume_settling())
			{
				data.practice.pause.resume_native = false;
				open_panel();
			}
			else
			{
				data.practice.pause.requested = true;
				data.practice.pause.injected = false;
			}
			practice_dummy_log(fmt::format("{\"type\":\"dummy_record_cancel\",\"boundary\":%u,\"slot\":%u}\n",
				index, practice_record_log_slot()));
		}
		return true;
	}
	return false;
}

void pbd::rollback_session::practice_steps::open_requested_panel(u16 pad, bool start_press, bool settings_pause, bool takeover)
{
	// START is injected once while both physical ports are claimed. PBD becomes visible only after DR acknowledges pause.
	if (!data.practice.menu.open && data.practice.native.screen == 0 && data.session.in_match && data.practice.native.pause_valid)
	{
		if (data.practice.pause.requested && data.practice.native.paused && !data.practice_resume_settling() && !practice_memory_pause())
			open_panel();
		else if (start_press || settings_pause || takeover)
		{
			data.practice.menu.on_settings = settings_pause && !start_press;
			if (data.practice.native.paused && !data.practice_resume_settling())
				open_panel();
			else
			{
				data.practice.pause.requested = true;
				data.practice.pause.injected = false;
			}
		}
	}
	if (data.practice.native.screen == 1 && !pad && pbd::practice::paint::screen_returned())
	{
		data.practice.native.screen = 0;
		open_panel();
	}
	else if (data.practice.native.screen == 2 && !data.session.in_match && !pad)
		data.practice.native.screen = 0;
	data.practice.dummy.mask_ui = data.practice.menu.open || data.practice.dummy.state == dummy_state::confirm;
}

void pbd::rollback_session::practice_steps::stop_unreadable_dummy(u16 pad, session_clock::time_point now)
{
	if (!data.practice.native.pause_valid && (data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::playing || data.practice.dummy.state == dummy_state::ready))
	{
		const bool keep_take = !data.practice.dummy.pending.words.empty() && (data.practice.dummy.state == dummy_state::recording ||
																				 data.practice.dummy.preview);
		const bool was_recording = data.practice.dummy.state == dummy_state::recording ||
		                           data.practice.dummy.state == dummy_state::ready;
		if (was_recording)
			practice_measure_pending();
		data.practice.dummy.state = keep_take ? dummy_state::confirm : dummy_state::idle;
		data.practice.dummy.preview = false;
		data.practice.dummy.have_play_slot = false;
		data.practice.dummy.play_pos = 0;
		data.practice.dummy.wait = 0;
		data.practice.movement.pos = 0;
		data.practice.play_before.valid = false;
		data.practice.record_word_valid = false;
		data.practice.pause.resume_native = false;
		data.practice.pause.resume_injected = false;
		data.practice.dummy.choice = 0;
		data.practice.dummy.ignore_until_release = pad != 0;
		data.practice.dummy.mask_ui = data.practice.menu.open || keep_take || data.practice.dummy.ignore_until_release;
		data.practice.view.toast = was_recording ? "Recording stopped\tThe game's practice data can't be read" : "Playback stopped\tThe game's practice data can't be read";
		data.practice.menu.toast_until = now + std::chrono::seconds(4);
		practice_dummy_log(fmt::format("{\"type\":\"dummy_reader_stop\",\"boundary\":%u,\"recording\":%s,\"pending_frames\":%u,\"review\":%s}\n",
			index, json_bool(was_recording), static_cast<u32>(data.practice.dummy.pending.words.size()), json_bool(keep_take)));
	}
}

bool pbd::rollback_session::practice_steps::release_practice_input(u16 pad, bool select, u16 chord_pressed)
{
	// Restart owns only the chord's action buttons. SELECT can stay down
	// between resets; a fresh action edge also rearms it while another is held.
	const bool restart_released = data.practice.restart.modifier_held && !(pad & ~session_select_bit);
	const bool next_restart = data.practice.restart.modifier_held && select && (chord_pressed & 0xf440) &&
	                          !data.practice.menu.open && !data.practice.native.screen &&
	                          data.practice.dummy.state != dummy_state::confirm &&
	                          practice_can_change_situation();
	if (!pad || restart_released || next_restart)
	{
		data.takeover.ignore_pad_until_release = false;
		if (data.practice.restart.modifier_held)
			data.practice.dummy.ignore_until_release = false;
		if (!pad)
			data.practice.restart.modifier_held = false;
	}
	if (data.practice.dummy.ignore_until_release)
	{
		data.practice.dummy.mask_ui = true;
		if (!pad)
		{
			data.practice.dummy.ignore_until_release = false;
			pbd::practice::input_claim.store(pbd::practice::controls::claims_input(data.practice_pause_scene()) ||
												 data.practice.restart.reset_request.has_value(),
				std::memory_order_release);
		}
		return true;
	}
	if (data.practice.native.screen)
		return true;

	return false;
}

bool pbd::rollback_session::practice_steps::read_without_pause(u16 pressed)
{
	if (!data.practice.native.pause_valid)
	{
		if (data.practice.dummy.state == dummy_state::confirm)
		{
			// Record again needs the game's pause state, so only Yes and Exit without recording remain.
			if (pressed & 0x5000)
				data.practice.dummy.choice = data.practice.dummy.choice == 0 ? 2 : 0;
			else if ((pressed & 0x0040) && data.practice.dummy.choice == 0 && !data.practice.dummy.pending.words.empty())
			{
				practice_dummy_accept(index, false);
				data.practice.dummy.state = dummy_state::idle;
				close_panel();
			}
			else if ((pressed & 0x0040) && data.practice.dummy.choice == 2)
			{
				practice_clear_pending();
				data.practice.dummy.state = dummy_state::idle;
				close_panel();
			}
		}
		else if (data.practice.menu.open)
		{
			// Without the game's pause state only navigation and Exit work; open lists and questions close, and
			// leaving the menu (Circle on the rail, or START) exits Practice.
			data.practice.menu.dropdown_open = false;
			data.practice.menu.confirm = pbd::practice::confirm_kind::none;
			if (session_runtime.m_practice_memory)
				session_runtime.m_practice_memory->set_editor.reset();
			if (pressed & session_start_bit)
				leave();
			else if (data.practice.menu.rail_focus)
			{
				pbd::practice::column_focus focus{true, data.practice.menu.page, data.practice.menu.row};
				const auto step = pbd::practice::rail_step(focus, pressed, category::count, data.practice.menu.page != category::move_list);
				if (step == pbd::practice::rail_action::resume)
					leave();
				else if (step != pbd::practice::rail_action::none)
				{
					data.practice.menu.page = focus.page;
					data.practice.menu.row = focus.row;
					data.practice.menu.rail_focus = focus.rail;
				}
			}
			else if (pbd::practice::list_back(pressed))
				data.practice.menu.rail_focus = true;
			else if (pressed & 0x5000)
				data.practice.menu.row = practice_row_step((pressed & 0x4000) != 0);
			else if (data.practice.menu.page == category::exit_practice && (pressed & 0x0040))
				leave();
			else if (data.practice.menu.page == category::settings && (pressed & 0x0040))
				pbd::settings_menu::request_open();
		}
		return true;
	}
	return false;
}

void pbd::rollback_session::practice_steps::select_circle_chord(session_clock::time_point now)
{
	using pbd::practice::dummy_phase;
	const auto phase = data.practice.dummy.state == dummy_state::ready     ? dummy_phase::ready :
	                   data.practice.dummy.state == dummy_state::recording ? dummy_phase::recording :
	                   data.practice.dummy.state == dummy_state::playing   ? (data.practice.dummy.preview ? dummy_phase::preview : dummy_phase::playing) :
	                                                                         dummy_phase::idle;
	// An Action After a Hit or Block is turned off and on the same way as a Movement Technique.
	const bool offense_reaction = data.practice.offense.after != pbd::practice::after_contact::none;
	const bool offense_movement = data.practice.dummy.enabled && (!data.practice.movement.input_sequence.empty() || offense_reaction);
	switch (pbd::practice::select_circle(data.practice.training_mode, phase, offense_movement))
	{
	case pbd::practice::circle_chord::toggle_movement:
		data.practice.offense.movement_off = !data.practice.offense.movement_off;
		if (data.practice.offense.movement_off && data.practice.dummy.state == dummy_state::playing)
			practice_dummy_stop_playback(index, "chord");
		data.practice.view.toast = offense_reaction ?
		                               (data.practice.offense.movement_off ? "Dummy reaction off\t{select}+{circle} turns it back on" :
																			 "Dummy reaction on\t{select}+{circle} turns it off") :
		                           data.practice.offense.movement_off ? "Dummy movement off\t{select}+{circle} turns it back on" :
		                                                                "Dummy movement on\t{select}+{circle} turns it off";
		data.practice.menu.toast_until = now + std::chrono::seconds(3);
		data.practice.dummy.ignore_until_release = true;
		practice_dummy_log(fmt::format("{\"type\":\"dummy_offense_movement\",\"boundary\":%u,\"on\":%s,\"movement\":%u,\"after_contact\":%u}\n",
			index, json_bool(!data.practice.offense.movement_off), static_cast<u32>(data.practice.offense.movement),
			static_cast<u32>(data.practice.offense.after)));
		break;
	case pbd::practice::circle_chord::start_recording:
		practice_dummy_start_record(index);
		// From the ready state the resume START may already be in flight; a second one would pause DR again.
		if (data.practice.native.paused && !data.practice_resume_settling())
			resume_native();
		break;
	case pbd::practice::circle_chord::stop_playback:
		practice_dummy_stop_playback(index, "chord");
		if (data.practice.dummy.state != dummy_state::confirm && data.practice.native.paused)
			resume_native();
		break;
	case pbd::practice::circle_chord::start_playback:
		if (practice_dummy_start_playback(index) && data.practice.native.paused)
			resume_native();
		data.practice.dummy.ignore_until_release = true;
		break;
	case pbd::practice::circle_chord::needs_defense:
		data.practice.view.toast = "Dummy playback is in Defense\tPractice Settings > Training Mode";
		data.practice.menu.toast_until = now + std::chrono::seconds(3);
		data.practice.dummy.ignore_until_release = true;
		break;
	// A take in progress is stopped before any other chord is read (record_stop, above).
	case pbd::practice::circle_chord::stop_recording:
	case pbd::practice::circle_chord::none:
		break;
	}
	// Like the restart chords, the toggle owns only Circle: releasing it while SELECT stays down rearms the
	// chord, so SELECT can be held and Circle tapped once to start playback and again to stop it.
	if (phase == dummy_phase::idle || phase == dummy_phase::playing)
	{
		data.practice.restart.modifier_held = true;
		data.practice.chord_select_held = true;
	}
	practice_dummy_log(fmt::format("{\"type\":\"dummy_circle_chord\",\"boundary\":%u,\"mode\":%u,\"state\":%u}\n",
		index, static_cast<u32>(data.practice.training_mode), static_cast<u32>(data.practice.dummy.state)));
}
