#pragma once

struct pbd::rollback_session::replay_pad_steps : replay_steps
{
	const std::array<u16, 2>& pads;
	const u16 pad, pressed;
	const bool viewer;
	static constexpr auto buttons = pbd::replay_controls::pbd_buttons;
	replay_pad_steps(replay_steps& steps, const std::array<u16, 2>& words, u16 word, u16 edges, bool has_viewer)
		: replay_steps(steps), pads(words), pad(word), pressed(edges), viewer(has_viewer) {}
	void run()
	{
		if (release_barrier())
			return;
		choose_prompt_side();
		if (direct_attempt())
			return;
		if (watch_controls())
			return;
		if (plain_takeover())
			return;
		if (viewer)
			viewer_controls();
		else if (legacy_controls())
			return;
		publish_leave_status();
	}

	bool release_barrier()
	{
		if (!viewer && data.lobby.direct_match.enabled() && data.takeover.ignore_pad_until_release)
		{
			data.replay.pad_prev = pad;
			data.replay.pad = 0;
			if (!pad)
				data.takeover.ignore_pad_until_release = false;
			return true;
		}
		return false;
	}

	void choose_prompt_side()
	{
		if (!viewer && data.replay.takeover && data.takeover.phase == pbd::takeover_phase::enable_prompt)
		{
			const u32 side = pbd::replay_controls::pbd_prompt_side(pad, data.replay.pad_prev, data.takeover.port);
			if (side != data.takeover.port)
			{
				data.takeover.port = side;
				log_event("takeover_confirm", static_cast<s32>(side));
			}
		}
	}

	bool direct_attempt()
	{
		if (!viewer && data.lobby.direct_match.enabled() && data.replay.takeover &&
			(data.takeover.phase == pbd::takeover_phase::controlling || data.takeover.phase == pbd::takeover_phase::end_prompt || data.takeover.phase == pbd::takeover_phase::enable_prompt))
		{
			if (data.takeover.phase == pbd::takeover_phase::controlling && !data.replay.paused &&
				pbd::replay_viewer::takeover_finished(index, data.takeover.deadline, fighting_now,
					data.last_fight_index, data.takeover.start))
				show_prompt(pad);
			using command = pbd::replay_controls::pbd_command;
			const auto action = pbd::replay_controls::pbd_attempt_command(pad, data.replay.pad_prev);
			if (direct_prompt(action))
				return true;
			switch (action)
			{
			case command::retry:
				if (data.takeover.phase != pbd::takeover_phase::enable_prompt)
					try_again();
				break;
			case command::return_to_replay:
				if (data.takeover.phase != pbd::takeover_phase::enable_prompt)
					return_to_replay();
				break;
			case command::pause:
				if (data.takeover.phase == pbd::takeover_phase::controlling)
					set_pause(!data.replay.paused);
				break;
			case command::leave:
				if (data.session.pending_end == collecting)
				{
					data.session.pending_end = user_stopped;
					log_event("leave", 0);
				}
				break;
			default: break;
			}
			data.replay.pad_prev = pad;
			data.replay.pad = data.takeover.phase == pbd::takeover_phase::controlling ?
			                      pbd::replay_controls::pbd_taken_word(pad, pads,
									  pbd::replay_controls::pbd_taken_input_port(data.replay.control_port, data.replay.input_port)) :
			                      0;
			return true;
		}
		return false;
	}

	bool direct_prompt(pbd::replay_controls::pbd_command action)
	{
		using command = pbd::replay_controls::pbd_command;
		if ((data.takeover.phase == pbd::takeover_phase::end_prompt || data.takeover.phase == pbd::takeover_phase::enable_prompt) && !(pad & buttons.select))
		{
			const u16 choices = data.takeover.phase == pbd::takeover_phase::enable_prompt ? 0x1000 | 0x4000 :
			                                                                                buttons.step_forward | buttons.step_back | 0x1000 | 0x4000;
			if (pressed & choices)
			{
				data.takeover.choice ^= 1;
				log_event("takeover_choice", static_cast<s32>(data.takeover.choice));
			}
			if (pressed & (0x0040 | buttons.start))
			{
				if (data.takeover.phase == pbd::takeover_phase::enable_prompt)
				{
					if (data.takeover.choice == 0)
						start_takeover(data.takeover.port);
					else
						cancel_arm_prompt();
				}
				else if (data.takeover.choice == 0)
					try_again();
				else
					return_to_replay();
				data.takeover.ignore_pad_until_release = true;
			}
			else if ((data.takeover.phase == pbd::takeover_phase::enable_prompt && (pressed & 0x0020)) ||
					 (data.takeover.phase == pbd::takeover_phase::enable_prompt && action == command::leave))
			{
				cancel_arm_prompt();
				data.takeover.ignore_pad_until_release = true;
			}
			data.replay.pad_prev = pad;
			data.replay.pad = 0;
			return true;
		}
		return false;
	}

