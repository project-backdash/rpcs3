#include "stdafx.h"
#include "pbd_native_call.h"

namespace pbd::practice::native
{
	namespace
	{
		thread_local frame_observer frame_data;
		constexpr std::array<u32, 2> frame_actors{0x10090040, 0x10090910};
		thread_local pbd::practice::pbd_getup_guard getup_state;
		// Removes a fighter's guard the way the game removes one its Dummy Action turns off (0xF2928): from the live flags
		// (+0xCC), from the copy the stance update reloads them from (+0xD0, 0x33F94) and from the guard the contact check reads
		// (+0x152). The game derives +0x152 from +0xCC every frame, so the guard stays off until a move or a move property
		// brings it back.
		void remove_guard(u32 actor) noexcept
		{
			const u32 flags = word(actor + 0xcc), kept = word(actor + 0xd0), guard = halfword(actor + 0x152);
			if (flags == ~0u || kept == ~0u || guard == ~0u || !((flags | kept | guard) & pbd::practice::pbd_guard_bits))
				return;
			be_t<u32> live = pbd::practice::pbd_without_guard(flags), copy = pbd::practice::pbd_without_guard(kept);
			be_t<u16> none = static_cast<u16>(pbd::practice::pbd_without_guard(guard));
			vm::try_access(actor + 0xcc, &live, sizeof(live), true);
			vm::try_access(actor + 0xd0, &copy, sizeof(copy), true);
			vm::try_access(actor + 0x152, &none, sizeof(none), true);
		}

		bool frame_allocation(u32 actor, u32 allocation)
		{
			const u32 moveset = word(actor + 0x50);
			if (moveset < 0x10000000 || moveset > 0x7ffffd00)
				return false;
			const u32 table = word(moveset + 0x1e0), count = word(moveset + 0x1e4);
			return table >= 0x10000000 && count && count <= 8192 &&
			       u64{table} + u64{count} * 0x4c <= 0x80000000 && allocation >= table &&
			       u64{allocation} < u64{table} + u64{count} * 0x4c && (allocation - table) % 0x4c == 0;
		}
		bool frame_command(u32 actor, u32 command)
		{
			const u32 moveset = word(actor + 0x50);
			if (moveset < 0x10000000 || moveset > 0x7ffffd00)
				return false;
			for (const u32 offset : {0x1b0u, 0x1b8u})
			{
				const u32 table = word(moveset + offset), count = word(moveset + offset + 4);
				if (table >= 0x10000000 && count && count <= 65536 && u64{table} + u64{count} * 24 <= 0x80000000 &&
					command >= table && u64{command} < u64{table} + u64{count} * 24 && (command - table) % 24 == 0)
					return true;
			}
			return false;
		}
	} // namespace

	void frame_observer::observe(u32 pc, unsigned side, u32 flags, u32 first, u32 last, u32 target)
	{
		if (!pbd::practice::requested())
			return;
		static const bool trace = []
		{
			const auto quiet = pbd::env::find("PBD_PRACTICE_REWIND_QUIET");
			return !quiet || std::string_view(quiet) != "1";
		}();
		if (!trace || observation_count == observations.size())
			return;
		const u32 actor = frame_actors[side];
		if (observation_count + 1 == observations.size())
		{
			observations[observation_count++] = {word(0x1008f05c)};
			return;
		}
		observations[observation_count++] = {word(0x1008f05c), pc, side, word(actor + 0xc4),
			halfword(actor + 0x96), word(actor + 0x320), word(actor + 0x48), flags, first, last, target};
	}

