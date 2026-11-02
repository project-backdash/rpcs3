#pragma once

#include "pbd_t5dr_memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

// Throw breaks and every other escape at offline timing in online play.
//
// How the game reads a fighter's input. Each battle update (0x15458) first commits queued moves and advances every
// fighter's frame (0x17744 -> 0xf1a28), then runs each fighter's input step (0x13d2c -> 0xecb58), then each fighter's
// cancel scan (0x19900 -> 0x1958c -> 0xf0594). A human's step loads the held mask PBD's input update stored in the port's
// slot (0x10099fb4 + 4 * port, at 0xed1e0) and +0x6c4, the previous step's held mask or 0x7fffffff on a frame the game
// marks as having no fresh presses (0xed1a0); pressed is held without +0x6c4. From those two the step builds everything
// the scan and the moves read: the break latch +0x204, the command fields +0x6ba (buttons) and +0x6bc/+0x6be (the held and
// the changed direction), the per-port command history, and +0x6c4 for the next step (0xecdfc). The scan matches each
// cancel of the current move against them.
//
// A grab (0xf015c, every class-0x1800 hold) gives the held fighter a move from the holder's moveset: +0xbc, the moveset
// of the current move, then differs from +0x50, the fighter's own. Every escape of a hold (breaks, reversals, mashes,
// chickens, the Ganryu and tackle escapes, the mount guard, a just-frame block) is a cancel of such a move.
//
// With input delay D, tick t runs the word sampled D ticks earlier (S), so an escape window loses the presses of its last
// D frames. The held fighter's own word L = D ticks ahead (A) is what the player presses at tick t, as offline. The rule
// moves only the held fighter's escape presses, so that every escape ends as it would offline for the same presses:
// - K(t), the buttons the held fighter's escapes read at this frame, from move data and the hold's state alone: the
//   current move's escape cancels (a move's break windows as one, since the latch runs between them; a sequence's window
//   from the oldest frame its command history reaches in the move), and all four buttons while the hold can go on (the
//   fighter runs the reaction the holder's current move gave it, and that move can still start another pair), since a
//   sequence of a later hold reads presses made before it.
// - A press of a button in K seen in A fires at once and is used up: when its copy arrives in S, L ticks later, it shows as
//   held but never fresh, so it acts once (no 1+2 attack after a 1+2 break, no second count in a mash). A button of a
//   window that A holds where a press of a window fires, pressed before its window and not in S yet, is used up with it:
//   offline it went fresh before the window and is held now (1 held, then 2, is 1+2).
// - A press whose copy reaches S where a window other than a sequence reads its button was made too early for it, before
//   that window opened: it misses, as offline (held, never fresh), so a wrong button pressed early locks nothing out. So
//   does one whose copy reaches S while the hold can go on: made before anything carried it, it would enter the command
//   history after presses made later.
// - Every other press fires when it arrives in S, as today: one made outside every window counts at the normal delay.
// - While a button is in K its held state comes from A, as offline, but on the step a press from S fires in K.
// - Directions are held states, never used up: while a window reads them, the scan alone sees the held and changed
//   direction of A (written before the scan and put back after it); the word, the history and +0x6c4 keep S's.
// The game builds every input field from the slot and +0x6c4 itself. The holder is never moved, nor anything after the
// held fighter commits a move of its own (an escape, a restart, the fall that ends a throw) outside a window, nor tech
// rolls (landing options), which keep the delay.
namespace pbd::throw_break
{
	using u8 = std::uint8_t;
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;

	// The probes. The actor is in r31 at all four. The word load (0xed1d8) comes after the no-fresh mark
	// (0xed1a0) and before the slot load (0xed1e0) and the +0x6c4 load (0xed1f4); the human step reaches it unless
	// 0x10425280 is 2 (0xed184) or the game mode is 5 (0xed1d4), which read other sources. The latch entry (0xecc94,
	// a byte read at +0x192) is passed once by every input step of every fighter, after the life gate (0xecc4c), with held in r7
	// and pressed in r6 (0xecc68..0xecc74). The scan entry (0x19708) and exit (0x19718, after the cancel scan)
	// bracket the fighter's cancel scan in 0x1958c, which runs after both input steps.
	inline constexpr u32 scan_entry = 0x19708;
	inline constexpr u32 scan_exit = 0x19718;
	inline constexpr u32 latch_entry = 0xecc94;
	inline constexpr u32 word_load = 0xed1d8;
	inline constexpr std::array<u32, 4> probes{scan_entry, scan_exit, latch_entry, word_load};

	constexpr bool probed(u32 pc) noexcept
	{
		return std::find(probes.begin(), probes.end(), pc) != probes.end();
	}

	// The held mask PBD's input update stores per port (mapped_mask of the injected word).
	inline constexpr u32 port_slots = 0x10099fb4;

	constexpr u32 slot_of(u16 port) noexcept
	{
		return port_slots + 4 * u32{port};
	}

	// +0x6c4 on a frame without fresh presses (0xed198..0xed1a0).
	inline constexpr u32 no_fresh_mark = 0x7fffffff;

	// The game mode word; Practice (10) blanks a step's word under its input display controls (0xed740).
	inline constexpr u32 game_mode = 0x1008ebac;
	inline constexpr u32 practice_mode = 10;

	// The four buttons in the word, as the command fields number them (1, 2, 4 and 8 in +0x6ba, 0xeccc0..0xecd4c), and the
	// directions (up 0x1000, right 0x2000, down 0x4000, left 0x8000).
	inline constexpr std::array<u16, 4> button_bits{0x0080, 0x0010, 0x0040, 0x0020};
	inline constexpr u16 escape_buttons = 0x00f0;
	inline constexpr u16 direction_bits = 0xf000;

	constexpr u16 word_buttons(u8 nibble) noexcept
	{
		u16 out = 0;
		for (u32 button = 0; button < 4; ++button)
			if (nibble & (1u << button))
				out = static_cast<u16>(out | button_bits[button]);
		return out;
	}

	// Start and Select are no fighter's input; masks applied after a word is frozen touch only them (session start, replay
	// controls), so live play, replays and watchers read the same buttons and directions ahead.
	inline constexpr u16 system_buttons = 0x0900;

	constexpr bool keeps_fight_buttons(u16 mask) noexcept
	{
		return (mask & ~system_buttons) == 0;
	}

	// The actor fields the rule reads, with the widths the game reads them.
	namespace actor
	{
		inline constexpr u32 port = 0x3c;               // u16
		inline constexpr u32 moveset = 0x50;            // u32, the fighter's own moveset
		inline constexpr u32 frame = 0x96;              // u16, the current move's frame (+0xb0 >> 12)
		inline constexpr u32 move_moveset = 0xbc;       // u32, the moveset the current move comes from (0xf015c on a grab)
		inline constexpr u32 move = 0xc4;               // u32, the current move record
		inline constexpr u32 controller = 0x1c0;        // u8, 0 for a human (byte read at 0xecb94); side 1 keeps 1 in the bytes after it
		inline constexpr u32 facing = 0x1c4;            // u8, which way the step turns the stick (byte read at 0xed1fc)
		inline constexpr u32 move_class = 0x15a;        // u16, the class word the current move was queued with (moves::extra_pair)
		inline constexpr u32 life = 0x6ac;              // u32, 16.16; the step zeroes held and pressed while it is 0
		inline constexpr u32 direction_held = 0x6bc;    // u16, 1 << the held direction's numpad digit
		inline constexpr u32 direction_changed = 0x6be; // u16, 1 << the digit it changed to, or 1 or 0x20
		inline constexpr u32 previous_held = 0x6c4;     // u32
	} // namespace actor

