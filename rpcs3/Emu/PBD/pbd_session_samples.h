#pragma once

#include "pbd_checkpoint_write_plan.h"
#include "pbd_session_trace.h"

namespace pbd::session_runtime
{
	using session_clock = std::chrono::steady_clock;
	// The values travel in the end message to the other peer (end message offset 20): new reasons are appended, never inserted.
	// lobby_set_ended (11): the lobby ended the set through the set channel (end_set), which blames neither player.
	enum finish_reason : u32
	{
		collecting,
		update_limit,
		guard_failed,
		setup_failed,
		emulation_stopped,
		hook_invalidated,
		output_failed,
		user_stopped,
		match_ended,
		left_vs_mode,
		takeover_relaunch,
		lobby_set_ended
	};
	enum class replay_control_result : u8
	{
		go,
		stopped,
		restored
	};
	// Fixture expectations: the retained arcade fixture (fixed character pair, P1 vs CPU)
	// has both counters at 0x4e0, the player hash below and battle scene 17/44 (16/44 admitted under the round lifecycle).
	// A different start fixture (the two-player VS fixture) declares its own values through PBD_FIXTURE_INPUT_COUNTER,
	// PBD_FIXTURE_BATTLE_COUNTER, PBD_FIXTURE_PLAYER_SHA256 and PBD_FIXTURE_SCENES ("a/b,c/d": the first pair is
	// the battle scene the fixture starts in, later pairs are admitted only under the round lifecycle). Nothing else
	// about the contract changes; every check still fails closed against the declared values.
	struct initial_state_expectations
	{
		u32 input_counter = 0x4e0;
		u32 battle_counter = 0x4e0;
		std::string player_sha256 = "8d3e9dab0b73143d6806e79bf81aa6307993a04cdf2e8f6a3ad8f5657d56d68e";
		std::vector<std::pair<u32, u32>> scenes{{17, 44}, {16, 44}};
		std::string source = "default_arcade";
	};
	inline initial_state_expectations& initial_state() noexcept
	{
		static initial_state_expectations value;
		return value;
	}
	inline constexpr u32 counter_address = 0x104cce40;
	// The game's input ring (pbd::input_taps::ring_reader): 16 big-endian words per port, port 1's 0x20 bytes after port 0's,
	// and the count of slots written, which the input thread advances after writing both ports' words.
	inline constexpr u32 input_ring_address = 0x104ccdfc, input_ring_written_address = 0x104cce3c;
	inline constexpr u32 masks_address = 0x10099fac;
	// Session buttons in the game's canonical 16-bit word (digital1 in the high byte).
	inline constexpr u16 session_select_bit = 0x0100, session_start_bit = 0x0800;
	// Start + Select held for this many admitted local samples (2 s at 60 per second) ends the session (filter_local_sample).
	inline constexpr u32 session_leave_hold_ticks = 120;
	// The VS loop passes scene 0 for one boundary between screens; the title screen and main menu stay in it. This many
	// consecutive scene-0 boundaries (5 s) means the game left VS mode.
	inline constexpr u32 session_left_vs_boundaries = 300;
	inline constexpr u64 frame_period_us = 16667; // one 60 Hz game frame
	inline constexpr u32 mapping_pointer = 0x468628;
	inline constexpr u32 mapping_base = 0x100500cc;
	inline constexpr std::array<u32, 6> stage_pcs{pbd::t5dr::boundary_pc, 0x12f80, 0x12ec0, 0x12f80, 0x12ec0, 0x12efc};
	struct session_error
	{
		const char* message;
	};

	struct port_sample
	{
		bool input_read_seen = false;
		bool injected = false;
		bool post_seen = false;
		bool stored_held_read = false;
		bool stored_rising_read = false;
		bool posted = false;
		u64 original_r3 = 0;
		u16 injected_r3 = 0;
		u64 raw = 0;
		u64 effective = 0;
		u64 held = 0;
		u64 rising = 0;
		u32 stored_held = 0;
		u32 stored_rising = 0;
	};

	struct update_sample
	{
		bool entry_seen = false;
		bool before_read = false;
		bool after_read = false;
		bool completed = false;
		u32 stage = 0;
		u32 last_pc = 0;
		u64 host_start_ns = 0;
		u64 host_end_ns = 0;
		u64 hook_duration_ns = 0;
		u32 before_counter = 0;
		u32 after_counter = 0;
		u64 entry_r1 = 0;
		std::array<u16, 2> raw{};
		pbd::session_start::phase start_phase{};
		const char* failure = nullptr;
		const char* soft = nullptr; // Round-transition mode: first recorded (non-fatal) guard of this dispatch.
		std::array<port_sample, 2> ports{};
	};

