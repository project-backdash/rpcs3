// The selected PPU executes boundary stages in order; callbacks publish rows only after their guest work completes.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_output_gain.h"
#include "../PBD/pbd_replay_standby.h"
#include "Emu/PBD/pbd_replay_carry.h"
#include "Emu/PBD/pbd_practice_match_prompt.h"

#include "../PBD/pbd_overlay_controls.h"

// keep_peer runs on every pass of the hold below and returns false once the session has ended; the call site supplies it
// with the peer transport.
replay_control_result pbd::rollback_session::boundary_call::game_overlay_controls_at(u32 index, const auto& keep_peer)
{
	return overlay_controls<std::remove_cvref_t<decltype(keep_peer)>>{*this, index, keep_peer}.run();
}

// The original Tekken 5 stages (pbd_stage_rule.h), once per emulation, at the first owner boundary after the checkpoint
// loads or a fresh boot reaches the VS select: before any stage list, rollback snapshot or session start digest. Every
// route passes here (a session, Practice, a replay, a watcher), so two PCs on one build hold the same byte. False ends
// the session.
bool pbd::rollback_session::boundary_call::open_original_stages()
{
	if (pc != pcs[0] || data.update_active || data.roundtrip.repeat_active || pbd::stage_rule::applied(data.generation))
		return true;
	if (pbd::stage_rule::apply(ppu, data.generation))
		return true;
	pbd_session_log.error("The original stages could not be made available: the game's save is not the one expected.");
	session_runtime.finish(guard_failed);
	data.end_callback();
	return false;
}

// The game's own sound options, once, at the first owner boundary after the checkpoint loads. False ends the session.
bool pbd::rollback_session::boundary_call::apply_sound_options()
{
	if (!data.sound_options.pending() || pc != pcs[0] || data.update_active || data.roundtrip.repeat_active)
		return true;
	const auto result = data.sound_options.apply_at_owner_boundary(ppu, pc,
		data.next_index == 0 && data.start_index == 0 && !data.session.start_synced);
	const bool applied = result.status == pbd::sound_options::application_status::applied;
	const auto text = fmt::format("{\"type\":\"sound_options\",\"applied\":%s,\"reason\":\"%s\",\"context_preserved\":%s,\"setters\":%u}\n",
		json_bool(applied), result.reason, json_bool(result.context_preserved), result.setters_completed);
	if (data.peer.file)
		data.peer.file.write(text.data(), text.size());
	else
		pbd_session_log.notice("Sound option result: %s", text);
	if (!applied)
	{
		pbd_session_log.error("Sound settings could not be applied safely.");
		session_runtime.finish(guard_failed);
		data.end_callback();
		return false;
	}
	pbd_session_log.notice("Sound settings applied after checkpoint load.");
	return true;
}

#include "Emu/PBD/pbd_checkpoint_compatibility.h"

void pbd::rollback_session::boundary_call::run()
{
	if (check_checkpoint_compatibility())
		return;
	if (wait_for_replay_boot() || !open_original_stages() || !apply_sound_options() || local_standby() || match_standby() ||
		spectator_standby())
		return;
	// Session alignment barrier (see session_state::segments_state::number): at the first callback of a LIVE dispatch, a scene change outside a match
	// (or into / out of one) starts a new segment; peers align its first boundary to the same index before anything of the tick is set.
	if (prepare_live_boundary())
		return;
	if (session_start_sync() || spectator_boundary())
		return;
	// Play begins at the first live boundary past the start sync (and a watcher's first frame), and the output fades in. A
	// replay's start sync is VS select, behind the launcher's loading screen: its sound waits for the recorded game to begin,
	// where the launcher shows the game, so the select music's end and the stage load's announcer are not heard over it.
	if (open_boundary_presentation())
		return;
	start_match_recording();
	if (lobby_set_start())
		return;
	if (session_segment_barrier())
		return;
	// The verification re-execution of tick N runs as a repeat row and is never published.
	const u32 index = data.roundtrip.repeat_active ? data.roundtrip.repeat_index : data.next_index;
	if (reclaim_session_history(index))
		return;
	if (claim_practice_history(index))
		return;
	// Exit gate (session_state::gate_state::enabled), decided at the boundary of the tick whose screen handler runs next.
	if (pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active)
		pbd::fn_hook::rematch_vs_init_gate.store(data.session.enabled && data.session.gate.enabled && data.peer.enabled &&
													 data.lobby.rematch.straight_to_vs() && data.session.segments.scene == 1u,
			std::memory_order_release);
	if (exit_gate(index))
		return;
	// Publish the boundary so generic function probes can attribute their events to a tick and a pass.
	if (pbd::fn_hook::requested())
		pbd::fn_hook::note_boundary(index, data.roundtrip.repeat_active);
	if (!data.history.practice_continuous && index >= data.extent())
	{
		session_runtime.finish(update_limit);
		data.end_callback();
		return;
	}
	// Reconstruct after publishing this boundary, before choosing its first input. Physical ticks remain monotone.
	if (restore_direct_round(index))
		return;
	finish_fast_playback(index);
	// The shared panel is the sole controller owner while visible. It holds this boundary, emits one host-side choice and keeps the
	// controller claimed through release, before replay or Practice can observe that same press. The replay viewer and Practice read
	// the controller through game_overlay.claim. A bounded matchmaking prompt can cover Practice's menu or recording question;
	// other launcher panels yield to them.
	// This is also the only reader of the lobby set channel (next game, post-set choices, corner chat, stream status): a lobby set
	// always has the shared panel, so its commands are polled here on every fresh boundary.
	if (hold_game_overlay(index))
		return;
	if (apply_replay_controls(index))
		return;
	if (run_practice_controls(index))
		return;
	const auto start = session_clock::now();
	// Correction repeats overwrite the unpublished provisional row in place; verification repeats use a side row.
	auto& row = data.roundtrip.repeat_active && !data.correction.active ? data.roundtrip.repeat_row : data.sample_at(index);
	if (choose_inputs(index, row, start))
		return;
	dispatch_tick(index, row, start);
}

