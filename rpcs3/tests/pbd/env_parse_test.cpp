#include "pbd_test.h"

#include "Emu/PBD/pbd_env_parse.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using pbd::test::check;

namespace
{
	using scenes = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

	// The number `text` reads as; nothing when it is refused, which must leave the value as it was.
	std::optional<std::uint32_t> decimal(std::wstring_view text)
	{
		constexpr std::uint32_t untouched = 0xdeadbeef;
		std::uint32_t value = untouched;
		if (pbd::env::parse_u32_decimal(text, value))
			return value;
		check(value == untouched, "a refused text leaves the value as it was");
		return std::nullopt;
	}

	bool refused(std::wstring_view text)
	{
		return !pbd::env::parse_initial_scenes(text).has_value();
	}
} // namespace

PBD_TEST(PbdEnvParse, MemoryRangesRequireCompleteHexEndpoints)
{
	std::uint64_t begin = 17, end = 23;
	check(pbd::env::parse_memory_range(std::string_view("0x10000-20000"), begin, end) && begin == 0x10000 && end == 0x20000);
	check(pbd::env::parse_memory_range(std::wstring_view(L"FFFFF000-0X100000000"), begin, end) && begin == 0xfffff000 && end == 0x100000000ull);
	for (const auto text : {"", "10000", "10000-", "-20000", "10000junk-20000", "10000-20000junk", "10000-20000-30000",
			 "20000-10000", "10000-10000", "10000-100000001", "+10000-20000", " 10000-20000"})
	{
		begin = 17;
		end = 23;
		check(!pbd::env::parse_memory_range(std::string_view(text), begin, end) && begin == 17 && end == 23);
		const std::wstring wide(text, text + std::char_traits<char>::length(text));
		check(!pbd::env::parse_memory_range(std::wstring_view(wide), begin, end) && begin == 17 && end == 23);
	}
}

PBD_TEST(PbdEnvParse, ADecimalNumberReadsUpTo4294967295)
{
	check(decimal(L"0") == 0u, "zero");
	check(decimal(L"7") == 7u, "one digit");
	check(decimal(L"0044") == 44u, "leading zeros are decimal, not octal");
	check(decimal(L"65535") == 65535u, "a 16-bit value");
	check(decimal(L"4294967295") == 4294967295u, "the largest value");
}

PBD_TEST(PbdEnvParse, AnythingButDigitsIsRefusedWhereWcstoulWouldReadIt)
{
	for (const std::wstring_view text : {L"", L" 5", L"5 ", L"+5", L"-1", L"0x10", L"1e3", L"5,", L"x", L"3/4"})
		check(!decimal(text), "only the digits 0-9 are read");
	const std::wstring with_nul{L'1', L'2', L'\0', L'3'};
	check(!decimal(with_nul), "an embedded terminator is not the end of the text");
}

PBD_TEST(PbdEnvParse, ANumberThatDoesNotFitIsRefusedInsteadOfClamped)
{
	check(!decimal(L"4294967296"), "one above the largest value");
	check(!decimal(L"99999999999999999999"), "far above it");
	check(decimal(L"00000000004294967295") == 4294967295u, "leading zeros never overflow");
}

PBD_TEST(PbdEnvParse, SessionNumbersConsumeTheWholeField)
{
	std::uint32_t value = 17;
	check(pbd::env::parse_u32(L"0xFFFFFFFF", value) && value == 0xffffffffu);
	check(pbd::env::parse_u32(L"0044", value) && value == 44);
	for (const auto text : {L"junk", L"12junk", L"-1", L"+2", L" 2", L"4294967296", L"0x100000000", L"0x"})
	{
		value = 17;
		check(!pbd::env::parse_u32(text, value) && value == 17);
	}
	std::uint64_t wide = 0;
	check(pbd::env::parse_unsigned(std::wstring_view(L"ffffffffffffffff"), wide, UINT64_MAX, 16) && wide == UINT64_MAX);
	check(!pbd::env::parse_unsigned(std::wstring_view(L"10000000000000000"), wide, UINT64_MAX, 16));
	std::array<std::uint32_t, 3> fields{};
	check(pbd::env::parse_decimal_fields(L"1:4294967295:2", fields) && fields[1] == 0xffffffffu);
	for (const auto text : {L"1:2:3junk", L"1:2:3:4", L"1:2", L"1:-2:1", L"1:2:4294967296"})
		check(!pbd::env::parse_decimal_fields(text, fields));
}

PBD_TEST(PbdEnvParse, InitialScenesReadAsScenePairsInOrder)
{
	check(pbd::env::parse_initial_scenes(L"3/44") == scenes{{3, 44}}, "one pair");
	check(pbd::env::parse_initial_scenes(L"17/44,16/44") == scenes{{17, 44}, {16, 44}}, "two pairs, in order");
	const auto eight = pbd::env::parse_initial_scenes(L"1/1,2/2,3/3,4/4,5/5,6/6,7/7,8/8");
	check(eight && eight->size() == pbd::env::max_initial_scenes && eight->back() == std::pair<std::uint32_t, std::uint32_t>{8, 8},
		"eight pairs");
	check(pbd::env::parse_initial_scenes(L"0/0,4294967295/4294967295") == scenes{{0, 0}, {4294967295u, 4294967295u}},
		"the whole 32-bit range");
}

PBD_TEST(PbdEnvParse, MalformedInitialScenesAreRefused)
{
	for (const std::wstring_view text : {L"x/5", L"3/4x", L"/5", L"3/", L"3", L"3/44,", L",3/44", L"3/44,,16/44", L"3/44/5",
			 L"3/4294967296", L"4294967296/3", L" 3/44", L"3/+44", L"3/-1", L"0x3/44", L""})
		check(refused(text), "a pair needs two decimal numbers, each a 32-bit value");
	check(refused(L"1/1,2/2,3/3,4/4,5/5,6/6,7/7,8/8,9/9"), "nine pairs are one too many");
}
