#pragma once

#include <algorithm>
#include <string_view>

namespace pbd::practice
{
	enum class advantage_tone
	{
		plain,
		green,
		yellow,
		orange,
		red,
	};

	// Only the leading number sets the tone, including values with reaction labels or alternate branches.
	inline constexpr advantage_tone advantage_tone_of(std::string_view value) noexcept
	{
		if (value.empty())
			return advantage_tone::plain;
		const bool negative = value.front() == '-';
		if (negative || value.front() == '+')
			value.remove_prefix(1);
		if (value.empty() || value.front() < '0' || value.front() > '9')
			return advantage_tone::plain;

		unsigned magnitude = 0;
		while (!value.empty() && value.front() >= '0' && value.front() <= '9')
		{
			// Only the thresholds matter, so cap the magnitude to avoid overflow.
			magnitude = (std::min)(11u, magnitude * 10u + static_cast<unsigned>(value.front() - '0'));
			value.remove_prefix(1);
		}
		if (!magnitude)
			return advantage_tone::plain;
		if (!negative)
			return advantage_tone::green;
		if (magnitude <= 7)
			return advantage_tone::yellow;
		if (magnitude <= 10)
			return advantage_tone::orange;
		return advantage_tone::red;
	}
} // namespace pbd::practice
