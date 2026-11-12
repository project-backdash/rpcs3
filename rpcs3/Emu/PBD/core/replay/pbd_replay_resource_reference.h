#pragma once

#include <cstdint>
#include <optional>

namespace pbd::replay_resource
{
	enum class section_kind : std::uint32_t
	{
		none,
		records80,
		samples16
	};
	struct cursor_reference
	{
		section_kind kind = section_kind::none;
		std::uint32_t index = 0, byte_offset = 0;
		std::uint16_t bank_style = 0;
	};
	struct catalog
	{
		std::uint32_t start = 0, count = 0, next = 0;
	};

	constexpr std::uint32_t stride(section_kind kind) noexcept
	{
		return kind == section_kind::records80 ? 80 : kind == section_kind::samples16 ? 2 :
		                                                                                0;
	}

	constexpr bool valid_catalog(section_kind kind, catalog table) noexcept
	{
		const auto width = stride(kind);
		return width && table.start && !(table.start & 1) && table.count && table.count <= 0x100000 &&
		       std::uint64_t{table.start} + std::uint64_t{table.count} * width == table.next;
	}

	constexpr std::optional<std::uint32_t> resolve(
		cursor_reference ref, catalog table, std::uint16_t loaded_style) noexcept
	{
		if (ref.kind == section_kind::none)
			return !ref.index && !ref.byte_offset && !ref.bank_style ? std::optional<std::uint32_t>{0} : std::nullopt;
		const auto width = stride(ref.kind);
		if (!valid_catalog(ref.kind, table) || ref.bank_style != loaded_style || ref.index >= table.count ||
			ref.byte_offset >= width || (ref.byte_offset & 1))
			return std::nullopt;
		const auto address = std::uint64_t{table.start} + std::uint64_t{ref.index} * width + ref.byte_offset;
		// The native cursor consumes one halfword, including the final sample in the catalog.
		if (address + 2 > table.next || address > 0xfffffffe)
			return std::nullopt;
		return static_cast<std::uint32_t>(address);
	}

	constexpr std::optional<cursor_reference> identify(
		section_kind kind, catalog table, std::uint16_t loaded_style, std::uint32_t address) noexcept
	{
		if (!valid_catalog(kind, table) || address < table.start || address >= table.next)
			return std::nullopt;
		const auto width = stride(kind);
		const cursor_reference ref{
			kind, (address - table.start) / width, (address - table.start) % width, loaded_style};
		const auto resolved = resolve(ref, table, loaded_style);
		return resolved && *resolved == address ? std::optional<cursor_reference>{ref} : std::nullopt;
	}
} // namespace pbd::replay_resource
