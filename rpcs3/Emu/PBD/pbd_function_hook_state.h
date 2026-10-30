#pragma once

#include "pbd_t5dr_memory.h"
#include "pbd_voice_health.h"

#include "pbd_session_policy.h"

#include "../Cell/pbd_fn_hook.h"
#include "../Cell/pbd_boot_request.h"
#include "../Cell/pbd_live_replay.h"
#include "../Cell/pbd_direct_match.h"
#include "../Cell/pbd_function_hook_config.h"
#include "../Cell/pbd_ppu_object_name.h"
#include "../Cell/pbd_skip_guard.h"
#include "../Cell/pbd_rollback_session.h"
#include "Emu/PBD/pbd_practice_presentation.h"
#include "../Cell/pbd_practice_native.h"
#include "Emu/PBD/pbd_replay_viewer_menu.h"
#include "pbd_character_rule_native.h"
#include "pbd_env.h"
#include "pbd_replay_entry.h"
#include "pbd_pick_transition.h"
#include "pbd_rank_screens.h"
#include "pbd_replay_carry.h"
#include "pbd_replay_standby.h"
#include "../Cell/pbd_game_lifecycle.h"
#include "../Cell/pbd_owner_timeline.h"
#include "pbd_rematch.h"
#include "../Cell/PPUThread.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/Memory/vm.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/pbd_frame_presentation.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/lv2/sys_semaphore.h"
#include "Emu/Cell/lv2/sys_timer.h"
#include "Emu/Memory/vm_locking.h"

#include "Utilities/File.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>
#include "util/sysinfo.hpp"

LOG_CHANNEL(pbd_fn_log, "PBD fn hook");

void pbd_dump_ppu_jit_map(const std::string& path); // PPUThread.cpp

namespace pbd::fn_hook
{
	using hook_clock = std::chrono::steady_clock;
	namespace detail
	{
		struct event_row
		{
			u32 address;
			u32 tick;
			u32 flags; // bit 0 re-execution, bit 1 the hook answered "skip"
			u64 ns;
			u64 reg0;
			u64 reg1;
			u64 cyc; // PBD_OWNER_TIMELINE=1 only: the owner's host thread cycles at this event
		};

		// Voice leases are shared by guest claimers and checkpoint restores; they own only their own state.
		struct voice_state
		{
			pbd::voice_health::monitor health; // Protected by lease_mutex; never restored with guest memory.
			// Widens the game's free-check/claim window; test-only, zero by default.
			u64 test_delay_us = 0;
			std::atomic<u64> test_delays{0}, test_stale{0};
			// Test rendezvous: hold exactly one foreign claimer after it owns the host critical section.
			u64 test_rendezvous_us = 0;
			u32 test_rendezvous_tick = 0;
			std::atomic<bool> test_rendezvous_armed{true};
			std::atomic<u64> test_rendezvous_entered{0}, test_rendezvous_released{0};
			bool lock = false;
			u64 timeout_us = 250000;
			// Diagnostic containment is separately opt-in. Its watchdog terminates the host process if the authenticated
			// no-save main-thread stop has not completed by the bounded deadline.
			bool fatal_timeout = false;
			bool test_drop_main_stop = false;
			u64 fatal_deadline_ms = 2000;
			std::atomic<bool> fatal_armed{false};
			std::atomic<u32> fatal_thread{0}, fatal_instruction{0};
			std::atomic<u64> fatal_generation{0};
			std::atomic<u64> fatal_outcomes{0}, fatal_late_events{0}, fatal_watchdog_exits{0};
			std::atomic<u64> main_stop_dropped{0};
			// Low bits count ALL active claims (including fail-open ones); high bit excludes claims during a restore.
			std::atomic<u32> activity{0}, owner{0};
			std::atomic<u64> claims{0}, contentions{0}, timeouts{0}, faults{0}, fail_stops{0}, restores{0}, restore_refused{0};
			std::atomic<u64> restore_waits{0}, foreign_claim_rewound{0}, balance_errors{0};
			// Short host-only bookkeeping sections; never held across guest code, sleeps, or restore writes.
			std::mutex lease_mutex;
			u32 foreign_active = 0;
			u64 foreign_sequence = 0;
			struct marker
			{
				u64 sequence;
				u32 thread, tick, replay, kind, slot;
			};
			std::vector<marker> foreign_events;
		};
		voice_state& voice_runtime() noexcept;
		std::optional<hook_action> voice_actions(ppu_thread& ppu, u32 address);

