#pragma once

#include <array>
#include <cstdint>

#include "pbd_stage_rule.h"

namespace pbd::practice
{
	enum class pbd_lesson_side : std::uint8_t { left, right };
	enum class pbd_lesson_stance : std::uint8_t { standing, crouching };
	enum class pbd_lesson_hit_condition : std::uint8_t { normal, counter };

	struct pbd_lesson_setup
	{
		// Base IDs ordered player, dummy; side selects the player port, independently of current input facing.
		std::array<std::uint32_t, 2> characters{~0u, ~0u};
		std::uint32_t stage = ~0u;
		pbd_lesson_side side = pbd_lesson_side::left;
		pbd_lesson_stance stance = pbd_lesson_stance::standing;
		pbd_lesson_hit_condition hit_condition = pbd_lesson_hit_condition::normal;
	};
	struct pbd_lesson_game
	{
		pbd_lesson_setup setup;
		std::uint32_t mode = 0;
		bool in_match = false, paused = false;
	};
	enum class pbd_lesson_check : std::uint8_t
	{
		ready,
		invalid_setup,
		unsupported_stage,
		not_practice,
		not_ready,
		wrong_fighters,
		wrong_stage,
		wrong_side,
		wrong_stance,
		wrong_hit_condition
	};
	constexpr std::uint32_t pbd_lesson_play_port(pbd_lesson_side side, bool demonstration) noexcept
	{
		const std::uint32_t player = side == pbd_lesson_side::right ? 1 : 0;
		return demonstration ? player : 1 - player;
	}
	constexpr pbd_lesson_check pbd_admit_lesson(const pbd_lesson_setup& requested, const pbd_lesson_game& game) noexcept
	{
		if (requested.characters[0] >= 35 || requested.characters[1] >= 35 || requested.side > pbd_lesson_side::right ||
			requested.stance > pbd_lesson_stance::crouching || requested.hit_condition > pbd_lesson_hit_condition::counter)
			return pbd_lesson_check::invalid_setup;
		if (!stage_rule::loadable(requested.stage))
			return pbd_lesson_check::unsupported_stage;
		if (game.mode != 10)
			return pbd_lesson_check::not_practice;
		if (!game.in_match || game.paused)
			return pbd_lesson_check::not_ready;
		if (game.setup.characters != requested.characters)
			return pbd_lesson_check::wrong_fighters;
		if (game.setup.stage != requested.stage)
			return pbd_lesson_check::wrong_stage;
		if (game.setup.side != requested.side)
			return pbd_lesson_check::wrong_side;
		if (game.setup.stance != requested.stance)
			return pbd_lesson_check::wrong_stance;
		if (game.setup.hit_condition != requested.hit_condition)
			return pbd_lesson_check::wrong_hit_condition;
		return pbd_lesson_check::ready;
	}
} // namespace pbd::practice
