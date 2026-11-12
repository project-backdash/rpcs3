#pragma once

#include <cstdint>

namespace pbd
{
	enum class takeover_phase : std::uint32_t
	{
		watching = 0,
		armed = 1,
		controlling = 2,
		end_prompt = 3,
		enable_prompt = 4
	};
} // namespace pbd
