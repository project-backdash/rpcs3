// Replay controls at the boundary: pause, frame step, speed, the viewer's round and side choices and the controller
// taking a side.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_replay_control_buttons.h"
#include "Emu/PBD/pbd_replay_rewind.h"
#include "Emu/PBD/pbd_controller_display.h"
#include "Emu/PBD/pbd_controller_settings.h"
#include "Emu/RSX/Overlays/overlay_message.h"

// The replay keys of the focused game window (pbd::replay_controls::pbd_keyboard) as one controller word.
u16 pbd::rollback_session::replay_steps::keyboard_word() noexcept
{
	if (pbd::controllers::read_display().active)
		return 0;
#ifdef _WIN32
	DWORD foreground = 0, focus = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
	GUITHREADINFO info{};
	info.cbSize = sizeof(info);
	if (GetGUIThreadInfo(0, &info) && info.hwndFocus)
		GetWindowThreadProcessId(info.hwndFocus, &focus);
	if (foreground != GetCurrentProcessId() && focus != GetCurrentProcessId())
		return 0;
	u16 word = 0;
	for (const auto& [key, button] : pbd::replay_controls::pbd_keyboard)
		if (GetAsyncKeyState(key) & 0x8000)
			word |= button;
	return word;
#else
	return 0;
#endif
}

u64 pbd::rollback_session::replay_steps::now_ms() noexcept
{
	return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch())
			.count());
}

// Replay controls (replay_state::controls) at the first dispatch of boundary index: read the controller, act on it, and hold
// here while paused or slowed.
replay_control_result pbd::rollback_session::boundary_call::replay_controls_at(u32 index)
{
	// Takeover (replay_state::takeover). fighting: the update before this boundary was a fight step.
	const bool fighting_now = data.session.in_match && index && data.last_fight_index + 1 == index;
	replay_steps steps{*this, index, fighting_now};
	steps.prepare_control_boundary();
	steps.read_pad();
	if (data.session.pending_end == guard_failed)
		return replay_control_result::stopped;
	if (data.takeover.redispatch)
		return replay_control_result::restored;
	steps.take_requested_moment();
	// Scripted actions (test only) due at this boundary.
	if (steps.scripted_actions())
		return replay_control_result::restored;
	if (data.session.pending_end == guard_failed)
		return replay_control_result::stopped;
	// The takeover's clock begins only when the armed player presses a game input. One in control ends when its frames run out or
	// takeover_grace frames after the round's last fight frame (a K.O. or time out), before the round transition.
	if (steps.finish_takeover_action())
		return replay_control_result::restored;
	if (data.session.pending_end == guard_failed)
		return replay_control_result::stopped;
	if (data.replay.takeover && data.takeover.phase == pbd::takeover_phase::controlling && index >= data.takeover.start &&
		(index - data.takeover.start) % 60 == 0)
		steps.log_event("attempt", static_cast<s32>(data.takeover.deadline > index ? data.takeover.deadline - index : 0));
	steps.publish();
	const auto release = steps.wait_control_release();
	if (release != replay_control_result::go)
		return release;
	if (data.replay.step)
	{
		data.replay.step = false;
		data.replay.steps++;
		steps.log_event("step", 0);
	}
	steps.publish();
	data.replay.last_release = session_clock::now();
	return replay_control_result::go;
}

void pbd::rollback_session::replay_steps::log_event(const char* kind, s32 value)
{
	const u32 remaining = data.takeover.phase == pbd::takeover_phase::controlling && data.takeover.deadline > index ? data.takeover.deadline - index : 0;
	const auto text = fmt::format("{\"type\":\"%s\",\"boundary\":%u,\"value\":%d,\"speed\":%d,\"paused\":%s,\"control\":%u,\"match\":%u,\"in_match\":%s,\"match_frame\":%u,\"takeover\":%u,\"frames_left\":%u,\"take_start\":%u,\"take_deadline\":%u,\"take_port\":%u,\"take_choice\":%u}\n",
		kind, index, value, data.replay.speed, json_bool(data.replay.paused), data.replay.control_port, data.session.matches,
		json_bool(data.session.in_match), match_frame(index), static_cast<u32>(data.takeover.phase), remaining, data.takeover.start, data.takeover.deadline,
		data.takeover.port, data.takeover.choice);
	data.replay.control_file.write(text.data(), text.size());
}

