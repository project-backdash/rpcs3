#pragma once

#include "pbd_dummy_move.h"
#include "Emu/PBD/pbd_move_readiness.h"

namespace pbd::practice::dummy_move
{
	inline std::optional<std::vector<std::uint16_t>> command_words(const pbd::move_list::command& command)
	{
		std::vector<std::uint16_t> words;
		for (const auto& sample : command.samples)
		{
			const u32 duration = sample[1] >> 8, flags = sample[1] & 255;
			if (!duration || flags > 1 || sample[2] || (sample[0] & 0x0f00) || words.size() + duration > 1800)
				return {};
			words.insert(words.end(), duration, sample[0]);
		}
		const auto first = std::find_if(words.begin(), words.end(), [](auto word)
			{
				return word != 0;
			});
		if (first == words.end())
			return {};
		words.erase(words.begin(), first);
		return words;
	}
	// The ordinary controller path buffers the first attack during the stock terminal window. Directional setup runs
	// before it; the first attack is held until the game accepts a transition. Pauses consume no input words.
	struct input_player
	{
		std::vector<std::uint16_t> words;
		u32 position = 0, first_attack = 0, port = 1;
		std::optional<u32> earliest;
		bool started = false, accepted = false, sent = false, permitted = false, has_attack = false;
		void stop()
		{
			*this = {};
		}
		bool active() const
		{
			return position < words.size();
		}
		bool arm(std::vector<std::uint16_t> input_sequence, u32 target)
		{
			stop();
			if (target > 1 || input_sequence.empty() || input_sequence.size() > 1800 ||
				std::any_of(input_sequence.begin(), input_sequence.end(), [](auto word)
					{
						return (word & 0x0f00) != 0;
					}))
				return false;
			const auto attack = std::find_if(input_sequence.begin(), input_sequence.end(), [](auto word)
				{
					return (word & 0xf0) != 0;
				});
			has_attack = attack != input_sequence.end();
			accepted = !has_attack;
			first_attack = static_cast<u32>(attack - input_sequence.begin());
			port = target;
			words = std::move(input_sequence);
			return true;
		}
		void recovery(u32 counter, u32 cursor, u32 first)
		{
			if (!active() || started || cursor > first || first - cursor > 600)
				return;
			earliest = counter + first - cursor;
		}
		void boundary(u32 counter, bool advanced, bool allowed)
		{
			if (sent && advanced && active() && (position != first_attack || accepted))
				++position;
			sent = false;
			permitted = false;
			if (!active() || !allowed)
				return;
			if (!started)
			{
				if (!earliest)
					return;
				const auto lead = has_attack ? first_attack + 5 : 0;
				if (counter + 1 < *earliest && *earliest - (counter + 1) > lead)
					return;
				started = true;
			}
			permitted = true;
		}
		std::optional<std::uint16_t> input(u32 target)
		{
			if (!active() || !started || target != port)
				return {};
			if (!permitted)
				return std::uint16_t{0};
			sent = true;
			return words[position];
		}
	};
} // namespace pbd::practice::dummy_move
