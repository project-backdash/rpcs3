#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_replay_control_buttons.h"
#include "Emu/PBD/pbd_replay_rewind.h"
#include "Emu/PBD/pbd_controller_display.h"
#include "Emu/PBD/pbd_controller_settings.h"
#include "Emu/RSX/Overlays/overlay_message.h"

void pbd::rollback_session::replay_steps::prepare_control_boundary()
{
	// A seek has arrived (its fast forward ended, or the restore landed on the moment): pause there if the replay was paused.
	if (data.rewind.seeking && !data.replay.fast.active)
	{
		data.rewind.seeking = false;
		if (std::exchange(data.rewind.pause_after, false))
			set_pause(true);
		log_event("seek_done", static_cast<s32>(match_frame(index)));
	}
	// Take Control asked for before the fight starts on its first fight boundary.
	if (data.takeover.auto_start && (data.takeover.phase != pbd::takeover_phase::armed || !data.session.in_match))
		data.takeover.auto_start = false;
	else if (data.takeover.auto_start && fighting_now)
	{
		data.takeover.auto_start = false;
		start_takeover(data.takeover.port);
	}
}

void pbd::rollback_session::replay_steps::take_requested_moment()
{
	// Trying a moment again (storage::replay_take_at): paused there with that side handed over; START plays. With the takeover it
	// is a timed takeover from this frame (Try again, SELECT + CROSS).
	if (data.replay.take.match && !data.replay.take.done && data.session.in_match && data.session.matches == data.replay.take.match &&
		match_frame(index) >= data.replay.take.frame)
	{
		data.replay.take.done = true;
		if (data.takeover.timed_take)
		{
			data.takeover.null_input = false;
			start_takeover(data.replay.take.port);
		}
		else
			take(data.replay.take.port);
		set_pause(true);
		if (data.replay.take.resume_ms)
			data.replay.test.resume_at = session_clock::now() + std::chrono::milliseconds(data.replay.take.resume_ms);
	}
}

bool pbd::rollback_session::replay_steps::scripted_actions()
{
	while (data.replay.test.next < data.replay.test.actions.size() && data.replay.test.actions[data.replay.test.next].boundary <= index)
	{
		const auto& a = data.replay.test.actions[data.replay.test.next++];
		scripted_action(a);
		if (data.session.pending_end == guard_failed)
			return false;
		if (data.takeover.redispatch)
			return true;
	}
	return false;
}

void pbd::rollback_session::replay_steps::scripted_action(const replay_test_action& a)
{
	if (scripted_playback_action(a))
		return;
	if (a.kind == 'k')
	{
		data.takeover.null_input = false;
		data.replay.test.word = a.word;
		data.replay.test.word_set = true;
		if (fighting_now)
			start_takeover(static_cast<u32>(a.value));
		else
			arm_takeover(static_cast<u32>(a.value));
	}
	else if (a.kind == 'u')
	{
		data.takeover.null_input = true;
		data.replay.test.word_set = false;
		if (fighting_now)
			start_takeover(static_cast<u32>(a.value));
		else
			arm_takeover(static_cast<u32>(a.value));
	}
	else if (a.kind == 'l')
	{
		data.takeover.null_input = false;
		if (fighting_now)
			start_takeover(1);
		else
			arm_takeover(1);
	}
	else if (a.kind == 'h')
	{
		data.takeover.null_input = false;
		if (fighting_now)
			start_takeover(2);
		else
			arm_takeover(2);
	}
	else if (a.kind == 'e' && data.takeover.phase == pbd::takeover_phase::controlling)
		show_prompt();
	else if (a.kind == 'g' && data.takeover.phase >= pbd::takeover_phase::controlling)
		try_again();
	else if (a.kind == 'r' && data.takeover.phase == pbd::takeover_phase::end_prompt)
		return_to_replay();
	else if (a.kind == 'r' || a.kind == 'g')
		data.replay.test.choice_pending = a.kind == 'g' ? 1 : 2;
	else if (a.kind == 'b')
		previous_round();
	else if (a.kind == 'n')
		next_round();
	else if (a.kind == 'x')
		seek_by(a.value);
	else if (a.kind == 'y')
		seek_to(static_cast<u32>(std::max(a.value, 0)));
	else if (a.kind == 'o')
		go_to_round(static_cast<u32>(std::max(a.value, 0)));
	else if (a.kind == 'c')
		take_over(static_cast<u32>(a.value));
}

