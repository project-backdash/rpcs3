#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_practice_lesson_channel.h"
#include "pbd_practice_start.h"
#include "pbd_custom_photo.h"
#include "pbd_dummy_move_runtime.h"

bool pbd::rollback_session::tick_dispatch::read_input_counter(bool soft_mode)
{
	u32 counter = 0;
	const bool counter_read = read_word(counter_address, counter);
	if (pc == pcs[3])
	{
		row.after_read = counter_read;
		row.after_counter = counter;
	}
	if (!counter_read)
	{
		fail("invocation_consumer_mismatch");
		return true;
	}
	if (counter != row.before_counter + (pc == pcs[3] ? 1 : 0))
	{
		// Keyread restarts the counter asynchronously, including between port stages or after this tick's increment.
		// Re-anchor as at entry during live execution and permitted re-execution; the counter is only a consistency
		// witness, whose sole guest load is its increment at 0x120740.
		const bool restart_after_increment = pc == pcs[3] && counter == 0;
		const bool late_restart = data.round_lifecycle && (!data.roundtrip.repeat_active || data.peer.enabled || data.rolling) &&
		                          pbd::session_start::input_counter_restarted(row.before_counter, counter, pc == pcs[3]);
		if (late_restart)
		{
			row.before_counter = 0;
			data.consumer_offset = -static_cast<s32>(initial_state().input_counter + index + (restart_after_increment ? 1 : 0));
			data.tick_at(index).round_reset = true;
			pbd_session_log.warning("Input-side counter restart landed inside boundary %u (stage %u); accepted (%s).", index, data.stage,
				data.correction.peer_held_index == index ? "entry held for the peer" : phase_lockstep(index) ? "lockstep phase" :
																											   "no hold");
		}
		else if (!soft_mode && !data.peer.enabled)
		{
			fail("invocation_consumer_mismatch");
			return true;
		}
		else
		{
			// Record an online counter mismatch: no game code reads this witness, and per-tick hashes enforce state agreement.
			soft_fail("invocation_consumer_mismatch");
			if (data.peer.enabled)
				pbd_session_log.warning("Input-side counter %u at boundary %u (stage %u, entry %u) did not match a restart; recorded, match continues.",
					counter, index, data.stage, row.before_counter);
		}
	}

	return false;
}

bool pbd::rollback_session::tick_dispatch::inject_input(u32 port, port_sample& sample, u32 raw)
{
	sample.input_read_seen = true;
	sample.original_r3 = ppu.gpr[3];
	read_practice_input(port, sample, raw);
	// A peer session froze the claim into its own sample (filter_local_sample), so its owned port executes that value
	// unchanged, live and in every re-execution. Only Practice, whose input no other PC executes, blanks the ports here.
	if (pbd::game_overlay::blanks_executed_port(data.game_overlay.claim, data.peer.enabled, data.practice.rewind))
	{
		raw = 0;
		row.raw[port] = 0;
		row.throw_break[port].practice_word = 0;
	}
	if (data.practice.rewind && !data.peer.enabled && pbd::practice::start::holds_port(port))
	{
		raw = 0;
		row.raw[port] = 0;
		row.throw_break[port].practice_word = 0;
	}
	// Select confirms and suppressed start/menu words cannot move either fighter or invalidate a neutral picture.
	if (data.practice.rewind && !data.peer.enabled)
		pbd::custom_photo::observe_input(raw, data.practice.view.fight_live);
	// Live controller: the device read is recorded in original_r3 and becomes the NEXT tick's input;
	// this tick injects row.raw exactly like a tape value so both peers execute identical, pre-published inputs.
	if (ppu.lr != pcs[1] || ppu.gpr[21] != mapping_base || !mapping_matches(port))
	{
		fail("latch_return_or_mapping_mismatch");
		return true;
	}
	if (data.session.enabled && data.match_at(index) && (raw & 0x0800) && !pbd::session_start::permits_skip(row.start_phase) &&
		!data.start_executed_at(index))
	{
		data.start_executed_at(index) = 1;
		data.session.start_in_battle_executed++;
		pbd_session_log.error("Start executed in battle at boundary %u, port %u, replay %u", index, port, data.roundtrip.repeat_active ? 1 : 0);
	}

	// Replace the raw word; the game's instructions still convert it and update held/rising masks and the counter.
	ppu.gpr[3] = raw;
	if (data.spectator.channel)
		data.spectator.expected[port] = static_cast<u16>(raw);
	sample.injected_r3 = raw;
	sample.injected = true;

	return false;
}

