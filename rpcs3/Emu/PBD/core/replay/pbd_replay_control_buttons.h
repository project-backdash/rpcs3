#pragma once

#include <cstdint>
#include <array>
#include <utility>

namespace pbd::replay_controls
{
	struct pbd_button_map
	{
		std::uint16_t select, start, slower, faster, step_forward, step_back, jump_forward, jump_back, take;
	};
	inline constexpr pbd_button_map pbd_buttons{0x0100, 0x0800, 0x0004, 0x0008, 0x2000, 0x8000, 0x0002, 0x0001, 0x0010};
	constexpr std::uint32_t pbd_taken_input_port(std::uint32_t side, std::uint32_t input_port) noexcept
	{
		return side >= 1 && side <= 2 ? (input_port >= 1 && input_port <= 2 ? input_port : side) : 0;
	}
	constexpr std::uint32_t pbd_take_side(const std::array<std::uint16_t, 2>& pads, std::uint32_t input_port,
		std::uint32_t preferred = 0) noexcept
	{
		if (preferred == 1 || preferred == 2)
			return preferred;
		return !input_port && (pads[1] & pbd_buttons.take) && !(pads[0] & pbd_buttons.take) ? 2 : 1;
	}
	constexpr std::uint32_t pbd_prompt_side(std::uint16_t held, std::uint16_t previous, std::uint32_t side) noexcept
	{
		if (held & pbd_buttons.select)
			return side;
		const auto pressed = static_cast<std::uint16_t>(held & ~previous);
		const auto direction = pressed & (pbd_buttons.step_back | pbd_buttons.step_forward);
		return direction == pbd_buttons.step_back ? 1 : direction == pbd_buttons.step_forward ? 2 :
		                                                                                        side;
	}
	constexpr std::uint16_t pbd_taken_word(std::uint16_t game,
		const std::array<std::uint16_t, 2>& pads, std::uint32_t side) noexcept
	{
		return side >= 1 && side <= 2 && !(game & pbd_buttons.select) ?
		           static_cast<std::uint16_t>(game & pads[side - 1] & ~(pbd_buttons.start | pbd_buttons.select)) :
		           0;
	}
	// Foreground keys share the controllers' canonical word. Esc belongs to Settings; Triangle uses the player's keyboard map.
	inline constexpr std::array<std::pair<int, std::uint16_t>, 8> pbd_keyboard{{{0x0d, pbd_buttons.start}, {0x20, pbd_buttons.start}, {0x27, pbd_buttons.step_forward},
		{0x25, pbd_buttons.step_back}, {0xdd, pbd_buttons.faster}, {0xdb, pbd_buttons.slower},
		{0xbe, pbd_buttons.jump_forward}, {0xbc, pbd_buttons.jump_back}}};
	enum class pbd_command
	{
		none,
		leave,
		pause,
		step,
		slower,
		faster,
		skip_scene,
		take,
		retry,
		return_to_replay
	};
	constexpr pbd_command pbd_attempt_command(std::uint16_t held, std::uint16_t previous) noexcept
	{
		constexpr std::uint16_t cross = 0x0040, circle = 0x0020;
		const auto pressed = static_cast<std::uint16_t>(held & ~previous);
		if (held & pbd_buttons.select)
		{
			if ((held & (cross | circle)) == (cross | circle))
				return pbd_command::none;
			if ((held & cross) && (pressed & (pbd_buttons.select | cross)))
				return pbd_command::retry;
			if ((held & circle) && (pressed & (pbd_buttons.select | circle)))
				return pbd_command::return_to_replay;
			return pbd_command::none;
		}
		// A bare Back tap leaves on release, so holding the modifier can form either restore chord.
		if ((previous & pbd_buttons.select) && !(previous & (cross | circle)))
			return pbd_command::leave;
		return pressed & pbd_buttons.start ? pbd_command::pause : pbd_command::none;
	}
	constexpr pbd_command pbd_taken_command(std::uint16_t held, std::uint16_t previous) noexcept
	{
		const auto pressed = static_cast<std::uint16_t>(held & ~previous);
		if (pressed & pbd_buttons.select)
			return pbd_command::leave;
		return pressed & pbd_buttons.start ? pbd_command::pause : pbd_command::none;
	}
	constexpr pbd_command pbd_watch_command(std::uint16_t held, std::uint16_t previous,
		bool paused, bool fighting, bool can_skip_scene, bool takeover) noexcept
	{
		const auto pressed = static_cast<std::uint16_t>(held & ~previous);
		if (pressed & pbd_buttons.select)
			return pbd_command::leave;
		if (pressed & pbd_buttons.start)
			return fighting ? pbd_command::pause : can_skip_scene ? pbd_command::skip_scene :
			                                                        pbd_command::none;
		if (paused && (held & pbd_buttons.step_forward))
			return pbd_command::step;
		if (pressed & pbd_buttons.slower)
			return pbd_command::slower;
		if (pressed & pbd_buttons.faster)
			return pbd_command::faster;
		if (takeover && (pressed & pbd_buttons.take))
			return pbd_command::take;
		return pbd_command::none; // Seeking uses the viewer's checkpoint controls.
	}
} // namespace pbd::replay_controls
