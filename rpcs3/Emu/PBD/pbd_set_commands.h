#pragma once

#include "pbd_set_channel_model.h"
#include "pbd_rank_stakes.h"

#include <tuple>

namespace pbd::set_channel
{
	enum class command_kind
	{
		continue_game,
		show_post_set,
		choice_rejected,
		end_set,
		chat_overlay,
		stream_status,
		vs_cards,
		next_set,
		rank_stakes
	};
	// vs_cards: one player's language and region as the VS card shows them ("EN", "N. America"); either may be empty.
	inline constexpr std::size_t max_card_language_bytes = 16, max_card_region_bytes = 32;
	struct card_origin
	{
		std::string language, region;
	};
	// vs_cards' optional "mode": the matchmaking the set came from, for the notices' wording; none in a lobby room.
	enum class card_mode : std::uint8_t
	{
		none,
		quick,
		ranked
	};
	// vs_cards' optional per-side "character": the VS caption, upper case, printable ASCII.
	inline constexpr std::size_t max_card_character_bytes = 24;
	struct command
	{
		command_kind kind = command_kind::end_set;
		std::uint64_t serial = 0;
		std::uint32_t game_number = 0;
		score games;
		// continue_game only: the drawn game is played again under its own number ("replay":"draw").
		bool replay = false;
		// continue_game only ("stage":"random", an in-game set's next game): the loser of the last game chose Rematch with random
		// stage, so this game's stage countdown starts at zero and the stage owner's roulette stops by itself
		// (pbd::select_timer::timer::begin).
		bool random_stage = false;
		std::optional<pbd::rematch::load> rematch;
		bool winner = false, challenger_waiting = false, visible = false;
		std::vector<std::string> allowed_actions;
		std::string reason, status;
		json_value additions;
		// vs_cards only: left, right, and the optional mode and fighter captions.
		std::array<card_origin, 2> origins;
		card_mode mode = card_mode::none;
		std::array<std::string, 2> characters;
		// next_set only: the new set's identity and length (game_number is 1 and games 0-0), and each player's rank for it when
		// the command names one (left, right).
		std::string next_set_id, next_match_id;
		std::uint32_t first_to_games = 0;
		std::array<std::optional<std::uint32_t>, 2> ranks;
		// rank_stakes only: what a set win and a set loss give each player (left, right), and each placing player's count.
		pbd::rank_stakes::stakes stakes{};
		std::array<std::optional<pbd::rank_stakes::placement>, 2> placements;
		// Display-only optional fields that were present but invalid, by name ("vs_cards.mode"): read as left out, never a
		// refusal, since a refused display command would end the set. The latch logs them.
		std::vector<std::string> ignored;
	};

	// A command's repeated stage (the object form) may only state the set's own stage rule.
	inline bool repeats_stage(const json_value& stage, const manifest& expected)
	{
		std::uint32_t id = 0;
		side picker = side::right;
		if (in_game(expected))
			return parse_in_game_stage(&stage, picker) && picker == expected.selection.stage_picker;
		return detail::number32(stage.find("id"), expected.stage_id, expected.stage_id, id) && detail::is(stage.find("rule"), "fixed");
	}

	// A repeated "character" may only state the set's own: none in an in-game set, the fighter's id and row in a fixed one.
	inline bool repeats_character(const json_value* character, const fighter& expected_fighter, const manifest& expected)
	{
		std::uint32_t row = 0;
		if (in_game(expected))
			return !character || character->kind == json_kind::null;
		return character && detail::is(character->find("id"), expected_fighter.character_id) &&
		       detail::number32(character->find("row"), expected_fighter.row, expected_fighter.row, row);
	}

	// A command's repeated "left" and "right" may only state the set's own members and, in a fixed set, its characters.
	inline bool repeats_fighters(const json_value& value, const manifest& expected)
	{
		using player = std::pair<std::string_view, const pbd::set_channel::fighter*>;
		for (const auto& [key, fighter] : {player{"left", &expected.left}, player{"right", &expected.right}})
		{
			if (const auto* repeated = value.find(key))
			{
				if (!detail::is(repeated->find("memberId"), fighter->member_id) || !repeats_character(repeated->find("character"), *fighter, expected))
					return false;
			}
		}
		return true;
	}