bool pbd::rollback_session::tick_dispatch::verify_input(u32 port, port_sample& sample, u32 raw)
{
	sample.post_seen = true;
	sample.raw = ppu.gpr[25];
	sample.effective = ppu.gpr[29];
	sample.held = ppu.gpr[0];
	sample.rising = ppu.gpr[9];
	sample.stored_held_read = read_word(0x10099fb4 + port * 4, sample.stored_held);
	sample.stored_rising_read = read_word(masks_address + port * 4, sample.stored_rising);
	const u32 held = mapped_mask(raw);
	const u32 rising = held & ~data.previous_held[port];
	if (sample.raw != raw || sample.effective != raw || sample.held != held || sample.rising != rising)
	{
		fail("native_raw_held_or_rising_mismatch");
		return true;
	}
	if (!sample.stored_held_read || !sample.stored_rising_read || sample.stored_held != held || sample.stored_rising != rising)
	{
		fail("stored_held_or_rising_mismatch");
		return true;
	}
	sample.posted = true;
	data.previous_held[port] = held;
	return false;
}

void pbd::rollback_session::tick_dispatch::read_practice_input(u32 port, const port_sample& sample, u32& raw)
{
	if (data.practice.rewind && !data.roundtrip.repeat_active)
	{
		// The game's own word, plus a button tapped into a ring slot written since the previous frame that this read
		// skipped (read_input_ring). The row keeps the result, so a rewind replays it unchanged.
		const auto game_word = static_cast<u16>(sample.original_r3);
		raw = read_input_ring(data.input_rings[port], port, game_word).value_or(game_word);
		// The pause or resume START Practice injects into P1's word this frame, the only START DR may read in the fight.
		bool practice_press = false;
		apply_practice_pause_input(port, raw, practice_press);
		if (!practice_press && data.practice.native.pause_valid)
		{
			const auto side = practice_side_of(port);
			const bool allowed = side && data.session.in_match && !data.practice.native.paused &&
			                     !pbd::practice::controls::claims_input(data.practice_pause_scene()) && !data.practice.restart.reset_request &&
			                     !data.practice.dummy.ignore_until_release && !data.takeover.ignore_pad_until_release &&
			                     !pbd::settings_menu::claimed() && !data.game_overlay.claim;
			const auto lesson_word = pbd::practice::lesson::input(port, allowed, side && *side == pbd::practice::movement::screen_side::right);
			if (lesson_word)
				raw = *lesson_word;
			const auto move_word = pbd::practice::dummy_move::input(port, allowed, side && *side == pbd::practice::movement::screen_side::right);
			if (move_word)
				raw = *move_word;
			if (lesson_word || move_word)
				row.throw_break[port].practice_word = static_cast<u16>(raw);
		}
		if (!practice_press)
			raw = pbd::practice::controls::fight_word(raw, data.session.in_match && data.practice.native.screen == 0);
		if (data.practice.menu.open && raw && !practice_press)
			++data.practice.input_leaks;
		// Loading phases consume neutral input in the same way at the injection and verification stages. A live
		// device read must not replace that decision between character select and the native Practice arena.
		if (data.session.enabled && data.neutral_at(index))
		{
			raw = 0;
			row.throw_break[port].practice_word = 0;
		}
		row.raw[port] = static_cast<u16>(raw);
	}
}

