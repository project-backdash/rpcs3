#pragma once

#include <optional>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pbd
{
	template <typename Text>
	inline std::optional<std::vector<std::size_t>> retained_argument_indices(std::span<const Text> original, std::span<const Text> retained)
	{
		std::vector<std::size_t> indices;
		std::size_t cursor = 0;
		for (const auto& argument : retained)
		{
			while (cursor < original.size() && original[cursor] != argument)
				++cursor;
			if (cursor == original.size())
				return std::nullopt;
			indices.push_back(cursor++);
		}
		return indices;
	}

	// Select the last spelling of an install option before the argument separator.
	inline std::optional<std::wstring_view> install_argument(std::span<const std::wstring_view> args, std::wstring_view option)
	{
		std::optional<std::wstring_view> value;
		const std::wstring named = std::wstring(option) + L"=";
		for (std::size_t i = 1; i < args.size(); ++i)
		{
			if (args[i] == L"--")
				break;
			if (args[i] == option && i + 1 < args.size())
				value = args[++i];
			else if (args[i].starts_with(named))
				value = args[i].substr(named.size());
		}
		return value;
	}
} // namespace pbd
