#include "pbd_test.h"

#include "Emu/PBD/pbd_throw_break.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using namespace pbd::throw_break;
	using pbd::test::check;
	using s64 = std::int64_t;
	using u64 = std::uint64_t;

	// Raw words as PBD injects them. The model's fighter faces right (facing 0), so forward is right.
	constexpr u16 one = 0x0080, two = 0x0010, three = 0x0040, four = 0x0020, both = one | two;
	constexpr u16 up = 0x1000, forward = 0x2000, down = 0x4000, back = 0x8000, start_button = 0x0800, select_button = 0x0100;
	constexpr u32 side_0 = pbd::replay_memory::actors, side_1 = pbd::replay_memory::actors + pbd::replay_memory::actor_stride;
	constexpr u32 wanted_offset = 0x192, matched_offset = 0x193, latch_offset = 0x204;
	// The class word of the queued move, which its commit copies to +0x15a and clears.
	constexpr u32 queued_class = 0x310;
	// The class words a pair commits: the holder's move started it (class 0x1800), the held fighter's is its reaction.
	constexpr u16 pair_class = 0x1ad6, reaction_class = 0x1f57;
	constexpr u32 bank_space = 0x20000000;

	// What the input update stores in the slot (mapped_mask in pbd_rollback_session_common.h).
	constexpr u16 held_mask(u16 raw)
	{
		u16 value = static_cast<u16>(raw & 0xfff0);
		if ((value & 0x5000) == 0x5000)
			value = static_cast<u16>(value & ~0x5000);
		if ((value & 0xa000) == 0xa000)
			value = static_cast<u16>(value & ~0xa000);
		return value;
	}

	// Guest memory: big-endian, zero where nothing was written.
	struct memory
	{
		std::vector<std::pair<u32, std::array<u8, 4096>>> pages;

		u8 operator()(u32 address) const
		{
			for (const auto& [page, bytes] : pages)
				if (page == address >> 12)
					return bytes[address & 0xfff];
			return 0;
		}
		u8& at(u32 address)
		{
			for (auto& [page, bytes] : pages)
				if (page == address >> 12)
					return bytes[address & 0xfff];
			pages.push_back({address >> 12, {}});
			return pages.back().second[address & 0xfff];
		}
		void put8(u32 address, u8 value)
		{
			at(address) = value;
		}
		void put16(u32 address, u16 value)
		{
			put8(address, static_cast<u8>(value >> 8));
			put8(address + 1, static_cast<u8>(value));
		}
		void put32(u32 address, u32 value)
		{
			put16(address, static_cast<u16>(value >> 16));
			put16(address + 2, static_cast<u16>(value));
		}
		u64 checksum() const
		{
			auto sorted = pages;
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
				{
					return a.first < b.first;
				});
			u64 hash = 0xcbf29ce484222325ull;
			for (const auto& [page, bytes] : sorted)
			{
				if (std::all_of(bytes.begin(), bytes.end(), [](u8 value)
						{
							return value == 0;
						}))
					continue;
				for (const u32 value : {page >> 24, page >> 16, page >> 8, page})
					hash = (hash ^ (value & 0xff)) * 0x100000001b3ull;
				for (const u8 value : bytes)
					hash = (hash ^ value) * 0x100000001b3ull;
			}
			return hash;
		}
	};

	// The loaded movesets, which nothing writes, and the state a run writes.
	struct guest
	{
		const memory* banks = nullptr;
		memory state;

		u8 operator()(u32 address) const
		{
			return address >= bank_space ? (*banks)(address) : state(address);
		}
	};

	// The latch's choice (0xecf84..0xed158): 1+2 when both are held and either is fresh, else a fresh 1, else a fresh 2.
	enum class break_class : u8
	{
		none,
		one,
		two,
		both,
	};

	constexpr break_class classify(u16 held, u16 pressed)
	{
		if ((held & both) == both && (pressed & both))
			return break_class::both;
		if (pressed & one)
			return break_class::one;
		if (pressed & two)
			return break_class::two;
		return break_class::none;
	}

	u16 requirement_for(break_class wanted)
	{
		return wanted == break_class::one ? moves::break_one : wanted == break_class::two ? moves::break_two :
		                                                                                    moves::break_both;
	}

	// What the step leaves for the scan and the history: the fresh and held buttons as the command fields keep them (1, 2, 4
	// and 8 for buttons 1 to 4), and the direction fields +0x6bc (held) and +0x6be (changed).
	struct step_fields
	{
		u8 fresh = 0, held = 0;
		u16 direction = 0x20, changed = 1;

		bool operator==(const step_fields&) const = default;
	};

	constexpr u8 nibble(u16 mask)
	{
		return static_cast<u8>((mask & one ? 1 : 0) | (mask & two ? 2 : 0) | (mask & three ? 4 : 0) | (mask & four ? 8 : 0));
	}

	struct cancel_def
	{
		u16 direction = 0, buttons = 0;
		std::vector<std::array<u16, 2>> requirements{}; // id, parameter; none: [0]
		int target = -1;
		u16 first = 1, last = 1, start = 1; // the window, and the frame from which the target commits (+0x30e)
		u16 entry = 1;                      // the frame the target starts at
		u16 option = 0;                     // beyond the 0x01 the loader sets on a cancel without a command
		u16 group = 0;                      // a group jump's group
		u32 extra = 0;                      // the extra word +0x0c points to
	};

	struct move_def
	{
		int bank = 1; // 0: the fighter's own moveset, 1: the opponent's
		std::vector<cancel_def> cancels{};
		bool final = false; // the run ends when it is committed
	};

	struct sequence_def
	{
		u16 frames = 0;
		std::vector<u8> presses{}; // button nibbles, oldest first
	};

	struct moveset_def
	{
		std::vector<move_def> moves{};
		std::vector<std::vector<cancel_def>> groups{};
		std::vector<sequence_def> sequences{}; // direction 0x8007 + index
	};

	cancel_def on(u16 direction, u16 buttons, int target, u16 first, u16 last, u16 start = 1)
	{
		cancel_def cancel;
		cancel.direction = direction;
		cancel.buttons = buttons;
		cancel.target = target;
		cancel.first = first;
		cancel.last = last;
		cancel.start = start;
		return cancel;
	}

	cancel_def automatic(int target, u16 frame)
	{
		return on(0, 0, target, frame, frame, frame);
	}

	cancel_def break_cancel(break_class wanted, int target, u16 first, u16 last, u16 start)
	{
		cancel_def cancel = on(0, 0, target, first, last, start);
		cancel.requirements = {{requirement_for(wanted), 0}};
		return cancel;
	}

	cancel_def group_jump(u16 group)
	{
		cancel_def cancel;
		cancel.direction = moves::group_jump;
		cancel.group = group;
		return cancel;
	}

	// A tech roll as the shipped thrown moves hold it: any of the buttons fresh (0x1000), extra word 0x90.
	cancel_def tech_roll(int target, u16 first, u16 last, u16 buttons = 0x100f)
	{
		cancel_def cancel = on(0, buttons, target, first, last);
		cancel.extra = 0x90;
		return cancel;
	}

	// Lays out a moveset the way the loader leaves a bank, once in each bank: header, moves, cancels with their
	// requirement lists and extra words, the group cancel table and the input sequences.
	struct layout
	{
		static constexpr std::array<u32, 2> base{0x21000000, 0x21400000};
		memory banks;
		std::array<std::vector<u32>, 2> moves;
	};

	layout lay_out(const moveset_def& set, bool terminate_requirements = true)
	{
		layout out;
		for (u32 bank = 0; bank < 2; ++bank)
		{
			const u32 base = layout::base[bank], groups = base + 0x8000, sequences = base + 0x60000;
			u32 cancel = base + 0x10000, requirement = base + 0x30000, extra = base + 0x50000, presses = base + 0x61000;
			out.banks.put32(base + moves::group_table, groups);
			out.banks.put32(base + moves::sequence_table, sequences);
			for (u32 at = 0; at < set.sequences.size(); ++at)
			{
				const sequence_def& sequence = set.sequences[at];
				out.banks.put16(sequences + 8 * at, sequence.frames);
				out.banks.put16(sequences + 8 * at + 2, static_cast<u16>(sequence.presses.size()));
				out.banks.put32(sequences + 8 * at + 4, presses);
				for (const u8 buttons : sequence.presses)
				{
					out.banks.put16(presses, 0x20);
					out.banks.put16(presses + 2, buttons);
					presses += 4;
				}
			}
			std::vector<u16> group_start;
			const auto put = [&](u32 at, const cancel_def& c)
			{
				out.banks.put16(at, c.direction);
				out.banks.put16(at + moves::cancel_buttons, c.buttons);
				out.banks.put32(at + moves::cancel_requirements, requirement);
				for (const auto& [id, parameter] : c.requirements.empty() ? std::vector<std::array<u16, 2>>(1, std::array<u16, 2>{0, 0}) : c.requirements)
				{
					out.banks.put16(requirement, id);
					out.banks.put16(requirement + 2, parameter);
					requirement += moves::requirement_stride;
				}
				if (terminate_requirements)
					out.banks.put16(requirement, moves::requirement_end);
				requirement += moves::requirement_stride;
				out.banks.put16(at + moves::cancel_group, c.direction == moves::group_jump ? group_start.at(c.group) : static_cast<u16>(std::max(c.target, 0)));
				out.banks.put16(at + moves::cancel_first, c.first);
				out.banks.put16(at + moves::cancel_last, c.last);
				out.banks.put16(at + 0x14, c.start);
				out.banks.put16(at + moves::cancel_option, static_cast<u16>(c.option | (c.direction || c.buttons ? 0 : moves::option_unread)));
				out.banks.put32(at + moves::cancel_extra, extra);
				out.banks.put32(extra, c.extra);
				extra += 4;
			};
			u16 entry = 0;
			for (const auto& list : set.groups)
			{
				group_start.push_back(entry);
				entry = static_cast<u16>(entry + list.size() + 1);
			}
			entry = 0;
			for (const auto& list : set.groups)
			{
				for (const cancel_def& c : list)
					put(groups + moves::cancel_stride * entry++, c);
				out.banks.put16(groups + moves::cancel_stride * entry++, moves::group_return);
			}
			for (u32 m = 0; m < set.moves.size(); ++m)
			{
				const u32 move = base + 0x1000 + 0x40 * m;
				out.moves[bank].push_back(move);
				out.banks.put32(move + moves::cancels, cancel);
				for (const cancel_def& c : set.moves[m].cancels)
				{
					put(cancel, c);
					cancel += moves::cancel_stride;
				}
				out.banks.put16(cancel, moves::cancel_end);
				cancel += moves::cancel_stride;
			}
		}
		return out;
	}

	// A moveset with its layout.
	struct world
	{
		moveset_def set;
		layout lay;

		explicit world(moveset_def definition)
			: set(std::move(definition)), lay(lay_out(set))
		{
		}
	};

	// The hook a run uses.
	enum class hook
	{
		none,
		rule,
		forget_used,  // the rule, but it forgets the presses it used, so their copies in the stock words fire again
		without_view, // the rule, but the scan sees the stock direction fields
	};

	// The thrower's side of a hold, where a run plays it: the other port's word commits the held fighter's next move when
	// it presses `buttons` (held masks; none: at the first such frame) while the fighter runs move `from` within frames
	// first..last, as the thrower's scan of the tick before queues the pair and the commit phase commits it. A move the held
	// fighter queued itself goes first. The pair puts the thrower's actor in move `holder` from frame 1, where one is given.
	struct thrower_cancel
	{
		int from = 0;
		u16 buttons = 0;
		u16 first = 1, last = 0xffff;
		int target = 1;
		u16 entry = 1;
		int holder = -1;
	};

	struct scenario
	{
		const world* w = nullptr;
		int neutral = 0, held = 1; // the fighter's move before the grab, and the move the grab commits
		u32 grab = 12;
		u32 ticks = 120;
		std::vector<u16> samples{}; // raw words, indexed by the boundary at which the player's pad was read
		u32 delay = 0, lookahead = 0;
		hook hooks = hook::none;
		// Ticks with life 0, the game's no-fresh frame, a paused simulation, a step word Practice blanks, and input mode 0,
		// which sets +0x6c4 to the word instead of stepping (0xed79c).
		std::vector<u32> closed_gate{}, no_fresh{}, paused{}, blanked{}, edge_reset{};
		bool practice = false; // the words come from the rows as Practice keeps them: the word before the delay, 2048 rows
		bool shared = false;   // a mirror match in which both fighters run one loaded moveset
		bool computer = false;
		bool tech_rolls = false; // the developer switch that counts landing options as escapes
		u32 actor = side_0;
		u16 port = 0;
		u16 attacker_choice = 0;               // what requirement 208 compares with
		std::vector<thrower_cancel> thrower{}; // the other port's moves on the held fighter
		int holder = -1;                       // the thrower's move from the grab, in its actor
		// Ticks at which the thrower commits a move of its own moveset from a cancel that starts no pair (a walk, a throw
		// attempt), while the held fighter runs on.
		std::vector<std::pair<u32, int>> thrower_own{};
		bool snapshots = false;
	};

	struct outcome
	{
		std::vector<std::pair<u32, int>> commits; // from the grab on: the tick and the move
		const char* refusal = nullptr;
		u8 wanted = 0;
		bool matched = false;
		u16 guess = 0;
		bool slot_changed = false;  // a step left the slot other than the input update stored it
		std::vector<u64> snapshots; // the state memory after each tick
		std::vector<step_fields> history;
		std::vector<std::array<port_record, 2>> rows;

		bool operator==(const outcome& other) const
		{
			return commits == other.commits && !refusal == !other.refusal && wanted == other.wanted && matched == other.matched &&
			       guess == other.guess;
		}
		int last_move() const
		{
			return commits.empty() ? -1 : commits.back().second;
		}
		std::optional<u32> committed(int move) const
		{
			for (const auto& [tick, committed_move] : commits)
				if (committed_move == move)
					return tick;
			return std::nullopt;
		}
		// How many presses of each button the step fired.
		std::array<u32, 4> fired() const
		{
			std::array<u32, 4> out{};
			for (const step_fields& step : history)
				for (u32 button = 0; button < 4; ++button)
					out[button] += (step.fresh >> button) & 1;
			return out;
		}
	};

	bool says(const char* refusal, std::string_view expected)
	{
		return refusal && expected == refusal;
	}

	bool listed(const std::vector<u32>& ticks, u32 tick)
	{
		return std::find(ticks.begin(), ticks.end(), tick) != ticks.end();
	}

	// Practice keeps this many rows (practice_window).
	constexpr u32 practice_rows = 2048;

	bool sequence_matches(const sequence_def& sequence, const std::vector<step_fields>& history)
	{
		if (history.empty() || sequence.presses.empty())
			return false;
		const std::size_t newest = history.size() - 1;
		if ((history[newest].fresh & sequence.presses.back()) != sequence.presses.back())
			return false;
		s64 need = static_cast<s64>(sequence.presses.size()) - 2;
		for (std::size_t at = newest; need >= 0 && at-- > 0 && newest - at < sequence.frames;)
			if ((history[at].fresh & sequence.presses[need]) == sequence.presses[need])
				--need;
		return need < 0;
	}

	// The command matcher (0xedb38) on what the scan reads: the direction half against +0x6bc, and with 0x400 against
	// +0x6be too; then the button code (0x2000: one listed button fresh and all held; 0x1000: one fresh; neither: all fresh
	// on the frame; 0x4000: nothing fresh; 0x4001: nothing held), or a sequence read from the command history.
	bool command_matches(const cancel_def& c, const step_fields& now, const std::vector<step_fields>& history, const moveset_def& set)
	{
		if (c.direction >= moves::first_sequence)
			return sequence_matches(set.sequences.at(c.direction - moves::first_sequence), history);
		if (const u16 way = c.direction & 0x87ff)
		{
			if ((way & 0x400) && !(way & now.changed))
				return false;
			if (!(way & now.direction))
				return false;
		}
		const u16 code = c.buttons, listed_buttons = code & 0xf;
		if (code == 0)
			return true;
		if (code == 0x4000)
			return now.fresh == 0;
		if (code == 0x4001)
			return now.held == 0;
		if (code & 0x2000)
			return (now.fresh & listed_buttons) && (now.held & listed_buttons) == listed_buttons;
		if (code & 0x1000)
			return (now.fresh & listed_buttons) != 0;
		return (now.fresh & listed_buttons) == listed_buttons;
	}

	struct queued_move
	{
		int target;
		u16 at, entry;
	};

	// The thrown fighter's game state: what a checkpoint keeps and a correction restores.
	struct fighter
	{
		guest g;
		int current = -1;
		std::optional<queued_move> queued;
		std::vector<step_fields> history;
		u16 guess = 0;
		u16 thrower_last = 0, thrower_before = 0; // the other port's held masks at the last two steps
		bool ended = false;                       // the final move is committed
		const char* refusal = nullptr;
		bool slot_changed = false; // a step left the slot other than the input update stored it
		std::vector<std::pair<u32, int>> commits;

		// Everything a combat hash would see, and more: the state memory, the move, the queue and the input history.
		u64 digest() const
		{
			u64 hash = 0xcbf29ce484222325ull;
			const auto mix = [&](u64 value)
			{
				hash = (hash ^ value) * 0x100000001b3ull;
			};
			std::vector<std::size_t> order(g.state.pages.size());
			for (std::size_t at = 0; at < order.size(); ++at)
				order[at] = at;
			std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
				{
					return g.state.pages[a].first < g.state.pages[b].first;
				});
			for (const std::size_t at : order)
			{
				const auto& [page, bytes] = g.state.pages[at];
				std::array<u64, 512> words{};
				std::memcpy(words.data(), bytes.data(), bytes.size());
				if (std::all_of(words.begin(), words.end(), [](u64 word)
						{
							return word == 0;
						}))
					continue;
				mix(page);
				for (const u64 word : words)
					mix(word);
			}
			mix(static_cast<u64>(current));
			mix(queued ? (u64{static_cast<u32>(queued->target)} << 32 | u64{queued->at} << 16 | queued->entry) : ~u64{0});
			mix(u64{guess} << 32 | u64{thrower_last} << 16 | thrower_before);
			mix(u64{ended} << 1 | u64{refusal != nullptr});
			for (const step_fields& step : history)
				mix(u64{step.fresh} | u64{step.held} << 8 | u64{step.direction} << 16 | u64{step.changed} << 32);
			for (const auto& [tick, move] : commits)
				mix(u64{tick} << 32 | static_cast<u32>(move));
			return hash;
		}
	};

	// A commit (0xf26f0..0xf278c): the move runs from `frame` with the class word it was queued with in +0x15a, and the
	// queued class is cleared.
	void commit(const scenario& c, fighter& f, int move, u16 frame, u16 klass)
	{
		const int bank = c.shared ? 0 : c.w->set.moves[move].bank;
		memory& state = f.g.state;
		state.put32(c.actor + actor::move, c.w->lay.moves[bank][move]);
		state.put32(c.actor + actor::move_moveset, layout::base[bank]);
		state.put16(c.actor + actor::frame, frame);
		state.put16(c.actor + actor::move_class, klass);
		state.put16(c.actor + queued_class, 0);
		state.put8(c.actor + wanted_offset, 0);
		state.put8(c.actor + matched_offset, 0);
		f.current = move;
	}

	// The other fighter's actor: the thrower's.
	u32 thrower_actor(const scenario& c)
	{
		return c.actor == side_0 ? side_1 : side_0;
	}

	// A pair (0x17ae4..0x17b04): the held fighter commits `move` with the reaction's class word (0xf015c), and the thrower's
	// actor `holder` of the thrower's moveset from frame 1 with the pair's, where one is given.
	void give(const scenario& c, fighter& f, int move, u16 frame, int holder)
	{
		commit(c, f, move, frame, reaction_class);
		if (holder < 0)
			return;
		memory& state = f.g.state;
		const u32 other = thrower_actor(c);
		const int bank = c.shared ? 0 : 1;
		state.put32(other + actor::moveset, layout::base[bank]);
		state.put32(other + actor::move_moveset, layout::base[bank]);
		state.put32(other + actor::move, c.w->lay.moves[bank][holder]);
		state.put16(other + actor::frame, 1);
		state.put16(other + actor::move_class, pair_class);
	}

	fighter start(const scenario& c)
	{
		fighter f;
		f.g.banks = &c.w->lay.banks;
		memory& state = f.g.state;
		const u32 a = c.actor;
		state.put32(a + actor::controller, c.computer ? 0x01000000u : a == side_1 ? 0x00000101u :
																					0u);
		state.put16(a + actor::port, c.port);
		state.put32(a + actor::moveset, layout::base[0]);
		state.put32(a + actor::life, 0x00910000u);
		state.put32(a + actor::direction_held, 0x00200001u);
		if (c.practice)
			state.put32(game_mode, practice_mode);
		commit(c, f, c.neutral, 1, 0);
		return f;
	}

	// One tick of the thrown fighter in guest memory, in the game's update order (0x15458): the input update stores the
	// word the tick runs (`executed`, raw) in the slot; then commit and frame advance; then the input step: the no-fresh
	// mark, the word load (the rule's first probe), held and pressed from the slot and +0x6c4, Practice's blank and the life
	// gate, the direction fields, the latch entry (the second probe), the latch, the command fields and history, +0x6c4
	// takes held; then the cancel scan between its entry and exit probes. `held_at(port, source)` gives the words the rule
	// reads, and `thrower` the other port's raw word.
	template <typename Words>
	void tick(const scenario& c, fighter& f, span_cache& spans, std::array<port_record, 2>& row, u32 t, u16 executed,
		const std::array<u32, 2>& ahead, Words&& held_at, u16 thrower = 0)
	{
		if (f.ended || f.refusal)
			return;
		const moveset_def& set = c.w->set;
		guest& g = f.g;
		memory& state = g.state;
		const u32 a = c.actor;
		const u16 port = c.port;
		auto& record = row[port];
		const auto span_of = [&](u32 move, u32 moveset)
		{
			return spans.get(g, move, moveset);
		};
		state.put32(slot_of(port), held_mask(executed));
		if (listed(c.paused, t))
			return;
		// Commit phase: the thrower's frame advances, and a pair or a move of its own puts it back at 1.
		if (const u32 other = thrower_actor(c); read32(g, other + actor::move))
			state.put16(other + actor::frame, static_cast<u16>(read16(g, other + actor::frame) + 1));
		for (const auto& [at, move] : c.thrower_own)
			if (at == t)
			{
				const u32 other = thrower_actor(c);
				state.put32(other + actor::move, c.w->lay.moves[c.shared ? 0 : 1][move]);
				state.put16(other + actor::frame, 1);
				state.put16(other + actor::move_class, 0x0080);
			}
		const u16 frame = read16(g, a + actor::frame);
		const u16 thrower_pressed = static_cast<u16>(f.thrower_last & ~f.thrower_before);
		const auto pair = std::find_if(c.thrower.begin(), c.thrower.end(), [&](const thrower_cancel& p)
			{
				return p.from == f.current && p.first <= frame && frame <= p.last && (thrower_pressed & p.buttons) == p.buttons;
			});
		if (t == c.grab)
		{
			f.queued.reset();
			give(c, f, c.held, 1, c.holder);
			f.commits.push_back({t, c.held});
		}
		else if (f.queued && frame >= f.queued->at)
		{
			commit(c, f, f.queued->target, f.queued->entry, read16(g, a + queued_class));
			f.commits.push_back({t, f.queued->target});
			f.queued.reset();
			if (set.moves[f.current].final)
			{
				f.ended = true;
				return;
			}
		}
		else if (!f.queued && pair != c.thrower.end())
		{
			give(c, f, pair->target, pair->entry, pair->holder);
			f.commits.push_back({t, pair->target});
			if (set.moves[f.current].final)
			{
				f.ended = true;
				return;
			}
		}
		else
			state.put16(a + actor::frame, static_cast<u16>(frame + 1));
		// Input step.
		if (listed(c.edge_reset, t))
			state.put32(a + actor::previous_held, held_mask(executed));
		else
		{
			const bool gate = !listed(c.closed_gate, t);
			state.put32(a + actor::life, gate ? 0x00910000u : 0u);
			if (listed(c.no_fresh, t))
				state.put32(a + actor::previous_held, no_fresh_mark);
			if (c.hooks != hook::none && !c.computer)
			{
				if (c.hooks == hook::forget_used)
				{
					record.escape.early = {};
					record.escape.used_held = 0;
				}
				step_plan plan = at_word_load(g, a, port, t, ahead, row, c.tech_rolls, span_of, held_at);
				if (plan.refusal)
				{
					f.refusal = plan.refusal;
					return;
				}
				if (c.hooks == hook::without_view)
					plan.view.reset();
				take_plan(record, plan);
				if (plan.slot)
					state.put32(slot_of(port), *plan.slot);
				if (plan.previous_held)
					state.put32(a + actor::previous_held, *plan.previous_held);
			}
			const u32 previous = read32(g, a + actor::previous_held);
			u16 held = static_cast<u16>(read32(g, slot_of(port)));
			u16 pressed = static_cast<u16>(held & ~previous);
			if (listed(c.blanked, t) || !gate)
				held = pressed = 0;
			const u32 fields = direction_fields(held, pressed, previous, read16(g, a + actor::direction_held), g(a + actor::facing));
			note_step(record, read_actor(g, a), held);
			if (c.hooks != hook::none)
			{
				if (const char* refusal = check_latch_entry(g, a, t, ahead, row, c.tech_rolls, {held, pressed}, span_of))
				{
					f.refusal = refusal;
					return;
				}
				if (record.loaded && record.slot != record.stock)
					state.put32(slot_of(port), record.stock);
			}
			f.slot_changed = f.slot_changed || read32(g, slot_of(port)) != held_mask(executed);
			if (!(g(a + wanted_offset) && g(a + latch_offset)))
				state.put8(a + latch_offset, static_cast<u8>(classify(held, pressed)));
			state.put32(a + actor::direction_held, fields);
			f.history.push_back({nibble(pressed), nibble(held), static_cast<u16>(fields >> 16), static_cast<u16>(fields)});
			state.put32(a + actor::previous_held, held);
		}
		f.thrower_before = f.thrower_last;
		f.thrower_last = held_mask(thrower);
		// Cancel scan, skipped while a move is queued (0x19704), between the scan probes.
		if (f.queued)
			return;
		const bool view = c.hooks != hook::none && record.loaded && record.view;
		const u32 stock_fields = read32(g, a + actor::direction_held);
		if (view)
			state.put32(a + actor::direction_held, record.view_fields);
		const u16 now_frame = read16(g, a + actor::frame);
		step_fields now = f.history.empty() ? step_fields{} : f.history.back();
		now.direction = read16(g, a + actor::direction_held);
		now.changed = read16(g, a + actor::direction_changed);
		const auto consider = [&](const cancel_def& cancel)
		{
			if (now_frame < cancel.first || now_frame > cancel.last || (cancel.option & moves::option_skipped))
				return false;
			const bool command = cancel.direction || cancel.buttons;
			if (command && !(cancel.option & moves::option_unread) && !command_matches(cancel, now, f.history, set))
				return false;
			for (const auto& [id, parameter] : cancel.requirements)
			{
				if (moves::is_break(id))
				{
					const auto wanted = static_cast<u8>(id - moves::break_one + 1);
					state.put8(a + wanted_offset, wanted);
					state.put8(a + matched_offset, g(a + latch_offset) == wanted);
					if (!g(a + matched_offset))
						return false;
				}
				else if (id == 0x8060)
					f.guess = parameter;
				else if (id == 208 && c.attacker_choice != parameter)
					return false;
			}
			// A queue of the fighter's own writes the cancel's class word (0xefee8).
			f.queued = queued_move{cancel.target, cancel.start, cancel.entry};
			state.put16(a + queued_class, static_cast<u16>(cancel.extra & 0x3fff));
			return true;
		};
		for (const cancel_def& cancel : set.moves[f.current].cancels)
		{
			bool matched = false;
			if (cancel.direction == moves::group_jump)
			{
				for (const cancel_def& entry : set.groups.at(cancel.group))
					if ((matched = consider(entry)))
						break;
			}
			else
				matched = consider(cancel);
			if (matched)
				break;
		}
		if (view)
			state.put32(a + actor::direction_held, stock_fields);
	}

	// One fighter's run on one PC. A session holds the words each tick runs: tick t runs the sample of boundary t - delay,
	// and at tick t the words through t + delay are fixed.
	outcome run(const scenario& c)
	{
		fighter f = start(c);
		const u16 port = c.port;
		const auto sampled = [&](s64 boundary) -> u16
		{
			return boundary >= 0 && boundary < static_cast<s64>(c.samples.size()) ? c.samples[boundary] : u16{0};
		};
		const auto executed = [&](u32 tick)
		{
			return sampled(static_cast<s64>(tick) - c.delay);
		};
		std::vector<std::array<port_record, 2>> rows(c.ticks);
		span_cache spans;
		std::array<u32, 2> ahead{};
		ahead[port] = c.lookahead;
		outcome out;
		for (u32 t = 0; t < c.ticks; ++t)
		{
			auto& row = rows[t];
			auto& record = row[port];
			if (c.practice)
			{
				record.practice_word = sampled(t);
				record.practice_delay = static_cast<u8>(c.delay);
				if (t >= practice_rows)
					rows[t - practice_rows] = {};
			}
			carry(record, t ? &rows[t - 1][port] : nullptr, t);
			const auto held_at = [&](u16 at_port, u32 source) -> std::optional<u16>
			{
				if (at_port != port)
					return std::nullopt;
				if (c.practice)
				{
					const auto word = practice_word_at(record, t, source - ahead[port]);
					return word ? std::optional<u16>{held_mask(*word)} : std::nullopt;
				}
				if (source > t + c.delay)
					return std::nullopt;
				return held_mask(executed(source));
			};
			tick(c, f, spans, row, t, executed(t), ahead, held_at);
			if (f.ended || f.refusal)
				break;
			if (c.snapshots)
				out.snapshots.push_back(f.g.state.checksum());
		}
		out.commits = f.commits;
		out.refusal = f.refusal;
		out.slot_changed = f.slot_changed;
		out.wanted = f.g(c.actor + wanted_offset);
		out.matched = f.g(c.actor + matched_offset) != 0;
		out.guess = f.guess;
		out.history = std::move(f.history);
		if (!c.practice)
			out.rows = std::move(rows);
		return out;
	}

	struct press
	{
		s64 at = 0;
		u16 buttons = 0;
		u32 frames = 1;
	};

	std::vector<u16> timeline(const std::vector<press>& presses, u32 length = 400)
	{
		std::vector<u16> words(length, 0);
		for (const press& p : presses)
			for (s64 b = p.at; b < p.at + static_cast<s64>(p.frames); ++b)
				if (b >= 0 && b < static_cast<s64>(length))
					words[b] |= p.buttons;
		return words;
	}

	scenario online(scenario c, u32 delay, u32 lookahead, hook hooks = hook::rule)
	{
		c.delay = delay;
		c.lookahead = lookahead;
		c.hooks = hooks;
		return c;
	}

	// The press patterns a sweep tries at boundary k: one press, a short and a long hold, the wrong press first, the wrong
	// press held into the right one, a wrong press held from before the grab plus the right one, the right buttons one
	// at a time a frame apart, and mashing.
	std::vector<std::vector<press>> patterns(u16 right, u16 wrong, s64 k, s64 grab)
	{
		std::vector<std::vector<press>> out{
			{{k, right, 1}},
			{{k, right, 3}},
			{{k, right, 40}},
			{{k, wrong, 1}, {k + 2, right, 1}},
			{{k, wrong, 2}, {k + 1, right, 2}},
			{{grab - 8, wrong, 60}, {k, right, 1}},
		};
		const u16 lowest = static_cast<u16>(right & (~right + 1));
		if (right != lowest)
			out.push_back({{k, lowest, 2}, {k + 1, static_cast<u16>(right & ~lowest), 2}});
		std::vector<press> mash;
		for (s64 b = k; b < k + 30; b += 2)
			mash.push_back({b, (b - k) % 4 == 0 ? right : wrong, 1});
		out.push_back(mash);
		return out;
	}

	// How many times each button rises in the words.
	std::array<u32, 4> rises(const std::vector<u16>& samples)
	{
		std::array<u32, 4> out{};
		for (std::size_t b = 0; b < samples.size(); ++b)
			for (u32 button = 0; button < 4; ++button)
				out[button] += (samples[b] & button_bits[button]) && !(b && (samples[b - 1] & button_bits[button]));
		return out;
	}

	bool fires_at_most_once(const outcome& o, const std::vector<u16>& samples)
	{
		const auto fired = o.fired(), pressed = rises(samples);
		for (u32 button = 0; button < 4; ++button)
			if (fired[button] > pressed[button])
				return false;
		return true;
	}

	// A case and what two runs of it committed, for a failed check.
	std::string described(const std::vector<press>& presses, u32 delay, const outcome& got, const outcome& wanted)
	{
		std::string out = " (D " + std::to_string(delay) + ", presses";
		for (const press& p : presses)
			out += " " + std::to_string(p.at) + ":" + std::to_string(p.buttons) + "x" + std::to_string(p.frames);
		for (const outcome* o : {&got, &wanted})
		{
			out += o == &got ? "; ran" : "; wanted";
			for (const auto& [tick, move] : o->commits)
				out += " " + std::to_string(tick) + ":" + std::to_string(move);
		}
		return out + ")";
	}

	struct sweep_counts
	{
		u32 compared = 0, differ_without = 0;
	};

	// For every pattern at every boundary from `first` (10 before the grab) to `last` frames after the grab and every D from
	// 0 to 6: online with the rule runs as offline, on the same ticks, and Practice does from its words before the delay,
	// presses made too early for a window included. No press ever fires twice. Without the rule the delay changes some
	// outcomes.
	template <typename Patterns>
	sweep_counts sweep(const scenario& base, s64 last, Patterns&& patterns_at, s64 first = -10)
	{
		sweep_counts counts;
		scenario offline = base;
		const s64 grab = base.grab;
		for (s64 k = grab + first; k <= grab + last; ++k)
			for (const auto& presses : patterns_at(k))
			{
				offline.samples = timeline(presses);
				const outcome expected = run(offline);
				check(!expected.refusal && !expected.commits.empty(), "the offline run");
				for (u32 delay = 0; delay <= 6; ++delay)
				{
					const outcome with_rule = run(online(offline, delay, delay));
					check(!with_rule.refusal && !with_rule.slot_changed, "every word the rule reads is at hand; the slot ends as stored");
					check(fires_at_most_once(with_rule, offline.samples), "no press fires twice");
					check(with_rule == expected, "online with the rule runs as offline, on the same ticks" + described(presses, delay, with_rule, expected));
					if (delay == 3)
					{
						scenario practice = online(offline, delay, delay);
						practice.practice = true;
						check(run(practice) == expected, "Practice, from the words before its delay, runs as offline");
					}
					if (delay)
						counts.differ_without += !(run(online(offline, delay, 0)) == expected);
					++counts.compared;
				}
			}
		return counts;
	}

	// A hold with one escape cancel, then an automatic fall into a thrown end: 0 standing (own), 1 the hold, 2 escaped,
	// 3 thrown.
	world single_escape(cancel_def escape, u16 thrown_at)
	{
		escape.target = 2;
		moveset_def set;
		set.moves = {{0}, {1, {escape, automatic(3, thrown_at)}}, {1, {}, true}, {1, {}, true}};
		return world(set);
	}

	// Window shapes of the shipped movesets, including both windows of a chain throw stage.
	struct window
	{
		u16 first = 0, last = 0, start = 0;
		break_class wanted = break_class::none;
	};

	const std::vector<std::vector<window>> shapes{
		{{1, 15, 15, break_class::one}},
		{{1, 15, 1, break_class::two}},
		{{1, 10, 11, break_class::both}},
		{{1, 5, 1, break_class::one}},
		{{10, 23, 24, break_class::two}},
		{{1, 23, 24, break_class::one}},
		{{1, 15, 1, break_class::one}, {91, 99, 100, break_class::one}},
	};

	u16 button_for(break_class wanted)
	{
		return wanted == break_class::one ? one : wanted == break_class::two ? two :
		                                                                       both;
	}

	u16 wrong_for(break_class wanted)
	{
		return wanted == break_class::one ? two : one;
	}

	// A throw: 0 standing, 1 the hold with its break windows, 2 broken, 3 thrown at frame 200.
	world throw_world(const std::vector<window>& windows)
	{
		moveset_def set;
		move_def hold{1};
		for (const window& w : windows)
			hold.cancels.push_back(break_cancel(w.wanted, 2, w.first, w.last, w.start));
		hold.cancels.push_back(automatic(3, 200));
		set.moves = {{0}, hold, {1, {}, true}, {1, {}, true}};
		return world(set);
	}

	scenario base_of(const world& w, u32 ticks = 120)
	{
		scenario c;
		c.w = &w;
		c.ticks = ticks;
		return c;
	}

	// Ganryu's Kumite Tsukami follow-ups: Tasukizori (3), Shumokuzori (4) and Tsuri Otoshi (3+4). A wrong button restarts the
	// same move at frame 21, past the 1..11 window, so it locks the escape out. 0 standing, 1-3 the holds, 4 escaped,
	// 5 thrown.
	world ganryu_world()
	{
		moveset_def set;
		const auto hold = [](u16 right, u16 wrong, int self)
		{
			cancel_def restart = on(0, static_cast<u16>(0x1000 | wrong), self, 1, 11);
			restart.entry = 21;
			return move_def{1, {on(0, right, 4, 1, 11), restart, automatic(5, 40)}};
		};
		set.moves = {{0}, hold(0x4, 0xb, 1), hold(0x8, 0x7, 2), hold(0xc, 0x3, 3), {1, {}, true}, {1, {}, true}};
		return world(set);
	}

	// As the shipped Ultimate Tackles: 1 the tackle reads 2 in 1..4 (escape) and 1+2 in 18..19 (reversal); the thrower's
	// follow-ups put the held fighter in 6, a punch that reads nothing, then in 2, the arm lock (by frame `lock_at` of the
	// punch), where 2 four times then 1+2 within 59 frames escapes in 18..65 and six times reverses in 1..60. 0 standing,
	// 3 thrown, 4 escaped, 5 reversed.
	world tackle_world(u16 lock_at)
	{
		moveset_def set;
		set.sequences = {{59, {2, 2, 2, 2, 2, 2, 3}}, {59, {2, 2, 2, 2, 3}}};
		set.moves = {
			{0},
			{1, {on(0, 0x2, 4, 1, 4, 10), on(0, 0x3, 5, 18, 19, 20), automatic(6, 22)}},
			{1, {on(0x8007, 0, 5, 1, 60), on(0x8008, 0, 4, 18, 65), automatic(3, 70)}},
			{1, {}, true},
			{1, {}, true},
			{1, {}, true},
			{1, {automatic(2, lock_at)}},
		};
		return world(set);
	}

	// A mount held as long as Marduk's, whose escapes read input up to frame 250 and which throws at frame 255: back with
	// 1+2 together, or 2 four times then 1+2 within 59 frames, escapes in 30..250. The thrower's punch (move 4) reads
	// nothing and returns to the mount at frame 100. 0 standing, 1 the mount, 2 escaped, 3 thrown.
	world mount_world()
	{
		moveset_def set;
		set.sequences = {{59, {2, 2, 2, 2, 3}}};
		cancel_def back_to_mount = automatic(1, 12);
		back_to_mount.entry = 100;
		set.moves = {{0}, {1, {on(0x0010, 0x3, 2, 30, 250), on(0x8007, 0, 2, 30, 250), automatic(3, 255)}}, {1, {}, true}, {1, {}, true},
			{1, {back_to_mount}}};
		return world(set);
	}

	// Jack-5's u/f+1+2: the lift (move 1) breaks on 1+2 in 1..15; the thrower's d/f+2 (here at frame 20 of the lift, or the
	// thrower's own press where a run plays it) puts the held fighter in pj_lfp_yxk (move 3), still in Jack's moveset, where
	// any held direction in frames 10..28 locks the block out (Test_grdF, move 4), back held in 29..30 blocks (Test_grd2,
	// move 5), and frame 31 ends in Test_grdF too. 0 standing, 2 broken.
	world jack_world(bool thrower_follows = false)
	{
		moveset_def set;
		move_def lift{1, {break_cancel(break_class::both, 2, 1, 15, 16)}};
		if (!thrower_follows)
			lift.cancels.push_back(automatic(3, 20));
		lift.cancels.push_back(automatic(4, 90));
		set.moves = {{0}, lift, {1, {}, true}, {1, {on(0x03de, 0, 4, 10, 28), on(0x0010, 0, 5, 29, 30), automatic(4, 31)}}, {1, {}, true},
			{1, {}, true}};
		return world(set);
	}

	// A pair's cancel in the thrower's move: class 0x1800, frames first..last, the target committing at `start`.
	cancel_def follow_up(u16 first, u16 last, u16 start)
	{
		cancel_def cancel = on(0, 0x2003, -1, first, last, start);
		cancel.extra = 0x1ad6;
		return cancel;
	}

	// As the shipped Ultimate Tackles (Paul's Ul_tacF, Ul_tacUdeh, Co_tp): the tackle mounts the held fighter (1), which
	// escapes with 2 in 1..4 and reverses with 1+2 in 18..19; the thrower's mount (9) can start a pair in frames 1..44 that
	// commits at 45: the arm lock (2), where six 2s then 1+2 within 59 frames reverse in 1..60 and four in 18..65 escape; the
	// right punch (3), where 1, 1, 2 within 30 frames escape in 1..20 and 1 breaks in 10..23 (sf 24); or the left-right
	// punch (4), which reads nothing while the thrower's punch (10) can start the arm lock in 1..17. A sequence escape
	// commits on the next tick, so its tick is the one it matched on. 0 standing, 5 escaped, 6 reversed, 7 thrown or up,
	// 8 broken.
	world mount_chain_world()
	{
		moveset_def set;
		set.sequences = {{59, {2, 2, 2, 2, 2, 2, 3}}, {59, {2, 2, 2, 2, 3}}, {30, {1, 1, 2}}};
		set.moves = {
			{0},
			{1, {on(0, 0x2002, 5, 1, 4, 10), on(0, 0x2003, 6, 18, 19, 20), automatic(7, 120)}},
			{1, {on(0x8007, 0, 6, 1, 60), on(0x8008, 0, 5, 18, 65), automatic(7, 95)}},
			{1, {on(0x8009, 0, 5, 1, 20), break_cancel(break_class::one, 8, 10, 23, 24), automatic(7, 44)}},
			{1, {automatic(7, 60)}},
			{1, {}, true},
			{1, {}, true},
			{1, {}, true},
			{1, {}, true},
			{1, {follow_up(1, 44, 45)}},
			{1, {follow_up(1, 17, 18)}},
			{1, {follow_up(1, 121, 122)}},
			{1, {}},
		};
		return world(set);
	}

	// The thrower's pairs from the mount: the arm lock, the right punch, or the left-right punch and then the arm lock.
	enum class mount_path
	{
		lock,
		punch,
		punches_then_lock,
	};

	scenario mount_chain(const world& w, mount_path path, u32 ticks = 160)
	{
		scenario c = base_of(w, ticks);
		c.held = 1;
		c.holder = 9;
		if (path == mount_path::lock)
			c.thrower = {{1, 0, 44, 44, 2, 1, 11}};
		else if (path == mount_path::punch)
			c.thrower = {{1, 0, 44, 44, 3, 1, 12}};
		else
			c.thrower = {{1, 0, 44, 44, 4, 1, 10}, {4, 0, 17, 17, 2, 1, 11}};
		return c;
	}

	// Two PCs playing a hold over a lossy link, as the peer boundary runs it (pbd_peer_boundary_steps.h) with each received
	// word counted once it arrives (PBD_PEER_USE_ARRIVED). Each PC owns one port, fixes its word D ticks ahead and sends the
	// newest 16 fixed words every frame, also while it holds; when the other PC's report of what it confirmed has not moved
	// for 15 frames and lies before that window, every other datagram carries the 16 words after it (append_outgoing_inputs).
	// It holds while its first unconfirmed row is max_prediction behind, or in lockstep until every earlier row is confirmed
	// and this tick's word is in, with the word L past it while the other PC's fighter may read ahead; runs a missing word as
	// the last one received, except in lockstep, which is final once it runs and refuses a word read ahead that has not
	// arrived; corrects from the first row that ran on a wrong word, from a ring of 9 checkpoints; publishes only what it
	// confirmed; and at the end waits at a segment barrier, where a misprediction is fatal. A PC past the barrier has left the
	// segment: its words from there count as neutral (remote_left), but a word read ahead counts only once it arrives. Both
	// PCs read the held fighter's port L = D ahead. `confirm_reads_ahead` off confirms as if no row read ahead,
	// `lockstep_waits_ahead` off runs a lockstep row without the word L past it, and `read_ahead_needs_arrival` off counts a
	// word read ahead past the other PC's exit as it counts the row's own.
	struct link_profile
	{
		u32 latency = 1, jitter = 0, drop = 0; // frames one way, up to that many more at random, percent of datagrams lost
		u32 outage_from = 0, outage_to = 0;    // frames in which every datagram from the PC of port 0 is lost
	};

	struct pair_setup
	{
		scenario c;                              // the held fighter: world, port, actor and the ticks before the barrier
		u32 delay = 0;                           // D, and L on both PCs
		std::array<std::vector<u16>, 2> samples; // raw words by the boundary at which each port's pad was read
		link_profile link;
		u32 max_prediction = 5; // the launcher's prediction bound
		u32 lockstep_from = 0;  // the first boundary that runs in lockstep (the stretch after the fight)
		u64 seed = 1;
		bool confirm_reads_ahead = true, lockstep_waits_ahead = true, read_ahead_needs_arrival = true;
	};

	struct pair_result
	{
		const char* fault = nullptr;
		std::array<std::vector<std::pair<u32, int>>, 2> commits;
		u32 corrections = 0, read_ahead_corrections = 0, deepest = 0, holds = 0;
		// What both PCs confirmed, tick by tick: the words each port ran (a replay's input rows and a watcher's frames), the
		// state after the tick and the rule's record of it.
		std::vector<std::array<u16, 2>> words;
		std::vector<u64> digests;
		std::vector<std::array<port_record, 2>> rows;
	};

	constexpr u32 checkpoint_ring = 9, replay_slots = 8;

	struct datagram
	{
		u32 arrives = 0;
		s64 confirmed = -1; // the newest of the receiver's ticks the sender has confirmed
		std::vector<std::pair<u32, u16>> words;
	};

	struct peer_side
	{
		u32 role = 0;
		fighter now; // the state before tick `next`
		std::array<std::optional<std::pair<u32, fighter>>, checkpoint_ring> checkpoints;
		std::vector<std::array<port_record, 2>> rows;
		std::vector<u16> ran; // the other port's word each row ran with
		std::vector<u8> arrived;
		std::vector<u16> received;
		std::vector<u64> digests; // after each tick, as last run
		std::vector<std::optional<u64>> confirmed, published;
		std::vector<datagram> inbox;
		span_cache spans;
		u32 next = 0, scan_from = 0, confirmed_through = 0, published_through = 0;
		s64 remote_confirmed = -1;
		u32 remote_confirmed_at = 0;
		bool resend_turn = false;
		bool done = false;
	};

	// Two records of one step read the same words ahead.
	bool same_use(const lookahead_use& a, const lookahead_use& b)
	{
		return a.mask == b.mask && a.no_fresh == b.no_fresh && a.source == b.source && a.word == b.word &&
		       a.uses_previous == b.uses_previous && a.previous_source == b.previous_source && a.previous_word == b.previous_word;
	}

	pair_result play_pair(const pair_setup& s)
	{
		const scenario& c = s.c;
		const u32 ticks = c.ticks, extent = ticks + s.delay + 1;
		const std::array<u32, 2> ahead{s.delay, s.delay};
		pair_result result;
		u64 random = s.seed * 0x9e3779b97f4a7c15ull | 1;
		const auto next_random = [&]
		{
			random ^= random << 13;
			random ^= random >> 7;
			random ^= random << 17;
			return random;
		};
		// Port p's word at tick t: its pad read at boundary t - D.
		const auto word = [&](u32 port, u32 t) -> u16
		{
			return t >= s.delay && t - s.delay < s.samples[port].size() ? s.samples[port][t - s.delay] : u16{0};
		};
		std::array<peer_side, 2> pcs;
		for (u32 role = 0; role < 2; ++role)
		{
			peer_side& p = pcs[role];
			p.role = role;
			p.now = start(c);
			p.rows.resize(extent);
			p.ran.resize(extent);
			p.arrived.resize(extent);
			p.received.resize(extent);
			p.digests.resize(extent);
			p.confirmed.resize(extent);
			p.published.resize(extent);
		}
		// The other PC has left the segment once it is past the barrier at `ticks`: its words from there count as neutral.
		const auto left = [&](const peer_side& p, u32 t)
		{
			return pcs[1 - p.role].done && t >= ticks;
		};
		const auto usable = [&](const peer_side& p, u32 t)
		{
			return t < extent && (p.arrived[t] != 0 || left(p, t));
		};
		const auto usable_ahead = [&](const peer_side& p, u32 t)
		{
			return t < extent && (p.arrived[t] != 0 || (!s.read_ahead_needs_arrival && left(p, t)));
		};
		const auto remote_for = [&](const peer_side& p, u32 t) -> u16
		{
			if (t < extent && !p.arrived[t] && left(p, t))
				return 0;
			for (u32 k = std::min(t + 1, extent); k-- > 0;)
				if (p.arrived[k])
					return p.received[k];
			return 0;
		};
		const auto remote_use = [](const peer_side& p, u32 t) -> const lookahead_use&
		{
			return p.rows[t][1 - p.role].use;
		};
		const auto execute = [&](peer_side& p, u32 t, u32 boundary)
		{
			p.checkpoints[t % checkpoint_ring] = std::pair{t, p.now};
			auto& row = p.rows[t];
			row = {};
			for (u32 port = 0; port < 2; ++port)
				carry(row[port], t ? &p.rows[t - 1][port] : nullptr, t);
			p.ran[t] = remote_for(p, t);
			std::array<u16, 2> words{};
			words[p.role] = word(p.role, t);
			words[1 - p.role] = p.ran[t];
			// The own port's words are fixed through the boundary + D; the other port's are what has arrived, or the prediction,
			// which a lockstep row never runs.
			const auto held_at = [&](u16 port, u32 source) -> std::optional<u16>
			{
				if (port == p.role)
					return source <= boundary + s.delay ? std::optional<u16>{held_mask(word(port, source))} : std::nullopt;
				if (t >= s.lockstep_from && !(source < extent && p.arrived[source]))
					return std::nullopt;
				return held_mask(remote_for(p, source));
			};
			tick(c, p.now, p.spans, row, t, words[c.port], ahead, held_at, words[1 - c.port]);
			p.digests[t] = p.now.digest();
		};
		const auto confirm = [](peer_side& p, u32 through)
		{
			for (; p.confirmed_through < through; ++p.confirmed_through)
				p.confirmed[p.confirmed_through] = p.digests[p.confirmed_through];
		};
		const auto publish = [](peer_side& p, u32 through)
		{
			for (; p.published_through < through; ++p.published_through)
				p.published[p.published_through] = p.digests[p.published_through];
		};
		// The boundary's scan: the first row from scan_from this PC cannot confirm, and the first that ran on a wrong word of
		// the other PC (`end` when none).
		const auto first_unconfirmed_row = [&](const peer_side& p, u32 end)
		{
			u32 unusable = p.scan_from;
			while (unusable < end && usable(p, unusable))
				++unusable;
			if (!s.confirm_reads_ahead)
				return unusable;
			return first_unconfirmed(p.scan_from, unusable, [&](u32 t) -> const lookahead_use&
				{
					return remote_use(p, t);
				},
				[&](u32 t)
				{
					return usable_ahead(p, t);
				});
		};
		const auto first_wrong_row = [&](const peer_side& p, u32 end, bool& read_ahead)
		{
			const auto received = [&](u32 t) -> std::optional<u16>
			{
				return usable_ahead(p, t) ? std::optional<u16>{held_mask(p.received[t])} : std::nullopt;
			};
			for (u32 t = p.scan_from; t < end; ++t)
			{
				read_ahead = s.confirm_reads_ahead && known_misprediction(remote_use(p, t), received);
				if (read_ahead || (usable(p, t) && p.ran[t] != p.received[t]))
					return t;
			}
			return end;
		};
		const auto boundary = [&](peer_side& p, peer_side& other, u32 step)
		{
			for (auto at = p.inbox.begin(); at != p.inbox.end();)
			{
				if (at->arrives > step)
				{
					++at;
					continue;
				}
				if (at->confirmed > p.remote_confirmed)
				{
					p.remote_confirmed = at->confirmed;
					p.remote_confirmed_at = step;
				}
				for (const auto& [t, value] : at->words)
				{
					if (!p.arrived[t])
					{
						p.arrived[t] = 1;
						p.received[t] = value;
					}
					else if (p.received[t] != value)
						result.fault = "peer_conflicting_duplicate";
				}
				at = p.inbox.erase(at);
			}
			const u32 index = p.next;
			datagram sent;
			sent.arrives = step + s.link.latency + (s.link.jitter ? static_cast<u32>(next_random() % (s.link.jitter + 1)) : 0u);
			sent.confirmed = static_cast<s64>(p.confirmed_through) - 1;
			u32 last = std::min(index + s.delay, extent - 1), first = last >= 15 ? last - 15 : 0;
			const s64 needed = p.remote_confirmed + 1;
			if (step - p.remote_confirmed_at > 15 && needed < first && (p.resend_turn = !p.resend_turn))
			{
				first = static_cast<u32>(needed);
				last = std::min(last, first + 15);
			}
			for (u32 t = first; t <= last; ++t)
				sent.words.push_back({t, word(p.role, t)});
			const bool outage = p.role == 0 && step >= s.link.outage_from && step < s.link.outage_to;
			if (next_random() % 100 >= s.link.drop && !outage)
				other.inbox.push_back(std::move(sent));
			if (p.done)
				return;
			const u32 u = first_unconfirmed_row(p, index);
			bool read_ahead = false;
			if (index == ticks)
			{
				// The segment barrier: nothing before it can run again.
				if (first_wrong_row(p, index, read_ahead) < index)
					result.fault = "session_barrier_after_misprediction";
				else if (u < index)
					++result.holds;
				else
				{
					p.scan_from = index;
					confirm(p, index);
					publish(p, index);
					p.done = true;
				}
				return;
			}
			// Lockstep: the word L past the row too while the other PC's fighter may read ahead there.
			const bool ahead_in = !s.lockstep_waits_ahead || !index || lookahead_arrived(p.rows[index - 1][1 - p.role], index, s.delay, [&](u32 source)
																		   {
																			   return source >= extent || p.arrived[source];
																		   });
			if (index >= s.lockstep_from ? !(u >= index && usable(p, index) && ahead_in) : index - u >= s.max_prediction)
			{
				++result.holds;
				return;
			}
			const u32 m = first_wrong_row(p, index, read_ahead);
			if (m < index)
			{
				const u32 depth = index - m;
				const auto& saved = p.checkpoints[m % checkpoint_ring];
				if (depth >= checkpoint_ring || depth > replay_slots || !saved || saved->first != m)
				{
					result.fault = "peer_correction_beyond_ring";
					return;
				}
				if (depth > s.max_prediction)
					result.fault = "a correction deeper than the prediction bound";
				if (m < p.published_through)
					result.fault = "a correction runs a published row again";
				if (m >= s.lockstep_from)
					result.fault = "a correction runs a lockstep row again";
				publish(p, std::min(m, u));
				p.now = saved->second;
				for (u32 t = m; t < index; ++t)
					execute(p, t, index);
				publish(p, std::min(m + 1, first_unconfirmed_row(p, index)));
				++result.corrections;
				result.read_ahead_corrections += read_ahead;
				result.deepest = std::max(result.deepest, depth);
			}
			else
			{
				p.scan_from = u;
				confirm(p, u);
				publish(p, u);
			}
			execute(p, index, index);
			if (p.now.refusal)
				result.fault = p.now.refusal;
			++p.next;
		};
		for (u32 step = 0; !(pcs[0].done && pcs[1].done) && !result.fault; ++step)
		{
			if (step == 40 * (ticks + 50))
				result.fault = "the pair stalled";
			for (u32 k = 0; k < 2 && !result.fault; ++k)
			{
				const u32 role = (step + k) % 2;
				boundary(pcs[role], pcs[1 - role], step);
			}
		}
		for (u32 t = 0; t < ticks && !result.fault; ++t)
		{
			for (const peer_side& p : pcs)
				if (p.confirmed[t] != p.digests[t] || p.published[t] != p.digests[t])
					result.fault = "a confirmed or published row ran again";
			const lookahead_use &left = pcs[0].rows[t][c.port].use, &right = pcs[1].rows[t][c.port].use;
			if (pcs[0].digests[t] != pcs[1].digests[t])
				result.fault = "the PCs differ at a confirmed tick";
			else if (!same_use(left, right))
				result.fault = "the PCs read different words ahead";
		}
		for (u32 role = 0; role < 2; ++role)
			result.commits[role] = pcs[role].now.commits;
		for (u32 t = 0; t < ticks; ++t)
		{
			result.words.push_back({word(0, t), word(1, t)});
			result.digests.push_back(pcs[0].digests[t]);
			result.rows.push_back(pcs[0].rows[t]);
		}
		return result;
	}

	// The same hold offline: each player's words on the ticks they pressed them.
	std::vector<std::pair<u32, int>> offline_commits(const scenario& c, const std::vector<u16>& held, const std::vector<u16>& thrower)
	{
		scenario offline = c;
		offline.hooks = hook::none;
		fighter f = start(offline);
		span_cache spans;
		std::vector<std::array<port_record, 2>> rows(c.ticks);
		const auto at = [](const std::vector<u16>& words, u32 t)
		{
			return t < words.size() ? words[t] : u16{0};
		};
		const auto nothing = [](u16, u32) -> std::optional<u16>
		{
			return std::nullopt;
		};
		for (u32 t = 0; t < c.ticks && !f.ended; ++t)
			tick(offline, f, spans, rows[t], t, at(held, t), {}, nothing, at(thrower, t));
		return f.commits;
	}

	// A held player's or a thrower's words in executed ticks, with directions that change every `every` ticks over
	// [from, to) and the presses on top.
	std::vector<u16> words_with(u32 length, const std::vector<press>& presses, u32 from = 0, u32 to = 0, u32 every = 4)
	{
		std::vector<u16> words = timeline(presses, length);
		for (u32 t = from; t < to && t < length; ++t)
			if (!(words[t] & (forward | back)))
				words[t] |= (t - from) / every % 2 ? back : forward;
		return words;
	}

	// One hold played by two PCs at every D from 1 to 6, on every link, with the held fighter on either side: both PCs end
	// on the same state at every tick and every escape lands on the tick it lands on offline. The thrower's moves land on
	// the same ticks at every delay (its pad is read D ticks before the tick that runs it); the held player's pad is read on
	// the ticks it is offline. Returns the corrections the read-ahead words caused.
	struct pair_case
	{
		const world* w = nullptr;
		std::vector<thrower_cancel> thrower;
		std::vector<u16> thrower_words; // by executed tick
		std::vector<u16> held_words;    // by boundary, the same as executed offline
		u32 ticks = 0, lockstep_from = 0;
	};

	// A LAN, a link that loses and reorders, a late and lossy one, and one whose datagrams from port 0's PC stop for 40 frames.
	const std::array<link_profile, 4> links{{{1, 0, 0}, {3, 3, 15}, {6, 6, 30}, {3, 2, 10, 70, 110}}};

	u32 play_pairs(const pair_case& p, std::vector<std::pair<u32, int>>* offline_out = nullptr)
	{
		u32 read_ahead = 0;
		for (u16 side = 0; side < 2; ++side)
		{
			scenario c = base_of(*p.w, p.ticks);
			c.grab = ~0u;
			c.thrower = p.thrower;
			c.hooks = hook::rule;
			c.port = side;
			c.actor = side ? side_1 : side_0;
			const auto expected = offline_commits(c, p.held_words, p.thrower_words);
			if (offline_out)
				*offline_out = expected;
			for (u32 delay = 1; delay <= 6; ++delay)
				for (u32 link = 0; link < links.size(); ++link)
				{
					pair_setup s;
					s.c = c;
					s.delay = delay;
					s.samples[side] = p.held_words;
					s.samples[1 - side].assign(p.thrower_words.size(), 0);
					for (std::size_t b = 0; b + delay < p.thrower_words.size(); ++b)
						s.samples[1 - side][b] = p.thrower_words[b + delay];
					s.link = links[link];
					s.max_prediction = link >= 2 ? 8 : 5;
					s.lockstep_from = p.lockstep_from;
					s.seed = (u64{side} << 40) ^ (u64{delay} << 32) ^ (u64{link} << 24) ^ p.ticks ^ p.held_words.size();
					const pair_result r = play_pair(s);
					const std::string where = " (side " + std::to_string(side) + ", D " + std::to_string(delay) + ", link " + std::to_string(link) + ")";
					check(!r.fault, (r.fault ? r.fault : "") + where);
					check(r.commits[0] == expected && r.commits[1] == expected, "both PCs land every move on its offline tick" + where);
					read_ahead += r.read_ahead_corrections;
				}
		}
		return read_ahead;
	}

	// A replay or a watched game of a set, as the native playback runs it (begin_throw_break_tick, then the probes): each tick
	// takes the setting its recording names there (take_recorded) and reads the recorded words ahead. `words` are the words
	// each tick ran, both ports, as a game file's input rows and a watcher's frames hold them; `at_hand(t)` is how many of them
	// the player holds when tick t runs. Playback starts at tick `first` from `from` with nothing carried, as a round jump
	// does, or at 0 from the game's start; `keep_at` keeps the state before that tick.
	struct playback
	{
		const char* refusal = nullptr;
		std::vector<u64> digests;
		std::vector<std::array<port_record, 2>> rows;
		std::vector<std::pair<u32, int>> commits;
		std::optional<fighter> kept;
	};

	template <typename AtHand>
	playback play_back(const scenario& c, const std::vector<std::array<u16, 2>>& words, const lookahead_rounds& rounds, AtHand&& at_hand,
		const fighter* from = nullptr, u32 first = 0, u32 keep_at = ~0u)
	{
		playback out;
		session_state state;
		state.configure_playback(rounds.any());
		fighter f = from ? *from : start(c);
		span_cache spans;
		out.rows.resize(words.size());
		out.digests.resize(words.size());
		for (u32 t = first; t < words.size(); ++t)
		{
			if (t == keep_at)
				out.kept = f;
			auto& row = out.rows[t];
			const std::array<port_record, 2>* before = t > first ? &out.rows[t - 1] : nullptr;
			for (u32 port = 0; port < 2; ++port)
				carry(row[port], before ? &(*before)[port] : nullptr, t);
			const lookahead_setting value = rounds.at(t);
			if ((out.refusal = take_recorded(row, before, t, value, true)))
				break;
			const auto held_at = [&](u16 port, u32 source) -> std::optional<u16>
			{
				if (source >= at_hand(t) || source >= words.size())
					return std::nullopt;
				return held_mask(words[source][port]);
			};
			scenario run = c;
			run.hooks = state.on ? hook::rule : hook::none;
			run.tech_rolls = value.tech_rolls;
			tick(run, f, spans, row, t, words[t][c.port], state.lookahead_of(row), held_at, words[t][1 - c.port]);
			out.digests[t] = f.digest();
			if ((out.refusal = f.refusal))
				break;
		}
		out.commits = f.commits;
		return out;
	}

	// What a watcher holds when it runs each tick. The frames sent before it joined come at once, in packets of `packet`;
	// after that one packet arrives every `every` ticks of play. It runs a tick only once it holds the frames through
	// t + `ahead`, or the game's last frame (local_channel::reserve with lookahead_frames).
	std::vector<u32> watcher_frames(u32 total, u32 joined, u32 packet, u32 every, u32 ahead)
	{
		std::vector<u32> held(total);
		u32 received = std::min(total, (joined + packet - 1) / packet * packet), clock = 0;
		for (u32 t = 0; t < total; ++t)
		{
			while (!(received > t + ahead || received == total))
				if (++clock % every == 0)
					received = std::min(total, received + packet);
			held[t] = received;
		}
		return held;
	}

	// The set each playback test plays: the tackle with the thrower's arm lock and a mash begun in the punch, held on either
	// side, and the other worlds the two-PC tests play.
	struct recorded_set
	{
		pair_setup setup;
		pair_result played;
	};

	recorded_set record_set(const pair_case& p, u16 side, u32 delay, u32 link = 1)
	{
		recorded_set out;
		scenario c = base_of(*p.w, p.ticks);
		c.grab = ~0u;
		c.thrower = p.thrower;
		c.hooks = hook::rule;
		c.port = side;
		c.actor = side ? side_1 : side_0;
		pair_setup& s = out.setup;
		s.c = c;
		s.delay = delay;
		s.samples[side] = p.held_words;
		s.samples[1 - side].assign(p.thrower_words.size(), 0);
		for (std::size_t b = 0; b + delay < p.thrower_words.size(); ++b)
			s.samples[1 - side][b] = p.thrower_words[b + delay];
		s.link = links[link];
		s.max_prediction = link >= 2 ? 8 : 5;
		s.lockstep_from = p.lockstep_from;
		s.seed = (u64{side} << 40) ^ (u64{delay} << 32) ^ (u64{link} << 24) ^ p.ticks ^ 77;
		out.played = play_pair(s);
		check(!out.played.fault, out.played.fault ? out.played.fault : "");
		return out;
	}

	pair_case tackle_case(const world& w)
	{
		pair_case p;
		p.w = &w;
		p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}, {6, held_mask(two), 1, 25, 2, 1}};
		p.ticks = 200;
		p.lockstep_from = 180;
		p.thrower_words = words_with(240, {{40, one | three, 1}, {70, two, 1}}, 5, 200, 7);
		p.held_words = words_with(240, {{66, two, 1}, {69, two, 1}, {85, two, 1}, {88, two, 1}, {91, both, 1}}, 30, 120, 4);
		return p;
	}

	pair_case break_case(const world& w)
	{
		pair_case p;
		p.w = &w;
		p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}};
		p.ticks = 280;
		p.lockstep_from = 260;
		p.thrower_words = words_with(320, {{40, one | three, 1}}, 5, 300, 6);
		p.held_words = words_with(320, {{55, one, 1}}, 30, 70, 3);
		return p;
	}

	pair_case mount_case(const world& w)
	{
		pair_case p;
		p.w = &w;
		p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}, {1, held_mask(one), 20, 90, 4, 1}};
		p.ticks = 300;
		p.lockstep_from = 285;
		p.thrower_words = words_with(340, {{40, one | three, 1}, {71, one, 1}}, 5, 320, 9);
		p.held_words = words_with(340, {{150, two, 1}, {153, two, 1}, {156, two, 1}, {159, two, 1}, {162, both, 1}}, 30, 300, 4);
		return p;
	}

	lookahead_rounds rounds_of(std::initializer_list<std::pair<u32, u32>> changes)
	{
		lookahead_rounds rounds;
		for (const auto& [from, ahead] : changes)
			check(rounds.add(from, {{ahead, ahead}}), "a round's setting in input order");
		return rounds;
	}

	const auto everything = [](u32)
	{
		return ~0u;
	};

	// A hold that ends in the fighter's own moveset, so the steps after it keep its last word: the CatchUpAfterAHold moves,
	// with the grab from the thrower's 1+3 on tick 40 and the held player's 2 at frame 4 of the hold.
	world jump_world()
	{
		moveset_def set;
		set.moves = {{0}, {1, {on(0, 0x2, 2, 1, 10), automatic(4, 30)}}, {1, {automatic(3, 2)}}, {0, {on(0, 0x2, 5, 1, 60), automatic(6, 60)}},
			{1, {}, true}, {0, {}, true}, {0, {}, true}};
		return world(set);
	}

	pair_case jump_case(const world& w)
	{
		pair_case p;
		p.w = &w;
		p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}};
		p.ticks = 160;
		p.lockstep_from = 150;
		p.thrower_words = words_with(200, {{40, one | three, 1}}, 5, 160, 6);
		p.held_words = words_with(200, {{45, two, 1}}, 30, 140, 3);
		return p;
	}

	// The mount chain: the thrower's tackle (1+3 on tick 40) mounts the held fighter at 41 and starts the arm lock at the
	// mount's frame 45 (tick 85); the held player mashes 2 from the mount's frame 32 into the lock, then 1+2, and reverses.
	pair_case mount_chain_case(const world& w)
	{
		pair_case p;
		p.w = &w;
		p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1, 9}, {1, 0, 44, 44, 2, 1, 11}};
		p.ticks = 200;
		p.lockstep_from = 180;
		p.thrower_words = words_with(240, {{40, one | three, 1}}, 5, 200, 7);
		p.held_words = words_with(240, {{72, two, 1}, {75, two, 1}, {78, two, 1}, {81, two, 1}, {84, two, 1}, {87, two, 1}, {90, both, 1}}, 30, 120, 4);
		return p;
	}

	// Jack-5's lift with the thrower's d/f+2 on tick 60 and the held player's back at frames 29..30 of pj_lfp_yxk.
	pair_case jack_case(const world& w)
	{
		pair_case p;
		p.w = &w;
		p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}, {1, held_mask(two), 15, 65, 3, 1}};
		p.ticks = 160;
		p.lockstep_from = 140;
		p.thrower_words = words_with(200, {{40, one | three, 1}, {60, two, 1}}, 5, 160, 5);
		p.held_words = words_with(200, {{90, back, 2}});
		return p;
	}

	// The last tick whose step read ahead, for the held fighter's port.
	u32 last_read_ahead(const std::vector<std::array<port_record, 2>>& rows, u16 port)
	{
		u32 last = 0;
		for (u32 t = 0; t < rows.size(); ++t)
			if (rows[t][port].use.reads())
				last = t;
		check(last, "the hold read ahead");
		return last;
	}
} // namespace

