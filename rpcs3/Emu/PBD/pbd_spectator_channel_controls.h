#pragma once

#include "pbd_spectator_channel_publication.h"

namespace pbd::spectator
{
	inline bool local_channel::accept_rematch(const value& root, u32 game, bool round_required, const pbd::rematch::options* actual)
	{
		const auto* item = root.find("load");
		if (!item)
			return !m_rematch_epoch || refuse("rematch_load_missing");
		const auto load = pbd::rematch::read_load(item);
		if (!actual)
			return refuse("rematch_options_unverified");
		if (!load || !round_required || !m_game || !m_ended || !m_applied_picks || !m_end ||
			*m_end != m_consumed || m_rematch)
			return refuse("rematch_tail_unverified");
		const pbd::rematch::sealed_state previous{m_game, m_consumed, m_rematch_epoch,
			{{m_applied_picks->left, m_applied_picks->right}, m_applied_picks->stage}, *actual};
		if (load->next_game != game || pbd::rematch::validate(*load, previous, false) != pbd::rematch::error::none)
			return refuse("rematch_load_changed");
		m_rematch = std::pair{*load, previous};
		m_rematch_epoch = load->epoch;
		return true;
	}

	inline void local_channel::clear_game_presentation(u32 game)
	{
		m_presentation.native_game = game;
		m_presentation.native_through = 0;
		m_presentation.sync_mask = 0;
		m_presentation.history = {};
		m_presentation.history_count = {};
		// The game the watch waited for has begun: its inputs, not the waiting control, keep the watch going now.
		m_presentation.waiting_title.clear();
		m_presentation.waiting_text.clear();
		m_alive_at.reset();
	}

	inline bool local_channel::begin_game(const value& root, const pbd::rematch::options* actual_options)
	{
		using namespace pbd::set_channel;
		u32 game = 0, origin = 0, counter = 0, rounds = 0, capsule_epoch = 0;
		// A fixed set's game repeats the set's picks. An in-game set's game may name none yet (they are chosen in this game's
		// select), or the picks the lobby already agreed, which the intro readback must then match.
		std::optional<picks> reported;
		const bool capsule = !m_game && !m_identity.pbd_capsule.empty();
		if (const auto* epoch = root.find("capsuleRematchEpoch"); epoch &&
																  (!capsule || !pbd::set_channel::detail::number32(epoch, 0, 18, capsule_epoch)))
			return refuse("capsule_rematch_epoch_invalid");
		if (!pbd::set_channel::detail::number32(root.find("gameNumber"), 1, 19, game) ||
			game != (capsule ? m_identity.pbd_capsule_game : m_game + 1) || (m_game && !m_ended) ||
			!m_frames.empty() || m_reserved ||
			!pbd::set_channel::detail::number32(root.find("originBoundary"), 0, maximum_boundary, origin) ||
			!pbd::set_channel::detail::number32(root.find("originCounter"), 0, 0xffffffffu, counter) ||
			(!capsule && game == 1 && (origin || counter != (0x80000000u | (m_identity.seed & 0x00ffffffu)))) ||
			(capsule && origin != m_identity.pbd_game_origin) ||
			!pbd::set_channel::detail::is(root.find("kind"), capsule ? "capsule_start" : game == 1 ? "clean_character_select" :
																									 "natural_vs") ||
			!read_picks(root, reported) ||
			!pbd::set_channel::detail::number32(root.find("roundsPerGame"), 3, 3, rounds))
			return refuse("game_control_unverified");
		std::optional<native_round> round;
		if (const auto* item = root.find("nativeRound"))
		{
			round = read_native_round(*item);
			if (!round)
				return refuse("native_round_control_invalid");
		}
		const auto* required = root.find("roundWitnessRequired");
		if (required && (required->kind != json_kind::boolean || !required->truth || !m_identity.applied))
			return refuse("native_round_control_invalid");
		// The game's own Ranked screens, as the players' game_started recorded them; display only, so a malformed value shows none.
		std::optional<pbd::rank_stakes::game_values> rank_screens;
		if (const auto* item = root.find("rankScreens"))
			rank_screens = pbd::rank_stakes::read_values(item);
		const bool from_start = required != nullptr, round_required = from_start || round.has_value();
		if (m_game && (m_round_required != round_required || m_from_start != from_start))
			return refuse("native_round_contract_changed");
		if (!capsule && !accept_rematch(root, game, round_required, actual_options))
			return false;
		if (capsule)
		{
			if (root.find("load"))
				return refuse("capsule_rematch_load_unexpected");
			m_rematch_epoch = capsule_epoch;
		}
		m_load.reset();
		if (m_rematch)
		{
			const auto& load = m_rematch->first;
			const picks frozen{load.selected.stage, load.selected.rows[0], load.selected.rows[1]};
			if (reported && !keeps(load, *reported))
				return refuse("rematch_picks_changed");
			// A repick's chooser picks in the select, so its game is known in full only at the intro.
			if (!load.repick)
				reported = frozen;
			m_load = load;
		}
		if (!m_identity.in_game && (!reported || *reported != fixed_picks()))
			return refuse("game_control_unverified");
		// A repick plays its select from the recorded input like any in-game selection, so the watcher shows it.
		m_direct_load = capsule || (m_load && !m_load->repick) || (game == 1 && m_identity.direct_start);
		m_round_required = round_required;
		m_from_start = from_start;
		m_expected_round = round;
		m_reported_picks = reported;
		m_applied_picks.reset();
		m_rank_screens = rank_screens;
		m_game = game;
		m_origin = origin;
		m_receipt = m_consumed = m_checked = capsule ? m_identity.pbd_first_frame : 0;
		m_end.reset();
		m_visible = m_ended = false;
		clear_game_presentation(game);
		draw();
		return true;
	}

