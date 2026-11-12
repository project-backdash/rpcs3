#pragma once

#include "pbd_movement_transitions.h"

namespace pbd::practice::movement
{
	// The fighter record's +0xC0 word addresses one 0x4C-byte entry of the fighter's own move table (the moveset's +0x1E0 /
	// +0x1E4 give the table and its count). Returns that entry's index, or nothing when the word is not exactly an entry of
	// the table. Captures show it on an entry in every sample; that it follows the move being performed is unverified, which
	// is why the gates only trust it through what rest_learner observes.
	inline constexpr std::uint32_t move_entry_size = 0x4c, max_move_entries = 8192;
	inline std::optional<std::uint32_t> move_index(std::uint32_t table, std::uint32_t count, std::uint32_t entry) noexcept
	{
		if (!table || !count || count > max_move_entries || entry < table)
			return std::nullopt;
		const std::uint32_t offset = entry - table;
		if (offset % move_entry_size || offset / move_entry_size >= count)
			return std::nullopt;
		return offset / move_entry_size;
	}

	// Which move entries are the dummy's resting stance: an entry the fighter stays on for rest_learn_frames with a neutral
	// word is a stance, since a move, its recovery included, ends by itself. Standing dominates the time spent at rest, so an
	// entry counts only with at least a quarter of the most-held entry's frames: a knockdown the dummy lay in for a while
	// does not.
	inline constexpr std::uint32_t rest_learn_frames = 45;
	// Who drove a frame the learner observes. The port: the dummy's input was a word the host knows (recording, playback, or
	// the player's own Controller Dummy Action). The game: one of the game's own Dummy Actions drove it, and the entry it holds
	// the dummy still on is the stance it gets the dummy up into (rest_learner::held); that entry joins the resting stances
	// but is never strengthened, so a crouch the game holds cannot outweigh the standing our neutral rests the dummy in.
	// None: the dummy may be lying on the floor (it is being hit or has not been seen up since), so the frame counts toward
	// settled() and teaches nothing.
	// An entry the dummy leaves for the main stance on a neutral frame is a move ending (a throw, a break, its recovery), even
	// one held long enough to look still: it is marked and never counts as rest again. The first time costs one acceptance.
	enum class teacher : std::uint8_t
	{
		none,
		port,
		game
	};
	struct rest_learner
	{
		static constexpr std::size_t capacity = 4, move_capacity = 16;
		struct entry
		{
			std::uint32_t move = 0, frames = 0;
		};
		std::array<entry, capacity> entries{};
		std::size_t count = 0;
		std::optional<std::uint32_t> last;
		std::uint32_t still = 0;
		// The entry the dummy last read as resting on outside a hit (resting()), and the one the game's own Dummy Action last
		// held it still on (teacher::game).
		std::optional<std::uint32_t> stance, held;
		// Entries seen ending into the main stance (newest replaces the oldest when full).
		std::array<std::uint32_t, move_capacity> moves{};
		std::size_t move_count = 0, move_next = 0;
		// The moveset the entries belong to; a new character or costume starts the learning again.
		std::uint64_t identity = 0;

		void reset(std::uint64_t id) noexcept
		{
			*this = {};
			identity = id;
		}
		bool learned() const noexcept
		{
			return count != 0;
		}

		bool is_move(std::uint32_t move) const noexcept
		{
			return std::find(moves.begin(), moves.begin() + move_count, move) != moves.begin() + move_count;
		}
		// The most-held learned entry: standing.
		std::optional<std::uint32_t> main_stance() const noexcept
		{
			if (!count)
				return std::nullopt;
			return std::max_element(entries.begin(), entries.begin() + count, [](const entry& a, const entry& b)
				{
					return a.frames < b.frames;
				})
			    ->move;
		}
		void mark_move(std::uint32_t move) noexcept
		{
			if (!is_move(move))
			{
				moves[move_next] = move;
				move_next = (move_next + 1) % move_capacity;
				move_count = (std::min)(move_count + 1, move_capacity);
			}
			const auto end = std::remove_if(entries.begin(), entries.begin() + count, [&](const entry& each)
				{
					return each.move == move;
				});
			for (auto at = end; at != entries.begin() + count; ++at)
				*at = {};
			count = static_cast<std::size_t>(end - entries.begin());
			if (stance == move)
				stance.reset();
		}
		// No entry index reaches this value (max_move_entries), so it marks a freed slot.
		void unmark_move(std::uint32_t move) noexcept
		{
			std::replace(moves.begin(), moves.begin() + move_count, move, 0xffffffffu);
		}

		// One frame that really ran: the dummy's move entry after it (nothing: unreadable), whether its word was neutral (a frame
		// the game drove is), and who drove it. Returns true when an entry becomes a resting stance for the first time.
		bool observe(std::optional<std::uint32_t> move, bool neutral, teacher by = teacher::port) noexcept
		{
			if (!move || !neutral)
			{
				last = move;
				still = 0;
				return false;
			}
			if (last != move)
			{
				// Only our neutral shows a move ending: the game's Dummy Action moves the dummy between stances itself (standing
				// into its crouch, while that crouch may still tie standing as the main stance). The stance the game holds the
				// dummy on (a crouch under Crouching) is never a move.
				if (last && by == teacher::port && main_stance() == *move && held != *last)
					mark_move(*last);
				last = move;
				still = 0;
			}
			if (still < rest_learn_frames)
				++still;
			if (still < rest_learn_frames || by == teacher::none)
				return false;
			if (by == teacher::game)
			{
				held = move;
				unmark_move(*move);
			}
			if (is_move(*move))
				return false;
			for (std::size_t i = 0; i < count; ++i)
				if (entries[i].move == *move)
				{
					if (by == teacher::port && entries[i].frames != 0xffffffffu)
						++entries[i].frames;
					return false;
				}
			if (count < capacity)
				entries[count++] = {*move, 1};
			else
				*std::min_element(entries.begin(), entries.end(), [](const entry& a, const entry& b)
					{
						return a.frames < b.frames;
					}) = {*move, 1};
			return true;
		}