PBD_TEST(PbdThrowBreak, ProbesAndActors)
{
	check(std::is_sorted(probes.begin(), probes.end()) && probes[0] == scan_entry && probes[1] == scan_exit && probes[2] == latch_entry &&
			  probes[3] == word_load,
		"the scan's entry and exit, the latch entry and the word load");
	check(probed(word_load) && probed(latch_entry) && probed(scan_entry) && probed(scan_exit) && !probed(0xed1e0) && !probed(0xecfb8),
		"nothing else");
	check(slot_of(0) == 0x10099fb4 && slot_of(1) == 0x10099fb8, "the ports' slots");
	check(side_of(side_0) == 0u && side_of(side_1) == 1u, "the two fighters");
	check(!side_of(side_0 + 4) && !side_of(0) && !side_of(side_1 + pbd::replay_memory::actor_stride), "nothing else is a fighter");
	check(word_buttons(0x1) == one && word_buttons(0x2) == two && word_buttons(0x4) == three && word_buttons(0x8) == four &&
			  word_buttons(0xf) == escape_buttons,
		"buttons 1 to 4 in the word");
}

PBD_TEST(PbdThrowBreak, ReadWidths)
{
	memory guest;
	// Side 1 holds the word 0x00000101 at +0x1c0 in every dump; the game reads its first byte.
	guest.put32(side_1 + actor::controller, 0x00000101);
	guest.put32(side_1 + actor::port, 0x0001ffff);
	guest.put32(side_1 + actor::frame, 0x000c7777);
	guest.put32(side_1 + actor::move, 0x2170f0b4);
	guest.put32(side_1 + actor::moveset, 0x216aa4c0);
	guest.put32(side_1 + actor::move_moveset, 0x2162df00);
	guest.put32(side_1 + actor::life, 0x008f8ccc);
	guest.put8(side_1 + actor::facing, 1);
	actor_view view = read_actor(guest, side_1);
	check(view.human && view.port == 1 && view.frame == 12 && view.move == 0x2170f0b4, "u8 controller, u16 port and frame, u32 move");
	check(view.moveset == 0x216aa4c0 && view.move_moveset == 0x2162df00 && view.gate_open && view.facing == 1, "both movesets; life; facing");
	guest.put8(side_1 + actor::controller, 1);
	check(!read_actor(guest, side_1).human, "a computer fighter never arms");
	guest.put32(side_1 + actor::life, 0);
	check(!read_actor(guest, side_1).gate_open, "life 0 closes the gate");
}

