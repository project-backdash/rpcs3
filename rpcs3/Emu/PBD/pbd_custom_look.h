#pragma once

#include "../Cell/pbd_game_profile.h"
#include "pbd_custom_catalogue.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

// A custom look: the four items and three colours T5DR's Customize mode puts on one of a fighter's costumes.
//
// Each player record (pbd::game_profile, the card at 0x106c9934 + side * 0x1f4) starts with the chosen fighter's three
// costume blocks of 0x58 bytes: 32 owned item ids, then at +0x40 the four equipped item ids (big-endian; 0xffff none) and at
// +0x54 the three chosen colours (-1 the model's own). The fighter loader (0x14d5b0) copies the block of the fighter's
// costume out of the record (0x29de18) when the fighters load and again at every match start (0x2a2aac -> 0x2a15e4), applies
// the colours through 0x26f82c and attaches each item above 0 through 0x14d2fc; only costumes 0 and 1, and 2 for Xiaoyu and
// Asuka, are read. The game fills the records only at its select confirm (0xb1290 -> 0x2a77b0, from the side's copy of the
// save) and in online, ghost battles and Customize. Practice's outgoing selector also fills the cards after its direct
// resource request, so the opening writes at the following resource-scene boundary.
//
// Both PCs, replays and watchers must draw the same bytes, so a look is only ever written as normalise() leaves it: items in
// their part's slot, the loader's other spellings of none rewritten to 0xffff, and anything the catalogue does not list for
// that row (pbd::custom_catalogue, decoded from the game's own item catalogue) turning the whole look into the defaults.
namespace pbd::custom_look
{
	using u8 = std::uint8_t;
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;
	using s8 = std::int8_t;

	inline constexpr u32 block_size = 0x58, block_count = 3, blocks_size = block_size * block_count;
	inline constexpr u32 equipped_offset = 0x40, colour_offset = 0x54;
	inline constexpr u32 item_slots = 4, colour_parts = 3;
	inline constexpr u16 no_item = 0xffff;
	inline constexpr s8 own_colour = -1;
	// The setup's save as the game loaded it (SYS-DATA from offset 0). Its blocks sit at file 0x820 + fighter * 0x108; entering
	// VS copies the whole save into each side's slot (0x1f99bc), from which the select confirm fills the records.
	inline constexpr u32 save_image = 0x10684960, save_blocks_offset = 0x820;

	struct look
	{
		u32 row = 0;
		std::array<u16, item_slots> items{no_item, no_item, no_item, no_item};
		std::array<s8, colour_parts> colours{own_colour, own_colour, own_colour};
		bool operator==(const look&) const = default;
	};

	constexpr u32 record_address(u32 side) noexcept
	{
		return pbd::game_profile::records + side * pbd::game_profile::record_stride;
	}
	constexpr u32 save_blocks_address(u32 fighter) noexcept
	{
		return save_image + save_blocks_offset + fighter * blocks_size;
	}

	constexpr const pbd::custom_catalogue::row* catalogue_row(u32 row) noexcept
	{
		return row < pbd::custom_catalogue::rows.size() ? &pbd::custom_catalogue::rows[row] : nullptr;
	}

	// The part (0 head, 1 face, 2 upper, 3 lower) whose list holds `item` for this row.
	constexpr std::optional<u32> part_of(const pbd::custom_catalogue::row& row, u16 item) noexcept
	{
		if (item >= 128)
			return std::nullopt;
		for (u32 part = 0; part < item_slots; ++part)
			if ((row.items[part][item >> 6] >> (item & 63)) & 1)
				return part;
		return std::nullopt;
	}

	constexpr bool is_none(u16 item) noexcept
	{
		// The loader skips an id that is zero or negative as a signed half-word (0x14d728-0x14d73c).
		return item == 0 || item >= 0x8000;
	}

	// The look as both PCs write it, or nothing when the game could not show it: a row outside the table, items or colours on a
	// costume the game never dresses, an item that row's catalogue does not list, two items for one part, or a colour past its
	// list. The defaults are a look on every row.
	constexpr std::optional<look> normalise(const look& wanted) noexcept
	{
		const auto* row = catalogue_row(wanted.row);
		// The last two archive entries are not in any selectable costume list.
		if (!row || wanted.row >= 84)
			return std::nullopt;
		look out{wanted.row};
		bool plain = true;
		for (const u16 item : wanted.items)
		{
			if (is_none(item))
				continue;
			const auto part = part_of(*row, item);
			if (!part || out.items[*part] != no_item)
				return std::nullopt;
			out.items[*part] = item;
			plain = false;
		}
		for (u32 part = 0; part < colour_parts; ++part)
		{
			const s8 colour = wanted.colours[part];
			if (colour == own_colour)
				continue;
			if (colour < 0 || colour >= row->colours[part])
				return std::nullopt;
			out.colours[part] = colour;
			plain = false;
		}
		if (!plain && !row->dressable)
			return std::nullopt;
		return out;
	}

