#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/PBD/pbd_move_readiness.h"
#include "Emu/PBD/pbd_practice_frames.h"
#include "Emu/PBD/pbd_native_call.h"

#include <map>
#include <optional>

PBD_TEST(PbdPractice, FullDiagnosticBufferPreservesLiveFrameData)
{
	using pbd::test::check;
	struct environment
	{
		std::string practice = pbd::env::find("PBD_PRACTICE_REWIND") ? pbd::env::find("PBD_PRACTICE_REWIND") : "";
		std::string quiet = pbd::env::find("PBD_PRACTICE_REWIND_QUIET") ? pbd::env::find("PBD_PRACTICE_REWIND_QUIET") : "";
		static void put(const char* key, const char* value)
		{
#ifdef _WIN32
			_putenv_s(key, value);
#else
			if (*value)
				setenv(key, value, 1);
			else
				unsetenv(key);
#endif
		}
		environment()
		{
			put("PBD_PRACTICE_REWIND", "1");
			put("PBD_PRACTICE_REWIND_QUIET", "0");
		}
		~environment()
		{
			put("PBD_PRACTICE_REWIND", practice.c_str());
			put("PBD_PRACTICE_REWIND_QUIET", quiet.c_str());
		}
	} switches;
	pbd::practice::native::frame_observer state;
	state.tracker.boundary(100);
	state.tracker.commit(0, 7, 10, true, 12);
	pbd::practice::frames::readiness_reading reading;
	reading.free = 21;
	state.calculator.commit_reading(0, 7, 100, 1, reading, true, 12, 10);
	state.windows[0] = {7, 20, 30};
	state.references[0].startup = 10;
	for (unsigned i = 0; i < 80; ++i)
		state.observe(0xf07a4, 0, 0, 20, 30, 0x8001);
	check(state.observation_count == state.observations.size());
	check(state.observations[62].pc == 0xf07a4 && state.observations[63].pc == 0);
	check(state.tracker.latest[0].move == 7 && state.tracker.latest[0].startup == 10);
	check(state.calculator.latest[0].move == 7 && state.calculator.latest[0].ready == 120);
	check(state.windows[0].move == 7 && state.windows[0].start == 20 && state.windows[0].end == 30 && state.references[0].startup == 10);
	for (unsigned frame = 101; frame <= 119; ++frame)
		state.tracker.boundary(frame);
	state.tracker.actionable(0, 7, 20);
	check(state.tracker.latest[0].on_whiff == -9 && state.tracker.latest[0].whiff_ready == 120);
}

namespace
{
	using u32 = std::uint32_t;
	using pbd::practice::frames::branch;
	using pbd::practice::frames::calculator;
	using pbd::practice::frames::contact;
	using pbd::practice::frames::readiness_reading;
	using pbd::practice::frames::ready_state;

	using pbd::test::check;

	// A move bank in the loaded layout: moves (0x4C bytes) at +0x1E0, commands (24 bytes) at +0x1B0, command groups at
	// +0x1B8, requirement lists at +0x190, extra words at +0x1C0, properties (8 bytes) at +0x1C8 and the alias table at +0xAC.
	struct move_bank_model
	{
		static constexpr u32 bank = 0x20000000, moves = 0x21000000, commands = 0x22000000, groups = 0x23000000, requirements = 0x25000000,
							 extra = 0x26000000, properties = 0x27000000;
		// Requirement lists, each ended by 327: always true, the contact tests, a tag, a side and a game condition.
		static constexpr u32 always = requirements, on_hit = requirements + 8, on_block = requirements + 16,
							 on_whiff = requirements + 24, tag = requirements + 32, side = requirements + 40, condition = requirements + 48;
		// Extra words: plain 0x80, the cursor-keeping 0x401 and 0x402, the 0x2400 marker and 0x3FFF (the end command).
		static constexpr u32 plain = extra, extra_401 = extra + 4, marker = extra + 8, extra_402 = extra + 12, to_end = extra + 16;
		static constexpr u32 standing = 60, crouching = 61, lying = 62;
		std::map<u32, u32> words;
		u32 next = commands, next_group = groups, next_property = properties;

		move_bank_model()
		{
			words[bank + 0x1e0] = moves;
			words[bank + 0x1e4] = 64;
			words[bank + 0x1b0] = commands;
			words[bank + 0x1b4] = 512;
			words[bank + 0x1b8] = groups;
			words[bank + 0x1bc] = 64;
			words[bank + 0x190] = requirements;
			words[bank + 0x194] = 14;
			words[bank + 0x1c0] = extra;
			words[bank + 0x1c4] = 5;
			words[bank + 0x1c8] = properties;
			words[bank + 0x1cc] = 32;
			const auto list = [&](u32 at, u32 id)
			{
				words[at] = id << 16;
				words[at + 4] = 327u << 16;
			};
			list(always, 0);
			list(on_hit, 42);
			list(on_block, 43);
			list(on_whiff, 44);
			list(tag, 0x8162);
			list(side, 113);
			words[condition] = (135u << 16) | 4u;
			words[condition + 4] = 327u << 16;
			words[plain] = 0x80;
			words[extra_401] = 0x401;
			words[marker] = 0x2400;
			words[extra_402] = 0x402;
			words[to_end] = 0x3fff;
			// Aliases 1 and 2 name the standing and crouching records, 4 and 0x17 a lying pose with a get-up from cursor 1.
			words[bank + 0xac + 4] = standing;
			words[bank + 0xac + 8] = crouching;
			words[bank + 0xac + 4 * 4] = lying;
			words[bank + 0xac + 0x17 * 4] = lying;
			move(standing, 60, 0x1052);
			end(0x8001, 60);
			move(crouching, 60, 0x1052);
			end(0x8002, 60);
			move(lying, 60, 0x6004);
			command(0x0004, 0x8001, 1, 60, 0x50);
			end(0x8001, 60);
		}
		u32 move(u32 index, u32 length, u32 flags = 0, u32 first = 0, u32 last = 0, u32 level = 0)
		{
			const u32 at = moves + index * 0x4c;
			words[at + 0x0c] = flags;
			words[at + 0x10] = level;
			words[at + 0x14] = next;
			words[at + 0x24] = length << 16;
			words[at + 0x44] = (first << 16) | last;
			return at;
		}
		u32 attack(u32 index, u32 length)
		{
			return move(index, length, 0, 10, 12, 0x217);
		}
		void command(u32 input, u32 target, u32 first, u32 last, u32 option, u32 requirement = always, u32 data = plain, u32 execute = 0)
		{
			words[next] = input;
			words[next + 4] = requirement;
			words[next + 8] = target << 16;
			words[next + 12] = data;
			words[next + 16] = (first << 16) | last;
			words[next + 20] = (execute << 16) | option;
			next += 24;
		}
		void automatic(u32 target, u32 at, u32 requirement = always, u32 data = plain)
		{
			command(0, target, at, at, 0x41, requirement, data);
		}
		void end(u32 target, u32 first, u32 last = 255)
		{
			command(0x80000000, target, first, last, 0x50);
		}
		// A group of commands (0x8005 in the move's list, its commands in the group table up to 0x8006), written by `fill`.
		template <class Fill>
		void group(Fill fill)
		{
			command(0x80050000, (next_group - groups) / 24, 0, 0, 0);
			const u32 list = next;
			next = next_group;
			fill();
			command(0x80060000, 0, 0, 0, 0);
			next_group = next;
			next = list;
		}
		// A 0x8073 property's value carries the flags it sets in its high half.
		void property(u32 at, u32 frame, u32 id, u32 value)
		{
			words[at + 0x30] = next_property;
			words[next_property] = (frame << 16) | id;
			words[next_property + 4] = value;
			// The entry after it reads frame 0: the list's end.
			next_property += 16;
		}
		static u32 record(u32 index)
		{
			return moves + index * 0x4c;
		}
		std::optional<readiness_reading> read(u32 at, u32 cursor = 1, contact kind = contact::unavailable, bool reaction = false) const
		{
			const auto word = [&](u32 address)
			{
				const auto found = words.find(address);
				return found == words.end() ? 0u : found->second;
			};
			const auto half = [&](u32 address)
			{
				return (word(address & ~3u) >> ((address & 2) ? 0 : 16)) & 65535u;
			};
			return pbd::move_list::readiness(bank, at, cursor, kind, word, half, reaction);
		}
		std::optional<u32> actionable(u32 at) const
		{
			const auto word = [&](u32 address)
			{
				const auto found = words.find(address);
				return found == words.end() ? 0u : found->second;
			};
			const auto half = [&](u32 address)
			{
				return (word(address & ~3u) >> ((address & 2) ? 0 : 16)) & 65535u;
			};
			return pbd::move_list::actionable(bank, at, word, half);
		}
		std::optional<pbd::move_list::queued_reading> queued(u32 at, u32 execute, u32 parameter) const
		{
			const auto word = [&](u32 address)
			{
				const auto found = words.find(address);
				return found == words.end() ? 0u : found->second;
			};
			const auto half = [&](u32 address)
			{
				return (word(address & ~3u) >> ((address & 2) ? 0 : 16)) & 65535u;
			};
			return pbd::move_list::queued_readiness(bank, at, execute, parameter, contact::block, word, half);
		}
	};

