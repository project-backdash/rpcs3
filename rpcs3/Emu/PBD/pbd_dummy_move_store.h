#pragma once

#include "pbd_dummy_move.h"
#include "pbd_json_reader.h"

namespace pbd::practice::dummy_move
{
	inline std::optional<reaction> read_reaction(const json::json_value* value)
	{
		using namespace json::detail;
		if (!value || value->kind != json::json_kind::object) return {};
		reaction out;
		if (is(value->find("source"), "off")) return out;
		if (is(value->find("source"), "recording"))
		{
			out.kind = source::recording;
			if (!number32(value->find("slot"), 0, 7, out.slot)) return {};
			return out;
		}
		if (!is(value->find("source"), "moves")) return {};
		out.kind = source::moves;
		const auto* ids = value->find("ids");
		if (!ids || ids->kind != json::json_kind::array || ids->items.empty() || ids->items.size() > 64) return {};
		for (const auto& item : ids->items)
		{
			u32 id = 0;
			if (!number32(&item, 0, 4095, id)) return {};
			out.ids.push_back(id);
		}
		std::sort(out.ids.begin(), out.ids.end());
		return valid(out) ? std::optional{std::move(out)} : std::nullopt;
	}
	inline std::optional<settings> read_settings(const json::json_value* value)
	{
		if (!value || value->kind != json::json_kind::object) return {};
		auto guard = read_reaction(value->find("afterGuard")), hit = read_reaction(value->find("afterHit"));
		if (!guard || !hit) return {};
		return settings{std::move(*guard), std::move(*hit)};
	}
	inline json::json_value encode(const reaction& value)
	{
		using json::json_value;
		if (value.kind == source::recording)
			return json_value::object({{"source", json_value::string("recording")}, {"slot", json_value::number(value.slot)}});
		if (value.kind == source::moves)
		{
			auto ids = json_value::array();
			for (const auto id : value.ids) ids.items.push_back(json_value::number(id));
			return json_value::object({{"source", json_value::string("moves")}, {"ids", std::move(ids)}});
		}
		return json_value::object({{"source", json_value::string("off")}});
	}
	inline json::json_value encode(const settings& value)
	{
		return json::json_value::object({{"afterGuard", encode(value.after_guard)}, {"afterHit", encode(value.after_hit)}});
	}
}
