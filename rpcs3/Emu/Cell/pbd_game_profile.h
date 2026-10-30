#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Each player's name and rank as the game itself draws them: under the life bars in a fight and on the VS screen.
//
// The port keeps the arcade's player records, one per side (what an inserted card held), at 0x106c9934 + side * 0x1f4 (the
// address the table-of-contents slot r2 - 0x1870 holds; 0x29dcf0 copies one record of 0x1f4 bytes). The fight's life-bar task
// (tcb_tex2d_battle_t, 0x1357a0 -> 0x134048) draws, for each side, a plate with the record's name and rank whenever the game is
// in VS (mode 4 at 0x1008ebac), the word at 0x1008f080 is set and the side's bit is set in the human-players mask at
// 0x1008f0b0; a PBD set's game holds 4, 1 and 3 there. The VS screen's player panel (tcb_swg_makuma_info_t, made by 0x1186ac)
// draws the same name and rank. A PBD set starts with both records zeroed, so the game draws its own blank-name text and the
// Beginner plate; writing the players' names and ranks into the records makes it draw them.
//
// Display only. In VS the name is read only by the name sprite (0x2a1740 -> 0x105bb4). The rank is read by the rank sprite
// (0x2a180c -> 0x1059cc), the VS screen's panel, a computer opponent's level setup (0xdaa40, which takes its default level in
// mode 4 whatever the rank) and the end-of-match bookkeeping (0x2a2488), whose promotion step (0x2a2240) and score bonuses
// (0x5e5e8, 0x5e8a4) run only in the modes 0, 1 and 6. None of these draws from a random generator in VS, and the records lie
// outside the regions the per-frame sync check hashes (pbd_rollback_session_common.h combat_regions).
namespace pbd::game_profile
{
	using u8 = std::uint8_t;
	using u32 = std::uint32_t;

	inline constexpr u32 records = 0x106c9934;
	inline constexpr u32 record_stride = 0x1f4;
	// The name: UTF-16 big-endian code units, at most 20, ended by a zero unit when shorter. The name sprite copies 20 units
	// (0x1052b0), drops trailing spaces and draws the game's blank-name text instead when nothing else is left (0x105474).
	inline constexpr u32 name_offset = 0x134;
	inline constexpr std::size_t name_units = 20;
	// At most this many characters are written: the width the game's online mode gives a PSN online ID (16 characters, widened by
	// 0x136ef4 into the same kind of buffer), which the lobby's own limit for a name matches.
	inline constexpr std::size_t shown_units = 16;
	// The rank: one byte, an index into rank_names. The end-of-match clamp (0x29e970) turns a value above 36 into 0, and the
	// rank sprite (0x1059cc) draws Beginner for one above 36 as well.
	inline constexpr u32 rank_offset = 0x11c;
	inline constexpr u32 rank_count = 37;

	// The ranks in the game's order, as the English rank sprites are named (the sprite names at 0x100109a0 onwards; the
	// Japanese names at 0x1000fee8 give the same order). In English the three plates after Tekken Lord all draw the Dark Lord
	// sprite (0x1059cc maps ranks 34, 35 and 36 to one sprite); the Japanese game draws Houou, Kouryu and Kirin there.
	inline constexpr std::array<std::string_view, rank_count> rank_names{"Beginner", "9th Kyu", "8th Kyu", "7th Kyu", "6th Kyu",
		"5th Kyu", "4th Kyu", "3rd Kyu", "2nd Kyu", "1st Kyu", "1st Dan", "2nd Dan", "3rd Dan", "4th Dan", "5th Dan", "6th Dan",
		"7th Dan", "Shihan", "Expert", "Warrior", "Master", "Virtuoso", "Champion", "Lone Wolf", "Destroyer", "Berserker", "Sage",
		"Legend", "Fujin", "Raijin", "Deity", "Lord of War", "Dragon Lord", "Tekken Lord", "Dark Lord", "Kouryu", "Kirin"};

	inline constexpr u32 name_address(u32 side) noexcept
	{
		return records + side * record_stride + name_offset;
	}
	inline constexpr u32 rank_address(u32 side) noexcept
	{
		return records + side * record_stride + rank_offset;
	}

	// Per-side result rows (r2 - 0x1864): battle setup at 0x2a2aac clears VS stakes and sets change to 3 (pending);
	// bookkeeping at 0x2a2488 clears VS change, and 0x2a1e58 clears stakes. Stakes 1/3 show promotion, 2 demotion and 4 nothing;
	// change 1 shows PROMOTED TO for the winner. The +0x10 word records the arcade bonus-match marker.
	inline constexpr u32 results = 0x106c9d24;
	inline constexpr u32 result_stride = 0x3c;
	inline constexpr u32 stake_offset = 0x08;
	inline constexpr u32 change_offset = 0x0c;
	inline constexpr u32 bonus_offset = 0x10;
	inline constexpr u32 stake_none = 0, stake_promotion = 1, stake_demotion = 2;
	inline constexpr u32 change_promoted = 1;
	// The side the game names as the last game's winner (pbd::replay_memory::previous_game_flow). The next game's random draws
	// depend on it, so the rank screens read it and never write it.
	inline constexpr u32 previous_winner = 0x1008f08c;
	// The winner bytes the game leaves for a decided match: 1, 0, 1 for left and 2, 1, 0 for right.
	inline constexpr u32 winner_bytes = 0x1008ebd5;

	inline constexpr u32 stake_address(u32 side) noexcept
	{
		return results + side * result_stride + stake_offset;
	}
	inline constexpr u32 change_address(u32 side) noexcept
	{
		return results + side * result_stride + change_offset;
	}

	// Whether the fight draws a plate for `side`, from the three words above (0x134048's test for a game outside the arcade
	// modes 2, 3 and 10; Practice is mode 10 and draws none).
	inline constexpr bool plate_drawn(u32 mode, u32 players_word, u32 humans, u32 side) noexcept
	{
		return mode != 2 && mode != 3 && mode != 10 && players_word != 0 && side < 2 && ((humans >> side) & 1) != 0;
	}

	// A rank the game draws as given; anything else draws Beginner, as the game itself treats it.
	inline constexpr u8 rank_byte(u32 rank) noexcept
	{
		return rank < rank_count ? static_cast<u8>(rank) : 0;
	}

	// The record's name field for `text`: printable ASCII characters as UTF-16 big-endian units, at most shown_units of them,
	// and zero units after. Other bytes are left out, since the game's font is drawn per code unit and a byte of a UTF-8
	// sequence would draw as an unrelated character. Leading spaces are left out as well; the game drops trailing ones itself.
	inline std::array<u8, name_units * 2> encode_name(std::string_view text) noexcept
	{
		std::array<u8, name_units * 2> out{};
		std::size_t units = 0;
		for (const char c : text)
		{
			const auto byte = static_cast<unsigned char>(c);
			if (byte < 0x20 || byte > 0x7e || (byte == ' ' && units == 0))
				continue;
			if (units == shown_units)
				break;
			out[units * 2 + 1] = byte;
			units++;
		}
		return out;
	}
} // namespace pbd::game_profile
