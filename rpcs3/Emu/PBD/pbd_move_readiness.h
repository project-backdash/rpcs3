#pragma once

#include "pbd_move_catalog.h"

namespace pbd::move_list
{
	template <class Word, class Half>
	class readiness_walk
	{
	public:
		using reading = pbd::practice::frames::readiness_reading;
		using state = pbd::practice::frames::ready_state;
		using contact = pbd::practice::frames::contact;

		readiness_walk(u32 moves, contact kind, bool reacts, Word read_word, Half read_half)
			: bank(moves), outcome(kind), reaction(reacts), word(read_word), half(read_half)
		{
		}

		std::optional<reading> committed(u32 allocation, u32 cursor) const
		{
			if (!is_part(allocation))
				return {};
			reading out;
			const auto found = run(allocation, (std::max)(cursor, 1u), 0, true, &out);
			if (!found)
				return {};
			out.free = found->free;
			out.state = found->at;
			if (out.own && *out.own >= out.free)
				out.own.reset();
			return out;
		}
		// The accepted queue's target, using the entry parameter prepared by the game and the cursor it will execute on.
		std::optional<queued_reading> queued(u32 allocation, u32 execute, u32 parameter) const
		{
			if (!is_part(allocation) || !execute || execute > 32767 || parameter > 65535)
				return {};
			const auto entered = prepared_cursor(allocation, parameter, execute, execute, via::command);
			const auto next = committed(allocation, entered.cursor);
			return next ? std::optional<queued_reading>{{entered.cursor, *next}} : std::nullopt;
		}

	private:
		static constexpr u32 end_input = 0x80000000;
		static constexpr std::size_t max_parts = 8, max_entries = 512;
		// How a transition was made: by a picked command, by the end command on the last cursor, or by a command of the end
		// target run inside the end command's window. The last never keeps the cursor (0xF2C4C).
		enum class via : std::uint8_t
		{
			command,
			natural,
			chain
		};
		// A command the resolver (0xEF790) can pick: option 0x40 set and 0x02 clear (others only run their side effects).
		// plain, free and escape say what its requirements allow (see requirements).
		struct entry
		{
			u32 input = 0, data = 0;
			std::uint16_t target = 0, first = 0, last = 0, execute = 0;
			bool plain = false, free = false, escape = false;
		};
		// A part's pickable commands in the resolver's order, groups (0x8005 .. 0x8006) expanded in place, and its end command.
		struct part
		{
			u32 at = 0, length = 0, flags = 0;
			bool attack = false;
			std::vector<entry> entries;
			entry end;
		};
		struct stop
		{
			u32 free = 0;
			state at = state::stand;
		};
		// What a part does from the cursor it is entered on: its fighter is free (or down) on `cursor`, or the part hands
		// over to `target` on `cursor` (the target shows its first cursor on the next frame). `left` is the last cursor on
		// which an input of the part can still be taken.
		struct course
		{
			enum class kind : std::uint8_t
			{
				free,
				down,
				hand_over
			} is = kind::free;
			u32 cursor = 0, left = 0, target = 0, data = 0, execute = 0;
			via how = via::command;
			state at = state::stand;
		};
		// A part on the walked path, for its escapes: the cursor it was entered on, the offset of its cursor 0 and the cursor
		// it was left on.
		struct visit
		{
			u32 at = 0, cursor = 0, offset = 0, left = 0;
		};
		struct entered_at
		{
			u32 cursor = 1;
			bool kept = false;
		};

		u32 bank;
		contact outcome;
		bool reaction;
		Word word;
		Half half;

