#pragma once

#include <array>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// Strict readers for the numbers in the emulator's switches. Unlike wcstoul they take no space, sign or prefix and refuse
// a value that does not fit, so a malformed switch is refused instead of being read as 0 or as the largest value.
namespace pbd::env
{
	// Unsigned decimal, or 0x-prefixed hex when base is zero; base 16 also accepts unprefixed hex.
	// No signs, whitespace, trailing text or overflow. Refusal leaves the destination unchanged.
	template <typename Char>
	constexpr bool parse_unsigned(std::basic_string_view<Char> text, std::uint64_t& value,
		std::uint64_t maximum = UINT64_MAX, unsigned base = 0) noexcept
	{
		if ((base == 0 || base == 16) && text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
		{
			base = 16;
			text.remove_prefix(2);
		}
		if (base == 0)
			base = 10;
		if (text.empty())
			return false;
		std::uint64_t result = 0;
		for (const Char c : text)
		{
			const unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
			                                                    c >= 'A' && c <= 'F'     ? c - 'A' + 10 :
			                                                                               16;
			if (digit >= base || digit > maximum || result > (maximum - digit) / base)
				return false;
			result = result * base + digit;
		}
		value = result;
		return true;
	}
	inline bool parse_u32(std::wstring_view text, std::uint32_t& value, unsigned base = 0) noexcept
	{
		std::uint64_t result = 0;
		if (!parse_unsigned(text, result, 0xffffffffu, base))
			return false;
		value = static_cast<std::uint32_t>(result);
		return true;
	}
	template <typename Char>
	constexpr bool parse_memory_range(std::basic_string_view<Char> text, std::uint64_t& begin, std::uint64_t& end) noexcept
	{
		const auto dash = text.find(static_cast<Char>('-'));
		std::uint64_t first = 0, last = 0;
		if (dash == text.npos || !parse_unsigned(text.substr(0, dash), first, 0x100000000ull, 16) ||
			!parse_unsigned(text.substr(dash + 1), last, 0x100000000ull, 16) || first >= last)
			return false;
		begin = first;
		end = last;
		return true;
	}
	template <std::size_t N>
	bool parse_decimal_fields(std::wstring_view text, std::array<std::uint32_t, N>& values) noexcept
	{
		std::array<std::uint32_t, N> parsed{};
		for (std::size_t i = 0; i < N; ++i)
		{
			const auto colon = text.find(L':');
			if ((colon == text.npos) != (i == N - 1) || !parse_u32(text.substr(0, colon), parsed[i], 10))
				return false;
			if (colon != text.npos)
				text.remove_prefix(colon + 1);
		}
		values = parsed;
		return true;
	}
	// The decimal number `text` spells. False, with `value` unchanged, for an empty text, any character other than the
	// digits 0-9, or a number above 4294967295.
	constexpr bool parse_u32_decimal(std::wstring_view text, std::uint32_t& value) noexcept
	{
		if (text.empty())
			return false;
		std::uint64_t result = 0;
		for (const wchar_t c : text)
		{
			if (c < L'0' || c > L'9')
				return false;
			result = result * 10 + static_cast<std::uint32_t>(c - L'0');
			if (result > 0xffffffffu)
				return false;
		}
		value = static_cast<std::uint32_t>(result);
		return true;
	}

	// A start state lists at most this many scene/subscene pairs it may begin in.
	inline constexpr std::size_t max_initial_scenes = 8;

	// PBD_FIXTURE_SCENES: one to max_initial_scenes "scene/subscene" pairs separated by commas, such as "17/44,16/44".
	// Nothing for any other text, including an empty pair or a trailing comma.
	inline std::optional<std::vector<std::pair<std::uint32_t, std::uint32_t>>> parse_initial_scenes(std::wstring_view text)
	{
		std::vector<std::pair<std::uint32_t, std::uint32_t>> scenes;
		for (;;)
		{
			const auto comma = text.find(L',');
			const auto item = text.substr(0, comma);
			const auto slash = item.find(L'/');
			std::uint32_t scene = 0, subscene = 0;
			if (slash == std::wstring_view::npos || scenes.size() == max_initial_scenes ||
				!parse_u32_decimal(item.substr(0, slash), scene) || !parse_u32_decimal(item.substr(slash + 1), subscene))
				return std::nullopt;
			scenes.emplace_back(scene, subscene);
			if (comma == std::wstring_view::npos)
				return scenes;
			text.remove_prefix(comma + 1);
		}
	}
} // namespace pbd::env
