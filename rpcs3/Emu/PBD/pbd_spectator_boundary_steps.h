#pragma once

#include "pbd_rank_screens.h"

struct pbd::rollback_session::spectator_boundary_steps : boundary_call
{
	pbd::spectator::local_channel& spectator;
	session_clock::time_point wait_from{};
	u32 wait_game = 0;
	spectator_boundary_steps(boundary_call& call, pbd::spectator::local_channel& channel) : boundary_call(call), spectator(channel) {}
	bool run()
	{

		if (prepare_channel())
			return true;
		if (data.lobby.direct_match.pbd_live_inputs())
		{
			// Resource loading executes normally before the live channel can supply the captured match's first input. The
			// captured game is already in its fight when its inputs begin, so its own Ranked screens take the values its start
			// recorded here, before the stock initializer and the battle's setup (pbd::rank_screens).
			if (!data.lobby.direct_match.begun())
			{
				if (pbd::rank_screens::configured().screens)
					pbd::rank_screens::arm(data.lobby.direct_match.pbd_rank_screens().value_or(pbd::rank_stakes::game_values{}), {0u, 0u});
				return false;
			}
			const auto anchor = data.lobby.direct_match.pbd_start_boundary();
			if (anchor == ~0u || !spectator.pbd_set_native_anchor(anchor))
			{
				terminal("capsule_native_anchor_unavailable");
				return true;
			}
			if (data.next_index == anchor)
			{
				data.session.segments.number = spectator.identity().pbd_source_segment;
				pbd::fn_hook::keyed_segment.store(data.session.segments.number);
			}
		}
		if (wait_input())
			return true;
		return apply_frame();
	}

	void terminal(const char* reason)
	{
		if (!spectator.stopped())
			spectator.refuse(reason);
		session_runtime.finish(spectator.stopped() ? user_stopped : guard_failed);
		data.end_callback();
	}

	bool prepare_channel()
	{
		if (!spectator.prepared())
		{
			// The clean start, then what the start sync made of the set's seed at this boundary.
			u32 counter = 0;
			const auto identity = Emu.PBDGetCheckpointBootIdentity();
			if (!spectator_start_clean() || spectator.identity().recipe != pbd::env::find("PBD_MATCH_REPLAY_RECIPE_SHA256") ||
				!read_word(0x1008f07c, counter) ||
				(!data.lobby.direct_match.pbd_live_inputs() && (!data.session.generator_syncs || !data.session.random_panel_syncs)) ||
				!spectator.prepare(data.generation, ppu.id, data.next_index, counter, identity->sha256, spectator.identity().emulator))
			{
				terminal("clean_start_unverified");
				return true;
			}
		}
		if (spectator.identity().direct_start && !data.lobby.direct_match.pbd_live_inputs() &&
			data.next_index == data.start_index && !data.lobby.rematch.active())
		{
			pbd::rematch::options rules;
			const auto& picks = spectator.identity();
			if (!pbd::rematch::capture_options(ppu, rules, true) || rules.rounds != picks.rounds ||
				!data.lobby.rematch.arm_start({{picks.left, picks.right}, picks.stage}, rules, data.generation, data.next_index))
			{
				terminal("spectator_direct_start_unverified");
				return true;
			}
		}

		return false;
	}

