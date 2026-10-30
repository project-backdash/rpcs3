#pragma once

#include "pbd_env.h"
#include "pbd_env_parse.h"
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

// Named execution policy for a player's direct peer session. Guest addresses and memory policy stay native-owned.
namespace pbd::session_policy
{
	enum class mode
	{
		diagnostic,
		direct,
		invalid
	};
	inline mode parse(std::string_view name) noexcept
	{
		return name.empty() ? mode::diagnostic : name == "direct-v1" ? mode::direct :
		                                                               mode::invalid;
	}
	struct description
	{
		mode selected = mode::diagnostic;
		bool stage_sync = false, exit_gate = false;
		bool direct() const noexcept
		{
			return selected == mode::direct;
		}
	};
	template <typename Find>
	description read(Find&& find) noexcept
	{
		const auto value = [&](const char* name) -> std::string_view
		{
			const auto* text = find(name);
			return text ? std::string_view(text) : std::string_view{};
		};
		const auto selected = parse(value("PBD_SESSION_POLICY"));
		const bool direct = selected == mode::direct;
		return {selected, direct && value("PBD_SESSION_STAGE_SYNC") != "0",
			direct && value("PBD_SESSION_EXIT_GATE") != "0" &&
				(!value("PBD_SET_CHANNEL").empty() || !value("PBD_MATCH_STANDBY_CHANNEL").empty() || value("PBD_PREPARE_FOR") == "lobby")};
	}
	inline description read() noexcept
	{
		return read([](const char* key)
			{
				return pbd::env::find(key);
			});
	}

	inline constexpr std::uint32_t gate_lag = 8, ring_size = 9, hold_index = 100, roll_begin = 110;
	// Recipe overrides belong to diagnostic runs. A named direct recipe always uses its native value.
	inline std::optional<std::uint32_t> recipe_number(bool direct, const wchar_t* text, std::uint32_t native,
		std::uint32_t minimum, std::uint32_t maximum) noexcept
	{
		if (direct || !text || !*text)
			return native;
		std::uint32_t value = 0;
		if (!pbd::env::parse_u32_decimal(text, value) || value < minimum || value > maximum)
			return std::nullopt;
		return value;
	}
	// Reseed entry, stage readiness entry/return, and the two loading state-slot entry/return pairs.
	inline constexpr std::uint32_t reseed_probe = 0x15a88;
	inline constexpr std::array<std::uint32_t, 2> stage_probes{0x52298, 0xcb96c};
	inline constexpr std::array<std::uint32_t, 4> gate_probes{0x52288, 0x52290, 0x117c70, 0x117c78};

	// Replay skips omit shadow, stage, player and skinning draws while exact withholding hides the corrected frames.
	// The player gate retains RNG-consuming paths; the stage-camera gate retains mode-2 or unreadable camera state
	// because its cache writes can be read by the next live frame before drawing.
	inline constexpr std::uint32_t player_draw = 0x2649a8;
	inline constexpr std::array<std::uint32_t, 4> replay_draw_skips{0x158d6c, 0x260d44, player_draw, 0x268778};
	struct replay_presentation
	{
		std::span<const std::uint32_t> skips;
		bool player_draw_gate = false, stage_camera_gate = false, withhold_exact = false;
	};
	// Every run of the direct policy (cold start, held game, hidden preparation) emits the same skips, so each finds the
	// same compiled objects and both PCs run the same code.
	inline replay_presentation replay_presentation_of(const description& launch) noexcept
	{
		if (!launch.direct())
			return {};
		return {replay_draw_skips, true, true, true};
	}
} // namespace pbd::session_policy
