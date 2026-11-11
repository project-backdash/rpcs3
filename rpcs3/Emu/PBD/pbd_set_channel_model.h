#pragma once

// A per-process set channel carries host metadata, never guest memory. Bounded parsing and immutable serials keep a malformed
// launcher command from changing match rules or advancing play before the lobby service has agreed on the previous game.

#include "pbd_env.h"
#include "pbd_custom_wire.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "pbd_json_reader.h"
#include "pbd_rematch_json.h"
#include "../Cell/pbd_game_profile.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace pbd::set_channel
{
	struct publication_result
	{
		bool published = false, retryable = false;
		const char* error = "event_publish_failed";
	};
	// A brief sharing denial must not replay an applied command. Retry its exact bytes within one frame's host budget.
	template <typename Attempt, typename Pause, typename Now>
	publication_result retry_publication(Attempt attempt, Pause pause, Now now)
	{
		const auto deadline = now() + std::chrono::milliseconds(16);
		for (;;)
		{
			const auto result = attempt();
			if (result.published || !result.retryable || now() >= deadline)
				return result;
			pause();
		}
	}
	// Set files are read with the bounded JSON reader, whose file limit is the channel's.
	using namespace pbd::json;

	// Contract 1 fixes both fighters and the stage before the game starts. Contract 2 adds the game's own selection: the players
	// choose in the stock character and stage select, and the events report what the game applied. A set's events and commands
	// carry the schema of its manifest, so either side can tell which rules a file was written under.
	inline constexpr std::string_view schema = "pbd-set-contract-1";
	inline constexpr std::string_view schema_in_game = "pbd-set-contract-2";
	// Native bounds of an applied pick: the character table has 85 rows and stock stage lists hold ids up to 0x25.
	inline constexpr std::uint32_t max_character_row = 84;
	inline constexpr std::uint32_t max_stage_id = 0x25;
	// The in-game select's countdowns, in whole seconds of boundaries.
	inline constexpr std::uint32_t min_selection_seconds = 5, max_selection_seconds = 99;
	// The highest rank a fighter may carry: the game's ranks run from 0 (Beginner) to 36 (pbd::game_profile::rank_names).
	inline constexpr std::uint32_t max_rank = 36;

	enum class side
	{
		left,
		right
	};
	// The PLAYER DATA bar's games won and lost (pbd::game_profile), each 0..99999.
	using game_record = pbd::game_profile::record;
	// One entry of an in-game fighter's record table: a character's rows in the game's table and that character's record.
	struct record_group
	{
		std::vector<std::uint32_t> rows;
		game_record record;
	};
	inline constexpr std::size_t max_record_groups = 64;

	// No character means the player chooses one in the game's own select (contract 2, in-game selection). The rank is the
	// game's rank index the fight shows beside the name (a Ranked set's rank; 0, Beginner, when the manifest names none). The
	// records are the player's games in this mode from before this set, display only: one for a fixed fighter, a table by
	// character rows for one who picks in the game's select. None means unknown, and the bar shows zeros.
	struct rank_group
	{
		std::vector<std::uint32_t> rows;
		std::uint32_t rank = 0;
	};
	struct fighter
	{
		std::string member_id;
		std::string name;
		std::string character_id;
		std::uint32_t row = 0;
		std::uint32_t rank = 0;
		std::optional<std::vector<rank_group>> ranks;
		bool has_character = true;
		std::optional<game_record> record;
		std::optional<std::vector<record_group>> records;
		std::optional<pbd::custom_wire::drawn> look;
		std::vector<std::uint8_t> look_table;
		bool custom_fields = false;
	};
	struct score
	{
		std::uint32_t left = 0;
		std::uint32_t right = 0;
	};
	// fixed: the agreed fighters and stage are entered by the automatic pre-selection before the first fight (contract 1, and
	// contract 2 with "mode":"fixed"). in_game: both players choose in the stock select, the Right side's game owns the stage
	// select, and each countdown ends with the local player's own press when it reaches zero.
	enum class selection_mode
	{
		fixed,
		in_game
	};
	struct selection_rules
	{
		selection_mode mode = selection_mode::fixed;
		std::uint32_t character_seconds = 0, stage_seconds = 0;
		side stage_picker = side::right;
	};
	struct manifest
	{
		std::string session_id, set_id, match_id, local_member_id, build, compat;
		side local_side = side::left;
		fighter left, right;
		std::uint32_t game_number = 0, stage_id = 0, first_to_games = 0, rounds_per_game = 0;
		score games;
		bool publish_confirmed_inputs = false;
		bool rank_profiles = false;
		bool allow_jinpachi = false;
		std::uint32_t contract = 1;
		selection_rules selection;
		// Display-only optional fields that were present but invalid, by name ("left.record"): read as left out, never a
		// refusal, since a refused manifest would end the set. The set's start logs them.
		std::vector<std::string> ignored;
	};
	struct capabilities
	{
		std::string build, compat;
		std::vector<std::uint32_t> character_rows, stage_ids;
	};

	inline bool in_game(const manifest& m)
	{
		return m.selection.mode == selection_mode::in_game;
	}
	inline std::string_view schema_of(const manifest& m)
	{
		return m.contract == 2 ? schema_in_game : schema;
	}

	// A "record": {"wins":<n>,"losses":<n>}, each 0..99999; anything else is none.
	inline std::optional<game_record> read_record(const json_value* value)
	{
		game_record out;
		if (!value || value->kind != json_kind::object || !detail::number32(value->find("wins"), 0, pbd::game_profile::record_max, out.wins) ||
			!detail::number32(value->find("losses"), 0, pbd::game_profile::record_max, out.losses))
			return {};
		return out;
	}

	// A "records" table: [{"rows":[<row>,...],"wins":<n>,"losses":<n>},...], at most 64 entries, each row 0..84 and in one entry
	// only. Anything else is none.
	inline std::optional<std::vector<record_group>> read_record_table(const json_value* value)
	{
		if (!value || value->kind != json_kind::array || value->items.size() > max_record_groups)
			return {};
		std::vector<record_group> out;
		std::array<bool, max_character_row + 1> taken{};
		for (const auto& item : value->items)
		{
			const auto record = read_record(&item);
			const auto* rows = record ? item.find("rows") : nullptr;
			if (!rows || rows->kind != json_kind::array || rows->items.empty())
				return {};
			record_group group{{}, *record};
			for (const auto& row : rows->items)
			{
				std::uint32_t at = 0;
				if (!detail::number32(&row, 0, max_character_row, at) || taken[at])
					return {};
				taken[at] = true;
				group.rows.push_back(at);
			}
			out.push_back(std::move(group));
		}
		return out;
	}

	// One side's record fields: "record" for a fixed fighter, "records" for one who picks in the game's select (the other key is
	// not read). Left out or null is none; an invalid one is none as well and is named in `ignored` as `name` plus the key.
	inline void read_records(const json_value& side_value, std::string_view name, bool in_game_selection, std::optional<game_record>& record,
		std::optional<std::vector<record_group>>& table, std::vector<std::string>& ignored)
	{
		const std::string_view key = in_game_selection ? "records" : "record";
		const auto* given = side_value.find(key);
		if (!given || given->kind == json_kind::null)
			return;
		if (in_game_selection)
			table = read_record_table(given);
		else
			record = read_record(given);
		if (in_game_selection ? !table : !record)
			ignored.push_back(std::string(name) + "." + std::string(key));
	}

	// An in-game set's decided games by the row each side played (left, right), counted once a command confirms each.
	using set_tally = std::array<std::array<game_record, max_character_row + 1>, 2>;

	// Each character row's base character as the game's own row table names it (the u16 at 0x4403c0 + row * 0x24, which the
	// select and a game start's "fighter" read), so all costumes of a character share one record.
	using row_characters = std::array<std::uint16_t, max_character_row + 1>;

	// The entry of `table` that holds `row`, or none.
	inline const record_group* record_group_of(const std::vector<record_group>& table, std::uint32_t row)
	{
		for (const auto& group : table)
			if (std::find(group.rows.begin(), group.rows.end(), row) != group.rows.end())
				return &group;
		return nullptr;
	}

	// The rows whose games count as `row`'s character: those of its table entry, and with `characters` every row of the same
	// base character. The entry is the one holding `row`, or else the first holding another costume of that character; none when
	// the table names the character nowhere.
	struct counted_rows
	{
		std::array<bool, max_character_row + 1> rows{};
		const record_group* entry = nullptr;
	};
	inline counted_rows counted_rows_of(const std::vector<record_group>& table, std::uint32_t row, const row_characters* characters)
	{
		const auto same = [&](std::uint32_t other)
		{
			return other == row || (characters && (*characters)[other] == (*characters)[row]);
		};
		counted_rows out;
		out.entry = record_group_of(table, row);
		for (const auto& group : table)
			if (!out.entry && std::any_of(group.rows.begin(), group.rows.end(), same))
				out.entry = &group;
		for (std::uint32_t other = 0; other <= max_character_row; ++other)
			out.rows[other] = same(other);
		if (out.entry)
			for (const auto other : out.entry->rows)
				out.rows[other] = true;
		return out;
	}

	// What the PLAYER DATA bar shows for each side in the current game (left, right): the record from before this set plus this
	// set's games. A fixed fighter adds the set's score: its own games as wins, the other side's as losses. An in-game fighter
	// takes the table entry of the character whose row it plays (`rows`, as the game holds them; counted_rows_of) and adds
	// this set's games with that character; a character the table does not name has no games before this set. None without a
	// record, or for an in-game side whose row is not a character: the bar then shows zeros.
	inline std::array<std::optional<game_record>, 2> shown_records(const manifest& set, const set_tally& tally, const std::array<std::uint32_t, 2>& rows,
		const row_characters* characters = nullptr)
	{
		using pbd::game_profile::shown;
		std::array<std::optional<game_record>, 2> out;
		const std::array<const fighter*, 2> players{&set.left, &set.right};
		const std::array<std::uint32_t, 2> games{set.games.left, set.games.right};
		for (std::size_t player = 0; player < 2; ++player)
		{
			game_record base, added;
			if (!in_game(set))
			{
				if (!players[player]->record)
					continue;
				base = *players[player]->record;
				added = {games[player], games[player ^ 1]};
			}
			else
			{
				if (!players[player]->records || rows[player] > max_character_row)
					continue;
				const auto counted = counted_rows_of(*players[player]->records, rows[player], characters);
				if (counted.entry)
					base = counted.entry->record;
				for (std::uint32_t row = 0; row <= max_character_row; ++row)
					if (counted.rows[row])
					{
						added.wins += tally[player][row].wins;
						added.losses += tally[player][row].losses;
					}
			}
			out[player] = game_record{shown(base.wins, added.wins), shown(base.losses, added.losses)};
		}
		return out;
	}

	// An in-game record table after its set: each row's games go to its character's entry (counted_rows_of), which takes the
	// row when it did not name it yet, and a character the table names nowhere gets its own entry when the set has games with it.
	// No row ends up in two entries.
	inline void fold_games(std::vector<record_group>& table, const std::array<game_record, max_character_row + 1>& games,
		const row_characters* characters = nullptr)
	{
		using pbd::game_profile::shown;
		for (std::uint32_t row = 0; row <= max_character_row; ++row)
		{
			if (!games[row].wins && !games[row].losses)
				continue;
			const auto* entry = counted_rows_of(table, row, characters).entry;
			const std::size_t at = entry ? static_cast<std::size_t>(entry - table.data()) : table.size();
			if (!entry)
				table.push_back({{row}, {}});
			else if (std::find(entry->rows.begin(), entry->rows.end(), row) == entry->rows.end())
				table[at].rows.push_back(row);
			auto& group = table[at];
			group.record = {shown(group.record.wins, games[row].wins), shown(group.record.losses, games[row].losses)};
		}
	}

	// Display-only rank groups cover all costumes of a verified character. Duplicate rows or invalid ranks discard the table.
	inline std::optional<std::vector<rank_group>> read_rank_table(const json_value* value)
	{
		if (!value || value->kind != json_kind::array || value->items.size() > max_record_groups)
			return {};
		std::array<bool, max_character_row + 1> taken{};
		std::vector<rank_group> out;
		for (const auto& item : value->items)
		{
			rank_group group;
			const auto* rows = item.find("rows");
			if (!detail::number32(item.find("rank"), 0, max_rank, group.rank) || !rows || rows->kind != json_kind::array || rows->items.empty() || rows->items.size() > taken.size())
				return {};
			for (const auto& row : rows->items)
			{
				std::uint32_t at = 0;
				if (!detail::number32(&row, 0, max_character_row, at) || taken[at])
					return {};
				taken[at] = true;
				group.rows.push_back(at);
			}
			out.push_back(std::move(group));
		}
		return out;
	}

	// Rank groups use the service's character rows: shared native fighter IDs must not merge distinct Ranked characters.
	// With a table, an unranked character is Beginner even after another character's ranked plate was shown.
	inline std::uint32_t shown_rank(const fighter& player, std::uint32_t row)
	{
		if (!player.ranks)
			return player.rank;
		if (row > max_character_row)
			return 0;
		for (const auto& group : *player.ranks)
			for (const auto other : group.rows)
				if (other == row)
					return group.rank;
		return 0;
	}

	// An in-game fighter carries no character: "character" is null or left out, and anything else is refused, because a fixed
	// character in an in-game set would never be entered.
	inline bool parse_fighter(const json_value* value, fighter& out, const capabilities& supported, bool in_game_selection = false)
	{
		if (!value || value->kind != json_kind::object || !detail::identifier(value->find("memberId")) || !detail::plain_text(value->find("name"), 1, 16))
			return false;
		const auto* character = value->find("character");
		out.member_id = value->find("memberId")->text;
		out.name = value->find("name")->text;
		if (!std::all_of(out.name.begin(), out.name.end(), [](unsigned char c)
				{
					return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' || c == '-';
				}))
			return false;
		// "rank" is optional (left out or null outside Ranked); a present one must be a rank the game has.
		out.rank = 0;
		if (const auto* rank = value->find("rank"); rank && rank->kind != json_kind::null && !detail::number32(rank, 0, max_rank, out.rank))
			return false;
		if (const auto* ranks = value->find("ranks"))
			out.ranks = read_rank_table(ranks);
		if (in_game_selection)
		{
			out.custom_fields = value->find("lookTable") || value->find("look");
			out.look_table = value->find("look") ? std::vector<std::uint8_t>{} : pbd::custom_wire::table_field(value->find("lookTable"));
			out.has_character = false;
			out.character_id.clear();
			out.row = 0;
			return !character || character->kind == json_kind::null;
		}
		if (!character || character->kind != json_kind::object || !detail::plain_text(character->find("id"), 1, 64) ||
			!detail::number32(character->find("row"), 0, std::numeric_limits<std::uint32_t>::max(), out.row) ||
			std::find(supported.character_rows.begin(), supported.character_rows.end(), out.row) == supported.character_rows.end())
			return false;
		out.character_id = character->find("id")->text;
		out.custom_fields = value->find("look") || value->find("lookTable");
		out.look = value->find("lookTable") ? std::nullopt : pbd::custom_wire::read_drawn(value->find("look"), out.row);
		return std::all_of(out.character_id.begin(), out.character_id.end(), [](unsigned char c)
			{
				return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
			});
	}

	// Contract 2's "selection" object. A malformed or unknown mode refuses the manifest; a fixed set needs nothing more.
	inline bool parse_selection(const json_value* value, selection_rules& out)
	{
		if (!value || value->kind != json_kind::object)
			return false;
		if (detail::is(value->find("mode"), "fixed"))
		{
			out = {};
			return true;
		}
		if (!detail::is(value->find("mode"), "in_game"))
			return false;
		out.mode = selection_mode::in_game;
		return detail::number32(value->find("characterSeconds"), min_selection_seconds, max_selection_seconds, out.character_seconds) &&
		       detail::number32(value->find("stageSeconds"), min_selection_seconds, max_selection_seconds, out.stage_seconds);
	}

	// The in-game stage rule names who picks in the stock stage select. Only the Right side's record opens it (DR's VS rule).
	inline bool parse_in_game_stage(const json_value* value, side& picker)
	{
		if (!value || value->kind != json_kind::object || !detail::is(value->find("rule"), "in_game") || value->find("id"))
			return false;
		if (!detail::is(value->find("picker"), "right"))
			return false;
		picker = side::right;
		return true;
	}

	inline bool parse_score(const json_value* value, std::uint32_t max, score& out)
	{
		return value && value->kind == json_kind::object && detail::number32(value->find("left"), 0, max, out.left) && detail::number32(value->find("right"), 0, max, out.right);
	}

	inline result<manifest> parse_manifest(std::string_view text, const capabilities& supported)
	{
		auto parsed = detail::json_parser(text, max_json_nodes, max_file_bytes, max_file_bytes).parse();
		if (!parsed.value)
			return {{}, parsed.error};
		const auto& value = *parsed.value;
		if (value.kind != json_kind::object)
			return {{}, "bad_schema"};
		manifest out;
		if (detail::is(value.find("schema"), schema))
			out.contract = 1;
		else if (detail::is(value.find("schema"), schema_in_game))
			out.contract = 2;
		else
			return {{}, "bad_schema"};
		// Contract 1 is always a fixed set. Contract 2 names its selection explicitly.
		if (out.contract == 2 && !parse_selection(value.find("selection"), out.selection))
			return {{}, "bad_selection"};
		const bool chosen_in_game = in_game(out);
		for (const auto name : {"sessionId", "setId", "matchId", "localMemberId"})
			if (!detail::identifier(value.find(name)))
				return {{}, "bad_identity"};
		out.session_id = value.find("sessionId")->text;
		out.set_id = value.find("setId")->text;
		out.match_id = value.find("matchId")->text;
		out.local_member_id = value.find("localMemberId")->text;
		if (detail::is(value.find("localSide"), "left"))
			out.local_side = side::left;
		else if (detail::is(value.find("localSide"), "right"))
			out.local_side = side::right;
		else
			return {{}, "bad_side"};
		if (!parse_fighter(value.find("left"), out.left, supported, chosen_in_game) || !parse_fighter(value.find("right"), out.right, supported, chosen_in_game) ||
			out.left.member_id == out.right.member_id || out.local_member_id != (out.local_side == side::left ? out.left.member_id : out.right.member_id))
			return {{}, "bad_fighters"};
		out.rank_profiles = value.find("rankProfiles") && value.find("rankProfiles")->kind == json_kind::boolean && value.find("rankProfiles")->truth;
		for (const auto& [key, player] : {std::pair{"left", &out.left}, std::pair{"right", &out.right}})
			if (value.find(key)->find("ranks") && !player->ranks)
				out.ignored.push_back(std::string(key) + ".ranks");
		read_records(*value.find("left"), "left", chosen_in_game, out.left.record, out.left.records, out.ignored);
		read_records(*value.find("right"), "right", chosen_in_game, out.right.record, out.right.records, out.ignored);
		const auto* stage = value.find("stage");
		const auto* rules = value.find("rules");
		const auto* spectator = value.find("spectator");
		if (chosen_in_game)
		{
			if (!parse_in_game_stage(stage, out.selection.stage_picker))
				return {{}, "bad_stage"};
		}
		else if (!stage || !detail::is(stage->find("rule"), "fixed") || !detail::number32(stage->find("id"), 0, std::numeric_limits<std::uint32_t>::max(), out.stage_id) ||
				 std::find(supported.stage_ids.begin(), supported.stage_ids.end(), out.stage_id) == supported.stage_ids.end())
			return {{}, "bad_stage"};
		if (!rules || !detail::number32(rules->find("firstToGames"), 1, 10, out.first_to_games) ||
			!detail::number32(rules->find("roundsPerGame"), 3, 3, out.rounds_per_game))
			return {{}, "bad_rules"};
		if (const auto* allow = rules->find("allowJinpachi"); allow && !detail::boolean(allow, out.allow_jinpachi))
			return {{}, "bad_rules"};
		if (!chosen_in_game && !out.allow_jinpachi && (out.left.row >= 81 || out.right.row >= 81))
			return {{}, "banned_fighter"};
		if (!parse_score(value.find("score"), out.first_to_games - 1, out.games) || !detail::number32(value.find("gameNumber"), 1, 19, out.game_number) ||
			out.game_number != out.games.left + out.games.right + 1)
			return {{}, "bad_score"};
		if (!detail::is(value.find("build"), supported.build) || !detail::is(value.find("compat"), supported.compat) ||
			supported.build.empty() || supported.compat.empty())
			return {{}, "incompatible_build"};
		out.build = supported.build;
		out.compat = supported.compat;
		if (!spectator || !detail::boolean(spectator->find("publishConfirmedInputs"), out.publish_confirmed_inputs) ||
			!detail::is(spectator->find("encoding"), "pbd-pad-v1"))
			return {{}, "bad_spectator"};
		return {std::move(out), {}};
	}

	inline result<manifest> read_manifest(const std::filesystem::path& directory, const capabilities& supported)
	{
		if (!directory.is_absolute())
			return {{}, "relative_channel"};
		auto text = detail::read_text(directory / "set-session.json");
		if (!text.value)
			return {{}, text.error};
		return parse_manifest(*text.value, supported);
	}
} // namespace pbd::set_channel