	// The fighter whose actor starts at `address`, or nothing for any other address.
	constexpr std::optional<u32> side_of(u32 address) noexcept
	{
		for (u32 side = 0; side < 2; ++side)
			if (address == pbd::replay_memory::actors + side * pbd::replay_memory::actor_stride)
				return side;
		return std::nullopt;
	}

	// The step's direction table (TOC - 0x561c, 0x1000765c): the numpad digits 1..9 of the held directions, per facing.
	inline constexpr std::array<std::array<u16, 9>, 2> numpad_table{{
		{0xc000, 0x4000, 0x6000, 0x8000, 0x0000, 0x2000, 0x9000, 0x1000, 0x3000},
		{0x6000, 0x4000, 0xc000, 0x2000, 0x0000, 0x8000, 0x3000, 0x1000, 0x9000},
	}};

	constexpr u32 numpad(u16 directions, u8 facing) noexcept
	{
		u32 digit = 5;
		for (u32 at = 0; at < 9; ++at)
			if (numpad_table[facing & 1][at] == (directions & direction_bits))
				digit = at + 1;
		return digit;
	}

	// The direction fields the step builds from what it held and pressed (0xed09c..0xed150, then 0xecc80, 0xecfbc and the
	// stores 0xecd50..0xecdf4), for a facing of 0 or 1. `previous` is +0x6c4 as the step loaded it (its low half, +0x6c6,
	// is the previous held mask) and `old_held` the +0x6bc the previous step left. Held is +0x6bc << 16 | +0x6be.
	constexpr u32 direction_fields(u16 held, u16 pressed, u32 previous, u16 old_held, u8 facing) noexcept
	{
		const u16 held_way = held & direction_bits, before = static_cast<u16>(previous) & direction_bits;
		const u16 changed_way = (pressed & direction_bits) ? static_cast<u16>((held | pressed) & direction_bits) : held_way == before ? u16{0} :
		                                                                                                                                held_way;
		const u32 digit = numpad(held_way, facing);
		u32 changed = numpad(changed_way, facing);
		if (changed == 5)
			changed = (old_held & 0x20) || digit != 5 ? 0 : 5;
		u32 held_field = 1u << digit, changed_field = 1u << changed;
		const u32 extra = (held & 0x4 ? 0x2000u : 0u) | (held & 0x8 ? 0x4000u : 0u) | (pressed & 0x4 ? 0x800u : 0u) | (pressed & 0x8 ? 0x1000u : 0u);
		held_field |= extra;
		changed_field |= extra;
		return held_field << 16 | (changed_field & 0xffff);
	}

	// Move data as a loaded bank holds it: relocated, every reference an absolute guest address.
	namespace moves
	{
		inline constexpr u32 sequence_table = 0x1f0; // moveset header: the input sequences, 8 bytes each (0xedf1c)
		inline constexpr u32 group_table = 0x1b8;    // moveset header: the group cancel table
		inline constexpr u32 cancels = 0x14;         // move record: its first cancel
		inline constexpr u32 cancel_stride = 0x18;
		inline constexpr u32 cancel_buttons = 0x02; // u16; the direction half is the cancel's first halfword
		inline constexpr u32 cancel_requirements = 0x04;
		inline constexpr u32 cancel_group = 0x08;                     // u16: a group jump's group index
		inline constexpr u32 cancel_extra = 0x0c;                     // u32: the address of the cancel's extra word
		inline constexpr u32 cancel_first = 0x10, cancel_last = 0x12; // u16 frames, tested inclusively (0xef860..0xef87c)
		inline constexpr u32 cancel_start = 0x14;                     // u16: the frame from which the target commits (+0x30e)
		inline constexpr u32 cancel_option = 0x16;                    // u16
		// The scan ends at 0x8000, jumps into group cancel list +0x08 at 0x8005 (0xef9cc) and returns after that jump at
		// the group's 0x8006 (0xef84c). From 0x8007 the direction half names an input sequence the matcher reads from the
		// command history (0xedf08): +0 frames, +2 count, +4 the presses, 4 bytes each with the buttons at +2 (0x4000: a
		// frame without presses).
		inline constexpr u16 cancel_end = 0x8000, group_jump = 0x8005, group_return = 0x8006, first_sequence = 0x8007;
		inline constexpr u16 sequence_gap = 0x4000;
		// Option 0x02 skips a cancel (0xef8a4); 0x01 matches it without reading input (0xedb4c). The loader sets 0x01 on
		// every cancel without a command, including the break cancels, whose requirement reads the latch instead.
		inline constexpr u16 option_skipped = 0x02, option_unread = 0x01;
		// An extra word of class 0 (bits 0x3c00; a grab is 0x1800, 0xf015c) with bit 0x10 marks a landing option: a tech
		// roll, or an action from the ground. They keep the delay. In none of the 2589 moves a held fighter can reach in the
		// 36 shipped movesets does one open within 6 frames after an escape window of the same move.
		inline constexpr u32 extra_class = 0x3c00, extra_landing = 0x10;
		// Class 0x1800 starts a pair. Every queue stores the queued move's class word in +0x310; the commit copies it to
		// +0x15a and clears +0x310 (0xf2784..0xf278c, 0x2a505c..0x2a5064), so only +0x15a describes the move a fighter runs.
		// The commit phase commits a pair's two moves together (0x17ae4..0x17b04): 0xf015c queues the partner's reaction with
		// the holder's moveset in +0xbc and the class word 0x1f57 (class 0x1c00, which no cancel of the shipped movesets
		// carries), then both commit. Until either fighter commits another move, the holder's +0x15a has class 0x1800 and
		// the partner's class 0x1c00.
		inline constexpr u32 extra_pair = 0x1800, extra_held = 0x1c00;
		inline constexpr u32 requirement_stride = 4; // u16 id, u16 parameter
		inline constexpr u16 requirement_end = 327;
		inline constexpr u16 break_one = 106, break_two = 107, break_both = 108;
		// No move in the 36 shipped movesets has more than 149 cancels or a requirement list longer than 16; a longer walk
		// reads something that is not a move.
		inline constexpr u32 max_cancels = 1024, max_requirements = 64, max_sequence = 64;

		constexpr bool is_break(u16 id) noexcept
		{
			return id >= break_one && id <= break_both;
		}

		// The buttons a command's button half reads (0xedbac..0xedd78): fresh, held and released nibbles, or all four for
		// the codes from 0x4000 on (nothing fresh, nothing held, and the rest).
		constexpr u8 code_buttons(u16 code) noexcept
		{
			if (!code)
				return 0;
			if (code >= 0x4000)
				return 0xf;
			return static_cast<u8>((code | code >> 4 | code >> 8) & 0xf);
		}
	} // namespace moves

