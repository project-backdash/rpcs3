// Replay takeover: the in-memory anchor from which the replay viewer's controller takes a side, its checkpoint and
// its restore, and the count of the lv2 calls that would make a rewind across them unsafe.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"

#include <unordered_set>

std::atomic<bool> g_pbd_takeover_anchor_live{false};

std::atomic<u64> g_pbd_takeover_lifecycle_syscalls{0};

std::atomic<u64> g_pbd_takeover_last_lifecycle_syscall{0};

#include "../PBD/pbd_takeover_restore_steps.h"

namespace
{
	constexpr bool pbd_takeover_changes_lv2_topology(u64 code) noexcept
	{
		// Execution-only calls such as SPU group start/join are deliberately absent: Tekken pairs them at fight
		// boundaries without creating or destroying an object. Refuse only when a rewind would cross a topology or
		// mapping mutation that the guest-memory journal cannot reconstruct.
		switch (code)
		{
		case 41:
		case 44:
		case 45:
		case 50:
		case 51:
		case 52:
		case 53: // PPU thread lifecycle
		case 150:
		case 160:
		case 161:
		case 169:
		case 170:
		case 171:
		case 172: // raw/SPU creation
		case 174:
		case 175:
		case 177: // SPU group suspension or termination
		case 185:
		case 186:
		case 191:
		case 192:
		case 193:
		case 194: // SPU event/queue topology
		case 230:
		case 231:
		case 232:
		case 233: // isolated SPU lifecycle
		case 251:
		case 252: // all-thread event connections
		case 300:
		case 301:
		case 302:
		case 303:
		case 304:
		case 305:
		case 313: // VM mappings and locks
		case 324:
		case 325:
		case 326:
		case 327:
		case 328:
		case 329:
		case 330:
		case 331:
		case 332:
		case 333:
		case 334:
		case 335:
		case 336:
		case 337:
		case 339:
		case 341:
		case 342:
		case 345:
		case 348:
		case 349:
		case 350:
		case 362: // memory objects and mappings
			return true;
		default:
			return false;
		}
	}
} // namespace

void pbd_takeover_note_syscall(u64 code) noexcept
{
	if (!g_pbd_takeover_anchor_live.load(std::memory_order_relaxed))
		return;
	if (!pbd_takeover_changes_lv2_topology(code))
		return;
	g_pbd_takeover_last_lifecycle_syscall.store(code, std::memory_order_relaxed);
	g_pbd_takeover_lifecycle_syscalls.fetch_add(1, std::memory_order_relaxed);
}

void pbd::rollback_session::boundary_call::save_takeover_host()
{
	auto& s = data.takeover.dispatch_slot;
	s.pbd_cursor = data.lobby.direct_match.pbd_capture_cursor();
	s.consumer_offset = data.consumer_offset;
	s.last_fight_index = data.last_fight_index;
	s.previous_held = data.previous_held;
	s.replay_round = data.replay.round;
	s.replay_round_start = data.replay.round_start;
	s.replay_was_fighting = data.replay.was_fighting;
	s.replay_taken = data.replay.taken;
	s.replay_control_port = data.replay.control_port;
	s.session_segment = data.session.segments.number;
	s.session_segment_index = data.session.segments.index;
	s.session_segment_scene = data.session.segments.scene;
	s.session_prev_segment_index = data.session.segments.prev_index;
	s.session_prev_segment_scene = data.session.segments.prev_scene;
	s.session_prev_segment_counter = data.session.segments.prev_counter;
	s.session_segment_counter = data.session.segments.counter;
	s.session_last_scene = data.session.segments.last_scene;
	s.session_last_scene_valid = data.session.segments.last_scene_valid;
	s.session_in_match = data.session.in_match;
	s.session_matches = data.session.matches;
	s.session_match_start = data.session.match_start;
	s.session_scene0_since = data.session.scene0_since;
	s.session_pending_end = data.session.pending_end;
	s.battle_counter = initial_state().battle_counter;
	s.replay_pad = data.replay.pad;
	s.replay_pad_prev = data.replay.pad_prev;
	s.replay_start_clean = data.replay.start_clean;
	s.session_leave_hold = data.session.leave_hold;
	s.session_leave_next_source = data.session.leave_next_source;
	s.session_leave_start_held = data.session.leave_start_held;
	s.takeover_rounds_size = data.takeover.rounds.size();
}

