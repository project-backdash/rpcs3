#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

namespace pbd::game_host
{
	inline bool fullscreen_key(bool f11, bool modified)
	{
		return f11 && !modified;
	}

	inline bool can_show_frame(bool first_frame, bool presentation_allowed)
	{
		return first_frame && presentation_allowed;
	}

	// Local window control has one bounded request, an exact token and no process-kill command.
	inline int parse_control(std::string_view request, std::uint32_t token)
	{
		if (!token || request.size() > 127 || !request.starts_with("PBD1 ") || !request.ends_with('\n'))
			return 0;
		request.remove_prefix(5);
		request.remove_suffix(1);
		std::uint32_t supplied = 0;
		const auto end = request.data() + request.size();
		const auto parsed = std::from_chars(request.data(), end, supplied);
		if (parsed.ec != std::errc{} || parsed.ptr == end || *parsed.ptr != ' ' || supplied != token)
			return 0;
		const std::string_view command(parsed.ptr + 1, end);
		if (command == "close")
			return 1;
		if (command == "resize")
			return 2;
		if (command == "launcher")
			return 3;
		if (command == "game")
			return 4;
		return 0;
	}
} // namespace pbd::game_host