	template <typename Byte>
	constexpr u16 read16(const Byte& byte, u32 address)
	{
		return static_cast<u16>(byte(address) << 8 | byte(address + 1));
	}

	template <typename Byte>
	constexpr u32 read32(const Byte& byte, u32 address)
	{
		return u32{read16(byte, address)} << 16 | read16(byte, address + 2);
	}

	// One frame range of a move in which cancels read input: the buttons they read (1, 2, 4 and 8), whether they read the
	// directions, whether they are break cancels (106-108, which read the latch), input sequences (read from the command
	// history) or landing options.
	struct escape_window
	{
		u16 first = 0, last = 0;
		u8 buttons = 0;
		bool directions = false, breaks = false, landing = false, sequence = false;
	};

	// What one move's cancels read, group cancels included. Cancels with the same frames and kind share a window. A move's
	// break cancels share one window, from the first one's start to the last one's end: they read the latch (+0x204), which
	// the step rewrites every frame until a break check has set +0x192 and a press has set +0x204, and +0x192 lasts until the
	// next move commit (0xf1b98), so in a gap between two break windows (a chain stage's 1..15 and 91..99) the first press
	// still decides the later window. A sequence's window starts where its frames (sequence +0) reach back from its first
	// frame, or at frame 1: a press there counts for it from the command history. `follow_up` is the last frame at which the
	// move can still start a pair (class 0x1800): the last frame of such a cancel, or the frame before its target commits.
	struct move_span
	{
		bool readable = false; // false: a list ran past its bound, a group jump did not return, or too many windows
		u8 count = 0;
		u16 follow_up = 0;
		std::array<escape_window, 24> windows{};
	};

	template <typename Byte>
	constexpr move_span span_of_move(const Byte& byte, u32 move, u32 moveset)
	{
		move_span span;
		u32 cancel = read32(byte, move + moves::cancels);
		std::optional<u32> jump;
		for (u32 count = 0; count <= moves::max_cancels; ++count)
		{
			const u16 head = read16(byte, cancel);
			if (head == moves::cancel_end)
			{
				span.readable = true;
				return span;
			}
			if (head == moves::group_jump)
			{
				if (jump)
					return {};
				jump = cancel;
				cancel = read32(byte, moveset + moves::group_table) + moves::cancel_stride * read16(byte, cancel + moves::cancel_group);
				continue;
			}
			if (head == moves::group_return)
			{
				if (!jump)
					return {};
				cancel = *jump + moves::cancel_stride;
				jump.reset();
				continue;
			}
			u32 requirement = read32(byte, cancel + moves::cancel_requirements);
			bool breaks = false;
			for (u32 at = 0; read16(byte, requirement) != moves::requirement_end; ++at, requirement += moves::requirement_stride)
			{
				if (at == moves::max_requirements)
					return {};
				breaks = breaks || moves::is_break(read16(byte, requirement));
			}
			const u16 option = read16(byte, cancel + moves::cancel_option), code = read16(byte, cancel + moves::cancel_buttons);
			const u32 extra = read32(byte, cancel + moves::cancel_extra);
			const u32 kind = extra ? read32(byte, extra) : 0;
			if (!(option & moves::option_skipped) && (kind & moves::extra_class) == moves::extra_pair)
			{
				const u16 start = read16(byte, cancel + moves::cancel_start);
				span.follow_up = std::max({span.follow_up, read16(byte, cancel + moves::cancel_last), static_cast<u16>(start ? start - 1 : 0)});
			}
			const bool command = head != 0 || code != 0;
			if (!(option & moves::option_skipped) && (breaks || (command && !(option & moves::option_unread))))
			{
				escape_window window;
				window.first = read16(byte, cancel + moves::cancel_first);
				window.last = read16(byte, cancel + moves::cancel_last);
				window.breaks = breaks;
				if (breaks)
					window.buttons = 0x3;
				else if (head >= moves::first_sequence)
				{
					const u32 sequence = read32(byte, moveset + moves::sequence_table) + 8 * u32{static_cast<u16>(head - moves::first_sequence)};
					const u32 frames = read16(byte, sequence), presses = read16(byte, sequence + 2), list = read32(byte, sequence + 4);
					if (presses > moves::max_sequence)
						return {};
					for (u32 at = 0; at < presses; ++at)
						if (const u16 buttons = read16(byte, list + 4 * at + 2); buttons != moves::sequence_gap)
							window.buttons |= moves::code_buttons(buttons);
					// The matcher takes the earlier presses from the frames - 1 history entries before the newest.
					const u32 back = frames ? frames - 1 : 0;
					window.sequence = true;
					window.first = static_cast<u16>(window.first > back ? window.first - back : std::min<u32>(window.first, 1));
				}
				else
				{
					window.buttons = moves::code_buttons(code);
					window.directions = head != 0;
				}
				window.landing = !breaks && !(kind & moves::extra_class) && (kind & moves::extra_landing);
				escape_window* same = nullptr;
				for (u32 at = 0; at < span.count; ++at)
				{
					escape_window& each = span.windows[at];
					if (window.breaks ? each.breaks :
										each.first == window.first && each.last == window.last && each.directions == window.directions &&
											!each.breaks && each.landing == window.landing && each.sequence == window.sequence)
						same = &each;
				}
				if (same)
				{
					same->buttons |= window.buttons;
					same->first = std::min(same->first, window.first);
					same->last = std::max(same->last, window.last);
				}
				else if (span.count == span.windows.size())
					return {};
				else
					span.windows[span.count++] = window;
			}
			cancel += moves::cancel_stride;
		}
		return {};
	}

	// The spans of the moves seen in this game. Move data does not change within a game; cleared at every match start and
	// end, since the next game may load other banks at the same addresses.
	class span_cache
	{
	public:
		template <typename Byte>
		move_span get(const Byte& byte, u32 move, u32 moveset)
		{
			if (!move || !moveset)
				return {};
			entry& slot = m_entries[(move >> 4) * 2654435761u >> 24];
			if (slot.move != move || slot.moveset != moveset)
				slot = {move, moveset, span_of_move(byte, move, moveset)};
			return slot.span;
		}
		void clear() noexcept
		{
			m_entries = {};
		}

	private:
		struct entry
		{
			u32 move = 0, moveset = 0;
			move_span span{};
		};
		std::array<entry, 256> m_entries{};
	};

	// One fighter as the rule reads it.
	struct actor_view
	{
		bool human = false;
		u16 port = 0, frame = 0;
		u32 move = 0, moveset = 0, move_moveset = 0;
		bool gate_open = false;
		u8 facing = 0;
	};

	template <typename Byte>
	constexpr actor_view read_actor(const Byte& byte, u32 address)
	{
		actor_view view;
		view.human = byte(address + actor::controller) == 0;
		view.port = read16(byte, address + actor::port);
		view.frame = read16(byte, address + actor::frame);
		view.move = read32(byte, address + actor::move);
		view.moveset = read32(byte, address + actor::moveset);
		view.move_moveset = read32(byte, address + actor::move_moveset);
		view.gate_open = read32(byte, address + actor::life) != 0;
		view.facing = byte(address + actor::facing);
		return view;
	}

