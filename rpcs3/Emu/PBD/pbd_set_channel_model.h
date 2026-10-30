#pragma once

// A per-process set channel carries host metadata, never guest memory. Bounded parsing and immutable serials keep a malformed
// launcher command from changing match rules or advancing play before the lobby service has agreed on the previous game.

#include "pbd_env.h"

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
	// No character means the player chooses one in the game's own select (contract 2, in-game selection). The rank is the
	// game's rank index the fight shows beside the name (a Ranked set's rank; 0, Beginner, when the manifest names none).
	struct fighter
	{
		std::string member_id;
		std::string name;
		std::string character_id;
		std::uint32_t row = 0;
		std::uint32_t rank = 0;
		bool has_character = true;
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
		bool allow_jinpachi = false;
		std::uint32_t contract = 1;
		selection_rules selection;
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
		if (in_game_selection)
		{
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
		auto parsed = detail::json_parser(text).parse();
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