void pbd::rollback_session::replay_steps::apply_speed()
{
	const u32 rate = static_cast<u32>(data.replay.fast.saved_vblank) << std::max(data.replay.speed, 0);
	g_cfg.video.vblank_rate.set(rate);
	pbd::replay_pacing::vblank_request.store(rate, std::memory_order_release);
	pbd::replay_pacing::pbd_player_rate.store(data.replay.speed > 0 ? rate : 0, std::memory_order_release);
	pbd::player_volume::apply(data.replay.paused || data.replay.speed != 0);
}

void pbd::rollback_session::replay_steps::set_pause(bool paused)
{
	if (paused == data.replay.paused)
		return;
	data.replay.paused = paused;
	apply_speed();
	log_event(paused ? "pause" : "resume", 0);
}

void pbd::rollback_session::replay_steps::set_speed(s32 speed)
{
	speed = std::clamp(speed, -2, 2);
	if (speed == data.replay.speed)
		return;
	data.replay.speed = speed;
	apply_speed();
	log_event("speed", speed);
}

void pbd::rollback_session::replay_steps::take(u32 port)
{
	if (port == data.replay.control_port)
		return;
	if (data.lobby.direct_match.enabled() &&
		(!data.lobby.direct_match.begun() || (!data.takeover.null_input && !data.lobby.direct_match.set_continuation(data.generation,
																			   static_cast<pbd_replay_continuation>(port)))))
	{
		log_event("take_refused", static_cast<s32>(port));
		return;
	}
	data.replay.control_port = port;
	if (port)
	{
		data.replay.taken = true;
		set_speed(0);
		set_pause(false);
	}
	log_event("take", static_cast<s32>(port));
}

void pbd::rollback_session::replay_steps::publish()
{
	if (data.replay.viewer_enabled)
	{
		pbd::replay_viewer::snapshot view;
		view.enabled = 1;
		view.menu = static_cast<u32>(data.replay.viewer.menu);
		view.choice = data.replay.viewer.choice;
		view.paused = data.replay.paused;
		view.speed = data.replay.speed + 2;
		view.round = data.replay.round;
		view.match = data.session.matches;
		view.frame = match_frame(index);
		view.takeover = data.takeover.phase;
		view.take_choice = data.takeover.choice;
		view.port = data.takeover.port;
		view.frames_left = data.takeover.phase == pbd::takeover_phase::armed                                         ? data.takeover.frames :
		                   data.takeover.phase == pbd::takeover_phase::controlling && data.takeover.deadline > index ? data.takeover.deadline - index :
		                                                                                                               0;
		view.flags = pbd::replay_viewer::display_flags(data.replay.viewer, data.replay.was_fighting);
		view.fast = data.replay.fast.active;
		view.bookmarked = data.replay.viewer_bookmarked;
		u32 counter = 0;
		const bool counter_valid = fighting_now && read_word(0x1008f05c, counter);
		pbd::practice::native::frame_boundary(counter, counter_valid);
		if (fighting_now)
			pbd::practice::native::frame_feedback(view.fighters);
		for (const auto& r : data.replay.viewer_rounds)
			if (r.match == data.session.matches && view.count < view.rounds.size())
				view.rounds[view.count++] = r.round;
		for (usz side = 0; side < 2; side++)
			view.history[side] = data.replay.viewer_history[side].rows;
		pbd::replay_viewer::presentation.store(view);
	}
	if (!data.live_stats)
		return;
	const u32 frame = match_frame(index);
	const bool fighting = data.replay.was_fighting;
	data.live_pub.replay(data.replay.paused, data.replay.speed, data.replay.control_port, data.session.in_match ? data.session.matches : 0, frame,
		fighting ? data.replay.round : 0, fighting ? index - data.replay.round_start : 0);
	const u32 frames_left = data.takeover.phase == pbd::takeover_phase::armed                                         ? data.takeover.frames :
	                        data.takeover.phase == pbd::takeover_phase::controlling && data.takeover.deadline > index ? data.takeover.deadline - index :
	                                                                                                                    0;
	data.live_pub.takeover(data.replay.takeover, static_cast<u32>(data.takeover.phase), data.takeover.port, frames_left, data.takeover.choice);
}