PBD_TEST(PbdThrowBreak, DirectionFields)
{
	// The step's direction fields: +0x6bc holds 1 << the held direction's numpad digit; +0x6be the digit it changed to, 1
	// when it did not, and 0x20 when it came back to neutral.
	check(numpad(0, 0) == 5 && numpad(forward, 0) == 6 && numpad(back, 0) == 4 && numpad(up | forward, 0) == 9 && numpad(down | back, 0) == 1,
		"facing right");
	check(numpad(forward, 1) == 4 && numpad(back, 1) == 6 && numpad(down | forward, 1) == 1, "facing left the stick turns round");
	check(direction_fields(forward, forward, 0, 0x20, 0) == (0x40u << 16 | 0x40), "forward pressed: held and changed");
	check(direction_fields(forward, 0, forward, 0x40, 0) == (0x40u << 16 | 0x1), "forward still held: no change");
	check(direction_fields(0, 0, forward, 0x40, 0) == (0x20u << 16 | 0x20), "back to neutral");
	check(direction_fields(0, 0, 0, 0x20, 0) == (0x20u << 16 | 0x1), "neutral still");
	check(direction_fields(back, 0, no_fresh_mark, 0x10, 0) == (0x10u << 16 | 0x10), "a frame without fresh presses reads as a change");
	check(direction_fields(back | two, back, two, 0x20, 0) == (0x10u << 16 | 0x10), "buttons do not count");
	check(direction_fields(back, back, 0, 0x20, 1) == (0x40u << 16 | 0x40), "facing left, the left stick is forward");
}

