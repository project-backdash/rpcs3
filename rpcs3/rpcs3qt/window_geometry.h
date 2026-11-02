#pragma once

#include <algorithm>
#include <cstdint>

namespace gui::utils
{
	struct window_geometry
	{
		int x;
		int y;
		int width;
		int height;
	};

	// All coordinates are logical screen coordinates, including on scaled displays.
	inline window_geometry centered_window_geometry(window_geometry screen, window_geometry base, int width, int height)
	{
		if (screen.width <= 0 || screen.height <= 0)
			return {};

		width = std::max(1, width);
		height = std::max(1, height);

		// Fit before clamping the position so that the clamp bounds cannot reverse.
		// Keep the requested aspect ratio and never enlarge an already fitting window.
		if (width > screen.width || height > screen.height)
		{
			if (std::int64_t{width} * screen.height > std::int64_t{height} * screen.width)
			{
				height = std::max(1, static_cast<int>(std::int64_t{height} * screen.width / width));
				width = screen.width;
			}
			else
			{
				width = std::max(1, static_cast<int>(std::int64_t{width} * screen.height / height));
				height = screen.height;
			}
		}

		const auto x = std::int64_t{base.x} + (std::int64_t{base.width} - width) / 2;
		const auto y = std::int64_t{base.y} + (std::int64_t{base.height} - height) / 2;
		return {
			static_cast<int>(std::clamp(x, std::int64_t{screen.x}, std::int64_t{screen.x} + screen.width - width)),
			static_cast<int>(std::clamp(y, std::int64_t{screen.y}, std::int64_t{screen.y} + screen.height - height)),
			width, height};
	}
} // namespace gui::utils