	std::shared_ptr<const frame_state> frame_save() noexcept
	{
		try
		{
			return std::make_shared<frame_state>(frame_state{frame_data.tracker, frame_data.calculator, frame_data.references, frame_data.generation, frame_data.counter, frame_data.windows});
		}
		catch (...)
		{
			return {};
		}
	}
	void frame_restore(const std::shared_ptr<const frame_state>& saved) noexcept
	{
		if (!saved)
		{
			frame_reset();
			return;
		}
		frame_data.tracker = saved->tracker;
		frame_data.calculator = saved->calculator;
		frame_data.references = saved->references;
		frame_data.generation = saved->generation;
		frame_data.counter = saved->counter;
		frame_data.windows = saved->windows;
		frame_data.observation_count = 0;
	}
	void frame_feedback(std::array<pbd::practice::feedback, 2>& out) noexcept
	{
		const auto& displayed = frame_data.calculator.displayed();
		for (u32 side = 0; side < 2; ++side)
		{
			const auto& reading = frame_data.tracker.latest[side];
			const auto& reached = displayed[side];
			auto& shown = out[side];
			const bool current = reached.move != 0 && reached.move == reading.move;
			shown.startup = current && reached.startup ? reached.startup : reading.startup;
			shown.on_block = current && reached.outcome == pbd::practice::frames::contact::block ? reached.advantage : std::nullopt;
			shown.on_hit = current && reached.outcome == pbd::practice::frames::contact::hit ? reached.advantage : std::nullopt;
			shown.on_whiff = current && reached.contacted ? std::nullopt : current && reached.whiff ? reached.whiff :
			                                                                                          reading.on_whiff;
			shown.result_label = current && reached.contacted ? pbd::practice::frames::label_text(reached.label) : std::string_view{};
			shown.outcome = !current || !reached.contacted                           ? pbd::practice::contact::unknown :
			                reached.outcome == pbd::practice::frames::contact::block ? pbd::practice::contact::block :
			                reached.outcome == pbd::practice::frames::contact::hit   ? pbd::practice::contact::hit :
			                                                                           pbd::practice::contact::unknown;
			// The same readiness text as the Practice panel: the label replaces the word and both numbers stay.
			try
			{
				const auto readiness = !current          ? pbd::practice::frames::advantage_display{} :
				                       reached.contacted ? pbd::practice::frames::advantage_text(reached, shown.result_label) :
				                                           pbd::practice::frames::whiff_text(reached);
				shown.advantage_text = readiness.text;
				shown.advantage_marked_at = readiness.marked_at;
				shown.advantage_marked_size = readiness.marked_size;
			}
			catch (...)
			{
				shown.advantage_text.clear();
				shown.advantage_marked_at = shown.advantage_marked_size = 0;
			}
		}
	}
	void frame_reset() noexcept
	{
		pbd::practice::dummy_move::reset_playback();
		frame_data.tracker.reset();
		frame_data.calculator.reset();
		frame_data.windows = {};
		frame_data.references = {};
		frame_data.counter.reset();
		frame_data.observation_count = 0;
	}
	void frame_boundary(u32 counter, bool valid) noexcept
	{
		const u64 generation = static_cast<u64>(Emu.GetEmulationIdentifier());
		if (!valid || generation != frame_data.generation || (frame_data.counter && counter != *frame_data.counter && counter != *frame_data.counter + 1))
		{
			frame_reset();
			frame_data.generation = generation;
		}
		if (valid)
		{
			for (unsigned side = 0; side < frame_actors.size(); ++side)
				frame_data.calculator.check_queue(side, word(frame_actors[side] + 0xc4), word(frame_actors[side] + 0x320));
			frame_data.tracker.boundary(counter);
			frame_data.calculator.boundary(counter);
			frame_data.counter = counter;
		}
	}
	const std::array<pbd::practice::frames::result, 2>& frame_results() noexcept
	{
		return frame_data.tracker.latest;
	}
	const std::array<pbd::practice::frames::calculated, 2>& frame_calculated() noexcept
	{
		return frame_data.calculator.displayed();
	}
	const std::array<pbd::practice::reference::result, 2>& frame_references() noexcept
	{
		return frame_data.references;
	}
	std::optional<wakeup_view> read_wakeup() noexcept
	{
		if (!supported() || !pbd::practice::requested() ||
			!pbd::practice::practice_fight(word(pbd::t5dr::game_mode_address), word(pbd::t5dr::scene_address), word(0x10093230)) ||
			word(frame_actors[0] + 0x5c) != frame_actors[1] || word(frame_actors[1] + 0x5c) != frame_actors[0])
			return {};
		const u32 actor = frame_actors[1], allocation = word(actor + 0xc4), bank = word(actor + 0x50);
		// Incoming reactions can use the opponent's loaded move bank. Resting aliases still belong to the dummy.
		if (!frame_allocation(actor, allocation) && !frame_allocation(frame_actors[0], allocation))
			return {};
		wakeup_view out;
		out.allocation = allocation;
		out.counter = word(0x1008f05c);
		if (out.counter == ~0u)
			return {};
		const u32 table = word(bank + 0x1e0), count = word(bank + 0x1e4);
		const auto alias = [&](u32 id)
		{
			const u32 index = word(bank + 0xac + id * 4);
			return index < count && allocation == table + index * 0x4c;
		};
		const u32 flags = word(actor + 0xcc);
		if (flags == ~0u)
			return {};
		out.knocked_down = (flags & 0x6000) == 0x6000;
		out.standing = !(flags & 4) && word(actor + 0x48) == 0 && word(actor + 0x320) == 0 && (alias(1) || alias(2));
		// The stock move-flag bit used by the get-up input handler is admitted only on its lying-pose aliases.
		// Training's byte +1b1 is a forced-input property, not a ground-position reading.
		if ((flags & 4) && halfword(actor + 0x146) == 1)
		{
			for (const u32 id : {4u, 5u, 6u, 8u, 9u, 11u, 12u, 13u, 14u})
				if (alias(id))
				{
					out.on_floor = true;
					out.actionable = halfword(actor + 0x96) > 0 && halfword(actor + 0x96) <= halfword(allocation + 0x24);
					out.position = id == 4 ? pbd::practice::pbd_ground_position::face_up_feet_toward :
					               id < 8  ? pbd::practice::pbd_ground_position::face_up_feet_away :
					                         pbd::practice::pbd_ground_position::face_down_feet_toward;
					break;
				}
		}
		return out;
	}
	std::optional<pbd::practice::throw_escape_view> read_throw_escape() noexcept
	{
		if (!supported() || !pbd::practice::requested() ||
			!pbd::practice::practice_fight(word(pbd::t5dr::game_mode_address), word(pbd::t5dr::scene_address), word(0x10093230)) ||
			word(frame_actors[0] + 0x5c) != frame_actors[1] || word(frame_actors[1] + 0x5c) != frame_actors[0])
			return {};
		const u32 actor = frame_actors[1], allocation = word(actor + 0xc4);
		// The thrown fighter's half of a throw runs from the thrower's loaded move bank.
		if (!frame_allocation(actor, allocation) && !frame_allocation(frame_actors[0], allocation))
			return {};
		// +0x192: the break the open escape command asks for; +0x204: the break input taken (pbd_practice_throw_escape.h).
		const u32 wanted = byte(actor + 0x192), given = byte(actor + 0x204);
		if (wanted < 1 || wanted > 3 || given == ~0u)
			return {};
		return pbd::practice::throw_escape_view{allocation, wanted, given};
	}
	void getup_guard(u32 chosen_action) noexcept
	{
		const auto now = read_wakeup();
		const bool down = now && (now->knocked_down || now->on_floor), standing = !now || now->standing;
		// The guard comes with each move the get-up enters; the commit hook in frame_event removes it there. This boundary
		// removes what a move property brought during the last frame, and the stance the get-up's last frame entered.
		if (getup_state.boundary(down, standing, chosen_action, static_cast<u64>(Emu.GetEmulationIdentifier()), now ? now->counter : 0) && now)
			remove_guard(frame_actors[1]);
	}
	std::span<const pbd::practice::frames::event> frame_events() noexcept
	{
		return {frame_data.observations.data(), frame_data.observation_count};
	}
	void frame_events_clear() noexcept
	{
		frame_data.observation_count = 0;
	}
	struct frame_step
	{
		ppu_thread& ppu;
		u32 address, counter, actor;
		unsigned side;
		u32 move = 0, opponent = 0, cursor = 0, direction = 0;
		void contact()
		{
			// The later application call, not the preceding classification call, counts the contact once.
			if (ppu.lr != 0x18490)
				return;
			const u32 record = static_cast<u32>(ppu.gpr[4]);
			if (record != actor + 0x530 && record != actor + 0x580)
				return;
			if (word(record) != frame_actors[1 - side])
				return;
			const u32 flags = static_cast<u32>(ppu.gpr[9]);
			frame_data.observe(address, side, flags);
			const auto kind = word(actor + 0x48) != 0            ? pbd::practice::frames::contact::unavailable :
			                  (flags & 0xf0000000) == 0xc0000000 ? pbd::practice::frames::contact::block :
			                  (flags & 0xf0000000) == 0x90000000 ? pbd::practice::frames::contact::hit :
			                                                       pbd::practice::frames::contact::unavailable;
			const u32 reaction = word(actor + 0x320);
			if (kind != pbd::practice::frames::contact::unavailable)
				pbd::practice::dummy_move::applied_contact(counter, side, kind == pbd::practice::frames::contact::block);
			// Block and hit reactions usually come from the attacker's loaded move bank, not the defender's own.
			const u32 attacker = frame_actors[1 - side];
			const bool known = frame_allocation(actor, reaction) || frame_allocation(attacker, reaction);
			frame_data.tracker.applied_contact(1 - side, known ? reaction : 0, kind);
			// What the contact led to, from the move data: the attacker's move, the move the defender was in and the
			// reaction the game queued for it (this fighter's reaction runs from whichever bank holds it).
			pbd::practice::frames::contact_reading reading;
			if (const u32 entry = word(attacker + 0xc4); frame_allocation(attacker, entry))
				reading.attacker_level = word(entry + 0x10);
			if (const u32 entry = word(actor + 0xc4); frame_allocation(actor, entry) || frame_allocation(attacker, entry))
				reading.defender_flags = word(entry + 0x0c);
			if (known)
			{
				reading.reaction_flags = word(reaction + 0x0c);
				reading.reaction_landing = halfword(reaction + 0x28);
				reading.reaction_landed = halfword(reaction + 0x2a);
				const u32 bank = word((frame_allocation(actor, reaction) ? actor : attacker) + 0x50);
				reading.reaction_plain = pbd::move_list::actionable(bank, reaction, word, halfword).has_value();
			}
			// The level this frame's contact check (0x1822C, in the same call as this application) blocked or let through: the
			// attacker's +0x150, which a move property can change part-way through the move.
			frame_data.calculator.applied_contact(1 - side, kind, counter, pbd::practice::frames::classify(kind, reading), halfword(attacker + 0x150));
			auto& observation = frame_data.tracker.latest[1 - side];
			observation.contact_flags = flags;
			observation.pending_reaction = reaction;
			observation.defender_state = word(actor + 0x48);
			// The attacker's move read again now that the contact kind is known: recovery that branches on hit, block or
			// whiff counts (move_list::readiness), read on from the cursor the contact frame reached.
			if (const u32 striker = frame_actors[1 - side], current = word(striker + 0xc4);
				kind != pbd::practice::frames::contact::unavailable && (frame_allocation(striker, current) || frame_allocation(actor, current)))
			{
				frame_data.calculator.reread(1 - side, current, pbd::move_list::readiness(word((frame_allocation(striker, current) ? striker : actor) + 0x50), current, halfword(striker + 0x96), kind, word, halfword));
			}
			return;
		}