void pbd::rollback_session::tick_dispatch::apply_practice_pause_input(u32 port, u32& raw, bool& practice_press)
{
	if (data.practice.native.pause_valid)
	{
		// In ordinary Practice both ports retain their device words. Recording gives the one live controller to P2 while
		// P1 gets up automatically; playback feeds P2 from the selected slot and leaves P1 live. Host gestures are never guest input.
		if (data.practice.dummy.enabled && port == 1 && (data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::playing || data.practice.dummy.state == dummy_state::ready || (data.practice.reactions.wakeup_control.armed && data.practice.reactions.wakeup_control.engaged) || data.practice.reactions.throw_escape_control.owns()))
			raw = row.raw[1];
		else if (data.practice.dummy.enabled && port == 0 &&
				 (data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::ready))
			raw = row.raw[0];
		if (port == 0)
		{
			// SELECT is a modifier: while it is down nothing else on this controller reaches the game, so a SELECT
			// chord never also attacks or moves. cellPad consumes SELECT before this hook, and the game's device read
			// and the boundary's host sample are taken at different moments, so a fresh host read joins them: SELECT
			// in any of the three withholds the word.
			const u16 host_now = pbd_local_pad_word();
			const u16 host_held = static_cast<u16>(data.practice.dummy.pad | host_now);
			const bool select_down = ((host_held | raw) & session_select_bit) != 0;
			raw = data.practice.select_modifier.filter(static_cast<u16>(raw), select_down, host_held);
			if (data.practice.dummy.enabled && (data.practice.dummy.mask_ui || data.practice.dummy.ignore_until_release))
				raw = 0;
		}

		if (port == 0 && data.session.in_match && !data.practice.native.paused)
			raw = pbd::practice::controls::map_attacks(static_cast<u16>(raw), data.practice.button_map);
		// R3 itself never reaches DR: it is Remember (read from the controller itself), or, given an action, that action's
		// attacks in the fight, which map_attacks has put in its place.
		if (port == 0)
			raw &= ~u32{0x0400};
		if (pbd::practice::controls::claims_input(data.practice_pause_scene()) || data.practice.dummy.ignore_until_release ||
			data.takeover.ignore_pad_until_release || data.practice.restart.reset_request)
			raw = 0;
		// The in-game Settings menu holds this PC's controller from the game while it is open and until its buttons are
		// released, before Practice's own pause has landed too; the dummy's playback keeps feeding P2.
		if (pbd::settings_menu::blanks_port(pbd::settings_menu::claimed(), data.practice.rewind, port, data.practice.dummy.state == dummy_state::playing))
			raw = 0;
		// Input Delay Frames: P1's fight words run late; the START Practice injects below never waits. The throw break rule
		// reads ahead from the word as it was before the delay, and how long it waits (pbd_throw_break.h).
		if (port == 0)
		{
			const bool recording = data.practice.dummy.enabled &&
			                       (data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::ready);
			// P1's automated recording get-up is not a player's delayed word; it must not remain queued when control returns.
			const bool fight = data.session.in_match && data.practice.native.screen == 0 && !data.practice.native.paused && !recording;
			row.throw_break[0].practice_word = static_cast<u16>(raw);
			row.throw_break[0].practice_delay = static_cast<u8>(fight ? std::min(data.practice.input_delay_frames, pbd::practice::max_input_delay) : 0);
			raw = data.practice.input_delay.frame(static_cast<u16>(raw), data.practice.input_delay_frames, fight);
		}
		if (data.practice.pause.requested)
		{
			raw = 0;
			// Held back while a resume settles: back-to-back STARTs read as one press.
			if (port == 0 && !data.practice.pause.injected && !data.practice_resume_settling())
			{
				raw = session_start_bit;
				practice_press = true;
				data.practice.pause.injected = true;
				data.practice.pause.injected_at = index;
			}
		}
		if (port == 0 && data.practice.pause.resume_native && !data.practice.pause.resume_injected &&
			data.practice.sounds.empty() && !data.practice.native.request)
		{
			raw = session_start_bit;
			practice_press = true;
			data.practice.pause.resume_injected = true;
			data.practice.pause.resume_native = false;
			data.practice.pause.resume_watch.sent(index);
		}
		// A word that replaces the delayed word replaces the word before the delay too.
		if (port == 0 && (data.practice.pause.requested || practice_press))
			row.throw_break[0].practice_word = static_cast<u16>(raw);
	}
	// Practice before its pause state can be read (its character and stage select, loading). The in-game Settings
	// menu's claim needs no test here: cellPad already hands every guest read neutral words while it holds this PC's
	// controller in Practice, so the word read above is neutral. This whole block is Practice's: a replay's fighters
	// and a watcher's two ports carry recorded input and never pass through it (pbd::settings_menu::blanks_port).
	else
		raw = practice_select_word(port, static_cast<u16>(raw));
}