namespace
{
	// What the player reads when the session start refuses with `guard`; nullptr for a guard that is not a start refusal.
	const char* start_refusal(std::string_view guard)
	{
		if (guard == "session_options_mismatch")
			return pbd::lobby_set::start_policy_refusal;
		if (guard == "session_setup_mismatch")
			return pbd::setup_digest::setup_refusal;
		if (guard == pbd::setup_digest::setup_unreadable_guard)
			return pbd::setup_digest::setup_unreadable_refusal;
		return nullptr;
	}
} // namespace

bool pbd::rollback_session::boundary_call::session_start_sync()
{
	if (data.session.enabled && pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active && !data.session.start_synced)
	{
		// The session's first frame (character select) is the start of segment 0 and gets the same menu sync as every later segment: the
		// frame counter becomes 0x80000000 | (seed & 0xffffff) on both peers (above any real counter, below the first match seed) and the
		// shared generator is reseeded from the session seed. Without it the first stage roulette can stop on different stages even though
		// the match seed agrees.
		data.session.start_synced = true;
		if (data.session.seed_set)
		{
			be_t<u32> counter_word = 0x80000000u | (data.session.seed & 0x00ffffffu);
			const u32 r13 = static_cast<u32>(ppu.gpr[13]);
			be_t<u32> seed_word = data.session.seed;
			u8 flag = 0;
			const bool ok = vm::try_access(pbd::t5dr::frame_counter, &counter_word, sizeof(counter_word), true) &&
			                vm::try_access(r13 - pbd::t5dr::random_tls::seed_before_r13, &seed_word, sizeof(seed_word), true) && vm::try_access(r13 + pbd::t5dr::random_tls::flag_after_r13, &flag, 1, true);
			pbd_session_log.warning("Session: start sync at boundary %u: frame counter %u, generator reseeded from %u (%s)", data.next_index,
				u32{counter_word}, data.session.seed, ok ? "ok" : "FAILED");
			if (ok)
				data.session.generator_syncs++;
			// The random-panel characters the checkpoint carries are replaced by seed-derived ones on both peers (segment 0).
			if (!sync_random_panel(0))
			{
				data.peer.fault = "character_rule_panel_sync";
				session_runtime.finish(guard_failed);
				data.in_callback.store(false);
				return true;
			}
			take_segment_digest(0);
		}
		// The players' names and ranks in the game's records from the session's first frame on (write_game_profiles): both
		// peers start here from the same checkpoint on the same boundary.
		{
			u32 scene = 0;
			read_word(pbd::t5dr::scene_address, scene);
			write_game_profiles(data.next_index, scene);
		}
		if (data.session.digests.enabled && data.peer.enabled)
		{
			// Options digest at the session start + button-map exchange (see session_state::digests_state::enabled). This is a barrier: nothing of the
			// session runs until both peers have each other's map and start digest.
			std::string listing;
			u32 other_slot = 0;
			record_start_digest(listing, other_slot);
			const u32 announce = data.next_index > 0 ? data.next_index - 1 : 0;
			if (wait_start_digest(announce))
				return true;
			peer_send(announce); // the other peer may still be waiting for this one's map
			if (!data.peer.fault && data.session.digests.peer_options[0x10000 % 8] != data.session.digests.options)
				data.peer.fault = "session_options_mismatch";
			if (!data.peer.fault)
			{
				data.session.digests.maps_installed = vm::try_access(other_slot, data.session.digests.peer_map.data(), 32, true);
				std::string map_hex;
				for (u8 b : data.session.digests.peer_map)
					fmt::append(map_hex, "%02x", b);
				pbd_session_log.warning("Session: start digests equal; the other side's button map installed at 0x%x (%s): %s", other_slot, data.session.digests.maps_installed ? "ok" : "FAILED", map_hex);
			}
			if (data.peer.fault)
			{
				if (const char* refusal = start_refusal(data.peer.fault))
				{
					pbd_session_log.error("%s", refusal);
					if (data.lobby.set)
						data.lobby.set->refuse(data.peer.fault, refusal);
				}
				pbd_session_log.error("Guard '%s' failed at the session start (local options %08x: %s; the other peer logs its own).", data.peer.fault, data.session.digests.options, listing);
				session_runtime.finish(guard_failed);
				data.end_callback();
				return true;
			}
		}
	}
	return false;
}

