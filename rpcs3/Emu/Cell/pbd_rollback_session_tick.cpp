// The stages of one boundary's tick: the entry stage with its scene, lobby and replay observations, and the input
// stages after it.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_practice_lesson_channel.h"
#include "../PBD/pbd_practice_start.h"
#include "../PBD/pbd_dummy_move_runtime.h"

void pbd::rollback_session::tick_dispatch::fail(const char* message)
{
	row.failure = message;
	data.guard_failure.store(message);
	if (data.live_stats)
		pbd::live_stats::publish_guard(message);
	pbd_session_log.error("Guard '%s' failed at boundary %u (stage %u).", message, index, data.stage);
	session_runtime.finish(guard_failed);
}

void pbd::rollback_session::tick_dispatch::soft_fail(const char* message)
{
	if (!row.soft)
		row.soft = message;
}

void pbd::rollback_session::tick_dispatch::handle(bool soft_mode)
{
	if (data.generation != static_cast<u64>(Emu.GetEmulationIdentifier()))
	{
		fail("emulation_generation_changed");
		return;
	}
	if (pc != stage_pcs[data.stage])
	{
		fail("unexpected_input_stage");
		return;
	}
	if (pc == pcs[0])
	{
		entry_stage(soft_mode);
		return;
	}
	input_stages(soft_mode);
}

void pbd::rollback_session::tick_dispatch::entry_stage(bool soft_mode)
{
	if (data.roundtrip.repeat_active && !data.correction.active && !data.takeover.bridge.active)
	{
		fail("roundtrip_repeat_entry");
		return;
	}
	row.entry_seen = true;
	if (prepare_battle_clock())
		return;
	data.battle_clock.tick = index; // battle clock: tick index executing from this boundary (live or re-executed)
	data.battle_clock.frame_started = false;
	// Correction frame-start adapter (see correction_state::frame_start_adapted): while a correction is replaying,
	// set the guest's frame-start timestamp to the value the original execution held at THIS boundary, so the
	// game's own interval arithmetic stays coherent through the restore. Ordinary execution is untouched.
	if (adapt_frame_clock())
		return;
	// Catch-up pacing: the frame executing the tick entered here has already passed its vblank wait, so this
	// credit serves the NEXT replayed frame (or the closing live frame). One credit per replayed tick in total:
	// this site covers boundaries m+1..T-1 and the restore site covers the window start.
	if (data.replay.vblank && data.correction.active && data.roundtrip.repeat_active)
	{
		// Lead (PBD_REPLAY_VBLANK_LEAD): the restore granted this boundary's credit already, so the last re-entered
		// boundary (index T-1) grants none; the window's total stays one credit per replayed tick.
		if (pbd::replay_pacing::lead.load(std::memory_order_relaxed) && index + 1 >= data.correction.at)
			pbd::replay_pacing::lead_skipped.fetch_add(1, std::memory_order_relaxed);
		else
			pbd::replay_pacing::grant();
	}
	if (!data.roundtrip.repeat_active)
		g_pbd_replay_frame.store(false, std::memory_order_relaxed);
	bool round_reset = false;
	if (read_boundary_input(soft_mode, round_reset))
		return;
	std::array<be_t<u32>, 2> scene_now{};
	const bool scene_read = vm::try_access(pbd::t5dr::scene_address, scene_now.data(), sizeof(scene_now), false);
	if (verify_boundary_scene(scene_read, scene_now, soft_mode))
		return;
	session_scenes(scene_read, scene_now);
	if (advance_native_match())
		return;
	if (lobby_set_observation(scene_read, scene_now))
		return;
	match_replay_observation(scene_read, scene_now);
	if (tick_record(round_reset, scene_now, soft_mode))
		return;
	if (correction())
		return;
	if (data.peer.live.enabled && data.lobby.set && !data.roundtrip.repeat_active && pbd::fight_timing::requested())
		pbd::fight_timing::samples.forward(data.generation, index, pbd_fight_frame);
	pbd::frame_presentation::mark_input(row.before_counter);
	pbd::frame_presentation::mark_logical(row.before_counter, data.session.segments.number, !data.roundtrip.repeat_active, index, scene_now[0]);
	data.stage++;
	if (verify_direct_replay())
		return;
	if (verify_spectator_replay())
		return;
	if (control_hold())
		return;
	if (roundtrip_compare())
		return;
	write_boundary_snapshots();
}

