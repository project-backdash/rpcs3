#include "pbd_test.h"

#include "Emu/Cell/pbd_game_profile.h"

using pbd::test::check;

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