	bool watch_controls()
	{
		const bool watching = data.takeover.phase == pbd::takeover_phase::watching && data.replay.control_port == 0;
		if (!viewer && (watching || (!data.replay.takeover && (pressed & buttons.select))))
		{
			using command = pbd::replay_controls::pbd_command;
			const auto action = pbd::replay_controls::pbd_watch_command(pad, data.replay.pad_prev,
				data.replay.paused, fighting_now, data.session.in_match, true);
			switch (action)
			{
			case command::leave:
				if (data.session.pending_end == collecting)
				{
					data.session.pending_end = user_stopped;
					log_event("leave", 0);
				}
				break;
			case command::pause: set_pause(!data.replay.paused); break;
			case command::step: data.replay.step = true; break;
			case command::slower: set_speed(data.replay.speed - 1); break;
			case command::faster: set_speed(data.replay.speed + 1); break;
			// Advance the original inputs to the next fight; no round reconstruction is needed.
			case command::skip_scene: next_round(); break;
			case command::take:
			{
				data.takeover.null_input = false;
				const char* preferred = pbd::env::find("PBD_REPLAY_DEFAULT_SIDE");
				const u32 default_side = preferred && std::string_view(preferred) == "2" ? 2 :
				                         preferred && std::string_view(preferred) == "1" ? 1 :
				                                                                           0;
				const u32 side = pbd::replay_controls::pbd_take_side(pads, data.replay.input_port, default_side);
				if (data.replay.takeover)
					show_arm_prompt(side, data.replay.paused);
				else
					log_event("take_refused", static_cast<s32>(side));
				data.takeover.ignore_pad_until_release = data.takeover.phase == pbd::takeover_phase::enable_prompt || data.replay.control_port != 0;
				break;
			}
			case command::none:
			case command::retry:
			case command::return_to_replay: break;
			}
			data.replay.pad_prev = pad;
			data.replay.pad = 0;
			return true;
		}
		return false;
	}

	bool plain_takeover()
	{
		if (!viewer && data.replay.control_port && data.takeover.phase == pbd::takeover_phase::watching)
		{
			// A plain takeover lasts through this game. Shoulders and directions now belong to the selected fighter.
			if (data.takeover.ignore_pad_until_release)
			{
				data.replay.pad_prev = pad;
				data.replay.pad = 0;
				if (!pad)
					data.takeover.ignore_pad_until_release = false;
				return true;
			}
			if (pbd::replay_controls::pbd_taken_command(pad, data.replay.pad_prev) == pbd::replay_controls::pbd_command::pause)
				set_pause(!data.replay.paused);
			data.replay.pad_prev = pad;
			data.replay.pad = pbd::controllers::settings::remap_attacks(pbd::replay_controls::pbd_taken_word(pad, pads,
				pbd::replay_controls::pbd_taken_input_port(data.replay.control_port, data.replay.input_port)));
			return true;
		}
		return false;
	}

	void viewer_controls()
	{
		settings_pause();
		// Expiry owns this boundary before a Retry/Return/Start word can dispatch or restore.
		// This also runs on paused/slowed polls; no game sample or step at the expired index can escape.
		if (data.replay.takeover && pbd::replay_viewer::takeover_prompt_due(data.takeover.phase, index,
										data.takeover.deadline, fighting_now, data.last_fight_index, data.takeover.start))
			show_prompt(pad);
		viewer_action(pad);
		// The taken fighter reads the player's own controller: the public Watch keeps it on port 2 whichever side is taken.
		if (data.replay.control_port)
			data.replay.pad = pbd::controllers::settings::remap_attacks(pbd::replay_controls::pbd_taken_word(data.replay.pad, pads,
				pbd::replay_controls::pbd_taken_input_port(data.replay.control_port, data.replay.input_port)));
	}

	bool legacy_controls()
	{
		if (data.takeover.ignore_pad_until_release)
		{
			data.replay.pad_prev = pad;
			data.replay.pad = 0;
			if (!pad)
				data.takeover.ignore_pad_until_release = false;
			return true;
		}
		const u16 pressed = static_cast<u16>(pad & ~data.replay.pad_prev);
		const bool select = (pad & session_select_bit) != 0, start = (pad & session_start_bit) != 0;
		if (start && !(data.replay.pad_prev & session_start_bit))
			data.replay.start_clean = !select;
		else if (start && select)
			data.replay.start_clean = false;
		if (!start && (data.replay.pad_prev & session_start_bit) && data.replay.start_clean &&
			data.takeover.phase != pbd::takeover_phase::end_prompt && data.takeover.phase != pbd::takeover_phase::enable_prompt)
			set_pause(!data.replay.paused);
		if (data.replay.takeover)
			legacy_takeover(pressed, select);
		else if (select)
		{
			if (pressed & buttons.step_back)
				take(1); // LEFT
			if (pressed & buttons.step_forward)
				take(2); // RIGHT
			if (pressed & 0x4000)
				take(0); // DOWN
		}
		else if (pressed & buttons.faster) // R1
		{
			if (data.replay.paused)
				data.replay.step = true;
			else
				set_speed(data.replay.speed + 1);
		}
		else if ((pressed & buttons.slower) && !data.replay.paused)
			set_speed(data.replay.speed - 1); // L1
		return false;
	}

