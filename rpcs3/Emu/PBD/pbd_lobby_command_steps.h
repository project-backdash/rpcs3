#pragma once

struct pbd::rollback_session::lobby_command_steps : boundary_call
{
	const pbd::set_channel::command& command;
	const bool next_set;
	const pbd::set_channel::manifest& manifest;
	bool repick = false;
	u32 counter = 0;
	std::optional<pbd::lobby_set::sealed_game_result> sealed;
	std::optional<pbd::hidden_selection::tables> tables;
	lobby_command_steps(boundary_call& call, const pbd::set_channel::command& request, bool restart_set)
		: boundary_call(call), command(request), next_set(restart_set), manifest(data.lobby.channel->identity()) {}
	void run()
	{
		if (admit_continuation())
			return;
		if (prepare_selection())
			return;
		data.lobby.battle_seen = false;
		// The next game's notices: its cover up to its own VS frame, its own first character intro, a drawn game's replay.
		data.lobby.vs_seen = false;
		data.lobby.fight_seen = false;
		data.lobby.game_replay = command.replay;
		if (record_continuation_prefix())
			return;
		publish_through(data.next_index);
		if ((command.rematch && !repick) || data.lobby.preselect)
			pbd::game_overlay::publish(pbd::game_overlay::selection_card(data.game_overlay.panel));
		return;
	}

	bool admit_continuation()
	{
		sealed = data.lobby.set->sealed_game();
		const auto previous = data.lobby.set->rematch_state();
		repick = command.rematch && command.rematch->repick;
		// Only an in-game repick steers the kept fighter and stage.
		tables = repick ? pbd::hidden_selection::read_native_tables() : std::nullopt;
		if (sealed && sealed->end_boundary < data.next_index && data.tick_at(sealed->end_boundary).read)
			counter = data.tick_at(sealed->end_boundary).frame_counter;
		else if (!read_word(0x1008f07c, counter))
		{
			session_runtime.finish(guard_failed);
			return true;
		}
		if (!sealed || sealed->terminal != next_set || (repick && !tables) ||
			(command.rematch && (!previous || !data.lobby.rematch.arm(*command.rematch, *previous,
												  command.replay, data.generation, data.next_index, next_set))))
		{
			session_runtime.finish(guard_failed);
			return true;
		}
#ifdef _WIN32
		if (next_set && data.peer.link.key_pipe)
		{
			std::lock_guard lock(data.peer.rx.mutex);
			if (!pbd::peer_key_pipe::install(data.peer.link.auth, *data.peer.link.key_pipe, data.peer.role, data.peer.session, command.next_set_id))
			{
				data.lobby.set->refuse("peer_auth_key_unavailable", "The next set's private authentication is unavailable.");
				session_runtime.finish(guard_failed);
				return true;
			}
			data.peer.link.auth_set_id = command.next_set_id;
			data.peer.rx.queue.clear();
		}
#endif
		// The game's own Ranked screens in the game this command starts, planned before the driver writes its start.
		const auto rank_screens = plan_rank_screens(command, next_set);
		if (!(next_set ? data.lobby.set->next_set(command, sealed->end_boundary, counter) :
						 data.lobby.set->continue_game(command, sealed->end_boundary, counter)))
		{
			session_runtime.finish(guard_failed);
			return true;
		}
		if (next_set)
		{
			// The stakes and placement counts belonged to the finished set; the launcher sends the new set's after this
			// acknowledgement.
			data.lobby.rank_stakes.reset();
			data.lobby.agreed_stakes.reset();
			data.lobby.placements = {};
			data.peer.link.terminal_idle.store(false, std::memory_order_release);
			pbd_session_log.notice("Lobby set: the next set %s (first to %u) starts at game 1 from boundary %u.", manifest.set_id,
				manifest.first_to_games, sealed->end_boundary);
		}
		arm_rank_screens(rank_screens);

		return false;
	}

