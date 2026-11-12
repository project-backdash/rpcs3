#pragma once

#include "pbd_game_overlay_model.h"

namespace pbd::game_overlay
{
	namespace detail
	{
		inline bool parse_u64(std::string_view text, std::uint64_t& out) noexcept
		{
			if (text.empty())
				return false;
			const auto [tail, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
			return ec == std::errc{} && tail == text.data() + text.size();
		}

		inline bool parse_u32(std::string_view text, std::uint32_t& out) noexcept
		{
			std::uint64_t wide = 0;
			if (!parse_u64(text, wide) || wide > 0xffffffffull)
				return false;
			out = static_cast<std::uint32_t>(wide);
			return true;
		}

		inline int hex(char c) noexcept
		{
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			return -1;
		}

		inline std::optional<std::string> decode(std::string_view input, std::size_t limit)
		{
			std::string out;
			out.reserve(input.size());
			for (std::size_t i = 0; i < input.size(); i++)
			{
				unsigned char c = static_cast<unsigned char>(input[i]);
				if (c == '%')
				{
					if (i + 2 >= input.size())
						return {};
					const int hi = hex(input[i + 1]), lo = hex(input[i + 2]);
					if (hi < 0 || lo < 0)
						return {};
					c = static_cast<unsigned char>((hi << 4) | lo);
					i += 2;
				}
				if (c < 0x20 || c == 0x7f || out.size() >= limit)
					return {};
				out.push_back(static_cast<char>(c));
			}
			return out;
		}

		// Splits a key's value at '|' into at most N fields; returns the field count, or N + 1 when there are more. A field's own bar
		// is percent-encoded.
		template <std::size_t N>
		inline std::size_t split(std::string_view value, std::array<std::string_view, N>& parts) noexcept
		{
			std::size_t count = 0;
			while (true)
			{
				if (count == N)
					return N + 1;
				const auto bar = value.find('|');
				parts[count++] = value.substr(0, bar);
				if (bar == std::string_view::npos)
					return count;
				value.remove_prefix(bar + 1);
			}
		}

		inline std::string_view take_line(std::string_view& text)
		{
			const auto end = text.find('\n');
			auto line = text.substr(0, end);
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			return line;
		}

		// score=<first to>|<games>|<left name>|<right name>. A game after the set was decided, a side past the set's length or any
		// letter other than L and R refuses the file, so the dots can never show a score the set cannot have.
		inline std::optional<set_score> parse_score(std::string_view value)
		{
			std::array<std::string_view, 4> parts{};
			for (std::size_t i = 0; i + 1 < parts.size(); i++)
			{
				const auto bar = value.find('|');
				if (bar == std::string_view::npos)
					return {};
				parts[i] = value.substr(0, bar);
				value.remove_prefix(bar + 1);
			}
			parts[3] = value;
			if (parts[3].find('|') != std::string_view::npos)
				return {}; // a name's own bar is percent-encoded
			set_score out;
			out.present = true;
			if (!parse_u32(parts[0], out.first_to) || !out.first_to || out.first_to > max_score_first_to)
				return {};
			std::uint32_t left = 0, right = 0;
			for (const char c : parts[1])
			{
				if (left == out.first_to || right == out.first_to)
					return {};
				if (c == 'L')
					left++;
				else if (c == 'R')
					right++;
				else
					return {};
			}
			out.games = std::string(parts[1]);
			auto left_name = decode(parts[2], max_score_name_bytes), right_name = decode(parts[3], max_score_name_bytes);
			if (!left_name || !right_name || left_name->empty() || right_name->empty())
				return {};
			out.left = std::move(*left_name);
			out.right = std::move(*right_name);
			return out;
		}

		// rank=<up|down|attained|same>|<rank index>|<rating>|<change>: the change is a whole number, signed or not ("+11", "11",
		// "-9").
		inline std::optional<rank_result> parse_rank(std::string_view value)
		{
			std::array<std::string_view, 4> parts{};
			rank_result out;
			if (split(value, parts) != 4)
				return {};
			if (parts[0] == "up")
				out.kind = rank_change::up;
			else if (parts[0] == "down")
				out.kind = rank_change::down;
			else if (parts[0] == "attained")
				out.kind = rank_change::attained;
			else if (parts[0] == "same")
				out.kind = rank_change::same;
			else
				return {};
			auto change = parts[3];
			const bool negative = !change.empty() && change.front() == '-';
			if (!change.empty() && (change.front() == '-' || change.front() == '+'))
				change.remove_prefix(1);
			std::uint32_t magnitude = 0;
			if (!parse_u32(parts[1], out.rank) || out.rank > max_rank_index || !parse_u32(parts[2], out.rating) || out.rating > max_rating ||
				!parse_u32(change, magnitude) || magnitude > static_cast<std::uint32_t>(max_rating_change))
				return {};
			out.change = negative ? -static_cast<std::int32_t>(magnitude) : static_cast<std::int32_t>(magnitude);
			return out;
		}

		// placing=<before>|<after>|<total>: a total of 1..10 sets, before <= after <= total, and at most the one set this panel
		// rated between them.
		inline std::optional<placing> parse_placing(std::string_view value)
		{
			std::array<std::string_view, 3> parts{};
			placing out;
			if (split(value, parts) != 3 || !parse_u32(parts[0], out.before) || !parse_u32(parts[1], out.after) || !parse_u32(parts[2], out.total) ||
				!out.total || out.total > max_placement_total || out.before > out.after || out.after > out.total || out.after - out.before > 1)
				return {};
			out.present = true;
			return out;
		}

		// A plate token: r<0..34> a rank plate, p<done>/<total> a placement plate (a total of 1..10, done at most the total), -
		// none.
		inline std::optional<plate> parse_plate(std::string_view token)
		{
			plate out;
			if (token == "-")
				return out;
			if (token.size() >= 2 && token[0] == 'r')
			{
				out.shown = plate::kind::rank;
				if (!parse_u32(token.substr(1), out.rank) || out.rank > max_rank_index)
					return {};
				return out;
			}
			const auto slash = token.find('/');
			if (token.size() < 4 || token[0] != 'p' || slash == std::string_view::npos)
				return {};
			out.shown = plate::kind::placing;
			if (!parse_u32(token.substr(1, slash - 1), out.done) || !parse_u32(token.substr(slash + 1), out.total) || !out.total ||
				out.total > max_placement_total || out.done > out.total)
				return {};
			return out;
		}

		// A field of printable ASCII only (a fighter's name as the game prints it), 0..limit bytes.
		inline std::optional<std::string> decode_ascii(std::string_view value, std::size_t limit)
		{
			auto text = decode(value, limit);
			if (!text || std::any_of(text->begin(), text->end(), [](char c)
							 {
								 return static_cast<unsigned char>(c) > 0x7e;
							 }))
				return {};
			return text;
		}
	} // namespace detail