bool pbd::rollback_session::boundary_call::hold_game_overlay(u32 index)
{
	if (data.game_overlay.enabled && !data.update_active && !data.roundtrip.repeat_active && pc == pcs[0])
	{
		// Resend the previous boundary while held, so lost owned input can still confirm the end and the peer stays alive.
		const u32 announce = index > 0 ? index - 1 : 0;
		auto last_send = session_clock::time_point{};
		const auto keep_peer = [&]() -> bool
		{
			if (!data.peer.enabled)
				return true;
			if (session_clock::now() - last_send > std::chrono::milliseconds(10))
			{
				peer_send(announce);
				last_send = session_clock::now();
			}
			peer_poll(announce);
			if (!data.peer.fault)
				return true;
			pbd_session_log.error("Guard '%s' failed while the in-game menu held boundary %u.", data.peer.fault, index);
			session_runtime.finish(guard_failed);
			return false;
		};
		const auto result = game_overlay_controls_at(index, keep_peer);
		data.overlay_poll_ns.store(0);
		if (result == replay_control_result::stopped)
		{
			session_runtime.finish(emulation_stopped);
			data.end_callback();
			return true;
		}
	}
	return false;
}

void pbd::rollback_session::boundary_call::record_start_digest(std::string& listing, u32& other_slot)
{
	const auto digest_ranges = [&](std::initializer_list<std::pair<u32, u32>> ranges, std::string& listing) -> u32
	{
		u32 digest = 0x811c9dc5u;
		for (const auto& [address, size] : ranges)
		{
			std::array<u8, 64> bytes{};
			const bool ok = size <= bytes.size() && vm::try_access(address, bytes.data(), size, false);
			fmt::append(listing, "%s0x%x:", listing.empty() ? "" : " ", address);
			for (u32 i = 0; i < size; i++)
			{
				const u8 b = ok ? bytes[i] : 0xee;
				digest = (digest ^ b) * 0x01000193u;
				fmt::append(listing, "%02x", b);
			}
		}
		return digest;
	};
	data.session.digests.options = digest_ranges({{pbd::t5dr::game_mode_address, 4}, {0x1008ebb0, 13}, {0x1008ebd4, 1}, {0x1008ebf4, 8}, {0x1008f098, 8}, {0x1008f0a8, 12},
													 {0x1008f0dc, 4}, {0x10684964, 6}, {0x10684990, 8}, {0x10093f58, 4}, {0x10093fd0, 4}, {0x10093fc0, 2}, {0x10094038, 2}},
		listing);
	data.session.digests.options = pbd::lobby_set::start_policy_digest(data.session.digests.options, pbd::fn_hook::keyed_draws_enabled(),
		pbd::character_rule::banned(data.generation));
	data.session.digests.options_seq = 0x10000;
	const u32 own_slot = 0x100500cc + 32 * (data.peer.role ? 1u : 0u);
	other_slot = 0x100500cc + 32 * (data.peer.role ? 0u : 1u);
	data.session.digests.own_map_valid = vm::try_access(own_slot, data.session.digests.own_map.data(), 32, false);
	pbd_session_log.warning("Session: options digest %08x at the start (%s)", data.session.digests.options, listing);
}

// Hold the first boundary until both start digests arrive, polling Leave and the lobby without advancing the guest.
bool pbd::rollback_session::boundary_call::wait_start_digest(u32 announce)
{
	const auto from = session_clock::now();
	const auto pause_credit = peer_pause_credit();
	auto last_send = from;
	bool held = false;
	session_wait_modal waiting{session_wait_leave()};
	peer_send(announce);
	while (!data.peer.fault)
	{
		peer_poll(announce);
		if (data.peer.fault)
			break;
		if (data.session.digests.peer_map_valid && data.session.digests.peer_options_seq[0x10000 % 8] == 0x10000)
			break;
		if (pbd_emulation_stopping(ppu))
		{
			waiting.end();
			pbd_session_log.warning("Emulation stopped during the session start wait.");
			session_runtime.finish(emulation_stopped);
			data.end_callback();
			return true;
		}
		if (!std::exchange(held, true))
			vm::temporary_unlock(ppu);
		if (waiting.poll())
		{
			waiting.end();
			leave_session_wait(announce);
			data.end_callback();
			return true;
		}
		if (lobby_wait_end())
		{
			waiting.end();
			data.end_callback();
			return true;
		}
		if (session_clock::now() - last_send > std::chrono::milliseconds(10))
		{
			peer_send(announce);
			last_send = session_clock::now();
		}
		if (peer_wait_expired(from, 180, pause_credit))
		{
			if (!data.peer.fault)
				data.peer.fault = "session_start_timeout";
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	waiting.end();
	return false;
}
