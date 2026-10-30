#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace pbd::practice
{
	enum class pbd_wakeup_option : std::uint8_t
	{
		stand_up,
		stay_down,
		back_roll,
		forward_roll,
		side_roll_up,
		side_roll_down,
		rising_low_kick,
		rising_mid_kick,
		toe_kick,
		spring_kick,
		quick_roll_up,
		quick_roll_down,
		quick_roll_back,
		spring_forward,
		random
	};
	inline constexpr std::uint32_t pbd_wakeup_option_count = 15;
	inline constexpr std::array<std::string_view, pbd_wakeup_option_count> pbd_wakeup_names{
		"Stand Up", "Stay Down", "Back Roll", "Forward Roll", "Side Roll Up", "Side Roll Down", "Rising Low Kick", "Rising Mid Kick",
		"Toe Kick", "Spring Kick", "Quick Roll Up", "Quick Roll Down", "Quick Roll Back", "Spring Forward", "Random"};

	enum class pbd_ground_position : std::uint8_t
	{
		unknown,
		face_up_feet_toward,
		face_up_feet_away,
		face_down_feet_toward,
		face_down_feet_away
	};

	constexpr bool pbd_wakeup_available(pbd_wakeup_option option, pbd_ground_position position) noexcept
	{
		if (option >= pbd_wakeup_option::random)
			return false;
		return (option != pbd_wakeup_option::toe_kick && option != pbd_wakeup_option::spring_kick) ||
		       position == pbd_ground_position::face_up_feet_toward;
	}

	// The caller supplies the random word once per knockdown. This selection never draws the game's random numbers.
	constexpr pbd_wakeup_option pbd_resolve_wakeup(
		pbd_wakeup_option option, pbd_ground_position position, std::uint32_t random_word = 0, bool can_tech = true) noexcept
	{
		if (option != pbd_wakeup_option::random)
			return pbd_wakeup_available(option, position) ? option : pbd_wakeup_option::stand_up;
		std::array<pbd_wakeup_option, pbd_wakeup_option_count - 1> available{};
		std::uint32_t count = 0;
		for (std::uint32_t at = 0; at < available.size(); ++at)
		{
			const auto candidate = static_cast<pbd_wakeup_option>(at);
			if (pbd_wakeup_available(candidate, position) && (can_tech || candidate < pbd_wakeup_option::quick_roll_up))
				available[count++] = candidate;
		}
		return available[random_word % count];
	}

	constexpr std::optional<std::uint32_t> pbd_wakeup_native_action(pbd_wakeup_option option) noexcept
	{
		switch (option)
		{
		case pbd_wakeup_option::quick_roll_down: return 6;
		case pbd_wakeup_option::quick_roll_up: return 7;
		case pbd_wakeup_option::spring_forward: return 8;
		case pbd_wakeup_option::quick_roll_back: return 9;
		default: return {};
		}
	}

	struct pbd_wakeup_plan
	{
		pbd_wakeup_option option = pbd_wakeup_option::stand_up;
		std::optional<std::uint32_t> native_action;
		// Opponent-relative canonical words. A tech that reaches the floor uses Stand Up.
		std::array<std::uint16_t, 2> words{0x1000, 0};
		std::uint32_t count = 1;
	};

	constexpr pbd_wakeup_plan pbd_plan_wakeup(pbd_wakeup_option requested, pbd_ground_position position,
		bool can_tech, std::uint32_t random_word = 0) noexcept
	{
		pbd_wakeup_plan out;
		out.option = pbd_resolve_wakeup(requested, position, random_word, can_tech);
		out.native_action = pbd_wakeup_native_action(out.option);
		if (out.native_action)
		{
			if (!can_tech)
			{
				out.option = pbd_wakeup_option::stand_up;
				out.native_action.reset();
			}
			return out;
		}
		switch (out.option)
		{
		case pbd_wakeup_option::stay_down: out.words = {}; out.count = 0; break;
		case pbd_wakeup_option::back_roll: out.words[0] = 0x8000; break;
		case pbd_wakeup_option::forward_roll: out.words[0] = 0x2000; break;
		case pbd_wakeup_option::side_roll_up: out.words[0] = 0x0080; break;
		case pbd_wakeup_option::side_roll_down: out.words[0] = 0x4080; break;
		case pbd_wakeup_option::rising_low_kick: out.words[0] = 0x0040; break;
		case pbd_wakeup_option::rising_mid_kick: out.words[0] = 0x0020; break;
		case pbd_wakeup_option::toe_kick: out.words[0] = 0x4040; break;
		case pbd_wakeup_option::spring_kick: out.words = {0x8000, 0x0060}; out.count = 2; break;
		default: break;
		}
		return out;
	}

	// Floor input starts on the first actionable floor frame. Once started, the roll's follow-up is consecutive,
	// even if its opening input has left the lying pose. A new knockdown, Restart or rewind resets this sequence.
	struct pbd_floor_wakeup
	{
		pbd_wakeup_plan plan;
		std::uint32_t given = 0;
		std::uint32_t waited = 0;
		bool started = false, responded = false;

		std::optional<std::uint16_t> next(bool on_floor, bool actionable, bool response = true) noexcept
		{
			if (!started)
			{
				if (!on_floor || !actionable)
					return {};
				started = true;
			}
			if (plan.option == pbd_wakeup_option::stay_down)
				return on_floor ? std::optional<std::uint16_t>(std::uint16_t{}) : std::nullopt;
			if (given && response) responded = true;
			if (given >= plan.count || given >= plan.words.size())
			{
				// A character can lack a shared ground attack. If its opening never starts, use Stand Up.
				if (!responded && on_floor && actionable && ++waited >= 8) return std::uint16_t{0x1000};
				return {};
			}
			return plan.words[given++];
		}
		void reset(pbd_wakeup_plan next_plan = {}) noexcept
		{
			plan = next_plan;
			given = 0;
			waited = 0;
			started = responded = false;
		}
	};

	struct pbd_wakeup_observation
	{
		std::uint32_t allocation = 0, counter = 0;
		bool on_floor = false, actionable = false, standing = false;
		pbd_ground_position position = pbd_ground_position::unknown;
		bool knocked_down = false;
	};
	struct pbd_wakeup_controller
	{
		pbd_floor_wakeup floor;
		pbd_wakeup_option choice = pbd_wakeup_option::stand_up;
		pbd_wakeup_option requested = pbd_wakeup_option::stand_up;
		std::optional<std::uint32_t> counter;
		std::uint32_t origin = 0, random_word = 0;
		std::uint16_t word = 0;
		bool armed = false, engaged = false, active = false;
		void reset() noexcept { *this = {}; }
		void arm(pbd_wakeup_option wanted, std::uint32_t seed = 0) noexcept
		{
			reset();
			requested = wanted; random_word = seed;
			choice = pbd_resolve_wakeup(wanted, pbd_ground_position::face_up_feet_toward, seed);
			armed = true;
		}
		std::optional<std::uint32_t> native_action() const noexcept
		{
			return armed && engaged && !active ? pbd_wakeup_native_action(choice) : std::nullopt;
		}
		std::optional<std::uint16_t> sample(const pbd_wakeup_observation& now) noexcept
		{
			if (now.standing) { reset(); return {}; }
			if (!armed) return {};
			if (now.knocked_down || now.on_floor) engaged = true;
			if (!active && now.on_floor)
			{
				active = true; origin = now.allocation;
				const bool pick_again = requested == pbd_wakeup_option::random &&
					(!pbd_wakeup_available(choice, now.position) || pbd_wakeup_native_action(choice));
				floor.reset(pbd_plan_wakeup(pick_again ? pbd_wakeup_option::random : choice, now.position, false, random_word));
			}
			if (!active) return {};
			if (counter && now.counter == *counter) return word;
			if (counter && now.counter != *counter + 1) { reset(); return {}; }
			counter = now.counter;
			word = floor.next(now.on_floor, now.actionable, now.allocation != origin).value_or(0);
			return word;
		}
	};

	// Standing (0), Crouching (2) and landing techs (6-9) never guard.
	// Other Dummy Actions, including unknown values, retain the game's guard behavior.
	constexpr bool pbd_dummy_action_guards(std::uint32_t action) noexcept
	{
		return action != 0 && action != 2 && (action < 6 || action > 9);
	}

	// Actor +0xCC supplies guard bits to +0x152 on move entry (0xF1D50) and each frame (0x32B20).
	// Contact (0x1822C) tests 0x10 for high/mid attacks and 0x08 for low attacks.
	inline constexpr std::uint32_t pbd_guard_bits = 0x18;
	constexpr std::uint32_t pbd_without_guard(std::uint32_t flags) noexcept
	{
		return flags & ~pbd_guard_bits;
	}

	// Stock neutral entry clears guard only with flag 0x100 and automatic guard disabled (+0x1C5, 0xF1D80).
	// Stand-up moves lack 0x100; Controller wake-up also retains guard, so non-guarding actions need it cleared through get-up.
	struct pbd_getup_guard
	{
		std::uint64_t generation = 0;
		std::uint32_t counter = 0;
		bool rising = false, clear = false;

		// Clear guard during get-up and once at its ending boundary for the final stance.
		// The caller supplies a guarding action when the chosen action is unreadable.
		constexpr bool boundary(bool down, bool standing, std::uint32_t chosen_action, std::uint64_t now_generation,
			std::uint32_t now_counter) noexcept
		{
			if (now_generation != generation)
				rising = clear = false;
			if (down)
				rising = true;
			else if (standing)
				rising = false;
			const bool ending = clear, removes = !pbd_dummy_action_guards(chosen_action);
			clear = rising && removes;
			generation = now_generation;
			counter = now_counter;
			return clear || (ending && removes);
		}
		// Move-entry clearing expires after one game frame or a lifetime change.
		constexpr bool entered(std::uint64_t now_generation, std::uint32_t now_counter) const noexcept
		{
			return clear && now_generation == generation && now_counter - counter <= 1;
		}
	};
} // namespace pbd::practice
