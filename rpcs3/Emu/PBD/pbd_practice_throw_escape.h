#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The thrown move's escape requirements 106-108 set actor +0x192 to 1, 2 or 3 inside the break window (0x39940).
// Input (0xECC94..0xED158) latches the first fresh break at +0x204; a 1+2 break needs both buttons held together.
// This control supplies presses through the dummy's Controller port after the window opens, without writing game memory.
namespace pbd::practice
{
	enum class throw_escape_option : std::uint8_t
	{
		none,
		all,
		random
	};
	inline constexpr std::uint32_t throw_escape_option_count = 3;
	inline constexpr std::array<std::string_view, throw_escape_option_count> throw_escape_names{"None", "All", "Random"};

	// Open-window reading: thrown move, requested break (+0x192) and latched input (+0x204, zero until pressed).
	struct throw_escape_view
	{
		std::uint32_t allocation = 0, wanted = 0, given = 0;
	};

	constexpr std::uint16_t throw_escape_word(std::uint32_t wanted) noexcept
	{
		return wanted == 1 ? 0x0080 : wanted == 2 ? 0x0010 : wanted == 3 ? 0x0090 : 0;
	}

	// Random consumes one caller-supplied host word per throw.
	constexpr bool throw_escape_breaks(throw_escape_option option, std::uint32_t random_word) noexcept
	{
		return option == throw_escape_option::all || (option == throw_escape_option::random && (random_word & 1) != 0);
	}

	// Alternate presses with neutral frames to produce fresh edges, stopping inside the shortest break window.
	inline constexpr std::uint32_t throw_escape_presses = 4;

	// One throw at a time, from the first boundary that shows its window open to the move change that ends it.
	struct throw_escape_control
	{
		std::uint32_t allocation = 0, presses = 0;
		std::uint16_t word = 0;
		bool active = false, breaks = false, done = false;

		void reset() noexcept { *this = {}; }
		constexpr bool fresh(const std::optional<throw_escape_view>& now) const noexcept
		{
			return now && (!active || now->allocation != allocation);
		}
		void begin(const throw_escape_view& now, bool will_break) noexcept
		{
			*this = {};
			active = true;
			allocation = now.allocation;
			breaks = will_break;
		}
		constexpr bool owns() const noexcept { return active && breaks && !done; }
		// Release the port as soon as the game latches an input or retries run out.
		std::optional<std::uint16_t> next(const std::optional<throw_escape_view>& now) noexcept
		{
			if (!now)
			{
				reset();
				return std::nullopt;
			}
			if (!owns() || now->allocation != allocation)
				return std::nullopt;
			if (now->given || (!word && presses == throw_escape_presses))
			{
				done = true;
				word = 0;
				return std::nullopt;
			}
			if (word)
			{
				word = 0;
				return word;
			}
			++presses;
			word = throw_escape_word(now->wanted);
			return word;
		}
	};
} // namespace pbd::practice
