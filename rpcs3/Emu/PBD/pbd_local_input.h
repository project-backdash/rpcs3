#pragma once

struct pbd::rollback_session::local_input_steps : boundary_call
{
	u16 raw;
	const u32 source;
	const u16 physical;
	bool selector_runs = false, selector_owns = false, selector_hidden = false;

	u16 run()
	{
		lobby_selection_audio();
		// Taken before the selector's sample, which ends the selection on its last (neutral) word. A repick's selector also runs
		// on the chooser's PC, for the stage and the battle readback, but that player's own word passes through it.
		selector_runs = data.lobby.selection && data.lobby.selection->active();
		selector_owns = data.lobby.selection && data.lobby.selection->owns_input();
		selector_hidden = data.lobby.selection && data.lobby.selection->hidden();
		if (!steer_selection())
			return 0;
		const u16 before = raw;
		filter_leave_chord();
		if (data.session.enabled && data.session.mask_start)
		{
			raw &= ~u16{session_start_bit}; // digital1 Start, in the game's canonical 16-bit word
			if ((before & session_start_bit) && source < data.extent() && !data.history.session_start_masked_at[source])
			{
				data.history.session_start_masked_at[source] = 1;
				data.session.start_masked++;
				const auto text = fmt::format("{\"type\":\"start_masked\",\"boundary\":%u,\"before\":%u,\"value\":%u,\"role\":%u}\n", source, before, raw, data.peer.role);
				data.peer.file.write(text.data(), text.size());
			}
		}
		// While the shared panel holds this controller (shown, or the press that closed it not yet released), the sample is
		// neutral here, before it is frozen, and the port hook executes the frozen value unchanged, so both games execute the same
		// word (pbd::game_overlay::read_claim). Every peer session: an in-game lobby set (a repick's steered words included), a
		// fixed set once its hidden selection has ended (the selector owns the sample before that), and any other session given
		// the panel. Blanking only the executed local port instead would let the other game run the held press: in the select, a
		// fighter picked or a cursor moved on one PC only; in a fight, the first frames played on one PC only. After the hold, a
		// button it saw down stays out until this sample stream itself shows it released, since the host read that ended the hold
		// is newer than this sample. A steered word blanked here is a press that comes later: the selector steers from what the
		// game shows at each sample.
		// The in-game Settings menu holds this controller the same way, so a press made in it is no fighter's on either PC.
		if (data.peer.enabled)
		{
			const auto admitted = pbd::game_overlay::admit_local_sample(raw, physical, data.game_overlay.claim,
				data.game_overlay.ignore_until_release, selector_hidden, data.game_overlay.hold_buttons, pbd::settings_menu::claimed());
			raw = admitted.word;
			data.game_overlay.hold_buttons = admitted.buttons;
		}
		// In-game lobby selection: the handicap confirmed at full and, at a countdown's zero, the local player's own press. Both
		// change only this PC's own sample here, after the leave chord has seen the physical buttons, so the value is frozen
		// before it is queued, sent and recorded, and the other PC executes it like any press. The timer never runs in a
		// re-execution.
		if (!sample_selection_timer())
			return 0;
		return raw;
	}

	bool steer_selection()
	{
		if (selector_runs)
		{
			const auto observed = pbd::hidden_selection::read_native_observation(data.peer.role);
			std::optional<pbd::hidden_selection::tables> live;
			if (selector_owns && observed && observed->scene == 3 && observed->state == 3 && observed->cursor == data.lobby.selection->target_cursor())
				live = pbd::hidden_selection::read_native_tables();
			if (!observed)
			{
				data.lobby.set->refuse("selection_state_unreadable", "The game selection state could not be verified.");
				session_runtime.finish(guard_failed);
				return false;
			}
			raw = data.lobby.selection->sample(source, *observed, raw, live ? &*live : nullptr);
			if (!data.lobby.selection->healthy())
			{
				data.lobby.set->refuse(data.lobby.selection->error(), fmt::format(
																		  "Native selection refusal: scene %u/%u, state %u, cursor %u, pad role %u, source %u.",
																		  observed->scene, observed->subscene, observed->state, observed->cursor, observed->pad_role, source));
				session_runtime.finish(guard_failed);
				return false;
			}
			// The loading card that covered a hidden selection gives way to the panel; a repick never showed one.
			if (selector_hidden && data.lobby.selection->complete())
				pbd::game_overlay::publish(data.game_overlay.panel);
		}
		else
			raw = pbd::fn_hook::steer_stage_sample(data.peer.role, source, raw);

		return true;
	}