bool pbd::rollback_session::replay_steps::finish_takeover_action()
{
	if (data.replay.takeover)
	{
		// Viewer expiry already ran before host dispatch in read_pad. Preserve legacy non-viewer ordering.
		if (data.takeover.phase == pbd::takeover_phase::controlling && !data.replay.viewer_enabled && !data.replay.paused &&
			pbd::replay_viewer::takeover_finished(index, data.takeover.deadline, fighting_now, data.last_fight_index, data.takeover.start))
			show_prompt();
		if (data.takeover.phase == pbd::takeover_phase::end_prompt && data.replay.test.choice_pending)
		{
			const u32 choice = std::exchange(data.replay.test.choice_pending, 0u);
			if (choice == 1)
				try_again();
			else
				return_to_replay();
		}
		// A scripted answer queued before the timed prompt can restore here, after the main scripted-action loop.
		// Leave the old hook dispatch exactly like direct actions and live input do; continuing would run stage 1
		// against the pre-restore stage-0 program counter.
		if (data.takeover.redispatch)
			return true;
	}
	return false;
}

replay_control_result pbd::rollback_session::replay_steps::wait_control_release()
{
	bool held = false;
	const auto hold = [&]()
	{
		if (held)
			return;
		held = true;
		vm::temporary_unlock(ppu);
	};
	for (;;)
	{
		// Paused: held until resumed, one frame is stepped, the player leaves or stops, or the emulation stops.
		for (u32 polls = 0; data.replay.paused && !data.replay.step && data.session.pending_end == collecting; polls++)
		{
			hold();
			if (polls % (data.replay.viewer_enabled ? 3 : 20) == 0)
				rsx::set_native_ui_flip(); // menu response within one frame while paused
			if (polls && polls % 400 == 0)
				log_event("paused", static_cast<s32>(polls / 200));
			if (!data.stop_file.empty() && polls % 200 == 0 && fs::is_file(data.stop_file))
			{
				data.session.pending_end = user_stopped;
				break;
			}
			if (pbd_emulation_stopping(ppu))
				return replay_control_result::stopped;
			if (data.takeover.phase != pbd::takeover_phase::end_prompt && data.takeover.phase != pbd::takeover_phase::enable_prompt && data.replay.test.resume_at != session_clock::time_point{} && session_clock::now() >= data.replay.test.resume_at)
			{
				data.replay.test.resume_at = {};
				set_pause(false);
				break;
			}
			if (data.takeover.phase != pbd::takeover_phase::end_prompt && data.takeover.phase != pbd::takeover_phase::enable_prompt && data.replay.test.step_resume && polls % 10 == 9)
			{
				if (data.replay.test.steps)
				{
					data.replay.test.steps--;
					data.replay.step = true;
					break;
				}
				data.replay.test.step_resume = false;
				set_pause(false);
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			read_pad();
			if (data.session.pending_end == guard_failed)
				return replay_control_result::stopped;
			if (data.takeover.redispatch)
				return replay_control_result::restored;
			publish();
		}
		if (data.replay.step || data.replay.paused || data.session.pending_end != collecting || data.replay.speed >= 0)
			break;
		// Slowed: held until this boundary is due (2 or 4 frame periods after the previous one, less a margin for the vblank wait).
		const auto due = data.replay.last_release + std::chrono::microseconds((frame_period_us << -data.replay.speed) - 4000);
		while (session_clock::now() < due && !data.replay.paused && data.replay.speed < 0 && data.session.pending_end == collecting)
		{
			hold();
			if (pbd_emulation_stopping(ppu))
				return replay_control_result::stopped;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			read_pad();
			if (data.session.pending_end == guard_failed)
				return replay_control_result::stopped;
			if (data.takeover.redispatch)
				return replay_control_result::restored;
			publish();
		}
		if (!data.replay.paused)
			break;
	}

	return replay_control_result::go;
}

bool pbd::rollback_session::replay_steps::scripted_playback_action(const replay_test_action& a)
{
	if (a.kind == 'p')
	{
		set_pause(true);
		data.replay.test.resume_at = session_clock::now() + std::chrono::milliseconds(std::max(a.value, 0));
	}
	else if (a.kind == 's')
	{
		set_pause(true);
		data.replay.test.steps = static_cast<u32>(std::max(a.value, 0));
		data.replay.test.step_resume = true;
	}
	else if (a.kind == 'v')
		set_speed(a.value);
	else if (a.kind == 'w')
	{
		data.replay.test.pad_word = static_cast<u16>(a.value);
		read_pad();
	}
	else if (a.kind == 't')
	{
		take(static_cast<u32>(a.value));
		data.replay.test.word = a.word;
		data.replay.test.word_set = a.value != 0;
	}

	else
		return false;
	return true;
}
