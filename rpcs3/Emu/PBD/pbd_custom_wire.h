#pragma once

#include "pbd_custom_look.h"
#include "pbd_json_reader.h"
#include "pbd_env.h"

#include <algorithm>
#include <string>
#include <vector>

// Transport validation preserves the agreed bytes; catalogue validation happens only when preparing the player card.
// This keeps a recording's identity stable even when an unsupported item makes both games draw the default costume.
namespace pbd::custom_wire
{
	using namespace pbd::custom_look;
	using json = pbd::json::json_value;
	inline constexpr std::size_t look_bytes = 12, slot_count = 10, block_bytes = look_bytes * slot_count, maximum_bytes = block_bytes * 35;
	inline constexpr std::string_view capsule_schema = "pbd-game-start-capsule-draft-4";
	using bytes = std::array<u8, look_bytes>;
	struct drawn
	{
		u32 slot = 0;
		bytes value{};
		bool operator==(const drawn&) const = default;
	};
	struct pick
	{
		u32 slot = 0;
		std::vector<u8> table;
		bool operator==(const pick&) const = default;
	};
	using pair = std::array<std::optional<drawn>, 2>;
	inline bool enabled()
	{
		const auto* value = pbd::env::find("PBD_CUSTOM_LOOKS");
		return value && std::string_view(value) == "1";
	}

	inline bool empty(std::span<const u8, look_bytes> value)
	{
		return std::all_of(value.begin(), value.end(), [](u8 byte)
			{
				return byte == 0xff;
			});
	}
	inline std::optional<look> unpack(std::span<const u8, look_bytes> value)
	{
		if (value[0] > 84)
			return {};
		look out{value[0]};
		for (u32 part = 0; part < item_slots; ++part)
		{
			const u16 item = static_cast<u16>((u16{value[1 + part * 2]} << 8) | value[2 + part * 2]);
			if (!item || (item >= 0x8000 && item != no_item))
				return {};
			out.items[part] = item;
		}
		for (u32 part = 0; part < colour_parts; ++part)
		{
			const u8 colour = value[9 + part];
			if (colour != 0xff && colour > 11)
				return {};
			out.colours[part] = static_cast<s8>(colour);
		}
		return out;
	}
	inline bytes pack(const look& value)
	{
		bytes out{};
		out[0] = static_cast<u8>(value.row);
		for (u32 part = 0; part < item_slots; ++part)
		{
			out[1 + part * 2] = static_cast<u8>(value.items[part] >> 8);
			out[2 + part * 2] = static_cast<u8>(value.items[part]);
		}
		for (u32 part = 0; part < colour_parts; ++part)
			out[9 + part] = static_cast<u8>(value.colours[part]);
		return out;
	}