	std::optional<u32> free_of(const std::optional<readiness_reading>& reading)
	{
		return reading ? std::optional<u32>(reading->free) : std::nullopt;
	}
	readiness_reading plain_reading(u32 free, ready_state state = ready_state::stand)
	{
		readiness_reading out;
		out.free = free;
		out.state = state;
		return out;
	}
	// A reading with one escape: the move `target` shows its cursor 1 on offset `commit` + 1 at the earliest and on `close`
	// at the latest, and is free `rest` frames after its own cursor 0.
	readiness_reading choice_reading(u32 free, ready_state state, u32 target, u32 commit, u32 rest, u32 close)
	{
		readiness_reading out = plain_reading(free, state);
		out.escape = commit + rest;
		out.escape_target = target;
		out.routes[0] = {target, rest, close};
		out.route_count = 1;
		return out;
	}
} // namespace

PBD_TEST(PbdPracticeReadiness, ReaderNeutralAndCrouchEnds)
{
	move_bank_model data;
	const u32 jab = data.attack(2, 39);
	data.command(0x2002, 3, 1, 14, 0x50);
	data.end(0x8001, 30);
	const auto standing = data.read(jab);
	check(standing && standing->free == 30u && standing->state == ready_state::stand && !standing->escape && !standing->path_count && !standing->own,
		"an end into the standing alias is free on its window's first cursor");
	check(data.actionable(jab) == 30u, "the standing command opens at cursor 30");
	const u32 low = data.attack(3, 60);
	data.end(0x8002, 54);
	const auto crouching = data.read(low);
	check(crouching && crouching->free == 54u && crouching->state == ready_state::crouch, "an end into the crouching alias is crouching");
	// An input-free hand-over executed on cursor 25: the standing alias shows on the next frame, the first one to act on.
	const u32 handed = data.attack(4, 60);
	data.automatic(0x8001, 25);
	data.end(0x8001, 50);
	check(free_of(data.read(handed)) == 26u, "a hand-over into the standing alias is free on the alias's first frame");
}

PBD_TEST(PbdPracticeReadiness, ReaderOpenPart)
{
	move_bank_model data;
	data.attack(2, 39);
	data.end(0x8001, 30);
	// Inside the end command's window the next part's cursor-1 commands already run: its input into an attack frees the
	// fighter on the window's first cursor, before the part is entered and not after the whole chain.
	const u32 chained = data.attack(4, 70);
	data.end(5, 45);
	data.move(5, 60);
	data.command(0x2002, 2, 1, 255, 0x50);
	data.end(0x8001, 38);
	const auto reading = data.read(chained);
	check(reading && reading->free == 45u && reading->state == ready_state::stance && !reading->path_count,
		"a part whose input into an attack is open on its first cursor frees the end window");
	check(data.actionable(chained) == 83u, "where the chain reader counted the part to its end");
	// An entered part: a group's input into the standing alias opens on its cursor 3.
	const u32 grouped = data.attack(6, 70);
	data.automatic(7, 45);
	data.end(0x8001, 60);
	const u32 open = data.move(7, 60);
	data.group([&]
		{
			data.command(0x0010, 0x8001, 3, 255, 0x50);
		});
	data.end(0x8001, 38);
	const auto group = data.read(grouped);
	check(group && group->free == 48u && group->state == ready_state::stance, "a group's input into a neutral alias frees an entered part");
	check(group->path_count == 1 && group->path[0] == open, "the entered part is the plain path");
	// An input that needs a game condition does not make a part free.
	const u32 gated = data.attack(8, 70);
	data.automatic(9, 45);
	data.end(0x8001, 60);
	data.move(9, 60);
	data.command(0x2002, 2, 1, 255, 0x50, move_bank_model::condition);
	data.end(0x8001, 38);
	const auto conditional = data.read(gated);
	check(conditional && conditional->free == 83u && conditional->state == ready_state::stand, "a conditional input leaves the part to its plain end");
}

PBD_TEST(PbdPracticeReadiness, ReaderGuardFlagsAndProperty)
{
	move_bank_model data;
	const u32 guarded = data.attack(10, 40);
	data.automatic(11, 20);
	data.end(0x8001, 40);
	data.move(11, 30, 0x1052);
	data.end(0x8001, 30);
	const auto flags = data.read(guarded);
	check(flags && flags->free == 21u && flags->state == ready_state::stance, "a part whose flags can guard is free on its first frame");
	const u32 later = data.attack(12, 40);
	data.automatic(13, 20);
	data.end(0x8001, 40);
	const u32 property = data.move(13, 30);
	data.property(property, 5, 0x8073, 0x10520000);
	data.end(0x8001, 30);
	check(free_of(data.read(later)) == 25u, "a 0x8073 property setting flags that can guard frees the part from its frame");
	const u32 turned = data.attack(14, 40);
	data.automatic(15, 20);
	data.end(0x8001, 40);
	const u32 away = data.move(15, 30);
	data.property(away, 5, 0x8073, 0x50520000);
	data.end(0x8001, 30);
	const auto unguarded = data.read(turned);
	check(unguarded && unguarded->free == 50u && unguarded->state == ready_state::stand, "flags with 0x4000 set cannot guard");
	// Inside an end command's window the next part's flags do not count yet: it guards once it shows.
	const u32 ended = data.attack(16, 40);
	data.end(11, 20);
	check(free_of(data.read(ended)) == 41u, "an end into a guarding part is free on the part's first frame");
}