	void legacy_takeover(u16 pressed, bool select)
	{
		if (data.replay.takeover)
		{
			// Once armed, the first ordinary fight button or direction belongs to the game and defines the exact Retry anchor.
			// START, SELECT and the stick clicks retain their replay UI roles and cannot accidentally start control.
			constexpr u16 player_input_bits = 0xf0ff;
			if (data.takeover.phase == pbd::takeover_phase::armed)
			{
				if (data.session.in_match && (pressed & player_input_bits))
					start_takeover(data.takeover.port);
			}
			// The replay buttons. In control, every other button belongs to the game (see the taken side's input).
			else if (data.takeover.phase == pbd::takeover_phase::controlling)
			{
				if (select && (pressed & 0x0040))
					try_again(); // SELECT + CROSS
				else if (select && (pressed & 0x0020))
					return_to_replay(); // SELECT + CIRCLE
			}
			else if (data.takeover.phase == pbd::takeover_phase::end_prompt || data.takeover.phase == pbd::takeover_phase::enable_prompt)
				legacy_prompt(pressed);
			else if (!select)
			{
				if (pressed & 0x0200)
				{
					data.takeover.null_input = false;
					show_arm_prompt(1, data.replay.paused);
				} // L3
				else if (pressed & 0x0400)
				{
					data.takeover.null_input = false;
					show_arm_prompt(2, data.replay.paused);
				} // R3
				else if (pressed & 0x0004)
					previous_round(); // L1
				else if (pressed & 0x0008)
					next_round(); // R1
				else if (pressed & 0x0002)
				{
					if (data.replay.paused)
						data.replay.step = true;
					else
						set_speed(data.replay.speed + 1);
				} // R2
				else if ((pressed & 0x0001) && !data.replay.paused)
					set_speed(data.replay.speed - 1); // L2
			}
		}
	}

	void legacy_prompt(u16 pressed)
	{
		if (data.takeover.phase == pbd::takeover_phase::end_prompt)
		{
			if (pressed & (0x1000 | 0x4000))
				data.takeover.choice ^= 1; // UP / DOWN
			if (pressed & 0x0040)
			{
				if (data.takeover.choice == 0)
					try_again();
				else
					return_to_replay();
			} // CROSS
			else if (pressed & 0x0020)
				return_to_replay(); // CIRCLE
		}
		else if (data.takeover.phase == pbd::takeover_phase::enable_prompt)
		{
			if (pressed & (0x1000 | 0x4000))
				data.takeover.choice ^= 1; // UP / DOWN
			if (pressed & 0x0040)
			{
				if (data.takeover.choice == 0)
					arm_takeover(data.takeover.port);
				else
					cancel_arm_prompt();
			} // CROSS
			else if (pressed & 0x0020)
				cancel_arm_prompt(); // CIRCLE
		}
	}

	void publish_leave_status()
	{
		const auto now = session_clock::now();
		const bool select = (pad & session_select_bit) != 0, start = (pad & session_start_bit) != 0;
		if (select && start)
		{
			if (!data.replay.leave_held)
				data.replay.leave_from = now;
			data.replay.leave_held = true;
		}
		else
			data.replay.leave_held = false;
		const u32 hold_ms = data.replay.leave_held ? static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(now - data.replay.leave_from).count()) : 0;
		if (hold_ms >= pbd::live_stats::leave_hold_total_ms && data.session.pending_end == collecting)
		{
			data.session.pending_end = user_stopped;
			log_event("leave", 0);
			pbd_session_log.warning("Replay: START + SELECT held at boundary %u: the player leaves the replay.", index);
		}
		if (data.live_stats)
			data.live_pub.leave(select, std::min(hold_ms, pbd::live_stats::leave_hold_total_ms));
		if (!viewer)
		{
			data.replay.pad_prev = pad;
			data.replay.pad = data.replay.control_port ?
			                      pbd::controllers::settings::remap_attacks(pbd::replay_controls::pbd_taken_word(pad, pads, data.replay.control_port)) :
			                      pad;
		}
	}
};