void pbd::rollback_session::replay_steps::start_takeover(u32 port)
{
	if (!port || port > 2 || (data.lobby.direct_match.enabled() && !data.lobby.direct_match.begun()))
	{
		log_event("take_refused", static_cast<s32>(port));
		rsx::overlays::queue_message(std::string("Take Control is available once the replay reaches the fight."), 3'000'000);
		return;
	}
	data.replay.viewer_bookmarked = false;
	if (data.replay.takeover_fast && (!data.takeover.keyframe.valid || data.takeover.keyframe.index != index))
		request_takeover_capture(index);
	data.takeover.phase = pbd::takeover_phase::controlling;
	data.takeover.port = port;
	data.takeover.start = index;
	data.takeover.start_frame = match_frame(index);
	data.takeover.match = data.session.matches;
	data.takeover.deadline = index + data.takeover.frames;
	take(port);
	set_speed(0);
	set_pause(false);
	log_event("takeover_start", static_cast<s32>(port));
}

void pbd::rollback_session::replay_steps::arm_takeover(u32 port)
{
	data.takeover.phase = pbd::takeover_phase::armed;
	data.takeover.port = port;
	set_speed(0);
	set_pause(false);
	log_event("takeover_armed", static_cast<s32>(port));
}

void pbd::rollback_session::replay_steps::show_arm_prompt(u32 port, bool was_paused)
{
	data.replay.step = false;
	data.replay.test.resume_at = {};
	data.replay.test.step_resume = false;
	data.replay.test.steps = 0;
	data.takeover.confirm_was_paused = was_paused;
	data.takeover.phase = pbd::takeover_phase::enable_prompt;
	data.takeover.port = port;
	data.takeover.choice = 0;
	set_pause(true);
	log_event("takeover_confirm", static_cast<s32>(port));
}

void pbd::rollback_session::replay_steps::cancel_arm_prompt()
{
	const bool was_paused = data.takeover.confirm_was_paused;
	const u32 port = data.takeover.port;
	data.takeover.phase = pbd::takeover_phase::watching;
	data.takeover.port = 0;
	data.takeover.choice = 0;
	set_pause(was_paused);
	log_event("takeover_cancel", static_cast<s32>(port));
	log_event("take", 0);
}

void pbd::rollback_session::replay_steps::show_prompt(u16 held)
{
	data.takeover.phase = pbd::takeover_phase::end_prompt;
	// Time-up owns this boundary until Retry/Return; a queued test resume or step cannot release it.
	data.replay.step = false;
	data.replay.test.resume_at = {};
	data.replay.test.step_resume = false;
	data.replay.test.steps = 0;
	if (data.replay.viewer_enabled)
	{
		data.replay.viewer.menu = pbd::replay_viewer::page::none;
		data.replay.viewer.barrier(held);
		data.replay.step = false;
		data.replay.pad = 0;
	}
	data.takeover.choice = 0;
	set_pause(true);
	log_event("takeover_prompt", static_cast<s32>(data.takeover.port));
}