// The throw break rule at its probes, on the owner thread inside the battle update that follows a tick's input update. Only
// reads the registers (observed sites keep the compiler's cached registers after the callback); the slot, +0x6c4 and the
// direction fields are written in guest memory, each after the undo log has its old value, so a rollback restores them with
// the rest of the tick.
void pbd::rollback_session::throw_break_probe(ppu_thread& ppu, u32 pc) noexcept
{
	if (ppu.id != pbd::t5dr::main_ppu_id || !m_active.load(std::memory_order_relaxed) || !m_storage || !m_storage->throw_break.on)
		return;
	namespace rule = pbd::throw_break;
	auto& data = *m_storage;
	auto& state = data.throw_break;
	if (!state.executing)
		return;
	const u32 tick = *state.executing;
	update_sample& row = state.side_row ? data.roundtrip.repeat_row : data.sample_at(tick);
	const u32 address = static_cast<u32>(ppu.gpr[31]);
	const auto byte = [](u32 at) -> u8
	{
		u8 value = 0;
		return vm::try_access(at, &value, 1, false) ? value : u8{0};
	};
	const auto lookahead_of = state.lookahead_of(row.throw_break);
	const auto refuse = [&](const char* reason)
	{
		data.guard_failure.store(reason);
		if (data.live_stats)
			pbd::live_stats::publish_guard(reason);
		pbd_session_log.error("Throw break rule refused at tick %u (pc 0x%x, actor 0x%x): %s", tick, pc, address, reason);
		finish(guard_failed);
	};
	const auto known = rule::probe_actor(byte, address);
	if (!known)
		return;
	const auto& fighter = *known;
	auto& record = row.throw_break[fighter.port];
	const auto spans = [&](u32 move, u32 moveset)
	{
		return state.spans.get(byte, move, moveset);
	};
	const auto write = [&](u32 at, u32 value)
	{
		pbd_tick_undo_store(ppu, at, 4);
		u8 bytes[4]{static_cast<u8>(value >> 24), static_cast<u8>(value >> 16), static_cast<u8>(value >> 8), static_cast<u8>(value)};
		return vm::try_access(at, bytes, 4, true);
	};
	// The scan sees the direction fields of the word ahead while a window reads them; the stock ones go back after it. A
	// step whose word Practice blanked built them from nothing held, as offline.
	if (pc == rule::scan_entry || pc == rule::scan_exit)
	{
		const u32 fields = address + rule::actor::direction_held;
		if (pc == rule::scan_entry)
		{
			if (!record.loaded || !record.view || record.view_written || record.blanked)
				return;
			record.stock_fields = rule::read32(byte, fields);
			if (record.stock_fields != record.view_fields && !write(fields, record.view_fields))
				return refuse("throw_break_write_failed");
			record.view_written = true;
			return;
		}
		if (!record.view_written)
			return;
		if (rule::read32(byte, fields) != record.view_fields)
			return refuse("throw_break_view_changed");
		if (record.stock_fields != record.view_fields && !write(fields, record.stock_fields))
			return refuse("throw_break_write_failed");
		record.view_written = false;
		return;
	}
	if (pc == rule::latch_entry)
	{
		rule::note_step(record, fighter, static_cast<u16>(ppu.gpr[7]));
		const rule::step_registers step{static_cast<u16>(ppu.gpr[7]), static_cast<u16>(ppu.gpr[6])};
		if (const char* refusal = rule::check_latch_entry(byte, address, tick, lookahead_of, row.throw_break, state.tech_rolls, step, spans))
		{
			pbd_session_log.error("Throw break step at tick %u: held 0x%x pressed 0x%x, +0x6c4 0x%x; loaded %u, stock 0x%x, slot 0x%x, +0x6c4 "
								  "planned 0x%x (gate %u, mask 0x%x); previous step %u stock 0x%x (gate %u, known %u)",
				tick, step.held, step.pressed, rule::read32(byte, address + rule::actor::previous_held), u32{record.loaded}, record.stock,
				record.slot, record.previous_loaded, u32{record.gate_open}, record.use.mask, record.previous_step, record.previous_stock,
				u32{record.previous_gate_open}, u32{record.previous_known});
			return refuse(refusal);
		}
		if (record.loaded && record.slot != record.stock && !write(rule::slot_of(fighter.port), record.stock))
			refuse("throw_break_write_failed");
		return;
	}
	if (record.loaded && (lookahead_of[0] || lookahead_of[1]))
		return refuse("throw_break_step_repeated");
	// A mirror match may run both fighters from one loaded moveset, where only the break windows count.
	if (!state.movesets_checked && (lookahead_of[0] || lookahead_of[1]))
	{
		state.movesets_checked = true;
		if (rule::shared_moveset(byte))
		{
			pbd_session_log.warning("Throw break rule: both fighters run the moveset at 0x%x in match %u; only break windows read ahead.",
				fighter.moveset, data.session.matches);
		}
	}
	// The held mask a port's input update stores at tick `source`. A peer session's own port reads the frozen local word,
	// which the push model fixes D ticks ahead; the other port reads what has arrived as of the tick's boundary, or the
	// prediction, except on a lockstep tick, which is final once it runs and so takes only a word that has arrived (the
	// boundary waits for every possible source before admitting the tick). Practice reads the player's word before the
	// delay, at the step L ticks earlier, from the tick's row. A replay reads its recorded word, and
	// a watched game the stream's frame for that tick, which the watcher holds before it runs a tick (local_channel::reserve).
	u32 stage = pc;
	boundary_call call{*this, data, ppu, stage};
	const bool watched = data.spectator.channel && (!data.lobby.direct_match.enabled() || data.lobby.direct_match.pbd_live_inputs());
	const bool lockstep = state.peer && tick < data.history.lockstep_ticks.size() && data.history.lockstep_ticks[tick];
	const auto held_at = [&](u16 port, u32 source) -> std::optional<u16>
	{
		std::optional<u16> word;
		if (state.peer)
		{
			if (source >= data.extent())
				return std::nullopt;
			if (port == data.peer.role)
				word = data.history.live_fixed[source] ? std::optional<u16>{data.history.live_inputs[source]} : std::nullopt;
			else if (lockstep)
				word = data.history.peer_remote_arrival[source] >= 0 ? std::optional<u16>{data.history.peer_remote_value[source]} : std::nullopt;
			else
				word = call.peer_remote_for(source, state.now);
		}
		else if (state.playback)
			word = watched ? data.spectator.channel->pbd_queued_pad(source, port) : data.lobby.direct_match.pbd_recorded_word(source, port);
		else if (port == 0)
			word = rule::practice_word_at(row.throw_break[0], tick, source - lookahead_of[0]);
		return word ? std::optional<u16>{static_cast<u16>(mapped_mask(*word))} : std::nullopt;
	};
	const bool was_reading = record.previous_read;
	const rule::step_plan plan =
		rule::at_word_load(byte, address, static_cast<u32>(ppu.gpr[30]), tick, lookahead_of, row.throw_break, state.tech_rolls, spans, held_at);
	if (plan.refusal)
	{
		pbd_session_log.error("Throw break source at tick %u: port %u, move 0x%x frame %u, movesets 0x%x/0x%x, L %u, boundary %u, "
							  "lockstep %u; source %u available %u, previous step %u source %u available %u",
			tick, fighter.port, fighter.move, fighter.frame, fighter.moveset, fighter.move_moveset, plan.ahead, state.now, u32{lockstep},
			plan.use.source, u32{held_at(fighter.port, plan.use.source).has_value()}, record.previous_step, plan.use.previous_source,
			u32{held_at(fighter.port, plan.use.previous_source).has_value()});
		return refuse(plan.refusal);
	}
	rule::take_plan(record, plan);
	if (plan.slot && !write(rule::slot_of(fighter.port), *plan.slot))
		return refuse("throw_break_write_failed");
	if (plan.previous_held && !write(address + rule::actor::previous_held, *plan.previous_held))
		return refuse("throw_break_write_failed");
	// A line where a window starts reading ahead, and one for every press it moves: fired from ahead, a used copy masked, or
	// a press that came too early for its window.
	if ((plan.use.reads() && !was_reading) || plan.early || plan.masked || plan.missed)
	{
		const auto text = fmt::format("{\"type\":\"throw_break\",\"tick\":%u,\"port\":%u,\"move\":%u,\"frame\":%u,\"lookahead\":%u,\"mask\":%u,"
									  "\"source\":%u,\"word\":%u,\"stock\":%u,\"early\":%u,\"masked\":%u,\"missed\":%u,\"replayed\":%s}\n",
			tick, fighter.port, fighter.move, fighter.frame, plan.ahead, plan.use.mask, plan.use.source, plan.use.word, plan.stock, plan.early,
			plan.masked, plan.missed, json_bool(state.now != tick));
		if (data.peer.file)
			data.peer.file.write(text.data(), text.size());
		else
			pbd_session_log.notice("%s", text.substr(0, text.size() - 1));
	}
}