		void preview()
		{
			// The resolver's return after every queue field has been prepared: +320 is the target, +30E its execution
			// cursor and +310 the adjusted entry parameter. Inspect the accepted queue; never run input predicates.
			const u32 target = word(actor + 0x320), length = halfword(move + 0x24);
			const auto& prior = frame_data.calculator.latest[side];
			const auto refuse = [&]
			{
				frame_data.calculator.clear_preview(side);
			};
			if (!prior.move || !prior.ready || word(address) != 0x38000080 || direction != 1 || !cursor || cursor > length || length > 32767 ||
				word(actor + 0x48) != 0 || (!frame_allocation(actor, target) && !frame_allocation(opponent, target)) ||
				pbd::practice::frames::attack_window(halfword(target + 0x44), halfword(target + 0x46), halfword(target + 0x24)) ||
				pbd::practice::frames::throw_hold(word(target + 0x10)))
			{
				refuse();
				return;
			}
			const u32 execute = (std::clamp)(halfword(actor + 0x30e), cursor, length);
			if (u64{counter} + execute - cursor > 0xffffffffu)
			{
				refuse();
				return;
			}
			if (static_cast<s64>(u64{counter} + execute - cursor) >= *prior.ready)
			{
				refuse();
				return;
			}
			const u32 bank = word((frame_allocation(actor, target) ? actor : opponent) + 0x50);
			const auto queued = pbd::move_list::queued_readiness(bank, target, execute, halfword(actor + 0x310),
				prior.outcome, word, halfword);
			if (!queued)
			{
				refuse();
				return;
			}
			frame_data.observe(address, side, 0, execute, queued->cursor, target);
			frame_data.calculator.preview_cancel(side, move, target, counter, counter + execute - cursor, queued->cursor, queued->readiness);
			return;
		}

