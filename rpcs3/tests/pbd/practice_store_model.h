#pragma once

#include <array>
#include <cstdint>
#include <vector>

// The eight takes of one recorded Practice run, as its practice-dummy-slot-<n>.json files (schema 2) hold them: the
// opponent-relative words run-length coded and the counts playback uses.
namespace store_model
{
	struct step
	{
		std::uint32_t frames = 0;
		std::uint16_t word = 0;
	};
	struct take
	{
		std::vector<step> steps;
		std::uint32_t leading = 0, trailing = 0, stop_chord = 0;
	};
	inline const std::array<take, 8> takes{{
		{{{4, 0x0000}, {5, 0x4000}, {6, 0x0000}, {87, 0x4000}}, 4, 0, 0},
		{{{10, 0x0000}, {5, 0x4000}, {17, 0x0000}}, 10, 17, 0},
		{{{8, 0x0000}, {4, 0x1000}, {28, 0x0000}}, 8, 28, 0},
		{{{7, 0x0000}, {4, 0x1000}, {7, 0x0000}, {40, 0x1000}}, 7, 0, 0},
		{{{1, 0x0000}, {6, 0x1000}, {11, 0x0000}, {21, 0xc000}, {24, 0x0000}}, 1, 24, 0},
		{{{5, 0x0000}, {6, 0x4000}, {12, 0x0000}, {1, 0x4000}, {21, 0xc000}, {17, 0x0000}}, 5, 17, 0},
		{{{6, 0x0000}, {6, 0x1000}, {12, 0x0000}, {20, 0x8000}, {11, 0x0000}}, 6, 11, 0},
		{{{4, 0x0000}, {7, 0x4000}, {11, 0x0000}, {12, 0x8000}, {8, 0x0000}}, 4, 8, 0},
	}};
} // namespace store_model