		struct random_state
		{
			// PBD_FN_RNG_REPAIR names a task whose post-call generator state is recorded live and restored when skipped
			// during re-execution, preserving its gameplay draws. PBD_FN_RNG_EXIT closes the task at the dispatcher return.
			u32 rng_repair = 0, rng_exit = 0x25b5fc, rng_offset = pbd::t5dr::random_tls::seed_before_r13;
			bool rng_inside = false;
			u32 rng_inside_tick = 0;
			// Last reported repair tick; another skip at that index has no unambiguous live-state mapping.
			u32 rng_window_tick = 0xffffffffu;
			// The latch digests the generator as the seed word at r13 - 0x7000 plus the 0x8c-byte TLS tail at
			// r13 + 0x10000 - 0x6f6c, so the repair restores exactly those two spans.
			static constexpr u32 rng_tail_offset = pbd::t5dr::random_tls::flag_after_r13, rng_tail_size = pbd::t5dr::random_tls::tail_bytes;
			struct rng_slot
			{
				u32 tick = 0xffffffffu;
				u32 seed = 0;
				std::array<u8, rng_tail_size> tail{};
			};
			std::array<rng_slot, 256> rng_after{}; // tick -> generator state the live pass left behind
			u64 rng_recorded = 0, rng_applied = 0, rng_missing = 0;
			// PBD_FN_RNG_FENCE / PBD_FN_RNG_FENCE_EXIT pair probed entries and returns. Saving and restoring the 137-byte
			// TLS generator around each call removes its draws in both passes, changing the stock sequence on every peer.
			std::vector<std::pair<u32, u32>> fence;
			// PBD_MATCH_SEED replaces the frame counter before the stock match-start reseed at 0x15a88 (r26: VS 2, arcade 8).
			// Both peers derive it from the session seed and match number; round-start reseeds use their shared counter unchanged.
			bool match_seed_set = false;
			u32 match_seed_base = 0;
			bool direct_match_epoch_applied = false;
			// PBD_SESSION_STAGE_SYNC reseeds libc rand() at scene 12's exit probe (0x52298), before stage objects draw from it.
			// The seed comes from PBD_MATCH_SEED and the match number, so loading history cannot change those draws.
			bool stage_sync = false;
			// Mobs created during loading precede the exit reseed and share rand() with per-frame particles. Each batch's
			// first controller init (0xcb96c) uses the session seed, match number and batch number instead of the loading tick.
			pbd::stage_sync::epoch epoch;
			bool draw_diagnostics = false;
			bool keyed_draws = false;
			u64 keyed = 0;
			struct draw_frame
			{
				u32 start = 0, end = 0, lr = 0, r1 = 0;
			};
			std::vector<draw_frame> draw_stack;
			// PBD_FN_RNG_AUDIT records live generator changes at the entry's return address and stack pointer without guest writes.
			// Statistics are grouped by entry and match; unwound calls are unmatched and return sites are recorded per entry.
			std::vector<u32> rng_audit;
			struct audit_frame
			{
				u32 entry = 0, r1 = 0, lr = 0, tick = 0;
				std::array<u8, 137> tls{};
				bool read = false;
				u32 drew = 0;
			};
			std::vector<audit_frame> audit_stack;
			struct audit_stat
			{
				u64 calls = 0, matched = 0, unmatched = 0, advanced = 0, steps = 0, max_steps = 0, drew_rand = 0, drew_lcg = 0, drew_mt = 0;
				u32 first_advanced_tick = ~0u;
			};
			// Count battle-scene Mersenne Twister draws by caller: the ambient-sound manager seeds it from host data.
			std::map<u32, u64> mt_in_fight;
			// A call advances when its TLS generator changes or a probed rand/LCG/MT core runs inside its bracket.
			std::map<std::pair<u32, u32>, audit_stat> audit_stats;
			std::map<u32, std::set<u32>> audit_returns;
			u64 audit_overflow = 0, audit_unreadable = 0;
			static constexpr u32 gen_seed_at = pbd::t5dr::random_tls::seed_before_r13; // subtracted from r13
			static constexpr u32 gen_flag_at = pbd::t5dr::random_tls::flag_after_r13;  // +0x9094: flag byte, then (at +4) the last word and the table
			u32 fence_depth = 0, fence_entry_open = 0;
			u64 fence_sp = 0;
			u32 saved_seed = 0, saved_last = 0;
			u8 saved_flag = 0;
			std::array<u32, 32> saved_table{};
			u64 fence_opened = 0, fence_restored = 0, fence_idle = 0, fence_faults = 0;
			std::array<u64, 66> fence_draws{}; // [n] = fences whose code advanced the LCG n times; [65] = more than 64
		};
		random_state& random_runtime() noexcept;

		struct config
		{
			const pbd::session_policy::description launch = pbd::session_policy::read();