		void recovery()
		{
			// The resolver has selected the actual terminal even before it admits buffered attacks. This observation
			// lets the dummy enter a motion before recovery ends, without calling a mutating command predicate. Every
			// reaction after a hit or block uses it, a movement or a recorded Dummy Action as much as a move: it gives the
			// first frame the dummy can act before that frame comes, while the dummy shows its standing move again only
			// once the end command's window is over.
			if (!pbd::practice::dummy_move::current.live || word(address) != 0xa15f0096 ||
				direction != 1 || cursor > 32767 || word(actor + 0x48) != 0)
				return;
			const u32 command = static_cast<u32>(ppu.gpr[30]);
			if ((!frame_command(actor, command) && !frame_command(opponent, command)) || halfword(command) != 0x8000 || halfword(command + 8) != 0x8001)
				return;
			const u32 first = halfword(command + 0x10), last = halfword(command + 0x12);
			if (first && first <= last && first <= 32767)
				pbd::practice::dummy_move::recovery(counter, side, cursor, first);
			return;
		}

		void terminal()
		{
			const u32 command = static_cast<u32>(ppu.gpr[30]);
			if ((!frame_command(actor, command) && !frame_command(opponent, command)) || halfword(command) != 0x8000)
			{
				frame_reset();
				return;
			}
			const u32 first = halfword(command + 0x10), last = halfword(command + 0x12);
			if (first > last || direction != 1 || cursor > 32767)
				return;
			frame_data.observe(address, side, 0, first, last, halfword(command + 8));
			// Only the standing neutral terminal is admitted. Floor and other terminal states need different action rules.
			if (halfword(command + 8) != 0x8001 || word(actor + 0x48) != 0)
			{
				frame_data.windows[side] = {};
				frame_data.tracker.reject_recovery(side, move);
				return;
			}
			frame_data.windows[side] = {move, first, last};
			pbd::practice::dummy_move::recovery(counter, side, cursor, first);
			// Earlier calls buffer an attack for the terminal frame; they do not end recovery yet.
			if (cursor >= first && cursor <= last)
				frame_data.tracker.actionable(side, move, cursor);
		}

