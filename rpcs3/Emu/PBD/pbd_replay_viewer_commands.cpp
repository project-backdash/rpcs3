#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_replay_control_buttons.h"
#include "Emu/PBD/pbd_replay_rewind.h"
#include "Emu/PBD/pbd_controller_display.h"
#include "Emu/PBD/pbd_controller_settings.h"
#include "Emu/RSX/Overlays/overlay_message.h"
#include "Emu/PBD/pbd_json_reader.h"
#include <fstream>

void pbd::rollback_session::replay_steps::viewer_action(u16 pad)
{
	if (data.replay.viewer_bookmarked == 1)
	{
		std::ifstream reply(data.replay.bookmark_reply, std::ios::binary);
		std::array<char, 257> bytes{};
		reply.read(bytes.data(), bytes.size());
		const auto size = reply.gcount();
		if (size > 0 && size < static_cast<std::streamsize>(bytes.size()))
		{
			const auto parsed = pbd::json::detail::json_parser(std::string_view(bytes.data(), static_cast<usz>(size)), 8).parse();
			u32 request = 0;
			if (parsed.value && pbd::json::detail::number32(parsed.value->find("requestId"), 1, UINT32_MAX, request) && request == data.replay.bookmark_request)
			{
				if (const auto* ok = parsed.value->find("ok"); ok && ok->kind == pbd::json::json_kind::boolean)
					data.replay.viewer_bookmarked = ok->truth ? 2 : 3;
			}
		}
		if (data.replay.viewer_bookmarked == 1 && now_ms() - data.replay.bookmark_requested_at > 10000)
			data.replay.viewer_bookmarked = 3;
	}
	if (data.session.in_match && data.lobby.direct_match.enabled() && !data.lobby.direct_match.pbd_live_inputs())
		pbd::replay_viewer::include_recorded_rounds(data.replay.viewer_rounds, data.session.matches,
			[&](u32 round)
			{
				return data.lobby.direct_match.pbd_round_frame(round);
			});
	// Between rounds, focus the last round fought in this match.
	u32 round_count = 0, round_focus = 0;
	for (const auto& r : data.replay.viewer_rounds)
	{
		if (r.match == data.session.matches)
		{
			if (r.round <= std::max<u32>(data.replay.round, 1))
				round_focus = round_count;
			++round_count;
		}
	}
	const pbd::replay_viewer::context context{data.replay.paused, data.session.in_match, data.replay.speed, data.takeover.phase,
		round_count, fighting_now, now_ms(), round_focus, pbd::controllers::settings::clicks_with_actions()};
	const auto pointer = pbd::replay_viewer::pointer_ui.take(data.replay.viewer.menu, data.takeover.phase, data.session.matches);
	// A click owns this host word before a mapped mouse button could start a taken fighter or confirm a new prompt.
	const auto action = pointer == pbd::replay_viewer::pointer_none ? data.replay.viewer.poll(pad, context) :
	                                                                  data.replay.viewer.pointer(pointer, context, pad);
	if (pointer != pbd::replay_viewer::pointer_none)
		data.replay.viewer.barrier(pad);
	data.replay.pad = action.game; // Only the returned game word can reach the taken fighter.
	data.replay.pad_prev = pad;
	using command = pbd::replay_viewer::command;
	if (action.resume && data.replay.paused)
		set_pause(false);
	switch (action.kind)
	{
	case command::exit:
		data.session.pending_end = user_stopped;
		set_pause(false);
		log_event("exit_menu", 0);
		break;
	case command::leave:
		// Return to Launcher / Exit Game: asked of the launcher through the game window's pause menu, which ends this run as it
		// does for its own rows. Where the launcher cannot be asked that way, the replay ends as the hold-to-leave chord ends it.
		if (pbd::settings_menu::request_leave(action.value == 2 ? pbd::settings_menu::entry::quit : pbd::settings_menu::entry::launcher))
		{
			log_event("leave_menu", action.value);
			break;
		}
		data.session.pending_end = user_stopped;
		set_pause(false);
		log_event("exit_menu", action.value);
		break;
	case command::pause: set_pause(!data.replay.paused); break;
	case command::step:
		set_pause(true);
		data.replay.step = true;
		break;
	case command::speed: set_speed(action.value); break;
	case command::step_back: seek_by(-1); break;
	case command::seek_back: seek_by(-static_cast<s32>(pbd::replay_rewind::step_frames)); break;
	case command::seek_forward: seek_by(static_cast<s32>(pbd::replay_rewind::step_frames)); break;
	case command::previous_round: go_to_round(pbd::replay_viewer::previous_round_target(data.replay.round, fighting_now)); break;
	case command::next_round: go_to_round(data.replay.round + 1); break;
	case command::round:
	{
		if (action.value < 0)
			break;
		u32 index = 0;
		for (const auto& r : data.replay.viewer_rounds)
		{
			if (r.match == data.session.matches && index++ == static_cast<u32>(action.value))
			{
				go_to_round(r.round);
				break;
			}
		}
		break;
	}
	case command::take_over: take_over(static_cast<u32>(action.value)); break;
	case command::arm:
		if (data.lobby.direct_match.enabled())
			start_takeover(data.takeover.port);
		else
			arm_takeover(data.takeover.port);
		break;
	case command::cancel_arm: cancel_arm_prompt(); break;
	case command::start_take: start_takeover(data.takeover.port); break;
	case command::retry: try_again(); break;
	case command::return_replay: return_to_replay(); break;
	case command::bookmark: bookmark_situation(); break;
	case command::none: break;
	}
	data.takeover.choice = data.replay.viewer.take_choice;
}

void pbd::rollback_session::replay_steps::bookmark_situation()
{
	if (data.replay.viewer_bookmarked == 1 || data.replay.viewer_bookmarked == 2)
		return;
	const auto request = ++data.replay.bookmark_request;
	const auto text = fmt::format("{\"type\":\"save_situation\",\"requestId\":%u,\"match\":%u,\"frame\":%u,\"side\":%u,\"seconds\":10,\"practice_ready\":false}\n",
		request, data.takeover.match, data.takeover.start_frame, data.takeover.port);
	data.replay.bookmark_requested_at = now_ms();
	data.replay.viewer_bookmarked = data.replay.control_file.write(text.data(), text.size()) == text.size() ? 1 : 3;
	if (data.replay.viewer_event_file)
	{
		const auto event = fmt::format("{\"type\":\"replay_situation\",\"schema\":1,\"match\":%u,\"frame\":%u,\"side\":%u,\"seconds\":10,\"practice_ready\":false}\n",
			data.takeover.match, data.takeover.start_frame, data.takeover.port);
		if (data.replay.viewer_event_file.size() + event.size() <= 65536)
			data.replay.viewer_event_file.write(event.data(), event.size());
	}
}