	void filter_leave_chord()
	{
		if (data.session.enabled)
		{
			// Select never reaches the game in a session: Start + Select quits a match to the main menu, where the session's
			// neutral inputs leave both games on the title screen. Held together for session_leave_hold_ticks samples they end
			// the session instead (at the next completed row, the same end as the stop file). The combination's Start is
			// removed too, and stays removed until Start is released, so letting go of Select first never becomes a new Start
			// press (a Start pressed before Select still reaches the game, as any single Start does). The value is frozen here,
			// before it is queued, sent or recorded, so both peers execute the same masked input. A repick's select is open on both
			// PCs, but its selector replaces the word wherever it steers or keeps the sample neutral (the kept player's whole select,
			// the stage, the VS load), so the chord reads the player's own buttons while it runs, as in any in-game select.
			const u16 chord = selector_runs && !selector_hidden ? physical : raw;
			const bool select = (chord & session_select_bit) != 0;
			const bool start = (chord & session_start_bit) != 0;
			const bool combo = select && start;
			if (source >= data.session.leave_next_source)
			{
				data.session.leave_next_source = source + 1;
				data.session.leave_start_held = start && (combo || data.session.leave_start_held);
				data.session.leave_hold = combo ? data.session.leave_hold + 1 : 0;
				if (select)
					data.session.selects_masked++;
				if (data.session.leave_hold == session_leave_hold_ticks && data.session.pending_end == collecting)
				{
					data.session.pending_end = user_stopped;
					pbd_session_log.warning("Start + Select held for %u samples through %u: the player leaves the session.", session_leave_hold_ticks, source);
					if (data.peer.file)
					{
						const auto text = fmt::format("{\"type\":\"leave_hold\",\"boundary\":%u,\"samples\":%u,\"role\":%u}\n", source, session_leave_hold_ticks, data.peer.role);
						data.peer.file.write(text.data(), text.size());
					}
				}
				if (data.live_stats)
					data.live_pub.leave(select, std::min<u32>(data.session.leave_hold, session_leave_hold_ticks) * 1000 / 60);
			}
			// Each value is admitted once per sample and frozen, so the latch state read here is the one this sample set.
			if (select)
				raw &= static_cast<u16>(~session_select_bit);
			if (combo || (start && data.session.leave_start_held))
				raw &= static_cast<u16>(~session_start_bit);
		}
	}

	bool sample_selection_timer()
	{
		if (data.lobby.select_timer && data.lobby.select_timer->selecting())
		{
			auto& select_clock = *data.lobby.select_timer;
			const auto select_now = select_view();
			const bool had_character = select_clock.character_deadline().has_value(), had_stage = select_clock.stage_deadline().has_value();
			raw = select_clock.sample(source, select_now, raw);
			if (const char* refusal = select_clock.refusal())
			{
				pbd::select_band::hide(data.generation);
				data.lobby.set->refuse(refusal, fmt::format("Native selection refusal: scene %u, states %u/%u, handicap %u/%u, source %u.",
													select_now.scene, select_now.state[0], select_now.state[1], select_now.handicap[0], select_now.handicap[1], source));
				session_runtime.finish(guard_failed);
				return false;
			}
			// The countdown band, composed from the same boundary and view as the word above and drawn by the RSX overlay. Display
			// only: it is published after the word is decided and never feeds back into it. A repeated source redraws nothing; the
			// first battle frame (the selection ends) and every scene outside the select publish a hidden band.
			if (!data.lobby.select_band_source || source > *data.lobby.select_band_source)
			{
				data.lobby.select_band_source = source;
				const auto& identity = data.lobby.channel->identity();
				const auto& opponent = identity.local_side == pbd::set_channel::side::left ? identity.right : identity.left;
				pbd::select_band::publish(data.generation,
					pbd::select_band::compose(select_clock.status(source, select_now), select_now, data.peer.role, opponent.name));
			}
			if (!had_character && select_clock.character_deadline())
				pbd_session_log.notice("Lobby set: character select from boundary %u; its countdown ends at %u.", source, *select_clock.character_deadline());
			if (!had_stage && select_clock.stage_deadline())
			{
				pbd_session_log.notice("Lobby set: stage select (role %u picks) from boundary %u; its countdown ends at %u.",
					select_now.stage_owner, source, *select_clock.stage_deadline());
				if (select_clock.random_stage() && select_clock.stage_stop() != select_clock.stage_deadline())
					pbd_session_log.notice("Lobby set: random stage: this PC's roulette stop press starts at boundary %u.", *select_clock.stage_stop());
			}
			if (!select_clock.selecting())
				pbd_session_log.notice("Lobby set: the selection ended at boundary %u.", source);
		}
		return true;
	}
};