		void transition()
		{
			frame_data.observe(address, side);
			// The stock transition has passed its gates and is about to replace the old animation cursor with 1.
			const auto previous = frame_data.windows[side];
			if (previous.move == move && direction == 1 && cursor >= previous.start && cursor <= previous.end &&
				word(actor + 0x48) == 0)
				frame_data.tracker.actionable(side, move, cursor);
		}

		void commit()
		{
			if (move != static_cast<u32>(ppu.gpr[7]))
			{
				frame_reset();
				return;
			}
			frame_data.observe(address, side);
			frame_data.windows[side] = {};
			const u32 hit_level = word(move + 0x10);
			if (hit_level == ~0u)
			{
				frame_reset();
				return;
			}
			// An attack has an attack window; jumps, dashes, sways, rolls and plain crouches carry a hit level too. The
			// thrower's half of a connected throw has none but is read from its level, and starts its own result like any other attack.
			const bool attack = pbd::practice::frames::attack_window(halfword(move + 0x44), halfword(move + 0x46), halfword(move + 0x24)) ||
			                    pbd::practice::frames::throw_hold(hit_level);
			if (attack)
			{
				const u32 bank = word(actor + 0x50);
				frame_data.references[side] = cursor == 1 && direction == 1 ?
				                                  pbd::move_list::read_timing(bank, move, word, halfword) :
				                                  pbd::practice::reference::result{};
			}
			// Reactions run from the attacker's bank, so the bank holding the move gives its timing whichever fighter runs it.
			// A reaction is read as one (a stun only for it, its own inputs not counted).
			const u32 move_bank = word((frame_allocation(actor, move) ? actor : opponent) + 0x50);
			const auto first_active = cursor == 1 ? pbd::practice::frames::startup(halfword(move + 0x44), halfword(move + 0x46), halfword(move + 0x24), attack) : std::nullopt;
			frame_data.calculator.commit_reading(side, move, counter, cursor,
				pbd::move_list::readiness(move_bank, move, cursor, pbd::practice::frames::contact::unavailable, word, halfword,
					frame_data.calculator.reacting(side, counter)),
				attack,
				attack && halfword(move + 0x46) ? std::optional<u32>(halfword(move + 0x46)) : std::nullopt, first_active,
				(word(move + 0x0c) & pbd::practice::frames::wall_flag) != 0);
			if (pbd::practice::frames::throw_hold(hit_level))
				frame_data.calculator.throw_connected(side);
			pbd::practice::dummy_move::committed(side, counter, move);
			frame_data.tracker.commit(side, move, cursor == 1 ? pbd::practice::frames::startup(halfword(move + 0x44), halfword(move + 0x46), halfword(move + 0x24), attack) : std::nullopt,
				attack, attack && frame_data.references[side].recovery ? std::optional<u32>(halfword(move + 0x46)) : std::nullopt);
		}

