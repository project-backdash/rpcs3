#pragma once

#include "pbd_practice_wakeup.h"

namespace pbd::practice
{
	// Only consume a floor option when the current move admits input. During an end-command window the resolver also
	// checks the target's cursor-1 inputs (0xF07A4); earlier landing frames cannot consume a kick or roll.
	template <class Word, class Half>
	bool pbd_floor_input_open(std::uint32_t bank, std::uint32_t allocation, std::uint32_t cursor, Word word, Half half, bool chain = true, std::uint32_t response = 0, std::uint32_t* entering = nullptr)
	{
		const auto member = [&](std::uint32_t offset, std::uint32_t stride, std::uint32_t address)
		{
			const auto table = word(bank + offset), count = word(bank + offset + 4);
			return table >= 0x10000000 && count && count <= 65536 && address >= table &&
			       std::uint64_t{table} + std::uint64_t{count} * stride <= 0x80000000 &&
			       std::uint64_t{address} < std::uint64_t{table} + std::uint64_t{count} * stride && (address - table) % stride == 0;
		};
		std::uint32_t command = word(allocation + 0x14), resume = 0;
		for (unsigned index = 0; index < 1024; ++index)
		{
			if (!member(0x1b0, 24, command) && !member(0x1b8, 24, command))
				return false;
			const auto input = word(command), head = input >> 16;
			if (input == 0x80000000)
			{
				if (!chain || resume || (cursor && (!half(command + 16) || half(command + 16) > cursor || half(command + 18) < cursor)))
					return false;
				const auto target = half(command + 8);
				const auto move = target >= 0x8000 && target < 0x8030 ? word(bank + 0xac + (target - 0x8000) * 4) : target;
				const auto table = word(bank + 0x1e0), count = word(bank + 0x1e4);
				if (!count || count > 8192 || move >= count || table < 0x10000000 || std::uint64_t{table} + std::uint64_t{count} * 0x4c > 0x80000000)
					return false;
				const bool open = pbd_floor_input_open(bank, table + move * 0x4c, cursor ? 1 : 0, word, half, false, response);
				if (open && entering && ((target >= 0x8004 && target <= 0x800e) || target == 0x8017))
					*entering = target - 0x8000;
				return open;
			}
			if (head == 0x8005 && !resume)
			{
				resume = command + 24;
				command = word(bank + 0x1b8) + half(command + 8) * 24;
				continue;
			}
			if (head == 0x8006)
			{
				if (!resume)
					return false;
				command = resume;
				resume = 0;
				continue;
			}
			if (input && head < 0x8000 && (half(command + 22) & 0x42) == 0x40 &&
				(!cursor || (half(command + 16) <= cursor && half(command + 18) >= cursor)))
			{
				if (!response)
					return true;
				const auto target = half(command + 8);
				const auto move = target >= 0x8000 && target < 0x8030 ? word(bank + 0xac + (target - 0x8000) * 4) : target;
				if (move < word(bank + 0x1e4) && word(bank + 0x1e0) + move * 0x4c == response)
					return true;
			}
			command += 24;
		}
		return false;
	}

	// The stock dummy input handler (0xF9530) asks for get-up with live flag 4, independently of the move alias.
	// Aliases identify the available ground attacks; they do not decide whether the fighter needs floor input.
	template <class Word, class Half>
	std::optional<pbd_wakeup_observation> pbd_read_wakeup(unsigned side, Word word, Half half)
	{
		constexpr std::array<std::uint32_t, 2> actors{0x10090040, 0x10090910};
		if (side >= actors.size())
			return {};
		const auto actor = actors[side], opponent = actors[1 - side];
		if (word(actor + 0x5c) != opponent || word(opponent + 0x5c) != actor)
			return {};
		const auto allocation = word(actor + 0xc4), bank = word(actor + 0x50);
		const auto member = [&](std::uint32_t owner)
		{
			const auto loaded = word(owner + 0x50);
			if (loaded < 0x10000000 || loaded > 0x7ffffd00)
				return false;
			const auto table = word(loaded + 0x1e0), count = word(loaded + 0x1e4);
			return table >= 0x10000000 && count && count <= 8192 &&
			       std::uint64_t{table} + std::uint64_t{count} * 0x4c <= 0x80000000 && allocation >= table &&
			       std::uint64_t{allocation} < std::uint64_t{table} + std::uint64_t{count} * 0x4c && (allocation - table) % 0x4c == 0;
		};
		if (bank < 0x10000000 || bank > 0x7ffffd00 || (!member(actor) && !member(opponent)))
			return {};
		pbd_wakeup_observation out;
		out.allocation = allocation;
		out.counter = word(0x1008f05c);
		const auto flags = word(actor + 0xcc);
		if (out.counter == ~0u || flags == ~0u)
			return {};
		const auto table = word(bank + 0x1e0), count = word(bank + 0x1e4);
		const auto alias = [&](std::uint32_t id)
		{
			const auto index = word(bank + 0xac + id * 4);
			return index < count && allocation == table + index * 0x4c;
		};
		out.knocked_down = (flags & 0x6000) == 0x6000;
		out.standing = !(flags & 4) && word(actor + 0x48) == 0 && word(actor + 0x320) == 0 && (alias(1) || alias(2));
		out.on_floor = (flags & 4) != 0;
		if (out.on_floor)
		{
			const auto cursor = half(actor + 0x96), direction = half(actor + 0x146);
			const auto length = half(allocation + 0x24);
			const auto owner = member(actor) ? bank : word(opponent + 0x50);
			std::uint32_t entering = 0;
			out.actionable = direction > 0 && direction <= 0x7fff && cursor > 0 && length <= 0x7fff && cursor <= length &&
			                 pbd_floor_input_open(owner, allocation, cursor, word, half, true, 0, &entering);
			bool resting = false;
			for (const std::uint32_t id : {4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 23u})
				if (alias(id) || entering == id)
				{
					resting = alias(id);
					out.position = (id == 4 || id == 6 || id == 7) ? pbd_ground_position::face_up_feet_toward :
					               id < 8                          ? pbd_ground_position::face_up_feet_away :
					                                                 pbd_ground_position::face_down_feet_toward;
					break;
				}
			// Side rolls and some face-down get-ups keep flag 4. A target of the floor's input commands is a response;
			// an automatic transition between its resting aliases is not.
			if (!resting)
				for (const std::uint32_t id : {6u, 7u, 9u, 10u, 14u, 23u})
				{
					const auto move = word(owner + 0xac + id * 4), begin = word(owner + 0x1e0);
					if (move < word(owner + 0x1e4) && pbd_floor_input_open(owner, begin + move * 0x4c, 0, word, half, true, allocation))
					{
						out.recovering = true;
						break;
					}
				}
		}
		return out;
	}
} // namespace pbd::practice