PBD_TEST(PbdPracticeReadiness, ReaderExtraWords)
{
	move_bank_model data;
	// 0x401 hands over to a twin of the same animation and keeps the cursor: the twin goes on from cursor 13.
	const u32 followed = data.attack(16, 50);
	data.automatic(17, 12, move_bank_model::always, move_bank_model::extra_401);
	data.end(0x8001, 30);
	const u32 twin = data.move(17, 50);
	data.end(0x8001, 30);
	const auto kept = data.read(followed);
	check(kept && kept->free == 30u && kept->path_count == 1 && kept->path[0] == twin, "a 0x401 hand-over keeps the cursor");
	// The plain extra word restarts the target on its cursor 1.
	const u32 restarted = data.attack(18, 50);
	data.automatic(19, 12);
	data.end(0x8001, 30);
	data.move(19, 20);
	data.end(0x8001, 10);
	check(free_of(data.read(restarted)) == 22u, "a plain hand-over restarts the target");
	// Only commands with option 0x40 and without 0x02 are picked; the 0x2400 markers carry option 0x01.
	const u32 marked = data.attack(20, 50);
	data.command(0, 19, 12, 12, 0x01, move_bank_model::always, move_bank_model::marker);
	data.command(0, 19, 13, 13, 0x43);
	data.end(0x8001, 30);
	const auto skipped = data.read(marked);
	check(skipped && skipped->free == 30u && !skipped->path_count, "a command the resolver cannot pick is no transition");
}

PBD_TEST(PbdPracticeReadiness, ReaderOutcomeGates)
{
	move_bank_model data;
	const u32 gated = data.attack(19, 60);
	data.automatic(20, 15, move_bank_model::on_hit);
	data.automatic(21, 16, move_bank_model::on_block);
	data.automatic(22, 17, move_bank_model::on_whiff);
	data.end(0x8001, 40);
	data.move(20, 20);
	data.end(0x8001, 5);
	data.move(21, 20);
	data.end(0x8001, 6);
	data.move(22, 20);
	data.end(0x8001, 7);
	check(free_of(data.read(gated, 12, contact::hit)) == 20u, "a hit takes the hit branch");
	check(free_of(data.read(gated, 12, contact::block)) == 22u, "a block takes the block branch");
	check(free_of(data.read(gated, 1, contact::unavailable)) == 24u, "an unknown contact takes the whiff branch");
	const u32 conditional = data.attack(23, 60);
	data.automatic(20, 15, move_bank_model::condition);
	data.end(0x8001, 40);
	check(free_of(data.read(conditional, 1, contact::hit)) == 40u, "a transition on a game condition is left out");
}

PBD_TEST(PbdPracticeReadiness, ReaderExecuteCursor)
{
	// An input-free command executes on its execute cursor, whichever cursor of its window it was picked on: here a
	// hand-over with window 1-15 and execute cursor 15 out of a hit active on 13-15 into the next hit (active on 19-20).
	move_bank_model data;
	const u32 first_hit = data.move(40, 50, 0, 13, 15, 0x217);
	data.command(0, 41, 1, 15, 0x41, move_bank_model::always, move_bank_model::extra_401, 15);
	data.end(0x8001, 50);
	data.move(41, 50, 0, 19, 20, 0x217);
	data.end(0x8001, 40);
	check(free_of(data.read(first_hit)) == 40u, "read at the commit, the next hit's end counts");
	check(free_of(data.read(first_hit, 13, contact::block)) == 40u, "read again at a contact on cursor 13, it is not free there");
	// Hand-overs on the contact with window 18-25 and execute cursor 25: neither fires on 18.
	const u32 gated = data.attack(42, 62);
	data.command(0, 43, 18, 25, 0x41, move_bank_model::on_whiff, move_bank_model::plain, 25);
	data.command(0, 44, 18, 25, 0x41, move_bank_model::on_block, move_bank_model::plain, 25);
	data.end(0x8001, 51);
	data.move(43, 40);
	data.end(0x8001, 30);
	data.move(44, 40);
	data.end(0x8001, 20);
	check(free_of(data.read(gated)) == 55u && free_of(data.read(gated, 12, contact::block)) == 45u, "each fires on its execute cursor");
}

PBD_TEST(PbdPracticeReadiness, ReaderTwinParts)
{
	// A multi-hit move in three parts of one 78-frame animation (hits on 12, 20 and 48-49), each handing over to the next on
	// its hit with 0x402: the cursor runs on, so the move ends where the last part ends.
	move_bank_model data;
	const u32 first_hit = data.move(45, 78, 0, 12, 12, 0x217);
	data.command(0, 46, 12, 12, 0x41, move_bank_model::always, move_bank_model::extra_402, 12);
	data.end(0x8001, 74);
	const u32 second_hit = data.move(46, 78, 0, 20, 20, 0x217);
	data.command(0, 47, 20, 20, 0x41, move_bank_model::always, move_bank_model::extra_402, 20);
	data.end(0x8001, 74);
	data.move(47, 78, 0, 48, 49, 0x217);
	data.end(0x8001, 77);
	const auto whole = data.read(first_hit);
	check(whole && whole->free == 77u && whole->path_count == 2, "the last part's end, not a sum of restarted parts");
	check(free_of(data.read(second_hit, 13)) == 77u, "the second part read from the cursor it was entered on");
	check(data.actionable(first_hit) == 74u, "the first part ends at cursor 74");
}

PBD_TEST(PbdPracticeReadiness, ReaderSelfTargets)
{
	move_bank_model data;
	// A reaction handing over to itself on cursor 1 with 0x401 goes on from cursor 2, so it reads to its own end.
	const u32 reaction = data.move(48, 45);
	data.automatic(48, 1, move_bank_model::always, move_bank_model::extra_401);
	data.end(0x8001, 40);
	check(free_of(data.read(reaction, 1, contact::hit, true)) == 40u, "a cursor-keeping hand-over to itself changes nothing");
	// An attack whose 0x3FFF command names itself on cursors 1-39: that form becomes the end command, which is not due yet.
	const u32 attack = data.attack(49, 60);
	data.command(0, 49, 1, 39, 0x41, move_bank_model::always, move_bank_model::to_end);
	data.end(0x8001, 45);
	const auto own_end = data.read(attack);
	check(own_end && own_end->free == 45u && own_end->state == ready_state::stand, "a 0x3FFF command is no restart");
}

PBD_TEST(PbdPracticeReadiness, ReaderEnteredAttackNotFree)
{
	// An attack handing over to a second attack part (its hit on 23-24) whose follow-up input opens on its cursor 11: the
	// fighter is still attacking, so the input does not free it.
	move_bank_model data;
	data.attack(2, 39);
	data.end(0x8001, 30);
	const u32 first_hit = data.attack(50, 40);
	data.automatic(51, 20);
	data.end(0x8001, 40);
	data.move(51, 60, 0, 23, 24, 0x217);
	data.command(0x2002, 2, 11, 255, 0x50);
	data.end(0x8001, 40);
	check(free_of(data.read(first_hit)) == 60u, "the second part's end");
}

PBD_TEST(PbdPracticeReadiness, ReaderGroundStance)
{
	// An attack ending in a stance on the ground (flags 0x6084, as a sitting stance has) whose attacks open on its first
	// cursor: a stance, not the floor.
	move_bank_model data;
	data.attack(2, 39);
	data.end(0x8001, 30);
	const u32 attack = data.attack(52, 60);
	data.end(53, 45);
	data.move(53, 75, 0x6084);
	data.command(0x2002, 2, 1, 255, 0x50);
	data.end(0x8001, 75);
	const auto reading = data.read(attack);
	check(reading && reading->free == 45u && reading->state == ready_state::stance && !reading->escape, "a ground stance is a stance");
}