	// What the rule moves at one step: the word bits of K; those a window other than a sequence reads, where a press that
	// arrives late was made too early; whether the hold can go on, where every press that arrives late was; and whether a
	// window reads the directions.
	struct escape_view
	{
		u16 buttons = 0, timed = 0;
		bool continuing = false, directions = false;

		constexpr bool any() const noexcept
		{
			return buttons || directions;
		}
	};

	// K at the fighter's frame. The combat code commits moves and advances +0x96 only before the input step (0xf1a28 and
	// its throw pair helpers in 0x17744), and the scan tests the same +0x96, so the move and frame the step sees are the ones
	// the tick's scan tests; a grab commits the held move, with +0xbc, before the held fighter's input step, so the grab
	// tick is covered too. A human held in a move from the opponent's moveset gets every window that reads input but the
	// landing options, which `tech_rolls`, a developer switch, adds back. In the fighter's own moves only break windows
	// count: a mirror match may run both fighters from one loaded moveset, where the moveset test cannot see the hold, and
	// no other move of the shipped movesets has a break cancel. A facing other than 0 or 1 skips the step's direction table
	// (0xecc7c), so no direction is moved then. While the hold `continues` (hold_continues) all four buttons are in K, so a
	// press reaches the command history at offline timing for a sequence of the next hold, except in the L frames before a
	// landing option of the move, whose presses keep the delay. The holder's move is then the one whose pair gave the held
	// move, and in none of the 952 pairs of the shipped movesets whose holder can start another pair does the held move have
	// a landing option, or an exit of its own, within 6 frames after the holder's last such frame.
	constexpr escape_view escapes_at(const actor_view& fighter, const move_span& span, bool tech_rolls, bool continues, u32 ahead) noexcept
	{
		escape_view view;
		if (!fighter.human || !span.readable)
			return view;
		const bool held = fighter.move_moveset != fighter.moveset;
		u32 landing = 0x10000;
		for (u32 at = 0; at < span.count; ++at)
		{
			const escape_window& window = span.windows[at];
			if (window.landing && !tech_rolls)
				landing = std::min<u32>(landing, window.first);
			if (fighter.frame < window.first || fighter.frame > window.last || (!held && !window.breaks) || (window.landing && !tech_rolls))
				continue;
			const u16 buttons = word_buttons(window.buttons);
			view.buttons = static_cast<u16>(view.buttons | buttons);
			if (!window.sequence)
				view.timed = static_cast<u16>(view.timed | buttons);
			view.directions = view.directions || (window.directions && fighter.facing <= 1);
		}
		if (held && continues && fighter.frame + ahead < landing)
		{
			view.buttons = static_cast<u16>(view.buttons | escape_buttons);
			view.continuing = true;
		}
		return view;
	}

	// Whether the hold of the fighter at `address` can go on into another: the fighter runs, from the opponent's moveset,
	// the reaction the opponent's current move gave it (+0x15a: class 0x1c00 for the fighter and 0x1800 for the opponent,
	// both from the pair's commit; any later move of either, an escape, a restart, the end of either move, rewrites its
	// own), and that move can still start a pair at its frame (the mount's punches and arm locks, a chain throw's next
	// stage). A reaction that outlasts its holder's move, or a holder that has gone on to a walk or a throw attempt of its
	// own, ends the hold. Both fighters' moves and frames are final for the tick before either input step.
	template <typename Byte, typename Spans>
	constexpr bool hold_continues(const Byte& byte, u32 address, const actor_view& fighter, Spans&& span_of)
	{
		const std::optional<u32> side = side_of(address);
		if (!side || fighter.move_moveset == fighter.moveset ||
			(read16(byte, address + actor::move_class) & moves::extra_class) != moves::extra_held)
			return false;
		const u32 other = pbd::replay_memory::actors + (1 - *side) * pbd::replay_memory::actor_stride;
		const actor_view holder = read_actor(byte, other);
		if (holder.move_moveset != fighter.move_moveset || (read16(byte, other + actor::move_class) & moves::extra_class) != moves::extra_pair)
			return false;
		const move_span span = span_of(holder.move, holder.move_moveset);
		return span.readable && span.follow_up && holder.frame <= span.follow_up;
	}

	// L for one port: the delay its words run with, while the rule is on and the port's word that many ticks ahead is at
	// hand when the tick runs. 0 never arms, so the port plays as stock.
	constexpr u32 lookahead(bool rule_on, bool ahead_known, std::optional<u32> delay) noexcept
	{
		return rule_on && ahead_known && delay ? *delay : 0;
	}

	// The largest delay a session runs (PBD_PEER_INPUT_DELAY and Practice's Input Delay Frames), so the largest L.
	inline constexpr u32 max_lookahead = 6;

	// The rule this build runs, as recordings, streams and the options digest name it. 1 read the held fighter's whole word
	// ahead through a hold; 2 moves only the held fighter's escape presses.
	inline constexpr u32 rule_version = 2;

	// What a replay or a watched game needs to run the rule as the session ran it: L per side, and whether landing options
	// counted as escapes (the developer switch).
	struct lookahead_setting
	{
		std::array<u32, 2> lookahead{};
		bool tech_rolls = false;

		constexpr bool any() const noexcept
		{
			return lookahead[0] || lookahead[1];
		}
		constexpr bool operator==(const lookahead_setting&) const = default;
	};

	// The settings a replay runs with: the game start's from its first input, then each recorded round's from that round's
	// first input, so a set whose delay changes between rounds plays each round with its own L.
	class lookahead_rounds
	{
	public:
		// False for a value before the last one added, or out of range. A value from the same input as the last one replaces it.
		bool add(u32 from, const lookahead_setting& value) noexcept
		{
			if ((m_count && from < m_changes[m_count - 1].from) || value.lookahead[0] > max_lookahead || value.lookahead[1] > max_lookahead)
				return false;
			if (m_count && from == m_changes[m_count - 1].from)
			{
				m_changes[m_count - 1].value = value;
				return true;
			}
			if (m_count == m_changes.size())
				return false;
			m_changes[m_count++] = {from, value};
			return true;
		}
		lookahead_setting at(u32 offset) const noexcept
		{
			lookahead_setting value;
			for (u32 at = 0; at < m_count && m_changes[at].from <= offset; ++at)
				value = m_changes[at].value;
			return value;
		}
		bool any() const noexcept
		{
			return std::any_of(m_changes.begin(), m_changes.begin() + m_count, [](const change& each)
				{
					return each.value.any();
				});
		}

	private:
		struct change
		{
			u32 from = 0;
			lookahead_setting value{};
		};
		std::array<change, 33> m_changes{}; // the game start and 32 rounds
		u32 m_count = 0;
	};

	// What one step read ahead, kept in the tick's row: confirmation compares the received words with the ones used, but
	// only in the bits the step used. Words are held masks; `word` and `previous_word` keep only those bits.
	struct lookahead_use
	{
		u16 mask = 0;          // K's buttons, and the directions while a window reads them
		bool no_fresh = false; // the game had marked the frame (0xed1a0)
		u32 source = 0;        // the tick whose word the step read ahead: tick + L
		u16 word = 0;
		bool uses_previous = false; // the previous step's word ahead, against which A's presses and direction changes count
		u32 previous_source = 0;
		u16 previous_word = 0;

