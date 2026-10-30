#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

namespace
{
	constexpr std::array<u32, 5> timer_table{1800, 2400, 3600, 4800, 5940};
}

bool pbd::rollback_session::tick_dispatch::prepare_battle_clock()
{
	if (data.battle_clock.enabled)
	{
		if (!data.battle_clock.anchor_set && g_pbd_restored_timebase_valid)
		{
			data.battle_clock.anchor = g_pbd_restored_timebase;
			data.battle_clock.anchor_set = true;
		}
		if (!data.battle_clock.anchor_set)
		{
			fail("battle_clock_anchor_unavailable");
			return true;
		}
		if (data.battle_clock.version == 2 && !data.battle_clock.site_checked)
		{
			u32 site = 0;
			if (!read_word(0x36c4b8, site) || site != 0x7c0c42e6)
			{
				fail("battle_clock_frame_start_site_mismatch");
				return true;
			}
			u32 gpu_call = 0, gpu_return = 0;
			if (!read_word(pbd::t5dr::gpu_load_probe - 4, gpu_call) || gpu_call != 0x480f3905 ||
				!read_word(pbd::t5dr::gpu_load_probe, gpu_return) || gpu_return != 0x60000000)
			{
				fail("battle_clock_gpu_load_site_mismatch");
				return true;
			}
			data.battle_clock.site_checked = true;
		}
		if (data.battle_clock.early_read)
		{
			fail("battle_clock_read_before_anchor");
			return true;
		}
	}
	return false;
}

bool pbd::rollback_session::tick_dispatch::adapt_frame_clock()
{
	data.correction.frame_start_adapted = false;
	if (data.correction.active && data.battle_clock.enabled && data.correction.clock.replaying && data.battle_clock.anchor_set)
	{
		const u64 logical = data.battle_clock.anchor + (static_cast<u64>(index) + 1) * data.battle_clock.freq / 60;
		if (!write_dword(0x1074f9f8, logical))
		{
			fail("correction_frame_start_write_failed");
			return true;
		}
		data.correction.frame_start_adapted = true;
	}

	return false;
}

bool pbd::rollback_session::tick_dispatch::read_boundary_input(bool soft_mode, bool& round_reset)
{
	row.entry_r1 = ppu.gpr[1];
	row.before_read = read_word(counter_address, row.before_counter);
	if (!row.before_read)
	{
		fail("entry_consumer_mismatch");
		return true;
	}
	if (const u32 expected_counter = initial_state().input_counter + index + static_cast<u32>(data.consumer_offset); row.before_counter != expected_counter)
	{
		if (data.round_lifecycle && row.before_counter == 0 && index > data.start_index)
		{
			// Round lifecycle: the input-side counter restarts at 0 at a phase change (K.O., round end, round start).
			// The dispatch index (network frame identity) is unaffected; only the expectation re-anchors.
			data.consumer_offset = -static_cast<s32>(initial_state().input_counter + index);
			round_reset = true;
		}
		else if (data.session.enabled && index > data.start_index && !data.match_at(index))
		{
			// Session MENU boundary: the input-side counter restarts at screen changes, not always at 0; re-anchor.
			data.consumer_offset += static_cast<s32>(row.before_counter) - static_cast<s32>(expected_counter);
			round_reset = true;
		}
		else if (pbd::session_start::entry_witness_recovery(data.peer.enabled, soft_mode, index > data.start_index))
		{
			// The input-side counter was reset or skipped between two dispatches (round end, cut-scene): re-anchor.
			data.consumer_offset = pbd::session_start::counter_offset(row.before_counter, initial_state().input_counter, index);
			round_reset = true;
			soft_fail("entry_consumer_mismatch");
		}
		else
		{
			fail("entry_consumer_mismatch");
			return true;
		}
	}
	if (ppu.lr != pbd::t5dr::boundary_lr || row.entry_r1 < 0x110)
	{
		fail("entry_caller_or_stack_mismatch");
		return true;
	}

	return false;
}

