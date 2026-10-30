#pragma once

#include "pbd_set_commands.h"

namespace pbd::set_channel
{
	inline std::string utc_now()
	{
		const auto now = std::chrono::system_clock::now();
		const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
		const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
		const auto clock = std::chrono::system_clock::to_time_t(seconds);
		std::tm utc{};
#ifdef _WIN32
		if (gmtime_s(&utc, &clock) != 0)
			return {};
#else
		if (!gmtime_r(&clock, &utc))
			return {};
#endif
		char date[32]{};
		char fraction[8]{};
		if (!std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc))
			return {};
		std::snprintf(fraction, sizeof(fraction), ".%03lldZ", static_cast<long long>(millis));
		return std::string(date) + fraction;
	}

	inline bool pad_data(const json_value* value, std::uint32_t frames)
	{
		if (!value || value->kind != json_kind::string || frames < 1 || frames > 240)
			return false;
		const auto& text = value->text;
		const std::size_t bytes = static_cast<std::size_t>(frames) * 8;
		if (text.size() != (bytes * 8 + 5) / 6)
			return false;
		int last = 0;
		for (const unsigned char c : text)
		{
			last = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 :
			                                    c >= '0' && c <= '9'     ? c - '0' + 52 :
			                                    c == '-'                 ? 62 :
			                                    c == '_'                 ? 63 :
			                                                               -1;
			if (last < 0)
				return false;
		}
		const auto padding_bits = static_cast<unsigned>((text.size() * 6) % 8);
		return (static_cast<unsigned>(last) & ((1u << padding_bits) - 1u)) == 0;
	}

	struct event_payload_reader
	{
		const json_value& value;
		const manifest& expected;
		std::optional<bool> post_winner;
		std::uint32_t game = 0, stage = 0, left = 0, right = 0;
		std::uint64_t serial = 0, first = 0;
		bool game_number()
		{
			return detail::number32(value.find("gameNumber"), 1, 19, game);
		}

		bool picks()
		{
			if (in_game(expected))
				return detail::number32(value.find("stageId"), 0, max_stage_id, stage) &&
				       detail::number32(value.find("leftCharacterRow"), 0, max_character_row, left) &&
				       detail::number32(value.find("rightCharacterRow"), 0, max_character_row, right);
			return detail::number32(value.find("stageId"), expected.stage_id, expected.stage_id, stage) &&
			       detail::number32(value.find("leftCharacterRow"), expected.left.row, expected.left.row, left) &&
			       detail::number32(value.find("rightCharacterRow"), expected.right.row, expected.right.row, right);
		}

		bool winner()
		{
			return detail::is(value.find("winner"), "left") || detail::is(value.find("winner"), "right");
		}

		bool game_preparing()
		{
			const auto* start = value.find("start");
			const auto* clean = value.find("cleanStart");
			const auto* required = start ? start->find("roundWitnessRequired") : nullptr;
			return game_number() && start && start->kind == json_kind::object && clean && clean->kind == json_kind::object &&
			       detail::is(start->find("encoding"), "pbd-spectator-start-v1") && required && required->kind == json_kind::boolean &&
			       required->truth && !start->find("nativeRound") && !value.find("stageId") &&
			       !value.find("leftCharacterRow") && !value.find("rightCharacterRow");
		}

		bool game_result()
		{
			score rounds;
			if (!game_number() || !picks() || !parse_score(value.find("roundWins"), expected.rounds_per_game, rounds))
				return false;
			if (const auto* state = value.find("rematchState"))
			{
				const auto actual = pbd::rematch::read_state(state);
				std::uint32_t end = 0;
				if (!actual || actual->game != game || actual->selected.rows != std::array<std::uint32_t, 2>{left, right} || actual->selected.stage != stage || !detail::number32(value.find("finalFrame"), 1, 0xffffffffu, end) ||
					actual->end_frame != end)
					return false;
			}
			// A drawn game names no winner: both fighters reached the rounds to win on the same final round (a double K.O. or a
			// time-out with equal health). It does not count, and the lobby plays the same game number again.
			if (const auto* outcome = value.find("outcome"))
			{
				const auto* named = value.find("winner");
				return detail::is(outcome, "draw") && named && named->kind == json_kind::null &&
				       rounds.left == expected.rounds_per_game && rounds.right == expected.rounds_per_game;
			}
			return winner() && (detail::is(value.find("winner"), "left") ? rounds.left == expected.rounds_per_game && rounds.right < expected.rounds_per_game :
																		   rounds.right == expected.rounds_per_game && rounds.left < expected.rounds_per_game);
		}

		bool set_ended()
		{
			score games;
			return detail::is(value.find("reason"), "completed") && winner() && parse_score(value.find("score"), expected.first_to_games, games) &&
			       (detail::is(value.find("winner"), "left") ? games.left == expected.first_to_games && games.right < expected.first_to_games :
															   games.right == expected.first_to_games && games.left < expected.first_to_games);
		}

		bool post_set_choice()
		{
			const auto* choice = value.find("choice");
			if (!choice || choice->kind != json_kind::object || !post_winner.has_value())
				return false;
			if (detail::is(choice->find("action"), "leave_seat"))
				return !choice->find("stage") && !choice->find("characterRow");
			if (!detail::is(choice->find("action"), "rematch"))
				return false;
			if (*post_winner)
				return detail::is(choice->find("stage"), "same") && !choice->find("characterRow");
			if (!detail::is(choice->find("stage"), "same") && !detail::is(choice->find("stage"), "random_other"))
				return false;
			return !choice->find("characterRow") || detail::number32(choice->find("characterRow"), 0, std::numeric_limits<std::uint32_t>::max(), left);
		}

		bool leave()
		{
			// The optional cause names what ended the game: the leave chord, the launcher's stop file or the game window's close.
			// The launcher can then tell a stop it wrote for its own reasons from one the player asked for.
			const auto* cause = value.find("cause");
			// peer_wait: the player left while this PC waited for the other PC's game (the waiting box's Leave); it has no cause.
			if (detail::is(value.find("action"), "peer_wait"))
				return !cause;
			if (cause && !detail::is(cause, "start_select") && !detail::is(cause, "stop_request") && !detail::is(cause, "window_closed"))
				return false;
			return detail::is(value.find("action"), "leave_queue") || detail::is(value.find("action"), "leave_seat") ||
			       detail::is(value.find("action"), "leave_lobby") || detail::is(value.find("action"), "exit_pbd");
		}

		bool confirmed_inputs()
		{
			std::uint32_t count = 0;
			const auto* digest = value.find("digest");
			return expected.publish_confirmed_inputs && game_number() && detail::is(value.find("side"), expected.local_side == side::left ? "left" : "right") &&
			       detail::number(value.find("firstFrame"), 0, max_safe_integer - 240, first) && detail::number32(value.find("frameCount"), 1, 240, count) &&
			       detail::is(value.find("encoding"), "pbd-pad-v1") && pad_data(value.find("data"), count) && detail::plain_text(digest, 64, 64) &&
			       std::all_of(digest->text.begin(), digest->text.end(), [](unsigned char c)
					   {
						   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
					   });
		}

		bool valid(std::string_view type)
		{
			if (value.kind != json_kind::object)
				return false;
			if (type == "game_preparing")
				return game_preparing();
			if (type == "game_started")
				return game_number() && picks();
			if (type == "game_result")
				return game_result();
			if (type == "between_games_ready")
				return game_number();
			if (type == "set_ended")
				return set_ended();
			if (type == "post_set_choice")
				return post_set_choice();
			if (type == "leave")
				return leave();
			if (type == "confirmed_inputs")
				return confirmed_inputs();
			if (type == "contract_error")
				return detail::identifier(value.find("code")) && detail::plain_text(value.find("detail"), 0, 1024);
			if (type == "command_applied")
				return detail::number(value.find("commandSerial"), 1, max_safe_integer, serial);
			return false;
		}
	};

	inline bool valid_event_payload(std::string_view type, const json_value& value, const manifest& expected, std::optional<bool> post_winner)
	{
		return event_payload_reader{value, expected, post_winner}.valid(type);
	}

} // namespace pbd::set_channel
