#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

// A retained pointer word written back exactly as a byte offset from a base that the next boot rebuilds: one of the two
// COM bank slots, or a fixed table in the pinned executable. When a character changes, the game loads the new COM image
// at the same slot base and leaves the actor words that pointed into the old image untouched, so such a word can name
// no member of the new image. The offset still rebuilds the same 32-bit word, and the style kept with it ties the word
// to the image that is loaded when it is written back. A word outside every base is refused, never carried as is.
namespace pbd::replay_bank_offset
{
	enum class base_kind : std::uint32_t
	{
		opposing,
		own,
		executable
	};
	struct reference
	{
		base_kind base = base_kind::opposing;
		std::uint32_t byte_offset = 0;
		std::uint16_t bank_style = 0;
	};
	// A slot's verified COM bank and the style loaded into it. A base of 0 means the slot has no readable bank.
	struct bank
	{
		std::uint32_t base = 0;
		std::uint16_t style = 0;
		std::uint32_t image_end = 0;
	};
	struct banks
	{
		bank opposing, own;
	};

	// 0x37958 points a cursor at 0x1000103c with a run of 8 halfwords; the run lies in this table of the executable.
	inline constexpr std::uint32_t executable_table = 0x10001030;
	inline constexpr std::uint32_t executable_table_bytes = 0x20;
	// The game loads the two COM slots 0x8a280 bytes apart (0x2162df00 and 0x216b8180), and a loaded image ends well
	// before that. An offset at or past this span is not a word of the slot.
	inline constexpr std::uint32_t max_bank_span = 0x8a280;
	// The reaction cursor walks halfwords; the reaction result is a 32-bit pointer.
	inline constexpr std::uint32_t cursor_alignment = 2;
	inline constexpr std::uint32_t result_alignment = 4;

	constexpr std::string_view name(base_kind base) noexcept
	{
		return base == base_kind::opposing ? "opposing" : base == base_kind::own ? "own" :
		                                                                           "executable";
	}

	constexpr std::optional<base_kind> parse_base(std::string_view text) noexcept
	{
		for (const auto base : {base_kind::opposing, base_kind::own, base_kind::executable})
			if (text == name(base))
				return base;
		return std::nullopt;
	}

	// The bounds that hold whatever is loaded: the alignment, the span of a slot and the size of the executable table.
	constexpr bool valid(reference ref, std::uint32_t alignment) noexcept
	{
		if ((alignment != cursor_alignment && alignment != result_alignment) || ref.byte_offset % alignment)
			return false;
		if (ref.base == base_kind::executable)
			return !ref.bank_style && ref.byte_offset < executable_table_bytes;
		return (ref.base == base_kind::opposing || ref.base == base_kind::own) && ref.byte_offset < max_bank_span;
	}

	// The loaded image ends before the motion archive. Also stop at the other slot if it begins sooner.
	constexpr std::uint32_t span(bank slot, bank other) noexcept
	{
		if (slot.image_end <= slot.base || std::uint64_t{slot.image_end} > std::uint64_t{slot.base} + max_bank_span)
			return 0;
		const auto bytes = slot.image_end - slot.base;
		return other.base > slot.base && other.base - slot.base < bytes ? other.base - slot.base : bytes;
	}

	constexpr std::optional<std::uint32_t> resolve(reference ref, banks loaded, std::uint32_t alignment) noexcept
	{
		if (!valid(ref, alignment))
			return std::nullopt;
		if (ref.base == base_kind::executable)
			return executable_table + ref.byte_offset;
		const bool opposing = ref.base == base_kind::opposing;
		const bank slot = opposing ? loaded.opposing : loaded.own;
		const bank other = opposing ? loaded.own : loaded.opposing;
		if (!slot.base || slot.base % alignment || slot.style != ref.bank_style ||
			std::uint64_t{ref.byte_offset} + alignment > span(slot, other) ||
			std::uint64_t{slot.base} + ref.byte_offset > 0xffffffffu)
			return std::nullopt;
		return slot.base + ref.byte_offset;
	}

	// The opposing slot is tried first, as the game's own writers aim the cursor and the result at the opponent's bank.
	constexpr std::optional<reference> identify(std::uint32_t address, banks loaded, std::uint32_t alignment) noexcept
	{
		for (const auto base : {base_kind::opposing, base_kind::own})
		{
			const bank slot = base == base_kind::opposing ? loaded.opposing : loaded.own;
			if (!slot.base || address < slot.base)
				continue;
			const reference ref{base, address - slot.base, slot.style};
			if (resolve(ref, loaded, alignment) == address)
				return ref;
		}
		if (address < executable_table || address - executable_table >= executable_table_bytes)
			return std::nullopt;
		const reference ref{base_kind::executable, address - executable_table, 0};
		return resolve(ref, loaded, alignment) == address ? std::optional<reference>{ref} : std::nullopt;
	}
} // namespace pbd::replay_bank_offset
