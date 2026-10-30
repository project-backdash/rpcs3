#pragma once

#include "pbd_practice_frames.h"

namespace pbd::practice::reference
{
	struct terminal
	{
		std::uint32_t first = 0, last = 0, target = 0;
		bool plain = false;
	};
	struct move
	{
		std::uint32_t first = 0, last = 0, length = 0;
		bool attack = false, fixed_timing = false;
		terminal end;
	};
	struct result
	{
		std::uint32_t allocation = 0;
		std::optional<std::uint32_t> startup, active, recovery, block_stun, hit_stun;
		std::optional<std::int32_t> on_block, on_hit;
	};
	inline bool standing(terminal value)
	{
		return value.plain && value.target == 0x8001 && value.first && value.first <= value.last && value.first <= 32767;
	}
	// Reference advantage assumes the first active frame connects, ordinary standing reactions and forward timing.
	// Recovery counts the frames after the final active frame. Observed contact timing is kept in frames::result.
	inline result calculate(move attack, terminal block = {}, terminal hit = {})
	{
		result out;
		out.startup = frames::startup(attack.first, attack.last, attack.length, attack.attack);
		if (!out.startup) return out;
		out.active = attack.last - attack.first + 1;
		if (!attack.fixed_timing || !standing(attack.end) || attack.end.first < attack.last || attack.end.first > attack.length)
			return out;
		out.recovery = attack.end.first - attack.last;
		const auto remaining = attack.end.first - attack.first;
		if (standing(block))
		{
			out.block_stun = block.first;
			out.on_block = static_cast<std::int32_t>(block.first) - static_cast<std::int32_t>(remaining);
		}
		if (standing(hit))
		{
			out.hit_stun = hit.first;
			out.on_hit = static_cast<std::int32_t>(hit.first) - static_cast<std::int32_t>(remaining);
		}
		return out;
	}

}
