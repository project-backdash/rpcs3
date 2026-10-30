#pragma once

#include "pbd_json_reader.h"
#include "pbd_set_notices.h"

namespace pbd::replay_notices
{
	using placements = std::array<std::optional<pbd::rank_stakes::placement>, 2>;
	using json = pbd::json::json_value;

	// Missing in older recordings; a null side explicitly names an already placed player.
	inline std::optional<placements> read(const json* value)
	{
		if (!value || value->kind != pbd::json::json_kind::array || value->items.size() != 2)
			return {};
		placements out;
		for (std::size_t side = 0; side < out.size(); ++side)
		{
			const auto& item = value->items[side];
			if (item.kind == pbd::json::json_kind::null)
				continue;
			pbd::rank_stakes::placement count;
			if (item.kind != pbd::json::json_kind::object || item.fields.size() != 2 ||
				!pbd::json::detail::number32(item.find("total"), 1, pbd::rank_stakes::max_placement_total, count.total) ||
				!pbd::json::detail::number32(item.find("done"), 0, count.total, count.done))
				return {};
			out[side] = count;
		}
		return out;
	}

	// Capture exactly the Ranked counts the current game's notices draw, at its immutable start.
	inline std::optional<json> capture(const pbd::set_notices::snapshot& shown, std::uint64_t generation)
	{
		if (!shown.active || shown.generation != generation || shown.mode != pbd::set_notices::set_mode::ranked)
			return {};
		std::vector<json> sides;
		for (const auto& player : shown.sides)
		{
			const auto& plate = player.shown;
			if (plate.shown == pbd::game_overlay::plate::kind::placing)
				sides.push_back(json::object({{"done", json::number(plate.done)}, {"total", json::number(plate.total)}}));
			else
				sides.emplace_back();
		}
		return json::array(std::move(sides));
	}

	inline pbd::set_notices::snapshot compose(const placements& recorded, std::uint32_t scene, std::uint32_t pending,
		bool first_intro, const std::array<bool, 2>& drawn, const std::array<bool, 2>& promoted,
		const std::array<std::uint32_t, 2>& stakes)
	{
		pbd::set_notices::set_view view;
		view.mode = pbd::set_notices::set_mode::ranked;
		view.placements = recorded;
		view.now = pbd::set_notices::screen_of(scene, pending);
		view.intro_side = pbd::set_notices::intro_side_of(scene, pending);
		view.first_intro = first_intro;
		view.plate_drawn = drawn;
		view.promoted = promoted;
		view.game_stakes = stakes;
		return pbd::set_notices::compose(view);
	}
} // namespace pbd::replay_notices