	// Percent-encoded PBD-MENU-1 fields are bounded and reject unknown or duplicate keys.
	// Set details need a scoreboard; drawn games repeat their number, and decided games follow its score.
	namespace detail
	{
		class panel_parser
		{
			snapshot out;
			std::array<bool, 29> seen{};
			bool needs_score = false;

			bool once(std::size_t n)
			{
				if (seen[n])
					return false;
				seen[n] = true;
				return true;
			}

			static bool flag(std::string_view value)
			{
				return value == "0" || value == "1";
			}

			const char* panel_fields(std::string_view key, std::string_view value)
			{
				if (key == "generation")
				{
					if (!once(0) || !detail::parse_u64(value, out.generation) || !out.generation)
						return "bad_generation";
				}
				else if (key == "visible")
				{
					if (!once(1) || !flag(value))
						return "bad_visible";
					out.visible = value == "1";
				}
				else if (key == "kind")
				{
					if (!once(2))
						return "duplicate_kind";
					if (value == "menu")
						out.kind = panel_kind::menu;
					else if (value == "drawer")
						out.kind = panel_kind::drawer;
					else if (value == "prompt")
						out.kind = panel_kind::prompt;
					else if (value == "text")
						out.kind = panel_kind::text;
					else if (value == "challenger")
						out.kind = panel_kind::challenger;
					else if (value == "post_set")
						out.kind = panel_kind::post_set;
					else if (value == "choice_rejected")
						out.kind = panel_kind::choice_rejected;
					else if (value == "set_ended")
						out.kind = panel_kind::set_ended;
					else
						return "bad_kind";
				}
				else if (key == "command")
				{
					if (!once(18) || !detail::parse_u64(value, out.command_serial) || !out.command_serial)
						return "bad_command";
				}
				else if (key == "selected")
				{
					if (!once(4) || !detail::parse_u32(value, out.selected))
						return "bad_selected";
				}
				else if (key == "close")
				{
					if (!once(5) || !flag(value))
						return "bad_close";
					out.close_on_action = value == "1";
				}
				return nullptr;
			}

