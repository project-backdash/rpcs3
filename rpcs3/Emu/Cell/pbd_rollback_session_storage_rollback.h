#pragma once

// The rollback machinery's state: per-tick history, checkpoints, corrections, the battle clock and the undo and fault capture. Members of pbd::rollback_session::storage
// (pbd_rollback_session_storage.h).

#include "pbd_rollback_session_common.h"
#include "../PBD/pbd_undo_restore.h"

namespace pbd::session_runtime
{
	// Only the boundary owner publishes these facts. Diagnostics never inspect live guest registers or histories.
	struct boundary_diagnostic
	{
		u64 generation = 0, sequence = 0;
		u32 next_index = 0, correction_at = 0, correction_depth = 0;
		bool repeat_active = false;
	};

	struct practice_prefix
	{
		u64 completed = 0, injected = 0;
	};
	struct practice_anchor_metadata
	{
		bool valid = false;
		tick_sample tick{};
		u8 match = 0, neutral = 0, start = 0;
		practice_prefix prefix{};
	};
	// Per-tick records, resized together and reached through history_slot().
	struct history_state
	{
		pbd::session_history::buffer<update_sample, rollback_session::max_updates> samples{0};
		bool practice_continuous = false;
		bool session_continuous = false, session_history_rolling = false;
		pbd::practice::rolling::window<2048> practice_window;
		practice_prefix practice_produced{}, practice_rewind_prefix{};
		practice_anchor_metadata practice_anchor{};
		pbd::session_history::buffer<tick_sample, rollback_session::max_updates> ticks{0};
		pbd::session_history::buffer<u8, rollback_session::max_updates> entry_assigned{};
		pbd::session_history::buffer<u16, rollback_session::max_updates> live_inputs{}; // Live controller: the owned port input fixed at each tick's first dispatch.
		pbd::session_history::buffer<u16, rollback_session::max_updates> entry_sample{};
		pbd::session_history::buffer<u8, rollback_session::max_updates> entry_sampled{};
		pbd::session_history::buffer<u8, rollback_session::max_updates> live_fixed{};
		pbd::session_history::buffer<u16, rollback_session::max_updates> peer_remote_value{};
		pbd::session_history::buffer<s32, rollback_session::max_updates> peer_remote_arrival{}; // Local boundary index at first receipt; -1 = not received.
		pbd::session_history::buffer<u8, rollback_session::max_updates> session_start_suppressed{};
		pbd::session_history::buffer<u8, rollback_session::max_updates> session_start_masked_at{}, session_start_executed_at{};
		pbd::session_history::buffer<u8, rollback_session::max_updates> match_ticks{}; // 1 = battle-scene boundary (session mode), set at the first dispatch of the tick
		// Session NEUTRAL INPUT: transition / loading frames (every scene except character select 3 and the battle 16/17) take a
		// variable number of frames on each host, and a peer that runs extra ones must not feed them the players' buttons: a few
		// extra frames of scene 12 with buttons held leave battle-state region 1 different at the first match tick.
		// The game gets 0 on both ports there; the row keeps the published value (protocol and rollback
		// comparisons unchanged) and is marked soft = "session_neutral_input".
		pbd::session_history::buffer<u8, rollback_session::max_updates> input_neutral_ticks{};
		pbd::session_history::buffer<pbd::match_recording::observation, rollback_session::max_updates> match_replay_observations{0};
		pbd::session_history::buffer<u8, rollback_session::max_updates> match_replay_observed{0};
		pbd::session_history::buffer<u8, rollback_session::max_updates> lockstep_ticks{}; // 1 = tick dispatched in phase lockstep (received remote value is usable at once).
	};
	// The static-page checkpoints, the tick trace and its probe.
	struct checkpoint_state
	{
		// Tick boundary evidence lives in its own file; the frozen update trace format is unchanged.
		bool tick = false;
		u32 tick_hold_index = 0, tick_hold_ms = 100;
		fs::file tick_file;
		bool capture_tick_state = false;
		std::vector<u32> checkpoint_pages;
		// Address-contiguous runs of checkpoint_pages as (first index, page count). Each vm::try_read_stream call copies
		// one run, amortizing range locking and address checks. Non-temporal stores keep capture from evicting the game's
		// data; the ring is read only during restore.
		std::vector<std::pair<u32, u32>> checkpoint_runs;
		std::unique_ptr<pbd_capture_pool> capture_pool; // PBD_TICK_CAPTURE_THREADS helpers; none = copy on the owner thread only
		// The two 64 KiB effect-region hashes of every tick row are a DIAGNOSTIC (for comparing runs); they cost a
		// byte-serial FNV over 128 KiB at every boundary, live and re-executed. Default: on without a peer, off with one.
		// PBD_TICK_EFFECT_HASH=0|1 overrides.
		int effect_hash = -1;
		std::filesystem::path capture_directory;
		checkpoint_memory_writer memory;
		u32 exclusion_file_ranges = 0;
		// Capture the tick write set and owner context at one later boundary for cross-run comparison.
		u32 late_index = 0;
		std::vector<u32> dump_indices; // PBD_TICK_DUMP_INDEX: full guest memory dumps at these boundaries (comma list; diagnostic).
		// Checkpoint ring: one entry per boundary, slot index % ring.size().
		std::vector<ring_entry> ring;
		// Undo pages: the first main-PPU interpreter store into a page outside the static tick write set during tick t saves
		// that page's prior bytes into ring entry t; a restore to m applies entries T-1..m newest to oldest. HLE/native
		// writes are not seen (counted as undo_hle_calls/undo_unsupported instructions).
		std::vector<u64> static_page_bits;
	};
	// The single-instance round-trip, repeat and branch tests.
	struct roundtrip_state
	{
		// Round trip (owner thread only).
		bool active = false, snapshot_ok = false, locks_free = false, clock_error = false;
		bool repeat_active = false, repeat_completed = false, repeat_equal = false;
		// Syscall wall time of the replayed tick being executed (owner thread only), by syscall number.
		std::array<u64, 1024> replay_syscall_ns{}, replay_syscall_count{};
		u32 phase = 0, counter_n = 0, clock_replay = 0, repeat_index = 0;
		u64 restored_bytes = 0, restore_ns = 0;
		std::vector<u8> pages_n, pages_s1, pages_s2;
		owner_context context_n{}, context_s1{};
		std::array<u32, 2> previous_held_n{};
		std::array<u32, 5> saved_battle_counters{};
		std::vector<std::pair<u32, u64>> clock;
		update_sample repeat_row{};
		bool identity_ok = false, identity_context_equal = false, readback_ok = false;
		u64 identity_written = 0, identity_diff = 0, identity_diff_excluded = 0, readback_diff = 0, readback_diff_excluded = 0;
		// Branch test: pass 1 re-executes tick N with roundtrip.branch_raw on port 0, pass 2 with the tape input.
		bool branch = false, wrong_repeat_completed = false;
		u16 branch_raw = 0;
		u32 pass = 0;
		std::vector<u8> pages_wrong;
		owner_context context_wrong{};
		std::array<u32, 5> battle_counters_mismatch{};
		std::array<u32, 2> wrong_injected{};
	};
	// In-place corrections: their window, clock replay, restore costs and readback.
	struct correction_state
	{
		u64 undo_excluded = 0; // undo bytes skipped by the declared exclusions in this correction
		// In-place depth-k correction: ticks [correction.at-depth, correction.at) first run with correction.predict_raw on
		// port 0; at boundary correction.at the ring entry of correction.at-depth is restored and the window re-executed
		// with the tape (authoritative) input. First-pass rows stay in the frozen trace; corrected rows go to
		// tick-correction.jsonl.
		// Correction frame-start adapter: the guest's frame-start timestamp global 0x1074f9f8 is written by the MAIN LOOP
		// (0x110e0 -> 0x259958 -> 0x36c4b8), which an in-place correction does not re-execute. Without this adapter a
		// re-entered tick differences its own logical now against a start belonging to the discarded future (an unsigned
		// underflow) or against a start several steps stale; both exceed the load monitor's start threshold 100, and
		// overload prunes live effect objects.
		bool frame_start_adapted = false;
		bool active = false;
		u32 peer_held_index = 0xffffffffu; // Latest boundary whose entry held for the other peer (evidence for late restarts).
		u32 at = 0, depth = 0, phase = 0;
		u16 predict_raw = 0;
		u64 restore_ns = 0, written = 0;
		bool undo_restore_unique = true;
		pbd::undo_restore::plan undo_restore_plan;
		// Correction budget: the phases inside restore_ns, and the readback policy. The readback (re-read the whole
		// static set and compare it byte by byte with the checkpoint) is a verification;
		// in peer sessions it is sampled (the first 4 corrections and every 32nd; PBD_RESTORE_READBACK=all|sample|off),
		// the confirmed-state hashes remain the running check. Rolling / single-emulator test modes still verify every time.
		u64 rewind_ns = 0, rearm_ns = 0;
		fs::file file;
		// Exact withhold (see pbd_replay_pacing.h): correction.withhold_flagged counts the re-executed frames the published markers name.
		bool withhold_exact = false;
		u32 withhold_flagged = 0;
		// Restore under suspend-all (PBD_RESTORE_SUSPEND=1): the undo pages and the static page set of a
		// rolling/peer correction are written while every other guest CPU thread is suspended (cpu_thread::suspend_all),
		// because an SPU job that reads memory during the rewind writes garbage afterwards (CellSpursKernel access violation
		// at 0xffdead00 followed by an RSX semaphore timeout). The RSX thread is not a cpu_thread and keeps running.
		bool restore_suspend = false;
		bool replay_skip_draws = false;        // PBD_REPLAY_SKIP_DRAWS=1: replayed frames are flipped without their draw calls
		u64 suspend_ns = 0;                    // wall time of the suspended restore (0 when not suspended)
		const char* suspend_skipped = nullptr; // why a requested suspension was not used for this correction
		u64 undo_applied = 0;
		std::string undo_pages;            // Page addresses undone by the latest correction, in application order.
		std::string skip_pages;            // Library-first-stored pages in the corrected window, deliberately not undone.
		session_clock::time_point begin{}; // Restore start of the latest correction (for correction_wall_ns).
		u64 audio_begin[3]{};              // cellAudio underruns / time advances / silence blocks at the restore start (cumulative).
		bool all_known = false;            // The running correction was triggered at a phase-lockstep boundary: every window tick is known.
		std::string foreign_pages;
		// Clock recording and replay inside a correction window.
		struct clock_state
		{
			bool error = false, recording = false;
			u32 tick = 0, cursor = 0, mismatches = 0;
			// Replay follows boundary-to-boundary intervals, exactly like recording (switched at input entries, not at row completion).
			bool replaying = false;
			u32 replay_tick = 0;
			std::array<u32, 8> replay_counts{}, journal_sizes{};
		} clock;
		// The guest locks a restore waits for.
		struct locks_state
		{
			bool free = false;
			u64 wait_ns = 0;                    // Time spent waiting for the guest locks before the latest rolling/peer restore.
			std::string busy, forced_signature; // Busy locks at the latest restore; the set last forced
			bool forced = false;
		} locks;
		// The readback that verifies a restore.
		struct readback_state
		{
			bool ok = false;
			u64 diff = 0, excluded = 0;
			u64 ns = 0;
			u32 policy = 0; // 0 = all, 1 = sample, 2 = off, 3 = first4 (the first 4 corrections only)
			bool checked = false;
			std::string spans; // Non-excluded bytes that differed right after the restore write.
		} readback;
		// Time spent in the latch callback during a correction window.
		struct window_state
		{
			// Time spent INSIDE the latch callback during a correction window (from the restore start), so that a window's
			// wall time splits into restore, latch work and guest execution. Per callback address: [pc, ns, calls].
			session_clock::time_point scope_start{};
			u64 hook_ns = 0;
			bool open = false;
			std::array<std::array<u64, 3>, 8> hook_by_pc{};
		} window;
		// Black-flash diagnostics: the renderer at a restore and at the window's end.
		struct render_state
		{
			// Black-flash diagnostics. PBD_RESTORE_RENDER_WAIT_MS=N (1-50, OFF by default): at window start, before the voice
			// gate and the rewind, wait up to N ms until the renderer has consumed everything submitted before the restore (it is parked in
			// the frame-handshake acquire, or its FIFO GET equals PUT). Record-only fields describe the renderer at the restore and at the
			// window end; the RSX thread publishes the exact lag at the marker hit (pbd_replay_pacing.h).
			u32 wait_ms = 0;
			u64 wait_ns = 0;
			bool wait_ok = false, waited = false;
			u64 rsx_ended = 0, rsx_skipped = 0, restore_steady_ns = 0;
			u32 rsx_parked = 0, rsx_get = 0, rsx_put = 0, marker_slot = 0;
			bool marker_published = false;
			// An idle FIFO (GET == PUT) sampled by the wait while other threads still run is not an owned safe point (only the
			// parked frame handshake is): the rewind re-checks it right before its first write and counts a stale sample.
			bool safe_point_stale = false;
			u32 safe_point_stale_total = 0;
		} render;
	};
	// The battle clock (PBD_BATTLE_CLOCK): guest time from battle progress.
	struct battle_clock_state
	{
		// Battle clock (PBD_BATTLE_CLOCK=1): main-PPU mftb reads observe battle progress instead of host time.
		// Before the first boundary: the savestate-restored timebase (anchor). During tick t (boundary t entry until boundary
		// t+1): anchor + floor((t + 1) * frequency / 60). One tick is 1/60 game second (battle-tick contract, round timer).
		// The phase is fixed at tick start, so in-tick intervals are zero; re-executed ticks derive time from their index.
		bool enabled = false;
		u64 anchor = 0, freq = 0, reads = 0;
		s64 tick = -1;
		// The latch configures during module loading, before Emulator::FixGuestTime restores the savestate timebase, so the
		// anchor is taken on first use; a policy read before a valid anchor fails the run closed at the next boundary.
		bool anchor_set = false, early_read = false;
		// Version 2 (PBD_BATTLE_CLOCK=2, battle-clock-v2): the logical step is phase-aligned to the game's own frame-start
		// timestamp (mftb at 0x36c4b8, called only from the main loop at 0x110e0, after the input boundary in each iteration).
		// During tick t reads return anchor + (t + 1 + s) * F / 60 with s = 1 once that sample ran in tick t; s is cleared at
		// every input boundary, so it needs no checkpoint (always 0 at a restore boundary) and the value never decreases.
		// Game time still advances exactly one tick per frame; the game's processing-time measurements do not span a step.
		u32 version = 0;
		bool frame_started = false, site_checked = false;
		u64 frame_samples = 0;
	};
	// Undo pages: the owner's first writes outside the static set, by tick.
	struct undo_state
	{
		std::vector<u64> page_bits;
		// The renderer, its IO map and the deny list stay fixed during a correction run, so each page's RSX/deny verdict
		// is cached. classified_page_bits marks that a verdict exists; refused_page_bits holds it.
		std::vector<u64> classified_page_bits, refused_page_bits, denied_page_bits;
		std::vector<u32> marked;
		u32 tick = 0, max_pages = 0;
		bool ready = false;
		u64 saves = 0, overflows = 0;
		std::vector<std::pair<u64, u64>> deny;
		// PBD_TICK_UNDO_ADMIT_IO_SPANS = hex "lo-hi,lo-hi" (end exclusive): RSX-IO mapped heap inside these spans IS armed
		// for write-fault capture although the rest of the IO map is refused. The game's whole heap is IO mapped; without this its
		// task / pool / replay-recorder state is restored only where the static page list happens to cover it, and
		// arming the WHOLE heap freezes the game (command buffer, vertex streams the renderer is reading).
		std::vector<std::pair<u64, u64>> admit_io_spans;
		bool admit_rsx_io = false, admit_library = false; // PBD_TICK_UNDO_ADMIT diagnostic: admit refused classes. // PBD_TICK_UNDO_DENY diagnostic spans never undone (listed with pc bit 2).
	};
	// Write-fault capture (PBD_TICK_UNDO_CAPTURE=fault).
	struct fault_state
	{
		// Write-fault capture (PBD_TICK_UNDO_CAPTURE=fault): see the design notes above fault_protect.
		bool capture = false, armed_all = false, capture_foreign = false;
		std::atomic<u64> lock{0};        // Host thread tag holding the capture lock.
		std::vector<u64> never_arm_bits; // Pages never write-protected: static set, RSX-facing, deny spans.
		// The stacks of OTHER guest threads (0xD0000000 up, outside the owner's stack) are never armed. A foreign first
		// write is never captured, so arming them would only cost those threads a fault per page and tick and the owner a
		// re-protect call per stack at every boundary (the owner does not write them).
		u32 owner_stack_lo = 0, owner_stack_hi = 0;
		bool foreign_stack(u64 page) const noexcept
		{
			return owner_stack_hi && page >= 0xD0000000ull && !(page >= owner_stack_lo && page < owner_stack_hi);
		}
		// Pages whose first write in some tick came from a thread other than the owner. Such a page is left uncaptured in that
		// tick, and the owner's later stores to it are then invisible (the page is already writable): the rollback rewinds the
		// owner's state everywhere else but not there.
		std::vector<u64> foreign_page_bits;
		std::vector<u32> disarmed;    // Pages unprotected since the last boundary re-arm.
		std::vector<std::string> log; // Diagnostic rows produced under the capture lock, drained by the owner at a boundary.
		u64 captures = 0, foreign = 0, foreign_pages = 0, events = 0, events_mark = 0, capture_ns = 0, capture_ns_mark = 0;
		u64 armed_pages = 0, arm_calls = 0, arm_ns = 0, remaps = 0;
	};
} // namespace pbd::session_runtime