	// Each block is a multiple of three bytes, so canonical base64 has no padding or unused bits.
	inline std::optional<std::vector<u8>> read_table(std::string_view text)
	{
		if (text.empty() || text.size() > maximum_bytes / 3 * 4 || text.size() % (block_bytes / 3 * 4))
			return {};
		constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::vector<u8> out;
		out.reserve(text.size() / 4 * 3);
		for (std::size_t at = 0; at < text.size(); at += 4)
		{
			u32 word = 0;
			for (u32 digit = 0; digit < 4; ++digit)
			{
				const auto n = alphabet.find(text[at + digit]);
				if (n == std::string_view::npos)
					return {};
				word = (word << 6) | static_cast<u32>(n);
			}
			out.insert(out.end(), {static_cast<u8>(word >> 16), static_cast<u8>(word >> 8), static_cast<u8>(word)});
		}
		for (std::size_t block = 0; block < out.size(); block += block_bytes)
		{
			bool populated = false;
			for (std::size_t at = block; at < block + block_bytes; at += look_bytes)
			{
				const std::span<const u8, look_bytes> value(out.data() + at, look_bytes);
				if (empty(value))
					continue;
				if (!unpack(value))
					return {};
				populated = true;
			}
			if (!populated)
				return {};
		}
		return out;
	}
	inline std::optional<pick> read_pick(const json* value)
	{
		if (!value || value->kind != pbd::json::json_kind::object || value->fields.size() != 2)
			return {};
		u32 slot = 0;
		const auto* table = value->find("table");
		if (!pbd::json::detail::number32(value->find("slot"), 1, slot_count, slot) || !table || table->kind != pbd::json::json_kind::string)
			return {};
		auto decoded = read_table(table->text);
		if (!decoded)
			return {};
		return pick{slot, std::move(*decoded)};
	}
	inline std::vector<u8> table_field(const json* value)
	{
		if (value && value->kind == pbd::json::json_kind::string)
			if (auto table = read_table(value->text))
				return std::move(*table);
		return {};
	}
	inline std::optional<drawn> resolve(const pick& value, u32 row)
	{
		if (!value.slot || value.slot > slot_count || row > 84 || value.table.empty() || value.table.size() > maximum_bytes || value.table.size() % block_bytes)
			return {};
		std::optional<drawn> out;
		for (std::size_t block = 0; block < value.table.size(); block += block_bytes)
		{
			const auto at = block + (value.slot - 1) * look_bytes;
			if (value.table[at] != row)
				continue;
			if (out)
				return {};
			drawn found{value.slot};
			std::copy_n(value.table.begin() + at, look_bytes, found.value.begin());
			if (!unpack(found.value))
				return {};
			out = found;
		}
		return out;
	}
	// A fighter's numbered cards come only from its own table. Ambiguous rows or slots are omitted.
	inline std::array<std::optional<look>, slot_count> choices(const std::vector<u8>& table, u32 fighter)
	{
		std::array<std::optional<look>, slot_count> out{};
		for (u32 slot = 1; slot <= slot_count; ++slot)
		{
			bool ambiguous = false;
			for (u32 row = 0; row < 84; ++row)
			{
				if (catalogue_row(row)->fighter != fighter)
					continue;
				if (const auto found = resolve({slot, table}, row))
					if (const auto normal = normalise(*unpack(found->value)))
					{
						ambiguous = ambiguous || out[slot - 1].has_value();
						out[slot - 1] = normal;
					}
			}
			if (ambiguous)
				out[slot - 1].reset();
		}
		return out;
	}
	inline std::string hex(const bytes& value)
	{
		constexpr char digits[] = "0123456789abcdef";
		std::string out;
		out.reserve(look_bytes * 2);
		for (u8 byte : value)
		{
			out += digits[byte >> 4];
			out += digits[byte & 15];
		}
		return out;
	}
	inline json drawn_json(const drawn& value)
	{
		return json::object({{"slot", json::number(value.slot)}, {"bytes", json::string(hex(value.value))}});
	}
	inline std::optional<drawn> read_drawn(const json* value, u32 row)
	{
		if (!value || value->kind != pbd::json::json_kind::object || value->fields.size() != 2)
			return {};
		drawn out;
		const auto* text = value->find("bytes");
		if (!pbd::json::detail::number32(value->find("slot"), 1, slot_count, out.slot) || !text ||
			text->kind != pbd::json::json_kind::string || text->text.size() != look_bytes * 2)
			return {};
		constexpr std::string_view digits = "0123456789abcdef";
		for (std::size_t at = 0; at < look_bytes; ++at)
		{
			const auto hi = digits.find(text->text[at * 2]), lo = digits.find(text->text[at * 2 + 1]);
			if (hi == std::string_view::npos || lo == std::string_view::npos)
				return {};
			out.value[at] = static_cast<u8>(hi * 16 + lo);
		}
		return out.value[0] == row && unpack(out.value) ? std::optional(out) : std::nullopt;
	}
	inline bool capsule_matches(const json& capsule, const std::array<u32, 2>& rows, const pair& expected)
	{
		const auto* players = capsule.find("players");
		if (!players)
			return !expected[0] && !expected[1];
		if (players->kind != pbd::json::json_kind::array || players->items.size() != 2)
			return false;
		for (std::size_t side = 0; side < 2; ++side)
		{
			const auto* field = players->items[side].find("look");
			const auto actual = read_drawn(field, rows[side]);
			if ((field && !actual) || actual != expected[side])
				return false;
		}
		return true;
	}
	inline bool capsule_matches_tables(const json& capsule, const std::array<std::vector<u8>, 2>& tables)
	{
		const auto *players = capsule.find("players"), *selection = capsule.find("selection");
		if (!players || players->kind != pbd::json::json_kind::array || players->items.size() != 2 || !selection)
			return false;
		for (u32 side = 0; side < 2; ++side)
		{
			u32 row = 0;
			if (!pbd::json::detail::number32(selection->find(side ? "rightRow" : "leftRow"), 0, 84, row))
				return false;
			if (const auto* value = players->items[side].find("look"))
			{
				const auto drawn = read_drawn(value, row);
				if (!drawn || resolve({drawn->slot, tables[side]}, row) != drawn)
					return false;
			}
		}
		return true;
	}
} // namespace pbd::custom_wire