PBD_TEST(PbdThrowBreak, MoveSpans)
{
	moveset_def set;
	set.sequences = {{59, {2, 2, 2, 3}}, {30, {1, 1, 2}}};
	set.groups = {{automatic(-1, 30), tech_roll(-1, 60, 69, 0x1003), on(0, 0x2, -1, 60, 69)}, {group_jump(0)}};
	cancel_def skipped = on(0, 0x1, -1, 1, 30);
	skipped.option = moves::option_skipped;
	cancel_def compound = break_cancel(break_class::both, -1, 1, 15, 15);
	compound.requirements = {{145, 0}, {108, 0}};
	cancel_def unread_landing = automatic(-1, 40);
	unread_landing.extra = 0x90;
	cancel_def escape = on(0, 0x3, -1, 2, 3);
	escape.extra = 0x1ad6; // a grab's class, with bit 0x10
	set.moves = {
		{1, {on(0, 0x2, -1, 1, 15), on(0, 0x2003, -1, 1, 15)}},
		{1, {automatic(0, 20), skipped}},
		{1, {break_cancel(break_class::one, -1, 1, 15, 15)}},
		{1, {break_cancel(break_class::one, -1, 1, 15, 1), break_cancel(break_class::one, -1, 91, 99, 100), automatic(0, 140)}},
		{1, {compound}},
		{1, {group_jump(0), on(0, 0x2, -1, 1, 5)}},
		{1, {on(0x8007, 0, -1, 18, 65), on(0x8008, 0, -1, 1, 20)}},
		{1, {}},
		{1, {group_jump(1)}},
		{1, {on(moves::group_return, 0, -1, 1, 1)}},
		{1, {on(0x0040, 0x2005, -1, 1, 10), on(0x0410, 0, -1, 59, 61), on(0x03de, 0, -1, 10, 28)}},
		{1, {unread_landing, escape, tech_roll(-1, 97, 106), on(0, 0x4000, -1, 5, 9)}},
	};
	const layout lay = lay_out(set);
	const auto span = [&](u32 move)
	{
		return span_of_move(lay.banks, lay.moves[1][move], layout::base[1]);
	};
	const auto window_at = [](const move_span& s, u32 at)
	{
		return s.windows.at(at);
	};
	move_span s = span(0);
	check(s.readable && s.count == 1 && window_at(s, 0).first == 1 && window_at(s, 0).last == 15 && window_at(s, 0).buttons == 0x3 &&
			  !window_at(s, 0).directions,
		"button cancels with the same frames share a window: 2, and 1+2 held with one fresh");
	s = span(1);
	check(s.readable && !s.count, "an automatic cancel and a skipped one read nothing");
	s = span(2);
	check(s.count == 1 && window_at(s, 0).breaks && window_at(s, 0).buttons == 0x3, "a break cancel reads the latch: 1 and 2, though the loader marks it unread");
	s = span(3);
	check(s.count == 1 && window_at(s, 0).breaks && window_at(s, 0).first == 1 && window_at(s, 0).last == 99,
		"a chain stage's break windows read as one: the latch runs between them");
	s = span(4);
	check(s.count == 1 && window_at(s, 0).breaks && window_at(s, 0).last == 15, "a break id beside another requirement");
	s = span(5);
	check(s.readable && s.count == 3 && window_at(s, 0).landing && window_at(s, 0).first == 60 && !window_at(s, 1).landing &&
			  window_at(s, 1).first == 60 && window_at(s, 2).first == 1,
		"group cancels count, and the walk returns after the group");
	s = span(6);
	check(s.count == 2 && window_at(s, 0).buttons == 0x3 && window_at(s, 1).buttons == 0x3 && !window_at(s, 0).directions,
		"a sequence reads the buttons of its presses from the command history");
	check(window_at(s, 0).sequence && window_at(s, 0).first == 1 && window_at(s, 0).last == 65 && window_at(s, 1).sequence &&
			  window_at(s, 1).first == 1 && !span(0).windows[0].sequence,
		"from the first frame its history reaches in the move: 18 - 58 and 1 - 29 are before frame 1");
	s = span(7);
	check(s.readable && !s.count, "a move without cancels");
	check(!span(8).readable, "a group that jumps again is not a move");
	check(!span(9).readable, "a group return outside a group is not a move");
	s = span(10);
	check(s.count == 3 && window_at(s, 0).directions && window_at(s, 0).buttons == 0x5 && window_at(s, 1).directions && !window_at(s, 1).buttons &&
			  window_at(s, 2).directions && window_at(s, 2).first == 10,
		"chickens, a fresh back and a held direction read the directions");
	s = span(11);
	check(s.count == 3 && !window_at(s, 0).landing && window_at(s, 0).buttons == 0x3 && window_at(s, 1).landing && window_at(s, 2).buttons == 0xf,
		"an unread landing reads nothing; a grab's class is no landing; a tech roll is; nothing fresh reads every button");
	check(s.follow_up == 3 && !span(0).follow_up, "a pair's cancel: the move can start a pair up to its last frame");
	{
		moveset_def late;
		late.sequences = {{59, {2, 2, 3}}};
		cancel_def automatic_pair = automatic(-1, 30);
		automatic_pair.extra = 0x1800;
		cancel_def skipped_pair = follow_up(1, 90, 91);
		skipped_pair.option = moves::option_skipped;
		late.moves = {{1, {follow_up(1, 20, 45), automatic_pair, skipped_pair, on(0x8007, 0, -1, 70, 80)}}};
		const layout laid = lay_out(late);
		const move_span later = span_of_move(laid.banks, laid.moves[1][0], layout::base[1]);
		check(later.follow_up == 44, "or up to the frame before its target commits; an automatic pair counts, a skipped one not");
		check(later.count == 2 && later.windows[1].sequence && later.windows[1].first == 12, "a sequence in 70..80 with 59 frames reads from 12");
	}
	{
		moveset_def open;
		open.moves = {{1, {on(0, 0x2, -1, 1, 15)}}};
		const layout unterminated = lay_out(open, false);
		check(!span_of_move(unterminated.banks, unterminated.moves[1][0], layout::base[1]).readable, "a requirement list without its end");
	}
	{
		memory banks;
		banks.put32(0x21000000 + moves::cancels, 0x21100000);
		for (u32 at = 0; at <= moves::max_cancels; ++at)
			banks.put32(0x21100000 + at * moves::cancel_stride + moves::cancel_requirements, 0x21200000);
		banks.put16(0x21200000, moves::requirement_end);
		check(!span_of_move(banks, 0x21000000, 0x21000000).readable, "a cancel list without its end");
	}
	{
		moveset_def many;
		move_def move{1};
		for (u16 at = 0; at < 30; ++at)
			move.cancels.push_back(on(0, 0x1, -1, at, at));
		many.moves = {move};
		const layout crowded = lay_out(many);
		check(!span_of_move(crowded.banks, crowded.moves[1][0], layout::base[1]).readable, "more windows than a span keeps");
	}
	check(moves::code_buttons(0) == 0 && moves::code_buttons(0x2003) == 0x3 && moves::code_buttons(0x0050) == 0x5 && moves::code_buttons(0x4001) == 0xf,
		"a code's buttons: fresh, held and released nibbles, or all four");
}

PBD_TEST(PbdThrowBreak, SpanCache)
{
	moveset_def set;
	set.moves = {{1, {tech_roll(-1, 50, 59)}}, {1, {break_cancel(break_class::two, -1, 10, 23, 24)}}};
	layout lay = lay_out(set);
	span_cache spans;
	const u32 first = lay.moves[1][0], second = lay.moves[1][1];
	check(spans.get(lay.banks, first, layout::base[1]).windows[0].first == 50 && spans.get(lay.banks, second, layout::base[1]).windows[0].first == 10,
		"each move's own span");
	// The cache answers from what it read while the game lasts, and reads again after a match start or end.
	lay.banks.put16(layout::base[1] + 0x10000 + moves::cancel_first, 45);
	check(spans.get(lay.banks, first, layout::base[1]).windows[0].first == 50, "kept within the game");
	spans.clear();
	check(spans.get(lay.banks, first, layout::base[1]).windows[0].first == 45, "read again after a match change");
	check(!spans.get(lay.banks, 0, layout::base[1]).readable && !spans.get(lay.banks, first, 0).readable, "no move or no moveset, no span");
}

PBD_TEST(PbdThrowBreak, EscapesAtFrames)
{
	move_span span;
	span.readable = true;
	span.count = 4;
	span.windows[0] = {1, 4, 0x2, false, false, false};
	span.windows[1] = {18, 19, 0x3, false, false, false};
	span.windows[2] = {10, 23, 0x3, false, true, false};
	span.windows[3] = {43, 52, 0xf, false, false, true};
	actor_view held{true, 0, 3, 0x21401000, 0x21000000, 0x21400000, true, 0};
	check(escapes_at(held, span, false, false, 0).buttons == two && !escapes_at(held, span, false, false, 0).directions, "frame 3: the tackle's 2");
	held.frame = 5;
	check(!escapes_at(held, span, false, false, 0).any(), "between windows nothing");
	held.frame = 18;
	check(escapes_at(held, span, false, false, 0).buttons == both, "two windows at once");
	held.frame = 45;
	check(!escapes_at(held, span, false, false, 0).any() && escapes_at(held, span, true, false, 0).buttons == escape_buttons,
		"a tech roll keeps the delay, unless the developer switch counts it");
	actor_view own = held;
	own.move_moveset = own.moveset;
	own.frame = 3;
	check(!escapes_at(own, span, false, false, 0).any(), "the fighter's own moves never move an input");
	own.frame = 12;
	check(escapes_at(own, span, false, false, 0).buttons == both, "but a break window does, for a mirror match that may share one moveset");
	actor_view computer = held;
	computer.human = false;
	computer.frame = 3;
	check(!escapes_at(computer, span, false, false, 0).any(), "computer fighters never");
	move_span chicken;
	chicken.readable = true;
	chicken.count = 1;
	chicken.windows[0] = {1, 10, 0x5, true, false, false};
	held.frame = 4;
	check(escapes_at(held, chicken, false, false, 0).buttons == (one | three) && escapes_at(held, chicken, false, false, 0).directions, "a chicken reads directions");
	held.facing = 2;
	check(!escapes_at(held, chicken, false, false, 0).directions, "a facing the step's table does not cover moves no direction");
	check(!escapes_at(held, move_span{}, false, false, 0).any(), "an unreadable move moves nothing");

	// A press that arrives late misses where a window other than a sequence reads its button; a sequence counts it from the
	// command history.
	held.facing = 0;
	held.frame = 18;
	check(escapes_at(held, span, false, false, 0).timed == both, "the reversal's 1+2 and the break read their buttons now");
	move_span lock;
	lock.readable = true;
	lock.count = 2;
	lock.windows[0] = {1, 60, 0x3, false, false, false, true};
	lock.windows[1] = {50, 52, 0x4, false, false, false, false};
	check(escapes_at(held, lock, false, false, 0).buttons == both && !escapes_at(held, lock, false, false, 0).timed, "a sequence alone");
	held.frame = 51;
	check(escapes_at(held, lock, false, false, 0).timed == three, "beside a button window");
	// While the hold can go on, all four buttons, but for the L frames before a landing option.
	held.frame = 30;
	check(escapes_at(held, span, false, true, 6).buttons == escape_buttons && !escapes_at(held, span, false, true, 6).timed &&
			  escapes_at(held, span, false, true, 6).continuing && !escapes_at(held, span, false, false, 6).any(),
		"between windows, while the holder can start another pair");
	held.frame = 18;
	check(escapes_at(held, span, false, true, 6).continuing && !escapes_at(held, span, false, false, 6).continuing, "in a window too");
	held.frame = 30;
	held.frame = 36;
	check(escapes_at(held, span, false, true, 6).buttons == escape_buttons && !escapes_at(held, span, false, true, 7).any() &&
			  !escapes_at(held, span, false, true, 7).continuing,
		"a press must not reach the landing option at 43 by its copy");
	held.frame = 45;
	check(escapes_at(held, span, true, true, 6).buttons == escape_buttons && !escapes_at(held, span, false, true, 6).any(),
		"nor after it, unless the developer switch counts tech rolls");
	own.frame = 30;
	check(!escapes_at(own, span, false, true, 6).any(), "never in the fighter's own moves");
}

PBD_TEST(PbdThrowBreak, HoldContinues)
{
	// Side 0 held in the mount, which the thrower's pair gave it; side 1 the thrower, in the mount that started the pair,
	// which can start another pair in frames 1..44.
	const world w = mount_chain_world();
	guest g;
	g.banks = &w.lay.banks;
	memory& m = g.state;
	m.put32(side_0 + actor::moveset, layout::base[0]);
	m.put32(side_0 + actor::move_moveset, layout::base[1]);
	m.put32(side_0 + actor::move, w.lay.moves[1][1]);
	m.put16(side_0 + actor::move_class, reaction_class);
	m.put32(side_1 + actor::moveset, layout::base[1]);
	m.put32(side_1 + actor::move_moveset, layout::base[1]);
	m.put32(side_1 + actor::move, w.lay.moves[1][9]);
	m.put16(side_1 + actor::move_class, pair_class);
	m.put16(side_1 + actor::frame, 30);
	span_cache spans;
	const auto span_of = [&](u32 move, u32 moveset)
	{
		return spans.get(g, move, moveset);
	};
	const auto continues = [&](u32 address = side_0)
	{
		return hold_continues(g, address, read_actor(g, address), span_of);
	};
	check(span_of(w.lay.moves[1][9], layout::base[1]).follow_up == 44 && span_of(w.lay.moves[1][10], layout::base[1]).follow_up == 17,
		"a pair's last frame, or the frame before its target commits");
	check(continues(), "the thrower's mount can still start a pair");
	m.put16(side_1 + actor::frame, 44);
	check(continues(), "up to its last such frame");
	m.put16(side_1 + actor::frame, 45);
	check(!continues(), "not after it");
	m.put16(side_1 + actor::frame, 30);
	m.put32(side_1 + actor::move, w.lay.moves[1][12]);
	check(!continues(), "a thrower's move without a pair");
	m.put32(side_1 + actor::move, w.lay.moves[1][9]);
	m.put16(side_0 + actor::move_class, pair_class);
	check(!continues(), "a pair the held fighter started itself (a reversal)");
	m.put16(side_0 + actor::move_class, 0);
	check(!continues(), "a move the held fighter committed itself (an escape, a restart, a fall)");
	m.put16(side_0 + actor::move_class, reaction_class);
	m.put16(side_0 + queued_class, 0);
	check(continues(), "+0x310 is the queued move's class, 0 once the reaction runs: never read");
	// The reaction outlasts the move that gave it: the thrower has gone on to a move of its own that can start a pair (a walk
	// or a throw attempt), committed from a cancel that starts none.
	m.put16(side_1 + actor::move_class, 0x0080);
	check(!continues(), "a holder that left the move whose pair gave the reaction");
	m.put16(side_1 + actor::move_class, 0x0213);
	check(!continues(), "nor any other class");
	m.put16(side_1 + actor::move_class, pair_class);
	m.put32(side_1 + actor::move_moveset, layout::base[0]);
	check(!continues(), "a thrower in another moveset's move");
	m.put32(side_1 + actor::move_moveset, layout::base[1]);
	m.put32(side_0 + actor::move_moveset, layout::base[0]);
	check(!continues(), "a fighter in its own moves");
	m.put32(side_0 + actor::move_moveset, layout::base[1]);
	check(continues() && !continues(side_0 + 4), "no other address is a fighter");
	// Side 1 held by side 0 reads side 0's move.
	check(!continues(side_1), "the thrower is not held");
}

PBD_TEST(PbdThrowBreak, CalibrationGuardReactionsAreNotEscapeWindows)
{
	// The failed calibration runs committed Ganryu's sGrd_mM00 (572) and Xiaoyu's sGrd_hM00 (748), from the
	// attacker's table. Their cancels read requirements and time, never input; table ownership alone proves no grab.
	cancel_def guard = automatic(-1, 1);
	guard.requirements = {{135, 4}, {32938, 32768}, {32939, 0}, {32931, 0}, {189, 0}, {32843, 1}};
	guard.last = 2;
	guard.extra = 0x2400;
	moveset_def set;
	set.moves = {{1, {guard}}, {1, {guard}}, {1, {automatic(0, 44)}}};
	set.moves[2].cancels[0].extra = pair_class;
	const layout lay = lay_out(set);
	guest g;
	g.banks = &lay.banks;
	for (const u32 address : {side_0, side_1})
	{
		g.state.put32(address + actor::moveset, address == side_0 ? layout::base[0] : layout::base[1]);
		g.state.put32(address + actor::move_moveset, layout::base[1]);
		g.state.put32(address + actor::life, 0x00910000);
	}
	g.state.put32(side_1 + actor::move, lay.moves[1][2]);
	g.state.put16(side_1 + actor::move_class, pair_class);
	g.state.put16(side_1 + actor::frame, 1);
	g.state.put16(side_0 + actor::move_class, 0x2400);
	span_cache spans;
	const auto span_of = [&](u32 move, u32 moveset)
	{
		return spans.get(g, move, moveset);
	};
	const auto no_source = [](u16, u32) -> std::optional<u16>
	{
		check(false, "a guard reaction must never request a future word");
		return std::nullopt;
	};
	for (const u32 move : {lay.moves[1][0], lay.moves[1][1]})
	{
		g.state.put32(side_0 + actor::move, move);
		check(span_of(move, layout::base[1]).readable && !span_of(move, layout::base[1]).count, "the guard has no input window");
		for (u32 delay = 1; delay <= max_lookahead; ++delay)
		{
			std::array<port_record, 2> row{};
			row[0].previous_known = true;
			row[0].previous_ahead = static_cast<u8>(delay);
			row[0].previous_step = 99;
			for (u16 frame = 1; frame <= 30; ++frame)
			{
				g.state.put16(side_0 + actor::frame, frame);
				check(!hold_continues(g, side_0, read_actor(g, side_0), span_of), "class 0x2400 is not the pair's 0x1f57 reaction");
				const step_plan plan = at_word_load(g, side_0, 0, 100, {delay, delay}, row, false, span_of, no_source);
				check(!plan.refusal && !plan.use.reads() && !plan.writes() && !plan.view, "ordinary guarding keeps its delayed input");
			}
		}
	}
}

PBD_TEST(PbdThrowBreak, CalibrationTargetMovesHaveEscapeWindows)
{
	// The intended targets in T55DATA1: Ganryu 494 Sm_tasuki_y, and Xiaoyu 1099 Fu_xiao01_y. A wrong Ganryu button
	// restarts the same foreign move with class 0x0401; its windows still read input without the pair's class marker.
	moveset_def set;
	set.moves = {{1, {on(0, 0x2001, 0, 1, 11), on(0, 0x2002, 0, 1, 11), on(0, 0x2008, 0, 1, 11), on(0, 0x2004, 2, 1, 11)}},
		{1, {break_cancel(break_class::both, 2, 1, 15, 1), on(0x000e, 0, 2, 1, 30), on(0x8001, 0, 2, 1, 30), on(0x8002, 0, 2, 1, 30)}},
		{1, {}, true}};
	const layout lay = lay_out(set);
	actor_view held{true, 0, 11, lay.moves[1][0], layout::base[0], layout::base[1], true, 0};
	const move_span ganryu = span_of_move(lay.banks, held.move, held.move_moveset);
	check(ganryu.readable && escapes_at(held, ganryu, false, false, 3).buttons == escape_buttons, "Tasukizori reads the right and lockout buttons");
	held.frame = 12;
	check(!escapes_at(held, ganryu, false, false, 3).any(), "the Ganryu window closes after frame 11");
	held.move = lay.moves[1][1];
	const move_span xiaoyu = span_of_move(lay.banks, held.move, held.move_moveset);
	check(xiaoyu.readable && escapes_at(held, xiaoyu, false, false, 3).buttons == both &&
			  escapes_at(held, xiaoyu, false, false, 3).directions,
		"Xiaoyu's break and direction windows are recognized");
	held.frame = 30;
	check(!escapes_at(held, xiaoyu, false, false, 3).buttons && escapes_at(held, xiaoyu, false, false, 3).directions,
		"the held-down window outlasts the break window");
	held.frame = 31;
	check(!escapes_at(held, xiaoyu, false, false, 3).any(), "ordinary directions after frame 30 keep their delay");
}