// Retry/Return attempts the pinned in-memory restore first. If it cannot restore, end this run so play-replay.ps1 can
// reconstruct (match, frame) from the replay's start with that side taken (0 = none).
void pbd::rollback_session::replay_steps::jump(const char* action, u32 match, u32 frame, u32 side)
{
	if (data.session.pending_end != collecting)
		return;
	if (!std::strcmp(action, "again") || !std::strcmp(action, "return"))
	{
		// Retry/Return discards automatic queues before restoring the earlier owner.
		data.replay.test.resume_at = {};
		data.replay.test.step_resume = false;
		data.replay.test.steps = 0;
	}
	if (data.replay.takeover_fast && (!std::strcmp(action, "again") || !std::strcmp(action, "return")))
	{
		if (restore_takeover(action, side, index))
			return;
		if (data.session.pending_end == guard_failed)
			return;
		if (!session_runtime.m_active.load())
			return;
		if (data.replay.viewer_enabled)
		{
			rsx::overlays::queue_message(std::string("This moment can no longer be restored. Exit to Replays and open the replay again."), 5'000'000);
			log_event("return_refused", static_cast<s32>(frame));
		}
	}
	const auto text = fmt::format("{\"type\":\"takeover_jump\",\"action\":\"%s\",\"match\":%u,\"frame\":%u,\"side\":%u,\"boundary\":%u,\"seconds\":%u}\n",
		action, match, frame, side, index, data.takeover.frames / 60);
	data.replay.control_file.write(text.data(), text.size());
	data.session.pending_end = takeover_relaunch;
	set_pause(false);
	pbd_session_log.warning("Replay: %s at boundary %u: the replay starts again at frame %u of match %u.", action, index, frame, match);
}

void pbd::rollback_session::replay_steps::try_again()
{
	jump("again", data.takeover.match, data.takeover.start_frame, data.takeover.port);
}

void pbd::rollback_session::replay_steps::return_to_replay()
{
	jump("return", data.takeover.match, data.takeover.start_frame, 0);
}

// L1: the round before the one being fought (between rounds, the one just fought) from its "Round n" intro; before any
// round of this match, the match's start.
void pbd::rollback_session::replay_steps::previous_round()
{
	if (!data.session.in_match)
		return;
	const u32 target = fighting_now ? (data.replay.round > 1 ? data.replay.round - 1 : 1) : std::max<u32>(data.replay.round, 1);
	if (data.rewind.enabled)
	{
		go_to_round(target);
		return;
	}
	u32 frame = 0;
	for (const auto& r : data.takeover.rounds)
	{
		if (r[0] == data.session.matches && r[1] == target && target > 1)
			frame = r[2] > 320 ? r[2] - 320 : 0;
	}
	jump("prev", data.session.matches, frame, 0);
}

// R1: fast forward (muted) until the next round's first fight frame.
void pbd::rollback_session::replay_steps::next_round()
{
	if (!data.session.in_match || data.replay.fast.active)
		return;
	data.takeover.next_round = data.replay.round + 1;
	data.takeover.next_match = data.session.matches;
	data.replay.speed = 0;
	set_pause(false);
	g_cfg.video.vblank_rate.set(data.replay.fast.rate);
	pbd::replay_pacing::vblank_request.store(data.replay.fast.rate, std::memory_order_release);
	pbd::player_volume::mute();
	data.replay.fast.active = true;
	log_event("next_round", static_cast<s32>(data.takeover.next_round));
}

#include "Emu/PBD/pbd_replay_pad_steps.h"

// Watch consumes both controllers and the focused keyboard once. A taken fighter receives only its own controller.
void pbd::rollback_session::replay_steps::read_pad()
{
	constexpr auto buttons = pbd::replay_controls::pbd_buttons;
	static_assert(buttons.start == session_start_bit && buttons.select == session_select_bit);
	// The in-game Settings menu holds this controller while it is open and until its buttons are released after it closes:
	// no replay control acts and a taken fighter receives a neutral word.
	const auto pads = pbd_replay_pad_words();
	const u16 pad = data.game_overlay.claim || pbd::settings_menu::claimed() ? 0 :
	                                                                           data.replay.test.pad_word.value_or(static_cast<u16>(pads[0] | pads[1] | keyboard_word()));
	const auto pressed = static_cast<u16>(pad & ~data.replay.pad_prev);
	// Every replay with the viewer, old or new, direct or session, runs the viewer's controls (pbd::replay_viewer) while
	// watching, in control and at the prompts. The bindings guarded by !viewer are those of a replay without the viewer.
	const bool viewer = data.replay.viewer_enabled;
	replay_pad_steps{*this, pads, pad, pressed, viewer}.run();
}
