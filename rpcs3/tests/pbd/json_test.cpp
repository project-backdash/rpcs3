// Golden bytes of the JSON strings the emulator writes into the set channel's events, the in-game settings files and the
// replay files' byte fields: the launcher reads all three, so an escaping or spelling change would change what it parses.
#include "pbd_test.h"

#include "Emu/PBD/pbd_json.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using pbd::test::check;

PBD_TEST(PbdJson, Quoted)
{
	using pbd::json::quoted;
	check(pbd::json::nullable_quoted(nullptr) == "null");
	check(pbd::json::nullable_quoted("") == quoted(""));
	check(pbd::json::nullable_quoted("\x01\n\t") == quoted("\x01\n\t"));
	check(quoted("") == "\"\"", "empty text is two quotes");
	check(quoted("lobby") == "\"lobby\"", "plain text is only wrapped in quotes");
	check(quoted("a\"b\\c") == "\"a\\\"b\\\\c\"", "quotes and backslashes are escaped with a backslash");
	check(quoted("a\"b\\c\x01") == "\"a\\\"b\\\\c\\u0001\"", "a control byte after escapes is a hex escape");
	check(quoted(std::string_view("\0\x1f", 2)) == "\"\\u0000\\u001f\"", "the first and last control bytes are hex escapes");
	check(quoted("\n\r\t") == "\"\\u000a\\u000d\\u0009\"", "newline, return and tab use hex escapes, not short ones");
	check(quoted(" /\x7f") == "\" /\x7f\"", "space, slash and DEL are copied as they are");
	check(quoted("caf\xc3\xa9 \xe2\x9c\x93") == "\"caf\xc3\xa9 \xe2\x9c\x93\"", "UTF-8 bytes are copied as they are");
	check(quoted("{0.0.0.00000000}.{aaaa}") == "\"{0.0.0.00000000}.{aaaa}\"", "braces in a device id are copied");
}

PBD_TEST(PbdJson, LowerHex)
{
	using pbd::json::lower_hex;
	using pbd::json::parse_lower_hex;
	using bytes = std::array<std::uint8_t, 4>;
	const bytes sample{0x00, 0x7f, 0xa5, 0xff};
	check(lower_hex(sample) == "007fa5ff", "each byte is two lowercase digits, high digit first");
	check(lower_hex({}).empty(), "no bytes are no digits");
	bytes read{};
	check(parse_lower_hex("007fa5ff", read) && read == sample, "the digits read back as the same bytes");
	check(parse_lower_hex("", std::span<std::uint8_t>{}), "no digits read back as no bytes");
	for (const auto* text : {"007fa5f", "007fa5ff00", "007FA5FF", "007fa5fg", " 07fa5ff", "007fa5f-", "0x7fa5ff"})
	{
		bytes kept{1, 2, 3, 4};
		check(!parse_lower_hex(text, kept), text);
		check(kept == bytes{1, 2, 3, 4}, "a refused text leaves the bytes as they were");
	}
}
