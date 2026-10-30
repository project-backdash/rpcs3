#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_checkpoint_compatibility.h"
#include "pbd_character_rule_native.h"
#include "pbd_replay_carry.h"
#include "pbd_output_gain.h"
#include "pbd_held_presentation.h"
#include "pbd_replay_standby.h"

bool pbd::rollback_session::boundary_call::check_checkpoint_compatibility()
{
	// The key comes from the launcher at start, or from a match return before its reboot (pbd_match_return.h); guest
	// callbacks cannot change it. It is read once per boot: bit 0 holds the answer, the rest the boot it was read for.
	static std::atomic<u64> checked{~u64{0}};
	constexpr u64 boot_bits = ~u64{0} >> 1;
	const u64 boot = static_cast<u64>(Emu.GetEmulationIdentifier()) & boot_bits;
	u64 known = checked.load(std::memory_order_acquire);
	if (known >> 1 != boot)
	{
		known = (boot << 1) | static_cast<u64>(pbd::checkpoint_compatibility::accepts(pbd::env::find("PBD_CHECKPOINT_COMPATIBILITY_KEY")));
		checked.store(known, std::memory_order_release);
	}
	if (!(known & 1))
	{
		pbd_session_log.error("Checkpoint compatibility changed; run setup again.");
		session_runtime.finish(guard_failed);
		data.end_callback();
		return true;
	}

	return false;
}

