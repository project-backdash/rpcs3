#pragma once

// PBD: generic guest-function probes and replay skips.
//
// Translator hooks observe chosen guest instruction addresses and can return early from selected function entries
// while the main PPU re-executes a corrected tick. Session features also require hooks for loading, input and RNG control.
//
// Optional environment settings extend the hooks required by the configured session:
//   PBD_FN_PROBES=0x25c3e0,0xb837c,...   COMPILE TIME. Instruction addresses that get a timing probe call.
//   PBD_FN_SKIPPABLE=0xb834c,...         COMPILE TIME. FUNCTION ENTRY addresses that get the probe call plus a
//                                           conditional early return (the equivalent of an immediate blr).
//   PBD_FN_SKIP=0xb834c,...              RUN TIME. Which of the skippable entries actually return immediately,
//                                           and only while the main PPU re-executes. Changing this list reuses the
//                                           same compiled objects because their emitted hooks do not change.
//   PBD_FN_REGS=29,3                     RUN TIME. Up to two register indices (0..31 = GPR, 32 = LR) recorded per
//                                           probe event. Registers are materialized before the call.
//   PBD_FN_MAX_EVENTS=0                  RUN TIME. Maximum captured raw events (default 0 = aggregates only).
//   PBD_PREPARE_FOR=lobby|watch          COMPILE TIME. A hidden run that only compiles: emit what a lobby set
//                                           (PBD_SET_CHANNEL) or a watcher (PBD_SPECTATOR_CHANNEL) emits, without
//                                           opening either channel.
//
// Each compiled object's name hashes the entries of the two COMPILE TIME lists its own code reaches (see
// pbd_ppu_object_name.h), so an object is never shared with a run that emits different hooks in it.
//
// Only the owner PPU (main_thread, id 0x01000000) is recorded and only the owner PPU can be skipped: another thread
// executing the same guest function is untouched.

#include <util/types.hpp>

#include "../PBD/pbd_stage_sync.h"

#include <atomic>
#include <map>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class ppu_thread;

namespace pbd::fn_hook
{
	enum class hook_action : u32
	{
		continue_guest = 0,
		skip = 1,
		fatal = 2,
	};

	// Compile-time side (read once per process, before the first PPU module is compiled).
	bool requested() noexcept;                            // any compile-time list non-empty
	bool observe_at(u32 address) noexcept;                // emit the probe call here
	bool skippable_at(u32 address) noexcept;              // emit the probe call and the conditional early return here
	const std::string& variant() noexcept;                // tag of the complete lists ("" when not requested)
	const std::vector<u32>& emitted_observers() noexcept; // sorted
	const std::vector<u32>& emitted_skippable() noexcept; // sorted, never also a probe
	// The immutable setting used by the runtime hook, not a second environment declaration.
	bool keyed_draws_enabled() noexcept;

	// Called by the main ELF loader before guest execution or compilation. Invalid configuration refuses boot.
	bool validate_configuration(const std::vector<std::pair<u32, u32>>& code_ranges);
	[[noreturn]] void reject_configuration_launch() noexcept;

	// Run-time side.
	// The generated code calls this at every probe point.
	hook_action event(ppu_thread& ppu, u32 address) noexcept;
	// OFF unless PBD_VOICE_CLAIM_LOCK=1: acquire BEFORE suspending guest CPUs.
	// A claim or restore timeout is a counted fault and schedules no-save emulation termination;
	// a restore never bypasses this gate.
	bool voice_restore_begin(ppu_thread& ppu) noexcept;
	void voice_restore_end() noexcept;
	// Snapshot before reading the ring's guest pages; inspect while the exclusive restore lease is held.
	u64 voice_checkpoint_token() noexcept;
	bool voice_restore_history_ok(u64 token, u32 target, u32 from) noexcept;
	bool voice_boundary_ok() noexcept;
	// The counts this run's stage reseeds are numbered by.
	pbd::stage_sync::epoch reseed_epoch() noexcept;
	// Bind an admitted live set before its held boundary 0 resumes. No guest state is written here.
	bool bind_match_seed(u32 seed) noexcept;
	// After all guest threads join, clear per-game hook, seed, gate, voice-lease and rank state for a resident restart.
	// Process settings stay; allocation failure returns false and forbids the next boot.
	bool reset_for_next_game() noexcept;
	// The same counts as the pair a takeover keeps and compares: match reseeds, then out-of-battle mob batches.
	inline std::pair<u32, u32> takeover_counters() noexcept
	{
		const auto epoch = reseed_epoch();
		return {epoch.match_seeds, epoch.mobs.count()};
	}
	// Apply the independently recorded game's logical seed epoch at a fresh owner boundary, before its stage load.
	bool begin_direct_match_epoch(ppu_thread& owner, u32 seed, u32 prior_matches) noexcept;
	// Test-only local input steering. Observes the selection work; never writes guest state.
	u16 steer_stage_sample(u32 role, u32 tick, u16 raw) noexcept;