void pbd::rollback_session::boundary_call::load_takeover_host(const takeover_host_slot& s)
{
	data.consumer_offset = s.consumer_offset;
	data.last_fight_index = s.last_fight_index;
	data.previous_held = s.previous_held;
	data.replay.round = s.replay_round;
	data.replay.round_start = s.replay_round_start;
	data.replay.was_fighting = s.replay_was_fighting;
	data.replay.taken = s.replay_taken;
	data.replay.control_port = s.replay_control_port;
	data.session.segments.number = s.session_segment;
	data.session.segments.index = s.session_segment_index;
	data.session.segments.scene = s.session_segment_scene;
	data.session.segments.prev_index = s.session_prev_segment_index;
	data.session.segments.prev_scene = s.session_prev_segment_scene;
	data.session.segments.prev_counter = s.session_prev_segment_counter;
	data.session.segments.counter = s.session_segment_counter;
	data.session.segments.last_scene = s.session_last_scene;
	data.session.segments.last_scene_valid = s.session_last_scene_valid;
	data.session.in_match = s.session_in_match;
	data.session.matches = s.session_matches;
	data.session.match_start = s.session_match_start;
	data.session.scene0_since = s.session_scene0_since;
	data.session.pending_end = s.session_pending_end;
	initial_state().battle_counter = s.battle_counter;
	data.replay.pad = s.replay_pad;
	data.replay.pad_prev = s.replay_pad_prev;
	data.replay.start_clean = s.replay_start_clean;
	data.session.leave_hold = s.session_leave_hold;
	data.session.leave_next_source = s.session_leave_next_source;
	data.session.leave_start_held = s.session_leave_start_held;
	if (data.takeover.rounds.size() > s.takeover_rounds_size)
		data.takeover.rounds.resize(s.takeover_rounds_size);
}

// One in-memory anchor at boundary index: the static pages, the owner's context and the guards a restore compares, with the
// host slot sampled before this boundary's replay controls. Its journal starts empty; the write-fault handler fills it.
bool pbd::rollback_session::boundary_call::capture_anchor(takeover_anchor& a, u32 index, const takeover_host_slot& host,
	bool reserve_journal)
{
	a = {};
	auto& entry = a.checkpoint;
	try
	{
		entry.pages.resize(data.checkpoint.checkpoint_pages.size() * 4096ull);
		if (reserve_journal)
		{
			a.journal_pages.reserve(4096);
			a.journal_bytes.reserve(4096ull * 4096);
			a.journal_index.reserve(4096);
		}
	}
	catch (...)
	{
		data.takeover.refusal = "anchor_allocation_failed";
		return false;
	}
	bool pages_ok = false;
	if (data.checkpoint.capture_pool)
		pages_ok = data.checkpoint.capture_pool->run(entry.pages.data());
	else
		pages_ok = read_pages(data.checkpoint.checkpoint_pages, entry.pages);
	std::array<be_t<u32>, 2> scene{};
	u32 counter = 0;
	if (!pages_ok || !read_word(counter_address, counter) ||
		!vm::try_access(pbd::t5dr::scene_address, scene.data(), sizeof(scene), false))
	{
		data.takeover.refusal = "anchor_read_failed";
		return false;
	}
	a.index = index;
	a.match = data.session.matches;
	a.frame = match_frame(index);
	a.segment = data.session.segments.number;
	a.scene = {u32{scene[0]}, u32{scene[1]}};
	a.battle_counter = initial_state().battle_counter;
	a.battle_anchor = data.battle_clock.anchor;
	a.fault_remaps = data.fault.remaps;
	a.lifecycle_syscalls = g_pbd_takeover_lifecycle_syscalls.load(std::memory_order_relaxed);
	a.fn_counters = pbd::fn_hook::takeover_counters();
	entry.valid = true;
	entry.index = index;
	entry.counter = counter;
	entry.voice_checkpoint_token = pbd::fn_hook::voice_checkpoint_token();
	entry.consumer_offset = data.consumer_offset;
	entry.last_fight_index = data.last_fight_index;
	entry.context = read_context(ppu);
	entry.previous_held = data.previous_held;
	// Guest pages and PPU context are captured at the ordinary rollback ring's checkpoint. Keep the host-control state
	// saved before the boundary's replay action so a chosen side is not baked into the anchor.
	a.host = host;
	if (data.replay.viewer_enabled || data.practice.rewind)
		a.frame_state = pbd::practice::native::frame_save();
	a.valid = true;
	return true;
}