PBD_TEST(PbdPracticeReadiness, ReaderFloorAndBufferedTech)
{
	// A jumping attack that hands over to its landing on cursor 40. A buffered 1+2 there executes a tech on the landing's
	// cursor 7; without it the landing ends on the floor alias, where the get-up runs from cursor 10.
	move_bank_model data;
	const u32 jump = data.attack(24, 45);
	data.automatic(25, 40);
	data.end(0x8001, 45);
	const u32 landing = data.move(25, 50, 0x6004);
	data.command(0x0003, 26, 1, 6, 0xd0, move_bank_model::always, move_bank_model::plain, 7);
	data.end(0x8004, 10);
	const u32 tech = data.move(26, 30);
	data.end(0x8001, 20);
	const auto reading = data.read(jump);
	check(reading && reading->state == ready_state::down && reading->free == 50u, "down where the floor's own inputs run");
	check(reading->escape == 67u && reading->escape_target == tech && reading->route_count == 1 && reading->routes[0].rest == 20u &&
			  reading->routes[0].close == 48u,
		"the tech executes on its execute cursor and is free after its own walk");
	check(reading->path_count == 2 && reading->path[0] == landing && reading->path[1] == move_bank_model::record(move_bank_model::lying),
		"the landing and the lying pose are the plain path");
}

PBD_TEST(PbdPracticeReadiness, ReaderCrumple)
{
	// Counter-hit crumple as the data has it: forward (a tag requirement) on cursors 20 to 22, executed on 22, breaks it into
	// a part that can guard; without it the 0x401 hand-over on cursor 50 goes on in a falling twin of the same animation.
	move_bank_model data;
	const u32 crumple = data.move(27, 90, 0x842);
	data.command(0x00004000, 28, 20, 22, 0x50, move_bank_model::tag, move_bank_model::plain, 22);
	data.automatic(29, 50, move_bank_model::always, move_bank_model::extra_401);
	data.end(0x8001, 90);
	const u32 guard = data.move(28, 20, 0x1052);
	data.end(0x8001, 20);
	const u32 collapse = data.move(29, 90, 0x7402);
	data.end(0x8004, 90);
	const auto reading = data.read(crumple, 1, contact::hit, true);
	check(reading && reading->state == ready_state::stun && reading->free == 51u, "the stun ends where the falling twin starts");
	check(reading->escape == 23u && reading->escape_target == guard && reading->route_count == 1 && reading->routes[0].close == 23u,
		"the fastest break leaves it able to guard on 23");
	check(reading->path_count == 1 && reading->path[0] == collapse, "the falling twin is the plain path");
	const auto own = data.read(crumple, 1, contact::hit, false);
	check(own && own->state == ready_state::down && own->free == 90u, "only a reaction is a stun; otherwise it reads to the floor");
}

PBD_TEST(PbdPracticeReadiness, ReaderStunNeedsBreak)
{
	move_bank_model data;
	// A stagger that collapses with no break in its data is a knockdown, read to the floor.
	const u32 stagger = data.move(54, 80, 0x842);
	data.automatic(55, 50, move_bank_model::always, move_bank_model::extra_401);
	data.end(0x8001, 80);
	data.move(55, 80, 0x7402);
	data.end(0x8004, 70);
	const auto fall = data.read(stagger, 1, contact::hit, true);
	check(fall && fall->state == ready_state::down && fall->free == 70u && !fall->escape, "no break: the fall is a knockdown");
	// A reaction whose own property sets the down bits before the hand-over is falling already: a knockdown too.
	const u32 falling = data.move(56, 80, 0x842);
	data.property(falling, 34, 0x8073, 0x74020000);
	data.command(0x00004000, 57, 20, 22, 0x50, move_bank_model::tag);
	data.automatic(55, 50, move_bank_model::always, move_bank_model::extra_401);
	data.end(0x8001, 80);
	data.move(57, 20, 0x1052);
	data.end(0x8001, 20);
	const auto knocked = data.read(falling, 1, contact::hit, true);
	check(knocked && knocked->state == ready_state::down && knocked->free == 70u, "down bits a property sets make it a knockdown");
}

PBD_TEST(PbdPracticeReadiness, ReaderKnockdownTech)
{
	move_bank_model data;
	// The tech input names a side (requirement 113): either side counts as possible.
	const u32 falling = data.move(30, 60, 0x6000);
	data.command(0x0003, 31, 34, 43, 0xd0, move_bank_model::side, move_bank_model::plain, 43);
	data.end(0x8004, 52);
	const u32 roll = data.move(31, 30);
	data.end(0x8001, 25);
	const auto reading = data.read(falling, 1, contact::hit, true);
	check(reading && reading->state == ready_state::down && reading->free == 52u, "a fall is down where the floor's own inputs run");
	check(reading->escape == 68u && reading->escape_target == roll && reading->routes[0].close == 44u,
		"the buffered tech executes on 43 and adds the roll's own walk");
	check(reading->path_count == 1 && reading->path[0] == move_bank_model::record(move_bank_model::lying), "the lying pose is the plain path");
}

PBD_TEST(PbdPracticeReadiness, ReaderRoutes)
{
	// Two inputs into the same roll count once; with more than four ways out the slowest gives way to a faster one.
	move_bank_model data;
	const u32 falling = data.move(30, 60, 0x6000);
	data.command(0x0003, 31, 34, 43, 0xd0, move_bank_model::side, move_bank_model::plain, 43);
	data.command(0x0003, 31, 34, 43, 0xd0, move_bank_model::side, move_bank_model::plain, 43);
	data.command(0x0010, 32, 30, 43, 0x50);
	data.command(0x0020, 33, 30, 43, 0x50);
	data.command(0x0040, 34, 30, 43, 0x50);
	data.command(0x0080, 35, 30, 43, 0x50);
	data.end(0x8004, 52);
	data.move(31, 30);
	data.end(0x8001, 25);
	data.move(32, 60);
	data.end(0x8001, 40);
	data.move(33, 60);
	data.end(0x8001, 41);
	data.move(34, 60);
	data.end(0x8001, 42);
	data.move(35, 30);
	data.end(0x8001, 5);
	const auto reading = data.read(falling, 1, contact::hit, true);
	check(reading && reading->escape == 35u && reading->escape_target == move_bank_model::record(35) && reading->route_count == 4,
		"the fastest escape is the alternative");
	unsigned rolls = 0, slowest = 0;
	for (u32 index = 0; index < reading->route_count; ++index)
	{
		rolls += reading->routes[index].target == move_bank_model::record(31);
		slowest += reading->routes[index].target == move_bank_model::record(34);
	}
	check(rolls == 1 && !slowest, "one route per move, the slowest dropped");
}

PBD_TEST(PbdPracticeReadiness, ReaderLoopBound)
{
	move_bank_model data;
	const u32 entry = data.move(32, 20);
	data.end(33, 10);
	const u32 stance = data.move(33, 30);
	data.command(0x2002, 2, 5, 255, 0x50, move_bank_model::condition);
	data.automatic(33, 30);
	data.end(0x8001, 0, 0);
	const auto entered = data.read(entry);
	check(entered && entered->free == 25u && entered->state == ready_state::stance, "a stance looping into itself is free where its inputs open");
	const auto own = data.read(stance);
	check(own && own->free == 5u && own->state == ready_state::stance, "and so is the stance committed itself");
}

