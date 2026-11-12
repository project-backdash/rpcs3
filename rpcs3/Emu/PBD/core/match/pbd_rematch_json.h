#pragma once

#include "pbd_rematch.h"
#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace pbd::rematch
{
	// Every required name once, each optional name at most once, and nothing else: a repeated or unknown key changes the count.
	template <typename Value>
	bool exact_fields(const Value* value, std::initializer_list<std::string_view> names, std::initializer_list<std::string_view> optional = {})
	{
		using kind = decltype(Value{}.kind);
		if (!value || value->kind != kind::object)
			return false;
		const auto present = std::count_if(optional.begin(), optional.end(), [&](auto name)
			{
				return value->find(name) != nullptr;
			});
		return value->fields.size() == names.size() + static_cast<std::size_t>(present) &&
		       std::all_of(names.begin(), names.end(), [&](auto name)
				   {
					   return value->find(name) != nullptr;
				   });
	}
	template <typename Value, typename Integer>
	bool read_number(const Value* value, u64 minimum, u64 maximum, Integer& out)
	{
		using kind = decltype(Value{}.kind);
		if (!value || value->kind != kind::number)
			return false;
		u64 number = 0;
		const auto begin = value->text.data(), end = begin + value->text.size();
		const auto result = std::from_chars(begin, end, number);
		if (result.ec != std::errc{} || result.ptr != end || number < minimum || number > maximum)
			return false;
		out = static_cast<Integer>(number);
		return true;
	}
	template <typename Value>
	bool read_schema(const Value* value, std::string_view expected)
	{
		using kind = decltype(Value{}.kind);
		return value && value->kind == kind::string && value->text == expected;
	}
	template <typename Value>
	bool read_selection(const Value* value, selection& out)
	{
		return exact_fields(value, {"leftRow", "rightRow", "stageId"}) &&
		       read_number(value->find("leftRow"), 0, 84, out.rows[0]) &&
		       read_number(value->find("rightRow"), 0, 84, out.rows[1]) &&
		       read_number(value->find("stageId"), 0, 0x25, out.stage);
	}
	template <typename Value>
	bool read_options(const Value* value, options& out)
	{
		using kind = decltype(Value{}.kind);
		if (!exact_fields(value, {"roundsPerGame", "roundTimeIndex", "handicapPercent"}))
			return false;
		const auto* handicap = value->find("handicapPercent");
		return read_number(value->find("roundsPerGame"), 3, 3, out.rounds) &&
		       read_number(value->find("roundTimeIndex"), 0, 4, out.time_index) &&
		       handicap->kind == kind::array && handicap->items.size() == 2 &&
		       read_number(&handicap->items[0], 100, 100, out.handicap[0]) &&
		       read_number(&handicap->items[1], 100, 100, out.handicap[1]);
	}
	template <typename Value>
	std::optional<sealed_state> read_state(const Value* value)
	{
		sealed_state out;
		if (!exact_fields(value, {"schema", "gameNumber", "epoch", "endFrame", "selection", "options"}) ||
			!read_schema(value->find("schema"), "pbd-rematch-state-1") ||
			!read_number(value->find("gameNumber"), 1, 19, out.game) ||
			!read_number(value->find("epoch"), 0, max_serial - 1, out.epoch) ||
			!read_number(value->find("endFrame"), 1, 0xffffffffu, out.end_frame) ||
			!read_selection(value->find("selection"), out.selected) ||
			!read_options(value->find("options"), out.rules))
			return {};
		return out;
	}
	template <typename Value>
	std::optional<load> read_load(const Value* value)
	{
		using kind = decltype(Value{}.kind);
		load out;
		if (!exact_fields(value, {"schema", "previousGame", "nextGame", "epoch", "endFrame", "selection", "options", "randomStage"}, {"repick"}) ||
			!read_schema(value->find("schema"), "pbd-rematch-load-1") ||
			!read_number(value->find("previousGame"), 1, 19, out.previous_game) ||
			!read_number(value->find("nextGame"), 1, 19, out.next_game) ||
			!read_number(value->find("epoch"), 1, max_serial, out.epoch) ||
			!read_number(value->find("endFrame"), 1, 0xffffffffu, out.end_frame) ||
			!read_selection(value->find("selection"), out.selected) ||
			!read_options(value->find("options"), out.rules) ||
			value->find("randomStage")->kind != kind::boolean)
			return {};
		out.random_stage = value->find("randomStage")->truth;
		// The optional repick field names the side that picks again; other loads omit it.
		if (const auto* side = value->find("repick"))
		{
			if (side->kind != kind::string || (side->text != "left" && side->text != "right") || out.random_stage)
				return {};
			out.repick = side->text == "right" ? 1u : 0u;
		}
		return out;
	}
	template <typename Value>
	Value selection_json(const selection& value)
	{
		return Value::object({{"leftRow", Value::number(value.rows[0])}, {"rightRow", Value::number(value.rows[1])},
			{"stageId", Value::number(value.stage)}});
	}
	template <typename Value>
	Value options_json(const options& value)
	{
		return Value::object({{"roundsPerGame", Value::number(value.rounds)}, {"roundTimeIndex", Value::number(value.time_index)},
			{"handicapPercent", Value::array({Value::number(value.handicap[0]), Value::number(value.handicap[1])})}});
	}
	template <typename Value>
	Value state_json(const sealed_state& value)
	{
		return Value::object({{"schema", Value::string("pbd-rematch-state-1")}, {"gameNumber", Value::number(value.game)},
			{"epoch", Value::number(value.epoch)}, {"endFrame", Value::number(value.end_frame)},
			{"selection", selection_json<Value>(value.selected)}, {"options", options_json<Value>(value.rules)}});
	}
	template <typename Value>
	Value load_json(const load& value)
	{
		auto out = Value::object({{"schema", Value::string("pbd-rematch-load-1")}, {"previousGame", Value::number(value.previous_game)},
			{"nextGame", Value::number(value.next_game)}, {"epoch", Value::number(value.epoch)}, {"endFrame", Value::number(value.end_frame)},
			{"selection", selection_json<Value>(value.selected)}, {"options", options_json<Value>(value.rules)},
			{"randomStage", Value::boolean(value.random_stage)}});
		if (value.repick)
			out.fields.emplace_back("repick", Value::string(*value.repick ? "right" : "left"));
		return out;
	}
} // namespace pbd::rematch
