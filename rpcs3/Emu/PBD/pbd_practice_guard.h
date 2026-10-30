#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace pbd::practice
{
	// Guard variants alternate Guard All (4) and Standing (0) at practice object +0x6C.
	// Input (0xED5D0) and automatic guard (0x2DC08) read this action each frame.
	enum class pbd_guard_option : std::uint8_t
	{
		game,
		after_first_hit,
		random
	};
	inline constexpr std::uint32_t pbd_guard_option_count = 3;
	inline constexpr std::uint32_t pbd_standing_action = 0, pbd_guard_all_action = 4;
	// After First Hit guards follow-ups until half a second of neutral since the last contact.
	inline constexpr std::uint32_t pbd_guard_reset_frames = 30;
	// An unreadable neutral state releases guard after three seconds without contact.
	inline constexpr std::uint32_t pbd_guard_unread_frames = 180;

	// Menu order preserves the game's actions and groups the two variants with Guard All.
	struct pbd_dummy_action_choice
	{
		std::string_view name;
		std::uint32_t action = 0;
		pbd_guard_option guard = pbd_guard_option::game;
		std::string_view help;
	};
	inline constexpr std::array<pbd_dummy_action_choice, 9> pbd_dummy_action_choices{{{"Standing", 0},
		{"Standing Guard", 1},
		{"Crouching", 2},
		{"Crouching Guard", 3},
		{"Guard All", 4},
		{"Guard After First Hit", 4, pbd_guard_option::after_first_hit,
			"Guards nothing until you land a hit, then guards everything until the dummy has been in neutral for half a second."},
		{"Random Guard", 4, pbd_guard_option::random, "Guards everything or nothing, chosen again after each hit or block."},
		{"Neutral Guard", 5},
		{"Movement", 10}}};

	// Guard variants apply only to Guard All; landing techs 6-9 belong to Wake-up.
	constexpr std::optional<std::uint32_t> pbd_dummy_action_option(std::uint32_t action, pbd_guard_option guard) noexcept
	{
		const auto wanted = action == pbd_guard_all_action ? guard : pbd_guard_option::game;
		for (std::uint32_t at = 0; at < pbd_dummy_action_choices.size(); ++at)
			if (pbd_dummy_action_choices[at].action == action && pbd_dummy_action_choices[at].guard == wanted)
				return at;
		return std::nullopt;
	}

	// Contact and post-frame neutral state from one executed frame; neutral is absent when unreadable.
	struct pbd_guard_frame
	{
		bool hit = false, blocked = false;
		std::optional<bool> neutral;
	};

	// Random draws once on entry and after each contact from caller-owned host state, never the game's RNG.
	struct pbd_guard_state
	{
		pbd_guard_option option = pbd_guard_option::game;
		bool guarding = true, drawn = false;
		std::uint32_t neutral_frames = 0, quiet_frames = 0;

		void choose(pbd_guard_option next) noexcept
		{
			option = next;
			restart();
		}
		// Restart and rewind begin a new exchange, including a fresh Random draw.
		void restart() noexcept
		{
			guarding = option != pbd_guard_option::after_first_hit;
			drawn = false;
			neutral_frames = quiet_frames = 0;
		}
		constexpr bool lifted() const noexcept
		{
			return option == pbd_guard_option::after_first_hit ? !guarding : option == pbd_guard_option::random && drawn && !guarding;
		}
		template <class Draw>
		void frame(const pbd_guard_frame& now, Draw&& draw)
		{
			if (option == pbd_guard_option::random)
			{
				if (!drawn || now.hit || now.blocked)
				{
					guarding = (draw() >> 31) != 0;
					drawn = true;
				}
				return;
			}
			if (option != pbd_guard_option::after_first_hit)
				return;
			if (now.hit || (guarding && now.blocked))
			{
				guarding = true;
				neutral_frames = quiet_frames = 0;
				return;
			}
			if (!guarding)
				return;
			neutral_frames = now.neutral.value_or(false) ? neutral_frames + 1 : 0;
			++quiet_frames;
			if (neutral_frames >= pbd_guard_reset_frames || (!now.neutral && quiet_frames >= pbd_guard_unread_frames))
			{
				guarding = false;
				neutral_frames = quiet_frames = 0;
			}
		}
	};

	constexpr std::uint32_t pbd_guard_action(std::uint32_t chosen, const pbd_guard_state& state) noexcept
	{
		return chosen == pbd_guard_all_action && state.lifted() ? pbd_standing_action : chosen;
	}

	// Random supplies Off (0) or On (1) for practice object +0x30 on entry and after each contact.
	// The caller supplies host random words; value is absent before the first draw or when Random is disabled.
	struct pbd_counter_hit_draw
	{
		bool random = false;
		std::optional<bool> value;

		void choose(bool on) noexcept
		{
			random = on;
			value.reset();
		}
		void restart() noexcept
		{
			value.reset();
		}
		template <class Draw>
		void frame(const pbd_guard_frame& now, Draw&& draw)
		{
			if (random && (!value || now.hit || now.blocked))
				value = (draw() >> 31) != 0;
		}
	};
} // namespace pbd::practice
