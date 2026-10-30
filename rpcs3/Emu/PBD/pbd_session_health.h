#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pbd::session_health
{
	class activity
	{
		std::uint32_t published = 0;
		std::uint64_t advanced_ns = 0;
		bool seen = false;

	public:
		std::string_view observe(std::uint32_t through, std::uint64_t now, bool battle, bool paused, bool recovering) noexcept
		{
			if (!seen || through != published)
			{
				seen = true;
				published = through;
				advanced_ns = now;
			}
			if (recovering)
				return "waiting";
			if (paused)
			{
				advanced_ns = now;
				return "paused";
			}
			if (!battle)
				return "loading";
			// A callback can be sampled while executing normally or waiting for the next input. Its occupancy is not a fault.
			return now >= advanced_ns && now - advanced_ns >= 1'000'000'000 ? "waiting" : "running";
		}
	};

	inline constexpr std::size_t snapshot_bytes = 4096, header_bytes = 31;
	using page = std::array<char, snapshot_bytes>;
	struct progress
	{
		std::uint32_t game_number;
		std::array<std::uint32_t, 2> round_wins;
		bool operator==(const progress&) const = default;
	};
	inline bool valid(const progress& value) noexcept
	{
		return value.game_number && value.game_number <= 65535 && value.round_wins[0] < 5 && value.round_wins[1] < 5;
	}
	inline std::string progress_json(const std::optional<progress>& value)
	{
		if (!value || !valid(*value))
			return {};
		return ",\"progress\":{\"gameNumber\":" + std::to_string(value->game_number) +
		       ",\"roundWins\":{\"left\":" + std::to_string(value->round_wins[0]) +
		       ",\"right\":" + std::to_string(value->round_wins[1]) + "}}";
	}

	inline std::uint32_t checksum(std::string_view payload) noexcept
	{
		std::uint32_t result = 2166136261u;
		for (const unsigned char byte : payload)
			result = (result ^ byte) * 16777619u;
		return result;
	}

	// One allocated page is overwritten in place. The checksum lets an external reader reject a torn observation.
	inline std::optional<page> snapshot(std::string_view payload) noexcept
	{
		if (payload.empty() || payload.size() > snapshot_bytes - header_bytes)
			return {};
		page result;
		result.fill(' ');
		constexpr std::string_view magic = "PBD-HEALTH-1\n";
		std::copy(magic.begin(), magic.end(), result.begin());
		const auto hex = [&](std::size_t offset, std::uint32_t number)
		{
			constexpr char digits[] = "0123456789abcdef";
			for (unsigned digit = 0; digit < 8; ++digit)
				result[offset + digit] = digits[(number >> ((7 - digit) * 4)) & 15];
			result[offset + 8] = '\n';
		};
		hex(13, static_cast<std::uint32_t>(payload.size()));
		hex(22, checksum(payload));
		std::copy(payload.begin(), payload.end(), result.begin() + header_bytes);
		return result;
	}
} // namespace pbd::session_health
