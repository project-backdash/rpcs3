#pragma once

#include <algorithm>
#include <string>

namespace pbd::display_rate
{
	struct sample
	{
		double hz = 0.;
		std::string source = "unavailable";
	};

	inline double frame_limit(double refresh_hz)
	{
		// The guest runs at 60 Hz. A slower display must not hold its flips across guest vblanks.
		return std::max(60., refresh_hz);
	}
} // namespace pbd::display_rate
