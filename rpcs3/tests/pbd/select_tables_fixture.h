#pragma once

#include "Emu/Cell/pbd_hidden_selection.h"

// The select's tables as the pinned executable holds them (pbd::hidden_selection::read_native_tables), for tests that steer it.
namespace pbd::test
{
	inline pbd::hidden_selection::tables select_tables()
	{
		using namespace pbd::hidden_selection;
		tables value;
		// Character cells and costume rows are metadata from the pinned executable.
		constexpr u16 characters[]{21, 17, 1, 6, 3, 31, 32, 13, 8, 0, 19, 25, 20, 2, 5, 16, 14, 7, 9, 15, 10, 4, 12, 11, 35, 33, 26, 18, 27, 28, 24, 29, 23, 22, 30, 34};
		for (u32 i = 0; i < 36; ++i)
		{
			value.grid[i].character = characters[i];
			value.grid[i].neighbor = {static_cast<std::uint8_t>((i + 24) % 36), static_cast<std::uint8_t>((i + 12) % 36),
				static_cast<std::uint8_t>((i / 12) * 12 + (i + 11) % 12), static_cast<std::uint8_t>((i / 12) * 12 + (i + 1) % 12)};
		}
		value.costumes[0].rows = {0, 1};
		value.costumes[1].rows = {2, 3, 4};
		value.costumes[2].rows = {5, 6};
		value.costumes[3].rows = {7, 8};
		value.costumes[4].rows = {9, 10, 11};
		value.costumes[5].rows = {12, 13};
		value.costumes[6].rows = {14, 15};
		value.costumes[7].rows = {16, 17, 18, 19, 20};
		value.costumes[8].rows = {21, 22};
		value.costumes[9].rows = {25, 26, 27};
		value.costumes[10].rows = {28, 29};
		value.costumes[11].rows = {30, 31};
		value.costumes[12].rows = {32, 33};
		value.costumes[13].rows = {34, 35};
		value.costumes[14].rows = {36, 37};
		value.costumes[15].rows = {38, 39, 40};
		value.costumes[16].rows = {41, 42};
		value.costumes[17].rows = {43, 44};
		value.costumes[18].rows = {45, 46};
		value.costumes[19].rows = {47, 48};
		value.costumes[20].rows = {49, 50};
		value.costumes[21].rows = {51, 52, 53};
		value.costumes[22].rows = {54, 55};
		value.costumes[23].rows = {56, 57, 58};
		value.costumes[24].rows = {59, 60, 61, 62};
		value.costumes[25].rows = {63, 64, 65};
		value.costumes[26].rows = {66, 67};
		value.costumes[27].rows = {68, 69};
		value.costumes[28].rows = {70, 71, 72};
		value.costumes[29].rows = {73, 74};
		value.costumes[30].rows = {75, 76};
		value.costumes[31].rows = {77, 78};
		value.costumes[32].rows = {79, 80};
		value.costumes[33].rows = {23, 24};
		value.costumes[34].rows = {81, 82, 83};
		for (u32 character = 0; character < 35; ++character)
			for (u32 variant = 0; variant < value.costumes[character].rows.size(); ++variant)
				value.characters[value.costumes[character].rows[variant]] = {static_cast<u16>(character), static_cast<u16>(variant)};
		value.characters[84] = {34, 3};
		value.buttons = {0, 0, 1, 1, 255, 0, 0, 1, 1, 2, 0, 2, 1, 1, 255, 0, 2, 1, 3, 255, 0, 3, 1, 4, 2};
		value.availability = 0x7ffffffffull;
		value.verified = true;
		return value;
	}
} // namespace pbd::test