			std::vector<u32> observed_sites; // compile time: probe only
			std::vector<u32> skippable;      // compile time: probe + conditional early return
			std::vector<u32> skip;           // run time: which skippable entries actually return immediately
			// PBD_FN_SKIP_UNLESS=entry:address[,...] permits a skip only when the corresponding guest word is readable and zero.
			std::vector<std::pair<u32, u32>> skip_unless;
			u64 skip_vetoed = 0;
			// PBD_FN_SKIP_GATE=player_draw keeps 0x2649a8's RNG-consuming model path (r24 == 4) live. Its source value is
			// [this+0x50] or (s16)[[[0x469dbc]+4]+0x12] by vtable; value 3, an unknown vtable or unreadable data vetoes the skip.
			bool gate_player_draw = false;
			// PBD_FN_SKIP_GATE=stage_camera keeps 0x260d44 live when [0x10053694] is 2 or unreadable: mode 2 writes camera
			// cache keys in render context 0x10749a00 that the next live frame may read before drawing.
			// Ominous Mist also retains the stage layer's fog updates; an unreadable stage vetoes the skip.
			bool gate_stage_camera = false;
			u64 gate_stage_skipped = 0, gate_stage_vetoed = 0;
			// PBD_TEST_STAGE_CAMERA_FORCE=N vetoes every N-th re-executed camera draw skip to exercise the original call.
			u64 test_stage_camera_force = 0, test_stage_camera_calls = 0, test_stage_camera_forced = 0;
			u64 gate_stage_live_mode2 = 0;
			u32 gate_stage_first_mode2_tick = 0xffffffffu; // live calls that would be vetoed (discovery)
			u64 gate_player_skipped = 0, gate_player_vetoed = 0;
			// PBD_FN_SKIP_GATE=effects_draw permits skipping 0x9f8d0 only when its kill path cannot run: verdict 1 = kill
			// table value 2, 2 = unknown vtable, 3 = invalid index or unreadable data; 0 permits the skip.
			bool gate_effects_draw = false;
			u64 gate_effects_skipped = 0, gate_effects_vetoed = 0, gate_effects_live_calls = 0;
			std::array<u64, 4> gate_effects_live_would_veto{}, gate_effects_veto_reason{};
			u64 gate_player_live_calls = 0;
			std::array<u64, 4> gate_player_live_would_veto{}, gate_player_veto_reason{};
			u32 gate_player_first_live_veto_tick = 0xffffffffu;
			// Dispatch sites pair a probed instruction with its vtable byte offset (exec 0x08; draw/cull 0x0c/0x10).
			// resolve_task uses r3's node to identify the guest entry; PBD_FN_SKIP_TASKS names entries eligible for skipping.
			std::vector<std::pair<u32, u32>> dispatch;
			std::vector<u32> skip_tasks;
			bool test_early_flush = false;
			// PBD_EARLY_FLUSH_LIVE publishes PUT at the 0x34b9d8 label handshake before the guest waits for vblank.
			// This requires sys_rsx.cpp's counted flip completions to publish each frame without losing its completion.
			bool early_flush_live = false;
			u64 test_early_flushes = 0, test_early_flush_refused = 0;
			std::atomic<u64> latest_flip_tag_sequence{0};
			// PBD_SESSION_BLOCKING_LOADS waits up to three seconds at the worker semaphore (0x9a884) and decoder status
			// (0x1ebe1c) polls outside battle, keeping loading frames and stage simulation independent of host speed.
			bool blocking_loads = false;
			// Session exit gate: the node whose state word is masked for the current readiness test (0 = none).
			u32 gate_node = 0;
			u32 steer_stage = 0, select_work = 0, select_players = 0, steer_last_cursor = ~0u, steer_last_mode = ~0u;
			bool skip_guard = false, skip_guard_record = false, skip_guard_test_taint = false, skip_guard_test_missing = false;
			bool skip_guard_subset = false;
			std::vector<u32> guard_entries;
			skip_guard_history guard_history;
			std::string tag;
			int reg0 = -1, reg1 = -1;
			u64 max_events = 0;

			std::vector<event_row> events;
			usz used = 0;
			bool overflow = false;
			bool origin_set = false;
			hook_clock::time_point origin{};
			u64 calls = 0, skips = 0, skip_ignored = 0;
			enum class report_outcome
			{
				unattempted,
				attempted,
				succeeded,
				failed
			};
			std::atomic<report_outcome> report{report_outcome::unattempted};
			std::atomic<u64> foreign{0};

			config();
			void configure_observed_sites();
			void configure_skippable();
			void configure_skip_guard();
			void configure_runtime_skips();
			void configure_registers();
			void configure_dispatch();
			void configure_random_state();
		};
		void check_code_addresses(const config& c, const std::vector<std::pair<u32, u32>>& code_ranges, std::vector<std::string>& errors);
		config& cfg() noexcept;
		std::string env_text(const char* name);
		u64 parse_number(const char* name, u64 fallback);
		extern std::vector<std::string> configuration_errors;
		extern std::vector<std::pair<std::string, std::vector<u32>>> supplied_addresses;
		hook_action voice_enter(ppu_thread& ppu) noexcept;
		void voice_leave(ppu_thread& ppu) noexcept;
		extern thread_local u32 voice_slot;
		void trace_voice(ppu_thread& ppu, u32 address);
	} // namespace detail
} // namespace pbd::fn_hook