PBD_TEST(PbdPracticeReadiness, ReaderAttackInputsNotEscapes)
{
	move_bank_model data;
	data.move(28, 20, 0x1052);
	data.end(0x8001, 20);
	data.attack(2, 39);
	data.end(0x8001, 30);
	// Moves ending on the floor, with an input on 12-20 into a part that can guard.
	const u32 string = data.attack(34, 50);
	data.command(0x00004000, 28, 12, 20, 0x50);
	data.end(0x8004, 30);
	const auto attack = data.read(string);
	check(attack && attack->state == ready_state::down && attack->free == 30u && !attack->escape && !attack->route_count,
		"an attack's own inputs are cancels and follow-ups, not escapes");
	const u32 reaction = data.move(35, 50);
	data.command(0x00004000, 28, 12, 20, 0x50);
	data.end(0x8004, 30);
	const auto escape = data.read(reaction, 1, contact::hit, true);
	check(escape && escape->state == ready_state::down && escape->free == 30u && escape->escape == 13u, "the same input in a reaction is an escape");
	const u32 counter = data.move(36, 50);
	data.command(0x00004000, 2, 12, 20, 0x50);
	data.end(0x8004, 30);
	check(data.read(counter, 1, contact::hit, true) && !data.read(counter, 1, contact::hit, true)->escape, "an input into an attack is not an escape");
	const u32 conditioned = data.move(38, 50);
	data.command(0x00004000, 28, 12, 20, 0x50, move_bank_model::condition);
	data.end(0x8004, 30);
	check(data.read(conditioned, 1, contact::hit, true) && !data.read(conditioned, 1, contact::hit, true)->escape,
		"an input on a game condition is not an escape");
	// A fighter that ends standing has no choice to make, so its inputs are not collected.
	const u32 standing = data.move(39, 50);
	data.command(0x00004000, 28, 12, 20, 0x50);
	data.end(0x8001, 30);
	check(data.read(standing, 1, contact::hit, true) && !data.read(standing, 1, contact::hit, true)->escape, "escapes only on the floor or in a stun");
}

PBD_TEST(PbdPracticeReadiness, ReaderOwnInputs)
{
	move_bank_model data;
	data.attack(2, 39);
	data.end(0x8001, 30);
	// A sway committed by the fighter: its own attacks open on cursor 5, its plain end is on 30.
	const u32 sway = data.move(36, 40, 0, 0, 0, 0x217);
	data.command(0x2002, 2, 5, 255, 0x50);
	data.end(0x8001, 30);
	const auto own = data.read(sway);
	check(own && own->free == 30u && own->own == 5u, "the fighter's own commit notes where its inputs free it");
	const auto later = data.read(sway, 8);
	check(later && later->own == 8u, "read on a later cursor, from there");
	const auto struck = data.read(sway, 1, contact::hit, true);
	check(struck && struck->free == 30u && !struck->own, "a reaction's own inputs do not count");
}

PBD_TEST(PbdPracticeReadiness, ClassifyWindowNotLevel)
{
	using pbd::practice::frames::attack_window;
	using pbd::practice::frames::throw_hold;
	check(!attack_window(0, 0, 40) && attack_window(10, 12, 40) && !attack_window(12, 10, 40) && !attack_window(10, 50, 40),
		"a move is an attack exactly when its window exists");
	check(throw_hold(0x0a00) && throw_hold(0x12340a00) && !throw_hold(0x217) && !throw_hold(0), "a 0x0A00 level marks the thrower's half");
	// Level 0x217 with window 0-0 (a sway, a roll) reads as a non-attack, whose own inputs free it; with a window the same
	// list is an attack's follow-ups.
	move_bank_model data;
	data.attack(2, 39);
	data.end(0x8001, 30);
	const u32 sway = data.move(37, 50, 0, 0, 0, 0x217);
	data.command(0x2002, 2, 12, 20, 0x50);
	data.end(0x8001, 30);
	check(data.read(sway) && data.read(sway)->own == 12u, "a level without a window is no attack");
	const u32 hit = data.move(38, 50, 0, 10, 12, 0x217);
	data.command(0x2002, 2, 12, 20, 0x50);
	data.end(0x8001, 30);
	check(data.read(hit) && !data.read(hit)->own, "a window makes it an attack");
}

PBD_TEST(PbdPracticeReadiness, HitLevelText)
{
	using pbd::practice::frames::hit_level_text;
	check(hit_level_text(0x0512) == "High" && hit_level_text(0x0217) == "Mid" && hit_level_text(0x010f) == "Low",
		"the three plain levels of the move banks");
	check(hit_level_text(0x031f) == "Special Mid" && hit_level_text(0x041f) == "Special Low", "both guards stop the special levels");
	check(hit_level_text(0x0707) == "Unblockable" && hit_level_text(0x0a00) == "Throw", "an unblockable and a caught throw");
	check(hit_level_text(0x00400512) == "High" && hit_level_text(0x00200217) == "Mid",
		"only the low half is the level; the word's high half carries other move flags");
	check(hit_level_text(0x0806) == "Unblockable" && hit_level_text(0x0907) == "Unblockable",
		"a level with no guard bit is one no guard stops");
	check(hit_level_text(0).empty() && hit_level_text(0x061f).empty() && hit_level_text(0xffff).empty(),
		"no level, an unmapped guarded level and a failed read show nothing");
}

PBD_TEST(PbdPracticeReadiness, HitLevelFollowsTheContact)
{
	calculator sample;
	sample.commit_reading(1, 900, 1, 1, plain_reading(1), false);
	sample.commit_reading(0, 130, 100, 1, plain_reading(30), true, 12, 10);
	check(!sample.latest[0].level, "an attack that has not connected has no level");
	sample.applied_contact(0, contact::block, 110, pbd::practice::frames::outcome_label::none, 0x0217);
	check(sample.latest[0].level == 0x0217u && sample.latest[0].contacted, "the contact keeps the level the contact check read");
	// The next hit of a string is a new move; its contact replaces the level, as a low after a mid.
	sample.commit_reading(0, 131, 112, 1, plain_reading(30), true, 8, 6);
	check(!sample.latest[0].level, "a new attack starts without a level");
	sample.applied_contact(0, contact::hit, 118, pbd::practice::frames::outcome_label::none, 0x010f);
	check(sample.latest[0].level == 0x010fu, "the latest contact's level");
	// A caught throw: the thrower's half commits as an attack, then the throw is a throw however the attempt read.
	sample.commit_reading(0, 132, 150, 1, plain_reading(60), true);
	sample.throw_connected(0);
	check(sample.latest[0].level == pbd::practice::frames::throw_level && sample.latest[0].label == pbd::practice::frames::outcome_label::throw_hit,
		"a caught throw reads Throw");
	sample.reset();
	check(!sample.latest[0].level && !sample.latest[1].level, "a restart clears it");
}

PBD_TEST(PbdPracticeReadiness, SwayCancelFromMoveData)
{
	// ss+1 read from move data, blocked at -10, then cancelled into a sway whose own attacks open on its cursor 1: the
	// sway's reading replaces the attacker's frames.
	move_bank_model data;
	data.attack(2, 39);
	data.end(0x8001, 30);
	const u32 attack = data.attack(3, 60);
	data.end(0x8001, 40);
	const u32 sway = data.move(4, 40);
	data.command(0x2002, 2, 1, 255, 0x50);
	data.end(0x8001, 30);
	for (const u32 delay : {7u, 10u})
	{
		calculator sample;
		sample.commit_reading(1, 900, 20, 1, plain_reading(1), false);
		sample.commit_reading(0, attack, 100, 1, data.read(attack), true, 12, 10);
		sample.applied_contact(0, contact::block, 111);
		sample.reread(0, attack, data.read(attack, 12, contact::block));
		sample.commit_reading(1, 220, 112, 1, plain_reading(18), false);
		check(sample.latest[0].advantage == -10, "the move's own value first");
		sample.commit_reading(0, sway, 111 + delay, 1, data.read(sway), false);
		const auto& out = sample.latest[0];
		check(out.advantage == (delay == 7 ? 11 : 8) && out.attacker_state == ready_state::stance && out.move == attack && out.startup == 10u,
			"the sway's own inputs give the new number; a later cancel a worse one");
	}
}