	// next_set's optional "left" and "right" carry each player's rank for the new set ("rank": the game's rank index, null or
	// left out for none), so a Ranked set's plates show the rank the finished set gave. Their "memberId" and "character" are
	// optional and may only repeat the set's own.
	inline std::string_view read_next_fighters(const json_value& value, const manifest& expected, command& out)
	{
		using player = std::tuple<std::string_view, const pbd::set_channel::fighter*, std::size_t>;
		for (const auto& [key, fighter, index] : {player{"left", &expected.left, 0}, player{"right", &expected.right, 1}})
		{
			const auto* repeated = value.find(key);
			if (!repeated)
				continue;
			if (repeated->kind != json_kind::object)
				return "bad_next_set";
			const auto* member = repeated->find("memberId");
			const auto* character = repeated->find("character");
			if ((member && !detail::is(member, fighter->member_id)) || (character && !repeats_character(character, *fighter, expected)))
				return "changed_set_values";
			if (const auto* rank = repeated->find("rank"); rank && rank->kind != json_kind::null)
			{
				std::uint32_t held = 0;
				if (!detail::number32(rank, 0, max_rank, held))
					return "bad_next_set";
				out.ranks[index] = held;
			}
		}
		return {};
	}

	// A new set starts at game 1 at 0-0. An optional sealed Rematch load keeps its fighters and loads its next stage directly.
	inline std::string_view read_next_set(const json_value& value, const manifest& expected, command& out)
	{
		out.kind = command_kind::next_set;
		const auto* set = value.find("nextSetId");
		const auto* match = value.find("matchId");
		const auto* rules = value.find("rules");
		if (!detail::identifier(set) || !detail::identifier(match) || set->text == expected.set_id || match->text == expected.match_id ||
			!detail::number32(value.find("gameNumber"), 1, 1, out.game_number) || !parse_score(value.find("score"), 0, out.games) ||
			!rules || !detail::number32(rules->find("firstToGames"), 1, 10, out.first_to_games) || value.find("replay"))
			return "bad_next_set";
		if (const auto* load = value.find("load"))
		{
			out.rematch = pbd::rematch::read_load(load);
			if (!out.rematch || out.rematch->previous_game != expected.game_number || out.rematch->next_game != 1 || out.rematch->repick)
				return "bad_rematch_load";
		}
		out.next_set_id = set->text;
		out.next_match_id = match->text;
		std::uint32_t rounds = 0;
		bool allow = false;
		const auto* stage = value.find("stage");
		const auto* jinpachi = rules->find("allowJinpachi");
		if (!detail::number32(rules->find("roundsPerGame"), expected.rounds_per_game, expected.rounds_per_game, rounds) ||
			(jinpachi && (!detail::boolean(jinpachi, allow) || allow != expected.allow_jinpachi)) ||
			(stage && (stage->kind != json_kind::object || !repeats_stage(*stage, expected))))
			return "changed_set_values";
		return read_next_fighters(value, expected, out);
	}

	inline std::string_view read_continuation(const json_value& value, const manifest& expected, command& out, std::uint32_t current_game)
	{
		out.kind = command_kind::continue_game;
		// A drawn game does not count: it is played again with the same number and an unchanged score, and says so explicitly.
		// Without the marker a continuation is always the next game.
		if (const auto* replay = value.find("replay"))
		{
			if (!detail::is(replay, "draw"))
				return "bad_next_game";
			out.replay = true;
		}
		if (!detail::number32(value.find("gameNumber"), out.replay ? 1u : 2u, 19, out.game_number) ||
			out.game_number != current_game + (out.replay ? 0u : 1u) ||
			!parse_score(value.find("score"), expected.first_to_games - 1, out.games) ||
			out.game_number != out.games.left + out.games.right + 1 ||
			(out.replay && (out.games.left != expected.games.left || out.games.right != expected.games.right)))
			return "bad_next_game";
		if (const auto* load = value.find("load"))
		{
			out.rematch = pbd::rematch::read_load(load);
			// A repick (the loser of the last game picks again) needs the game's own select, a loser and the last game's stage:
			// never a fixed set, a drawn game's replay or a random stage.
			if (!out.rematch || out.rematch->previous_game != current_game || out.rematch->next_game != out.game_number ||
				(out.replay && out.rematch->random_stage) || value.find("stage") ||
				(out.rematch->repick && (!in_game(expected) || out.replay || out.rematch->random_stage)))
				return "bad_rematch_load";
		}
		// Optional repeated set values are assertions, never permission to change the original selection. In an in-game set
		// they can only repeat the rule: the next game's choices are made in the game's own select again. The one exception is
		// the string "random" in an in-game set's next game (never a fixed set, never a drawn game's replay, which has no
		// loser to ask for it): that game's stage is the roulette's own stop. Anything else is refused, and the game ends the
		// set with invalid_command.
		if (const auto* stage = value.find("stage"); stage && stage->kind == json_kind::string)
		{
			if (!detail::is(stage, "random") || !in_game(expected) || out.replay)
				return "bad_random_stage";
			out.random_stage = true;
		}
		else if (stage && !repeats_stage(*stage, expected))
			return "changed_set_values";
		if (!repeats_fighters(value, expected))
			return "changed_set_values";
		if (const auto* rules = value.find("rules"))
		{
			std::uint32_t first = 0, rounds = 0;
			if (!detail::number32(rules->find("firstToGames"), expected.first_to_games, expected.first_to_games, first) ||
				!detail::number32(rules->find("roundsPerGame"), expected.rounds_per_game, expected.rounds_per_game, rounds))
				return "changed_set_values";
			if (const auto* repeated = rules->find("allowJinpachi"))
			{
				bool allow = false;
				if (!detail::boolean(repeated, allow) || allow != expected.allow_jinpachi)
					return "changed_set_values";
			}
		}
		return {};
	}