		bool at_rest(std::uint32_t move) const noexcept
		{
			std::uint64_t most = 0;
			for (std::size_t i = 0; i < count; ++i)
				most = (std::max)(most, std::uint64_t{entries[i].frames});
			for (std::size_t i = 0; i < count; ++i)
				if (entries[i].move == move && std::uint64_t{entries[i].frames} * 4 >= most)
					return true;
			return false;
		}

		// The dummy has stayed on `move` with a neutral word for rest_learn_frames up to now. The gates accept this as rest
		// too, so a stance not yet counted (a second idle, a stance a take ends in) costs rest_learn_frames, not the cap.
		bool settled(std::uint32_t move) const noexcept
		{
			return last == move && still >= rest_learn_frames && !is_move(move);
		}

		// The rest reading for the entry the dummy is on now. While it is hit, knocked down or getting up (`hurt`) a still
		// entry is not enough, as a dummy lying on the floor is still: only a learned resting stance, the stance it rested on
		// before, or the one the game's own Dummy Action holds it on (a crouch under Crouching, say) counts.
		bool resting(std::uint32_t move, bool hurt) noexcept
		{
			if (hurt)
				return at_rest(move) || stance == move || held == move;
			const bool now = at_rest(move) || settled(move);
			if (now)
				stance = move;
			return now;
		}
	};

	// Readable text for the capture files. Directions are Tekken notation for a fighter on `side` (f is toward the opponent,
	// n is neutral); buttons are the game's after the button map (1 to 4 are the attack buttons).
	inline std::string direction_text(std::uint16_t word, screen_side side)
	{
		const auto relative = oriented(word, side);
		std::string text;
		if ((relative & up) && (relative & down))
			text = "ud";
		else if (relative & up)
			text = "u";
		else if (relative & down)
			text = "d";
		if ((relative & forward) && (relative & back))
			text += "fb";
		else if (relative & forward)
			text += "f";
		else if (relative & back)
			text += "b";
		return text.empty() ? std::string("n") : text;
	}

	inline std::string button_text(std::uint16_t word)
	{
		struct name
		{
			std::uint16_t bit;
			const char* text;
		};
		static constexpr std::array<name, 12> names{{{0x0080, "1"}, {0x0010, "2"}, {0x0040, "3"}, {0x0020, "4"}, {0x0004, "L1"},
			{0x0008, "R1"}, {0x0001, "L2"}, {0x0002, "R2"}, {0x0100, "SELECT"}, {0x0200, "L3"}, {0x0400, "R3"}, {0x0800, "START"}}};
		std::string text;
		for (const auto& each : names)
		{
			if (!(word & each.bit))
				continue;
			if (!text.empty())
				text += "+";
			text += each.text;
		}
		return text;
	}

	inline std::string word_hex(std::uint16_t word)
	{
		char text[8]{};
		std::snprintf(text, sizeof(text), "0x%04x", static_cast<unsigned>(word));
		return text;
	}

	inline const char* side_text(std::optional<screen_side> side) noexcept
	{
		return !side ? "unknown" : *side == screen_side::left ? "left" :
		                                                        "right";
	}

	// One row of practice-inputs.jsonl: the word a port gave the game from game frame `frame` on. When the side is unknown
	// the directions are written for `assumed` (P1 on the left, the dummy on the right, as at round start).
	inline std::string input_row(std::uint64_t frame, std::uint64_t boundary, std::uint32_t port, std::uint16_t word,
		std::optional<screen_side> side, screen_side assumed)
	{
		return "{\"type\":\"input\",\"frame\":" + std::to_string(frame) + ",\"boundary\":" + std::to_string(boundary) +
		       ",\"port\":" + std::to_string(port) + ",\"word\":\"" + word_hex(word) + "\",\"dirs\":\"" +
		       direction_text(word, side.value_or(assumed)) + "\",\"buttons\":\"" + button_text(word) + "\",\"side\":\"" +
		       side_text(side) + "\"}\n";
	}

	// A take as run-length steps, the readable part of a saved tape: [{"frames":3,"word":"0x4000","dirs":"d","buttons":""},...]
	inline std::string steps_json(std::span<const std::uint16_t> take, screen_side side)
	{
		std::string text = "[";
		for (std::size_t at = 0; at < take.size();)
		{
			std::size_t end = at;
			while (end < take.size() && take[end] == take[at])
				++end;
			if (text.size() > 1)
				text += ",";
			text += "{\"frames\":" + std::to_string(end - at) + ",\"word\":\"" + word_hex(take[at]) + "\",\"dirs\":\"" +
			        direction_text(take[at], side) + "\",\"buttons\":\"" + button_text(take[at]) + "\"}";
			at = end;
		}
		return text + "]";
	}

	// Character-table rows (the game's table of character and costume variant; P2's row is the dummy's) of the Mishima-style
	// characters with a wavedash: Heihachi 0x22-0x23, Kazuya 0x24-0x25, Devil Jin 0x44-0x45.
	inline constexpr bool wavedash_character(std::uint32_t row) noexcept
	{
		return (row >= 0x22 && row <= 0x25) || row == 0x44 || row == 0x45;
	}
} // namespace pbd::practice::movement