			const char* text_fields(std::string_view key, std::string_view value)
			{
				if (key == "title" || key == "subtitle" || key == "confirm" || key == "back")
				{
					const std::size_t which = key == "title" ? 6 : key == "subtitle" ? 7 :
					                                           key == "confirm"      ? 8 :
					                                                                   9;
					const std::size_t limit = key == "subtitle" ? 256 : key == "title" ? 80 :
					                                                                     40;
					if (!once(which))
						return "duplicate_text";
					auto decoded = detail::decode(value, limit);
					if (!decoded)
						return "bad_text";
					(key == "title" ? out.title : key == "subtitle" ? out.subtitle :
											  key == "confirm"      ? out.confirm :
																	  out.back) = std::move(*decoded);
				}
				else if (key == "row")
				{
					if (out.row_count >= max_file_rows)
						return "too_many_rows";
					std::array<std::string_view, 4> parts{};
					const auto count = detail::split(value, parts);
					if (count < 3 || count > 4 || !flag(parts[0]))
						return "bad_row";
					auto label = detail::decode(parts[1], 80);
					auto note = detail::decode(parts[2], 120);
					auto help = detail::decode(parts[3], max_help_bytes);
					if (!label || label->empty() || !note || !help)
						return "bad_row_text";
					auto& target = out.rows[out.row_count++];
					target.enabled = parts[0] == "1";
					target.label = std::move(*label);
					target.detail = std::move(*note);
					target.help = std::move(*help);
				}
				return nullptr;
			}

			const char* score_fields(std::string_view key, std::string_view value)
			{
				if (key == "score")
				{
					if (!once(11))
						return "duplicate_score";
					auto score = detail::parse_score(value);
					if (!score)
						return "bad_score";
					out.score = std::move(*score);
				}
				else if (key == "fighters" || key == "next")
				{
					const bool fighters = key == "fighters";
					std::array<std::string_view, 2> parts{};
					const std::size_t limit = fighters ? max_fighter_bytes : max_next_bytes;
					std::optional<std::string> left, right;
					if (!once(fighters ? 3 : 17) || detail::split(value, parts) != 2 || !(left = detail::decode(parts[0], limit)) ||
						!(right = detail::decode(parts[1], limit)))
						return fighters ? "bad_fighters" : "bad_next";
					(fighters ? out.fighters : out.next) = {std::move(*left), std::move(*right)};
					needs_score = true;
				}
				else if (key == "you")
				{
					if (!once(12) || (value != "L" && value != "R"))
						return "bad_you";
					out.you = value[0];
					needs_score = true;
				}
				return nullptr;
			}

			const char* status_fields(std::string_view key, std::string_view value)
			{
				if (key == "timer")
				{
					std::array<std::string_view, 5> parts{};
					auto& timer = out.timer;
					std::optional<std::string> title, detail_text;
					if (!once(13) || detail::split(value, parts) != 5 || !detail::parse_u64(parts[0], timer.deadline_ms) || !timer.deadline_ms ||
						!detail::parse_u32(parts[1], timer.total_ms) || !timer.total_ms || timer.total_ms > max_timer_total_ms ||
						!(title = detail::decode(parts[2], max_timer_title_bytes)) || title->empty() ||
						!(detail_text = detail::decode(parts[3], max_timer_detail_bytes)) || !flag(parts[4]))
						return "bad_timer";
					timer.present = true;
					timer.title = std::move(*title);
					timer.detail = std::move(*detail_text);
					timer.ready = parts[4] == "1";
				}
				else if (key == "chosen")
				{
					std::uint32_t row = 0;
					if (!once(14) || !detail::parse_u32(value, row))
						return "bad_chosen";
					out.chosen = row;
				}
				else if (key == "status")
				{
					auto decoded = detail::decode(value, max_status_bytes);
					if (!once(15) || !decoded)
						return "bad_status";
					out.status = std::move(*decoded);
				}
				return nullptr;
			}