	inline bool local_channel::control_stop()
	{
		using namespace pbd::set_channel;

		m_stopped = true;
		m_presentation.hidden = true;
		publish_presentation(m_native_generation, m_presentation);
		draw_ui();
		return true;
	}

	inline bool local_channel::control_game_state(const value& root)
	{
		using namespace pbd::set_channel;

		u32 game = 0;
		const auto* item = root.find("nativeRound");
		const auto round = item ? read_native_round(*item) : std::nullopt;
		if (!game_active() || !m_from_start || !m_round_required || !round ||
			!pbd::set_channel::detail::number32(root.find("gameNumber"), 1, 19, game) || game != m_game)
			return refuse("native_round_control_invalid");
		if (m_expected_round && *m_expected_round != *round)
			return refuse("native_round_start_mismatch");
		m_expected_round = round;
		return true;
	}

	inline bool local_channel::control_end_game(const value& root)
	{
		using namespace pbd::set_channel;

		u32 game = 0, final = 0;
		if (!pbd::set_channel::detail::number32(root.find("gameNumber"), m_game, m_game, game) || !game_active() ||
			!pbd::set_channel::detail::number32(root.find("finalFrame"), m_receipt, maximum_frames, final) || (m_end && *m_end != final))
			return refuse("end_watermark_mismatch");
		m_end = final;
		return true;
	}

	inline bool local_channel::control_game_picks(const value& root)
	{
		using namespace pbd::set_channel;

		// The lobby's agreed picks for the game being watched, appended to the stream when both fighters reported them. A
		// cross-check only: the intro is exposed from this game's own readback whether or not they came, and nothing waits for
		// them. Before the intro they are held for it; after it they must equal what the game applied.
		u32 game = 0;
		std::optional<picks> reported;
		if (!pbd::set_channel::detail::number32(root.find("gameNumber"), 1, 19, game) || !m_game || game != m_game ||
			!read_picks(root, reported) || !reported)
			return refuse("game_picks_invalid");
		if ((!m_identity.in_game && *reported != fixed_picks()) || (m_applied_picks && *reported != *m_applied_picks) ||
			(m_reported_picks && *reported != *m_reported_picks) || (m_load && !keeps(*m_load, *reported)))
			return refuse("game_picks_mismatch");
		m_reported_picks = reported;
		return true;
	}