		void observe_move()
		{
			move = word(actor + 0xc4);
			// A block or hit reaction runs the attacker's move, so its allocation may belong to either fighter's bank.
			opponent = frame_actors[1 - side];
			if (!frame_allocation(actor, move) && !frame_allocation(opponent, move))
			{
				frame_reset();
				return;
			}
			cursor = halfword(actor + 0x96);
			direction = halfword(actor + 0x146);

			if (address == 0xf0600)
			{
				preview();
				return;
			}
			if (address == 0xf0764)
			{
				recovery();
				return;
			}
			if (address == 0xf07a4)
				terminal();
			else if (address == 0xf2c84)
				transition();
			else
				commit();
		}
	};

	void frame_event(ppu_thread& ppu, u32 address) noexcept
	{
		if ((address != 0xf0600 && address != 0xf26f4 && address != 0xf2c84 && address != 0xf0764 && address != 0xf07a4 && address != 0x16d9c) || (!pbd::practice::requested() && !pbd::replay_viewer::requested()) ||
			ppu.id != pbd::t5dr::main_ppu_id || get_current_cpu_thread() != &ppu || !supported() || in_native_call())
			return;
		try
		{
			const u32 mode = word(pbd::t5dr::game_mode_address), scene = word(pbd::t5dr::scene_address), phase = word(0x10093230);
			const bool practice = pbd::practice::practice_fight(mode, scene, phase);
			if (!practice && !(pbd::replay_viewer::requested() && pbd::replay_viewer::combat_fight(mode, scene, phase)))
				return;
			if (word(frame_actors[0] + 0x5c) != frame_actors[1] || word(frame_actors[1] + 0x5c) != frame_actors[0] ||
				word(0xf26f4) != 0xb01f0158 || word(0xf2c84) != 0x38c00001 || word(0xf07a4) != 0x38000001 || word(0x16d9c) != 0x792917e2)
			{
				frame_reset();
				return;
			}
			const u32 counter = word(0x1008f05c);
			if (counter == ~0u)
			{
				frame_reset();
				return;
			}
			frame_boundary(counter, true);
			const u32 actor = static_cast<u32>(ppu.gpr[address == 0x16d9c ? 3 : 31]);
			if (actor != frame_actors[0] && actor != frame_actors[1])
				return;
			const unsigned side = actor == frame_actors[0] ? 0 : 1;
			// The commit of a move change, after the game has given the dummy the new move's guard (0xF1D50) and before this
			// frame's contact check (0x1822C): removed here, the guard a get-up move brings blocks on none of its frames
			// (getup_guard).
			if (address == 0xf26f4 && side == 1 && practice && getup_state.entered(static_cast<u64>(Emu.GetEmulationIdentifier()), counter))
				remove_guard(actor);
			frame_step step{ppu, address, counter, actor, side};
			if (address == 0x16d9c)
				step.contact();
			else
				step.observe_move();
		}
		catch (...)
		{
			frame_reset();
		}
	}
	bool start_models_match(const std::array<u32, 2>& rows) noexcept
	{
		if (!supported() || word(0x10091ab0) != frame_actors[0] || word(0x10091ab4) != frame_actors[1])
			return false;
		for (u32 side = 0; side < 2; ++side)
		{
			const u32 actor = frame_actors[side];
			if (rows[side] >= 85 || halfword(actor + 0x42) != rows[side] || word(actor + 0x5c) != frame_actors[1 - side] ||
				!frame_allocation(actor, word(actor + 0xc4)))
				return false;
		}
		return true;
	}

} // namespace pbd::practice::native