		constexpr bool reads() const noexcept
		{
			return mask != 0;
		}
	};

	// A received word changes the tick when a bit the step used differs: a button of K, or a direction a window read.
	constexpr bool mispredicted(const lookahead_use& use, u16 word, u16 previous_word) noexcept
	{
		return use.reads() && ((word & use.mask) != use.word || (use.uses_previous && (previous_word & use.mask) != use.previous_word));
	}

	// Confirmation: the boundary's misprediction scan, its wait and the segment barrier. A remote row that read ahead also
	// ran the other PC's words its use names, so it is final only once those words are usable too, and wrong as soon as one
	// of them that is usable differs in a bit it used, whether or not the row's own word has arrived. `usable(tick)` and
	// `received(tick)` are the boundary's view of the other PC's words: `received` gives the held mask, and nothing while the
	// word is not usable.
	template <typename Usable>
	constexpr bool sources_usable(const lookahead_use& use, Usable&& usable)
	{
		return !use.reads() || (usable(use.source) && (!use.uses_previous || usable(use.previous_source)));
	}

	template <typename Received>
	constexpr bool known_misprediction(const lookahead_use& use, Received&& received)
	{
		if (!use.reads())
			return false;
		const std::optional<u16> word = received(use.source);
		if (word && mispredicted(use, *word, use.previous_word))
			return true;
		const std::optional<u16> previous = use.uses_previous ? received(use.previous_source) : std::nullopt;
		return previous && mispredicted(use, use.word, *previous);
	}

	// The first row from `from` that cannot be confirmed: its own word is not usable (`unusable` is the first such row), or a
	// word its remote use read is not. `use_at(t)` is row t's record of the other PC's port.
	template <typename UseAt, typename Usable>
	constexpr u32 first_unconfirmed(u32 from, u32 unusable, UseAt&& use_at, Usable&& usable)
	{
		for (u32 t = from; t < unusable; ++t)
			if (!sources_usable(use_at(t), usable))
				return t;
		return unusable;
	}

	// The presses the rule tracks for one port between steps. A step sees a press as a rise between the word of its previous
	// step and its own, so a press is named by the range of word indices its first held word may have: (p, t] for a rise in
	// S seen at step t whose previous step was p, (p + L, t + L] for one in A. One step after another these are single words;
	// a paused simulation, which skips steps, widens them, and a rise in S is the copy of a press used from A when the two
	// ranges meet.
	// `early` holds, per button, up to four presses used from A whose copies have not risen in S: each byte is last - `base`
	// in its low nibble (1..L, 0 for none) and last - first in its high nibble, where `base` is the step that left the state.
	// No copy rises in S before the next step, so first is kept from base + 1 on and last - first stays below L, however many
	// steps a pause skipped. `used_held`: S holds a button by such a copy, or by a press that came too early for a window or
	// before a hold that can go on, which never fires again. `queued`: two presses of a button fired on one step; the second
	// fires on the next.
	struct escape_state
	{
		u32 base = 0;
		std::array<std::array<u8, 4>, 4> early{};
		u8 used_held = 0, queued = 0;

		constexpr bool empty() const noexcept
		{
			return !used_held && !queued && std::all_of(early.begin(), early.end(), [](const std::array<u8, 4>& presses)
												{
													return presses == std::array<u8, 4>{};
												});
		}
		constexpr bool operator==(const escape_state&) const = default;
	};

	// One port's part of a tick's row. The input step runs once per battle update, and a paused simulation skips the input
	// step, commits and scans together (0x13d70, 0x17810, 0x199a8), so the previous step is the last earlier tick whose
	// step ran. What the next step needs from earlier steps travels from row to row, so a pause of any length needs no
	// older row.
	struct port_record
	{
		bool stepped = false;   // the port's input step ran this tick (the latch entry)
		bool loaded = false;    // and passed the word load
		bool gate_open = false; // its life was not 0 at that step
		bool blanked = false;   // Practice blanked a word that held something (0xed740): the step held and pressed nothing
		u16 held = 0;           // what the step held after the life gate (r7 at the latch entry): the next +0x6c4
		u16 stock = 0;          // the slot at the word load: the word the port runs this tick
		u8 ahead = 0;           // the L the word load ran with
		// What the word load left for the step: the slot, and the +0x6c4 the step loads; `writes` when either differs from
		// what the game had there. The latch entry puts the stock word back.
		bool writes = false;
		u16 slot = 0;
		u32 previous_loaded = 0;
		// Practice: the word before Input Delay Frames, replaced wherever the delayed word is replaced after the delay, and the
		// delay that word waits.
		u16 practice_word = 0;
		u8 practice_delay = 0;
		// A replay or a watched game: the port's L and the tech-roll switch at this tick, from the recording or the stream.
		u8 lookahead = 0;
		bool tech_rolls = false;
		lookahead_use use{};
		// The direction fields the scan sees (+0x6bc << 16 | +0x6be), planned at the word load, and the stock ones the scan
		// entry replaced, which the scan exit puts back.
		bool view = false, view_written = false;
		u32 view_fields = 0, stock_fields = 0;
		// Carried from the last step that passed the word load, which replaces them: whether the fighter ran a move of the
		// opponent's moveset there, and the presses the rule tracks.
		bool in_hold = false;
		escape_state escape{};
		// The port's last earlier step: its tick, gate, blank, stock word, held mask, Practice word, L and whether it read
		// ahead.
		bool previous_known = false, previous_gate_open = false, previous_blanked = false, previous_read = false;
		u32 previous_step = 0;
		u16 previous_stock = 0, previous_held = 0, previous_practice_word = 0;
		u8 previous_ahead = 0;
	};

	// At tick t's input update: the port's earlier steps, from row t - 1 (null when that row is not held).
	constexpr void carry(port_record& now, const port_record* before, u32 tick) noexcept
	{
		if (!before)
			return;
		now.in_hold = before->in_hold;
		now.escape = before->escape;
		now.previous_known = before->stepped || before->previous_known;
		if (!before->stepped)
		{
			now.previous_step = before->previous_step;
			now.previous_gate_open = before->previous_gate_open;
			now.previous_blanked = before->previous_blanked;
			now.previous_stock = before->previous_stock;
			now.previous_held = before->previous_held;
			now.previous_practice_word = before->previous_practice_word;
			now.previous_ahead = before->previous_ahead;
			now.previous_read = before->previous_read;
			return;
		}
		now.previous_step = tick - 1;
		now.previous_gate_open = before->gate_open;
		now.previous_blanked = before->blanked;
		now.previous_stock = before->stock;
		now.previous_held = before->held;
		now.previous_practice_word = before->practice_word;
		now.previous_ahead = before->ahead;
		now.previous_read = before->use.reads();
	}

	// Whether no press the rule moved still waits for its copy in the stock words, or still holds a button, or fires on the
	// next step, for the port at tick `tick` (its record after carry). Only there can L change, or a replay start a round
	// with nothing carried from the rows before: the rest of what a step needs is read from the words.
	constexpr bool settled(const port_record& record) noexcept
	{
		return record.escape.empty();
	}