bool pbd::rollback_session::tick_dispatch::verify_boundary_scene(bool scene_read, const std::array<be_t<u32>, 2>& scene_now, bool soft_mode)
{
	if (data.session.start_skip)
	{
		row.start_phase.scene = scene_now[0];
		row.start_phase.verified = scene_read && read_word(0x10093230, row.start_phase.pending);
	}
	if (!scene_read || (!data.session.enabled && !scene_admitted(scene_now[0], scene_now[1], data.round_lifecycle)))
	{
		if (scene_read && data.peer.enabled && data.round_lifecycle && index > data.start_index)
		{
			// LAN session: the game left the battle scene (match over).
			// The session ends cleanly here; an asymmetric exit surfaces as the other peer's hold timeout.
			pbd_session_log.warning("Battle scene left at boundary %u (scene %u/%u): match ended.", index, u32{scene_now[0]}, u32{scene_now[1]});
			session_runtime.finish(match_ended);
			return true;
		}
		if (!soft_mode || !scene_read)
		{
			fail("entry_scene_mismatch");
			return true;
		}
		soft_fail("entry_scene_mismatch");
	}

	return false;
}

bool pbd::rollback_session::tick_dispatch::advance_native_match()
{
	if (data.lobby.direct_match.enabled() && !data.roundtrip.repeat_active)
	{
		// Keep the ordinary native end/score observation, then stop before any menu input executes.
		if (data.lobby.direct_match_end_pending)
		{
			session_runtime.finish(match_ended);
			data.in_callback.store(false);
			return true;
		}
		if (!data.lobby.direct_match.start(ppu, index))
		{
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
	}
	if (data.lobby.rematch.active() && !data.roundtrip.repeat_active)
	{
		const auto loaded = data.lobby.rematch.advance(ppu, index);
		if (loaded.state == pbd::rematch::progress::refused)
		{
			pbd_session_log.error("Lobby Rematch failed: %s (CPU flags %x, scheduler preemption %u, current function %u)",
				loaded.reason, static_cast<u32>(ppu.state.load()), ppu.ack_suspend ? 1u : 0u, ppu.current_function ? 1u : 0u);
			if (data.lobby.set)
				data.lobby.set->refuse(loaded.reason, "The next game could not load.");
			session_runtime.finish(guard_failed);
			data.in_callback.store(false);
			return true;
		}
		if (loaded.state == pbd::rematch::progress::complete)
			pbd::game_overlay::publish(data.game_overlay.panel);
	}

	return false;
}

bool pbd::rollback_session::tick_dispatch::verify_direct_replay()
{
	if (data.lobby.direct_match.enabled())
	{
		const auto& tick = data.tick_at(index);
		if (!data.lobby.direct_match.verify_sync(index, data.session.segments.number, tick.scene[0], tick.frame_counter,
				hash_mask(index), tick.combat, tick.read && tick.combat_read && tick.generator_read))
		{
			if (data.checkpoint.capture_tick_state) // the refused boundary's memory, to compare with the recording's match snapshot
				write_memdump(data.checkpoint.capture_directory / fmt::format("memdump-refused-%u", index));
			fail("direct_replay_peer_sync_mismatch");
			return true;
		}
	}
	return false;
}

bool pbd::rollback_session::tick_dispatch::verify_spectator_replay()
{
	if (data.spectator.channel && (!data.lobby.direct_match.pbd_live_inputs() || data.spectator.frame))
	{
		const auto& tick = data.tick_at(index);
		if (!tick.read || !tick.combat_read || !tick.generator_read ||
			!data.spectator.channel->verify_state(index, data.session.segments.number, tick.scene[0], tick.frame_counter, hash_mask(index), tick.combat))
		{
			fail("spectator_peer_sync_hash_mismatch");
			return true;
		}
		const bool battle = tick.scene[0] == 16u || tick.scene[0] == 17u;
		data.spectator.was_battle = data.spectator.was_battle || battle;
		if (battle && !data.spectator.intro_seen)
		{
			u32 pending = 0, stage = 0, left = 0, right = 0, rounds = 0;
			const bool read = read_word(0x10093230, pending) && read_word(0x1008f088, stage) && read_word(0x10093f50, left) &&
			                  read_word(0x10093f54, right) && read_word(0x1008f0a0, rounds);
			const auto phase = pbd::match_recording::classify(tick.scene[0], pending, false);
			const bool intro = phase.verified && (data.spectator.channel->needs_round_state() ? pending == 4 :
																								(phase.value == pbd::match_recording::phase::character_intro || phase.value == pbd::match_recording::phase::round_intro));
			if (intro && read)
			{
				pbd::spectator::native_round native_round;
				if (data.spectator.channel->needs_round_state())
				{
					be_t<u16> random_left{}, random_right{};
					if (!read_word(0x1008f0e4, native_round.ordinal) || native_round.ordinal > 8 ||
						!vm::try_access(0x100901b6, &random_left, sizeof(random_left), false) ||
						!vm::try_access(0x10090a86, &random_right, sizeof(random_right), false))
					{
						fail("spectator_round_unreadable");
						return true;
					}
					native_round.pending_phase = pending;
					native_round.random = {u16{random_left}, u16{random_right}};
				}
				if (!data.spectator.channel->expose(ppu.id, index, stage, left, right, rounds, true,
						data.spectator.channel->needs_round_state() ? &native_round : nullptr))
				{
					fail("spectator_intro_unverified");
					return true;
				}
				data.spectator.intro_seen = true;
			}
		}
	}
	return false;
}

void pbd::rollback_session::tick_dispatch::write_boundary_snapshots()
{
	if (data.checkpoint.capture_tick_state && !data.roundtrip.repeat_active && (std::find(data.checkpoint.dump_indices.begin(), data.checkpoint.dump_indices.end(), index) != data.checkpoint.dump_indices.end() || (data.session.dump_matches && data.session.matches && (index == data.session.match_start || index == data.session.match_start + 300))))
	{
		// Diagnostic full-memory dump: every mapped page of main memory, the RSX context/IO block and
		// the stack, read through the sudo alias before this boundary dispatches.
		const u32 count = write_memdump(data.checkpoint.capture_directory / fmt::format("memdump-%u", index));
		pbd_session_log.warning("Memory dump at boundary %u: %u pages.", index, count);
	}
	if (data.checkpoint.capture_tick_state && data.session.enabled && !data.session.snapshot_ranges.empty() && data.session.matches && index == data.session.match_start && !data.roundtrip.repeat_active)
	{
		const u32 count = write_memdump(data.checkpoint.capture_directory / fmt::format("match-snapshot-%u-%u", data.session.matches, index), &data.session.snapshot_ranges);
		pbd_session_log.warning("Match snapshot %u at boundary %u: %u pages.", data.session.matches, index, count);
	}
	if (data.checkpoint.capture_tick_state && data.checkpoint.late_index && index == data.checkpoint.late_index)
	{
		// Tick write set and owner context at a later boundary, same DRTPB001 format as the holds.
		std::vector<u8> bytes;
		const bool read_ok = read_pages(data.checkpoint.checkpoint_pages, bytes);
		const auto stem = data.checkpoint.capture_directory / fmt::format("tick-probe-%u-late", index);
		const auto bin_utf8 = std::filesystem::path(stem.wstring() + L".bin").u8string();
		fs::file out(std::string(bin_utf8.begin(), bin_utf8.end()), fs::write_new + fs::isfile);
		const u32 count = static_cast<u32>(data.checkpoint.checkpoint_pages.size());
		const u32 cr = ppu.cr.pack();
		const std::array<u8, 4> xer{u8{ppu.xer.so}, u8{ppu.xer.ov}, u8{ppu.xer.ca}, ppu.xer.cnt};
		const bool written = read_ok && !!out && out.write("DRTPB001", 8) == 8 && out.write(&index, 4) == 4 && out.write(&count, 4) == 4 &&
		                     out.write(data.checkpoint.checkpoint_pages.data(), count * 4ull) == count * 4ull && out.write(bytes.data(), bytes.size()) == bytes.size() &&
		                     out.write(ppu.gpr, sizeof(ppu.gpr)) == sizeof(ppu.gpr) && out.write(ppu.fpr, sizeof(ppu.fpr)) == sizeof(ppu.fpr) &&
		                     out.write(ppu.vr, sizeof(ppu.vr)) == sizeof(ppu.vr) && out.write(&cr, 4) == 4 && out.write(&ppu.lr, 8) == 8 &&
		                     out.write(&ppu.ctr, 8) == 8 && out.write(xer.data(), 4) == 4 && out.write(&ppu.cia, 4) == 4;
		const auto meta = fmt::format(
			"{\"type\":\"probe\",\"index\":%u,\"phase\":\"late\",\"pages\":%u,\"unreadable_pages\":%u,\"capture_ns\":0,\"written\":%s,"
			"\"lwmutex\":[],\"mutex\":{\"id\":0,\"read\":false,\"owner\":0,\"waiters\":false,\"lock_count\":0},\"owner_id\":%u,\"cia\":%u}\n",
			index, count, read_ok ? 0 : 1, json_bool(written), ppu.id, ppu.cia);
		const auto json_utf8 = std::filesystem::path(stem.wstring() + L".json").u8string();
		fs::file json(std::string(json_utf8.begin(), json_utf8.end()), fs::write_new + fs::isfile);
		if (json)
			json.write(meta.data(), meta.size());
	}
}

void pbd::rollback_session::tick_dispatch::record_match_options_digest()
{
	if (data.session.digests.enabled)
	{
		// Options digest at the match start (session_state::digests_state::enabled): the applied values after round init.
		u32 od = 0x811c9dc5u;
		std::string listing;
		const auto add = [&](u32 address, u32 size)
		{
			std::array<u8, 64> bytes{};
			const bool ok = address && size <= bytes.size() && vm::try_access(address, bytes.data(), size, false);
			fmt::append(listing, "%s0x%x:", listing.empty() ? "" : " ", address);
			for (u32 i = 0; i < size; i++)
			{
				const u8 b = ok ? bytes[i] : 0xee;
				od = (od ^ b) * 0x01000193u;
				fmt::append(listing, "%02x", b);
			}
		};
		add(pbd::t5dr::game_mode_address, 4);
		add(0x1008ebb0, 13);
		add(0x1008ebd4, 1);
		add(0x1008ebf4, 8);
		add(0x1008f098, 16);
		add(0x1008f0a8, 12);
		add(0x1008f0dc, 4);
		for (const u32 slot : {0x10091ab0u, 0x10091ab4u})
		{
			u32 player = 0;
			read_word(slot, player);
			add(player ? player + 0x6ac : 0, 8);
		}
		add(0x100500cc, 64);
		data.session.digests.options_seq = data.session.matches;
		data.session.digests.options = od;
		const u32 oslot = data.session.matches % 8;
		const bool oknown = data.session.digests.peer_options_seq[oslot] == data.session.matches;
		if (oknown && data.session.digests.peer_options[oslot] != od)
			data.peer.fault = "session_options_mismatch";
		pbd_session_log.warning("Session: options digest %08x at match %u%s (%s)", od, data.session.matches,
			oknown ? (data.session.digests.peer_options[oslot] == od ? ", equal to the other peer's" : ", DIFFERENT from the other peer's") : "", listing);
	}
}

void pbd::rollback_session::tick_dispatch::record_match_stage_digest()
{
	// Stage digest (session_state::digests_state::stage).
	u32 digest = 0x811c9dc5u;
	const auto mix = [&](u32 word)
	{
		for (u32 s = 0; s < 32; s += 8)
			digest = (digest ^ ((word >> s) & 0xffu)) * 0x01000193u;
	};
	mix(static_cast<u32>(pbd::fn_hook::stage_controllers.size()));
	for (const u32 controller : pbd::fn_hook::stage_controllers)
	{
		be_t<u32> word{};
		mix(vm::try_access(controller + 0x48, &word, sizeof(word), false) ? u32{word} : 0xffffffffu);
	}
	data.session.digests.stage_count = static_cast<u32>(pbd::fn_hook::stage_controllers.size());
	pbd::fn_hook::stage_controllers.clear();
	data.session.digests.stage = digest;
	data.session.digests.stage_match = data.session.matches;
	record_match_options_digest();
	const u32 slot = data.session.matches % 8;
	const bool known = data.session.digests.peer_stage_match[slot] == data.session.matches;
	if (known && data.session.digests.peer_stage[slot] != digest)
		data.peer.fault = "session_stage_mismatch";
	pbd_session_log.warning("Session: stage digest %08x over %u mob controllers (match %u)%s", digest, data.session.digests.stage_count, data.session.matches,
		known ? (data.session.digests.peer_stage[slot] == digest ? ", equal to the other peer's" : ", DIFFERENT from the other peer's") : "");
	if (data.peer.enabled)
	{
		const auto text = fmt::format("{\"type\":\"session_stage_digest\",\"boundary\":%u,\"match\":%u,\"controllers\":%u,\"digest\":%u}\n",
			index, data.session.matches, data.session.digests.stage_count, digest);
		data.peer.file.write(text.data(), text.size());
	}
}

void pbd::rollback_session::tick_dispatch::record_match_transition(bool battle, const std::array<be_t<u32>, 2>& scene_now)
{
	u32 counter = 0;
	read_word(0x1008f07c, counter);
	pbd_session_log.warning("Session: %s %u at boundary %u (scene %u/%u, frame counter %u)", battle ? "match start" : "match end",
		data.session.matches, index, u32{scene_now[0]}, u32{scene_now[1]}, counter);
	// The stage id (game-flow object +0x28) and each
	// player's character-table row (0x4403c0 rows: character, costume variant, name index) chosen for this match.
	u32 stage_id = ~0u, p1_row = ~0u, p2_row = ~0u;
	read_word(0x1008f088, stage_id);
	read_word(0x10093f50, p1_row);
	read_word(0x10093f54, p2_row);
	if (battle)
		pbd_session_log.warning("Session: match %u picks: stage 0x%x, P1 row 0x%x, P2 row 0x%x", data.session.matches, stage_id, p1_row, p2_row);
	if (data.peer.enabled)
	{
		const auto text = fmt::format("{\"type\":\"session_%s\",\"boundary\":%u,\"match\":%u,\"scene\":[%u,%u],\"frame_counter\":%u,\"stage\":%d,\"p1_row\":%d,\"p2_row\":%d}\n",
			battle ? "match_start" : "match_end", index, data.session.matches, u32{scene_now[0]}, u32{scene_now[1]}, counter,
			static_cast<s32>(stage_id), static_cast<s32>(p1_row), static_cast<s32>(p2_row));
		data.peer.file.write(text.data(), text.size());
	}
}

void pbd::rollback_session::tick_dispatch::read_lobby_battle(pbd::match_recording::observation& observed, pbd::lobby_set::context& context, const std::array<be_t<u32>, 2>& scene_now)
{
	using namespace pbd::match_recording;
	data.lobby.battle_seen = true;
	u32 pending = ~0u, time_index = ~0u, timer_frames = 0;
	// The applied handicap percents feed the stock vitality: big-endian halves at 0x1008ebf6 (left) and 0x1008ebfa
	// (right); the halves before them count VS wins.
	std::array<be_t<u16>, 2> handicap_left{}, handicap_right{};
	context.verified = read_word(0x1008f088, context.stage) && read_word(0x10093f50, context.left_row) &&
	                   read_word(0x10093f54, context.right_row) && read_word(0x1008f0a0, context.rounds) &&
	                   read_word(0x1008f09c, time_index) && time_index < timer_table.size() &&
	                   read_word(0x10052db0 + time_index * 4, timer_frames) && timer_frames == timer_table[time_index] &&
	                   vm::try_access(0x1008ebf4, handicap_left.data(), sizeof(handicap_left), false) &&
	                   vm::try_access(0x1008ebf8, handicap_right.data(), sizeof(handicap_right), false);
	context.handicap = {u16{handicap_left[1]}, u16{handicap_right[1]}};
	context.time_index = time_index;
	const bool pending_read = read_word(0x10093230, pending);
	context.pending_phase = pending;
	be_t<u16> random_left{}, random_right{};
	context.round_verified = pending_read && pending == 4 && read_word(0x1008f0e4, context.round_ordinal) &&
	                         context.round_ordinal <= 8 && vm::try_access(0x100901b6, &random_left, sizeof(random_left), false) &&
	                         vm::try_access(0x10090a86, &random_right, sizeof(random_right), false);
	if (context.round_verified)
		context.round_random = {u16{random_left}, u16{random_right}};
	const auto classified = classify(u32{scene_now[0]}, pending, data.lobby.set->round_closed(), data.lobby.set->match_decided());
	observed.native_phase = classified.value;
	observed.phase_verified = pending_read && classified.verified;
}

void pbd::rollback_session::tick_dispatch::read_match_replay_context()
{
	if (!data.match_replay.context && !data.roundtrip.repeat_active)
	{
		u32 stage = ~0u, p1 = ~0u, p2 = ~0u, rounds = 0, time_index = ~0u, timer_frames = 0;
		const bool context_read = read_word(0x1008f088, stage) && read_word(0x10093f50, p1) && read_word(0x10093f54, p2) &&
		                          read_word(0x1008f0a0, rounds) && read_word(0x1008f09c, time_index) &&
		                          stage <= 255 && p1 <= 255 && p2 <= 255 && rounds == data.match_replay.lifecycle.rounds_to_win() &&
		                          time_index < timer_table.size() && read_word(0x10052db0 + 4 * time_index, timer_frames) &&
		                          timer_frames == timer_table[time_index];
		if (!context_read)
			replay_refuse(index, "native_match_context_unverified");
		else
		{
			const auto text = fmt::format("{\"type\":\"replay_match_context\",\"match\":1,\"boundary\":%u,\"p1_row\":%u,\"p2_row\":%u,\"stage\":%u,\"rounds_to_win\":%u,\"round_time\":%u}\n",
				index, p1, p2, stage, rounds, timer_frames / 60);
			if (data.peer.file.write(text.data(), text.size()) != text.size())
				replay_refuse(index, "marker_write_failed");
			else
				data.match_replay.context = true;
		}
	}
}

void pbd::rollback_session::tick_dispatch::read_replay_score(pbd::match_recording::observation& observed)
{
	using namespace pbd::match_recording;
	u32 player_left = 0, player_right = 0;
	std::array<be_t<s16>, 2> left_result{}, right_result{};
	const bool result_read = read_word(0x10091ab0, player_left) && read_word(0x10091ab4, player_right) &&
	                         player_left == 0x10090040 && player_right == 0x10090910 &&
	                         vm::try_access(player_left + 0x90, left_result.data(), sizeof(left_result), false) &&
	                         vm::try_access(player_right + 0x90, right_result.data(), sizeof(right_result), false);
	observed.result_verified = result_read && s16{left_result[0]} >= 0 && s16{right_result[0]} >= 0 &&
	                           s16{left_result[0]} <= 5 && s16{right_result[0]} <= 5;
	if (observed.result_verified)
	{
		observed.score_left = static_cast<u32>(s16{left_result[0]});
		observed.score_right = static_cast<u32>(s16{right_result[0]});
		u32 health_left = 0, health_right = 0, timer = 0;
		// The draw branch writes both round-result markers to -1. Require its native terminal health/timer condition too.
		observed.draw_verified = s16{left_result[1]} == -1 && s16{right_result[1]} == -1 &&
		                         read_word(player_left + 0x6ac, health_left) && read_word(player_right + 0x6ac, health_right) &&
		                         read_word(0x1008f054, timer) && health_left == health_right &&
		                         (static_cast<s32>(health_left) <= 0 || static_cast<s32>(timer) <= 0);
	}
}