PBD_TEST(PbdThrowBreak, LookaheadPerPort)
{
	check(lookahead(true, true, 3u) == 3, "the applied delay");
	check(lookahead(false, true, 3u) == 0, "rule off");
	check(lookahead(true, false, 3u) == 0, "a word not at hand that many ticks ahead");
	check(lookahead(true, true, std::nullopt) == 0, "zero until the agreed delay is applied");
	check(lookahead(true, true, 0u) == 0 && lookahead(true, true, 6u) == 6, "delay 0 changes nothing; the full delay");

	std::array<port_record, 2> row{};
	session_state peer;
	peer.configure_peer(setting::on, setting::unset, true, std::nullopt);
	check(peer.on && !peer.applied && peer.lookahead_of(row) == std::array<u32, 2>{0, 0},
		"an automatic delay, or a fixed one below 4: 0 until input_delay_applied");
	peer.apply_delay(2);
	check(peer.lookahead_of(row) == std::array<u32, 2>{2, 2}, "then the applied delay, for both ports");
	peer.apply_delay(1);
	check(peer.lookahead_of(row) == std::array<u32, 2>{1, 1}, "a later application is followed, down as well");
	session_state fixed;
	fixed.configure_peer(setting::on, setting::unset, true, 4u);
	check(fixed.applied && fixed.lookahead_of(row) == std::array<u32, 2>{4, 4}, "a delay fixed at configure applies at once");
	session_state no_phase;
	no_phase.configure_peer(setting::on, setting::unset, false, 4u);
	check(no_phase.lookahead_of(row) == std::array<u32, 2>{0, 0}, "without the phase delay and the entry sample, nothing");
	session_state peer_default;
	peer_default.configure_peer(setting::unset, setting::unset, true, 3u);
	check(!peer_default.on && peer_default.lookahead_of(row) == std::array<u32, 2>{0, 0}, "a peer session runs it only when asked");
	check(peer.developer && !peer.tech_rolls && !peer_default.developer, "asked for, it is a developer run; tech rolls keep the delay");

	session_state practice;
	practice.configure_practice(setting::unset, setting::unset);
	check(!practice.on && !practice.developer && !practice.tech_rolls, "Practice runs it only when asked");
	practice.configure_practice(setting::on, setting::unset);
	session_state feel_test;
	feel_test.configure_practice(setting::on, setting::on);
	check(feel_test.developer && feel_test.tech_rolls, "the developer switch for tech rolls");
	row[0].practice_delay = 3;
	row[1].practice_delay = 3;
	check(practice.lookahead_of(row) == std::array<u32, 2>{3, 0}, "Practice: the player's port by its delay, never the dummy's");
	row[0].practice_delay = 0;
	check(practice.lookahead_of(row) == std::array<u32, 2>{0, 0}, "a Practice word that did not wait");
	practice.configure_practice(setting::off, setting::unset);
	row[0].practice_delay = 3;
	check(practice.lookahead_of(row) == std::array<u32, 2>{0, 0}, "switched off");

	check(read_setting(nullptr) == setting::unset && read_setting("") == setting::unset, "unset");
	check(read_setting("1") == setting::on && read_setting("0") == setting::off && read_setting("yes") == setting::invalid,
		"1, 0 and nothing else");
}

PBD_TEST(PbdThrowBreak, AbsentSwitchKeepsTheRuleOff)
{
	constexpr u32 basis = 0x811c9dc5;
	for (const char* text : {static_cast<const char*>(nullptr), ""})
	{
		for (const setting tech : {setting::unset, setting::on})
		{
			for (u32 delay = 0; delay <= 6; ++delay)
			{
				std::array<port_record, 2> row{};
				row[0].practice_delay = row[1].practice_delay = static_cast<u8>(delay);
				row[0].lookahead = row[1].lookahead = delay;
				session_state peer;
				peer.configure_peer(read_setting(text), tech, true, std::nullopt);
				peer.apply_delay(delay);
				check(!peer.on && !peer.developer && peer.lookahead_of(row) == std::array<u32, 2>{},
					"an absent switch leaves peer input at its ordinary delay, even with tech rolls requested");
				check(!peer.recorded().any() && peer.options_digest(basis) == basis && !peer.match_changed(true),
					"an absent switch adds no recording rule, digest change or match-start refusal");
				session_state practice;
				practice.configure_practice(read_setting(text), tech);
				check(!practice.on && !practice.developer && practice.lookahead_of(row) == std::array<u32, 2>{},
					"an absent switch leaves Practice input at its ordinary delay, even with tech rolls requested");
			}
		}
	}
}

PBD_TEST(PbdThrowBreak, MatchStartAndOptionsDigest)
{
	session_state peer;
	peer.configure_peer(setting::on, setting::unset, true, std::nullopt);
	check(says(peer.match_changed(true), "throw_break_lookahead_unset"), "a match before the delay is applied");
	check(!peer.match_changed(false), "leaving a match is no start");
	peer.apply_delay(3);
	peer.movesets_checked = true;
	check(!peer.match_changed(true) && !peer.movesets_checked, "a match after it; its movesets are compared again");
	constexpr u32 basis = 0x811c9dc5u;
	check(peer.options_digest(basis) != basis, "L is in the digest both peers compare");
	u32 version_1 = basis;
	for (const u32 byte : {0x54u, 0x42u, 3u, 3u})
		version_1 = (version_1 ^ byte) * 0x01000193u;
	check(peer.options_digest(basis) != version_1, "and the rule's version, so a build of the whole-word rule refuses at the start");
	session_state other;
	other.configure_peer(setting::on, setting::unset, true, 2u);
	check(other.options_digest(basis) != peer.options_digest(basis), "another L hashes differently");
	session_state rolls;
	rolls.configure_peer(setting::on, setting::on, true, 3u);
	check(rolls.options_digest(basis) != peer.options_digest(basis), "and so does the developer switch for tech rolls");
	session_state off;
	off.configure_peer(setting::off, setting::on, true, 3u);
	session_state zero;
	zero.configure_peer(setting::on, setting::on, true, 0u);
	check(off.options_digest(basis) == basis && zero.options_digest(basis) == basis && !off.match_changed(true),
		"rule off and L = 0 play as stock and hash as stock");
	session_state practice;
	practice.configure_practice(setting::on, setting::unset);
	check(!practice.match_changed(true) && practice.options_digest(basis) == basis, "Practice has no other peer");
}

PBD_TEST(PbdThrowBreak, SharedMoveset)
{
	memory guest;
	check(!shared_moveset(guest), "no movesets loaded yet");
	guest.put32(side_0 + actor::moveset, 0x2162df00);
	guest.put32(side_1 + actor::moveset, 0x216aa4c0);
	check(!shared_moveset(guest), "Kazuya and King run their own banks");
	guest.put32(side_1 + actor::moveset, 0x2162df00);
	check(shared_moveset(guest), "one bank for both fighters");
}

PBD_TEST(PbdThrowBreak, PreviousSteps)
{
	port_record first{}, second{}, third{}, fourth{};
	carry(first, nullptr, 10);
	check(!first.previous_known, "no row before: unknown");
	first.practice_word = two;
	first.stock = held_mask(two | back);
	first.ahead = 3;
	first.use.mask = both;
	first.in_hold = true;
	first.escape.base = 10;
	first.escape.early[1] = {0x03, 0, 0, 0};
	note_step(first, {true, 0, 1, 0, 0, 0, false, 0}, one);
	check(first.stepped && first.held == one && !first.gate_open, "the step, its gate and what it held");
	carry(second, &first, 11);
	check(second.previous_known && second.previous_step == 10 && !second.previous_gate_open && second.previous_held == one &&
			  second.previous_practice_word == two && second.previous_stock == held_mask(two | back) && second.previous_ahead == 3 &&
			  second.previous_read,
		"the tick before, with its gate, held mask, Practice word, stock word, L and whether it read ahead");
	check(second.in_hold && second.escape == first.escape && !settled(second), "the hold and the presses the rule tracks");
	// Tick 11 does not step (a paused simulation); tick 12 still compares with tick 10.
	carry(third, &second, 12);
	check(third.previous_known && third.previous_step == 10 && third.previous_stock == held_mask(two | back) && third.previous_read &&
			  third.escape == first.escape,
		"across ticks without a step");
	note_step(third, {true, 0, 2, 0, 0, 0, true, 0}, both);
	third.stock = held_mask(forward);
	third.escape = {};
	carry(fourth, &third, 13);
	check(fourth.previous_step == 12 && fourth.previous_gate_open && fourth.previous_held == both && fourth.previous_stock == held_mask(forward) &&
			  !fourth.previous_read && settled(fourth) && settled_after(third, 12),
		"the newest step; with nothing tracked, settled");
	check(may_read_ahead(fourth) && !may_read_ahead(port_record{}), "a fighter in the opponent's moves may read ahead at its next step");

	port_record record{};
	record.practice_word = one;
	check(practice_word_at(record, 12, 12) == one && !practice_word_at(record, 12, 10), "this tick's word; no earlier step known");
	record.previous_known = true;
	record.previous_step = 10;
	record.previous_practice_word = two;
	check(practice_word_at(record, 12, 10) == two && !practice_word_at(record, 12, 11), "the previous step's word, and no other tick's");
}

PBD_TEST(PbdThrowBreak, WordLoadAndLatchEntry)
{
	moveset_def set;
	set.moves = {{0, {on(0, 0x2, -1, 1, 10)}}, {1, {on(0, 0x3, -1, 1, 10)}}};
	const layout lay = lay_out(set);
	guest g;
	g.banks = &lay.banks;
	memory& m = g.state;
	// Side 1 on port 1, in the hold (move 1 of the opponent's moveset, 1+2 in 1..10) at frame 5. L is 3.
	m.put32(side_1 + actor::controller, 0x00000101);
	m.put16(side_1 + actor::port, 1);
	m.put32(side_1 + actor::moveset, layout::base[0]);
	m.put32(side_1 + actor::move_moveset, layout::base[1]);
	m.put32(side_1 + actor::move, lay.moves[1][1]);
	m.put16(side_1 + actor::frame, 5);
	m.put32(side_1 + actor::life, 0x00910000);
	std::array<port_record, 2> row{};
	port_record& record = row[1];
	std::map<u32, u16> words;
	const auto held_at = [&](u16 port, u32 source) -> std::optional<u16>
	{
		const auto found = words.find(source);
		return port == 1 && found != words.end() ? std::optional<u16>{found->second} : std::nullopt;
	};
	span_cache spans;
	const auto span_of = [&](u32 move, u32 moveset)
	{
		return spans.get(g, move, moveset);
	};
	const std::array<u32, 2> ahead{0, 3};
	const auto prepare = [&](u16 stock, u16 previous_stock, u32 previous_held, escape_state escape = {})
	{
		row = {};
		record.previous_known = record.previous_gate_open = true;
		record.previous_step = 99;
		record.previous_ahead = 3;
		record.previous_stock = previous_stock;
		record.escape = escape;
		m.put32(slot_of(1), stock);
		m.put32(side_1 + actor::previous_held, previous_held);
	};
	const auto plan_at = [&](u32 tick)
	{
		return at_word_load(g, side_1, 1, tick, ahead, row, false, span_of, held_at);
	};
	// The step loads what the plan left, and the latch entry checks it.
	const auto step_and_check = [&](const step_plan& p, std::optional<step_registers> registers = std::nullopt)
	{
		take_plan(record, p);
		if (p.slot)
			m.put32(slot_of(1), *p.slot);
		if (p.previous_held)
			m.put32(side_1 + actor::previous_held, *p.previous_held);
		const u16 held = static_cast<u16>(read32(g, slot_of(1)));
		const step_registers loaded = registers.value_or(step_registers{held, static_cast<u16>(held & ~read32(g, side_1 + actor::previous_held))});
		note_step(record, read_actor(g, side_1), loaded.held);
		return check_latch_entry(g, side_1, 100, ahead, row, false, loaded, span_of);
	};

	// 2 pressed now (in the word 3 ticks ahead): it fires, and is noted for its copy at word 103.
	words = {{102, held_mask(down)}, {103, held_mask(two | down)}};
	const u16 stock = held_mask(start_button | forward);
	prepare(stock, stock, stock);
	step_plan plan = plan_at(100);
	check(!plan.refusal && plan.port == 1 && plan.stock == stock && plan.in_hold && plan.early == two, "the press counts from the word ahead");
	check(plan.use.mask == both && plan.use.source == 103 && plan.use.word == two && plan.use.uses_previous && plan.use.previous_source == 102 &&
			  plan.use.previous_word == 0,
		"the step used buttons 1 and 2 of words 103 and 102, not their directions");
	check(plan.slot == u16{held_mask(start_button | forward | two)} && !plan.previous_held, "the slot holds it, with the stock directions");
	check(plan.escape.base == 100 && plan.escape.early[1] == std::array<u8, 4>{0x03, 0, 0, 0} && !plan.escape.used_held, "word 103 is due in the stock words");
	check(!step_and_check(plan), "the latch entry finds the planned word, with 2 pressed");
	prepare(stock, stock, stock);
	check(says(step_and_check(plan, step_registers{stock, 0}), "throw_break_model_mismatch"), "the step loaded the stock word");
	prepare(stock, stock, stock);
	check(says(step_and_check(plan, step_registers{0, 0}), "throw_break_model_mismatch"), "a blanked word outside Practice");
	m.put32(game_mode, practice_mode);
	prepare(stock, stock, stock);
	check(!step_and_check(plan, step_registers{0, 0}), "Practice blanks the word under its input display controls");
	m.put32(game_mode, 0);
	prepare(stock, stock, stock);
	{
		step_plan moved = plan;
		take_plan(record, moved);
		m.put32(side_1 + actor::previous_held, held_mask(two));
		const u16 held = static_cast<u16>(*moved.slot);
		note_step(record, read_actor(g, side_1), held);
		check(says(check_latch_entry(g, side_1, 100, ahead, row, false, {held, 0}, span_of), "throw_break_model_mismatch"),
			"+0x6c4 set by something else (input mode 0)");
	}

	// Word 103 rises in the stock words at tick 103: it is the copy of the press used at 100, so it is held but not fresh.
	words = {{105, held_mask(two)}, {106, held_mask(two)}};
	escape_state used;
	used.base = 102;
	used.early[1] = {0x01, 0, 0, 0};
	prepare(held_mask(two), 0, 0, used);
	record.previous_step = 102;
	plan = plan_at(103);
	check(plan.masked == two && !plan.early && plan.escape.used_held == 0x2 && plan.escape.early[1] == std::array<u8, 4>{}, "the copy is masked");
	check(!plan.slot && plan.previous_held == u32{two}, "+0x6c4 holds 2 already, so the step does not press it");
	check(!step_and_check(plan), "and the latch entry finds it held, not pressed");
	// Outside every window the copy is masked all the same.
	m.put32(side_1 + actor::move, lay.moves[0][0]);
	m.put32(side_1 + actor::move_moveset, layout::base[0]);
	prepare(held_mask(two), 0, 0, used);
	record.previous_step = 102;
	plan = plan_at(103);
	check(plan.masked == two && !plan.use.reads() && plan.previous_held == u32{two} && !plan.in_hold, "a copy reaching the fighter's own move");
	// Another press, which did not fire ahead, fires as it arrives in the fighter's own move.
	prepare(held_mask(two), 0, 0);
	record.previous_step = 102;
	plan = plan_at(103);
	check(!plan.masked && !plan.missed && !plan.slot && !plan.previous_held && !plan.use.reads(), "a stock press in the fighter's own move: nothing written");
	// In a window it was made before the window opened: too early, it misses, held as A holds it and never fresh.
	m.put32(side_1 + actor::move, lay.moves[1][1]);
	m.put32(side_1 + actor::move_moveset, layout::base[1]);
	prepare(held_mask(two), 0, 0);
	record.previous_step = 102;
	words = {{105, held_mask(two)}, {106, held_mask(two)}};
	plan = plan_at(103);
	check(plan.missed == two && !plan.masked && !plan.early && plan.escape.used_held == 0x2 && !plan.slot && plan.previous_held == u32{two},
		"a stock press in a window misses: held, not pressed");
	check(!step_and_check(plan), "and the latch entry finds it so");
	prepare(held_mask(two), 0, 0);
	record.previous_step = 102;
	words = {{105, 0}, {106, 0}};
	plan = plan_at(103);
	check(plan.missed == two && plan.slot == u16{0} && !plan.previous_held, "released in A: not held either");
	// A frame without fresh presses: the press ahead is used up, as offline.
	words = {{102, 0}, {103, held_mask(one)}};
	prepare(0, 0, no_fresh_mark);
	plan = plan_at(100);
	check(plan.use.no_fresh && plan.early == one && plan.escape.early[0] == std::array<u8, 4>{0x03, 0, 0, 0} && !plan.previous_held, "the game's no-fresh frame keeps its mark");

	// Refusals.
	words = {{102, 0}};
	prepare(0, 0, 0);
	check(says(plan_at(100).refusal, "throw_break_source_missing"), "a word not fixed yet is never guessed");
	words = {{103, 0}};
	check(says(plan_at(100).refusal, "throw_break_source_missing"), "nor the previous step's");
	words = {{102, 0}, {103, 0}};
	record.previous_known = false;
	check(says(plan_at(100).refusal, "throw_break_previous_step_unknown"), "a window whose previous step is not held");
	prepare(0, 0, 0);
	record.previous_ahead = 2;
	check(!plan_at(100).use.reads() && !plan_at(100).refusal, "the first step with a new L tracks from there");
	prepare(0, 0, 0);
	check(says(at_word_load(g, side_1, 0, 100, ahead, row, false, span_of, held_at).refusal, "throw_break_port_mismatch"), "another port's slot");
	check(!at_word_load(g, side_1, 0, 100, std::array<u32, 2>{0, 0}, row, false, span_of, held_at).refusal, "without L nothing is read");
	check(!at_word_load(g, side_1, 1, 100, std::array<u32, 2>{3, 0}, row, false, span_of, held_at).use.reads(), "L of the other port");

	// A step the word load did not see.
	prepare(0, 0, 0);
	check(says(check_latch_entry(g, side_1, 100, ahead, row, false, {0, 0}, span_of), "throw_break_step_unhooked"), "a window step from another source");
	m.put32(side_1 + actor::move, lay.moves[0][0]);
	m.put32(side_1 + actor::move_moveset, layout::base[0]);
	prepare(held_mask(forward), 0, 0);
	check(!check_latch_entry(g, side_1, 100, ahead, row, false, {held_mask(forward), 0}, span_of) && record.stock == held_mask(forward),
		"a step that needed nothing still leaves its stock word for the next");
}

PBD_TEST(PbdThrowBreak, AttackerNeverMoves)
{
	// The thrower runs its own moves, whose cancels read input (follow-ups, the d/f+2 of a lift): nothing is read ahead or
	// written for it at any frame, whatever its words ahead hold.
	moveset_def set;
	set.moves = {{0, {on(0, 0x2, -1, 15, 65), on(0x0010, 0x3, -1, 1, 70), on(0x8007, 0, -1, 1, 70)}}};
	set.sequences = {{30, {1, 1, 2}}};
	const layout lay = lay_out(set);
	guest g;
	g.banks = &lay.banks;
	memory& m = g.state;
	m.put16(side_0 + actor::port, 0);
	m.put32(side_0 + actor::moveset, layout::base[0]);
	m.put32(side_0 + actor::move_moveset, layout::base[0]);
	m.put32(side_0 + actor::move, lay.moves[0][0]);
	m.put32(side_0 + actor::life, 0x00910000);
	span_cache spans;
	const auto span_of = [&](u32 move, u32 moveset)
	{
		return spans.get(g, move, moveset);
	};
	const auto held_at = [](u16, u32 source) -> std::optional<u16>
	{
		return static_cast<u16>(source % 3 ? held_mask(two | back) : held_mask(one | forward));
	};
	std::array<port_record, 2> row{};
	row[0].previous_known = row[0].previous_gate_open = true;
	row[0].previous_ahead = 6;
	for (u16 frame = 0; frame <= 80; ++frame)
	{
		m.put16(side_0 + actor::frame, frame);
		m.put32(slot_of(0), held_mask(frame % 2 ? two : 0));
		row[0].previous_step = 99 + frame;
		row[0].previous_stock = held_mask(frame % 2 ? 0 : two);
		const step_plan plan = at_word_load(g, side_0, 0, 100 + frame, std::array<u32, 2>{6, 6}, row, false, span_of, held_at);
		check(!plan.refusal && !plan.use.reads() && !plan.writes() && !plan.view && !plan.early && !plan.in_hold && plan.escape.empty(),
			"the thrower's own moves are never moved");
	}
}

PBD_TEST(PbdThrowBreak, OfflineTimingObservations)
{
	// Timed offline: presses up to frame 14 after the grab break, frame 15 misses, and a 1..15 sf 15 break plays 15 frames
	// after the grab whenever the press came.
	const world w = throw_world({{1, 15, 15, break_class::one}});
	scenario c = base_of(w);
	for (s64 k = 0; k <= 14; ++k)
	{
		c.samples = timeline({{c.grab + k, one, 1}});
		check(run(c).committed(2) == c.grab + 15, "frame 0..14 breaks, at frame 15");
	}
	c.samples = timeline({{c.grab + 15, one, 1}});
	check(!run(c).committed(2), "frame 15 misses");
	c.samples = timeline({{c.grab - 1, one, 1}});
	check(!run(c).committed(2), "a press before the grab is forgotten");
	c.samples = timeline({{c.grab, one, 1}});
	check(run(c).committed(2), "a press on the grab frame counts: the hold is committed before the input step");
	c.samples = timeline({{c.grab + 3, two, 1}, {c.grab + 5, one, 1}});
	check(!run(c).committed(2) && run(c).wanted == 1 && !run(c).matched, "the first button decides");
	const world press_frame = throw_world({{1, 15, 1, break_class::one}});
	scenario d = base_of(press_frame);
	d.samples = timeline({{d.grab + 6, one, 1}});
	check(run(d).committed(2) == d.grab + 7, "a press-frame break plays on the next tick");
}

PBD_TEST(PbdThrowBreak, ThrowBreaks)
{
	u32 compared = 0, differ = 0;
	for (const auto& windows : shapes)
	{
		const world w = throw_world(windows);
		const break_class wanted = windows.front().wanted;
		const auto counts = sweep(base_of(w, 130), windows.back().last + 4, [&](s64 k)
			{
				return patterns(button_for(wanted), wrong_for(wanted), k, 12);
			});
		compared += counts.compared;
		differ += counts.differ_without;
	}
	check(compared > 10000 && differ, "the sweep ran; without the rule late presses miss and early ones count");
}

PBD_TEST(PbdThrowBreak, EveryCommandCode)
{
	struct code
	{
		u16 direction, buttons, right, wrong, first, last;
	};
	const std::vector<code> codes{
		{0, 0x0002, two, one, 1, 2},                                    // 2 fresh: the running tackle's escape
		{0, 0x0003, both, two, 18, 19},                                 // 1+2 fresh on one frame: its reversal
		{0, 0x2003, both, three, 1, 11},                                // one of 1 and 2 fresh with both held
		{0, 0x100f, three, forward, 1, 8},                              // any button fresh
		{0, 0x0004, three, one, 1, 11},                                 // 3 fresh: Ganryu's Tasukizori
		{0, 0x000c, three | four, four, 1, 11},                         // 3+4 fresh: Tsuri Otoshi
		{0, 0x4000, two, one, 5, 9},                                    // nothing fresh
		{0, 0x4001, two, one, 5, 9},                                    // nothing held
		{0x0040, 0x2005, forward | one | three, one | three, 1, 10},    // forward held with 1+3, one fresh: a chicken
		{0x0200, 0x200a, up | forward | two | four, two | four, 1, 10}, // up-forward with 2+4: a chicken
		{0x0010, 0x0000, back, forward, 59, 61},                        // back held: an escape by direction
		{0x0410, 0x0000, back, forward, 59, 61},                        // back fresh: Jinpachi's
	};
	u32 differ = 0;
	for (const code& each : codes)
	{
		const world w = single_escape(on(each.direction, each.buttons, 2, each.first, each.last), static_cast<u16>(each.last + 10));
		const auto counts = sweep(base_of(w, each.last + 40u), each.last + 4, [&](s64 k)
			{
				return patterns(each.right, each.wrong, k, 12);
			});
		differ += counts.differ_without;
	}
	check(differ, "without the rule the input delay changes escapes");
}

