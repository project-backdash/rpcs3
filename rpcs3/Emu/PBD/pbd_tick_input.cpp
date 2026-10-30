#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_practice_lesson_channel.h"
#include "pbd_practice_start.h"
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
	}
	if (data.practice.rewind && !data.peer.enabled && pbd::practice::start::holds_port(port))
	{
		raw = 0;
		row.raw[port] = 0;
	}
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
			if (const auto lesson_word = pbd::practice::lesson::input(port, allowed,
					side && *side == pbd::practice::movement::screen_side::right))
				raw = *lesson_word;
			if (const auto move_word = pbd::practice::dummy_move::input(port, allowed,
					side && *side == pbd::practice::movement::screen_side::right))
				raw = *move_word;
		}
		if (!practice_press)
			raw = pbd::practice::controls::fight_word(raw, data.session.in_match && data.practice.native.screen == 0);
		if (data.practice.menu.open && raw && !practice_press)
			++data.practice.input_leaks;
		// Loading phases consume neutral input in the same way at the injection and verification stages. A live
		// device read must not replace that decision between character select and the native Practice arena.
		if (data.session.enabled && data.neutral_at(index))
			raw = 0;
		row.raw[port] = static_cast<u16>(raw);
	}
}

void pbd::rollback_session::tick_dispatch::apply_practice_pause_input(u32 port, u32& raw, bool& practice_press)
{
	if (data.practice.native.pause_valid)
	{
		// In ordinary Practice both ports retain their device words. Recording gives the one live controller to P2 while
		// P1 is neutral; playback feeds P2 from the selected slot and leaves P1 live. Host gestures are never guest input.
		if (data.practice.dummy.enabled && port == 1 && (data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::playing || data.practice.dummy.state == dummy_state::ready || (data.practice.reactions.wakeup_control.armed && data.practice.reactions.wakeup_control.engaged) || data.practice.reactions.throw_escape_control.owns()))
			raw = row.raw[1];
		else if (data.practice.dummy.enabled && port == 0 &&
				 (data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::ready))
			raw = 0;
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
		// Input Delay Frames: P1's fight words run late; the START Practice injects below never waits.
		if (port == 0)
			raw = data.practice.input_delay.frame(static_cast<u16>(raw), data.practice.input_delay_frames,
				data.session.in_match && data.practice.native.screen == 0 && !data.practice.native.paused);
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
	}
	// Practice before its pause state can be read (its character and stage select, loading). The in-game Settings
	// menu's claim needs no test here: cellPad already hands every guest read neutral words while it holds this PC's
	// controller in Practice, so the word read above is neutral. This whole block is Practice's: a replay's fighters
	// and a watcher's two ports carry recorded input and never pass through it (pbd::settings_menu::blanks_port).
	else
		raw = practice_select_word(static_cast<u16>(raw));
}