		// Flags that can guard: 0x1000 set, with neither 0x4000 (falling or lying) nor 0x8000 (airborne).
		static bool guards(u32 flags)
		{
			return (flags & 0x1000) && !(flags & 0xc000);
		}
		static bool down_bits(u32 flags)
		{
			return (flags & 0x6000) == 0x6000;
		}
		static bool floor_alias(u32 target)
		{
			return (target >= 0x8004 && target <= 0x800e) || target == 0x8017;
		}
		static void note(reading& out, u32 at)
		{
			if (!at)
				return;
			for (u32 index = 0; index < out.path_count; ++index)
				if (out.path[index] == at)
					return;
			if (out.path_count < out.path.size())
				out.path[out.path_count++] = at;
		}
		bool is_part(u32 at) const
		{
			return table_member(bank, 0x1e0, 0x4c, at, 8192, word);
		}
		bool is_command(u32 at) const
		{
			return table_member(bank, 0x1b0, 24, at, 65536, word) || table_member(bank, 0x1b8, 24, at, 65536, word);
		}
		bool attack(u32 at) const
		{
			return pbd::practice::frames::attack_window(half(at + 0x44), half(at + 0x46), half(at + 0x24));
		}
		// A target's move record: a move of the bank, or an alias resolved through the bank's alias table (0xD469C).
		u32 record(u32 target) const
		{
			if (target >= 0x8030)
				return 0;
			if (target >= 0x8000)
				target = word(bank + 0xac + (target - 0x8000) * 4);
			const u32 count = word(bank + 0x1e4);
			if (target >= count || count > 8192)
				return 0;
			const u32 at = word(bank + 0x1e0) + target * 0x4c;
			return is_part(at) ? at : 0;
		}
		// The stance a neutral target names: standing (0x8001), crouching (0x8002), or another alias of their records.
		std::optional<state> neutral(u32 target) const
		{
			if (target == 0x8001)
				return state::stand;
			if (target == 0x8002)
				return state::crouch;
			if (target < 0x8000 || target >= 0x8030)
				return {};
			const u32 index = word(bank + 0xac + (target - 0x8000) * 4);
			if (index == word(bank + 0xac + 4))
				return state::stand;
			if (index == word(bank + 0xac + 8))
				return state::crouch;
			return {};
		}
		state standing(u32 at) const
		{
			if (at == record(0x8001))
				return state::stand;
			if (at == record(0x8002))
				return state::crouch;
			return state::stance;
		}
		// A requirement list (ids before the 327 terminator). The plain path takes lists that always hold or test the
		// contact (42 hit, 43 block, 44 whiff); a free part's input takes only lists that always hold; an escape also takes
		// tags (0x8000 on) and sides (113..116).
		void requirements(u32 list, entry& out) const
		{
			bool plain = true, free = true, escape = true;
			for (unsigned index = 0; index < 16; ++index, list += 4)
			{
				if (!table_member(bank, 0x190, 4, list, 65536, word))
					break;
				const u32 id = half(list);
				if (id == 327)
				{
					out.plain = plain;
					out.free = free;
					out.escape = escape;
					return;
				}
				if (id == 0)
					continue;
				free = false;
				plain = plain && ((id == 42 && outcome == contact::hit) || (id == 43 && outcome == contact::block) ||
									 (id == 44 && outcome == contact::unavailable));
				escape = escape && (id >= 0x8000 || (id >= 113 && id <= 116));
			}
			out.plain = out.free = out.escape = false;
		}
		entry read_entry(u32 command, u32 input) const
		{
			entry out;
			out.input = input;
			const u32 data = word(command + 12);
			out.data = table_member(bank, 0x1c0, 4, data, 4096, word) ? word(data) : 0x80;
			out.target = static_cast<std::uint16_t>(half(command + 8));
			out.first = static_cast<std::uint16_t>(half(command + 16));
			out.last = static_cast<std::uint16_t>(half(command + 18));
			out.execute = static_cast<std::uint16_t>(half(command + 20));
			if (input != end_input)
				requirements(word(command + 4), out);
			return out;
		}
		bool load(u32 at, part& out) const
		{
			out.entries.clear();
			if (!is_part(at))
				return false;
			out.at = at;
			out.length = half(at + 0x24);
			out.flags = word(at + 0x0c);
			out.attack = attack(at);
			if (!out.length || out.length > 32767)
				return false;
			u32 command = word(at + 0x14), resume = 0;
			for (unsigned index = 0; index < 1024; ++index)
			{
				if (!is_command(command))
					return false;
				const u32 input = word(command), head = input >> 16;
				if (input == end_input)
				{
					if (resume)
						return false;
					out.end = read_entry(command, input);
					return true;
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
				if ((half(command + 22) & 0x42) == 0x40 && (!input || head < 0x8000))
				{
					if (out.entries.size() == max_entries)
						return false;
					out.entries.push_back(read_entry(command, input));
				}
				command += 24;
			}
			return false;
		}
		// An input that makes its fighter free: one that always holds, into an attack or a neutral alias, and not the 0x3FFF
		// form (which the resolver turns into the end command).
		bool action(const entry& command) const
		{
			if (!command.input || !command.free || command.data == 0x3fff)
				return false;
			if (neutral(command.target))
				return true;
			const u32 at = command.target < 0x8000 ? record(command.target) : 0;
			return at && attack(at);
		}
		// The cursor a command taken on `taken` executes on: its execute cursor (0xFF: the part's last), not before the
		// cursor it was taken on and at the latest on the part's last cursor (0xF1AC4).
		static u32 executes(const entry& command, u32 taken, u32 length)
		{
			const u32 execute = command.execute == 0xff ? length : command.execute;
			return (std::min)((std::max)(taken, execute), length);
		}
		// The first cursor a transition's target shows: the extra word's low 14 bits as 0xEEC00 adjusts them, then the
		// commit (0xF2020): the 0x400 forms keep the cursor (the next one) unless the target's attack window is already past
		// or the transition came through the end command's window, 0x1400 keeps it, 0x2000 takes the execute cursor, the
		// unchanged forms leave it as it is, and the rest restart on cursor 1. Clamped to the target's length.
		entered_at entry_cursor(u32 target_at, u32 data, u32 cursor, u32 execute, via how) const
		{
			const u32 length = (std::max)(half(target_at + 0x24), 1u), first = half(target_at + 0x44), last = half(target_at + 0x46);
			u32 value = data & 0x3fff;
			const u32 low = value & 0x3f;
			const bool short_form = low >= 2 && low <= 4;
			if ((value & 0x3c00) == 0x400)
			{
				if (first && execute + 1 < last && length >= last)
					value = short_form ? value : ((value & ~0x3fu) | 2);
				else if (short_form)
					value = 0x213;
			}
			else if (low <= 11 && ((0x81cu >> low) & 1) && !first)
				value = 0x213;
			return prepared_cursor(target_at, value, cursor, execute, how);
		}
		// +310 has already passed through 0xEEC00; only the commit's entry-cursor switch remains.
		entered_at prepared_cursor(u32 target_at, u32 value, u32 cursor, u32 execute, via how) const
		{
			const u32 length = (std::max)(half(target_at + 0x24), 1u), first = half(target_at + 0x44), last = half(target_at + 0x46);
			entered_at out;
			switch (value & 0x3c00)
			{
			case 0x0000:
			case 0x0800:
			case 0x0c00:
			case 0x1000:
			case 0x1800: break;
			case 0x0400:
				if (how != via::chain && !(first && (length < first || last < cursor)))
					out = {cursor + 1, true};
				break;
			case 0x1400: out = {cursor + 1, true}; break;
			case 0x2000: out = {execute, false}; break;
			default: out = {cursor, true}; break;
			}
			out.cursor = (std::clamp)(out.cursor, 1u, length);
			return out;
		}
		// The first cursor from `from` on at which a 0x8073 property of the part sets flags (its value's high half) that can
		// guard, or, with `down`, flags with both down bits.
		std::optional<u32> property_flags(u32 at, u32 from, bool down) const
		{
			std::optional<u32> found;
			u32 property = word(at + 0x30);
			for (unsigned index = 0; property && index < 16; ++index, property += 8)
			{
				if (!table_member(bank, 0x1c8, 8, property, 65536, word))
					break;
				const u32 frame = half(property);
				if (!frame || frame > 32767)
					break;
				const u32 flags = word(property + 4) >> 16;
				if (half(property + 2) == 0x8073 && (down ? down_bits(flags) : guards(flags)) && frame >= from && (!found || frame < *found))
					found = frame;
			}
			return found;
		}
		// The end command's window: the target's cursor-1 commands run as if it were already there (0xF07A4). One of its
		// inputs into an action makes the fighter free (on a floor alias, any input it can take: down); its first
		// input-free command hands over straight away; neither: nothing happens in the window.
		std::optional<course> chain(const entry& end) const
		{
			part target;
			const u32 at = record(end.target);
			if (!at || !load(at, target))
				return {};
			const bool floor = floor_alias(end.target);
			for (const entry& command : target.entries)
			{
				if (command.first > 1 || command.last < 1)
					continue;
				if (command.input)
				{
					if (floor ? command.escape && command.data != 0x3fff : !target.attack && action(command))
					{
						course out;
						out.is = floor ? course::kind::down : course::kind::free;
						out.target = end.target;
						out.at = floor ? state::down : standing(at);
						return out;
					}
					continue;
				}
				if (!command.plain)
					continue;
				if (command.data == 0x3fff)
					return {};
				course out;
				out.is = course::kind::hand_over;
				out.target = command.target;
				out.data = command.data;
				out.execute = end.first;
				out.how = via::chain;
				return out;
			}
			return {};
		}
		// What part `p` does without input from cursor `from`. With `checks`, its inputs, guard flags and guard properties
		// make it free; otherwise, with `own`, the cursor its inputs would free it on is noted there. On each cursor the
		// resolver takes the first command in list order whose window holds it; after an input-free one the part waits for
		// its execute cursor. The end command comes last: inside its window it runs its target's cursor-1 commands, and on
		// the last cursor it hands over to the target.
		std::vector<u32> follow_cursors(const part& p, u32 from, u32 length, bool checks, std::optional<u32>& guard) const
		{
			std::vector<u32> cursors{from, length};
			const auto add = [&](u32 cursor)
			{
				if (cursor >= from && cursor <= length)
					cursors.push_back(cursor);
			};
			for (const entry& command : p.entries)
			{
				add(command.first);
				add(command.last + 1u);
			}
			add(p.end.first);
			if (checks)
			{
				if (guards(p.flags))
					guard = from;
				if (const auto property = property_flags(p.at, from, false); property && (!guard || *property < *guard))
					guard = property;
				if (guard)
					add(*guard);
			}
			std::sort(cursors.begin(), cursors.end());
			cursors.erase(std::unique(cursors.begin(), cursors.end()), cursors.end());

			return cursors;
		}

		std::optional<course> follow(const part& p, u32 from, bool checks, std::optional<u32>* own, std::optional<u32>* inputs) const;
		// The walk from part `at` shown first on cursor `cursor`, whose cursor 0 is on offset `offset` from the move's
		// origin. `committed`: the part is the fighter's committed move, read without its own inputs, as are the twins it
		// hands over to keeping the cursor. With `out`, the plain path, the committed non-attack's own free cursor and, when
		// the walk stops down or stunned, the escapes are noted.
		std::optional<stop> run(u32 at, u32 cursor, u32 offset, bool committed, reading* out) const;
		// A part met again on the same cursor (a loop), or one past the limit: free where its inputs open, or on the cursor.
		std::optional<stop> loop(u32 at, u32 cursor, u32 offset) const
		{
			part p;
			if (!load(at, p))
				return {};
			std::optional<u32> inputs;
			follow(p, cursor, false, nullptr, &inputs);
			return stop{offset + (inputs ? *inputs : (std::min)(cursor, p.length)), standing(at)};
		}
		// A walk stopped on the floor: the escapes of the parts before it (tech rolls) are noted.
		std::optional<stop> finish(stop found, const std::array<visit, max_parts>& visits, std::size_t parts, bool committed_attack, reading* out) const
		{
			if (out)
			{
				reading escapes;
				collect(visits, parts, committed_attack, ~0u, escapes);
				copy_escapes(escapes, *out);
			}
			return found;
		}
		static void copy_escapes(const reading& from, reading& to)
		{
			to.escape = from.escape;
			to.escape_target = from.escape_target;
			to.routes = from.routes;
			to.route_count = from.route_count;
		}
		// The escapes of the walked parts that are not attacks: inputs whose requirements always hold, are tags or sides,
		// into a move that is no attack and whose own walk ends free (standing, crouching or a stance). Each is taken on the
		// first cursor its window and the part share and executes on its execute cursor; the part's last input cursor
		// closes it. Only those that leave the fighter free before `before` count; one route per target, the fastest.
		void collect(const std::array<visit, max_parts>& visits, std::size_t parts, bool committed_attack, u32 before, reading& out) const;
		static void add_route(reading& out, std::array<u32, 4>& totals, const pbd::practice::frames::escape_route& route, u32 total)
		{
			if (!out.escape || total < *out.escape)
			{
				out.escape = total;
				out.escape_target = route.target;
			}
			std::size_t slot = out.route_count;
			for (std::size_t index = 0; index < out.route_count; ++index)
				if (out.routes[index].target == route.target)
					slot = index;
			if (slot == out.route_count && out.route_count == out.routes.size())
			{
				// Full: the slowest route gives way to a faster one.
				slot = 0;
				for (std::size_t index = 1; index < out.routes.size(); ++index)
					if (totals[index] > totals[slot])
						slot = index;
			}
			if (slot < out.route_count && totals[slot] <= total)
				return;
			if (slot == out.route_count)
				++out.route_count;
			out.routes[slot] = route;
			totals[slot] = total;
		}
	};

