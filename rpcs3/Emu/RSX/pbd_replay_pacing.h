#pragma once
#include "util/types.hpp"
#include <atomic>
#include <array>

// PBD catch-up presentation pacing.
//
// The game's main loop blocks once per frame in
// sys_semaphore_wait on the semaphore its vblank handler posts from the GCM interrupt thread, and that handler runs
// only when the host "VBlank Thread" posts a vblank at the configured 60 Hz. A correction re-executes the window
// ticks inside one host frame period, but every re-executed frame still waits for a REAL vblank, so a correction
// costs one frame period per replayed tick (33/67/133 ms at depths 2/4/8).
//
// While a latch with PBD_REPLAY_VBLANK=1 is active, the vblank thread polls `credits` at a fine interval and posts
// one extra ("virtual") vblank per credit immediately, through the same post_vblank_event path as a scheduled vblank
// (same guest handler, same counters). The latch grants exactly one credit per replayed tick: one when the restore
// completes and one at every re-entered window boundary. Scheduled vblanks are untouched, so live frames keep their
// real pacing. Nothing here changes guest logic; it only changes WHEN the guest's own vblank wait is satisfied.
namespace pbd::replay_pacing
{
	inline std::atomic<u32> armed{0};   // nonzero while a latch with replay pacing is active: the vblank thread polls credits
	inline std::atomic<u32> credits{0}; // virtual vblanks still to post
	inline std::atomic<u64> granted{0}; // evidence: credits granted by the latch
	inline std::atomic<u64> posted{0};  // evidence: virtual vblanks actually posted by the vblank thread
	// Without notification the main thread spends a third to a half of every re-executed tick blocked
	// in the game's per-frame sys_semaphore_wait on the vblank semaphore (posted by _gcm_intr_thread from the vblank handler),
	// because a credit is only served when the vblank thread's poll wakes (thread_ctrl::wait_for(200 us), host timer granularity).
	// PBD_REPLAY_VBLANK_NOTIFY=1 (OFF by default) makes grant() wake that poll immediately. It changes only WHEN the already
	// granted virtual vblank is delivered inside a re-executed window; the guest handler, counters and live pacing are untouched.
	inline std::atomic<u32> notify_on_grant{0};
	inline std::atomic<u64> notified{0}; // evidence: grants that woke the vblank thread
	// PBD_REPLAY_VBLANK_LEAD=1 (OFF by default): the latch grants the replayed window's credits one tick early (two at the restore,
	// none at the last re-entered boundary; the total per window is unchanged), so the vblank handler has posted the semaphore before
	// the frame-sync task waits on it. Presentation pacing only; the guest handler and counters are unchanged.
	inline std::atomic<u32> lead{0};
	inline std::atomic<u64> lead_extra{0}, lead_skipped{0}; // evidence: extra credits at restores, credits withheld at last boundaries
	void notify_vblank();                                   // defined in RSXThread.cpp (needs the vblank thread object)
	inline void grant() noexcept
	{
		credits.fetch_add(1, std::memory_order_acq_rel);
		granted.fetch_add(1, std::memory_order_relaxed);
		if (notify_on_grant.load(std::memory_order_relaxed))
		{
			notified.fetch_add(1, std::memory_order_relaxed);
			notify_vblank();
		}
	}
	// One virtual vblank served at once whatever notify_on_grant says: a watcher's hidden catch-up frame waits for nothing.
	inline void grant_now() noexcept
	{
		credits.fetch_add(1, std::memory_order_acq_rel);
		granted.fetch_add(1, std::memory_order_relaxed);
		notify_vblank();
	}
	// PBD_REPLAY_SKIP_DRAWS=1: nonzero while a corrected window is being re-executed; the RSX thread then
	// flips the frames submitted meanwhile without executing their draw calls (renderer skip_current_frame), so the
	// guest's flip wait ends at once. frames_skipped counts the frames flipped that way.
	inline std::atomic<u32> skip_draws{0};
	inline std::atomic<u64> frames_skipped{0};
	// Time sync between peers: a pending shift of the SCHEDULED vblank grid in microseconds (positive = later = this
	// peer falls back, negative = earlier = it catches up). The vblank thread moves the grid by at most slew_step_us per
	// scheduled vblank (1.5 % of the period: not visible, not audible) until the request is used up; slewed_us is evidence.
	inline std::atomic<s64> slew_us{0};
	inline std::atomic<s64> slewed_us{0};
	inline constexpr s64 slew_step_us = 250;
	// Scheduled (real) vblanks posted so far; the latch counts them against the live ticks it runs (repay).
	inline std::atomic<u64> scheduled{0};
	// A new vblank rate to use from the next vblank on (0 = none pending). The vblank thread otherwise takes video.vblank_rate only
	// once per emulated second; a replay's speed change sets both so the new speed starts at once instead of up to a second later.
	inline std::atomic<u32> vblank_request{0};
	// A faster replay must also exceed a display-rate frame cap. Zero retains the player's configured limiter.
	inline std::atomic<u32> pbd_player_rate{0};
	// EXACT WITHHOLD (PBD_REPLAY_WITHHOLD_EXACT=1). skip_draws is a level the PPU raises and lowers while the RSX
	// thread runs behind it, and the renderer samples it at the END of the previous frame: sampled that way, the first
	// re-executed frame of every window is drawn and presented and the frame after the window is not.
	// With guest-side draw skipping that first frame would show without stage and fighters.
	// Instead the latch publishes the FIFO PUT position at the restore and the depth. Everything below that position was
	// submitted before the restore; the RSX thread reaches exactly that position when it has consumed it, and the frames it
	// builds from there are the re-executed ones: it withholds `depth` frames, counted at its own in-band frame ends. No
	// counting on the PPU side, no waiting, no assumption about how far the renderer lags.
	inline std::atomic<u32> exact{0};
	inline std::atomic<u64> frames_ended{0};          // frame ends the RSX thread has counted (diagnostic)
	inline std::array<std::atomic<u64>, 8> markers{}; // (depth << 32) | put position
	inline std::atomic<u32> marker_write{0};          // markers published so far (the RSX thread keeps its own read count)
	inline std::atomic<u32> markers_missed{0};        // dropped because the RSX never stood at the position (diagnostic)
	// Black-flash diagnostics (record only, no behaviour change). At the marker hit the RSX thread publishes, per marker
	// slot (slot % 8): the pre-restore frames it ended after the restore (marker_age = renderer lag in frames at the restore), the
	// re-executed ticks the latch had completed by then (how far the window ran ahead of the renderer) and the hit time.
	inline std::array<std::atomic<u32>, 8> marker_hit_lag{};
	inline std::array<std::atomic<u32>, 8> marker_hit_replayed{};
	inline std::array<std::atomic<u64>, 8> marker_hit_ns{};   // steady_clock nanoseconds since epoch
	inline std::array<std::atomic<u32>, 8> marker_hit_slot{}; // slot + 1 of the marker last hit in this ring entry (freshness)
	inline std::atomic<u32> replay_completed{0};              // latch: re-executed ticks completed in the current window
	// RSX thread: 1 while it waits in the frame-handshake acquire (label + 0xFD0) for a value the guest has not written yet, i.e.
	// it has consumed everything up to the end of the last present; the optional bounded wait at window start polls it.
	inline std::atomic<u32> handshake_parked{0};
	inline std::atomic<u64> handshake_waits{0};
	// Frame-handshake acquires the renderer found already passed: the label held a newer value than the one awaited (nv406e.cpp).
	inline std::atomic<u64> handshake_passed{0};
	// PBD_REPLAY_WITHHOLD_AT_RISK=1 (OFF by default; exact withhold only): the latch counts restores that are about to
	// rewrite memory (incremented right before the rewind, taken back if the correction then fails; every counted restore publishes
	// exactly one marker). A live frame that the RSX ends while a counted restore's marker has not been reached yet was (partly) drawn
	// from memory the restore rewrote: it is withheld (the previous image stays one more refresh) instead of presented black.
	inline std::atomic<u32> withhold_at_risk{0};
	inline std::atomic<u32> restores_started{0};
	inline std::atomic<u64> frames_withheld_at_risk{0};
	// Frames withheld after a dropped (expired) marker to cover the rest of its restore's depth (at-risk withhold only).
	inline std::atomic<u64> markers_expired_withheld{0};

	// A reused game boots a new RSX thread whose marker read count starts at 0, so the published markers and counted restores
	// start there too; stale counts would hold every live frame of the next match until they drained.
	inline void reset_for_boot() noexcept
	{
		marker_write.store(0, std::memory_order_release);
		restores_started.store(0, std::memory_order_release);
		replay_completed.store(0, std::memory_order_relaxed);
		handshake_parked.store(0, std::memory_order_relaxed);
		for (auto& marker : markers)
			marker.store(0, std::memory_order_relaxed);
		for (usz slot = 0; slot < marker_hit_slot.size(); ++slot)
		{
			marker_hit_lag[slot].store(0, std::memory_order_relaxed);
			marker_hit_replayed[slot].store(0, std::memory_order_relaxed);
			marker_hit_ns[slot].store(0, std::memory_order_relaxed);
			marker_hit_slot[slot].store(0, std::memory_order_relaxed);
		}
	}
} // namespace pbd::replay_pacing
