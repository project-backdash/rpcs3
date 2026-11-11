#pragma once

#include <string_view>

namespace pbd::checkpoint_compatibility
{
	// Bump when checkpoint layout, capture policy, or code before the captured boundary changes.
	// The complete NUL-terminated marker is read from the executable by setup and the launcher.
	inline constexpr char key[] = "PBD_CHECKPOINT_COMPAT_V1_0000000000000000000000000000000000000000000000000000000000000002";
	inline bool accepts(const char* captured) noexcept
	{
		return !captured || !*captured || std::string_view(captured) == key;
	}

	struct captured_version
	{
		unsigned short type;
		unsigned short version;
	};

	// The savestate versions a capture under this key records: the newest version of every savestate category when the key
	// was cut (savestate_utils.cpp). A build that cannot load one of them refuses every capture made under this key, so it
	// needs a new key and this list renewed; checkpoint_versions_test.cpp fails when a build stops loading them.
	inline constexpr captured_version captured_versions[] = {
		{0, 25},
		{1, 3},
		{2, 1},
		{3, 1},
		{4, 1},
		{5, 3},
		{6, 2},
		{7, 3},
		{8, 4},
		{9, 1},
		{10, 8},
		{11, 1},
		{12, 1},
		{13, 2},
		{14, 2},
		{15, 3},
		{16, 1},
		{17, 1},
		{18, 1},
		{19, 1},
		{20, 1},
		{21, 1},
		{22, 1},
		{23, 2},
		{24, 1},
		{25, 1},
		{26, 2},
		{27, 1},
		{28, 1},
		{29, 1},
		{30, 1},
		{31, 1},
		{32, 1},
		{33, 1},
	};
} // namespace pbd::checkpoint_compatibility