	inline std::string_view read_post_set(const json_value& value, command& out)
	{
		out.kind = command_kind::show_post_set;
		if (detail::is(value.find("role"), "winner"))
			out.winner = true;
		else if (!detail::is(value.find("role"), "loser"))
			return "bad_role";
		if (!detail::boolean(value.find("challengerWaiting"), out.challenger_waiting))
			return "bad_challenger";
		const auto* actions = value.find("allowedActions");
		if (!actions || actions->kind != json_kind::array || actions->items.empty() || actions->items.size() > 8)
			return "bad_actions";
		for (const auto& action : actions->items)
		{
			if (!detail::plain_text(&action, 1, 32))
				return "bad_actions";
			// join_battle: the next set against the same opponent (next_set), offered to both players.
			const bool common = action.text == "rematch_same" || action.text == "leave_seat" || action.text == "watch" ||
			                    action.text == "leave_queue" || action.text == "join_battle";
			const bool loser = action.text == "random_other" || action.text == "change_fighter";
			if ((!common && !loser) || (out.winner && loser) ||
				std::find(out.allowed_actions.begin(), out.allowed_actions.end(), action.text) != out.allowed_actions.end())
				return "bad_actions";
			out.allowed_actions.push_back(action.text);
		}
		return {};
	}

	inline std::string_view read_end_reason(const json_value& value, command& out, const json_value* type)
	{
		out.kind = detail::is(type, "end_set") ? command_kind::end_set : command_kind::choice_rejected;
		if (!detail::plain_text(value.find("reason"), 1, 128))
			return "bad_reason";
		out.reason = value.find("reason")->text;
		if (out.kind == command_kind::end_set && out.reason != "completed" && out.reason != "left" && out.reason != "service_lost" && out.reason != "result_disputed")
			return "bad_reason";
		return {};
	}

	inline std::string_view read_chat(const json_value& value, command& out)
	{
		out.kind = command_kind::chat_overlay;
		if (!detail::boolean(value.find("visible"), out.visible))
			return "bad_chat";
		const auto* additions = value.find("additions");
		if (!additions || additions->kind != json_kind::array || additions->items.size() > 20)
			return "bad_chat";
		for (const auto& message : additions->items)
		{
			if (message.kind != json_kind::object || !detail::identifier(message.find("messageId")) || !detail::plain_text(message.find("at"), 1, 40))
				return "bad_chat";
			if (const auto* content = message.find("text"); content && content->kind != json_kind::null && !detail::plain_text(content, 0, 1120))
				return "bad_chat";
			if (const auto* name = message.find("authorName"); name && !detail::plain_text(name, 1, 16))
				return "bad_chat";
		}
		out.additions = *additions;
		return {};
	}

	inline std::string_view read_stream(const json_value& value, command& out)
	{
		out.kind = command_kind::stream_status;
		if (!detail::plain_text(value.find("status"), 1, 32))
			return "bad_stream_status";
		out.status = value.find("status")->text;
		if (out.status != "active" && out.status != "paused_slow" && out.status != "ended" && out.status != "suspended")
			return "bad_stream_status";
		if (const auto* reason = value.find("reason"); reason && reason->kind != json_kind::null)
		{
			if (!detail::plain_text(reason, 1, 128))
				return "bad_reason";
			out.reason = reason->text;
		}
		return {};
	}