	// What a player shows for a wanted look: the look itself, or that row's defaults when the game could not show it.
	constexpr look shown(const look& wanted) noexcept
	{
		return normalise(wanted).value_or(look{wanted.row});
	}

	// The three blocks for a record, from the fighter's own blocks in the setup's save: owned lists and owned-colour masks as
	// saved, every costume in its defaults, then `worn` on its row's costume. A default look therefore also clears whatever an
	// earlier record held for that fighter.
	inline std::array<u8, blocks_size> record_blocks(std::span<const u8, blocks_size> saved, const look& worn) noexcept
	{
		std::array<u8, blocks_size> out{};
		for (u32 i = 0; i < blocks_size; ++i)
			out[i] = saved[i];
		const auto safe = shown(worn);
		const auto* row = catalogue_row(safe.row);
		for (u32 costume = 0; costume < block_count; ++costume)
		{
			const bool chosen = row && row->costume == costume;
			u8* block = out.data() + costume * block_size;
			for (u32 slot = 0; slot < item_slots; ++slot)
			{
				const u16 item = chosen ? safe.items[slot] : no_item;
				block[equipped_offset + slot * 2] = static_cast<u8>(item >> 8);
				block[equipped_offset + slot * 2 + 1] = static_cast<u8>(item);
			}
			for (u32 part = 0; part < colour_parts; ++part)
				block[colour_offset + part] = static_cast<u8>(chosen ? safe.colours[part] : own_colour);
		}
		return out;
	}

	// Read only the selected costume's equipped items and colours. Owned inventory never becomes part of a saved look.
	inline std::optional<look> read_record(std::span<const u8, blocks_size> blocks, u32 selected) noexcept
	{
		const auto* row = catalogue_row(selected);
		if (!row || selected >= 84 || !row->dressable || row->costume >= block_count)
			return {};
		const auto* block = blocks.data() + row->costume * block_size;
		look out{selected};
		for (u32 part = 0; part < item_slots; ++part)
			out.items[part] = static_cast<u16>(u16{block[equipped_offset + part * 2]} << 8 | block[equipped_offset + part * 2 + 1]);
		for (u32 part = 0; part < colour_parts; ++part)
			out.colours[part] = static_cast<s8>(block[colour_offset + part]);
		return normalise(out);
	}

	// The shop's private card owns exactly its fighter's catalogue. Every list is rebuilt, so inventory from another fighter
	// cannot be carried into the editor. The caller keeps these bytes out of the player's save and both side-save copies.
	inline std::optional<std::array<u8, blocks_size>> shop_blocks(const look& wanted) noexcept
	{
		const auto normal = normalise(wanted);
		const auto* selected = catalogue_row(wanted.row);
		if (!normal || !selected->dressable)
			return {};
		std::array<u8, blocks_size> owned{};
		for (const auto& row : pbd::custom_catalogue::rows)
		{
			if (row.fighter != selected->fighter || !row.dressable || row.costume >= block_count)
				continue;
			auto* block = owned.data() + row.costume * block_size;
			u32 count = 0;
			for (u16 item = 1; item < 128; ++item)
				if (part_of(row, item))
				{
					if (count >= equipped_offset / 2)
						return {};
					block[count * 2] = static_cast<u8>(item >> 8);
					block[count * 2 + 1] = static_cast<u8>(item);
					++count;
				}
			for (u32 part = 0; part < colour_parts; ++part)
			{
				if (row.colours[part] > 32)
					return {};
				const u32 mask = row.colours[part] == 32 ? ~u32{0} : (u32{1} << row.colours[part]) - 1;
				for (u32 byte = 0; byte < 4; ++byte)
					block[0x48 + part * 4 + byte] = static_cast<u8>(mask >> (24 - byte * 8));
			}
		}
		return record_blocks(owned, *normal);
	}
} // namespace pbd::custom_look
