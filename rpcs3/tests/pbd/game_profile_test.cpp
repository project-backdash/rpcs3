#include "pbd_test.h"

#include "Emu/Cell/pbd_game_profile.h"
#include "Emu/PBD/pbd_player_plates.h"

using pbd::test::check;

PBD_TEST(PbdGameProfile, CharacterRanksJoinTheRecordAgreementWithoutChangingFixedPicks)
{
	using namespace pbd::game_profile;
	const std::array<std::optional<record>, 2> records{record{4, 1}, record{}};
	const std::array<std::optional<u32>, 2> ranks{19, 0};
	check(profiles_word(records, {}) == records_word(records));
	check(profiles_word(records, ranks) != records_word(records));
	check(profiles_word(records, ranks) != profiles_word(records, {20, 0}));
	check(profiles_word(records, ranks) != profiles_word(records, {0, 19}));
	check(profiles_word(records, {0, std::nullopt}) != profiles_word(records, {std::nullopt, 0}));
	check(agreed_ranks(ranks, true) == ranks);
	check(agreed_ranks(ranks, false) == std::array<std::optional<u32>, 2>{0, 0});
	check(agreed_ranks({19, std::nullopt}, false) == std::array<std::optional<u32>, 2>{0, std::nullopt});
}

PBD_TEST(PbdGameProfile, TheRecordsAreWhereTheGameReadsThem)
{
	using namespace pbd::game_profile;
	check(name_address(0) == 0x106c9a68 && name_address(1) == 0x106c9c5c);
	check(rank_address(0) == 0x106c9a50 && rank_address(1) == 0x106c9c44);
	// The name field ends inside its own record.
	check(name_offset + name_units * 2 <= record_stride && rank_offset < name_offset);
}

PBD_TEST(PbdGameProfile, NamesAreWrittenAsTheGameDrawsThem)
{
	using namespace pbd::game_profile;
	// UTF-16 big-endian units, then zero units.
	const auto ana = encode_name("Ana");
	check(ana[0] == 0 && ana[1] == 'A' && ana[2] == 0 && ana[3] == 'n' && ana[4] == 0 && ana[5] == 'a' && ana[6] == 0 && ana[7] == 0);
	// At most 16 characters, the width of an online ID; the rest of the field stays zero.
	const auto longest = encode_name("ABCDEFGHIJKLMNOPQRSTUVWX");
	check(longest[31] == 'P' && longest[32] == 0 && longest[33] == 0 && longest[39] == 0);
	// Bytes the font would not draw as themselves are left out, and leading spaces too (the game drops trailing ones).
	const auto mixed = encode_name("  K\xC3\xA9n_1\n");
	check(mixed[1] == 'K' && mixed[3] == 'n' && mixed[5] == '_' && mixed[7] == '1' && mixed[8] == 0 && mixed[9] == 0);
	check(encode_name("") == std::array<u8, name_units * 2>{} && encode_name("   ") == std::array<u8, name_units * 2>{});
}

PBD_TEST(PbdGameProfile, RanksFollowTheGamesOrder)
{
	using namespace pbd::game_profile;
	check(rank_names[0] == "Beginner" && rank_names[9] == "1st Kyu" && rank_names[10] == "1st Dan" && rank_names[17] == "Shihan");
	check(rank_names[33] == "Tekken Lord" && rank_names[34] == "Dark Lord");
	check(rank_byte(0) == 0 && rank_byte(10) == 10 && rank_byte(36) == 36);
	// Past the last rank the game itself draws Beginner.
	check(rank_byte(37) == 0 && rank_byte(255) == 0);
}

PBD_TEST(PbdGameProfile, ThePlatesShowForHumanPlayersOutsideTheArcadeModes)
{
	using namespace pbd::game_profile;
	// A PBD set's game: VS (4), the players word set, both players human.
	check(plate_drawn(4, 1, 3, 0) && plate_drawn(4, 1, 3, 1));
	// Practice (10) and the arcade modes 2 and 3 take another path.
	check(!plate_drawn(10, 1, 3, 0) && !plate_drawn(2, 1, 3, 0) && !plate_drawn(3, 1, 3, 1));
	// Without the players word, or for a side that is not human, no plate.
	check(!plate_drawn(4, 0, 3, 0) && !plate_drawn(4, 1, 1, 1) && plate_drawn(4, 1, 1, 0) && !plate_drawn(4, 1, 3, 2));
}