PBD_TEST(PbdThrowBreak, GanryuKumite)
{
	const world w = ganryu_world();
	const std::array<std::pair<u16, u16>, 3> presses{{{three, one}, {four, three}, {static_cast<u16>(three | four), two}}};
	for (int move = 1; move <= 3; ++move)
	{
		scenario base = base_of(w, 80);
		base.held = move;
		const u16 right = presses[move - 1].first, wrong = presses[move - 1].second;
		sweep(base, 15, [&](s64 k)
			{
				return patterns(right, wrong, k, 12);
			});
		scenario lockout = base;
		lockout.samples = timeline({{lockout.grab + 2, wrong, 1}, {lockout.grab + 4, right, 1}});
		const outcome offline = run(lockout);
		check(offline.last_move() == 5 && !offline.committed(4), "offline: a wrong button first locks the escape out");
		for (u32 delay = 1; delay <= 6; ++delay)
			check(run(online(lockout, delay, delay)) == offline, "online: the same lockout on the same ticks");
	}
}

PBD_TEST(PbdThrowBreak, UltimateTackleMashing)
{
	// The mount's own escapes, and its presses mashed into the arm lock that the thrower starts at its frame 45: the mount
	// can still lead into another hold until then, so every press made in it counts at offline timing.
	const world w = mount_chain_world();
	constexpr u32 lock = 44; // the arm lock's first tick after the grab
	for (const auto& buttons : {std::pair<u16, u16>{two, one}, std::pair<u16, u16>{both, two}})
		sweep(mount_chain(w, mount_path::lock, 140), 24, [&](s64 k)
			{
				return patterns(buttons.first, buttons.second, k, 12);
			});
	// Mashing from the mount's last frames on into the lock.
	const auto mash = [](s64 k)
	{
		std::vector<std::vector<press>> out;
		for (const u32 count : {3u, 4u, 6u})
			for (const s64 gap : {s64{2}, s64{3}, s64{5}})
			{
				std::vector<press> presses;
				for (u32 n = 0; n < count; ++n)
					presses.push_back({k + gap * n, two, 1});
				presses.push_back({k + gap * count, both, 1});
				out.push_back(presses);
			}
		out.push_back({{k, two, 1}, {k + 2, both, 1}, {k + 4, two, 1}, {k + 6, both, 1}});
		return out;
	};
	const auto counts = sweep(mount_chain(w, mount_path::lock, 140), lock + 31, mash, 20);
	check(counts.differ_without, "without the rule the delay moves the mash against the window");
	scenario mashed = mount_chain(w, mount_path::lock, 140);
	std::vector<press> presses;
	for (const u32 at : {50u, 53u, 56u, 59u})
		presses.push_back({mashed.grab + at, two, 1});
	presses.push_back({mashed.grab + 62, both, 1});
	mashed.samples = timeline(presses);
	check(run(mashed).last_move() == 5, "the model escapes on four presses before 1+2");
	presses.insert(presses.begin(), {{mashed.grab + 44, two, 1}, {mashed.grab + 47, two, 1}});
	mashed.samples = timeline(presses);
	check(run(mashed).last_move() == 6, "and reverses on six");
}

PBD_TEST(PbdThrowBreak, AMashPressBetweenWindowsCountsOnceAtTheDelay)
{
	// A press in a move that reads nothing and that the held fighter's own move led to (here the punch after the tackle;
	// in the shipped movesets a fighter caught again after an escape), where no hold can go on, keeps the delay: it is never
	// lost, it reaches the command history once, D ticks later, as online today, while the presses inside the lock count at
	// offline timing.
	const world w = tackle_world(4);
	scenario c = base_of(w, 110);
	const u32 between = c.grab + 22, lock = c.grab + 26;
	const std::vector<press> presses{{between, two, 1}, {c.grab + 44, two, 1}, {c.grab + 47, two, 1}, {c.grab + 50, two, 1}, {c.grab + 53, both, 1}};
	c.samples = timeline(presses);
	const outcome offline = run(c);
	check(offline.last_move() == 4 && offline.committed(4) == c.grab + 54, "offline the punch's press counts: four, then 1+2");
	const auto fresh_ticks = [](const outcome& o, u8 button)
	{
		std::vector<u32> ticks;
		for (u32 at = 0; at < o.history.size(); ++at)
			if (o.history[at].fresh & button)
				ticks.push_back(at);
		return ticks;
	};
	const std::vector<u32> offline_twos = fresh_ticks(offline, 2);
	for (u32 delay = 1; delay <= 6; ++delay)
	{
		const outcome with_rule = run(online(c, delay, delay));
		check(!with_rule.refusal && with_rule.commits == offline.commits, "the escape lands on its offline tick");
		std::vector<u32> twos = fresh_ticks(with_rule, 2);
		check(twos.size() == offline_twos.size(), "every press of 2 counts once");
		check(twos.front() == offline_twos.front() + delay && twos.front() < lock + delay, "the punch's press at the normal delay");
		check(std::equal(twos.begin() + 1, twos.end(), offline_twos.begin() + 1), "the lock's presses at offline timing");
		check(fires_at_most_once(with_rule, c.samples), "none twice");
	}
}

PBD_TEST(PbdThrowBreak, ASequenceSeesPressesInOfflineOrder)
{
	// A sequence matches the command history, so presses made before its move count for it, in order, while they are within
	// its frames. Every press made while the hold can go on (the mount, and the left-right punch that reads nothing, while
	// the thrower can still start a pair) reaches the history at offline timing, so the arm lock's mashes and the right
	// punch's 1, 1, 2 land as offline with presses before and inside them, and the oldest press leaves the sequence's frames
	// on its offline tick.
	const world w = mount_chain_world();
	const auto mash = [](s64 k)
	{
		std::vector<std::vector<press>> out;
		for (const u32 count : {4u, 5u, 6u, 7u})
			for (const s64 gap : {s64{2}, s64{3}, s64{9}})
			{
				std::vector<press> presses;
				for (u32 n = 0; n < count; ++n)
					presses.push_back({k + gap * n, two, 1});
				presses.push_back({k + gap * count, both, 1});
				out.push_back(presses);
			}
		return out;
	};
	u32 differ = 0;
	differ += sweep(mount_chain(w, mount_path::lock), 64, mash, 0).differ_without;
	differ += sweep(mount_chain(w, mount_path::punches_then_lock), 80, mash, 10).differ_without;
	const auto punch = [](s64 k)
	{
		std::vector<std::vector<press>> out;
		for (const s64 gap : {s64{2}, s64{5}, s64{9}, s64{14}})
		{
			out.push_back({{k, one, 1}, {k + gap, one, 1}, {k + 2 * gap, two, 1}});
			out.push_back({{k, one, 1}, {k + gap, two, 1}, {k + 2 * gap, one, 1}, {k + 3 * gap, two, 1}});
			out.push_back({{k, two, 1}, {k + gap, one, 1}, {k + 2 * gap, one, 1}, {k + 2 * gap + 1, two, 1}});
		}
		return out;
	};
	differ += sweep(mount_chain(w, mount_path::punch), 62, punch, 10).differ_without;
	check(differ, "without the rule the delay changes which sequences land");

	// The arm lock opens at grab + 44; six 2s from grab + 30, then 1+2 58 frames after the first reverse, 59 frames after it
	// the first has left the 59 frames and four 2s then 1+2 escape instead.
	for (const s64 after : {s64{58}, s64{59}})
	{
		scenario c = mount_chain(w, mount_path::lock);
		const s64 first = static_cast<s64>(c.grab) + 30;
		std::vector<press> presses;
		for (s64 n = 0; n < 6; ++n)
			presses.push_back({first + 3 * n, two, 1});
		presses.push_back({first + after, both, 1});
		c.samples = timeline(presses);
		const outcome offline = run(c);
		check(offline.last_move() == (after == 58 ? 6 : 5) && offline.committed(after == 58 ? 6 : 5) == first + after + 1, "offline");
		for (u32 delay = 1; delay <= 6; ++delay)
			check(run(online(c, delay, delay)) == offline, "the oldest press ages out on its offline tick" + described(presses, delay, run(online(c, delay, delay)), offline));
	}
	// A press made before the grab, in the fighter's own moves, keeps the delay. Where its copy arrives in the mount's 2
	// window, too early for it, it never escapes the mount and never reaches the command history either, which offline it
	// does at its tick: a lock whose 59 frames reach back before the grab counts it offline only.
	{
		scenario c = mount_chain(w, mount_path::lock);
		const s64 grab = static_cast<s64>(c.grab);
		std::vector<press> presses{{grab - 5, two, 1}};
		for (s64 n = 1; n < 6; ++n)
			presses.push_back({grab - 5 + 9 * n, two, 1});
		presses.push_back({grab + 49, both, 1});
		c.samples = timeline(presses);
		const outcome offline = run(c);
		check(offline.last_move() == 6, "offline the press before the grab is the sixth 2: the lock reverses");
		for (u32 delay = 1; delay <= 6; ++delay)
		{
			const outcome with_rule = run(online(c, delay, delay));
			if (delay < 5)
				check(with_rule == offline, "a copy that arrives before the grab reaches the history");
			else
				check(with_rule.committed(1) && !with_rule.committed(5) && !with_rule.committed(6), "one inside the mount's window does neither");
		}
	}
	// The right punch opens at grab + 44: 1 and 1 just before it, then 2 inside, escape at once; 2 before the second 1 does
	// not.
	for (const bool in_order : {true, false})
		for (u32 delay = 1; delay <= 6; ++delay)
		{
			scenario c = mount_chain(w, mount_path::punch);
			const s64 opens = static_cast<s64>(c.grab) + 44;
			const std::vector<press> presses = in_order ? std::vector<press>{{opens - 3, one, 1}, {opens - 1, one, 1}, {opens + 1, two, 1}} :
			                                              std::vector<press>{{opens - 3, one, 1}, {opens - 1, two, 1}, {opens + 1, one, 1}};
			c.samples = timeline(presses);
			const outcome offline = run(c);
			check(offline.committed(5).has_value() == in_order, "offline");
			check(run(online(c, delay, delay)) == offline, "the punch's sequence in offline order" + described(presses, delay, run(online(c, delay, delay)), offline));
		}
}

PBD_TEST(PbdThrowBreak, AHoldGoesOnOnlyWithTheMoveThatGaveIt)
{
	// The mount outlasts the move whose pair gave it: at grab + 10 the thrower commits a move of its own that can start a
	// pair (a walk, a throw attempt). The hold no longer goes on, so a press between the mount's windows keeps the delay, as
	// online today; while the mount's own move holds the fighter, the same press reaches the command history at offline
	// timing.
	const world w = mount_chain_world();
	const auto fresh_ticks = [](const outcome& o, u8 button)
	{
		std::vector<u32> ticks;
		for (u32 at = 0; at < o.history.size(); ++at)
			if (o.history[at].fresh & button)
				ticks.push_back(at);
		return ticks;
	};
	for (const bool walked : {false, true})
	{
		scenario c = mount_chain(w, mount_path::lock, 100);
		c.thrower.clear();
		if (walked)
			c.thrower_own = {{c.grab + 10, 11}};
		const u32 at = c.grab + 30;
		c.samples = timeline({{at, one, 1}});
		const outcome offline = run(c);
		check(fresh_ticks(offline, 1) == std::vector<u32>{at}, "offline the press is fresh on its tick");
		for (u32 delay = 1; delay <= 6; ++delay)
		{
			const outcome with_rule = run(online(c, delay, delay));
			const outcome without = run(online(c, delay, delay, hook::none));
			check(!with_rule.refusal && with_rule.commits == offline.commits, "the hold ends as offline");
			check(fresh_ticks(without, 1) == std::vector<u32>{at + delay}, "online today the press is fresh D ticks later");
			check(fresh_ticks(with_rule, 1) == (walked ? fresh_ticks(without, 1) : fresh_ticks(offline, 1)),
				walked ? "a holder that has gone on: the delay" : "the move that gave the mount: offline timing");
		}
	}
}

PBD_TEST(PbdThrowBreak, APressBeforeAHoldThatGoesOnNeverCountsAfterALaterOne)
{
	// 2 just before the grab, 1 just after it. Offline both reach the command history, 2 first. Online the 1 counts at
	// offline timing (the hold can go on), and the 2's copy arrives later: in the mount's 2 window it was too early, and in
	// the gap after it, where the hold goes on, it would reach the history after the 1, so it misses there too. A lock's
	// sequence never reads them out of order.
	const world w = mount_chain_world();
	const auto fresh_ticks = [](const outcome& o, u8 button)
	{
		std::vector<u32> ticks;
		for (u32 at = 0; at < o.history.size(); ++at)
			if (o.history[at].fresh & button)
				ticks.push_back(at);
		return ticks;
	};
	scenario c = mount_chain(w, mount_path::lock, 100);
	c.samples = timeline({{c.grab - 1, two, 1}, {c.grab + 1, one, 1}});
	const outcome offline = run(c);
	check(fresh_ticks(offline, 2) == std::vector<u32>{c.grab - 1} && fresh_ticks(offline, 1) == std::vector<u32>{c.grab + 1}, "offline");
	for (u32 delay = 1; delay <= 6; ++delay)
	{
		const outcome with_rule = run(online(c, delay, delay));
		check(!with_rule.refusal && with_rule.commits == offline.commits, "the mount as offline");
		check(fresh_ticks(with_rule, 1) == std::vector<u32>{c.grab + 1}, "the 1 at offline timing");
		check(fresh_ticks(with_rule, 2).empty(), "the 2 never after it");
		check(fires_at_most_once(with_rule, c.samples), "nothing twice");
	}
}

PBD_TEST(PbdThrowBreak, ALatePressIsHeldOnlyOnTheStepItFires)
{
	// A hold whose 1, 1, 1, 2 sequence reads presses from frame 10 (its 30 frames reach back from 39), and whose 20..25
	// tell 1+2 (to 2) from a plain 2 (to 3). 1 is held from frame 8, before anything reads it, to frame 19; 2 is pressed at
	// frame 21. Offline 1 is released by then: a plain 2. Online 1's copy fires late, inside the sequence's frames from D 2,
	// and the stock words still hold it at frame 21; held from the word ahead after its step, it is released as offline.
	moveset_def set;
	set.sequences = {{30, {1, 1, 1, 2}}};
	set.moves = {{0}, {1, {on(0x8007, 0, 4, 39, 50), on(0, 0x2003, 2, 20, 25), on(0, 0x2002, 3, 20, 25), automatic(4, 60)}}, {1, {}, true},
		{1, {}, true}, {1, {}, true}};
	const world w(set);
	scenario c = base_of(w, 100);
	c.samples = timeline({{c.grab + 7, one, 12}, {c.grab + 20, two, 1}});
	const outcome offline = run(c);
	check(offline.last_move() == 3 && offline.committed(3) == c.grab + 21, "offline a plain 2");
	for (u32 delay = 1; delay <= 6; ++delay)
	{
		const outcome with_rule = run(online(c, delay, delay));
		check(with_rule == offline, "a plain 2 at every D" + described({{c.grab + 7, one, 12}, {c.grab + 20, two, 1}}, delay, with_rule, offline));
		check(with_rule.history.size() > c.grab + 7 + delay && with_rule.history[c.grab + 7 + delay].fresh == 1, "the late 1 fires as it arrives");
	}
}

PBD_TEST(PbdThrowBreak, TheExtraEscapesLandOnTheirOfflineTicks)
{
	// Three more inputs take the held fighter out of a hold, so they count as escapes: Bryan's SN_UKLP2y (back held in
	// 11..40, sf 11, to SN_sway), Xiaoyu's Fu_xiao01_y (d/b, d or d/f held in 1..30, sf 41) and Ganryu's Sm_love_y against
	// Julia (1 in 50..105, sf 96, to Jl_superLP). Each lands on its offline tick at every D.
	struct extra
	{
		cancel_def escape;
		std::vector<std::pair<u16, u16>> presses; // right, wrong
	};
	const std::vector<extra> extras{
		{on(0x0010, 0, 2, 11, 40, 11), {{back, forward}, {static_cast<u16>(back | down), up}}},
		{on(0x000e, 0, 2, 1, 30, 41), {{down, up}, {static_cast<u16>(down | back), forward}, {static_cast<u16>(down | forward), back}}},
		{on(0, 0x2001, 2, 50, 105, 96), {{one, two}, {both, three}}},
	};
	u32 differ = 0;
	for (const extra& each : extras)
	{
		const world w = single_escape(each.escape, static_cast<u16>(each.escape.last + 10));
		for (const auto& [right, wrong] : each.presses)
			differ += sweep(base_of(w, each.escape.last + 40u), each.escape.last + 4, [&](s64 k)
				{
					return patterns(right, wrong, k, 12);
				}).differ_without;
	}
	check(differ, "without the rule the delay moves them");
}

PBD_TEST(PbdThrowBreak, DragunovGuess)
{
	// The held fighter guesses 1, 2 or 1+2 in frames 21..45 (a 0x8060 setter keeps it); requirement 208 compares it with
	// the thrower's choice at frame 1 of the guess move. 0 standing, 1 the hold, 2-4 the guesses, 5 thrown, 6 escaped.
	moveset_def set;
	const auto guess = [](u16 buttons, int target, u16 value)
	{
		cancel_def cancel = on(0, buttons, target, 21, 45);
		cancel.requirements = {{0x8060, value}};
		return cancel;
	};
	const auto judged = [](u16 value)
	{
		cancel_def escape = automatic(6, 1);
		escape.requirements = {{208, value}};
		return move_def{1, {escape, automatic(5, 2)}};
	};
	set.moves = {{0}, {1, {guess(0x3, 4, 3), guess(0x1, 2, 1), guess(0x2, 3, 2), automatic(5, 60)}}, judged(1), judged(2), judged(3),
		{1, {}, true}, {1, {}, true}};
	const world w(set);
	for (const u16 choice : {u16{1}, u16{2}, u16{3}})
	{
		scenario base = base_of(w, 100);
		base.attacker_choice = choice;
		sweep(base, 50, [&](s64 k)
			{
				return patterns(choice == 1 ? one : choice == 2 ? two :
																  both,
					choice == 1 ? two : one, k, 12);
			});
	}
}

PBD_TEST(PbdThrowBreak, TechRolls)
{
	// As the shipped thrown moves: the break window early, any button rolls on landing in frames 60..68 of the same move.
	// 0 standing, 1 the throw (break 1 in 1..10), 2 broken, 3 rolled, 4 down.
	moveset_def set;
	set.moves = {{0}, {1, {break_cancel(break_class::one, 2, 1, 10, 15), tech_roll(3, 60, 68), automatic(4, 80)}}, {1, {}, true},
		{1, {}, true}, {1, {}, true}};
	const world w(set);
	// The break counts ahead.
	sweep(base_of(w, 110), 14, [](s64 k)
		{
			return patterns(one, two, k, 12);
		});
	// A tech roll keeps the delay: at every D, a press lands on the same tick as without the rule.
	u32 compared = 0, as_offline = 0;
	for (s64 k = 12 + 40; k <= 12 + 75; ++k)
		for (const auto& presses : patterns(three, forward, k, 12))
		{
			scenario offline = base_of(w, 110);
			offline.samples = timeline(presses);
			const outcome expected = run(offline);
			for (u32 delay = 1; delay <= 6; ++delay)
			{
				const outcome stock = run(online(offline, delay, 0, hook::none));
				const outcome with_rule = run(online(offline, delay, delay));
				check(!with_rule.refusal && !with_rule.slot_changed && with_rule == stock, "a tech roll on the tick it has without the rule");
				scenario practice = online(offline, delay, delay);
				practice.practice = true;
				check(run(practice) == stock, "in Practice too");
				as_offline += with_rule == expected;
				++compared;
				// The developer switch counts them as escapes, for a feel test.
				scenario feel = online(offline, delay, delay);
				feel.tech_rolls = true;
				check(run(feel) == expected, "with the developer switch, at offline timing");
			}
		}
	check(compared > 1000 && as_offline < compared, "the delay still moves tech rolls");
}

PBD_TEST(PbdThrowBreak, TheFirstActionAfterABreakKeepsTheDelay)
{
	// 0 standing; 1 the hold (break 1 in 1..10, sf 11); 2 the break, which reads nothing and returns to the fighter's own
	// moves; 3 recovery, where 1 jabs and 2 kicks; 4 thrown, 5 jabbed, 6 kicked, 7 standing; 8 a thrower's move that can
	// start a pair up to frame 60.
	moveset_def set;
	set.moves = {{0}, {1, {break_cancel(break_class::one, 2, 1, 10, 11), automatic(4, 30)}}, {1, {automatic(3, 4)}},
		{0, {on(0, 0x1, 5, 1, 60), on(0, 0x2, 6, 1, 60), automatic(7, 60)}}, {1, {}, true}, {0, {}, true}, {0, {}, true}, {0, {}, true},
		{1, {follow_up(1, 60, 61)}}};
	const world w(set);
	// The break is a move the fighter queued itself, so nothing after it reads ahead, though the thrower could still start
	// another pair: a press made in the break's animation, which reads nothing, acts in the recovery as online today.
	u32 jabs = 0;
	for (u32 delay = 1; delay <= 6; ++delay)
	{
		scenario c = base_of(w, 120);
		c.holder = 8;
		c.samples = timeline({{c.grab + 1, one, 1}, {c.grab + 13, one, 1}});
		const outcome with_rule = run(online(c, delay, delay)), today = run(online(c, delay, 0, hook::none));
		check(with_rule.commits == today.commits, "the break's animation keeps the delay");
		jabs += with_rule.committed(5).has_value() && !run(c).committed(5);
	}
	check(jabs, "a press there jabs where offline it would not, as online today");
	for (const u16 action : {one, two})
		for (const s64 at : {s64{20}, s64{24}, s64{30}})
		{
			scenario c = base_of(w, 120);
			c.samples = timeline({{c.grab + 4, one, 1}, {c.grab + at, action, 1}});
			const outcome offline = run(c);
			const int acted = action == one ? 5 : 6;
			check(offline.committed(2) && offline.committed(acted), "offline: the break, then the action");
			for (u32 delay = 1; delay <= 6; ++delay)
			{
				const outcome with_rule = run(online(c, delay, delay));
				check(with_rule.committed(2) == offline.committed(2), "the break at offline timing");
				check(with_rule.committed(acted) == *offline.committed(acted) + delay, "the first action after it D ticks later, as online today");
			}
		}
}

PBD_TEST(PbdThrowBreak, ABreakNeverAlsoAttacks)
{
	// 1+2 breaks the throw; in the fighter's own moves after the break, 1+2 is an attack. The break's press is used up, so
	// its copy in the stock words, which arrives in the fighter's own move, never attacks. 0 standing; 1 the hold (1+2 in
	// 1..10, sf 11); 2 the break; 3 recovery; 4 thrown, 5 the 1+2 attack, 6 standing.
	moveset_def set;
	set.moves = {{0}, {1, {break_cancel(break_class::both, 2, 1, 10, 11), automatic(4, 30)}}, {1, {automatic(3, 2)}},
		{0, {on(0, 0x3, 5, 1, 60), automatic(6, 60)}}, {1, {}, true}, {0, {}, true}, {0, {}, true}};
	const world w(set);
	u32 attacks_without_mask = 0;
	for (s64 k = 0; k <= 9; ++k)
		for (const u32 frames : {1u, 3u})
		{
			scenario c = base_of(w, 100);
			c.samples = timeline({{c.grab + k, both, frames}});
			const outcome offline = run(c);
			check(offline.committed(2) && !offline.committed(5), "offline: the break, no attack");
			for (u32 delay = 1; delay <= 6; ++delay)
			{
				const outcome with_rule = run(online(c, delay, delay));
				check(with_rule == offline && !with_rule.committed(5), "the break at offline timing, and no 1+2 attack after it");
				attacks_without_mask += run(online(c, delay, delay, hook::forget_used)).committed(5).has_value();
			}
		}
	check(attacks_without_mask, "a rule that forgot the presses it used would attack with the break's copy");
}

