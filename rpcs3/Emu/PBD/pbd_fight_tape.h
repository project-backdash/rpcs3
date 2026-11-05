#pragma once

#include "../Cell/Modules/pbd_input_script.h"
#include <atomic>
#include <limits>

namespace pbd
{
	inline std::atomic<bool> input_script_fight_mode{false};

	class fight_tape_clock
	{
		static constexpr auto unset = std::numeric_limits<std::uint64_t>::max();
		std::atomic<std::uint64_t> m_anchor{unset}, m_target{unset};

	public:
		void observe(std::uint32_t boundary, std::uint32_t scene, std::uint32_t pending, std::uint32_t ordinal,
			std::uint32_t delay) noexcept
		{
			// The recorder omits the match initializer's input: inputOffset N names input boundary match_start + N + 1.
			// Its round snapshot is taken after this update's input, so frame zero is the next applied input boundary.
			if ((scene == 16 || scene == 17) && pending == 4 && ordinal == 0)
			{
				auto expected = unset;
				m_anchor.compare_exchange_strong(expected, static_cast<std::uint64_t>(boundary) + 1, std::memory_order_acq_rel);
			}
			m_target.store(static_cast<std::uint64_t>(boundary) + delay, std::memory_order_release);
		}
		std::optional<std::uint64_t> frame(std::uint64_t applied_boundary) const noexcept
		{
			const auto anchor = m_anchor.load(std::memory_order_acquire);
			if (anchor == unset || applied_boundary < anchor)
				return std::nullopt;
			return applied_boundary - anchor;
		}
		std::optional<std::uint64_t> poll_frame() const noexcept
		{
			const auto target = m_target.load(std::memory_order_acquire);
			return target == unset ? std::nullopt : frame(target);
		}
	};

	bool fight_tape_enabled();
	void observe_fight_tape(std::uint32_t boundary, std::uint32_t scene, std::uint32_t pending, std::uint32_t ordinal,
		std::uint32_t delay);
	std::optional<input_script_output> fight_tape_at(std::uint32_t port, std::uint64_t applied_boundary);
} // namespace pbd
