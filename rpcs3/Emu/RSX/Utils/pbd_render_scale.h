#pragma once

#include "../../PBD/pbd_env.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace rsx::pbd
{
	struct render_scale_ratio
	{
		std::uint32_t numerator;
		std::uint32_t denominator;
	};

	constexpr render_scale_ratio render_ratio(std::uint16_t percentage, std::string_view resolution)
	{
		// Whole percentages cannot produce these exact sizes from a 1920 x 1080 render surface.
		if (percentage == 67 && resolution == "1280x720")
			return {2, 3};
		if (percentage == 133 && resolution == "2560x1440")
			return {4, 3};
		return {percentage, 100};
	}

	inline render_scale_ratio render_ratio(std::uint16_t percentage)
	{
		// One process owns one presentation preference. Other applications keep their ordinary percentage scaling.
		static const std::string resolution = []
		{
			const char* requested = ::pbd::env::find("PBD_RENDER_RESOLUTION");
			return requested ? std::string(requested) : std::string{};
		}();
		// The in-game Settings menu switches between the launcher's sizes while the game runs, so a PBD process (one that asked
		// for a size at all) keeps every launcher size exact, not only the one it started with.
		if (!resolution.empty() && percentage == 67)
			return render_ratio(percentage, "1280x720");
		if (!resolution.empty() && percentage == 133)
			return render_ratio(percentage, "2560x1440");
		return render_ratio(percentage, resolution);
	}
} // namespace rsx::pbd
