#pragma once

#include "pbd_practice_frames.h"
#include "pbd_practice_reference.h"
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace pbd::move_list
{
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	inline bool range(u32 address, u32 count, u32 stride, u32 limit)
	{
		return address >= 0x10000000 && count && count <= limit && u64{address} + u64{count} * stride <= 0x80000000;
	}
	// Whether `address` is a record of the bank table at `offset` (pointer, then count) with records of `stride` bytes.
	template <class Word>
	bool table_member(u32 bank, u32 offset, u32 stride, u32 address, u32 limit, Word word)
	{
		const u32 table = word(bank + offset), count = word(bank + offset + 4);
		return table >= 0x10000000 && count && count <= limit && u64{table} + u64{count} * stride <= 0x80000000 &&
		       address >= table && u64{address} < u64{table} + u64{count} * stride && (address - table) % stride == 0;
	}

	struct entry
	{
		u32 id = 0;
		std::u16string name, notation;
		u32 command_id = 0, variants = 0;
		u32 conditions = 0, execution = 0;
	};
	// A titled descriptor without commands: a category of the game's own list, shown before entries[position].
	struct header
	{
		u32 position = 0;
		std::u16string name;
		bool operator==(const header&) const = default;
	};
	struct catalog
	{
		u32 character = 0, bank = 0;
		std::vector<entry> entries;
		std::vector<header> headers;
	};
	struct command
	{
		u32 id = 0, record = 0;
		std::array<u32, 3> header{};
		std::uint16_t kind = 0;
		// The stock six-byte demonstration records, in guest halfword order. The executor interprets their flags.
		std::vector<std::array<std::uint16_t, 3>> samples;
	};
	template <class Half>
	std::optional<std::u16string> text(u32 address, Half half)
	{
		if (!range(address, 256, 2, 256) || (address & 1))
			return {};
		std::u16string out;
		bool trailing = false;
		for (u32 index = 0; index < 256; ++index)
		{
			const u32 value = half(address + index * 2);
			if (value > 0xffff)
				return {};
			if (!value)
				return trailing ? std::nullopt : std::optional<std::u16string>(out);
			if (trailing ? value < 0xdc00 || value > 0xdfff : value >= 0xdc00 && value <= 0xdfff)
				return {};
			trailing = value >= 0xd800 && value <= 0xdbff;
			out.push_back(static_cast<char16_t>(value));
		}
		return {};
	}
	template <class Word, class Half>
	std::optional<command> read_command(u32 bank, u32 id, Word word, Half half)
	{
		if (bank < 0x10000000 || bank > 0x7ffffd00)
			return {};
		const u32 mapping = word(bank + 0x200), count = word(bank + 0x204);
		const u32 records = word(bank + 0x208), total = word(bank + 0x20c);
		const u32 sample_table = word(bank + 0x210), sample_count = word(bank + 0x214);
		if (!range(mapping, count, 2, 65536) || !range(records, total, 20, 8192) || id >= count)
			return {};
		const u32 slot = half(mapping + id * 2);
		if (slot >= total)
			return {};
		const u32 record = records + slot * 20, samples = half(record + 14), data = word(record + 16);
		if (!range(sample_table, sample_count, 6, 262144) || !range(data, samples, 6, 4096) || (data & 1) ||
			data < sample_table || (data - sample_table) % 6 || u64{data} + u64{samples} * 6 > u64{sample_table} + u64{sample_count} * 6)
			return {};
		command out;
		out.id = id;
		out.record = slot;
		for (u32 at = 0; at < 3; ++at)
			out.header[at] = word(record + at * 4);
		const u32 kind = half(record + 12);
		if (kind > 0xffff)
			return {};
		out.kind = static_cast<std::uint16_t>(kind);
		out.samples.reserve(samples);
		for (u32 at = 0; at < samples; ++at)
		{
			std::array<std::uint16_t, 3> sample{};
			for (u32 field = 0; field < 3; ++field)
			{
				const u32 value = half(data + at * 6 + field * 2);
				if (value > 0xffff)
					return {};
				sample[field] = static_cast<std::uint16_t>(value);
			}
			out.samples.push_back(sample);
		}
		if (mapping != word(bank + 0x200) || count != word(bank + 0x204) || records != word(bank + 0x208) ||
			total != word(bank + 0x20c) || sample_table != word(bank + 0x210) || sample_count != word(bank + 0x214) ||
			slot != half(mapping + id * 2) || data != word(record + 16) || samples != half(record + 14))
			return {};
		return out;
	}
	template <class Word, class Half>
	std::optional<catalog> read_catalog(u32 bank, u32 character, Word word, Half half)
	{
		if (bank < 0x10000000 || bank > 0x7ffffd00 || character >= 64)
			return {};
		const u32 table = word(bank + 0x218), count = word(bank + 0x21c), commands = word(bank + 0x204);
		if (!range(table, count, 60, 4096) || !commands || commands > 65536)
			return {};
		catalog out;
		out.character = character;
		out.bank = bank;
		for (u32 id = 0; id < count; ++id)
		{
			const u32 record = table + id * 60, command_id = half(record + 44), variants = half(record + 46);
			if (command_id == 0xffff)
			{
				// A row without commands titles the moves after it, when it has a readable title; one without is skipped.
				if (const auto title = text(word(record + 4), half); title && !title->empty())
					out.headers.push_back({static_cast<u32>(out.entries.size()), *title});
				continue;
			}
			if (command_id >= commands || variants > 255 || u64{command_id} + variants >= commands)
				return {};
			for (u32 variant = 0; variant <= variants; ++variant)
				if (!read_command(bank, command_id + variant, word, half))
					return {};
			const auto name = text(word(record + 4), half), notation = text(word(record + 16), half);
			if (!name || !notation)
				return {};
			const u32 conditions = half(record + 54), execution = half(record + 56);
			if (conditions > 0xffff || execution > 0xffff)
				return {};
			if (!name->empty())
				out.entries.push_back({id, *name, *notation, command_id, variants, conditions, execution});
		}
		if (table != word(bank + 0x218) || count != word(bank + 0x21c) || commands != word(bank + 0x204))
			return {};
		return out;
	}
	namespace detail
	{
		template <class Word, class Half>
		inline bool timing_properties(u32 bank, u32 at, Word& word, Half& half)
		{
			if (!at)
				return true;
			u32 previous = 0;
			for (unsigned index = 0; index < 4; ++index, at += 8)
			{
				if (!table_member(bank, 0x1c8, 8, at, 65536, word))
					return false;
				const u32 frame = half(at), id = half(at + 2), value = word(at + 4);
				if (!frame)
					return !id && !value;
				if (frame <= previous)
					return false;
				previous = frame;
				// These standing reaction records have matching held-input first-actionable evidence.
				if (!((id == 0x8066 && (frame == 10 || frame == 13) && value == 0x00010000) ||
						(id == 0x814a && frame == 1 && value == 0x04020000) ||
						(id == 0x814d && frame == 3 && value == 0x01100000)))
					return false;
			}
			return false;
		}

		// The command glyphs in order, as text or as the overlay's glyph tokens (notation_text, notation_glyphs).
		inline void append_notation_text(std::string& out, char16_t code)
		{
			if (code < 0x80)
				out += static_cast<char>(code);
			else if (code < 0x800)
			{
				out += static_cast<char>(0xc0 | (code >> 6));
				out += static_cast<char>(0x80 | (code & 0x3f));
			}
			else if (code < 0xd800 || code > 0xdfff)
			{
				out += static_cast<char>(0xe0 | (code >> 12));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
				out += static_cast<char>(0x80 | (code & 0x3f));
			}
		}

		inline std::string notation(std::u16string_view glyphs, bool tokens)
		{
			static constexpr std::array<const char*, 8> directions{"d/b", "d", "d/f", "b", "f", "u/b", "u", "u/f"};
			static constexpr std::array<const char*, 8> held{"D/B", "D", "D/F", "B", "F", "U/B", "U", "U/F"};
			// The overlay's arrow sprites for the same order, as a fighter facing right reads them (f is right).
			static constexpr std::array<const char*, 8> arrows{"arrow_dl", "arrow_d", "arrow_dr", "arrow_l", "arrow_r", "arrow_ul", "arrow_u", "arrow_ur"};
			std::string out;
			enum class last
			{
				other,
				direction,
				button
			} previous = last::other;
			for (const char16_t code : glyphs)
			{
				if (code >= 0xe000 && code <= 0xe00f)
				{
					if (previous != last::other)
						out += ' ';
					const bool hold = code >= 0xe008;
					const u32 index = (code - 0xe000u) & 7u;
					if (tokens)
						out += std::string(hold ? "{held:" : "{") + arrows[index] + "}";
					else
						out += hold ? held[index] : directions[index];
					previous = last::direction;
				}
				else if (code >= 0xe015 && code <= 0xe023)
				{
					if (previous == last::direction)
						out += '+';
					else if (previous == last::button)
						out += ' ';
					const u32 mask = code - 0xe014u;
					if (tokens)
						// The four-button cluster with the pressed ones lit, as the command history draws buttons.
						out += "{cluster_" + std::to_string(mask) + "}";
					else
					{
						bool first = true;
						for (u32 button = 1; button <= 4; ++button)
							if (mask & (1u << (button - 1)))
							{
								if (!first)
									out += '+';
								out += static_cast<char>('0' + button);
								first = false;
							}
					}
					previous = last::button;
				}
				else if (code >= 0xe000 && code <= 0xf8ff)
					previous = last::other;
				else
				{
					// The label's own text (commas, "+", letters): UTF-8, surrogates left out. A brace would read as a
					// token, so it is left out of the tokens.
					if (tokens && (code == '{' || code == '}'))
						continue;
					append_notation_text(out, code);
					previous = last::other;
				}
			}
			// One space between words, none at the ends or before a comma.
			std::string tidy;
			for (const char c : out)
			{
				if (c == ' ' && (tidy.empty() || tidy.back() == ' '))
					continue;
				if (c == ',' && !tidy.empty() && tidy.back() == ' ')
					tidy.pop_back();
				tidy += c;
			}
			while (!tidy.empty() && tidy.back() == ' ')
				tidy.pop_back();
			return tidy;
		}
	} // namespace detail
	// The game's command glyphs as Tekken notation: the private-use arrows (U+E000..E007 tapped, U+E008..E00F held, in the
	// order d/b, d, d/f, b, f, u/b, u, u/f; held ones in capitals) and buttons (U+E015..E023, a mask of 1 to 4). A direction
	// glyph straight before a button glyph is one input ("d/f+1"); other glyphs that touch are separated by a space. The
	// label's own characters are kept; another private-use glyph has no text and is left out. Returns UTF-8.
	inline std::string notation_text(std::u16string_view glyphs)
	{
		return detail::notation(glyphs, false);
	}
	// The same command as the overlay draws it, in glyph tokens: each arrow its arrow sprite ("{arrow_dr}", a held one
	// "{held:arrow_dr}"), each button glyph the four-button cluster with its buttons lit ("{cluster_1}"), joined the same
	// way: "{arrow_dr}+{cluster_1}" for d/f+1.
	inline std::string notation_glyphs(std::u16string_view glyphs)
	{
		return detail::notation(glyphs, true);
	}

	// The move a fighter can act from again: standing (alias 0x8001) or crouching (alias 0x8002), or another alias of the
	// same bank that resolves to one of their records. Floor, wall, airborne and throw states are not.
	template <class Word>
	bool neutral_target(u32 bank, u32 target, Word word)
	{
		if (target == 0x8001 || target == 0x8002)
			return true;
		if (target < 0x8000 || target >= 0x8030)
			return false;
		// The game resolves alias N through the word at bank + 0xAC + 4N (0xD469C).
		const u32 index = word(bank + 0xac + (target - 0x8000) * 4);
		return index == word(bank + 0xac + 4) || index == word(bank + 0xac + 8);
	}

	// The animation cursor of `allocation` at which its fighter can act again when nothing else interrupts it, counted from
	// cursor 1 and following the game's input-free transitions (0xEF790 picks the command, 0xF0764 applies its window):
	// the end command (input 0x80000000) fires when the cursor reaches its window start, or at the animation's last frame
	// when that comes first or the window is empty; an earlier automatic command (no input, only always-true requirement
	// 0 before the 327 terminator, option 0x41, plain extra data 0x80) replaces it. A transition into another move of
	// the same bank continues with that move: the queued move starts at cursor 1 on the next frame, so the chain adds each
	// move's own count. Conditional and input commands are left out, so a reaction or recovery that only ends on a held
	// input or a game condition reads as its plain end. A floor, wall or other non-neutral end, a chain longer than four
	// moves or a malformed table gives no value.
	template <class Word, class Half>
	std::optional<u32> actionable(u32 bank, u32 allocation, Word word, Half half)
	{
		if (bank < 0x10000000 || bank > 0x7ffffd00)
			return {};
		const auto unconditional = [&](u32 list)
		{
			for (unsigned index = 0; index < 8; ++index, list += 4)
			{
				if (!table_member(bank, 0x190, 4, list, 65536, word))
					return false;
				const u32 id = half(list);
				if (id == 327)
					return true;
				if (id != 0)
					return false;
			}
			return false;
		};
		u32 total = 0;
		for (unsigned depth = 0; depth < 4; ++depth)
		{
			if (!table_member(bank, 0x1e0, 0x4c, allocation, 8192, word))
				return {};
			const u32 length = half(allocation + 0x24);
			if (!length || length > 32767)
				return {};
			std::optional<u32> end;
			u32 target = 0, automatic_first = 0, automatic_target = 0;
			u32 command = word(allocation + 0x14);
			for (unsigned index = 0; index < 512; ++index, command += 24)
			{
				if (!table_member(bank, 0x1b0, 24, command, 65536, word) && !table_member(bank, 0x1b8, 24, command, 65536, word))
					return {};
				const u32 input = word(command);
				if (input == 0x80000000)
				{
					const u32 first = half(command + 16);
					end = first ? (std::min)(first, length) : length;
					target = half(command + 8);
					break;
				}
				if (input || half(command + 22) != 0x41)
					continue;
				const u32 first = half(command + 16), last = half(command + 18), data = word(command + 12);
				if (!first || first > last || (automatic_first && automatic_first <= first))
					continue;
				if (!table_member(bank, 0x1c0, 4, data, 4096, word) || word(data) != 0x80 || !unconditional(word(command + 4)))
					continue;
				automatic_first = first;
				automatic_target = half(command + 8);
			}
			if (!end)
				return {};
			if (automatic_first && automatic_first < *end)
			{
				end = automatic_first;
				target = automatic_target;
			}
			total += *end;
			if (total > 32767)
				return {};
			if (neutral_target(bank, target, word))
				return total;
			if (target >= 0x8000 || target >= word(bank + 0x1e4))
				return {};
			allocation = word(bank + 0x1e0) + target * 0x4c;
		}
		return {};
	}

	// Reads the loaded move bank only. Conditional reactions, aliases, special terminals and unqualified property
	// records stay unavailable; no guest predicate is invoked and no guest state or random number is changed.
	template <class Word, class Half>
	pbd::practice::reference::result read_timing(std::uint32_t bank, std::uint32_t allocation, Word word, Half half)
	{
		pbd::practice::reference::result empty;
		if (bank < 0x10000000 || bank > 0x7ffffd00)
			return empty;
		if (!table_member(bank, 0x1e0, 0x4c, allocation, 8192, word))
			return empty;
		const auto endpoint = [&](u32 record, bool reaction) -> pbd::practice::reference::terminal
		{
			if (!table_member(bank, 0x1e0, 0x4c, record, 8192, word) || (reaction && word(record + 0x10) != 0))
				return {};
			const u32 extra = word(record + 0x30);
			if (reaction ? !detail::timing_properties(bank, extra, word, half) : extra != 0)
				return {};
			u32 command = word(record + 0x14);
			for (unsigned index = 0; index < 256; ++index, command += 24)
			{
				if (!table_member(bank, 0x1b0, 24, command, 65536, word) && !table_member(bank, 0x1b8, 24, command, 65536, word))
					return {};
				const u32 input = word(command);
				if (half(command) >= 0x8000 && input != 0x80000000)
					return {};
				if (!reaction && input == 0)
					return {};
				const u32 data = word(command + 12);
				if (input == 0x80000000)
				{
					if (!table_member(bank, 0x1c0, 4, data, 4096, word) || word(data) != 0x80 || half(command + 22) != 0x50)
						return {};
					const u32 first = half(command + 16), last = half(command + 18), length = half(record + 0x24);
					if (!first || first > last || first > length || length > 32767)
						return {};
					return {first, last, half(command + 8), true};
				}
				// Ordinary standing reactions can contain early input tests. Other script forms need their own timing proof.
				if (reaction && (input != 0 || !table_member(bank, 0x1c0, 4, data, 4096, word) ||
									(word(data) != 0x80 && word(data) != 0x2400) ||
									(half(command + 22) != 1 && half(command + 22) != 0x41)))
					return {};
			}
			return {};
		};
		const u32 first = half(allocation + 0x44), last = half(allocation + 0x46), length = half(allocation + 0x24);
		const pbd::practice::reference::move attack{first, last, length,
			pbd::practice::frames::attack_window(first, last, length), word(allocation + 0x30) == 0, endpoint(allocation, false)};
		pbd::practice::reference::terminal block, hit;
		const u32 condition = word(allocation + 0x20);
		if (table_member(bank, 0x198, 12, condition, 65536, word))
		{
			const u32 requirement = word(condition), reaction = word(condition + 8);
			if (table_member(bank, 0x190, 4, requirement, 65536, word) && half(requirement) == 327 && half(requirement + 2) == 0 &&
				table_member(bank, 0x188, 0x50, reaction, 8192, word))
			{
				const u32 table = word(bank + 0x1e0), count = word(bank + 0x1e4);
				const u32 block_id = half(reaction + 0x48), hit_id = half(reaction + 0x34);
				if (block_id < count)
					block = endpoint(table + block_id * 0x4c, true);
				if (hit_id < count)
					hit = endpoint(table + hit_id * 0x4c, true);
			}
		}
		auto out = pbd::practice::reference::calculate(attack, block, hit);
		out.allocation = allocation;
		return out;
	}

	struct queued_reading
	{
		u32 cursor = 1;
		pbd::practice::frames::readiness_reading readiness;
	};
} // namespace pbd::move_list