	// The same for the tick after `tick`, from tick `tick`'s record as far as its step has run.
	constexpr bool settled_after(const port_record& record, u32 tick) noexcept
	{
		port_record next;
		carry(next, &record, tick + 1);
		return settled(next);
	}

	// Whether the port's next step may read ahead, from its record of the tick before: the fighter runs a move of the
	// opponent's moveset, or its last step read ahead (a break window in its own moveset). A lockstep row is final once it
	// runs, so the boundary also waits for the other PC's word L ticks past the row then; a hold that starts on a lockstep
	// tick has no such wait, and its word load refuses there unless the words it reads have arrived.
	constexpr bool may_read_ahead(const port_record& record) noexcept
	{
		return record.in_hold || (record.stepped ? record.use.reads() : record.previous_read);
	}

	// The lockstep wait for row `tick`, from the port's record of the row before: a step that reads ahead reads the word L
	// past its tick and the word L past its previous step, against which its presses count, so both must have arrived
	// (`arrived(source)`).
	template <typename Arrived>
	constexpr bool lookahead_arrived(const port_record& before, u32 tick, u32 ahead, Arrived&& arrived)
	{
		if (!ahead || !may_read_ahead(before))
			return true;
		const std::optional<u32> previous = before.stepped ? std::optional<u32>{tick - 1} : before.previous_known ? std::optional<u32>{before.previous_step} :
		                                                                                                            std::nullopt;
		return arrived(tick + ahead) && (!previous || arrived(*previous + ahead));
	}

	// A replay's or a watched game's tick, after carry: each port takes the recorded setting, which may differ from the row
	// before's only where the port is settled, as the session changes it between rounds. A replay that no longer follows its
	// recording (Take Control: the taken side plays without the rule) is not held to that. The refusal, or null.
	constexpr const char* take_recorded(std::array<port_record, 2>& now, const std::array<port_record, 2>* before, u32 tick,
		const lookahead_setting& value, bool following) noexcept
	{
		static_cast<void>(tick);
		for (u32 port = 0; port < 2; ++port)
		{
			now[port].lookahead = static_cast<u8>(value.lookahead[port]);
			now[port].tech_rolls = value.tech_rolls;
			const bool changed = before && ((*before)[port].lookahead != now[port].lookahead || (*before)[port].tech_rolls != now[port].tech_rolls);
			if (following && changed && !settled(now[port]))
				return "throw_break_lookahead_change_pending";
		}
		return nullptr;
	}

	// At the latch entry: the port's step ran, with the gate the step used (nothing in 0xecb58 writes +0x6ac) and what it
	// held.
	constexpr void note_step(port_record& now, const actor_view& fighter, u16 held) noexcept
	{
		now.stepped = true;
		now.gate_open = fighter.gate_open;
		now.held = held;
	}

	// Practice's word before its delay at the port's step `step`: this tick's, or the previous step's.
	constexpr std::optional<u16> practice_word_at(const port_record& record, u32 tick, u32 step) noexcept
	{
		if (step == tick)
			return record.practice_word;
		if (record.previous_known && step == record.previous_step)
			return record.previous_practice_word;
		return std::nullopt;
	}

	// What the word load writes, and what it leaves for the next step; or the session ends because a word or a step the
	// step needs is not known.
	struct step_plan
	{
		const char* refusal = nullptr;
		u16 port = 0;
		u8 ahead = 0;
		u16 stock = 0;
		std::optional<u16> slot;          // the slot, where it differs from the stock word
		std::optional<u32> previous_held; // +0x6c4, where it differs from what the game left there
		u32 previous_loaded = 0;          // the +0x6c4 the step then loads
		std::optional<u32> view;          // the direction fields the scan sees
		lookahead_use use{};
		escape_state escape{};
		bool in_hold = false;
		// Buttons pressed in A and used now; buttons whose used copy rose in S now; buttons whose press rose in S too early
		// for the window that reads it now.
		u16 early = 0, masked = 0, missed = 0;

		constexpr bool writes() const noexcept
		{
			return slot || previous_held;
		}
		constexpr bool needed() const noexcept
		{
			return refusal || writes() || view || use.reads();
		}
	};

	// The tick's record of what the word load did.
	constexpr void take_plan(port_record& record, const step_plan& plan) noexcept
	{
		record.loaded = true;
		record.stock = plan.stock;
		record.ahead = plan.ahead;
		record.use = plan.use;
		record.escape = plan.escape;
		record.in_hold = plan.in_hold;
		record.writes = plan.writes();
		record.slot = plan.slot.value_or(plan.stock);
		record.previous_loaded = plan.previous_loaded;
		record.view = plan.view.has_value();
		record.view_fields = plan.view.value_or(0);
	}

