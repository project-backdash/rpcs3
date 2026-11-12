#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace pbd::practice::pad
{
	struct sample
	{
		std::uint32_t boundary = 0, host = 0, pad = 0, pressed = 0, state = 0;
		bool menu = false, claimed = false, other_claim = false, ignore = false;
		bool restart_ignore = false, anchor = false, in_match = false;
	};
	inline std::string trace(const sample& row)
	{
		constexpr std::array<std::string_view, 12> names{"boundary", "host", "pad", "pressed", "state", "menu",
			"claimed", "other_claim", "ignore", "restart_ignore", "anchor", "in_match"};
		const auto boolean = [](bool value)
		{
			return value ? "true" : "false";
		};
		const std::array<std::string, 12> values{std::to_string(row.boundary), std::to_string(row.host),
			std::to_string(row.pad), std::to_string(row.pressed), std::to_string(row.state), boolean(row.menu),
			boolean(row.claimed), boolean(row.other_claim), boolean(row.ignore), boolean(row.restart_ignore),
			boolean(row.anchor), boolean(row.in_match)};
		std::string out = "{\"type\":\"practice_pad\"";
		for (std::size_t i = 0; i < names.size(); ++i)
		{
			out += ",\"";
			out += names[i];
			out += "\":";
			out += values[i];
		}
		return out + "}\n";
	}
} // namespace pbd::practice::pad