void pbd::rollback_session::tick_dispatch::session_scenes(bool scene_read, const std::array<be_t<u32>, 2>& scene_now)
{
	if (data.session.enabled && !data.roundtrip.repeat_active && scene_read)
	{
		data.match_replay.live.observe_boundary(index, scene_now[0]);
		// Leaving VS mode: a long run of scene 0 is the title screen or main menu (any way back there, such as a quit
		// combination), where the session's neutral inputs would leave both games running the attract demos. Both
		// peers read the same scenes at the same boundaries, so both end here; the end message covers a peer that lags.
		if (scene_now[0] == 0u)
		{
			if (data.session.scene0_since == ~0u)
				data.session.scene0_since = index;
			else if (index - data.session.scene0_since >= session_left_vs_boundaries && data.session.pending_end == collecting)
			{
				data.session.pending_end = left_vs_mode;
				pbd_session_log.warning("Scene 0 since boundary %u (%u boundaries): the game left VS mode; the session ends.", data.session.scene0_since, index - data.session.scene0_since);
				if (data.peer.file)
				{
					const auto text = fmt::format("{\"type\":\"left_vs_mode\",\"boundary\":%u,\"since\":%u}\n", index, data.session.scene0_since);
					data.peer.file.write(text.data(), text.size());
				}
			}
		}
		else
			data.session.scene0_since = ~0u;
		// Session mode: a match starts at the first battle-scene boundary and ends when the battle scene is left.
		const bool battle = scene_now[0] == 17u || scene_now[0] == 16u;
		if (battle != data.session.in_match)
		{
			data.practice.restart.placement.reset();
			data.practice.restart.reset_request.reset();
			data.practice.restart.position = pbd::practice_store::kept_restart(data.practice.restart.position);
			data.session.in_match = battle;
			if (battle)
			{
				data.session.matches++;
				data.session.match_start = index;
			}
			if (!battle && data.replay.fast.active && data.replay.fast.until_match && data.session.matches + 1 >= data.replay.fast.until_match)
				end_replay_fast(data, index);
			if (!battle && data.replay.taken && data.session.pending_end == collecting)
			{
				// A replay whose side was taken has left its recording: it ends with the match that was played.
				data.session.pending_end = match_ended;
				pbd_session_log.warning("Replay: match %u ended at boundary %u after a side was taken; the replay ends.", data.session.matches, index);
			}
			if (battle)
				record_match_stage_digest();
			record_match_transition(battle, scene_now);
		}
	}
}