PBD_TEST(PbdThrowBreak, ATooEarlyPressMissesAsOffline)
{
	// A press made up to L ticks before a window opens is too early, as offline, though its copy arrives inside the window.
	// The right button misses; a wrong one decides nothing (no latch, no lockout), so a right press inside the window still
	// escapes. Without the rule the copy counts in the window.
	struct family
	{
		const char* name;
		const world* w;
		int held;
		s64 opens; // the window's first tick after the grab
		u16 right, wrong;
		int escaped;
		u16 all_along = 0; // held through the hold: a chicken's direction
	};
	const world breaks = throw_world({{1, 15, 15, break_class::one}});
	const world late_break = throw_world({{10, 23, 24, break_class::two}});
	const world ganryu = ganryu_world();
	const world tackle = tackle_world(4);
	const world chicken = single_escape(on(0x0040, 0x2005, 2, 1, 10), 20);
	const world jack = jack_world();
	const std::vector<family> families{
		{"a break", &breaks, 1, 0, one, two, 2},
		{"a break from frame 10", &late_break, 1, 9, two, one, 2},
		{"Tasukizori", &ganryu, 1, 0, three, one, 4},
		{"Tsuri Otoshi", &ganryu, 3, 0, three | four, two, 4},
		{"the tackle's 2", &tackle, 1, 0, two, one, 4},
		{"the tackle's 1+2", &tackle, 1, 17, both, two, 5},
		{"a chicken", &chicken, 1, 0, one | three, two | four, 2, forward},
		{"Jack-5's lift", &jack, 1, 0, both, one, 2},
	};
	u32 counted_without = 0, locked_out_without = 0;
	for (const family& each : families)
		for (u32 delay = 1; delay <= 6; ++delay)
			for (s64 before = 1; before <= delay; ++before)
			{
				scenario c = base_of(*each.w, 120);
				c.held = each.held;
				const s64 opens = static_cast<s64>(c.grab) + each.opens;
				const std::string where = std::string(" (") + each.name + ", D " + std::to_string(delay) + ", " + std::to_string(before) + " before)";
				c.samples = timeline({{opens - before, each.right, 1}, {0, each.all_along, 200}});
				const outcome early = run(c);
				check(!early.committed(each.escaped), "offline the right press is too early" + where);
				check(run(online(c, delay, delay)) == early, "online with the rule it misses on the same ticks" + where);
				counted_without += run(online(c, delay, 0, hook::none)).committed(each.escaped).has_value();
				c.samples = timeline({{opens - before, each.wrong, 1}, {opens + 1, each.right, 1}, {0, each.all_along, 200}});
				const outcome wrong_first = run(c);
				check(wrong_first.committed(each.escaped).has_value(), "offline the wrong press decides nothing" + where);
				check(run(online(c, delay, delay)) == wrong_first, "online with the rule the right one escapes on the same tick" + where);
				locked_out_without += !run(online(c, delay, 0, hook::none)).committed(each.escaped);
			}
	check(counted_without && locked_out_without, "without the rule an early right press escapes and an early wrong one locks out");
}

PBD_TEST(PbdThrowBreak, BreakWindowsOfOneMoveReadAsOne)
{
	// A chain stage breaks in 1..15 and again in 91..99. Once the first window's check has set +0x192 the latch keeps
	// running between them, so offline a press in the gap decides the second window: the right button breaks at its
	// first frame, a wrong one freezes the latch. A gap press counts at offline timing, at every D.
	const world w = throw_world({{1, 15, 1, break_class::one}, {91, 99, 1, break_class::one}});
	u32 differ_without = 0;
	for (u32 delay = 1; delay <= 6; ++delay)
		for (s64 frame = 80; frame <= 90; ++frame)
		{
			scenario c = base_of(w, 130);
			const s64 at = static_cast<s64>(c.grab) + frame - 1;
			c.samples = timeline({{at, one, 1}});
			const outcome offline = run(c);
			check(offline.committed(2) == c.grab + 91, "offline a gap press breaks at the second window's first frame");
			check(run(online(c, delay, delay)) == offline, "online with the rule on the same tick" + described({{at, one, 1}}, delay, run(online(c, delay, delay)), offline));
			differ_without += !(run(online(c, delay, 0, hook::none)) == offline);
			c.samples = timeline({{at, two, 1}, {c.grab + 90, one, 1}});
			check(!run(c).committed(2), "offline a wrong gap press freezes the latch");
			check(run(online(c, delay, delay)) == run(c), "online with the rule too");
		}
	check(differ_without, "without the rule a late gap press breaks late");
}

PBD_TEST(PbdThrowBreak, AButtonHeldIntoAWindowCompletesTheEscapeOnce)
{
	// 1 pressed just before a 1+2 window and held, then 2 inside it: 1+2 breaks, as offline. 1 went fresh before the window
	// offline, so its copy in the stock words, which arrives after 2, never fires: no jab in the fighter's own recovery.
	// 0 standing; 1 the hold (1+2 in 1..10, sf 1); 2 the break; 3 recovery, where 1 jabs; 4 thrown, 5 jabbed, 6 standing.
	moveset_def set;
	set.moves = {{0}, {1, {break_cancel(break_class::both, 2, 1, 10, 1), automatic(4, 30)}}, {1, {automatic(3, 2)}},
		{0, {on(0, 0x1, 5, 1, 60), automatic(6, 60)}}, {1, {}, true}, {0, {}, true}, {0, {}, true}};
	const world w(set);
	u32 cases = 0;
	for (u32 delay = 2; delay <= 6; ++delay)
		for (s64 before = 1; before < delay; ++before)
			for (s64 second = 0; second < static_cast<s64>(delay) - before; ++second)
			{
				scenario c = base_of(w, 100);
				const std::vector<press> presses{{static_cast<s64>(c.grab) - before, one, 12}, {static_cast<s64>(c.grab) + second, two, 2}};
				c.samples = timeline(presses);
				const outcome offline = run(c);
				check(offline.committed(2) && !offline.committed(5), "offline: 1+2 breaks, and the held 1 never jabs");
				const outcome with_rule = run(online(c, delay, delay));
				check(with_rule == offline, "the break on its offline tick" + described(presses, delay, with_rule, offline));
				check(!with_rule.committed(5), "no jab after it" + described(presses, delay, with_rule, offline));
				for (u32 t = static_cast<u32>(c.grab + second) + 1; t < with_rule.history.size(); ++t)
					check(!(with_rule.history[t].fresh & 1), "1 never fresh again after 2 used it");
				++cases;
			}
	check(cases > 30, "the cases ran");
}

PBD_TEST(PbdThrowBreak, AfterAPracticeBlankHeldButtonsAreFreshAgain)
{
	// Practice blanks a step's word under its input display controls (0xed740), and the step after it sees every held
	// button fresh again, offline and today. The rule leaves +0x6c4 as the game left it there, before the grab as in a hold.
	const world w = throw_world({{1, 15, 15, break_class::one}});
	for (const u32 blank : {6u, 16u})
		for (u32 delay = 1; delay <= 6; ++delay)
		{
			scenario c = base_of(w);
			c.practice = true;
			c.blanked = {blank};
			c.samples = timeline({{0, one | three, 40}});
			const outcome today = run(online(c, delay, 0, hook::none)), with_rule = run(online(c, delay, delay));
			check(!with_rule.refusal && with_rule == today && with_rule.history == today.history,
				"every held button fresh again after the blank, as today (blank at " + std::to_string(blank) + ", D " + std::to_string(delay) + ")");
		}
}

PBD_TEST(PbdThrowBreak, LockstepWaitsForBothWordsAhead)
{
	// A lockstep row whose fighter may read ahead runs only once the word L past it and the word L past its previous step,
	// against which its presses count, have arrived.
	std::vector<u32> in{23};
	const auto arrived = [&](u32 source)
	{
		return listed(in, source);
	};
	port_record before;
	before.stepped = true;
	before.in_hold = true;
	check(!lookahead_arrived(before, 20, 3, arrived), "row 20 reads words 23 and 22; 22 has not arrived");
	in.push_back(22);
	check(lookahead_arrived(before, 20, 3, arrived), "both in");
	before.stepped = false;
	before.previous_known = true;
	before.previous_step = 15;
	check(!lookahead_arrived(before, 20, 3, arrived), "after a pause the previous step is 15: word 18");
	in.push_back(18);
	check(lookahead_arrived(before, 20, 3, arrived), "all in");
	check(lookahead_arrived(port_record{}, 20, 3, arrived) && lookahead_arrived(before, 20, 0, [](u32)
																  {
																	  return false;
																  }),
		"a fighter that cannot read ahead, or L 0, waits for nothing");
}

PBD_TEST(PbdThrowBreak, DirectionsAndOtherButtonsKeepTheDelay)
{
	// Inside a break window only 1 and 2 move: the stick and buttons 3 and 4 reach the command history as online today, and
	// the slot holds the stock word after every step.
	const world w = throw_world({{1, 15, 15, break_class::one}, {91, 99, 100, break_class::one}});
	scenario c = base_of(w, 130);
	std::vector<press> presses{{c.grab + 6, one, 1}, {c.grab - 3, three, 2}, {c.grab + 4, four, 3}, {c.grab + 9, three | four, 2}};
	c.samples = timeline(presses);
	for (u32 b = 0; b < c.samples.size(); ++b)
		if (b % 7 < 3)
			c.samples[b] |= (b / 7) % 2 ? back : up | forward;
	for (u32 delay = 1; delay <= 6; ++delay)
	{
		const outcome with_rule = run(online(c, delay, delay)), stock = run(online(c, delay, 0, hook::none));
		check(!with_rule.slot_changed && with_rule.committed(2) == run(c).committed(2), "the break at offline timing; the slot as stored");
		check(with_rule.history.size() == stock.history.size() || with_rule.commits != stock.commits, "the same steps");
		const std::size_t steps = std::min(with_rule.history.size(), stock.history.size());
		for (std::size_t at = 0; at < steps; ++at)
		{
			const step_fields &ruled = with_rule.history[at], &plain = stock.history[at];
			check(ruled.direction == plain.direction && ruled.changed == plain.changed, "the stick as online today");
			check((ruled.fresh & 0xc) == (plain.fresh & 0xc) && (ruled.held & 0xc) == (plain.held & 0xc), "buttons 3 and 4 as online today");
		}
	}
}

PBD_TEST(PbdThrowBreak, JackJustFrameBlock)
{
	// Jack-5's u/f+1+2: after the thrower's d/f+2 the held fighter, still in Jack's moveset, blocks the punch by holding back
	// in frames 29..30 of pj_lfp_yxk, and locks the block out by holding any direction in 10..28. The scan reads the
	// directions of the word ahead there, so the block lands on its offline tick at every D.
	const world w = jack_world();
	scenario base = base_of(w, 100);
	const auto stick = [&](s64 k)
	{
		std::vector<std::vector<press>> out;
		for (const u16 way : {back, static_cast<u16>(back | down), forward})
			for (const u32 frames : {1u, 2u, 4u, 30u})
				out.push_back({{k, way, frames}});
		out.push_back({{k, back, 2}, {k + 3, both, 1}});
		return out;
	};
	const auto counts = sweep(base, 60, stick);
	check(counts.differ_without, "without the rule the delay moves the block");
	// Frame 1 of pj_lfp_yxk: the lift's frame 20 queues it, the next tick commits it.
	scenario c = base;
	const s64 entered = c.grab + 20;
	c.samples = timeline({{entered + 28, back, 2}});
	const outcome offline = run(c);
	check(offline.last_move() == 5, "offline: back at frame 29 blocks");
	c.samples = timeline({{entered + 27, back, 3}});
	check(run(c).last_move() == 4, "back from frame 28 locks the block out");
	c.samples = timeline({{entered + 28, back, 2}});
	u32 late = 0;
	for (u32 delay = 1; delay <= 6; ++delay)
	{
		check(run(online(c, delay, delay)) == offline, "the block on its offline tick");
		late += !(run(online(c, delay, delay, hook::without_view)) == offline);
	}
	check(late, "a scan that saw the stock directions would miss it");
}

PBD_TEST(PbdThrowBreak, MovesetTest)
{
	// The fighter's own move with a button cancel: the rule never moves an input and writes nothing.
	moveset_def own_set;
	own_set.moves = {{0, {on(0, 0x1, 1, 1, 100)}}, {0, {}, true}};
	const world own(own_set);
	scenario standing = base_of(own);
	standing.grab = 1000;
	standing.snapshots = true;
	standing.samples = timeline({{30, one, 1}});
	for (u32 delay = 1; delay <= 6; ++delay)
	{
		const outcome stock = run(online(standing, delay, 0, hook::none));
		const outcome with_rule = run(online(standing, delay, delay));
		check(stock.snapshots == with_rule.snapshots && stock.committed(1) == with_rule.committed(1), "the fighter's own moves keep the delay");
	}

	// A mirror match running both fighters from one loaded moveset: the break window counts ahead, an escape outside it keeps
	// the delay. 0 standing, 1 the hold (break 1 in 1..10; 2 in 18..19), 2 broken, 3 escaped, 4 thrown.
	moveset_def mirror_set;
	mirror_set.moves = {{0}, {1, {break_cancel(break_class::one, 2, 1, 10, 11), on(0, 0x2, 3, 18, 19), automatic(4, 40)}}, {1, {}, true},
		{1, {}, true}, {1, {}, true}};
	const world mirror(mirror_set);
	scenario shared = base_of(mirror);
	shared.shared = true;
	for (const s64 k : {s64{14}, s64{17}, s64{21}})
	{
		shared.samples = timeline({{k, one, 1}});
		check(run(online(shared, 3, 3)) == run(shared), "a break in a shared moveset at offline timing");
	}
	shared.samples = timeline({{shared.grab + 17, two, 1}});
	check(run(online(shared, 3, 3)) == run(online(shared, 3, 0, hook::none)), "an escape outside the break windows keeps the delay");

	// A computer fighter never reaches the word load.
	scenario computer = base_of(mirror);
	computer.computer = true;
	computer.samples = timeline({{computer.grab + 3, one, 1}});
	check(run(online(computer, 3, 3)) == run(online(computer, 3, 0, hook::none)), "computer fighters play as stock");
}

PBD_TEST(PbdThrowBreak, RuleOffOrZeroWritesNothing)
{
	// Rule off and L = 0 leave every byte as today: the slot, +0x6c4 and everything the step builds.
	const world w = throw_world({{1, 15, 15, break_class::one}});
	scenario c = base_of(w);
	c.snapshots = true;
	for (const s64 k : {s64{10}, s64{14}, s64{20}, s64{26}})
	{
		c.samples = timeline({{k, one, 1}, {k + 3, two, 4}});
		for (u32 delay = 0; delay <= 6; ++delay)
			check(run(online(c, delay, 0)).snapshots == run(online(c, delay, 0, hook::none)).snapshots, "L = 0 writes nothing");
	}
	// With L, the slot ends each tick as the input update left it.
	c.samples = timeline({{14, one, 1}});
	const outcome ran = run(online(c, 3, 3));
	check(!ran.refusal && ran.committed(2) && !ran.slot_changed, "the slot is the stock word after every step");
}

PBD_TEST(PbdThrowBreak, EquivalenceAroundGameEvents)
{
	// A paused simulation, a KO inside the window and frames without fresh presses happen to the game state, so they fall
	// on the same ticks offline and online. A press made only while the simulation is paused is one no step sees offline;
	// online its stock copy arrives after the pause, and since the word ahead never showed it, it misses in the window as
	// offline (today it breaks there).
	const world w = throw_world({{1, 15, 15, break_class::one}});
	u32 hidden = 0;
	for (u32 delay = 1; delay <= 6; ++delay)
		for (s64 k = 12; k <= 32; ++k)
		{
			scenario offline = base_of(w);
			const std::vector<press> presses{{k, one, 2}};
			offline.samples = timeline(presses);
			for (const auto& events : {std::vector<u32>{16, 17}, std::vector<u32>{13}, std::vector<u32>{22, 23, 24}})
				for (const int kind : {0, 1, 2})
				{
					scenario c = offline;
					(kind == 0 ? c.paused : kind == 1 ? c.closed_gate :
														c.no_fresh) = events;
					const outcome with_rule = run(online(c, delay, delay)), expected = run(c);
					const std::string where = described(presses, delay, with_rule, expected) + " event " + std::to_string(kind) + " at " + std::to_string(events.front());
					check(!with_rule.refusal, "no refusal" + where);
					hidden += kind == 0 && listed(events, static_cast<u32>(k)) && listed(events, static_cast<u32>(k + 1)) &&
					          !(run(online(c, delay, 0, hook::none)) == expected);
					check(with_rule == expected, "events change both timelines alike" + where);
					// A held button is fresh again after a closed gate, offline as online.
					check(kind == 1 || fires_at_most_once(with_rule, c.samples), "no press fires twice" + where);
				}
		}
	check(hidden, "presses made only in a pause, which count without the rule");
}

PBD_TEST(PbdThrowBreak, MaskedMispredictions)
{
	lookahead_use use;
	use.mask = both;
	use.source = 103;
	use.word = two;
	use.uses_previous = true;
	use.previous_source = 102;
	use.previous_word = 0;
	check(!mispredicted(use, held_mask(two | forward), 0), "the buttons it used; the stick is not compared");
	check(!mispredicted(use, held_mask(two | three | start_button), held_mask(four)), "nor buttons 3 and 4, Start or Select");
	check(mispredicted(use, held_mask(one | two), 0), "another button it used");
	check(mispredicted(use, 0, 0), "a release");
	check(mispredicted(use, two, two), "the previous word too, against which the press counts");
	use.mask = static_cast<u16>(both | direction_bits);
	use.word = held_mask(two | back);
	check(mispredicted(use, held_mask(two | forward), 0) && !mispredicted(use, held_mask(two | back | three), 0),
		"while a window reads the stick, the stick too");
	use = {};
	check(!mispredicted(use, held_mask(back), held_mask(two)), "a step that read nothing ahead");
}

PBD_TEST(PbdThrowBreak, MasksKeepFightButtons)
{
	check(keeps_fight_buttons(0x0800) && keeps_fight_buttons(0x0100) && keeps_fight_buttons(0x0900) && keeps_fight_buttons(0),
		"Start and Select");
	check(!keeps_fight_buttons(0x0060) && !keeps_fight_buttons(0x0080) && !keeps_fight_buttons(0x2000), "anything a fighter reads");
}

PBD_TEST(PbdThrowBreak, PracticeLongPause)
{
	// Practice keeps 2048 rows and its menu pauses the simulation, so the first step after a long pause inside a hold
	// compares with a step whose row is gone. The record carries what that step left.
	const world w = throw_world({{1, 15, 15, break_class::one}});
	scenario offline = base_of(w, 3300);
	for (u32 t = 17; t < 3017; ++t)
		offline.paused.push_back(t);
	for (const s64 k : {s64{14}, s64{16}, s64{3017}, s64{3019}, s64{3026}, s64{3027}})
	{
		offline.samples = timeline({{k, one, 1}}, 3300);
		scenario practice = online(offline, 3, 3);
		practice.practice = true;
		const outcome result = run(practice);
		check(!result.refusal, "no refusal after a pause longer than the rows Practice keeps");
		check(result == run(offline), "breaks as offline across the pause");
	}
	// Button 1 held from before the grab and through the pause is not fresh after it.
	offline.samples = timeline({{7, one, 3030}}, 3300);
	scenario practice = online(offline, 3, 3);
	practice.practice = true;
	check(!run(offline).committed(2) && run(practice) == run(offline), "a button held through the pause");
}

PBD_TEST(PbdThrowBreak, ModelCheck)
{
	// Where the step ran from something the rule does not model, the rule ends the session rather than leave the game on
	// a word it would not have read.
	const world w = throw_world({{1, 15, 15, break_class::one}});
	scenario base = base_of(w);
	const auto refusal = [](const scenario& c)
	{
		return run(online(c, 3, 3)).refusal;
	};
	// Button 1 pressed at frame 3: the step that fires it holds a word other than the stock one.
	base.samples = timeline({{14, one, 1}});
	check(!refusal(base), "the modelled game");
	scenario blanked = base;
	blanked.blanked = {14};
	check(says(refusal(blanked), "throw_break_model_mismatch"), "a step word blanked outside Practice");
	blanked.practice = true;
	check(!run(online(blanked, 3, 3)).refusal && run(online(blanked, 3, 3)) == run(blanked), "Practice's blank, as offline");
	// The modelled events keep passing the check: the plan reads +0x6c4 as the game left it, also after input mode 0.
	scenario events = base;
	events.paused = {13};
	events.closed_gate = {15};
	events.no_fresh = {16};
	events.edge_reset = {18};
	check(!refusal(events), "a paused simulation, a closed gate, a no-fresh frame and input mode 0 are modelled");
}

PBD_TEST(PbdThrowBreak, ConfirmationReadsAhead)
{
	std::map<u32, u16> arrived{{10, held_mask(two)}, {13, held_mask(two | forward)}};
	const auto usable = [&](u32 t)
	{
		return arrived.count(t) != 0;
	};
	const auto received = [&](u32 t) -> std::optional<u16>
	{
		const auto found = arrived.find(t);
		return found != arrived.end() ? std::optional<u16>{found->second} : std::nullopt;
	};
	const lookahead_use stock{};
	check(sources_usable(stock, usable) && !known_misprediction(stock, received), "a stock row reads nothing ahead");
	lookahead_use window;
	window.mask = both;
	window.source = 13;
	window.word = two;
	window.uses_previous = true;
	window.previous_source = 12;
	window.previous_word = 0;
	check(!sources_usable(window, usable), "a window step waits for the previous step's word too");
	check(!known_misprediction(window, received), "the word that has arrived is the one it ran, in the bits it used");
	window.word = 0;
	check(known_misprediction(window, received), "a word that has arrived and differs is wrong at once, before the other arrives");
	window.word = two;
	arrived[12] = held_mask(one);
	check(sources_usable(window, usable) && known_misprediction(window, received), "and so is the previous step's");
	arrived[12] = held_mask(back | three | start_button);
	check(!known_misprediction(window, received), "bits it did not use change nothing");

	// Rows 20..25: 23 reads word 30, which has not arrived.
	std::array<lookahead_use, 6> rows{};
	rows[2] = window;
	rows[3].mask = both;
	rows[3].source = 30;
	const auto use_at = [&](u32 t) -> const lookahead_use&
	{
		return rows[t - 20];
	};
	check(first_unconfirmed(20, 26, use_at, usable) == 23, "the first row whose words from ahead are not all usable");
	check(first_unconfirmed(20, 22, use_at, usable) == 22, "or the first whose own word is not, if that comes first");
	check(first_unconfirmed(24, 26, use_at, usable) == 26, "rows that read nothing ahead wait for nothing more");
	arrived[30] = 0;
	check(first_unconfirmed(20, 26, use_at, usable) == 26, "once the word arrives");
}

PBD_TEST(PbdThrowBreak, TwoPcsThrowBreak)
{
	// The thrower grabs with 1+3 on tick 40, so the hold starts at 41; the held player breaks with 1 at frame 1, 8 or 15 of
	// the 1..15 window (sf 15) while turning the stick, which the thrower does all along.
	const world w = throw_world({{1, 15, 15, break_class::one}});
	pair_case p;
	p.w = &w;
	p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}};
	p.ticks = 280;
	p.lockstep_from = 260;
	p.thrower_words = words_with(320, {{40, one | three, 1}}, 5, 300, 6);
	u32 read_ahead = 0;
	for (const u32 k : {0u, 7u, 14u})
	{
		p.held_words = words_with(320, {{41 + k, one, 1}}, 30, 70, 3);
		std::vector<std::pair<u32, int>> offline;
		read_ahead += play_pairs(p, &offline);
		check(offline == std::vector<std::pair<u32, int>>{{41, 1}, {56, 2}}, "offline: the break, at frame 15");
	}
	// Pressed at frame 16 the throw is not broken, and the stretch after the fight starts inside the hold: every lockstep
	// tick of the window waits for the words the held fighter read ahead.
	p.held_words = words_with(320, {{56, one, 1}}, 30, 250, 3);
	p.lockstep_from = 50;
	std::vector<std::pair<u32, int>> offline;
	read_ahead += play_pairs(p, &offline);
	check(offline == std::vector<std::pair<u32, int>>{{41, 1}, {241, 3}}, "offline: thrown at frame 200");
	check(read_ahead, "the thrower's PC corrected words the held fighter read ahead");
}