bool pbd::rollback_session::boundary_call::prepare_live_boundary()
{
	if (data.inspect_saved_boss && pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active && data.next_index == data.inspection_index)
	{
		// Preserve a genuinely played saved-fighter entry. This explicit diagnostic never edits RAM or the save,
		// and never marks its checkpoint as the launcher's canonical first-run fixture.
		pbd::replay_carry::vs_match_history history;
		u32 mode = 0;
		bool valid = data.session.enabled && data.peer.enabled && !data.lobby.set && !pbd::character_rule::banned(data.generation) &&
		             data.session.matches > 0 && read_word(pbd::t5dr::game_mode_address, mode) && mode == 4;
		u32 scene = 0, subscene = 0;
		valid = valid && read_word(pbd::t5dr::scene_address, scene) && scene == 3 &&
		        read_word(pbd::t5dr::subscene_address, subscene) && subscene == 44 &&
		        pbd::replay_carry::read_vs_match_history(ppu, data.generation, history) && history.completed_games > 0;
		for (u32 side = 0; side < 2; ++side)
		{
			u32 row = 0, cursor = 0, state = 0, last = 0;
			valid = valid && read_word(0x10093f50 + side * 4, row) && row >= 81 && row <= 83 &&
			        read_word(0x10093f58 + side * 0x78, cursor) && cursor == 35 &&
			        read_word(0x10093f78 + side * 0x78, state) && (state >> 24) == 3 &&
			        read_word(0x10684990 + side * 4, last);
			pbd_session_log.notice("Played saved-fighter probe: side=%u previousRow=%u cursor=%u state=%u savedLast=%u completedGames=%u verified=%u",
				side, row, cursor, state >> 24, last, history.completed_games, valid ? 1u : 0u);
		}
		if (!valid)
		{
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
		session_runtime.finish(user_stopped);
		Emu.CallFromMainThread([]
			{
				Emu.PBDInspectCheckpoint();
			});
		data.in_callback.store(false);
		return true;
	}
	if (data.session.enabled && pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active && !data.lobby.direct_match.enabled() &&
		!pbd::character_rule::normalize(ppu, data.generation))
	{
		data.peer.fault = "character_rule_unverified";
		session_runtime.finish(guard_failed);
		data.in_callback.store(false);
		return true;
	}
	if (data.session.enabled && pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active && !data.session.start_synced &&
		data.lobby.direct_match.enabled())
	{
		// The file supplies the match context. Keep menu alignment and its seed sync out of direct entry.
		// Start resources only after the ordinary entry checks have observed the clean startup below.
		data.session.start_synced = true;
	}

	return false;
}

bool pbd::rollback_session::boundary_call::open_boundary_presentation()
{
	if (pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active &&
		(!data.lobby.direct_match.enabled() || data.lobby.direct_match.begun()))
	{
		pbd::held_presentation::output().play(data.next_index);
		pbd::output_gain::output().open(pbd::output_gain::fade_in_ms);
		if (!pbd::replay_standby::pbd_publish_presentation())
		{
			session_runtime.finish(guard_failed);
			data.end_callback();
			return true;
		}
	}
	return false;
}

void pbd::rollback_session::boundary_call::start_match_recording()
{
	if (data.match_replay.requested && pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active && !data.match_replay.attempted)
	{
		data.match_replay.attempted = true;
		std::array<be_t<u32>, 2> scene{};
		be_t<u8> rounds{};
		u32 counter = 0;
		const bool clean_scene = vm::try_access(pbd::t5dr::scene_address, scene.data(), sizeof(scene), false) &&
		                         scene[0] == 3u && scene[1] == 44u;
		bool phase_code = true;
		for (const auto& [address, expected] : {std::pair<u32, u32>{0x413a4, 0x81228860}, {0x413a8, 0x80690004},
				 {0x413e4, 0x90690004}, {0x443c4, 0x4bffcfd1}, {0x44790, 0x38600005}, {0x448d8, 0x38600006}})
		{
			u32 word = 0;
			phase_code = read_word(address, word) && word == expected && phase_code;
		}
		if (data.match_replay.setup_error)
			replay_refuse(data.next_index, data.match_replay.setup_error);
		else if (!data.peer.file || data.next_index != data.start_index || data.session.matches || data.practice.rewind || data.replay.taken ||
				 !clean_scene || Emu.GetTitleID() != pbd::t5dr::title_id || !phase_code)
			replay_refuse(data.next_index, "fresh_character_select_required");
		else if (!replay_clean_checkpoint(data.match_replay.clean_state, data.match_replay.state_sha256))
			replay_refuse(data.next_index, "checkpoint_identity_mismatch");
		else if (!read_word(0x1008f07c, counter) || counter != (0x80000000u | (data.session.seed & 0x00ffffffu)) ||
				 !data.session.generator_syncs || !data.session.random_panel_syncs ||
				 !vm::try_access(0x1008ebd4, &rounds, sizeof(rounds), false) || u8{rounds} < 1 || u8{rounds} > 5)
			replay_refuse(data.next_index, "clean_seed_or_rules_unverified");
		else
		{
			data.match_replay.next = data.next_index;
			replay_events(data.match_replay.lifecycle.begin(1, data.next_index, counter, u8{rounds}, true));
		}
	}
}

bool pbd::rollback_session::boundary_call::reclaim_session_history(u32 index)
{
	if (data.history.session_history_rolling && pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active)
	{
		const auto deadline = session_clock::now() + std::chrono::milliseconds(500);
		bool unlocked = false;
		while (!pbd::session_history::buffer<u8, max_updates>::reclaimable(index + 64, data.history_retired()))
		{
			if (!unlocked)
			{
				vm::temporary_unlock(ppu);
				unlocked = true;
			}
			if (pbd_emulation_stopping(ppu) || session_clock::now() >= deadline)
			{
				pbd_session_log.error("Session history could not retire boundary %u before reuse.", index);
				session_runtime.finish(output_failed);
				data.in_callback.store(false);
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	return false;
}

bool pbd::rollback_session::boundary_call::claim_practice_history(u32 index)
{
	if (data.history.practice_continuous)
	{
		const auto refuse_history = [&](const char* reason)
		{
			pbd_session_log.error("Practice history refused at boundary %u: %s", index, reason);
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
		};
		if (index >= 0x7fff0000u - std::min(initial_state().input_counter, 0x7fff0000u))
		{
			refuse_history("counter_safety_ceiling");
			return true;
		}
		if (!data.history_contains(index))
		{
			if (pc != pcs[0] || data.update_active || data.roundtrip.repeat_active)
			{
				refuse_history("stale_or_unclaimed_boundary");
				return true;
			}
			const auto deadline = session_clock::now() + std::chrono::milliseconds(500);
			bool unlocked = false;
			while (!data.history.practice_window.reclaimable(index, data.written_through.load(std::memory_order_acquire)))
			{
				if (!unlocked)
				{
					vm::temporary_unlock(ppu);
					unlocked = true;
				}
				if (pbd_emulation_stopping(ppu) || session_clock::now() >= deadline)
				{
					refuse_history("writer_retirement_timeout");
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			data.sample_at(index) = {};
			data.tick_at(index) = {};
			data.match_at(index) = 0;
			data.neutral_at(index) = 0;
			data.start_executed_at(index) = 0;
			if (!data.history.practice_window.claim(index, data.written_through.load(std::memory_order_acquire)))
			{
				refuse_history("writer_retirement_changed");
				return true;
			}
		}
	}
	return false;
}

bool pbd::rollback_session::boundary_call::restore_direct_round(u32 index)
{
	if (pc == pcs[0] && !data.update_active && !data.roundtrip.repeat_active && data.lobby.direct_match.enabled())
	{
		bool reconstructed = false;
		if (!data.lobby.direct_match.round_boundary(ppu, index, &reconstructed))
		{
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
		if (reconstructed)
		{
			data.replay.round_reconstructed = true;
			data.last_fight_index = ~0u;
			data.replay.was_fighting = false;
			// The stock initializer owns guest held state. Re-anchor only the host check of the next rising edge.
			std::array<u32, 2> native_held{};
			if (!read_word(0x10099fb4, native_held[0]) || !read_word(0x10099fb8, native_held[1]))
			{
				pbd_session_log.error("Round reconstruction refused: native_round_held_readback at boundary %u", index);
				session_runtime.finish(guard_failed);
				data.in_callback.store(false);
				return true;
			}
			data.previous_held = native_held;
		}
	}
	return false;
}

void pbd::rollback_session::boundary_call::finish_fast_playback(u32 index)
{
	// Watching from a moment: the fast forward ends at that frame of that match.
	if (data.replay.fast.active && data.replay.fast.to_match && !data.update_active && !data.roundtrip.repeat_active && pc == pcs[0] &&
		pbd::replay_viewer::seek_finished(data.replay.fast.to_match, data.replay.fast.to_frame, data.session.matches, data.session.in_match, match_frame(index)))
		end_replay_fast(data, index);
	if (data.replay.fast.active && !data.update_active && !data.roundtrip.repeat_active && pc == pcs[0] &&
		pbd::replay_viewer::round_forward_finished(data.takeover.next_match, data.takeover.next_round,
			data.session.matches, data.session.in_match, data.replay.round, data.replay.was_fighting))
		end_replay_fast(data, index);
}

bool pbd::rollback_session::boundary_call::run_practice_controls(u32 index)
{
	if (data.practice.rewind && !data.update_active && !data.roundtrip.repeat_active && pc == pcs[0])
	{
		const auto result = practice_controls_at(index);
		if (result == replay_control_result::restored)
		{
			data.takeover.redispatch = false;
			data.end_callback();
			return true;
		}
		if (result == replay_control_result::stopped)
		{
			session_runtime.finish(emulation_stopped);
			data.end_callback();
			return true;
		}
		// After the controls, so a restart or placement made at this boundary is what the sample sees.
		practice_read_sides();
		practice_sides_follow(index);
		practice_read_moves(index);
		practice_watch_contact(index);
		practice_offense_begin(index);
	}
	return false;
}

void pbd::rollback_session::boundary_call::dispatch_tick(u32 index, update_sample& row, session_clock::time_point start)
{
	pbd::stall_metrics::scope measure_dispatch("tick_dispatch", pc, data.stage, true, 200'000);
	row.stage = data.stage;
	row.last_pc = pc;
	tick_dispatch dispatch{*this, index, row};
	// This exit is outside handle(), so it does not reach the stamp and clear at the end of run(). It must do both itself,
	// like every other exit before handle(): the writer's drain gate emits the end rows and returns only once in_callback is
	// false, and operator= joins the writer at emulation stop. With the flag left set (a run without session fail-stop, whose
	// park scope would clear it), the end row is never written and the stop blocks forever in that join. The stamp comes
	// first because the freeze probe reads in_callback before last_callback_ns.
	if (pc == pcs[0] && !pbd::fn_hook::voice_boundary_ok())
	{
		dispatch.fail("voice_lease_imbalance");
		data.end_callback();
		return;
	}

	// Round-transition mode (ordinary runs only): guards that describe the one-round battle contract are RECORDED
	// here and the dispatch continues with injection, so a round end, its cut-scenes and the next round start are
	// observed with the tape applied. Injection-correctness guards stay fatal. Corrections, rolling and peers never
	// use this path: there the same guards fail closed.
	const bool soft_mode = data.round_transitions && !data.correction.active && !data.rolling && !data.peer.enabled;
	dispatch.handle(soft_mode);
	dispatch.commit_spectator();
	const auto end = session_clock::now();
	row.host_end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - data.origin).count();
	row.hook_duration_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
	measure_dispatch.finish();
	complete_row(index, row);
	data.end_callback();
}

void pbd::rollback_session::tick_dispatch::commit_spectator()
{
	if (data.spectator.channel && row.completed && !data.roundtrip.repeat_active && (!data.lobby.direct_match.pbd_live_inputs() || data.spectator.frame))
	{
		const u32 played = data.spectator.channel->consumed();
		const bool committed = data.spectator.channel->commit(ppu.id, pc, index, row, data.spectator.expected);
		if (!committed)
			fail("spectator_completed_row_unverified");
		if (committed && data.spectator.execution_file)
		{
			const auto& tick = data.tick_at(index);
			const u32 source_index = data.spectator.channel->pbd_recorded_boundary(index);
			const auto encoded = pbd::lobby_set::sync_record({source_index, tick.session_segment, tick.frame_counter, tick.scene[0], hash_mask(index), tick.gate_hold,
				tick.combat, tick.read && tick.combat_read && tick.generator_read});
			const std::vector<u8> facts(encoded.begin(), encoded.end());
			const auto text = fmt::format("{\"type\":\"spectator_frame\",\"gameNumber\":%u,\"frame\":%u,\"boundary\":%u,\"leftPadWord\":%u,\"rightPadWord\":%u,\"leftInjectedWord\":%u,\"rightInjectedWord\":%u,\"syncFacts\":{\"encoding\":\"pbd-sync-facts-v1\",\"definition\":\"pbd-peer-sync-v3\",\"data\":\"%s\",\"digest\":\"%s\"}}\n",
				data.spectator.channel->game(), played, source_index, row.raw[0], row.raw[1], row.ports[0].injected_r3, row.ports[1].injected_r3,
				pbd::lobby_set::base64url(facts), hash_hex(facts.data(), facts.size()));
			if (data.spectator.execution_file.write(text.data(), text.size()) != text.size())
				fail("spectator_execution_trace_failed");
		}
		data.spectator.frame.reset();
	}
}