PBD_TEST(PbdGameProfile, ThePlayerDataNumbersAreWhereTheBarReadsThem)
{
	using namespace pbd::game_profile;
	check(wins_address(0) == 0x106c9a3c && losses_address(0) == 0x106c9a40);
	check(wins_address(1) == 0x106c9c30 && losses_address(1) == 0x106c9c34);
	// Their 8 bytes end where the streak at +0x110 begins, before the rank, the bar's hide field at +0x11e and the name, so
	// writing them never touches any of those.
	check(losses_offset == wins_offset + 4 && losses_offset + 4 <= 0x110 && 0x110 < rank_offset && rank_offset < 0x11e && 0x11e < name_offset);
}

PBD_TEST(PbdGameProfile, ThePlayerDataNumbersAreBigEndianWordsCappedLikeTheGames)
{
	using namespace pbd::game_profile;
	check(encode_record({41, 30}) == std::array<u8, 8>{0, 0, 0, 41, 0, 0, 0, 30});
	check(encode_record({0x12345, 0x100}) == std::array<u8, 8>{0, 1, 0x23, 0x45, 0, 0, 1, 0});
	check(encode_record({}) == std::array<u8, 8>{});
	// 99999 (0x1869f) is the most the game shows; anything larger is written as 99999.
	check(encode_record({99999, 100000}) == std::array<u8, 8>{0, 1, 0x86, 0x9f, 0, 1, 0x86, 0x9f});
	check(shown(41, 1) == 42 && shown(0, 0) == 0 && shown(99998, 1) == 99999);
	check(shown(99999, 1) == 99999 && shown(5, 0xffffffffu) == 99999 && shown(0xffffffffu, 0) == 99999);
}

// Each PC announces the numbers it would write as one word; the bars change only to numbers both announced. The word tells a
// side without a record from one with zeros, so both replays record the same, and caps like the bars.
PBD_TEST(PbdGameProfile, BothPCsCompareThePlayerDataNumbersByOneWord)
{
	using namespace pbd::game_profile;
	using shown_pair = std::array<std::optional<record>, 2>;
	check(records_word(shown_pair{}) == 0x4211a50du && records_word(shown_pair{record{41, 30}, std::nullopt}) == 0x2ea4f67bu);
	check(records_word(shown_pair{record{41, 30}, std::nullopt}) == records_word(shown_pair{record{41, 30}, std::nullopt}));
	check(records_word(shown_pair{}) != records_word(shown_pair{record{}, record{}}));
	check(records_word(shown_pair{record{41, 30}, std::nullopt}) != records_word(shown_pair{std::nullopt, record{41, 30}}));
	check(records_word(shown_pair{record{41, 30}, record{}}) != records_word(shown_pair{record{41, 31}, record{}}));
	check(records_word(shown_pair{record{99999, 0}, std::nullopt}) == records_word(shown_pair{record{100000, 0}, std::nullopt}));
}

// The numbers stay as they are from the end of the VS screen (10) through the loads (11, 12) and the fight (16, 17).
PBD_TEST(PbdGameProfile, ThePlayerDataNumbersHoldFromTheVsScreenToTheFightsEnd)
{
	using namespace pbd::game_profile;
	for (const u32 scene : {11u, 12u, 16u, 17u})
		check(!records_may_change(scene));
	for (const u32 scene : {0u, 1u, 2u, 3u, 8u, 9u, 10u, 13u, 14u, 15u})
		check(records_may_change(scene));
}

PBD_TEST(PbdGameProfile, FriendPlatesUseTheSelectedCharacterAndAgreeOnBothNames)
{
	using namespace pbd::player_plates;
	const auto parsed = parse(R"({"schema":"pbd-player-plates-1","left":{"name":"Own","ranks":[{"rows":[12,13],"rank":19}]},"right":{"name":"Peer","ranks":[]}})");
	check(parsed);
	const auto shown = selected(*parsed, {13, 32});
	check(shown.names == std::array<std::string, 2>{"Own", "Peer"});
	check(shown.ranks == std::array<std::uint32_t, 2>{19, 0});
	check(selected(*parsed, {32, 12}).ranks[0] == 0);
	auto other = shown;
	other.names[1] = "Different";
	check(word(shown) != word(other));
	other = shown;
	other.ranks[0] = 20;
	check(word(shown) != word(other));
	check(agreed(shown, false) == plan{});
	check(agreed(shown, true) == shown);
	check(replay_names(shown) == shown.names);
	check(replay_names(plan{}) == std::array<std::string, 2>{"Player 1", "Player 2"});
	const auto malformed = parse(R"({"schema":"pbd-player-plates-1","left":{"name":"<bad>","ranks":[{"rows":[85],"rank":19}]},"right":{"name":"Peer"}})");
	check(malformed && malformed->ignored.size() == 2);
	check(selected(*malformed, {12, 32}).names[0].empty());
	check(!parse(R"({"schema":"other"})"));
}