	// The word load. `port_register` is the port whose slot the step loads (r30 at 0xed1d8), `lookahead_of`
	// L per port at this tick, `records` the tick's row, `span_of(move, moveset)` a move's span and `held_at(port, source)`
	// the held mask the port's input update stores at tick `source` (nothing when not known yet).
	//
	// Per button: S is the stock word, A the word L ticks ahead, edges count against the previous step's. A press seen in A
	// while its button is in K fires now and is noted as used; when its copy rises in S it is masked: held, never fresh. Any
	// other rise in S was not seen in K at its offline tick: where a window other than a sequence reads the button now, it
	// came before that window opened and misses, as offline (held, never fresh, and held from then on as a used copy is); so
	// does one while the hold can go on, made before anything carried it, which would reach the command history after
	// presses made later; elsewhere it fires as today, a sequence counting it from the command history. In K the button is
	// held as A holds it, as offline, and on the step a press from S fires; out of K as S holds it. +0x6c4 gets, for every
	// button the rule touches, held and not fired, so the step's pressed is exactly the fired buttons and a button held on
	// from a window is no new press; the rest stays as the game left it. A frame the game marked has no fresh press, as
	// offline: a press seen in A there is used up all the same. After a step whose life gate was closed, or whose word
	// Practice blanked, every held button is fresh again, offline as online, so +0x6c4 stays as the game left it then too.
	// Where a press fires while a window other than a sequence reads it and A holds another button of such a window that S
	// does not hold yet, that button went fresh before this step offline and this step uses it as held (1 held, then 2, is
	// 1+2): it is used up too, so its copy never fires after the escape.
	template <typename Byte, typename Spans, typename Words>
	step_plan at_word_load(const Byte& byte, u32 address, u32 port_register, u32 tick, const std::array<u32, 2>& lookahead_of,
		const std::array<port_record, 2>& records, bool tech_rolls, Spans&& span_of, Words&& held_at)
	{
		step_plan plan;
		const actor_view fighter = read_actor(byte, address);
		if (!fighter.human || fighter.port > 1)
			return plan;
		if (port_register != fighter.port && (lookahead_of[0] || lookahead_of[1]))
		{
			plan.refusal = "throw_break_port_mismatch";
			return plan;
		}
		plan.port = fighter.port;
		plan.stock = static_cast<u16>(read32(byte, slot_of(fighter.port)));
		const u32 current = read32(byte, address + actor::previous_held);
		plan.previous_loaded = current;
		const u32 ahead = lookahead_of[fighter.port];
		plan.ahead = static_cast<u8>(ahead);
		const port_record& record = records[fighter.port];
		lookahead_use& use = plan.use;
		use.no_fresh = current == no_fresh_mark;
		if (!ahead)
			return plan;
		plan.in_hold = fighter.move_moveset != fighter.moveset;
		plan.escape.base = tick;
		const escape_view view =
			escapes_at(fighter, span_of(fighter.move, fighter.move_moveset), tech_rolls, hold_continues(byte, address, fighter, span_of), ahead);
		if (view.any() && !record.previous_known)
		{
			plan.refusal = "throw_break_previous_step_unknown";
			return plan;
		}
		// The first step with this L tracks from here and moves nothing: the session changes L only where nothing is tracked
		// (Practice's delay drops to 0 only off the fight).
		if (!record.previous_known || record.previous_ahead != ahead)
			return plan;
		const escape_state& before = record.escape;
		const u16 stock = plan.stock & escape_buttons;
		const u16 stock_rise = static_cast<u16>(stock & ~record.previous_stock);
		u16 ahead_word = 0, ahead_rise = 0;
		if (view.any())
		{
			use.mask = static_cast<u16>(view.buttons | (view.directions ? direction_bits : 0));
			use.source = tick + ahead;
			use.uses_previous = true;
			use.previous_source = record.previous_step + ahead;
			const std::optional<u16> word = held_at(fighter.port, use.source), previous = held_at(fighter.port, use.previous_source);
			if (!word || !previous)
			{
				plan.refusal = "throw_break_source_missing";
				return plan;
			}
			use.word = *word & use.mask;
			use.previous_word = *previous & use.mask;
			ahead_word = *word;
			ahead_rise = static_cast<u16>(*word & ~*previous & view.buttons);
		}
		escape_state& after = plan.escape;
		u16 held = static_cast<u16>(plan.stock & ~escape_buttons), fired = 0, touched = 0;
		for (u32 button = 0; button < 4; ++button)
		{
			const u16 bit = button_bits[button];
			const u8 flag = static_cast<u8>(1u << button);
			// The used presses, with this step's press from A if there is one, as ranges [first, last] of word indices. A rise
			// in S now is the copy of the oldest whose range it meets (after a pause that can be the press A shows now); used
			// presses S has passed without one never come, and the rest wait.
			const bool rose = stock_rise & bit;
			const bool in_k = view.buttons & bit, ahead_press = in_k && (ahead_rise & bit);
			std::array<std::pair<u32, u32>, 5> presses_used{};
			u32 count = 0;
			for (const u8 each : before.early[button])
				if (each)
					presses_used[count++] = {before.base + (each & 0xf) - (each >> 4), before.base + (each & 0xf)};
			if (ahead_press)
				presses_used[count++] = {record.previous_step + ahead + 1, tick + ahead};
			u32 oldest = count;
			for (u32 at = 0; at < count; ++at)
				if (rose && presses_used[at].first <= tick && (oldest == count || presses_used[at].second < presses_used[oldest].second))
					oldest = at;
			const bool used = oldest != count;
			std::array<u8, 4> pending{};
			for (u32 at = 0, kept = 0; at < count && kept < 4; ++at)
				if (at != oldest && presses_used[at].second > tick)
				{
					const u32 first = std::max(presses_used[at].first, tick + 1);
					pending[kept++] = static_cast<u8>((presses_used[at].second - first) << 4 | (presses_used[at].second - tick));
				}
			after.early[button] = pending;
			const bool missed = rose && !used && ((view.timed & bit) || view.continuing);
			const bool stock_press = rose && !used && !missed;
			const bool still = (stock & bit) && !rose;
			const bool used_held = used || missed || (still && (before.used_held & flag));
			const bool unused_held = (stock & bit) && !used_held;
			const bool queued = before.queued & flag;
			const u32 presses = u32{ahead_press} + u32{stock_press} + u32{queued};
			// A press from S that fires in K is held on its step, released or not in A by then; from the next step A says.
			bool down = in_k ? ((ahead_word & bit) || stock_press) : (stock & bit) != 0;
			if (queued)
				down = true;
			const bool fires = presses && !use.no_fresh;
			if (used_held)
				after.used_held = static_cast<u8>(after.used_held | flag);
			if (presses > 1 && !use.no_fresh)
				after.queued = static_cast<u8>(after.queued | flag);
			if (down)
				held = static_cast<u16>(held | bit);
			if (fires)
				fired = static_cast<u16>(fired | bit);
			if (in_k || used || unused_held || used_held || queued)
				touched = static_cast<u16>(touched | bit);
			if (ahead_press)
				plan.early = static_cast<u16>(plan.early | bit);
			if (used)
				plan.masked = static_cast<u16>(plan.masked | bit);
			if (missed)
				plan.missed = static_cast<u16>(plan.missed | bit);
		}
		if (fired & view.timed)
			for (u32 button = 0; button < 4; ++button)
			{
				// A holds the button at tick + L and S does not at tick, so the press is in words (tick, tick + L].
				const u16 bit = button_bits[button];
				if ((view.timed & bit) && (ahead_word & bit) && !(stock & bit) && after.early[button] == std::array<u8, 4>{})
				{
					after.early[button][0] = static_cast<u8>((ahead - 1) << 4 | ahead);
					plan.early = static_cast<u16>(plan.early | bit);
				}
			}
		if (held != plan.stock)
			plan.slot = held;
		const bool previous_live = record.previous_gate_open && !record.previous_blanked;
		if (!use.no_fresh && previous_live)
		{
			const u32 wanted = (current & ~u32{touched}) | (held & ~fired & touched);
			if (wanted != current)
			{
				plan.previous_held = wanted;
				plan.previous_loaded = wanted;
			}
		}
		if (view.directions)
		{
			// The direction fields as the step builds them offline, from A's held and changed directions.
			const u16 ahead_way = fighter.gate_open ? static_cast<u16>(ahead_word & direction_bits) : u16{0};
			const u16 previous_way = previous_live ? static_cast<u16>(use.previous_word & direction_bits) : u16{0};
			const u32 previous = use.no_fresh ? no_fresh_mark : previous_way;
			const u16 pressed = static_cast<u16>(ahead_way & ~previous);
			plan.view = direction_fields(ahead_way, pressed, previous, static_cast<u16>(1u << numpad(previous_way, fighter.facing)), fighter.facing);
		}
		return plan;
	}

	// What the step loaded, read at the latch entry: held in r7 and pressed in r6 (set at 0xecc74 and 0xecc6c after the
	// life gate; nothing on the way writes either).
	struct step_registers
	{
		u16 held = 0, pressed = 0;
	};

