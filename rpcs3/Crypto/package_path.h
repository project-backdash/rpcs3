#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace pkg_path
{
	inline std::filesystem::path from_utf8(std::string_view value)
	{
		return std::filesystem::path(std::u8string(value.begin(), value.end()));
	}

	inline std::string to_utf8(const std::filesystem::path& value)
	{
		const auto text = value.u8string();
		return std::string(text.begin(), text.end());
	}

	inline bool inside(const std::filesystem::path& directory, const std::filesystem::path& path)
	{
		auto base = directory;
		if (base.filename().empty())
			base = base.parent_path();
		return std::mismatch(base.begin(), base.end(), path.begin(), path.end()).first == base.end();
	}
} // namespace pkg_path