PBD_TEST(PbdPracticeReadiness, SwayCancel)
{
	// ss+1 blocked at -10 (free on cursor 40 = frame 139, the block reaction on 129), cancelled into a sway seven frames
	// after the contact: the sway is free on its cursor 1, so +11.
	for (const u32 delay : {7u, 10u})
	{
		calculator sample;
		sample.commit_reading(1, 900, 20, 1, plain_reading(1), false);
		sample.commit_reading(0, 120, 100, 1, plain_reading(40), true, 12, 15);
		sample.applied_contact(0, contact::block, 111);
		sample.commit_reading(1, 220, 112, 1, plain_reading(18), false);
		check(sample.latest[0].advantage == -10, "the move's own value first");
		sample.commit_reading(0, 121, 111 + delay, 1, plain_reading(1, ready_state::stance), false);
		const auto& out = sample.latest[0];
		if (delay == 7)
		{
			check(out.advantage == 11 && out.attacker_state == ready_state::stance,
				"the cancel replaces the attacker's frames");
			check(out.move == 120u && out.startup == 15u, "the attack stays the one shown, with its startup");
			sample.commit_reading(0, 122, 122, 1, plain_reading(20), true, 10, 12);
			check(sample.latest[0].move == 122u && sample.latest[0].startup == 12u, "an attack four frames later has its own startup");
		}
		else
			check(out.advantage == 8, "a later cancel gives a worse number");
	}
}

PBD_TEST(PbdPracticeReadiness, QueuedCancelUsesMoveDataBeforeCommit)
{
	move_bank_model data;
	data.attack(2, 39);
	data.end(0x8001, 30);
	const u32 attack = data.attack(3, 60);
	data.end(0x8001, 40);
	const u32 sway = data.move(4, 40);
	data.command(0x2002, 2, 1, 255, 0x50);
	data.end(0x8001, 30);
	const auto queued = data.queued(sway, 19, 0x80);
	check(queued && queued->cursor == 1 && queued->readiness.own == 1u, "the queued sway starts at one and can act in its own stance at one");
	calculator sample;
	sample.commit_reading(1, 900, 20, 1, plain_reading(1), false);
	sample.commit_reading(0, attack, 100, 1, data.read(attack), true, 12, 10);
	sample.applied_contact(0, contact::block, 111);
	sample.reread(0, attack, data.read(attack, 12, contact::block));
	sample.commit_reading(1, 220, 112, 1, plain_reading(18), false);
	check(sample.displayed()[0].advantage == -10, "ss+1 initially shows its own block value");
	sample.preview_cancel(0, attack, sway, 115, 118, queued->cursor, queued->readiness);
	check(sample.latest[0].advantage == -10, "a queued input does not rewrite committed timing");
	const auto preview = sample.displayed()[0];
	check(preview.advantage == 11 && preview.move == attack && preview.startup == 10u && preview.attacker_state == ready_state::stance,
		"the accepted input shows the sway's advantage immediately and retains the attack's identity and startup");
	sample.boundary(115);
	sample.check_queue(0, attack, sway);
	sample.preview_cancel(0, attack, sway, 116, 118, queued->cursor, queued->readiness);
	check(sample.displayed()[0].revision == preview.revision, "waiting in the same queue does not invent new input events");
	// The actual commit is authoritative, including when it arrives later than projected.
	sample.commit_reading(0, sway, 121, 1, data.read(sway), false);
	check(sample.latest[0].advantage == 8 && sample.displayed()[0].advantage == 8, "the actual commit replaces the preview with its real frame");
}

PBD_TEST(PbdPracticeReadiness, QueuedCancelCanBeReplacedClearedOrInterrupted)
{
	const auto start = []
	{
		calculator sample;
		sample.commit_reading(1, 900, 20, 1, plain_reading(1), false);
		sample.commit_reading(0, 120, 100, 1, plain_reading(40), true, 12, 15);
		sample.applied_contact(0, contact::block, 111);
		sample.commit_reading(1, 220, 112, 1, plain_reading(18), false);
		return sample;
	};
	auto sample = start();
	sample.preview_cancel(0, 120, 121, 115, 118, 1, plain_reading(1, ready_state::stance));
	const auto first_revision = sample.displayed()[0].revision;
	sample.preview_cancel(0, 120, 122, 116, 120, 1, plain_reading(3, ready_state::stance));
	check(sample.displayed()[0].advantage == 7 && sample.latest[0].advantage == -10, "a replacement queue replaces only the preview");
	sample.check_queue(0, 120, 0);
	check(sample.displayed()[0].advantage == -10 && sample.displayed()[0].revision > first_revision, "clearing the queue restores the original displayed number");
	sample.preview_cancel(0, 120, 121, 115, 118, 1, plain_reading(1, ready_state::stance));
	auto restored = sample;
	check(restored.displayed()[0].advantage == 11, "a saved frame retains its queue preview");
	restored.check_queue(0, 120, 122);
	check(restored.displayed()[0].advantage == -10 && sample.displayed()[0].advantage == 11, "restored queues can change independently");
	sample.boundary(118);
	check(sample.displayed()[0].advantage == 11, "the projection remains through its expected commit frame");
	sample.boundary(120);
	check(sample.displayed()[0].advantage == -10, "a queue that never commits cannot leave a permanent preview");
	sample = start();
	sample.preview_cancel(0, 120, 121, 115, 118, 1, plain_reading(1, ready_state::stance));
	sample.applied_contact(1, contact::hit, 116);
	check(sample.displayed()[0].advantage == sample.latest[0].advantage, "a hit interrupting the cancel removes its preview");
	sample = start();
	sample.preview_cancel(0, 120, 121, 115, 118, 1, plain_reading(1, ready_state::stance));
	sample.reset();
	check(!sample.displayed()[0].move && !sample.displayed()[0].advantage, "Restart removes the queued cancel and its number");
	for (const auto [source, now, commits] : std::array<std::array<u32, 3>, 3>{{{119, 115, 118}, {120, 140, 140}, {120, 119, 118}}})
	{
		sample = start();
		sample.preview_cancel(0, source, 121, now, commits, 1, plain_reading(1));
		check(sample.displayed()[0].advantage == -10, "a stale source, an input outside recovery or an execution before acceptance cannot preview");
	}
}

PBD_TEST(PbdPracticeReadiness, QueuedCancelKeepsCrumpleAndBreakValues)
{
	calculator sample;
	sample.commit_reading(1, 900, 1, 1, plain_reading(1), false);
	sample.commit_reading(0, 140, 200, 1, plain_reading(48), true, 15, 13);
	sample.applied_contact(0, contact::hit, 213);
	sample.commit_reading(1, 240, 214, 1, choice_reading(59, ready_state::stun, 800, 20, 20, 50), false);
	check(sample.latest[0].advantage == 25 && sample.latest[0].alternative == 6, "the full crumple remains primary before the input");
	sample.preview_cancel(0, 140, 141, 236, 240, 1, plain_reading(2, ready_state::stance));
	const auto preview = sample.displayed()[0];
	check(preview.advantage == 31 && preview.alternative == 12 && preview.defender_chooses && preview.taken == branch::none,
		"the cancel recalculates both crumple paths without claiming a break has happened");
	check(sample.latest[0].advantage == 25 && sample.latest[0].alternative == 6, "the original readings remain available if the queue is discarded");
	sample.commit_reading(0, 141, 240, 1, plain_reading(2, ready_state::stance), false);
	check(sample.displayed()[0].advantage == preview.advantage && sample.latest[0].alternative == preview.alternative,
		"the matching actual commit confirms both preview numbers");
}

