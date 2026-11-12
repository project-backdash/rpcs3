#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pbd::json
{
	// Bytes as two lowercase hex digits each, the form the replay files give byte fields.
	inline std::string lower_hex(std::span<const std::uint8_t> bytes)
	{
		constexpr char digits[] = "0123456789abcdef";
		std::string out(2 * bytes.size(), '0');
		for (std::size_t at = 0; at < bytes.size(); ++at)
		{
			out[2 * at] = digits[bytes[at] >> 4];
			out[2 * at + 1] = digits[bytes[at] & 15];
		}
		return out;
	}
	// Reads `out` back from exactly that form. Any other text, the same bytes in capitals included, is refused and
	// leaves `out` as it was, so a byte field has one spelling.
	inline bool parse_lower_hex(std::string_view text, std::span<std::uint8_t> out) noexcept
	{
		if (text.size() != 2 * out.size() || text.find_first_not_of("0123456789abcdef") != text.npos)
			return false;
		const auto nibble = [](char c)
		{
			return c <= '9' ? c - '0' : c - 'a' + 10;
		};
		for (std::size_t at = 0; at < out.size(); ++at)
			out[at] = static_cast<std::uint8_t>(nibble(text[2 * at]) << 4 | nibble(text[2 * at + 1]));
		return true;
	}
	// The JSON string for text: quotes and backslashes are escaped, control bytes (below 0x20) become hex escapes, and
	// every other byte (DEL, '/', UTF-8) is copied as it is, so valid UTF-8 text stays valid.
	inline std::string quoted(std::string_view text)
	{
		std::string out = "\"";
		constexpr char digits[] = "0123456789abcdef";
		for (const unsigned char c : text)
		{
			if (c == '"' || c == '\\')
			{
				out += '\\';
				out += static_cast<char>(c);
			}
			else if (c < 0x20)
			{
				out += "\\u00";
				out += digits[c >> 4];
				out += digits[c & 0xf];
			}
			else
				out += static_cast<char>(c);
		}
		out += '"';
		return out;
	}
	inline std::string nullable_quoted(const char* text)
	{
		return text ? quoted(text) : "null";
	}
} // namespace pbd::json
