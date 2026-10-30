#pragma once

#include <string_view>

namespace pbd::checkpoint_compatibility
{
	// Bump when checkpoint layout, capture policy, or code before the captured boundary changes.
	// The complete NUL-terminated marker is read from the executable by setup and the launcher.
	inline constexpr char key[] = "PBD_CHECKPOINT_COMPAT_V1_0000000000000000000000000000000000000000000000000000000000000001";
	inline bool accepts(const char* captured) noexcept
	{
		return !captured || !*captured || std::string_view(captured) == key;
	}
} // namespace pbd::checkpoint_compatibility