	bool prepare_selection()
	{
		if (repick)
		{
			if (prepare_repick())
				return true;
		}
		else if (command.rematch)
		{
			data.lobby.selection.reset();
			data.lobby.select_timer.reset();
			// The command can arrive after the scene-1 barrier, while this first update is held for the lobby.
			// Arm its exit gate before resuming; waiting for another scene change would miss the whole loader.
			if (data.session.enabled && data.peer.enabled && data.session.gate.enabled && data.session.segments.scene == 1u &&
				data.session.gate.seg != data.session.segments.number)
			{
				data.session.gate.seg = data.session.segments.number;
				data.session.gate.ready = 0;
				data.session.gate.opened = false;
				pbd::fn_hook::gate_ready_tick.store(0);
				pbd::fn_hook::gate_hold.store(1);
				pbd::fn_hook::rematch_vs_init_gate.store(true, std::memory_order_release);
			}
		}
		// Older commands retain their original selection contract; a resolved load never enters its countdowns. The stock
		// select after a plain Rematch (which dropped the timer) arms a fresh one.
		else if (!pbd::select_timer::arm_select(data.lobby.select_timer, manifest.selection.character_seconds, manifest.selection.stage_seconds,
					 data.peer.role, command.random_stage, command.random_stage ? host_random_word() : 0))
		{
			data.lobby.set->refuse("selection_timer_unavailable", "The in-game selection countdowns could not be armed.");
			session_runtime.finish(guard_failed);
			return true;
		}
		else if (command.random_stage)
			pbd_session_log.notice("Lobby set: game %u has a random stage: its stage countdown starts at zero.", command.game_number);

		return false;
	}

	bool prepare_repick()
	{
		// Change character: the stock select runs again in the open, driven by input alone. The kept fighter's PC steers
		// it to the frozen row and costume (that player's own buttons never reach the select), the stage owner's PC steers
		// the frozen stage, and the loser picks with the character countdown and its zero press. Every word is frozen,
		// sent and recorded like any other, so the other game and every watcher execute the same select, and the battle
		// readback checks the frozen picks (pbd::lobby_set::driver, pbd::rematch::native_load).
		const auto& load = *command.rematch;
		const u32 chooser = *load.repick;
		data.lobby.selection = std::make_unique<pbd::hidden_selection::driver>();
		if (!data.lobby.selection->begin({load.selected.rows[0], load.selected.rows[1], load.selected.stage, manifest.rounds_per_game,
											 data.peer.role, chooser, pbd::hidden_selection::selection_samples + manifest.selection.character_seconds * 60},
				std::move(*tables), data.next_index))
		{
			data.lobby.set->refuse(data.lobby.selection->error(), "The kept fighter or the stage is unavailable in this game.");
			session_runtime.finish(guard_failed);
			return true;
		}
		if (data.session.mask_start && data.lobby.selection->owns_input() && (data.lobby.selection->confirm_button() & session_start_bit))
		{
			data.lobby.set->refuse("selection_costume_masked", "This costume needs a selection button disabled by the session settings.");
			session_runtime.finish(guard_failed);
			return true;
		}
		data.lobby.select_timer.emplace();
		if (!data.lobby.select_timer->begin_repick(manifest.selection.character_seconds, data.peer.role, chooser))
		{
			data.lobby.set->refuse("selection_timer_unavailable", "The in-game selection countdowns could not be armed.");
			session_runtime.finish(guard_failed);
			return true;
		}
		pbd_session_log.notice("Lobby set: game %u: the %s side changes character; the other fighter and the stage stay.",
			command.game_number, chooser ? "Right" : "Left");
		return false;
	}

	bool record_continuation_prefix()
	{
		// The exclusive end callback may already have executed while its confirmed result was published.
		// Its real neutral input belongs to the next game's presentation, so preserve that row instead of inventing one.
		for (u32 boundary = sealed->end_boundary; boundary < data.next_index; ++boundary)
		{
			const auto& tick = data.tick_at(boundary);
			if (!data.sample_at(boundary).completed || !tick.read)
			{
				session_runtime.finish(guard_failed);
				return true;
			}
			pbd::match_recording::observation observed{};
			observed.boundary = boundary;
			const auto classified = pbd::match_recording::classify(tick.scene[0], 0, false);
			observed.native_phase = classified.value;
			observed.phase_verified = classified.verified;
			if (!data.lobby.set->record(observed, {}))
			{
				session_runtime.finish(guard_failed);
				return true;
			}
		}

		return false;
	}
};