			const char* game_fields(std::string_view key, std::string_view value)
			{
				if (key == "dialog")
				{
					std::array<std::string_view, 2> parts{};
					std::optional<std::string> message, detail_text;
					if (!once(16) || detail::split(value, parts) != 2 || !(message = detail::decode(parts[0], max_dialog_message_bytes)) || message->empty() ||
						!(detail_text = detail::decode(parts[1], max_dialog_detail_bytes)))
						return "bad_dialog";
					out.question = {true, std::move(*message), std::move(*detail_text)};
				}
				else if (key == "base")
				{
					std::array<std::string_view, 3> parts{};
					std::optional<std::string> label, note;
					if (out.base_count >= max_base_rows || detail::split(value, parts) != 3 || !flag(parts[0]) ||
						!(label = detail::decode(parts[1], 80)) || label->empty() || !(note = detail::decode(parts[2], 120)))
						return "bad_base";
					out.base[out.base_count++] = {parts[0] == "1", std::move(*label), std::move(*note)};
				}
				else if (key == "game")
				{
					std::array<std::string_view, 5> parts{};
					game_line game;
					std::optional<std::string> stage;
					if (out.game_count >= max_game_lines || detail::split(value, parts) != 5 || !detail::parse_u32(parts[0], game.number) ||
						(parts[1] != "L" && parts[1] != "R" && parts[1] != "D") || !detail::parse_u32(parts[2], game.winner_rounds) || !detail::parse_u32(parts[3], game.loser_rounds) || game.winner_rounds > 9 || game.loser_rounds > 9 ||
						!(stage = detail::decode(parts[4], max_stage_bytes)))
						return "bad_game";
					game.winner = parts[1][0];
					game.stage = std::move(*stage);
					out.games[out.game_count++] = std::move(game);
					needs_score = true;
				}
				else if (key == "rank")
				{
					auto rank = detail::parse_rank(value);
					if (!once(19) || !rank)
						return "bad_rank";
					out.rank = *rank;
				}
				return nullptr;
			}

			// The Ranked notices' keys (ranked-notices-1). Each is accepted once; complete() checks the kind and keys each needs.
			const char* notice_fields(std::string_view key, std::string_view value)
			{
				if (key == "placing")
				{
					auto placement = detail::parse_placing(value);
					if (!once(20) || !placement)
						return "bad_placing";
					out.placement = *placement;
				}
				else if (key == "rival")
				{
					auto text = detail::decode(value, max_rival_bytes);
					if (!once(21) || !text || text->empty())
						return "bad_rival";
					out.rival = std::move(*text);
				}
				else if (key == "link")
				{
					std::array<std::string_view, 2> parts{};
					std::optional<std::string> words;
					if (!once(22) || detail::split(value, parts) != 2 || !detail::parse_u32(parts[0], out.link.bars) || out.link.bars > max_bars ||
						!(words = detail::decode(parts[1], max_link_words_bytes)) || words->empty())
						return "bad_link";
					out.link.present = true;
					out.link.words = std::move(*words);
				}
				else if (key == "countdown_row")
				{
					std::uint32_t row = 0;
					if (!once(23) || !detail::parse_u32(value, row))
						return "bad_countdown_row";
					out.countdown_row = row;
				}
				else if (key == "card")
				{
					std::array<std::string_view, 3> parts{};
					std::optional<std::string> character, name;
					std::optional<plate> shown;
					if (!once(24) || detail::split(value, parts) != 3 || !(character = detail::decode_ascii(parts[0], max_card_character_bytes)) ||
						!(name = detail::decode(parts[1], max_card_name_bytes)) || name->empty() || !(shown = detail::parse_plate(parts[2])))
						return "bad_card";
					out.card = {true, std::move(*character), std::move(*name), *shown};
				}
				else if (key == "ping")
				{
					std::array<std::string_view, 2> parts{};
					if (!once(25) || detail::split(value, parts) != 2 || !detail::parse_u32(parts[0], out.ping.bars) || !out.ping.bars ||
						out.ping.bars > max_bars || !detail::parse_u32(parts[1], out.ping.ms) || out.ping.ms > max_ping_ms)
						return "bad_ping";
					out.ping.present = true;
				}
				else if (key == "pill")
				{
					if (!once(26) || value != "placement")
						return "bad_pill";
					out.placement_pill = true;
				}
				else if (key == "opponent")
				{
					std::array<std::string_view, 2> parts{};
					std::optional<plate> shown;
					if (!once(28) || detail::split(value, parts) != 2 || !(shown = detail::parse_plate(parts[0])) || shown->shown == plate::kind::none)
						return "bad_opponent";
					if (shown->shown == plate::kind::rank)
					{
						if (!detail::parse_u32(parts[1], out.opponent.rating) || out.opponent.rating > max_rating)
							return "bad_opponent";
					}
					else if (parts[1] != "-")
						return "bad_opponent";
					out.opponent.shown = *shown;
				}
				return nullptr;
			}

