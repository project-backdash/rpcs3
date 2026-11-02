#pragma once

#include "pbd_env.h"

#include <filesystem>
#include <string>

namespace pbd::boot_request::detail
{
	inline std::string environment(const char* name)
	{
#ifdef _WIN32
		const auto value = pbd::env::find_wide(name);
		if (value)
		{
			const auto utf8 = std::filesystem::path(value).u8string();
			return std::string(utf8.begin(), utf8.end());
		}
#else
		if (const auto value = pbd::env::find(name))
			return value;
#endif
		return {};
	}
} // namespace pbd::boot_request::detail
