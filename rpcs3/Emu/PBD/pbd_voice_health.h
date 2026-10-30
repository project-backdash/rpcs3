#pragma once

#include <cstdint>

namespace pbd::voice_health
{
	// Host allocation outcomes, independent of the optional voice-claim lock and rollback state.
	struct monitor
	{
		std::uint64_t successes = 0, failures = 0, exhausted = 0;
		std::uint64_t streak = 0, max_streak = 0, failure_since_us = 0;
		bool reported = false;

		void observe(bool allocated, std::uint64_t now_us) noexcept
		{
			if (allocated)
			{
				++successes;
				streak = 0;
				reported = false;
				return;
			}
			++failures;
			if (!streak++)
				failure_since_us = now_us;
			if (streak > max_streak)
				max_streak = streak;
			// Brief saturation can be normal; sustained failed requests while music still streams are not.
			if (!reported && streak >= 8 && now_us >= failure_since_us && now_us - failure_since_us >= 5000000)
			{
				++exhausted;
				reported = true;
			}
		}
	};
} // namespace pbd::voice_health
