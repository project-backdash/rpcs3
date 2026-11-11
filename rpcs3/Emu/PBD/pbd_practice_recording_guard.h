#pragma once

#include "pbd_practice_wakeup.h"

namespace pbd::practice
{
	// Live flags (+0xCC), the stance's kept flags (+0xD0), and the contact check's guard halfword (+0x152).
	using pbd_guard_values = std::array<std::uint32_t, 3>;

	template <class Write>
	std::array<bool, 3> pbd_clear_guard(const pbd_guard_values& flags, Write write)
	{
		std::array<bool, 3> written{};
		for (unsigned field = 0; field < flags.size(); ++field)
			if (flags[field] & pbd_guard_bits)
				written[field] = write(field, pbd_without_guard(flags[field]));
		return written;
	}

	struct pbd_recording_guard_view
	{
		std::uint64_t generation = 0;
		std::uint32_t counter = 0, move = 0;
		pbd_guard_values flags{};
	};

	// The player receives neutral/get-up input while recording the dummy. Remove guard by the same flags as Guard None,
	// retaining only writes we own so Stop can restore the current stance before its next frame.
	struct pbd_recording_guard
	{
		std::uint64_t generation = 0;
		std::uint32_t counter = 0, move = 0;
		bool active = false;
		std::array<std::optional<std::uint32_t>, 3> saved{};

		bool current(const pbd_recording_guard_view& now) const noexcept
		{
			return active && generation == now.generation && now.counter - counter <= 1;
		}

		template <class Write>
		void clear(const pbd_recording_guard_view& now, Write write)
		{
			for (unsigned field = 0; field < saved.size(); ++field)
				if (saved[field] && now.flags[field] != pbd_without_guard(*saved[field]))
					saved[field].reset();
			const auto written = pbd_clear_guard(now.flags, write);
			for (unsigned field = 0; field < saved.size(); ++field)
				if (written[field])
					saved[field] = now.flags[field];
		}

		template <class Write>
		void boundary(bool recording, const std::optional<pbd_recording_guard_view>& now, Write write)
		{
			if (!now)
			{
				*this = {};
				return;
			}
			const bool owned = current(*now) && move == now->move;
			if (!recording)
			{
				if (owned)
					for (unsigned field = 0; field < saved.size(); ++field)
						if (saved[field] && now->flags[field] == pbd_without_guard(*saved[field]))
							write(field, *saved[field]);
				*this = {};
				return;
			}
			if (!owned)
				saved = {};
			active = true;
			generation = now->generation;
			counter = now->counter;
			move = now->move;
			clear(*now, write);
		}

		// A move has just supplied new flags, even if it reentered the same allocation. Clear them before contact, and
		// replace the restoration snapshot with this move's flags rather than those of the previous stance.
		template <class Write>
		void entered(const std::optional<pbd_recording_guard_view>& now, Write write)
		{
			if (!now || !current(*now))
			{
				*this = {};
				return;
			}
			saved = {};
			move = now->move;
			clear(*now, write);
		}
	};
} // namespace pbd::practice