	// The latch publishes the boundary it is in and whether this is a re-execution, so events can be attributed.
	inline std::atomic<u32> current_tick{0};
	inline std::atomic<u32> current_pass{0}; // 0 = live, 1 = re-executed
	// Session exit gate (latch storage::session_exit_gate): the first tick + 1 at which the gated screen's last readiness test found its slot
	// ready (0 = not yet; the latch resets it at the screen's first frame), and whether the latch holds that test failing in this tick.
	inline std::atomic<u32> gate_ready_tick{0};
	inline std::atomic<u32> gate_hold{0};
	// Watcher resource readiness: 0 = ordinary guest, 1 = retain scene 1, 2 = finish native loading for scene 2.
	inline std::atomic<u32> spectator_vs_init_gate{0};
	// Rematch's final scene-1 resource predicate shares the peers' existing loading exit gate.
	inline std::atomic<bool> rematch_vs_init_gate{false};
	// Stage sync: the mob controllers initialised outside the battle since the last match start (0xcb96c r3, in
	// creation order). Owner PPU thread only (the fn hook and the latch both run on it); the latch digests and clears it at match start.
	inline std::vector<u32> stage_controllers;
	// Draw census (LOG ONLY; PBD_SESSION_DRAW_CENSUS=1 together with the generator probes):
	// while draw_statistics_active (set by the latch at every barrier into a non-battle scene, cleared at the match-start reseed), every draw of
	// libc rand() (0x3afc3c), the flow-object LCGs (0xe859c, generator = the state address) and the Mersenne Twister (0x66950) is counted
	// per (generator, effective caller), wrappers (0x121de4, 0xe85c8, 0xe8674, 0x66b14..0x66db4, 0x162204) resolved to their callers,
	// with an FNV digest of the state each draw starts from. Owner PPU thread only; the latch writes and clears it at each barrier.
	inline std::atomic<u32> draw_statistics_active{1};
	// Keyed draws (PBD_SESSION_KEYED_DRAWS=1): inside the
	// same window as the census (from a menu barrier to the match-start reseed), every libc rand() draw of the owner thread returns a value
	// keyed only by (session seed, segment, effective caller, that caller's count in the segment): the hook sets the generator to the
	// srand(key) state before the stock routine runs. keyed_segment / keyed_counts are set and cleared by the latch at every segment start.
	inline std::atomic<u32> keyed_segment{0};
	inline std::map<u32, u32> keyed_counts;
	// Keyed-draw check: libc rand() calls of the owner thread inside the keyed window during a RE-EXECUTED frame.
	// Lockstep menus should keep this at 0; anything else is reported loudly (the keyed state would not be reproduced by the live pass).
	inline std::atomic<u64> keyed_in_replay{0};
	// Audio orphan detector: sys_ppu_thread_detach calls since boot (the game's only detach is
	// the voice-end callback at 0x33f8d0; a nonzero count means a feeder thread may be orphaned and a later join can hang).
	inline std::atomic<u32> thread_detaches{0};
	inline std::map<std::pair<u32, u32>, std::pair<u32, u64>> draw_statistics;
	void note_boundary(u32 tick, bool replay) noexcept;

	// Written after all emulation threads have joined, next to the session's other records.
	void write_report(const std::filesystem::path& directory) noexcept;
} // namespace pbd::fn_hook

// Generated-code ABI: 0 continues execution, 1 skips the function, and 2 contains a fatal fault.
extern "C" u32 ppu_pbd_fn_observe(ppu_thread* ppu, u32 address) noexcept;