	// One-tick witnesses: battle counters written only inside the battle update 0x15458
	// (0x1008f05c, 0x1008ebe4 = consumer, round timer 0x1008f054, 0x1008f0f0) and its pause flag.
	inline constexpr std::array<u32, 5> battle_counter_addresses{0x1008f05c, 0x1008ebe4, 0x1008f054, 0x1008f0f0, 0x1008f0dc};
	// Canonical combat state hashed at every tick boundary: player records, battle state, input history/masks.
	inline constexpr std::array<std::pair<u32, u32>, 3> combat_regions{{{replay_memory::actors, replay_memory::actor_records_bytes},
		{pbd::t5dr::game_mode_address, 0xc54}, {0x10099f80, 0x80}}};
	// Combat hash v3: region 0 leaves out each fighter's bone-root pointer, whose object two peers may hold at different
	// addresses (pbd_t5dr_memory.h actor_bone_root). The raw bytes stay in the mismatch bundles.
	inline constexpr std::array<std::pair<u32, u32>, 2> combat_pointer_spans{
		{{replay_memory::actor_bone_root, replay_memory::actor_bone_root + 4},
			{replay_memory::actor_stride + replay_memory::actor_bone_root,
				replay_memory::actor_stride + replay_memory::actor_bone_root + 4}}};
	inline constexpr bool combat_pointer_excluded(u32 region, u32 offset) noexcept
	{
		if (region != 0)
			return false;
		for (const auto& [begin, end] : combat_pointer_spans)
			if (offset >= begin && offset < end)
				return true;
		return false;
	}
	// 64 KiB neighbourhoods written by effect-active in-place corrections.
	// Candidate effect-object memory, hashed per boundary so a correction that
	// preserves canonical combat state but loses a visible effect can be attributed. Not an established structure.
	inline constexpr std::array<u32, 2> effect_candidate_regions{0x10650000, 0x10740000};

	struct tick_sample
	{
		bool read = false;
		bool checked = false;
		bool one_tick = false;
		bool held = false;
		u64 hold_ns = 0;
		bool round_reset = false;   // Round-transition mode: this boundary re-anchored the consumer expectation.
		bool fight = false;         // Round lifecycle: this boundary was exactly one battle tick (battle-side consumer stepped).
		bool lockstep = false;      // This boundary ran in phase lockstep (no prediction, no correction).
		u32 fight_count = 0;        // Round lifecycle: battle ticks since the run start; witness[1] == fixture().battle_counter + fight_count.
		std::array<u32, 2> scene{}; // Scene selectors read at this boundary (17/44 is the in-battle pair).
		u32 frame_counter = 0;      // Game-flow frame counter 0x1008f07c at this boundary (the match / round reseed source).
		u32 session_segment = 0, gate_hold = 0;
		std::array<u32, 5> battle_counters{};
		bool combat_read = false;
		std::array<u64, 3> combat{}; // FNV-1a 64 of each combat_regions entry at this boundary (final execution), excluding audio spans.
		u64 audio_bookkeeping = 0;   // FNV-1a 64 of the excluded sound-engine spans of region 0 (reported, never enforced).
		bool hash_relaxed = false;   // Non-fight boundary under the round lifecycle: audio/intro spans and the TLS tail left out.
		// Diagnostic, read on the owner PPU at this boundary (state before tick index executes): the generator at
		// 0x3afc3c keeps its state in the calling thread's TLS, seed u32 at r13-0x7000 and init flag/last/32-entry table at
		// r13+0x10000-0x6f6c (0x8c bytes). digest = FNV-1a 64 over seed bytes then those 0x8c bytes; countdown = u16 at
		// players+0x2d6 (0x10090316).
		bool generator_read = false;
		u32 generator_base = 0, generator_seed = 0;
		u64 generator_digest = 0;
		bool countdown_read = false;
		u16 countdown = 0;
		u64 battle_time = 0, battle_reads = 0; // Battle clock value for this tick and cumulative policy reads
		// Sampled at the boundary because trace rows are formatted later.
		u32 battle_clock_version = 0;
		u64 battle_frame_samples = 0;
		// Diagnostic, snapshotted at this boundary (state before tick index executes): load monitor
		// table T entry 12 at 0x100538bc (state, start threshold 100, end threshold 95, index 12) and the game's
		// frame-start timestamp global 0x1074f9f8. State 2 means entry 12 reports overload and objects are pruned.
		bool monitor_read = false;
		u32 monitor_state = 0, monitor_start = 0, monitor_end = 0, monitor_index = 0;
		// 64-bit guest timebase at 0x1074f9f8. At an ordinary boundary t this equals anchor + (t + 1) * F / 60.
		u64 frame_start = 0;
		bool frame_start_adapted = false; // the correction adapter wrote this boundary's logical frame start
		// Diagnostic, snapshotted at this boundary: FNV-1a 64 over the two 64 KiB neighbourhoods that
		// effect-active in-place corrections write (around
		// 0x10652000..0x10657000 and 0x10743000..0x10748000). These are CANDIDATE effect-object regions, not an
		// established effect structure: the canonical combat hashes cover players/battle state/input masks only, so
		// effect-object state is otherwise unobserved and "simulation restored" cannot be claimed for it.
		bool effect_read = false;
		std::array<u64, 2> effect{};
		// Localization: per-PAGE FNV-1a 64 over the same two 64 KiB regions (16 pages each). Only three
		// pages per region are ever undone by a correction, so per-page hashes say whether a residual divergence sits in
		// a page the rolling undo touched (undo incomplete) or in one it never saw (a writer the undo does not observe).
		std::array<u64, 32> effect_pages{};
		bool ring_captured = false;
		u64 ring_capture_ns = 0;
		u64 ring_bytes = 0, rtt_us = 0;
		u32 rollback_depth = 0;
		// The parts of the per-tick checkpoint capture, so its cost in every live tick can be attributed.
		u64 ring_pages_ns = 0, ring_context_ns = 0;
		u32 ring_calls = 0; // vm::try_read_stream calls for serial capture, or caller/helper shards for parallel capture.
		u32 faults = 0;     // Write-fault capture: faults handled since the previous boundary (any thread) ...
		u64 fault_ns = 0;   // ... and the time spent inside the handler for them.
	};