	bool wait_input()
	{
		// Nothing for 60 s ends the watch, unless the launcher's waiting control says the source is between games or in its next
		// game's select (pbd::spectator::input_wait_outcome); a real stall still ends with its own reason.
		wait_from = session_clock::now();
		wait_game = spectator.game();
		bool held = false;
		pbd::rematch::options rematch_options;
		const bool options_read = pbd::rematch::capture_options(ppu, rematch_options);
		for (u32 polls = 0; !data.spectator.frame; ++polls)
		{
			if (!spectator.poll_control(options_read ? &rematch_options : nullptr) || !spectator.healthy())
			{
				terminal("spectator_control_refused");
				return true;
			}
			if (const auto load = spectator.take_rematch();
				load && !data.lobby.rematch.arm(load->first, load->second, false, data.generation, data.next_index))
			{
				terminal("spectator_rematch_unverified");
				return true;
			}
			// A new game gets its own input deadline, independent of the time spent between games.
			if (spectator.game() != wait_game)
			{
				wait_game = spectator.game();
				wait_from = session_clock::now();
			}
			// Watcher controls stay responsive while waiting; the shared menu keeps its controller claim.
			spectator.read_pad(pbd_spectator_pad_word(), pbd::settings_menu::claimed());
			if (observe_game_end())
				return true;
			if (!spectator.poll_input([](const std::uint8_t* bytes, std::size_t size)
					{
						pbd::spectator::digest out{};
						mbedtls_sha256_ret(bytes, size, out.data(), 0);
						return out;
					}))
			{
				terminal("spectator_input_refused");
				return true;
			}
			data.spectator.frame = spectator.reserve();
			if (data.spectator.frame)
				break;
			if (check_input_deadline())
				return true;
			if (!held)
			{
				held = true;
				vm::temporary_unlock(ppu);
			}
			if (polls % 20 == 0)
				rsx::set_native_ui_flip();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		return false;
	}

	bool observe_game_end()
	{
		if (spectator.awaiting_end())
		{
			u32 scene = 0, left = 0, right = 0;
			std::array<be_t<s16>, 2> left_result{}, right_result{};
			std::array<u8, 3> winner{};
			const bool over = data.spectator.was_battle && read_word(pbd::t5dr::scene_address, scene) && scene != 16u && scene != 17u &&
			                  read_word(0x10091ab0, left) && read_word(0x10091ab4, right) && left == 0x10090040 &&
			                  right == 0x10090910 &&
			                  vm::try_access(left + 0x90, left_result.data(), sizeof(left_result), false) &&
			                  vm::try_access(right + 0x90, right_result.data(), sizeof(right_result), false) &&
			                  vm::try_access(0x1008ebd5, winner.data(), winner.size(), false);
			const bool left_named = winner == std::array<u8, 3>{1, 0, 1};
			const bool right_named = winner == std::array<u8, 3>{2, 1, 0};
			const bool decided = over && ((left_named && s16{left_result[0]} == 3 && s16{right_result[0]} < 3) ||
											 (right_named && s16{right_result[0]} == 3 && s16{left_result[0]} < 3));
			// A drawn game, classified as the lobby and replay paths classify it: winner bytes that name neither side (the
			// lobby records a named side as that side's win whatever the scores, so such an end is never a draw here either),
			// both fighters at the rounds to win, the draw branch's markers, equal terminal health, and a K.O. or a time-out.
			// The lobby replays it under its own number, which this watch does not follow (pbd::spectator::local_channel::end).
			u32 health_left = 0, health_right = 0, round_clock = 0;
			const bool drawn = over && !left_named && !right_named &&
			                   s16{left_result[0]} == static_cast<s16>(spectator.identity().rounds) &&
			                   s16{right_result[0]} == s16{left_result[0]} && s16{left_result[1]} == -1 && s16{right_result[1]} == -1 &&
			                   read_word(left + 0x6ac, health_left) && read_word(right + 0x6ac, health_right) &&
			                   read_word(0x1008f054, round_clock) && health_left == health_right &&
			                   (static_cast<s32>(health_left) <= 0 || static_cast<s32>(round_clock) <= 0);
			if (!spectator.end(data.next_index, decided || drawn, drawn))
			{
				terminal("natural_end_unverified");
				return true;
			}
			if (drawn)
				pbd_session_log.notice("Spectator: game %u was drawn at boundary %u; watching ends before its replay.",
					spectator.game(), data.next_index);
			data.spectator.was_battle = false;
			data.spectator.intro_seen = false;
		}
		return false;
	}

	bool check_input_deadline()
	{
		if (pbd_emulation_stopping(ppu))
		{
			terminal("emulation_stopped");
			return true;
		}
		if (const auto outcome = pbd::spectator::input_wait_outcome(wait_from, spectator.alive_at(), session_clock::now());
			outcome != pbd::spectator::wait_outcome::waiting)
		{
			// After a drawn game nothing more comes for this watch: it ends as a stop with the draw's notice, not a fault.
			if (spectator.draw_closed())
				spectator.close("draw_replay");
			const char* reason = pbd::spectator::wait_reason(outcome);
			pbd_session_log.warning("Spectator: no input for game %u at boundary %u (%s).",
				spectator.game() + (spectator.between_games() ? 1u : 0u), data.next_index, reason);
			terminal(reason);
			return true;
		}

		return false;
	}

	bool apply_frame()
	{
		const auto& frame = *data.spectator.frame;
		const u32 native_boundary = spectator.pbd_native_boundary(frame.boundary);
		u32 scene = 0;
		if (native_boundary < data.next_index || native_boundary >= data.extent() ||
			!read_word(pbd::t5dr::scene_address, scene) || frame.scene != scene)
		{
			pbd_session_log.error("Spectator coordinate refusal: game %u frame %u, recorded boundary %u/scene %u, native next %u/scene %u",
				spectator.game(), spectator.consumed(), frame.boundary, frame.scene, data.next_index, scene);
			terminal("native_scene_or_boundary_mismatch");
			return true;
		}
		const auto following_scene = spectator.following_scene();
		if (frame.scene == 1 && following_scene && *following_scene != 1 && *following_scene != 2)
		{
			terminal("natural_vs_init_transition_invalid");
			return true;
		}
		pbd::fn_hook::spectator_vs_init_gate.store(frame.scene == 1 ? (following_scene && *following_scene == 2 ? 2u : 1u) : 0u,
			std::memory_order_release);
		// Peer alignment holes are explicit native coordinates, never neutral filler rows.
		data.next_index = native_boundary;
		data.replay.sync[frame.segment] = frame.counter;
		pbd::fn_hook::gate_hold.store(frame.gate_hold, std::memory_order_release);
		if (data.spectator.game != spectator.game())
		{
			data.spectator.game = spectator.game();
			data.spectator.intro_seen = false;
		}
		// The watcher's own screen shows only while the watched game has a fight on screen (pbd_spectator_ui::fight_on_screen):
		// from round 1's start, never in the intros, at a K.O. or between rounds. A read of the pending phase at this boundary,
		// never a write; the watched game's input is the recorded frame above whatever this decides.
		{
			u32 pending = 0;
			spectator.observe_fight(read_word(0x10093230, pending) && pbd::spectator_ui::fight_on_screen(scene, pending));
		}
		// The watched players' names under the life bars and on the VS screen, drawn by the game from its records
		// (pbd::game_profile), as the launcher's presentation names them; a watcher is given no ranks, so the plates show
		// Beginner. Display only: the records feed no watched input and no hashed state.
		store_game_profiles({spectator.display().left_name, spectator.display().right_name}, with_kept_promotions({0u, 0u}));
		// The game's own Ranked screens write what the players' games wrote in this game (pbd::rank_screens); a promotion the
		// game showed stays in the records until the next game's setup.
		if (pbd::rank_screens::configured().screens && pbd::rank_stakes::values_may_change(scene))
			pbd::rank_screens::arm(spectator.rank_screens().value_or(pbd::rank_stakes::game_values{}), {0u, 0u});
		publish_vs_cards(scene);
		// Confirmed character-select frames need the same catch-up pacing as combat; loading keeps its I/O cadence.
		const bool fast = spectator.display().catch_up && (scene == 3u || scene == 16u || scene == 17u);
		spectator_pacing(fast);
		pbd::player_volume::apply(fast || spectator.display().hidden || !spectator.visible());

		return false;
	}

	void publish_vs_cards(u32 scene)
	{
		// The watcher's VS cards (pbd::vs_cards) while the watched game passes its VS screen: the names from the launcher's
		// presentation and the set length from its watch_ui. A watcher holds no ranks or score, so the cards carry neither.
		// Display only, as in a fighter's game; only the boundary that leaves the VS screens publishes the hidden cards.
		if (const auto& watch_screen = spectator.screen(); watch_screen && watch_screen->first_to)
		{
			const bool vs = pbd::vs_cards::vs_scene(scene);
			if (vs || data.lobby.vs_cards_shown)
			{
				data.lobby.vs_cards_shown = vs;
				pbd::vs_cards::set_view set;
				set.names = {spectator.display().left_name, spectator.display().right_name};
				set.local = 0;
				set.game_number = spectator.game();
				set.first_to = watch_screen->first_to;
				// Over a cover the cards keep the names; over the game's own VS screen its portraits show them.
				set.over_cover = pbd::vs_cards::watch_over_cover(pbd::spectator::requested_directory().has_value(), spectator.visible());
				pbd::vs_cards::publish(data.generation, pbd::vs_cards::compose(set, scene));
			}
		}
	}
};
