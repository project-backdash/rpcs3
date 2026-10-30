#pragma once

#include "pbd_t5dr_memory.h"
#include <array>
#include <cstdint>
#include <optional>

namespace pbd::character_rule
{
	enum class mode
	{
		friend_room,
		lobby,
		ranked
	};
	enum class rule
	{
		stock,
		ban_jinpachi
	};
	inline constexpr std::uint32_t jinpachi = 34, random_cursor = 24, jinpachi_cursor = 35;
	inline constexpr std::uint64_t jinpachi_bit = std::uint64_t{1} << jinpachi;
	inline constexpr std::uint64_t roster_mask = (jinpachi_bit << 1) - 1;

	constexpr rule for_session(mode session, bool lobby_allows = false)
	{
		return session == mode::friend_room || (session == mode::lobby && lobby_allows) ? rule::stock : rule::ban_jinpachi;
	}

	constexpr std::uint64_t candidates(rule policy, std::uint64_t private_mask)
	{
		return policy == rule::ban_jinpachi ? private_mask & ~jinpachi_bit : private_mask;
	}

	constexpr bool row_available(rule policy, std::uint32_t row)
	{
		return row < 85 && (policy == rule::stock || row < 81);
	}

	template <class Tables>
	bool cursor_available(rule policy, const Tables& table, std::uint32_t cursor)
	{
		if (cursor >= table.grid.size())
			return false;
		if (policy == rule::ban_jinpachi && cursor == jinpachi_cursor)
			return false;
		if (policy == rule::ban_jinpachi && cursor == random_cursor)
			return (table.availability & ~jinpachi_bit & roster_mask) != 0;
		const auto character = cursor == random_cursor || cursor == jinpachi_cursor ? jinpachi : table.grid[cursor].character;
		return character <= jinpachi && (table.availability & (std::uint64_t{1} << character)) != 0;
	}

	// A loaded select may predate the session rule. Follow the stock left-neighbor skip law before input or presentation.
	template <class Tables>
	std::optional<std::uint32_t> initial_cursor(rule policy, const Tables& table, std::uint32_t cursor)
	{
		if (!table.verified || cursor >= table.grid.size())
			return {};
		for (std::size_t scanned = 0; scanned < table.grid.size(); ++scanned)
		{
			if (cursor_available(policy, table, cursor))
				return cursor;
			cursor = table.grid[cursor].neighbor[2];
			if (cursor >= table.grid.size())
				return {};
		}
		return {};
	}

	// Retain an eligible seed-derived panel; otherwise use the stock roulette's forward scan. No generator is called.
	inline std::optional<std::uint32_t> initial_panel(rule policy, std::uint32_t index, std::uint64_t global_mask, std::uint64_t private_mask)
	{
		if (index > jinpachi || (global_mask & ~roster_mask) || (private_mask & ~roster_mask))
			return {};
		const auto allowed = global_mask & candidates(policy, private_mask);
		for (std::uint32_t scanned = 0; scanned <= jinpachi; ++scanned)
		{
			if (allowed & (std::uint64_t{1} << index))
				return index;
			index = index == jinpachi ? 0 : index + 1;
		}
		return {};
	}

	struct selection_plan
	{
		std::uint32_t cursor = 0, panel = 0, base = 0, row = 0;
		bool move = false, preview = false, reroute = false;
	};

	template <class Tables>
	std::optional<selection_plan> initial_selection(const Tables& table,
		std::uint32_t cursor, std::uint32_t panel, std::uint32_t base, std::uint32_t row, std::uint64_t private_mask)
	{
		if (panel > jinpachi)
			return {};
		const auto next_cursor = initial_cursor(rule::ban_jinpachi, table, cursor);
		const auto next_panel = panel == jinpachi ? initial_panel(rule::ban_jinpachi, panel, table.availability, private_mask) :
		                                            std::optional<std::uint32_t>{panel};
		if (!next_cursor || !next_panel)
			return {};
		selection_plan plan{*next_cursor, *next_panel, base, row, *next_cursor != cursor,
			*next_cursor != cursor || base == jinpachi || (row >= 81 && row < 85), *next_panel != panel};
		if (plan.preview)
		{
			plan.base = plan.cursor == random_cursor ? plan.panel : table.grid[plan.cursor].character;
			if (plan.base >= jinpachi || table.costumes[plan.base].rows.empty())
				return {};
			plan.row = table.costumes[plan.base].rows.front();
		}
		if (plan.base >= jinpachi || (plan.row != 0xffffffffu && !row_available(rule::ban_jinpachi, plan.row)))
			return {};
		return plan;
	}

	enum class predicate
	{
		stock,
		available,
		unavailable,
		unverified
	};
	struct call
	{
		std::uint64_t generation = 0, active_generation = 0;
		std::uint32_t owner = 0;
		std::uint64_t toc = 0;
		std::uint32_t mode = 0, scene = 0, subscene = 0, grid_mode = 0;
		std::uint64_t link = 0;
		std::uint32_t character = 0;
		std::uint64_t record = 0;
		std::uint32_t cursor = 0;
		bool pinned = false;
	};

	// Only the two selection readers receive a rule override. Stage and Mokujin consumers keep the saved unlock mask.
	constexpr predicate selection_predicate(rule policy, const call& value)
	{
		if (policy == rule::stock || value.character != jinpachi || (value.link != 0xaf538 && value.link != 0xb0b6c))
			return predicate::stock;
		// Direct loading reaches the final availability call in subscene 2. Confirmation can advance to 4, and a rematch
		// can clear the scene word (0) in the same tick before the subscene moves. Both peers run that order from the same inputs.
		// Initialization and saved-fighter readers still require the character-select scene and subscene.
		const bool confirmation = value.link == 0xb0b6c;
		const bool scene_ok = value.scene == 3 || (confirmation && value.scene == 0 && value.subscene == 44);
		const bool subscene_ok = value.subscene == 44 ||
		                         (confirmation && value.scene == 3 && (value.subscene == 2 || value.subscene == 4));
		if (!value.pinned || !value.generation || value.generation != value.active_generation || value.owner != t5dr::main_ppu_id ||
			value.toc != t5dr::toc || value.mode != 4 || !scene_ok || !subscene_ok || value.grid_mode != 1 ||
			(value.record != 0x10093f58 && value.record != 0x10093fd0) || value.cursor > jinpachi_cursor)
			return predicate::unverified;
		if (value.link == 0xb0b6c)
			return predicate::unavailable;
		if (value.cursor == random_cursor)
			return predicate::available;
		if (value.cursor == jinpachi_cursor)
			return predicate::unavailable;
		return predicate::unverified;
	}

	// A prior game's banned fighter cannot replace the eligible cursor during stock selection initialization.
	constexpr predicate saved_character_predicate(rule policy, const call& value, std::uint32_t side)
	{
		if (policy == rule::stock || value.character != jinpachi || value.link != 0xb16c8)
			return predicate::stock;
		if (!value.pinned || !value.generation || value.generation != value.active_generation || value.owner != t5dr::main_ppu_id ||
			value.toc != t5dr::toc || value.mode != 4 || value.scene != 3 || value.subscene != 44 || value.grid_mode != 1 ||
			side > 1 || value.record != 0x10093f58ull + side * 0x78ull || value.cursor > jinpachi_cursor ||
			value.cursor == jinpachi_cursor)
			return predicate::unverified;
		return predicate::unavailable;
	}
} // namespace pbd::character_rule