PBD_TEST(PbdPracticeReadiness, QueuedCancelUpdatesWhiffWithoutConsumingTheMove)
{
	calculator sample;
	sample.commit_reading(0, 120, 100, 1, plain_reading(40), true, 12, 15);
	sample.boundary(112);
	check(sample.latest[0].whiff == -29, "the uncancelled whiff uses the attack's original recovery");
	sample.preview_cancel(0, 120, 121, 115, 118, 1, plain_reading(1, ready_state::stance));
	check(sample.displayed()[0].whiff == -8 && sample.latest[0].whiff == -29 && sample.displayed()[0].startup == 15u,
		"the accepted cancel updates whiff recovery immediately while preserving committed timing and startup");
	sample.preview_cancel(0, 120, 121, 115, 118, 1, {});
	check(sample.displayed()[0].whiff == -29, "an unread queued target cannot leave an invented recovery");
	sample.preview_cancel(0, 120, 121, 115, 118, 1, plain_reading(1, ready_state::stance));
	sample.commit_reading(0, 121, 118, 1, plain_reading(1, ready_state::stance), false);
	check(sample.latest[0].whiff == -8 && sample.displayed()[0].whiff == -8, "the committed cancel confirms the whiff preview");
}

PBD_TEST(PbdPracticeReadiness, QueuedEntryCursorFollowsPreparedTransition)
{
	move_bank_model data;
	data.attack(2, 39);
	data.end(0x8001, 30);
	const u32 sway = data.move(4, 40);
	data.command(0x2002, 2, 1, 255, 0x50);
	data.end(0x8001, 35);
	for (const auto [parameter, cursor] : std::array<std::array<u32, 2>, 5>{{{0x80, 1}, {0x402, 19}, {0x1400, 19}, {0x2000, 18}, {0x3000, 18}}})
	{
		const auto queued = data.queued(sway, 18, parameter);
		check(queued && queued->cursor == cursor && queued->readiness.own == cursor, "the prepared entry preserves the same cursor that the native transition will commit");
	}
	check(!data.queued(sway + 1, 18, 0x80) && !data.queued(sway, 0, 0x80) && !data.queued(sway, 40000, 0x80),
		"invalid queue targets and cursors stay unavailable");
}

PBD_TEST(PbdPracticeReadiness, RecoveryEscapeIsCancel)
{
	// The sway input sits on the recovery's next part, so the reading lists it as an escape; the fighter stands, so the
	// input is still a cancel that replaces the attacker's frames.
	calculator sample;
	sample.commit_reading(1, 900, 20, 1, plain_reading(1), false);
	sample.commit_reading(0, 120, 100, 1, choice_reading(40, ready_state::stand, 121, 20, 1, 30), true, 12, 15);
	sample.applied_contact(0, contact::block, 111);
	sample.commit_reading(1, 220, 112, 1, plain_reading(18), false);
	check(sample.latest[0].advantage == -10 && !sample.latest[0].alternative, "a standing fighter shows no alternative");
	sample.commit_reading(0, 121, 118, 1, plain_reading(1, ready_state::stance), false);
	check(sample.latest[0].advantage == 11 && sample.latest[0].taken == branch::none, "its escape input is a cancel");
}
PBD_TEST(PbdPracticeReadiness, AttackerTech)
{
	// A jumping attack blocked: it lands down on frame 129 (+15), or techs to be free on 135 (+9). The tech can show on
	// frames 132 and 133.
	const auto setup = [](calculator& sample)
	{
		sample.commit_reading(1, 900, 1, 1, plain_reading(1), false);
		auto landing = choice_reading(30, ready_state::down, 700, 32, 4, 34);
		landing.path[0] = 701;
		landing.path_count = 1;
		sample.commit_reading(0, 130, 100, 1, landing, true, 12, 10);
		sample.applied_contact(0, contact::block, 110);
		sample.commit_reading(1, 230, 111, 1, plain_reading(34), false);
	};
	calculator teched;
	setup(teched);
	check(teched.latest[0].advantage == 15 && teched.latest[0].alternative == 9 && teched.latest[0].taken == branch::none &&
			  teched.latest[0].attacker_state == ready_state::down,
		"staying down is the primary, the tech the alternative");
	teched.commit_reading(0, 701, 130, 1, plain_reading(50, ready_state::down), false);
	check(teched.latest[0].advantage == 15 && teched.latest[0].taken == branch::none, "landing is the plain path, not a cancel");
	teched.commit_reading(0, 700, 132, 1, plain_reading(10), false);
	check(teched.latest[0].taken == branch::alternative && teched.latest[0].advantage == 15 && teched.latest[0].alternative == 9,
		"a tech commit takes the alternative and keeps the primary");
	calculator late;
	setup(late);
	late.commit_reading(0, 700, 133, 1, plain_reading(10), false);
	check(late.latest[0].taken == branch::alternative && late.latest[0].alternative == 8, "the alternative counts from the real commit");
	calculator stayed;
	setup(stayed);
	stayed.commit_reading(0, 701, 140, 1, plain_reading(50, ready_state::down), false);
	check(stayed.latest[0].taken == branch::primary && stayed.latest[0].advantage == 15 && stayed.latest[0].alternative == 9,
		"without the tech, the plain path's next move takes the primary");
	calculator waited;
	setup(waited);
	waited.boundary(133);
	check(waited.latest[0].taken == branch::none, "the tech can still come on the window's last frame");
	waited.boundary(134);
	check(waited.latest[0].taken == branch::primary, "once the window is over the primary was taken");
}

namespace
{
	// CH d/f+1+2 into a crumple: the attack is free on frame 247; the stun (reaction origin 213) ends on 263 (+16), the
	// fastest break leaves the defender able to guard on 233 (-14).
	void crumple_setup(calculator& sample)
	{
		sample.commit_reading(1, 900, 1, 1, plain_reading(1), false);
		sample.commit_reading(0, 140, 200, 1, plain_reading(48), true, 15, 13);
		sample.applied_contact(0, contact::hit, 213);
		auto stun = choice_reading(50, ready_state::stun, 800, 20, 0, 23);
		stun.path[0] = 801;
		stun.path_count = 1;
		sample.commit_reading(1, 240, 214, 1, stun, false);
	}
} // namespace

PBD_TEST(PbdPracticeReadiness, CrumpleBreak)
{
	calculator broken;
	crumple_setup(broken);
	check(broken.latest[0].advantage == 16 && broken.latest[0].alternative == -14 && broken.latest[0].defender_chooses &&
			  broken.latest[0].defender_state == ready_state::stun,
		"the full stun is the primary, the fastest break the alternative");
	broken.commit_reading(1, 800, 234, 1, plain_reading(20, ready_state::stance), false);
	check(broken.latest[0].taken == branch::alternative && broken.latest[0].advantage == 16 && broken.latest[0].alternative == -14,
		"a break on reaction cursor 21 is marked, and the primary stays");
	calculator held;
	crumple_setup(held);
	held.commit_reading(1, 801, 264, 1, plain_reading(40, ready_state::down), false);
	check(held.latest[0].taken == branch::primary && held.latest[0].advantage == 16 && held.latest[0].alternative == -14,
		"without the break the collapse leaves both numbers");
}