bool pbd::rollback_session::boundary_call::capture_takeover(u32 index)
{
	if (!data.replay.takeover_fast)
		return true;
	data.takeover.capture_pending = false;
	auto& a = data.takeover.keyframe;
	if (!capture_anchor(a, index, data.takeover.capture_host, true))
		return false;
	if (data.practice.rewind)
	{
		a.practice_native_controlled = data.practice.native.controlled;
		a.practice_native_getup = data.practice.native.getup;
		a.practice_native_action = data.practice.native.action;
		a.practice_native_written = data.practice.native.written;
		data.practice.restart.moment_combos = data.practice.combos;
		data.practice.restart.moment_health_previous = data.practice.health_previous;
		data.practice.restart.moment_health_valid = data.practice.health_valid;
	}
	if (data.history.practice_continuous)
	{
		data.history.practice_anchor = {true, data.tick_at(index), data.match_at(index), data.neutral_at(index),
			data.start_executed_at(index), data.history.practice_produced};
	}
	g_pbd_takeover_anchor_live.store(true, std::memory_order_release);
	const u32 audit_pages = data.takeover.audit ? write_memdump(data.checkpoint.capture_directory / fmt::format("memdump-%u", index)) : 0;
	const auto text = fmt::format("{\"type\":\"takeover_anchor\",\"boundary\":%u,\"match\":%u,\"frame\":%u,\"segment\":%u,\"static_pages\":%u,\"static_bytes\":%u,\"audit_pages\":%u}\n",
		index, a.match, a.frame, a.segment, data.checkpoint.checkpoint_pages.size(), a.checkpoint.pages.size(), audit_pages);
	data.replay.control_file.write(text.data(), text.size());
	if (data.practice.rewind)
	{
		data.practice.remembers++;
		data.practice.view.toast = "Moment remembered\t{select}+{r3} goes back to it";
		data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(2);
		pbd::practice::publish(data.practice.view);
	}
	return true;
}

void pbd::rollback_session::boundary_call::request_takeover_capture(u32 index)
{
	if (!data.replay.takeover_fast)
		return;
	save_takeover_host();
	data.takeover.capture_host = data.takeover.dispatch_slot;
	data.takeover.capture_index = index;
	data.takeover.capture_pending = true;
}

bool pbd::rollback_session::boundary_call::restore_takeover(const char* action, u32 side, u32 from)
{
	auto& a = data.takeover.keyframe;
	takeover_anchor* const own[] = {&a};
	if (!restore_anchor(a, own, action, side, from))
		return false;
	a.journal_pages.clear();
	a.journal_bytes.clear();
	a.journal_index.clear();
	a.overflow = false;
	rewind_forget_after(a.index);
	return true;
}

// Restore anchor a from boundary `from`. `journals` are the undo journals written since a, oldest first (a's own first); the
// earliest copy of a page wins, so each page gets its value at a. Then a's static pages and context, and the one-boundary
// bridge that re-enters a with `side` (0 = the recording) driving the taken fighter.
bool pbd::rollback_session::boundary_call::restore_anchor(takeover_anchor& a, std::span<takeover_anchor* const> journals,
	const char* action, u32 side, u32 from)
{
	return takeover_restore_steps{*this, a, journals, action, side, from}.run();
}