	struct trace_snapshot
	{
		u32 index = 0;
		update_sample row{};
		tick_sample tick{};
		bool in_match = false;
	};
	using trace_buffer = pbd::session_trace::buffer<trace_snapshot>;

	// Capture-only probe: liblv2 lightweight mutexes and the sys_mutex the main PPU
	// uses every tick. Observation only; nothing is restored.
	inline constexpr std::array<u32, 15> sampled_lwmutexes{0x106d4a50, 0x106d4d00, 0x1073d4a0, 0x1073de00, 0x1073f480, 0x1073f780, 0x10742380,
		0x107499a0, 0x10749c38, 0x10749d18, 0x1074bea8, 0x1074bee0, 0x1074d388, 0x1074d498, 0x10752088};
	inline constexpr u32 sampled_mutex_id = 0x85006d00;

	// Round trip. Spans inside the tick write set that other threads write are never restored:
	// keyread ring and counter, and spans changed while the owner was held.
	// 0x104ccdf8..0x104cce40: keyread's port 0 and port 1 rings and keyread counter.
	inline constexpr const auto& roundtrip_exclusions = pbd::checkpoint_memory::default_exclusions;

	struct owner_context
	{
		std::array<u64, 32> gpr{};
		std::array<f64, 32> fpr{};
		std::array<v128, 32> vr{};
		u32 cr = 0, vrsave = 0;
		u64 lr = 0, ctr = 0;
		std::array<u8, 4> xer{};
	};

	inline owner_context read_context(const ppu_thread& ppu) noexcept
	{
		owner_context c;
		std::memcpy(c.gpr.data(), ppu.gpr, sizeof(ppu.gpr));
		std::memcpy(c.fpr.data(), ppu.fpr, sizeof(ppu.fpr));
		std::memcpy(c.vr.data(), ppu.vr, sizeof(ppu.vr));
		c.cr = ppu.cr.pack();
		c.vrsave = ppu.vrsave;
		c.lr = ppu.lr;
		c.ctr = ppu.ctr;
		c.xer = {u8{ppu.xer.so}, u8{ppu.xer.ov}, u8{ppu.xer.ca}, ppu.xer.cnt};
		return c;
	}

	inline void write_context(ppu_thread& ppu, const owner_context& c) noexcept
	{
		std::memcpy(ppu.gpr, c.gpr.data(), sizeof(ppu.gpr));
		std::memcpy(ppu.fpr, c.fpr.data(), sizeof(ppu.fpr));
		std::memcpy(ppu.vr, c.vr.data(), sizeof(ppu.vr));
		ppu.cr.unpack(c.cr);
		ppu.vrsave = c.vrsave;
		ppu.lr = c.lr;
		ppu.ctr = c.ctr;
		ppu.xer.so = c.xer[0] != 0;
		ppu.xer.ov = c.xer[1] != 0;
		ppu.xer.ca = c.xer[2] != 0;
		ppu.xer.cnt = c.xer[3];
	}