	// A move's readiness from the loaded bank, read from cursor `cursor` of the committed move `allocation` with the
	// contact as known (unavailable: a whiff), for a fighter that entered it as a reaction (`reaction`) or by itself: the
	// offset from the move's origin (commit frame minus commit cursor) of the frame on which the fighter is free again, how
	// it stands then, and the escapes the data offers on the way.
	//
	// It follows the game's own command handling. On each cursor the resolver (0xEF790) picks the first command in list
	// order whose window holds the cursor, whose option has 0x40 and not 0x02 and whose input and requirements hold; an
	// extra word of 0x3FFF turns the pick into the end command. Without input only input-free commands are picked, with
	// requirements that always hold or test the contact (42 hit, 43 block, 44 whiff). A picked command executes on its
	// execute cursor and the next frame shows its target: on cursor 1, or for the cursor-keeping extra words (the 0x400
	// forms) on the next cursor (0xEEC00, 0xF2020). Inside the end command's window the target's cursor-1 commands run on
	// the spot (0xF07A4), so a neutral target is free there; otherwise the end command hands over on the last cursor.
	// Offsets count frames: a part's cursor k is on its cursor-0 offset + k, and a fighter is free on the frame its input is
	// taken, so a neutral alias entered by a hand-over is free on its first frame.
	//
	// An entered part that is not an attack (and not a twin of the committed move entered keeping the cursor) is free from
	// the first cursor on which: its flags (+0x0C) can guard (0x1000 set, 0x4000 and 0x8000 clear); a 0x8073 property sets
	// such flags; an input that always holds into an attack or a neutral alias is taken. The committed move's own inputs are
	// not counted, so a reaction reads to its end; for a committed non-attack that is no reaction (a sway, a sidestep, a
	// stance) `own` notes where they would free the fighter. The walk stops at:
	//  - the first free part: standing (alias 0x8001), crouching (alias 0x8002) or a stance (any other part);
	//  - a floor alias (0x8004..0x800E, 0x8017): down, on its first frame, the first from which the fighter can roll or
	//    get up;
	//  - in a reaction without down bits (0x6000, in its flags or set by a 0x8073 property), the first hand-over into a part
	//    with them, when an escape leaves the fighter free before then: a stun, ending on that part's first frame. Without
	//    such an escape the walk goes on to the floor.
	// Escapes are collected only when the walk stops down or stunned (see collect). At most eight parts are walked; a part
	// met again on the same cursor (a loop) or a ninth stops where that part's inputs open.
	template <class Word, class Half>
	std::optional<pbd::practice::frames::readiness_reading> readiness(u32 bank, u32 allocation, u32 cursor,
		pbd::practice::frames::contact outcome, Word word, Half half, bool reaction = false)
	{
		if (bank < 0x10000000 || bank > 0x7ffffd00)
			return {};
		return readiness_walk<Word, Half>(bank, outcome, reaction, word, half).committed(allocation, cursor);
	}
	template <class Word, class Half>
	std::optional<queued_reading> queued_readiness(u32 bank, u32 allocation, u32 execute, u32 parameter,
		pbd::practice::frames::contact outcome, Word word, Half half)
	{
		if (bank < 0x10000000 || bank > 0x7ffffd00)
			return {};
		return readiness_walk<Word, Half>(bank, outcome, false, word, half).queued(allocation, execute, parameter);
	}
} // namespace pbd::move_list

#include "pbd_move_readiness_path.h"
