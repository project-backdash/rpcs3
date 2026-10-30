#pragma once

#include "pbd_movement_observation.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pbd::practice::precision
{
	using u32 = std::uint32_t;
	using u16 = std::uint16_t;
	inline constexpr u16 directions = 0xf000, attacks = 0x00f0, two = 0x0010, four = 0x0020;
	inline constexpr u32 capacity = 96, step_limit = 16, commit_limit = 8;
	struct input
	{
		u16 word = 0;
		u32 frames = 0;
	};
	struct attempt
	{
		std::array<input, 8> inputs{};
		u32 count = 0, first = 0, pressed = 0;
		u16 button = 0;
		bool shortcut = false;
	};
	struct performed
	{
		u32 allocation = 0, cursor = 0, startup = 0;
		std::string name;
	};
	struct result
	{
		attempt motion;
		performed move;
		u32 total = 0, optimal = 0;
	};

	// Allocation +0 names the move the game committed, independently of the input that tried to produce it.
	template <class Byte>
	std::string move_name(u32 address, Byte byte)
	{
		if (address < 0x10000000 || address > 0x7fffffc0)
			return {};
		std::string out;
		for (u32 index = 0; index < 64; ++index)
		{
			const u32 code = byte(address + index);
			if (!code)
				return out;
			if (code < 0x20 || code > 0x7e || code == '{' || code == '}' || code == '\t')
				return {};
			out += static_cast<char>(code);
		}
		return {};
	}

	inline std::string text(const result& value)
	{
		std::string out = value.move.name + " (i" + std::to_string(value.move.startup) + ")\t";
		for (u32 index = 0; index < value.motion.count; ++index)
		{
			const auto& part = value.motion.inputs[index];
			const u16 direction = part.word & directions;
			if (index)
				out += ", ";
			out += direction == movement::forward ? "f" : direction == movement::down                   ? "d" :
			                                          direction == (movement::down | movement::forward) ? "d/f" :
			                                                                                              "n";
			if (part.word & attacks)
				out += "+" + movement::button_text(part.word & attacks);
			out += " " + std::to_string(part.frames) + "f";
		}
		out += " | " + std::to_string(value.total) + "f / " + std::to_string(value.optimal) + "f optimal";
		return out;
	}

	class tracker
	{
		std::array<u16, capacity> history{};
		u32 clock = 0, size = 0;
		std::optional<attempt> pending;
		performed previous;
		u32 row = ~0u;

		std::optional<attempt> read_attempt(u16 button) const
		{
			attempt out;
			out.button = button;
			out.pressed = clock;
			const auto word = [&](u32 ago)
			{
				return history[(clock - ago) % capacity];
			};
			u32 ago = 0;
			// Direction runs locate the motion; full input runs below retain an early or late attack button.
			std::array<u16, 4> reversed{};
			u32 count = 0;
			while (ago < size && count < reversed.size())
			{
				const u16 direction = word(ago) & directions;
				u32 length = 0;
				while (ago < size && (word(ago) & directions) == direction)
				{
					++ago;
					if (++length > step_limit)
						return {};
				}
				reversed[count] = direction;
				++count;
				if (direction == movement::forward)
					break;
			}
			const bool early = count == 3 && reversed[0] == movement::down;
			if (count < 3 || (!early && reversed[0] != (movement::down | movement::forward)) || reversed[count - 1] != movement::forward ||
				reversed[count - 2] != 0 || (count == 4 && reversed[1] != movement::down))
				return {};
			out.shortcut = count == 3 && !early;
			out.first = clock + 1 - ago;
			for (u32 offset = ago; offset > 0; --offset)
			{
				const u16 value = word(offset - 1);
				if (out.count && out.inputs[out.count - 1].word == value)
					++out.inputs[out.count - 1].frames;
				else
				{
					if (out.count == out.inputs.size())
						return {};
					out.inputs[out.count++] = {value, 1};
				}
			}
			return out;
		}

	public:
		void reset()
		{
			*this = {};
		}

		// One input the game actually played, facing-relative, with its current committed attack. Pauses never call this.
		std::optional<result> frame(u16 relative, u32 character_row, const performed& move)
		{
			if (row != character_row)
			{
				reset();
				row = character_row;
			}
			const u16 word = relative & (directions | attacks);
			const u16 before = size ? history[(clock - 1) % capacity] : 0;
			history[clock % capacity] = word;
			size = (std::min)(size + 1, capacity);
			const u16 pressed = word & ~before & attacks;
			if (pressed == two || pressed == four)
				pending = read_attempt(pressed);
			if (pending && clock - pending->pressed > commit_limit)
				pending.reset();
			const bool committed = move.allocation && (move.allocation != previous.allocation || move.cursor < previous.cursor);
			std::optional<result> out;
			if (committed && pending)
			{
				const bool kazuya = character_row == 0x24 || character_row == 0x25;
				const bool mishima = kazuya || character_row == 0x22 || character_row == 0x23 || character_row == 0x44 || character_row == 0x45;
				if (mishima && (!pending->shortcut || (kazuya && pending->button == two)) && move.cursor && move.cursor <= commit_limit + 1 &&
					move.startup && move.startup <= 120 && !move.name.empty() && clock + 1 >= pending->first + move.cursor)
				{
					const u32 optimal = pending->button == two ? (pending->shortcut ? 13u : 14u) : move.startup + 3;
					out = result{*pending, move, clock + 1 - pending->first - move.cursor + move.startup, optimal};
				}
				pending.reset();
			}
			previous = move;
			++clock;
			return out;
		}
	};
} // namespace pbd::practice::precision