bool pbd::rollback_session::tick_dispatch::lobby_set_observation(bool scene_read,
	const std::array<be_t<u32>, 2>& scene_now)
{
	if (data.lobby.set && data.lobby.set->active() && scene_read)
	{
		using namespace pbd::match_recording;
		observation observed{};
		observed.boundary = index;
		pbd::lobby_set::context context;
		const bool battle = scene_now[0] == 16u || scene_now[0] == 17u;
		u32 player_left = 0, player_right = 0;
		std::array<be_t<s16>, 2> left_result{}, right_result{};
		observed.result_verified = read_word(0x10091ab0, player_left) && read_word(0x10091ab4, player_right) &&
		                           player_left == 0x10090040 && player_right == 0x10090910 &&
		                           vm::try_access(player_left + 0x90, left_result.data(), sizeof(left_result), false) &&
		                           vm::try_access(player_right + 0x90, right_result.data(), sizeof(right_result), false) &&
		                           s16{left_result[0]} >= 0 && s16{right_result[0]} >= 0 && s16{left_result[0]} <= 5 && s16{right_result[0]} <= 5;
		if (observed.result_verified)
		{
			observed.score_left = static_cast<u32>(s16{left_result[0]});
			observed.score_right = static_cast<u32>(s16{right_result[0]});
			u32 health_left = 0, health_right = 0, timer = 0;
			observed.draw_verified = s16{left_result[1]} == -1 && s16{right_result[1]} == -1 &&
			                         read_word(player_left + 0x6ac, health_left) && read_word(player_right + 0x6ac, health_right) &&
			                         read_word(0x1008f054, timer) && health_left == health_right &&
			                         (static_cast<s32>(health_left) <= 0 || static_cast<s32>(timer) <= 0);
		}
		if (battle)
		{
			read_lobby_battle(observed, context, scene_now);
		}
		else if (data.lobby.battle_seen)
		{
			std::array<u8, 3> winner_bytes{};
			const bool winner_read = vm::try_access(0x1008ebd5, winner_bytes.data(), winner_bytes.size(), false);
			if (winner_read && winner_bytes == std::array<u8, 3>{1, 0, 1})
				observed.outcome = winner::left;
			else if (winner_read && winner_bytes == std::array<u8, 3>{2, 1, 0})
				observed.outcome = winner::right;
			// A drawn match, classified exactly as the replay path below does: both fighters at the rounds to win, and the
			// draw branch's markers with its terminal health and timer. The lobby replays that game under its own number.
			else if (winner_read && observed.draw_verified && observed.score_left == data.lobby.channel->identity().rounds_per_game &&
					 observed.score_right == observed.score_left)
				observed.outcome = winner::draw;
			else
				observed.result_verified = false;
			observed.native_phase = phase::match_end;
			observed.phase_verified = true;
			pbd_session_log.warning("Lobby native end at boundary %u: players=%x/%x, score=%u/%u, result_verified=%u, winner_read=%u, winner=%u/%u/%u, draw=%u",
				index, player_left, player_right, observed.score_left, observed.score_right, observed.result_verified,
				winner_read, winner_bytes[0], winner_bytes[1], winner_bytes[2], observed.outcome == winner::draw);
		}
		else
		{
			const auto classified = classify(u32{scene_now[0]}, 0, false);
			observed.native_phase = classified.value;
			observed.phase_verified = classified.verified;
		}
		// Replacement is per boundary, including reexecution. Only publish_through consumes the confirmed observation.
		pbd_fight_frame = observed.phase_verified && observed.native_phase == phase::fight;
		const bool recorded = data.lobby.set->record(observed, context);
		if (recorded)
			publish_through(data.published.load(std::memory_order_acquire));
		if (!recorded || !data.lobby.set->healthy())
		{
			pbd_session_log.warning("Lobby native lifecycle refused at boundary %u: reason=%u, phase=%u, result_verified=%u, score=%u/%u, outcome=%u",
				index, static_cast<u32>(data.lobby.set->result_refusal()), static_cast<u32>(observed.native_phase),
				observed.result_verified, observed.score_left, observed.score_right, static_cast<u32>(observed.outcome));
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
	}
	return false;
}

void pbd::rollback_session::tick_dispatch::match_replay_observation(bool scene_read,
	const std::array<be_t<u32>, 2>& scene_now)
{
	if (data.match_replay.requested && data.match_replay.attempted && !data.match_replay.refused && scene_read)
	{
		using namespace pbd::match_recording;
		const bool battle = scene_now[0] == 16u || scene_now[0] == 17u;
		if (data.match_replay.lifecycle.complete() && battle)
			replay_refuse(index, "fresh_checkpoint_required");
		else if (data.match_replay.lifecycle.active() && !data.match_replay.end_pending)
		{
			observation observed{};
			observed.boundary = index;
			read_replay_score(observed);
			if (battle)
			{
				data.match_replay.battle_seen = true;
				read_match_replay_context();
				u32 pending = ~0u;
				const bool pending_read = read_word(0x10093230, pending);
				const auto classified = classify(u32{scene_now[0]}, pending, data.match_replay.lifecycle.round_closed(), data.match_replay.lifecycle.match_decided());
				observed.native_phase = classified.value;
				observed.phase_verified = pending_read && classified.verified;
			}
			else if (data.match_replay.battle_seen)
			{
				std::array<u8, 3> winner_bytes{};
				const bool winner_read = vm::try_access(0x1008ebd5, winner_bytes.data(), winner_bytes.size(), false);
				if (winner_read && winner_bytes == std::array<u8, 3>{1, 0, 1})
					observed.outcome = winner::left;
				else if (winner_read && winner_bytes == std::array<u8, 3>{2, 1, 0})
					observed.outcome = winner::right;
				else if (winner_read && observed.draw_verified && observed.score_left == data.match_replay.lifecycle.rounds_to_win() &&
						 observed.score_right == observed.score_left)
					observed.outcome = winner::draw;
				else
					observed.result_verified = false;
				observed.native_phase = phase::match_end;
				observed.phase_verified = true;
				data.match_replay.end = observed;
				data.match_replay.end_pending = true;
			}
			else
			{
				const auto classified = classify(u32{scene_now[0]}, 0, false);
				observed.native_phase = classified.value;
				observed.phase_verified = classified.verified;
			}
			if (!data.match_replay.refused && !data.match_replay.end_pending)
			{
				if (index >= data.history.match_replay_observed.size())
					replay_refuse(index, "native_timeline_limit");
				else
				{
					// A corrected pass replaces the speculative phase/result before any marker for this update is published.
					data.history.match_replay_observations[index] = observed;
					data.history.match_replay_observed[index] = 1;
				}
			}
			replay_publish(data.published.load(std::memory_order_acquire));
		}
	}
}

void pbd::rollback_session::tick_dispatch::input_stages(bool soft_mode)
{
	if (!row.entry_seen || ppu.gpr[1] != row.entry_r1 - 0x110)
	{
		fail("invocation_stack_mismatch");
		return;
	}
	if (read_input_counter(soft_mode))
		return;
	if (pc == pcs[3])
	{
		if (!row.ports[0].injected || !row.ports[1].injected || !row.ports[0].posted || !row.ports[1].posted || ppu.gpr[26] != 2)
		{
			if (!soft_mode)
			{
				fail("incomplete_ordered_ports");
				return;
			}
			soft_fail("incomplete_ordered_ports");
		}
		row.completed = true;
		data.stage = 0;
		return;
	}
	const u32 port = data.stage < 3 ? 0 : 1;
	auto& sample = row.ports[port];
	if (ppu.gpr[26] != port || ppu.gpr[30] != port || ppu.gpr[31] != port ||
		ppu.gpr[28] != 0x10099fb4 + 4 * port || ppu.gpr[27] != masks_address + 4 * port)
	{
		fail("port_register_or_output_pointer_mismatch");
		return;
	}
	u32 raw = row.raw[port];
	if (data.session.enabled && data.neutral_at(index))
	{
		raw = 0; // session neutral input (see history_state::input_neutral_ticks)
		row.soft = "session_neutral_input";
	}
	if (data.session.enabled && data.session.start_skip && index < data.extent())
	{
		const u8 bit = static_cast<u8>(1u << port);
		const auto input = pbd::session_start::filter(static_cast<u16>(raw), row.start_phase,
			index > 0 && (data.history.session_start_suppressed[index - 1] & bit));
		const bool suppress = data.match_at(index) && input.suppressed;
		if (data.match_at(index) && (raw & 0x0800))
			raw = input.word;
		if (suppress)
			data.history.session_start_suppressed[index] |= bit;
		else
			data.history.session_start_suppressed[index] &= static_cast<u8>(~bit);
	}
	if (pc == pcs[1])
	{
		if (inject_input(port, sample, raw))
			return;
	}
	else
	{
		if (verify_input(port, sample, raw))
			return;
	}
	data.stage++;
}
