#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace pbd::picture_facing
{
	// An APP15 segment records the capture side without changing the pixels or requiring a companion file.
	inline constexpr std::array<std::uint8_t, 13> marker{0xff, 0xef, 0, 12, 'P', 'B', 'D', 'P', 'H', 'O', 'T', 'O', 1};
	inline constexpr std::size_t marker_bytes = marker.size() + 1;
	inline std::uint32_t taken_on(std::span<const std::uint8_t> jpeg) noexcept
	{
		return jpeg.size() >= 16 && jpeg[0] == 0xff && jpeg[1] == 0xd8 &&
		               std::equal(marker.begin(), marker.end(), jpeg.begin() + 2) && jpeg[15] == 1 ?
		           1u :
		           0u;
	}
	inline bool mirrored(std::span<const std::uint8_t> jpeg, std::uint32_t display_side) noexcept
	{
		return taken_on(jpeg) != display_side;
	}
	inline void record(std::vector<std::uint8_t>& jpeg, std::uint32_t side)
	{
		if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8)
			return;
		jpeg.insert(jpeg.begin() + 2, marker.begin(), marker.end());
		jpeg.insert(jpeg.begin() + 15, static_cast<std::uint8_t>(side == 1));
	}
} // namespace pbd::picture_facing
