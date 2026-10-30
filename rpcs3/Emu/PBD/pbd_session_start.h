#pragma once

#include <cstdint>
#include <bit>
#include <string>

namespace pbd::session_start
{
	inline constexpr std::uint16_t start_button = 0x0800;
	inline constexpr bool entry_witness_recovery(bool peer, bool soft, bool after_start) noexcept
	{
		return after_start && (peer || soft);
	}
	inline constexpr std::int32_t counter_offset(std::uint32_t actual, std::uint32_t initial, std::uint32_t index) noexcept
	{
		return std::bit_cast<std::int32_t>(actual - initial - index);
	}

	// Keyread may reset the input-side witness before or after the main thread increments it.
	// A reset after the increment is valid even when this update already entered at zero.
	inline constexpr bool input_counter_restarted(std::uint32_t before, std::uint32_t after, bool completed) noexcept
	{
		return completed && after == 0 ? true : before != 0 && after == (completed ? 1u : 0u);
	}

	struct phase
	{
		std::uint32_t scene = 0, pending = ~std::uint32_t{0};
		bool verified = false;
	};

	// Pending selects the next native dispatcher case; the completed battle counter is one update late.
	inline constexpr bool permits_skip(const phase& next) noexcept
	{
		if (!next.verified || (next.scene != 16 && next.scene != 17))
			return false;
		// Case 9 consumes the stock round replay's skip mask; the KO setup and round call do not.
		return next.pending <= 2 || (next.pending >= 9 && next.pending <= 12);
	}

	struct input
	{
		std::uint16_t word = 0;
		bool suppressed = false;
	};

	// A press withheld outside a skippable presentation stays withheld until its own port releases Start.
	inline constexpr input filter(std::uint16_t word, const phase& next, bool previously_suppressed) noexcept
	{
		const bool suppressed = (word & start_button) && (!permits_skip(next) || previously_suppressed);
		return {static_cast<std::uint16_t>(suppressed ? word & ~start_button : word), suppressed};
	}

	struct confirmed_input
	{
		std::uint32_t boundary = 0, port = 0;
		std::uint16_t raw = 0, applied = 0;
		phase next{};
	};

	// The confirmed words replace predictions; a withdrawn press produces no Start decision.
	inline std::string describe(const confirmed_input& value)
	{
		if (!(value.raw & start_button))
			return {};
		return "{\"type\":\"start_skip\",\"boundary\":" + std::to_string(value.boundary) +
		       ",\"port\":" + std::to_string(value.port) +
		       ",\"masked\":" + (value.applied & start_button ? "false" : "true") +
		       ",\"phase\":" + std::to_string(value.next.pending) +
		       ",\"phase_read\":" + (value.next.verified ? "true" : "false") + ",\"confirmed\":true}\n";
	}
} // namespace pbd::session_start