			// The kind and keys each notice key needs.
			const char* notice_rules() const
			{
				const bool post_set = out.kind == panel_kind::post_set, challenger = out.kind == panel_kind::challenger;
				if (out.placement.present && (!post_set || !out.you))
					return "bad_placing";
				if (out.opponent.shown.shown != plate::kind::none && (!post_set || !out.you || !out.score.present))
					return "bad_opponent";
				if (!out.rival.empty() && !out.you && !challenger)
					return "bad_rival";
				if (out.link.present && !post_set)
					return "bad_link";
				if (out.countdown_row && (!out.timer.present || *out.countdown_row >= out.row_count))
					return "bad_countdown_row";
				if (out.card.present && !challenger)
					return "bad_card";
				if (out.ping.present && !challenger)
					return "bad_ping";
				if (out.placement_pill && !challenger)
					return "bad_pill";
				return nullptr;
			}

			const char* field(std::string_view key, std::string_view value)
			{
				if (key == "placing" || key == "rival" || key == "link" || key == "countdown_row" || key == "card" || key == "ping" || key == "pill" || key == "opponent")
					return notice_fields(key, value);
				if ((key == "generation") || (key == "visible") || (key == "kind") || (key == "command") || (key == "selected") || (key == "close"))
					return panel_fields(key, value);
				else if ((key == "title" || key == "subtitle" || key == "confirm" || key == "back") || (key == "row"))
					return text_fields(key, value);
				else if ((key == "score") || (key == "fighters" || key == "next") || (key == "you"))
					return score_fields(key, value);
				else if ((key == "timer") || (key == "chosen") || (key == "status"))
					return status_fields(key, value);
				else if ((key == "dialog") || (key == "base") || (key == "game") || (key == "rank"))
					return game_fields(key, value);
				return "unknown_key";
			}

			parse_result complete()
			{
				if (!seen[0] || !seen[1])
					return {{}, "missing_required"};
				if (needs_score && !out.score.present)
					return {{}, "missing_score"};
				// The row this player already chose cannot be chosen again; the others may stay open (a Rematch chosen while Next
				// opponent is still offered).
				if (out.chosen && (*out.chosen >= out.row_count || out.rows[*out.chosen].enabled))
					return {{}, "bad_chosen"};
				if (const char* error = notice_rules())
					return {{}, error};
				if (out.base_count && !out.question.present)
					return {{}, "bad_base"};
				if (out.game_count)
				{
					// Each line names the game it was: the decided games before it plus one; a draw is played again under that number.
					std::string decided;
					for (std::uint32_t i = 0; i < out.game_count; i++)
					{
						const auto& game = out.games[i];
						if (game.number != decided.size() + 1 || (game.winner != 'D' && game.winner_rounds <= game.loser_rounds))
							return {{}, "bad_game"};
						if (game.winner != 'D')
							decided += game.winner;
					}
					if (decided != out.score.games)
						return {{}, "bad_game"};
				}
				if (out.visible && (out.title.empty() || (out.row_count && out.selected >= out.row_count)))
					return {{}, "bad_visible_panel"};
				return {std::move(out), {}};
			}

		public:
			parse_result read_lines(std::string_view text)
			{
				while (!text.empty())
				{
					const auto line = detail::take_line(text);
					if (line.empty())
						continue;
					const auto eq = line.find('=');
					if (eq == std::string_view::npos)
						return {{}, "bad_line"};
					const auto key = line.substr(0, eq), value = line.substr(eq + 1);
					if (const char* error = field(key, value))
						return {{}, error};
				}
				return complete();
			}
		};
	} // namespace detail

	inline parse_result parse(std::string_view text)
	{
		if (text.size() > max_file_bytes)
			return {{}, "file_too_large"};
		if (detail::take_line(text) != "PBD-MENU-1")
			return {{}, "bad_header"};
		return detail::panel_parser{}.read_lines(text);
	}
} // namespace pbd::game_overlay