	// At the latch entry, after note_step: whether the step ran what the rule planned. A step the word load did not see
	// (another source, 0xed184 or 0xed1d4) must not have needed it; it still leaves what the rule tracks for the next step.
	// Where the word load wrote, +0x6c4 must hold what it left there (the input mode 0 path 0xed79c sets it without a step),
	// and the step must have held the planned word and pressed what it fired, or nothing where Practice blanked it (0xed740),
	// which the game decides from the word itself as it does offline. A wrong model would change the game alike on both PCs,
	// where no hash sees it.
	template <typename Byte, typename Spans>
	const char* check_latch_entry(const Byte& byte, u32 address, u32 tick, const std::array<u32, 2>& lookahead_of,
		std::array<port_record, 2>& records, bool tech_rolls, step_registers step, Spans&& span_of)
	{
		const actor_view fighter = read_actor(byte, address);
		if (!fighter.human || fighter.port > 1)
			return nullptr;
		port_record& record = records[fighter.port];
		if (!record.loaded)
		{
			const auto any = [](u16, u32) -> std::optional<u16>
			{
				return u16{0};
			};
			const step_plan unseen = at_word_load(byte, address, fighter.port, tick, lookahead_of, records, tech_rolls, span_of, any);
			if (unseen.needed())
				return "throw_break_step_unhooked";
			record.stock = unseen.stock;
			record.ahead = unseen.ahead;
			record.escape = unseen.escape;
			record.in_hold = unseen.in_hold;
			return nullptr;
		}
		const u16 held = record.gate_open ? record.slot : u16{0};
		const bool blanked = step.held == 0 && step.pressed == 0 && read32(byte, game_mode) == practice_mode;
		record.blanked = blanked && held != 0;
		if (!record.writes)
			return nullptr;
		const u16 pressed = static_cast<u16>(held & ~record.previous_loaded);
		if (read32(byte, address + actor::previous_held) != record.previous_loaded || (!blanked && (step.held != held || step.pressed != pressed)))
			return "throw_break_model_mismatch";
		return nullptr;
	}

	// Whether both fighters run one loaded moveset, where the moveset test cannot see a hold and only the break windows count.
	// Whether a mirror match loads one moveset or two is not settled. No other mark covers a whole hold: +0x15a has class
	// 0x1c00 only in the reaction a pair gave, and every later move of the held fighter's own (its escapes, a wrong-button
	// restart, a reaction's end into the next) rewrites it.
	template <typename Byte>
	constexpr bool shared_moveset(const Byte& byte)
	{
		const u32 left = read32(byte, pbd::replay_memory::actors + actor::moveset);
		const u32 right = read32(byte, pbd::replay_memory::actors + pbd::replay_memory::actor_stride + actor::moveset);
		return left && left == right;
	}

	// The switch, PBD_THROW_BREAK: 1 on, 0 off. Unset leaves the rule off in Practice and peer sessions.
	// Replays and watchers follow the recording or the stream instead: they run the rule with each round's L
	// as the session ran it, whatever the switch. PBD_THROW_BREAK_TECH_ROLLS=1, a developer switch, counts landing options
	// as escapes too, so tech rolls read ahead; they keep the delay until a feel test settles them.
	enum class setting : u8
	{
		unset,
		on,
		off,
		invalid,
	};

	constexpr setting read_setting(const char* text) noexcept
	{
		if (!text || !*text)
			return setting::unset;
		const std::string_view value(text);
		return value == "1" ? setting::on : value == "0" ? setting::off :
		                                                   setting::invalid;
	}

	// The session's side of the rule.
	struct session_state
	{
		// `playback`: a replay or a watched game, whose rows carry each tick's L from the recording or the stream.
		bool on = false, peer = false, playback = false;
		// Landing options count as escapes; PBD_THROW_BREAK was set to 1 rather than left to the default; this match's
		// movesets were compared.
		bool tech_rolls = false, developer = false, movesets_checked = false;
		// Peer sessions: whether the words L ticks ahead are at hand (the push model fixes each local word D ticks ahead, and
		// the other PC's words count once they arrive), L per port once the agreed delay is applied, and whether it is.
		bool ahead_known = false, applied = false;
		std::array<u32, 2> peer_lookahead{};
		// The tick whose input update completed last and whose battle update runs now, whether its row is the verification
		// side row, and the boundary its remote words are read as of. Cleared at every boundary.
		std::optional<u32> executing;
		bool side_row = false;
		u32 now = 0;
		// A replay's round jump: the tick from which nothing carries from the rows before, which belong to another point of
		// the game.
		std::optional<u32> restart;
		span_cache spans;

		// A delay fixed at configure applies at once; an automatic one, or a fixed one below 4 (which opens at 4), applies
		// at input_delay_applied. Local words beyond the applied delay are not fixed before that.
		void configure_peer(setting value, setting tech, bool fixed_ahead, std::optional<u32> fixed_delay) noexcept
		{
			on = value == setting::on;
			peer = true;
			tech_rolls = tech == setting::on;
			developer = value == setting::on;
			ahead_known = fixed_ahead;
			if (fixed_delay)
				apply_delay(*fixed_delay);
		}
		void configure_practice(setting value, setting tech) noexcept
		{
			on = value == setting::on;
			peer = false;
			tech_rolls = tech == setting::on;
			developer = value == setting::on;
		}
		// A replay or a watched game whose recording or stream may read ahead (`recorded`). Its rows take L and the tech-roll
		// switch tick by tick from the recording or the stream (begin of each tick), and a shared moveset only logs, as in the
		// players' games, so no local switch changes how it plays.
		void configure_playback(bool recorded) noexcept
		{
			on = recorded;
			peer = false;
			playback = true;
			tech_rolls = false;
			developer = false;
		}
		void apply_delay(u32 delay) noexcept
		{
			const u32 ahead = lookahead(on, ahead_known, delay);
			peer_lookahead = {ahead, ahead};
			applied = true;
		}
		// L per port at the executing tick. Practice delays only the player's port, by what the tick's word waited.
		std::array<u32, 2> lookahead_of(const std::array<port_record, 2>& row) const noexcept
		{
			if (!on)
				return {};
			if (peer)
				return peer_lookahead;
			if (playback)
				return {row[0].lookahead, row[1].lookahead};
			return {lookahead(true, true, row[0].practice_delay), 0};
		}
		// What a peer session's recording and its watchers' stream carry from here on: L per side once the agreed delay is
		// applied, and the tech-roll switch. Nothing with the rule off or L 0, which record and stream as before the rule.
		lookahead_setting recorded() const noexcept
		{
			if (!on || !peer || !applied || !(peer_lookahead[0] | peer_lookahead[1]))
				return {};
			return {peer_lookahead, tech_rolls};
		}
		// A match start or end. A peer match that starts before the delay is applied would run with a provisional L.
		const char* match_changed(bool started) noexcept
		{
			spans.clear();
			movesets_checked = false;
			return started && on && peer && !applied ? "throw_break_lookahead_unset" : nullptr;
		}
		// Mixed into the options digest both peers compare at each match start, only while some port has L: rule off and
		// L = 0 play the same and hash the same. The rule's version (2: escape presses only) goes in too, so two builds that
		// would move different inputs refuse at the match start rather than part later.
		u32 options_digest(u32 digest) const noexcept
		{
			if (!on || !peer || !(peer_lookahead[0] | peer_lookahead[1]))
				return digest;
			for (const u32 byte : {0x54u, 0x42u, rule_version, peer_lookahead[0], peer_lookahead[1]})
				digest = (digest ^ (byte & 0xffu)) * 0x01000193u;
			if (tech_rolls)
				digest = (digest ^ 0x55u) * 0x01000193u;
			return digest;
		}
	};
} // namespace pbd::throw_break
