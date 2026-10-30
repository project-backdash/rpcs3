#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>
#include "pbd_env_parse.h"

namespace pbd::input_script
{
	// Both headers identify the same tape format, including the stored legacy spelling.
	inline constexpr std::array<std::string_view, 2> headers{"pbd-tape-1", "DR2026_SESSION_REPLAY 1"};
	inline constexpr bool accepts_header(std::string_view value) noexcept
	{
		return value == headers[0] || value == headers[1];
	}
	inline constexpr std::size_t maximum_legacy_bytes = 64 * 1024 * 1024;
	struct legacy_script
	{
		std::map<std::uint32_t, std::vector<std::array<std::uint32_t, 3>>> inputs;
		std::map<std::uint32_t, std::uint32_t> sync;
		const char* error = nullptr;
		std::uint32_t line = 0;
	};
	inline legacy_script parse_legacy(std::string_view text)
	{
		legacy_script out;
		if (text.size() > maximum_legacy_bytes)
		{
			out.error = "session_replay_too_large";
			return out;
		}
		bool header = false;
		while (!text.empty())
		{
			++out.line;
			const auto newline = text.find('\n');
			auto line = text.substr(0, newline);
			text.remove_prefix(newline == text.npos ? text.size() : newline + 1);
			if (line.ends_with('\r'))
				line.remove_suffix(1);
			if (line.empty() || line.front() == '#')
				continue;
			if (!header)
			{
				if (!(header = accepts_header(line)))
				{
					out.error = "session_replay_bad_header";
					return out;
				}
				continue;
			}
			std::array<std::string_view, 5> fields{};
			std::size_t count = 0;
			for (auto at = line.find_first_not_of(" \t"); at != line.npos;)
			{
				if (count == fields.size())
				{
					out.error = "session_replay_invalid_row";
					return out;
				}
				const auto end = line.find_first_of(" \t", at);
				fields[count++] = line.substr(at, end == line.npos ? end : end - at);
				at = end == line.npos ? end : line.find_first_not_of(" \t", end);
			}
			const bool segment = fields[0] == "seg";
			std::array<std::uint32_t, 4> values{};
			bool valid = segment ? count == 5 : fields[0] == "sync" && count == 3;
			for (std::size_t i = 1; valid && i < count; ++i)
			{
				std::uint64_t value = 0;
				valid = pbd::env::parse_unsigned(fields[i], value, segment && i >= 3 ? 65535 : 0xffffffffu, 10);
				values[i - 1] = static_cast<std::uint32_t>(value);
			}
			if (valid && segment)
			{
				auto& rows = out.inputs[values[0]];
				valid = rows.empty() || values[1] > rows.back()[0];
				if (valid)
					rows.push_back({values[1], values[2], values[3]});
			}
			else if (valid)
				valid = out.sync.emplace(values[0], values[1]).second;
			if (!valid)
			{
				out.error = "session_replay_invalid_row";
				return out;
			}
		}
		if (!header || out.inputs.empty())
			out.error = "session_replay_empty";
		return out;
	}
} // namespace pbd::input_script