	inline std::string context_differences(const owner_context& a, const owner_context& b)
	{
		std::string out;
		const auto add = [&](const std::string& name)
		{
			if (!out.empty())
				out += ',';
			out += '"' + name + '"';
		};
		for (u32 i = 0; i < 32; i++)
		{
			if (a.gpr[i] != b.gpr[i])
				add(fmt::format("gpr%u", i));
			if (std::memcmp(&a.fpr[i], &b.fpr[i], sizeof(f64)))
				add(fmt::format("fpr%u", i));
			if (std::memcmp(&a.vr[i], &b.vr[i], sizeof(v128)))
				add(fmt::format("vr%u", i));
		}
		if (a.cr != b.cr)
			add("cr");
		if (a.vrsave != b.vrsave)
			add("vrsave");
		if (a.lr != b.lr)
			add("lr");
		if (a.ctr != b.ctr)
			add("ctr");
		if (a.xer != b.xer)
			add("xer");
		return out;
	}

	// Checkpoint ring entry for one tick boundary.
	struct ring_entry
	{
		bool valid = false;
		u32 index = 0, counter = 0;
		u64 voice_checkpoint_token = 0;
		// Round lifecycle: the input-side counter expectation in effect at this boundary. A phase change re-anchors it at
		// the closing boundary BEFORE that callback performs a correction, so it must travel with the checkpoint
		// (otherwise a restore to an earlier boundary re-executes the ticks before the phase change under the new offset).
		s32 consumer_offset = 0;
		u32 last_fight_index = 0; // Restored with the checkpoint for phase lockstep decisions.
		std::vector<u8> pages;
		owner_context context{};
		std::array<u32, 2> previous_held{};
		// Owner timebase reads made while executing this tick (replayed on correction).
		std::vector<std::pair<u32, u64>> clock;
		// Pages outside the static write set first stored into by the main PPU during this tick, with their prior bytes.
		// Evidence per undo page: ppu.cia (chained static handlers update it only at chain start, so approximate) and ppu.lr.
		std::vector<u32> undo_pages, undo_pcs, undo_lrs;
		std::vector<u32> skip_pages, skip_pcs; // First stored by library (PRX/LLE) code this tick: never undone (evidence).
		std::vector<u32> foreign_pages;        // Write-fault capture: first written this tick by a thread other than the owner.
		std::vector<u8> undo_bytes;
		bool undo_overflow = false;
	};

	// Host state sampled before replay round tracking at a first dispatch. Restoring this slot and redispatching the
	// boundary makes the latch take the same host-side decisions it took the first time.
	struct takeover_host_slot
	{
		pbd_replay_cursor pbd_cursor{};
		s32 consumer_offset = 0;
		u32 last_fight_index = 0;
		std::array<u32, 2> previous_held{};
		u32 replay_round = 0, replay_round_start = 0;
		bool replay_was_fighting = false, replay_taken = false;
		u32 replay_control_port = 0;
		u32 session_segment = 0, session_segment_index = 0, session_segment_scene = 0;
		u32 session_prev_segment_index = 0, session_prev_segment_scene = 0, session_prev_segment_counter = 0;
		u32 session_segment_counter = 0;
		std::array<u32, 2> session_last_scene{};
		bool session_last_scene_valid = false, session_in_match = false;
		u32 session_matches = 0, session_match_start = 0, session_scene0_since = ~0u, session_pending_end = 0;
		u32 battle_counter = 0;
		u16 replay_pad = 0, replay_pad_prev = 0;
		bool replay_start_clean = false;
		u32 session_leave_hold = 0, session_leave_next_source = 0;
		bool session_leave_start_held = false;
		usz takeover_rounds_size = 0;
	};

	struct takeover_anchor
	{
		std::shared_ptr<const pbd::practice::native::frame_state> frame_state;
		bool valid = false, overflow = false;
		bool practice_native_controlled = false, practice_native_getup = false;
		std::array<u32, 2> practice_native_action{};
		std::array<u32, 2> practice_native_written{10, 10};
		u32 index = 0, match = 0, frame = 0, segment = 0;
		std::array<u32, 2> scene{};
		u32 battle_counter = 0;
		u64 battle_anchor = 0, fault_remaps = 0, lifecycle_syscalls = 0;
		std::pair<u32, u32> fn_counters{};
		// The pinned takeover boundary is represented by the same checkpoint type as a rollback-ring boundary. The
		// long-lived journal below only replaces the ring's bounded per-tick undo storage; restore and resume consume
		// this checkpoint exactly like the production correction path consumes a ring entry.
		ring_entry checkpoint{};
		takeover_host_slot host{};
		std::vector<u32> journal_pages;
		std::vector<u8> journal_bytes;
		std::unordered_map<u32, usz> journal_index;
	};

} // namespace pbd::session_runtime
