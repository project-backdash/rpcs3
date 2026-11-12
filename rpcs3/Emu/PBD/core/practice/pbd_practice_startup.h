#pragma once

#include "pbd_practice_frame_model.h"

namespace pbd::practice::frames
{
	// A jump can carry its cursor into an attack or finish a separate windup before the attack restarts. Keep the
	// input's origin across those parts; an attack window abandoned before its first active frame is not a first hit.
	class startup_sequence
	{
		std::optional<std::int64_t> origin, active;
		std::optional<std::uint32_t> first;
		std::optional<std::uint32_t> grab;
		bool windup = false;
		bool abandoned = false;

	public:
		void reset()
		{
			*this = {};
		}
		std::optional<std::uint32_t> connected() const
		{
			return grab;
		}
		bool skipped() const
		{
			return abandoned;
		}

		std::optional<std::uint32_t> commit(tick now, std::uint32_t cursor, bool attack, bool prelude,
			bool continues, std::optional<std::uint32_t> first_active, std::optional<std::uint32_t> last_active)
		{
			grab = attack && continues ? first : std::nullopt;
			abandoned = !attack && continues && active && *active >= now;
			const bool reached = continues && active && *active < now;
			if (!cursor || cursor > 32767)
			{
				reset();
				return {};
			}
			if (!attack)
			{
				if (reached)
					return first;
				if (prelude || (continues && active && *active >= now))
				{
					if (!windup && !(continues && active))
						origin = cursor == 1 ? std::optional<std::int64_t>(std::int64_t{now} - 1) : std::nullopt;
					windup = true;
					active.reset();
					first.reset();
				}
				else
					reset();
				return {};
			}
			if (reached)
				return first;
			if (!windup && !(continues && active))
				origin = cursor == 1 ? std::optional<std::int64_t>(std::int64_t{now} - 1) : std::nullopt;
			windup = false;
			active.reset();
			first.reset();
			if (!origin || !first_active || (last_active && cursor > *last_active))
				return {};
			active = std::int64_t{now} + (std::max)(*first_active, cursor) - cursor;
			const auto elapsed = *active - *origin;
			if (elapsed > 0 && elapsed <= 32767)
				first = static_cast<std::uint32_t>(elapsed);
			return first;
		}
	};
} // namespace pbd::practice::frames
