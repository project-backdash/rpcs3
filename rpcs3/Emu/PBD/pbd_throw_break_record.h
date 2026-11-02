#pragma once

#include "pbd_throw_break.h"
#include "../Cell/pbd_set_channel.h"

#include <optional>
#include <string_view>

// The throw break rule's setting where a recording or a watcher's stream carries it: a game start capsule and each round
// start capsule of a replay, a watcher's game controls, and the input policy of the players' game starts.
// "throwBreakLookahead": {"version": 2, "left": L, "right": L}, plus "techRolls": true only when a developer run counted
// landing options as escapes. It is written only while some L is above 0, so a recording or a stream without it comes from
// before the rule or ran with it off, and plays as it always has. The version names the rule that ran; a reader refuses
// any other, so a recording never plays under a rule it was not made with.
namespace pbd::throw_break
{
	inline constexpr std::string_view record_key = "throwBreakLookahead";
	inline constexpr u32 record_version = rule_version;
	// Version 1 read the held fighter's whole word ahead through a hold. This build cannot run it, so a recording or a stream
	// made with it refuses with this reason rather than play another game.
	inline constexpr u32 retired_version = 1;
	inline constexpr const char* retired_refusal = "throw_break_rule_retired";

	// A game start capsule that carries the rule names a schema older builds refuse, rather than ignore the field and play
	// the game without the rule; one without it keeps the schema every recording before the rule has.
	inline constexpr std::string_view capsule_schema_stock = "pbd-game-start-capsule-draft-2";
	inline constexpr std::string_view capsule_schema_rule = "pbd-game-start-capsule-draft-3";

	constexpr std::string_view capsule_schema(const lookahead_setting& value) noexcept
	{
		return value.any() ? capsule_schema_rule : capsule_schema_stock;
	}

	inline pbd::set_channel::json_value record_json(const lookahead_setting& value)
	{
		using json = pbd::set_channel::json_value;
		auto out = json::object({{"version", json::number(record_version)}, {"left", json::number(value.lookahead[0])},
			{"right", json::number(value.lookahead[1])}});
		if (value.tech_rolls)
			out.fields.emplace_back("techRolls", json::boolean(true));
		return out;
	}

	// Whether a record names the retired rule.
	inline bool retired_record(const pbd::set_channel::json_value* field)
	{
		u32 version = 0;
		return field && field->kind == pbd::set_channel::json_kind::object &&
		       pbd::set_channel::detail::number32(field->find("version"), retired_version, retired_version, version);
	}

	// The setting a record names: none for an absent field (L 0 on both sides), nothing for any other form, including a field
	// that names no L or another version, which no writer of this rule produces.
	inline std::optional<lookahead_setting> read_record(const pbd::set_channel::json_value* field)
	{
		using pbd::set_channel::json_kind;
		lookahead_setting out;
		if (!field)
			return out;
		if (field->kind != json_kind::object)
			return std::nullopt;
		for (const auto& [name, value] : field->fields)
		{
			if (name == "version" || name == "left" || name == "right")
				continue;
			if (name != "techRolls" || value.kind != json_kind::boolean || !value.truth)
				return std::nullopt;
			out.tech_rolls = true;
		}
		u32 version = 0;
		if (!pbd::set_channel::detail::number32(field->find("version"), record_version, record_version, version) ||
			!pbd::set_channel::detail::number32(field->find("left"), 0, max_lookahead, out.lookahead[0]) ||
			!pbd::set_channel::detail::number32(field->find("right"), 0, max_lookahead, out.lookahead[1]) || !out.any())
			return std::nullopt;
		return out;
	}
} // namespace pbd::throw_break