	inline bool local_channel::control_presentation(const value& root)
	{
		using namespace pbd::set_channel;

		const auto *hidden = root.find("hidden"), *catch_up = root.find("catchUp");
		const auto *mode = root.find("mode"), *left = root.find("leftName"), *right = root.find("rightName"), *score = root.find("score");
		if (!hidden || hidden->kind != json_kind::boolean || !catch_up || catch_up->kind != json_kind::boolean ||
			(!pbd::set_channel::detail::is(mode, "watch") && !pbd::set_channel::detail::is(mode, "broadcast") && !pbd::set_channel::detail::is(mode, "analyze")) ||
			!left || left->kind != json_kind::string || left->text.size() > 16 || !right || right->kind != json_kind::string || right->text.size() > 16 ||
			!score || !pbd::set_channel::detail::number32(score->find("left"), 0, 10, m_presentation.left_score) ||
			!pbd::set_channel::detail::number32(score->find("right"), 0, 10, m_presentation.right_score) ||
			!pbd::set_channel::detail::number32(root.find("delayFrames"), 0, 3600, m_presentation.delay_frames))
			return refuse("presentation_invalid");
		for (const auto* name : {left, right})
			for (unsigned char c : name->text)
				if (c < 0x20 || c == 0x7f)
					return refuse("presentation_invalid");
		m_presentation.hidden = hidden->truth;
		m_presentation.catch_up = catch_up->truth;
		m_presentation.broadcast = pbd::set_channel::detail::is(mode, "broadcast");
		m_presentation.analyze = pbd::set_channel::detail::is(mode, "analyze");
		m_presentation.left_name = left->text;
		m_presentation.right_name = right->text;
		draw();
		if (m_presentation.broadcast)
			pbd::corner_chat::clear();
		return true;
	}

	inline bool local_channel::control_chat(const value& root)
	{
		using namespace pbd::set_channel;

		const auto *visible = root.find("visible"), *additions = root.find("additions");
		if (!visible || visible->kind != json_kind::boolean || !additions || additions->kind != json_kind::array || additions->items.size() > 20)
			return refuse("chat_invalid");
		std::vector<pbd::corner_chat::message> messages;
		for (const auto& item : additions->items)
		{
			const auto *id = item.find("messageId"), *author = item.find("authorName"), *text = item.find("text");
			if (!id || id->kind != json_kind::string || (author && author->kind != json_kind::string) || !text || text->kind != json_kind::string)
				return refuse("chat_invalid");
			messages.push_back({id->text, author ? author->text : std::string{}, text->text});
		}
		if (!pbd::corner_chat::publish(m_native_generation, visible->truth && !m_presentation.broadcast && !m_presentation.hidden, messages))
			return refuse("chat_invalid");
		return true;
	}

	inline bool local_channel::control_waiting(const value& root)
	{
		using namespace pbd::set_channel;

		// The source is legitimately between games, or its next game has not started on the stream yet (its select and VS load
		// run first, and the watcher receives them at that game's start). It names the game the watch waits for; one that names
		// that game while the watch is between games moves the input deadline (input_wait_outcome) and tells the cover. Any other
		// game number is a late or early marker and changes nothing, and so is any marker after a drawn game (that watch closes);
		// only a malformed one refuses.
		u32 game = 0;
		const auto* phase = root.find("phase");
		const bool between = pbd::set_channel::detail::is(phase, "between_games");
		if (!pbd::set_channel::detail::number32(root.find("gameNumber"), 1, 19, game) || (!between && !pbd::set_channel::detail::is(phase, "next_game")))
			return refuse("waiting_invalid");
		if (game == m_game + 1 && between_games() && !m_draw_closed)
		{
			m_alive_at = std::chrono::steady_clock::now();
			m_presentation.waiting_title = between ? "BETWEEN GAMES" : "NEXT GAME";
			m_presentation.waiting_text = between ? "Game " + std::to_string(game) + " starts when both players are ready." :
			                                        "The players are choosing their fighters for game " + std::to_string(game) + ".";
			draw();
		}
		return true;
	}