PBD_TEST(PbdThrowBreak, TwoPcsGanryuEscape)
{
	// The grab puts the held fighter in Tasukizori: 3 in frames 1..11 escapes, and a wrong button first locks it out.
	const world w = ganryu_world();
	pair_case p;
	p.w = &w;
	p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}};
	p.ticks = 140;
	p.lockstep_from = 120;
	p.thrower_words = words_with(160, {{40, one | three, 1}}, 5, 140, 5);
	u32 read_ahead = 0;
	const std::vector<std::pair<std::vector<press>, int>> plays{{{{43, three, 1}}, 4}, {{{51, three, 2}}, 4}, {{{43, one, 1}, {45, three, 1}}, 5}};
	for (const auto& [presses, last] : plays)
	{
		p.held_words = words_with(160, presses, 30, 70, 3);
		std::vector<std::pair<u32, int>> offline;
		read_ahead += play_pairs(p, &offline);
		check(!offline.empty() && offline.back().second == last, "offline: escaped, or locked out and thrown");
	}
	check(read_ahead, "the thrower's PC corrected words the held fighter read ahead");
}

PBD_TEST(PbdThrowBreak, TwoPcsTackleLockMash)
{
	// The thrower's tackle (1+3 on tick 40) runs into its punch at 63, and its 2 on tick 70 puts the held fighter in the
	// arm lock at 71, where 2 four times then 1+2 escapes in 18..65 and six times reverses in 1..60. A mash begun in the
	// punch, which reads nothing, counts its presses there at the normal delay, once.
	const world w = tackle_world(30);
	pair_case p;
	p.w = &w;
	p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}, {6, held_mask(two), 1, 25, 2, 1}};
	p.ticks = 200;
	p.lockstep_from = 180;
	p.thrower_words = words_with(240, {{40, one | three, 1}, {70, two, 1}}, 5, 200, 7);
	const auto mash = [](std::vector<u32> twos, u32 last)
	{
		std::vector<press> presses;
		for (const u32 at : twos)
			presses.push_back({at, two, 1});
		presses.push_back({last, both, 1});
		return presses;
	};
	u32 read_ahead = 0;
	const std::vector<std::pair<std::vector<press>, int>> plays{
		{mash({95, 98, 101, 104}, 107), 4}, {mash({80, 83, 86, 89, 92, 95}, 98), 5}, {mash({62, 65, 85, 88}, 91), 4}};
	for (const auto& [presses, last] : plays)
	{
		p.held_words = words_with(240, presses, 30, 120, 4);
		std::vector<std::pair<u32, int>> offline;
		read_ahead += play_pairs(p, &offline);
		check(offline.size() == 4 && offline[2] == std::pair<u32, int>{71, 2} && offline.back().second == last, "offline: the lock, then the mash");
	}
	check(read_ahead, "the thrower's PC corrected words the held fighter read ahead");
}

PBD_TEST(PbdThrowBreak, TwoPcsMount)
{
	// The mount reads 1, 2 and the stick in frames 30..250, so every turn of the held player's stick there corrects the
	// thrower's PC. The thrower's 1 on tick 71 punches (reads nothing) and returns to the mount's frame 100.
	const world w = mount_world();
	pair_case p;
	p.w = &w;
	p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}, {1, held_mask(one), 20, 90, 4, 1}};
	p.ticks = 300;
	p.lockstep_from = 285;
	p.thrower_words = words_with(340, {{40, one | three, 1}, {71, one, 1}}, 5, 320, 9);
	u32 read_ahead = 0;
	struct play
	{
		std::vector<press> presses;
		u32 turns_until;
		int last;
	};
	const std::vector<play> plays{
		{{}, 300, 3},                                                                           // thrown at frame 255
		{{{195, back, 7}, {200, both, 1}}, 190, 2},                                             // back with 1+2, frame 216
		{{{150, two, 1}, {153, two, 1}, {156, two, 1}, {159, two, 1}, {162, both, 1}}, 300, 2}, // the mash
	};
	for (const play& each : plays)
	{
		p.held_words = words_with(340, each.presses, 30, each.turns_until, 4);
		std::vector<std::pair<u32, int>> offline;
		read_ahead += play_pairs(p, &offline);
		check(offline.size() == 4 && offline[2] == std::pair<u32, int>{84, 1} && offline.back().second == each.last,
			"offline: the punch, back in the mount, then the escape or the throw");
	}
	check(read_ahead > 100, "the thrower's PC corrected the held player's turns all through the windows");
}

PBD_TEST(PbdThrowBreak, TwoPcsJackJustFrameBlock)
{
	// The thrower's u/f+1+2 (1+3 here) on tick 40, then d/f+2 (2) on tick 60: the held fighter holds back at frames 29..30
	// of pj_lfp_yxk, or turns the stick early and is locked out.
	const world w = jack_world(true);
	pair_case p;
	p.w = &w;
	p.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}, {1, held_mask(two), 15, 65, 3, 1}};
	p.ticks = 160;
	p.lockstep_from = 140;
	p.thrower_words = words_with(200, {{40, one | three, 1}, {60, two, 1}}, 5, 160, 5);
	u32 read_ahead = 0;
	const std::vector<std::pair<std::vector<press>, int>> plays{{{{90, back, 2}}, 5}, {{{80, back, 1}}, 4}, {{{88, back, 3}}, 4}};
	for (const auto& [presses, last] : plays)
	{
		p.held_words = words_with(200, presses);
		std::vector<std::pair<u32, int>> offline;
		read_ahead += play_pairs(p, &offline);
		check(!offline.empty() && offline.back().second == last, "offline: blocked at frame 29, or locked out");
	}
	check(read_ahead, "the thrower's PC corrected the stick the held fighter turned in the window");
}

PBD_TEST(PbdThrowBreak, TwoPcsMountChain)
{
	// The mash begun in the mount reads ahead while the mount can still lead into the arm lock: both PCs agree at every
	// confirmed tick, and the reversal lands on its offline tick.
	const world w = mount_chain_world();
	std::vector<std::pair<u32, int>> offline;
	const u32 read_ahead = play_pairs(mount_chain_case(w), &offline);
	check(offline == std::vector<std::pair<u32, int>>{{41, 1}, {85, 2}, {91, 6}}, "offline: the mount, the lock, the reversal");
	check(read_ahead, "the thrower's PC corrected words the held fighter read ahead");
}

PBD_TEST(PbdThrowBreak, TwoPcsNeedReadAheadConfirmation)
{
	// Confirming as if no row read ahead, the thrower's PC keeps predicted words of the held player's that it never
	// corrects, and the two PCs part.
	const world w = mount_world();
	pair_setup s;
	s.c = base_of(w, 300);
	s.c.grab = ~0u;
	s.c.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}};
	s.c.hooks = hook::rule;
	s.c.port = 1;
	s.c.actor = side_1;
	s.delay = 3;
	s.samples[1] = words_with(340, {{140, two, 1}, {149, two, 1}, {161, two, 1}, {170, both, 1}});
	s.samples[0] = words_with(340, {{37, one | three, 1}});
	s.link = links[1];
	s.lockstep_from = 285;
	check(!play_pair(s).fault, "the pair as it runs");
	s.confirm_reads_ahead = false;
	check(says(play_pair(s).fault, "the PCs differ at a confirmed tick"), "without the words read ahead in confirmation");
}

PBD_TEST(PbdThrowBreak, LockstepRowsRunOnFinalWords)
{
	// A hold that goes on into the stretch after the fight (a throw that KOs, or one that straddles time-up): a lockstep row
	// is final once it runs, so it also waits for the word its remote fighter reads ahead. Without that wait a row runs on a
	// predicted word, which a later correction would run again, or which the segment barrier finds wrong.
	const world w = mount_world();
	pair_setup s;
	s.c = base_of(w, 300);
	s.c.grab = ~0u;
	s.c.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}};
	s.c.hooks = hook::rule;
	s.lockstep_from = 120;
	u32 parted = 0;
	for (u16 side = 0; side < 2; ++side)
		for (u32 delay = 1; delay <= 6; ++delay)
			for (u32 link = 0; link < links.size(); ++link)
			{
				s.c.port = side;
				s.c.actor = side ? side_1 : side_0;
				s.delay = delay;
				s.samples[side] = words_with(340, {}, 30, 300, 4);
				s.samples[1 - side] = words_with(340, {{40 - delay, one | three, 1}});
				s.link = links[link];
				s.max_prediction = link >= 2 ? 8 : 5;
				s.seed = (u64{side} << 40) ^ (u64{delay} << 32) ^ (u64{link} << 24);
				s.lockstep_waits_ahead = true;
				const pair_result r = play_pair(s);
				const std::string where = " (side " + std::to_string(side) + ", D " + std::to_string(delay) + ", link " + std::to_string(link) + ")";
				check(!r.fault, (r.fault ? r.fault : "") + where);
				s.lockstep_waits_ahead = false;
				parted += play_pair(s).fault != nullptr;
			}
	check(parted, "a lockstep row that runs on a predicted word ahead");
}

PBD_TEST(PbdThrowBreak, AWindowThatOpensInLockstepRunsExactlyOrRefuses)
{
	// No hold starts in the stretches that run in lockstep, but if one did, its first window step would need words no
	// boundary waited for: it runs if they have arrived and refuses if not, and never runs a prediction that cannot be
	// corrected.
	const world w = throw_world({{1, 15, 15, break_class::one}});
	pair_setup s;
	s.c = base_of(w, 120);
	s.c.grab = ~0u;
	s.c.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}};
	s.c.hooks = hook::rule;
	s.lockstep_from = 20;
	u32 ran = 0, refused = 0;
	for (u16 side = 0; side < 2; ++side)
		for (u32 delay = 1; delay <= 6; ++delay)
			for (u32 link = 0; link < links.size(); ++link)
			{
				s.c.port = side;
				s.c.actor = side ? side_1 : side_0;
				s.delay = delay;
				s.samples[side] = words_with(160, {{48, one, 1}}, 30, 70, 3);
				s.samples[1 - side] = words_with(160, {{40 - delay, one | three, 1}}, 5, 140, 6);
				s.link = links[link];
				s.seed = (u64{side} << 40) ^ (u64{delay} << 32) ^ (u64{link} << 24) ^ 7;
				const pair_result r = play_pair(s);
				if (!r.fault)
					++ran;
				else
				{
					check(says(r.fault, "throw_break_source_missing"), std::string(r.fault) + " (side " + std::to_string(side) + ", D " +
																		   std::to_string(delay) + ", link " + std::to_string(link) + ")");
					++refused;
				}
			}
	check(ran + refused == 48, "every pair ran exactly or refused");
}

PBD_TEST(PbdThrowBreak, AWordReadAheadPastTheOtherPcsExitMustArrive)
{
	// The other PC passed the segment barrier first, so its words from there count as neutral here. A row before the barrier
	// whose held fighter read ahead past it ran on a prediction, and the other PC ran its real word: that word must arrive
	// before the row counts as confirmed. Counted as neutral instead, a row can be confirmed on the wrong word and the PCs
	// part silently; waiting for it, the barrier finds the wrong word and ends the match.
	const world w = mount_world();
	pair_setup s;
	s.c = base_of(w, 200);
	s.c.grab = ~0u;
	s.c.thrower = {{0, held_mask(one | three), 1, 0xffff, 1, 1}};
	s.c.hooks = hook::rule;
	s.lockstep_from = 200;
	u32 parted = 0;
	for (const bool arrival : {true, false})
		for (u16 side = 0; side < 2; ++side)
			for (u32 delay = 1; delay <= 6; ++delay)
				for (u32 link = 0; link < links.size(); ++link)
				{
					s.read_ahead_needs_arrival = arrival;
					s.c.port = side;
					s.c.actor = side ? side_1 : side_0;
					s.delay = delay;
					s.samples[side] = words_with(260, {{197, back, 40}});
					s.samples[1 - side] = words_with(260, {{40 - delay, one | three, 1}});
					s.link = links[link];
					s.max_prediction = 8;
					s.seed = (u64{side} << 40) ^ (u64{delay} << 32) ^ (u64{link} << 24) ^ 11;
					const pair_result r = play_pair(s);
					const bool silent = says(r.fault, "the PCs differ at a confirmed tick") || says(r.fault, "the PCs read different words ahead") ||
					                    says(r.fault, "a confirmed or published row ran again");
					if (arrival)
						check(!r.fault || says(r.fault, "session_barrier_after_misprediction"),
							(r.fault ? r.fault : "") + std::string(" (side ") + std::to_string(side) + ", D " + std::to_string(delay) + ", link " +
								std::to_string(link) + ")");
					else
						parted += silent;
				}
	check(parted, "a word read ahead past the exit counted as neutral parts the PCs");
}

PBD_TEST(PbdThrowBreak, RoundSettings)
{
	lookahead_rounds rounds;
	check(!rounds.any() && rounds.at(0) == lookahead_setting{} && rounds.at(500) == lookahead_setting{}, "no record: L 0, as before the rule");
	check(rounds.add(0, {{3, 3}}) && rounds.add(90, {{2, 2}, true}) && rounds.add(200, {{3, 3}}), "the game start, then each round");
	check(rounds.any() && rounds.at(0).lookahead[0] == 3 && rounds.at(89).lookahead[1] == 3 && rounds.at(90) == lookahead_setting{{2, 2}, true} &&
			  rounds.at(199).tech_rolls && rounds.at(200) == lookahead_setting{{3, 3}},
		"each input runs with the setting of the round it is in");
	check(!rounds.add(150, {{3, 3}}) && !rounds.add(300, {{7, 3}}), "a round out of order, or an L past the largest delay");
	check(rounds.add(200, {{1, 1}}) && rounds.at(250).lookahead[0] == 1, "a second value for the same input replaces the first");
	lookahead_rounds zeros;
	check(zeros.add(0, {}) && !zeros.any(), "L 0 everywhere reads nothing ahead");
	session_state state;
	state.configure_playback(true);
	std::array<port_record, 2> row{};
	row[0].lookahead = 3;
	row[1].lookahead = 2;
	check(state.on && state.playback && state.lookahead_of(row) == std::array<u32, 2>{3, 2} && !state.developer,
		"playback follows the recording, and a shared moveset only logs there, as in the players' games");
	state.configure_playback(false);
	check(!state.on && state.lookahead_of(row) == std::array<u32, 2>{} && !state.developer, "a recording without the rule plays as stock");
	session_state peer;
	peer.configure_peer(setting::on, setting::unset, true, std::nullopt);
	check(peer.recorded() == lookahead_setting{}, "before the delay is applied nothing is recorded");
	peer.apply_delay(4);
	check(peer.recorded() == lookahead_setting{{4, 4}}, "the applied delay, both sides");
	peer.apply_delay(0);
	check(!peer.recorded().any(), "L 0 records as before the rule");
	session_state off;
	off.configure_peer(setting::off, setting::on, true, 3);
	check(!off.recorded().any(), "the rule off records nothing, whatever the tech-roll switch");
}

PBD_TEST(PbdThrowBreak, ARecordedSetPlaysBackBitIdentical)
{
	// A replay of a set with holds runs exactly as both PCs confirmed it: the game start's L, then each round's (here the
	// same again from a round that starts before the grab), reading the recorded words ahead. Every tick ends on the same
	// state with the same words read ahead, and every move lands on the same tick. Without the rule it parts.
	const world tackle = tackle_world(30), thrown = throw_world({{1, 15, 15, break_class::one}}), mount = mount_world(), jump = jump_world(),
				jack = jack_world(true), chain = mount_chain_world();
	u32 parted = 0;
	for (const pair_case& p : {tackle_case(tackle), break_case(thrown), mount_case(mount), jump_case(jump), jack_case(jack), mount_chain_case(chain)})
		for (u16 side = 0; side < 2; ++side)
			for (const u32 delay : {1u, 3u, 6u})
			{
				const recorded_set set = record_set(p, side, delay, delay % 2 ? 1 : 3);
				const pair_result& played = set.played;
				const playback replay = play_back(set.setup.c, played.words, rounds_of({{0, delay}, {20, delay}}), everything);
				const std::string where = " (side " + std::to_string(side) + ", D " + std::to_string(delay) + ")";
				check(!replay.refusal, (replay.refusal ? replay.refusal : "") + where);
				check(replay.digests == played.digests, "the replay matches the PCs at every tick" + where);
				for (u32 t = 0; t < played.rows.size(); ++t)
					check(same_use(replay.rows[t][side].use, played.rows[t][side].use), "the replay reads the same words ahead" + where);
				check(replay.commits == played.commits[0], "every move lands on its recorded tick" + where);
				parted += play_back(set.setup.c, played.words, lookahead_rounds{}, everything).digests != played.digests;
			}
	check(parted == 36, "a replay without the rule does not run the recorded game");
}

PBD_TEST(PbdThrowBreak, AnOldRecordingPlaysAsToday)
{
	// A recording from before the rule, or with it off, names no setting: the replay loads the stock word at every step and
	// runs as a replay of the same words ran before the rule.
	const world w = tackle_world(30);
	const recorded_set set = record_set(tackle_case(w), 0, 3);
	const auto& words = set.played.words;
	const playback old = play_back(set.setup.c, words, lookahead_rounds{}, everything);
	check(!old.refusal, "an old recording plays");
	scenario stock = set.setup.c;
	stock.hooks = hook::none;
	fighter f = start(stock);
	span_cache spans;
	std::vector<std::array<port_record, 2>> rows(words.size());
	const auto nothing = [](u16, u32) -> std::optional<u16>
	{
		return std::nullopt;
	};
	for (u32 t = 0; t < words.size(); ++t)
	{
		tick(stock, f, spans, rows[t], t, words[t][0], {}, nothing, words[t][1]);
		check(old.digests[t] == f.digest(), "the same state as a replay before the rule, at every tick");
		check(!old.rows[t][0].use.reads() && !old.rows[t][0].lookahead && !old.rows[t][1].lookahead, "nothing read ahead");
	}
}

PBD_TEST(PbdThrowBreak, ARoundJumpStartsOnlyWhereNothingIsTracked)
{
	// A round jump restores the round's start and plays on with nothing carried from the rows before. Where the rule tracks
	// no press, that is the recorded game; where it still tracks one it may not be, which is why the recording keeps no
	// round start there (settled_after, pbd_live_replay). A jump to a tick that reads ahead, which needs the step before it,
	// refuses.
	const world w = jump_world();
	u32 inexact = 0;
	for (u16 side = 0; side < 2; ++side)
		for (const u32 delay : {1u, 3u, 6u})
		{
			const recorded_set set = record_set(jump_case(w), side, delay);
			const auto& c = set.setup.c;
			const auto& words = set.played.words;
			const auto rounds = rounds_of({{0, delay}});
			const playback full = play_back(c, words, rounds, everything);
			check(!full.refusal && full.digests == set.played.digests, "the replay as recorded");
			const u32 last = last_read_ahead(full.rows, side);
			u32 exact = 0;
			for (u32 at = 20; at <= last + delay + 3; ++at)
			{
				const playback before = play_back(c, words, rounds, everything, nullptr, 0, at);
				check(before.kept.has_value(), "the state before the round's first tick");
				const playback jumped = play_back(c, words, rounds, everything, &*before.kept, at);
				bool settled_there = true;
				for (u32 port = 0; port < 2; ++port)
					settled_there = settled_there && settled_after(full.rows[at - 1][port], at - 1);
				const bool same = !jumped.refusal && std::equal(jumped.digests.begin() + at, jumped.digests.end(), full.digests.begin() + at);
				if (full.rows[at][side].use.reads())
				{
					check(says(jumped.refusal, "throw_break_previous_step_unknown"), "a window's step needs the step before");
					continue;
				}
				check(same || !settled_there, "a round start where nothing is tracked plays as recorded");
				exact += settled_there;
				inexact += !same;
			}
			check(exact, "round starts where nothing is tracked");
			check(!settled_after(full.rows[last][side], last), "the press the window used is tracked until its copy has passed");
		}
	check(inexact, "a round start while a press is tracked is not the recorded game");
}

PBD_TEST(PbdThrowBreak, LookaheadChangesOnlyWhereNothingIsTracked)
{
	// A set's delay may change between rounds; the replay takes each round's L where it starts, and only where the rule
	// tracks no press, as the session applies it. Anywhere else the record is not the game, and playback stops.
	const world w = jump_world();
	const recorded_set set = record_set(jump_case(w), 1, 3);
	const auto& c = set.setup.c;
	const auto& words = set.played.words;
	const playback full = play_back(c, words, rounds_of({{0, 3}}), everything);
	const u32 last = last_read_ahead(full.rows, 1);
	u32 settled_from = last;
	while (!settled(full.rows[settled_from][1]))
		++settled_from;
	check(!play_back(c, words, rounds_of({{0, 3}, {20, 2}}), everything).refusal, "a change before the hold");
	check(!play_back(c, words, rounds_of({{0, 3}, {settled_from + 2, 5}}), everything).refusal, "a change once nothing is tracked");
	check(says(play_back(c, words, rounds_of({{0, 3}, {last + 1, 2}}), everything).refusal, "throw_break_lookahead_change_pending"),
		"while the press the window used is tracked");
	lookahead_rounds tech;
	check(tech.add(0, {{3, 3}}) && tech.add(last + 1, {{3, 3}, true}), "the tech-roll switch from a later round");
	check(says(play_back(c, words, tech, everything).refusal, "throw_break_lookahead_change_pending"), "the tech-roll switch changes there too");
	// Take Control: the taken side plays without the rule from the tick it is taken, wherever that is.
	std::array<port_record, 2> row = full.rows[last + 1], before = full.rows[last];
	check(!take_recorded(row, &before, last + 1, {{3, 0}}, false) && row[1].lookahead == 0, "a taken side drops its L at once");
}

PBD_TEST(PbdThrowBreak, AWatcherJoiningMidHoldMatches)
{
	// A watcher plays the stream's frames, which arrive in packets. It joins while the players are inside the hold, catches
	// up to them, then waits at the live edge until it holds the frame L ticks past the one it runs. At every tick it ends on
	// the players' state. Running each tick as soon as its own frame is there, it would need frames not sent yet.
	const world tackle = tackle_world(30), mount = mount_world();
	u32 short_of_frames = 0;
	for (const pair_case& p : {tackle_case(tackle), mount_case(mount)})
		for (u16 side = 0; side < 2; ++side)
			for (const u32 delay : {1u, 3u, 6u})
			{
				const recorded_set set = record_set(p, side, delay);
				const auto& words = set.played.words;
				const u32 total = static_cast<u32>(words.size());
				const auto stream = rounds_of({{0, delay}});
				for (const auto& [joined, packet, every] : {std::array<u32, 3>{51, 7, 7}, {75, 1, 1}, {44, 16, 16}, {120, 120, 120}})
				{
					const auto held = watcher_frames(total, joined, packet, every, delay);
					const playback watched = play_back(set.setup.c, words, stream, [&](u32 t)
						{
							return held[t];
						});
					const std::string where = " (side " + std::to_string(side) + ", D " + std::to_string(delay) + ", joined " + std::to_string(joined) + ")";
					check(!watched.refusal, (watched.refusal ? watched.refusal : "") + where);
					check(watched.digests == set.played.digests, "the watcher matches the players at every tick" + where);
					const auto eager = watcher_frames(total, joined, packet, every, 0);
					short_of_frames += says(play_back(set.setup.c, words, stream, [&](u32 t)
												{
													return eager[t];
												})
												.refusal,
						"throw_break_source_missing");
				}
			}
	check(short_of_frames, "without holding L frames ahead a watcher runs short of frames at the live edge");
}
