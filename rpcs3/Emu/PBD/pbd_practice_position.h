#pragma once

#include "../Cell/pbd_game_lifecycle.h"
#include "../Cell/pbd_practice_walls.h"

#include <bit>
#include <cmath>
#include <numbers>
#include <utility>

namespace pbd::practice
{
	// Native yaw 0 faces +Z, and the 16-bit facing spans a full turn.
	inline pbd::game_lifecycle::actor_placement facing_placement(pbd::practice::walls::point at,
		pbd::practice::walls::point forward) noexcept
	{
		const float yaw = std::atan2(forward.x, forward.z);
		const auto facing = std::bit_cast<s16>(static_cast<u16>(std::lround(yaw * (32768.f / std::numbers::pi_v<float>))));
		return {static_cast<s32>(std::bit_cast<u32>(at.x)), 0, static_cast<s32>(std::bit_cast<u32>(at.z)), facing, std::bit_cast<u32>(yaw)};
	}

	// The pair a restart writes, with Restart Settings' variant applied (pbd::game_lifecycle::placement_variant). A dummy
	// turned away looks along the line from P1 through it, so it shows P1 its back wherever the pair stands, a remembered
	// pair included; on P1's own spot, with no line to follow, it turns half a turn from its facing instead. It keeps its
	// spot either way.
	inline std::array<pbd::game_lifecycle::actor_placement, 2> arranged(std::array<pbd::game_lifecycle::actor_placement, 2> pair,
		pbd::game_lifecycle::placement_variant variant) noexcept
	{
		if (variant.switched)
			std::swap(pair[0], pair[1]);
		if (!variant.dummy_back)
			return pair;
		const auto coordinate = [](s32 bits)
		{
			return std::bit_cast<float>(static_cast<u32>(bits));
		};
		auto& dummy = pair[1];
		const pbd::practice::walls::point at{coordinate(dummy.x), coordinate(dummy.z)};
		const pbd::practice::walls::point away{at.x - coordinate(pair[0].x), at.z - coordinate(pair[0].z)};
		if (std::isfinite(away.x) && std::isfinite(away.z) && std::hypot(away.x, away.z) >= 1.f)
		{
			const auto turned = facing_placement(at, away);
			dummy.facing = turned.facing;
			dummy.yaw_bits = turned.yaw_bits;
			return pair;
		}
		const float yaw = std::bit_cast<float>(dummy.yaw_bits);
		dummy.facing = std::bit_cast<s16>(static_cast<u16>(std::bit_cast<u16>(dummy.facing) + 0x8000u));
		dummy.yaw_bits = std::bit_cast<u32>(yaw > 0.f ? yaw - std::numbers::pi_v<float> : yaw + std::numbers::pi_v<float>);
		return pair;
	}

	std::optional<pbd::game_lifecycle::placement_token> random_destination(
		const pbd::practice::walls::boundary& arena, const pbd::game_lifecycle::practice_reset& command,
		std::array<u32, 2> actor_addresses);
} // namespace pbd::practice