	inline std::string_view read_cards(const json_value& value, command& out)
	{
		// Card visibility, set mode and fighter captions. Language and region remain required for wire compatibility;
		// player names and set rules come from the manifest.
		out.kind = command_kind::vs_cards;
		if (!detail::boolean(value.find("visible"), out.visible))
			return "bad_vs_cards";
		for (const auto& [key, index] : {std::pair<std::string_view, std::size_t>{"left", 0}, {"right", 1}})
		{
			const auto* card = value.find(key);
			if (!card || card->kind != json_kind::object || !detail::plain_text(card->find("language"), 0, max_card_language_bytes) ||
				!detail::plain_text(card->find("region"), 0, max_card_region_bytes))
				return "bad_vs_cards";
			out.origins[index] = {card->find("language")->text, card->find("region")->text};
			// The fighter's caption (ranked-notices-1): 1..24 bytes of printable ASCII.
			if (const auto* character = card->find("character"))
			{
				const bool ascii = detail::plain_text(character, 1, max_card_character_bytes) &&
				                   std::all_of(character->text.begin(), character->text.end(), [](char c)
									   {
										   return static_cast<unsigned char>(c) <= 0x7e;
									   });
				if (ascii)
					out.characters[index] = character->text;
				else
					out.ignored.push_back("vs_cards." + std::string(key) + ".character");
			}
		}
		if (const auto* mode = value.find("mode"))
		{
			if (detail::is(mode, "ranked"))
				out.mode = card_mode::ranked;
			else if (detail::is(mode, "quick"))
				out.mode = card_mode::quick;
			else
				out.ignored.push_back("vs_cards.mode");
		}
		return {};
	}

	inline std::string_view read_stakes(const json_value& value, command& out)
	{
		// A Ranked set's stakes (display only, applied at once and acknowledged): what a set win and a set loss give each player,
		// {"left":{"win","winRank","loss","lossRank"},"right":{...}}. The envelope names the set they belong to; the launcher sends
		// them again after next_set and whenever they change, and each replaces the last.
		out.kind = command_kind::rank_stakes;
		for (const auto& [key, index] : {std::pair<std::string_view, std::size_t>{"left", 0}, {"right", 1}})
		{
			const auto* side = value.find(key);
			const auto stakes = pbd::rank_stakes::read_side(side);
			if (!stakes)
				return "bad_rank_stakes";
			out.stakes[index] = *stakes;
			// The placing player's count (ranked-notices-1), display only.
			out.placements[index] = pbd::rank_stakes::read_placement(side);
			if (const auto* given = side->find("placement"); given && given->kind != json_kind::null && !out.placements[index])
				out.ignored.push_back("rank_stakes." + std::string(key) + ".placement");
		}
		return {};
	}

	inline result<command> parse_command(std::string_view text, const manifest& expected, std::uint64_t serial, std::uint32_t current_game)
	{
		auto parsed = detail::json_parser(text).parse();
		if (!parsed.value)
			return {{}, parsed.error};
		const auto& value = *parsed.value;
		command out;
		if (value.kind != json_kind::object || !detail::is(value.find("schema"), schema_of(expected)) ||
			!detail::is(value.find("sessionId"), expected.session_id) || !detail::is(value.find("setId"), expected.set_id) ||
			!detail::number(value.find("serial"), 1, max_safe_integer, out.serial) || out.serial != serial)
			return {{}, "bad_command_envelope"};
		const auto* type = value.find("type");
		std::string_view refusal;
		if (detail::is(type, "continue_game"))
			refusal = read_continuation(value, expected, out, current_game);
		else if (detail::is(type, "next_set"))
			refusal = read_next_set(value, expected, out);
		else if (detail::is(type, "show_post_set"))
			refusal = read_post_set(value, out);
		else if (detail::is(type, "choice_rejected") || detail::is(type, "end_set"))
			refusal = read_end_reason(value, out, type);
		else if (detail::is(type, "chat_overlay"))
			refusal = read_chat(value, out);
		else if (detail::is(type, "stream_status"))
			refusal = read_stream(value, out);
		else if (detail::is(type, "vs_cards"))
			refusal = read_cards(value, out);
		else if (detail::is(type, "rank_stakes"))
			refusal = read_stakes(value, out);
		else
			return {{}, "unknown_command"};
		if (!refusal.empty())
			return {{}, std::string(refusal)};
		return {std::move(out), {}};
	}

} // namespace pbd::set_channel