// A replay or a watched game plays as it was played, so it runs the rule as its recording or stream names it, whatever
// this PC's switch says. A watcher learns each game's setting as that game begins (local_channel::pbd_throw_break), so it
// keeps the rule ready for every game; with L 0 the rule writes nothing.
const char* pbd::rollback_session::configure_throw_break_playback() noexcept
{
	if (!m_storage || m_storage->peer.enabled || m_storage->practice.rewind)
		return nullptr;
	auto& s = *m_storage;
	const bool watched = s.spectator.channel && (!s.lobby.direct_match.enabled() || s.lobby.direct_match.pbd_live_inputs());
	const bool recorded = watched || s.lobby.direct_match.pbd_throw_break_recorded();
	s.throw_break.configure_playback(recorded);
	if (recorded && !watched)
		pbd_session_log.notice("Throw break rule: the replay reads ahead as its players' games did.");
	return nullptr;
}

// The live recording asks at each game and round start, on the owner thread. A game start needs only the applied delay: no
// press is tracked before a game, wherever the initializer runs. A round start reads the tick whose input update has run;
// the stretch between rounds runs in lockstep, so that tick is final.
std::optional<pbd::throw_break::lookahead_setting> pbd::rollback_session::throw_break_round_start(bool round) noexcept
{
	if (!m_storage)
		return pbd::throw_break::lookahead_setting{};
	auto& data = *m_storage;
	const auto& state = data.throw_break;
	if (state.on && state.peer && !state.applied)
		return std::nullopt;
	const auto value = state.recorded();
	if (!value.any() || !round)
		return value;
	const u32 tick = pbd::fn_hook::current_tick.load(std::memory_order_relaxed);
	if (state.executing != tick || state.side_row || !data.history_contains(tick))
		return std::nullopt;
	const auto& row = data.sample_at(tick);
	for (u32 port = 0; port < 2; ++port)
		if (!pbd::throw_break::settled_after(row.throw_break[port], tick))
			return std::nullopt;
	return value;
}