PBD_TEST(PbdPracticeReadiness, CancelAfterCrumple)
{
	calculator sample;
	crumple_setup(sample);
	sample.commit_reading(0, 141, 240, 1, plain_reading(2, ready_state::stance), false);
	check(sample.latest[0].advantage == 22 && sample.latest[0].alternative == -8 && sample.latest[0].attacker_ready == 241,
		"a cancel recalculates both from the defender's absolute frames");
	check(sample.latest[0].defender_state == ready_state::stun && sample.latest[0].defender_chooses, "the defender's crumple stays");
	calculator realized;
	crumple_setup(realized);
	realized.commit_reading(1, 800, 235, 1, plain_reading(20, ready_state::stance), false);
	realized.commit_reading(0, 141, 240, 1, plain_reading(2, ready_state::stance), false);
	check(realized.latest[0].taken == branch::alternative && realized.latest[0].alternative == -7,
		"a break already taken keeps its real frame through the cancel");
}

PBD_TEST(PbdPracticeReadiness, ContactRewalk)
{
	// The attack's block branch is shorter than the whiff branch it was read with at its commit.
	move_bank_model data;
	const u32 gated = data.attack(19, 60);
	data.automatic(20, 15, move_bank_model::on_hit);
	data.automatic(21, 16, move_bank_model::on_block);
	data.automatic(22, 17, move_bank_model::on_whiff);
	data.end(0x8001, 40);
	data.move(20, 20);
	data.end(0x8001, 5);
	data.move(21, 20);
	data.end(0x8001, 6);
	data.move(22, 20);
	data.end(0x8001, 7);
	calculator sample;
	sample.commit_reading(1, 900, 1, 1, plain_reading(1), false);
	sample.commit_reading(0, gated, 300, 1, data.read(gated), true, 12, 10);
	check(sample.latest[0].ready == 323, "read at its commit, the attack takes the whiff branch");
	sample.applied_contact(0, contact::block, 310);
	sample.reread(0, gated, data.read(gated, 11, contact::block));
	sample.commit_reading(1, 250, 311, 1, plain_reading(20), false);
	check(sample.latest[0].attacker_ready == 321 && sample.latest[0].advantage == 9, "read again at the contact, the block branch counts");
}

PBD_TEST(PbdPracticeReadiness, StanceAttackObservedFree)
{
	calculator sample;
	sample.commit_reading(1, 900, 20, 1, plain_reading(1), false);
	sample.commit_reading(0, 120, 100, 1, plain_reading(40), true, 12, 15);
	sample.applied_contact(0, contact::block, 111);
	sample.commit_reading(1, 220, 112, 1, plain_reading(18), false);
	sample.commit_reading(0, 121, 118, 1, plain_reading(5, ready_state::stance), false);
	check(sample.latest[0].attacker_ready == 122, "the stance's own reading");
	sample.commit_reading(0, 123, 120, 1, plain_reading(20), true, 10, 11);
	check(sample.latest[0].move == 123u && sample.latest[0].startup == 11u,
		"an attack out of the stance before its predicted frame is a new move, not the string's next hit");
	// A defender knocked into a stance that attacks out of it early was free earlier too.
	calculator defender;
	defender.commit_reading(1, 900, 1, 1, plain_reading(1), false);
	defender.commit_reading(0, 130, 200, 1, plain_reading(30), true, 12, 10);
	defender.applied_contact(0, contact::hit, 210);
	defender.commit_reading(1, 230, 211, 1, plain_reading(30, ready_state::stance), false);
	check(defender.latest[0].advantage == 11, "the defender's stance reading");
	defender.commit_reading(1, 231, 231, 1, plain_reading(20), true, 10, 10);
	check(defender.latest[0].defender_ready == 230 && defender.latest[0].advantage == 1, "its attack shows it was free the frame before");
}

PBD_TEST(PbdPracticeReadiness, WhiffWithAlternative)
{
	calculator sample;
	sample.commit_reading(1, 900, 1, 1, plain_reading(1), false);
	sample.commit_reading(0, 160, 400, 1, choice_reading(40, ready_state::down, 700, 42, 4, 43), true, 12, 10);
	sample.boundary(411);
	check(!sample.latest[0].whiff, "nothing while the attack can still connect");
	sample.boundary(412);
	check(sample.latest[0].whiff == -29 && sample.latest[0].whiff_alternative == -35 && sample.latest[0].state == ready_state::down,
		"a whiff shows its recovery through both branches");
	sample.commit_reading(0, 700, 442, 1, plain_reading(4), false);
	check(sample.latest[0].taken == branch::alternative && sample.latest[0].whiff == -29 && sample.latest[0].whiff_alternative == -35,
		"and which one was taken");
}

PBD_TEST(PbdPracticeReadiness, DisplayText)
{
	using pbd::practice::frames::advantage_text;
	check(advantage_text(0, {}, std::nullopt, branch::none).text == "+0", "even is signed");
	check(advantage_text(-10, {}, std::nullopt, branch::none).text == "-10", "a plain number");
	const auto crumple = advantage_text(16, "Crumple", -14, branch::none);
	check(crumple.text == "+16 Crumple (-14)" && !crumple.marked_size && crumple.logged == crumple.text, "the stun with its fastest break");
	check(advantage_text(15, "Down", 9, branch::none).text == "+15 Down (+9)", "the attacker's tech");
	const auto broken = advantage_text(16, "Crumple", -14, branch::alternative);
	check(broken.text == "+16 Crumple (-14)" && broken.marked_at == 13 && broken.marked_size == 3 && broken.logged == "+16 Crumple (-14*)",
		"the taken alternative is marked");
	const auto stayed = advantage_text(16, "Crumple", -14, branch::primary);
	check(stayed.marked_at == 0 && stayed.marked_size == 3 && stayed.logged == "+16* Crumple (-14)", "so is a taken primary");
	check(advantage_text(-100, "Crumple", -100, branch::none).text.size() <= pbd::practice::frames::advantage_text_limit &&
			  advantage_text(-100, "Crumple", -100, branch::none).text == "-100 (-100)",
		"the word goes before the text runs long");
	pbd::practice::frames::calculated knocked;
	knocked.advantage = 40;
	knocked.alternative = 30;
	knocked.defender_state = ready_state::down;
	knocked.defender_chooses = true;
	check(advantage_text(knocked).text == "+40 KND (+30)" && advantage_text(knocked, "Launch").text == "+40 Launch (+30)",
		"an outcome label replaces the word and keeps both numbers");
	pbd::practice::frames::calculated missed;
	check(pbd::practice::frames::whiff_text(missed).text.empty(), "a whiff without a recovery has no text");
	missed.whiff = -29;
	missed.whiff_alternative = -35;
	missed.state = ready_state::down;
	missed.taken = branch::alternative;
	const auto whiff = pbd::practice::frames::whiff_text(missed);
	check(whiff.text == "-29 Down (-35)" && whiff.logged == "-29 Down (-35*)" &&
			  whiff.text.substr(whiff.marked_at, whiff.marked_size) == "-35",
		"whiff text carries both recoveries and marks the taken branch");
	// The panel draws the text with its mark; without a text it shows the plain signed value.
	pbd::practice::feedback value;
	value.on_hit = 16;
	value.advantage_text = broken.text;
	value.advantage_marked_at = broken.marked_at;
	value.advantage_marked_size = broken.marked_size;
	const auto lines = pbd::practice::frame_data_text(value);
	check(lines[1].value == broken.text && lines[1].marked_at == broken.marked_at && lines[1].marked_size == broken.marked_size,
		"the panel shows the text and its mark");
}