	inline bool local_channel::control_watch_ui(const value& root)
	{
		using namespace pbd::set_channel;

		// The watcher's whole screen (pbd_spectator_ui::parse). A preview fades from the moment its message first shows
		// (draw_ui): the same message in a later watch_ui keeps its time, another one waits to show and starts again.
		auto control_ui = spectator_ui::parse(root, m_ui ? m_ui->revision : 0);
		if (!control_ui.value)
			return refuse(control_ui.error);
		if (control_ui.value->new_line && (!m_ui || !m_ui->new_line || m_ui->new_line->id != control_ui.value->new_line->id))
			m_preview_since = 0;
		m_ui = std::move(*control_ui.value);
		m_pad.apply(*m_ui);
		draw_ui();
		return true;
	}

	inline bool local_channel::apply_control(const value& root, const pbd::rematch::options* actual_options)
	{
		const auto* type = root.find("type");
		if (pbd::set_channel::detail::is(type, "stop"))
			return control_stop();
		if (pbd::set_channel::detail::is(type, "begin_game"))
			return begin_game(root, actual_options);
		if (pbd::set_channel::detail::is(type, "game_witness"))
			return control_game_state(root);
		if (pbd::set_channel::detail::is(type, "end_game"))
			return control_end_game(root);
		if (pbd::set_channel::detail::is(type, "game_picks"))
			return control_game_picks(root);
		if (pbd::set_channel::detail::is(type, "presentation"))
			return control_presentation(root);
		if (pbd::set_channel::detail::is(type, "chat"))
			return control_chat(root);
		if (pbd::set_channel::detail::is(type, "waiting"))
			return control_waiting(root);
		if (pbd::set_channel::detail::is(type, "watch_ui"))
			return control_watch_ui(root);
		return refuse("unknown_control");
	}

	inline bool local_channel::poll_control(const pbd::rematch::options* actual_options)
	{
		using namespace pbd::set_channel;
		if (!healthy())
			return false;
		const auto control_text = pbd::set_channel::detail::read_text(m_directory / ("spectator-control-" + std::to_string(m_control_serial + 1) + ".json"));
		if (!control_text.value)
			return control_text.error == "unavailable" || refuse("control_read_failed");
		const auto parsed = pbd::set_channel::detail::json_parser(*control_text.value).parse();
		if (!parsed.value)
			return refuse("control_invalid");
		const auto& root = *parsed.value;
		u64 serial = 0;
		if (!pbd::set_channel::detail::is(root.find("schema"), "pbd-spectator-control-1") || !pbd::set_channel::detail::is(root.find("generation"), std::to_string(m_identity.generation)) ||
			!pbd::set_channel::detail::is(root.find("bindingDigest"), hex(m_identity.binding)) || !pbd::set_channel::detail::number(root.find("serial"), 1, 131072, serial) || serial != m_control_serial + 1)
			return refuse("control_binding_mismatch");
		const auto* type = root.find("type");
		// After a drawn game the lobby replays it under its own number, which this watch does not follow. The replay goes through
		// the select again, so its begin_game, its game_picks marker (the same game number, possibly other fighters) and its
		// end_game end the watch with draw_replay instead of being checked against the drawn game.
		if (m_draw_closed && (pbd::set_channel::detail::is(type, "begin_game") || pbd::set_channel::detail::is(type, "end_game") ||
								 pbd::set_channel::detail::is(type, "game_picks") || pbd::set_channel::detail::is(type, "game_witness")))
			return close("draw_replay");
		if (!apply_control(root, actual_options))
			return false;
		++m_control_serial;
		return true;
	}

} // namespace pbd::spectator
