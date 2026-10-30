#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

// The DRMD001 guest memory dump: the 8 bytes "DRMD001\0", the page count, then for each page its guest address and its
// 4096 bytes. The count and the addresses are little-endian u32. Unmapped pages are left out, so a dump of a wide area
// holds only the memory the guest has mapped.
namespace pbd::memory_dump
{
	inline constexpr std::uint32_t page_size = 4096;
	inline constexpr std::array<std::uint8_t, 8> magic{'D', 'R', 'M', 'D', '0', '0', '1', 0};
	inline constexpr std::size_t header_size = 12;
	inline constexpr std::uint64_t record_size = 4 + page_size;
	// At most this many ranges in one paste request.
	inline constexpr std::size_t max_ranges = 64;

	// The guest bytes from `begin` up to `end`, `end` excluded.
	struct range
	{
		std::uint32_t begin = 0;
		std::uint32_t end = 0;
	};

	constexpr std::uint32_t little_u32(const std::uint8_t* bytes) noexcept
	{
		return std::uint32_t{bytes[0]} | std::uint32_t{bytes[1]} << 8 | std::uint32_t{bytes[2]} << 16 |
		       std::uint32_t{bytes[3]} << 24;
	}

	constexpr std::array<std::uint8_t, 4> little_bytes(std::uint32_t value) noexcept
	{
		return {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
			static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
	}

	// Where record `index` starts in the file; its page's bytes follow its 4-byte address.
	constexpr std::uint64_t record_offset(std::uint32_t index) noexcept
	{
		return header_size + index * record_size;
	}

	// The page count `header` declares, when its magic is right and the file is exactly as long as that count needs.
	inline std::optional<std::uint32_t> page_count(std::span<const std::uint8_t> header, std::uint64_t file_size) noexcept
	{
		if (header.size() < header_size || !std::equal(magic.begin(), magic.end(), header.begin()))
			return std::nullopt;
		const std::uint32_t count = little_u32(header.data() + magic.size());
		if (file_size != record_offset(count))
			return std::nullopt;
		return count;
	}

	// Writes the dump of `ranges` to `out`, whose put(bytes) appends and put_at(offset, bytes) overwrites, each false
	// when the bytes were not written. `read(address, page)` fills one page and is false for a page the guest has not
	// mapped, which the dump leaves out. A range starts at the page that holds its `begin`. The count is written last,
	// over the header's placeholder. The page count, or nothing when a write failed.
	template <typename ReadPage, typename Sink>
	std::optional<std::uint32_t> write(std::span<const range> ranges, ReadPage&& read, Sink& out)
	{
		if (!out.put(magic) || !out.put(little_bytes(0)))
			return std::nullopt;
		std::uint32_t count = 0;
		std::array<std::uint8_t, page_size> page{};
		for (const range& area : ranges)
			for (std::uint64_t address = area.begin - area.begin % page_size; address < area.end; address += page_size)
			{
				const auto at = static_cast<std::uint32_t>(address);
				if (!read(at, std::span<std::uint8_t, page_size>(page)))
					continue;
				if (!out.put(little_bytes(at)) || !out.put(page))
					return std::nullopt;
				++count;
			}
		if (!out.put_at(magic.size(), little_bytes(count)))
			return std::nullopt;
		return count;
	}

	// The file offset of each held page's bytes, by guest address; nothing when an address is unaligned or repeats.
	using page_index = std::unordered_map<std::uint32_t, std::uint64_t>;
	inline std::optional<page_index> index_pages(std::span<const std::uint32_t> addresses)
	{
		page_index pages;
		pages.reserve(addresses.size());
		for (std::size_t i = 0; i < addresses.size(); ++i)
			if (addresses[i] % page_size || !pages.emplace(addresses[i], record_offset(static_cast<std::uint32_t>(i)) + 4).second)
				return std::nullopt;
		return pages;
	}

	// One copy into guest memory: `size` bytes to `address`, read from `offset` in the dump. It never crosses a page.
	struct piece
	{
		std::uint32_t address = 0;
		std::uint32_t size = 0;
		std::uint64_t offset = 0;
	};

	struct paste_plan
	{
		std::vector<piece> pieces;
		std::uint64_t absent = 0; // bytes of the range on pages the dump does not hold
	};

	// The copies that paste `wanted` from a dump holding `pages`.
	inline paste_plan plan(const page_index& pages, range wanted)
	{
		paste_plan out;
		for (std::uint64_t at = wanted.begin; at < wanted.end;)
		{
			const std::uint64_t page = at - at % page_size;
			const std::uint64_t stop = std::min<std::uint64_t>(wanted.end, page + page_size);
			const auto size = static_cast<std::uint32_t>(stop - at);
			if (const auto found = pages.find(static_cast<std::uint32_t>(page)); found != pages.end())
				out.pieces.push_back({static_cast<std::uint32_t>(at), size, found->second + (at - page)});
			else
				out.absent += size;
			at = stop;
		}
		return out;
	}

	// What pasting one range did: the bytes written, how many of them differed from what the guest held and where the
	// first and the last of those were, and the bytes skipped because the dump lacks them or the guest cannot write them.
	struct paste_stats
	{
		std::uint64_t written = 0, changed = 0, absent = 0, unwritable = 0;
		std::uint32_t first_change = 0, last_change = 0;

		// Adds `pasted`, written at `address` over `held`.
		void add(std::uint32_t address, std::span<const std::uint8_t> held, std::span<const std::uint8_t> pasted) noexcept
		{
			written += pasted.size();
			for (std::size_t i = 0; i < pasted.size() && i < held.size(); ++i)
			{
				if (held[i] == pasted[i])
					continue;
				const std::uint32_t at = address + static_cast<std::uint32_t>(i);
				if (!changed++)
					first_change = at;
				last_change = at;
			}
		}
	};

	// A paste request: "<dump file>;<begin>-<end>[,<begin>-<end>...]", each address in hex with or without 0x. The file
	// name ends at the last ';', so a path that holds one still reads.
	template <typename Char>
	struct paste_request
	{
		std::basic_string_view<Char> file;
		std::vector<range> ranges;
	};

	namespace detail
	{
		template <typename Char>
		constexpr int hex_digit(Char c) noexcept
		{
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
			return -1;
		}

		template <typename Char>
		constexpr bool hex_u32(std::basic_string_view<Char> text, std::uint32_t& value) noexcept
		{
			if (text.size() > 2 && text[0] == Char{'0'} && (text[1] == Char{'x'} || text[1] == Char{'X'}))
				text.remove_prefix(2);
			if (text.empty() || text.size() > 8)
				return false;
			std::uint32_t out = 0;
			for (const Char c : text)
			{
				const int digit = hex_digit(c);
				if (digit < 0)
					return false;
				out = out << 4 | static_cast<std::uint32_t>(digit);
			}
			value = out;
			return true;
		}
	} // namespace detail

	// The request `text` spells; nothing when the file name is empty, a range is malformed or empty, or there are more
	// than max_ranges of them.
	template <typename Char>
	std::optional<paste_request<Char>> parse_paste(std::basic_string_view<Char> text)
	{
		const auto split = text.rfind(Char{';'});
		if (split == text.npos || split == 0)
			return std::nullopt;
		paste_request<Char> out{text.substr(0, split), {}};
		auto rest = text.substr(split + 1);
		while (true)
		{
			const auto comma = rest.find(Char{','});
			const auto item = rest.substr(0, comma);
			const auto dash = item.find(Char{'-'});
			range wanted;
			if (dash == item.npos || out.ranges.size() == max_ranges || !detail::hex_u32(item.substr(0, dash), wanted.begin) ||
				!detail::hex_u32(item.substr(dash + 1), wanted.end) || wanted.begin >= wanted.end)
				return std::nullopt;
			out.ranges.push_back(wanted);
			if (comma == rest.npos)
				return out;
			rest.remove_prefix(comma + 1);
		}
	}
} // namespace pbd::memory_dump
