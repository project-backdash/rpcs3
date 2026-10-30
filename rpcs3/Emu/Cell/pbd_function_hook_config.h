#pragma once

// Parses hook settings before guest state is loaded.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pbd::fn_hook
{
	// Unbound games and watchers receive the checked seed at binding.
	template <typename Environment, typename Paths>
	bool pbd_stage_sync_seed_missing(bool seeded, Environment read, Paths path)
	{
		return !seeded && !(read("PBD_SESSION") == "1" &&
							  ((read("PBD_SPECTATOR_STANDBY") == "1" && path("PBD_SPECTATOR_CHANNEL").is_absolute()) ||
								  (read("PBD_PREPARE_FOR") == "lobby" && path("PBD_MATCH_STANDBY_CHANNEL").is_absolute())));
	}

	struct address_parse_result
	{
		std::vector<std::uint32_t> values;
		std::vector<std::string> errors;
	};

	inline address_parse_result parse_hook_addresses(std::string_view name, std::string_view text)
	{
		address_parse_result result;
		constexpr std::string_view separators = ",; \t\r\n";
		for (std::size_t pos = text.find_first_not_of(separators); pos != text.npos;)
		{
			const auto end = text.find_first_of(separators, pos);
			const auto token = text.substr(pos, end == text.npos ? end : end - pos);
			std::uint32_t value = 0;
			std::string reason;
			if (!token.starts_with("0x") || token.size() <= 2)
				reason = "requires 0x followed by hexadecimal digits";
			else
			{
				const auto parsed = std::from_chars(token.data() + 2, token.data() + token.size(), value, 16);
				if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
					reason = "not a complete 32-bit hexadecimal address";
				else if (value & 3)
					reason = "not 4-byte aligned";
			}
			if (reason.empty())
				result.values.push_back(value);
			else
				result.errors.push_back(std::string(name) + " token '" + std::string(token) + "': " + reason);
			pos = end == text.npos ? end : text.find_first_not_of(separators, end);
		}
		return result;
	}

	// Half-open, file-backed executable LOAD ranges from the actual main ELF (including savestate boots).
	inline bool address_in_code(std::uint32_t address, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& ranges)
	{
		return !(address & 3) && std::any_of(ranges.begin(), ranges.end(), [address](const auto& range)
									 {
										 return address >= range.first && static_cast<std::uint64_t>(address) + 4 <= range.second;
									 });
	}
} // namespace pbd::fn_hook
