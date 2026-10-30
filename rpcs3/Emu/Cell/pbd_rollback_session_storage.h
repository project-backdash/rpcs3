#pragma once

// The update latch's state: one object per run, created by configure() and reached through m_storage. Included only
// by the latch's own source files.

#include "pbd_rollback_session_common.h"
#include "Emu/PBD/pbd_session_policy.h"
#include "pbd_rollback_session_storage_rollback.h"
#include "pbd_rollback_session_storage_peer.h"
#include "pbd_rollback_session_storage_lobby.h"
#include "pbd_rollback_session_storage_replay.h"
#include "pbd_rollback_session_storage_practice.h"

struct pbd::rollback_session::storage
{
	const pbd::session_policy::description launch = pbd::session_policy::read();
	u32 rollback_updates = 1;
	// Absolute tape indices. A capturing generation stops at capture_index entry;
	// a generation loaded from that capture resumes at start_index.
	u32 start_index = 0, capture_index = 0, inspection_index = 0;
	bool inspect_saved_boss = false;
	bool capture = false, inspect_checkpoint = false;
	std::vector<pbd::update_inputs> input_sequence;
	std::atomic<u32> published{0};
	std::atomic<u32> written_through{0};
	std::unique_ptr<pbd::session_runtime::trace_buffer> trace;
	std::atomic<bool> in_callback{false};
	std::atomic<u64> last_callback_ns{0}; // Freeze probe: time (since origin) of the newest hook callback
	std::atomic<u64> overlay_poll_ns{0};  // Newest completed menu poll while a boundary is deliberately held
	std::mutex diagnostic_mutex;
	pbd::session_runtime::boundary_diagnostic diagnostic;
	std::atomic<u32> reason{collecting};
	std::atomic<const char*> guard_failure{nullptr}; // Static guard name, published before the terminal reason.
	// Single selected PPU owns these until the active/in_callback drain barrier.
	bool update_active = false;
	u32 stage = 0;
	std::array<u32, 2> previous_held{};
	fs::file file;
	std::thread writer;
	session_clock::time_point origin;
	u64 generation = 0;
	pbd::sound_options::runtime_application sound_options;
	bool initial_validation_passed = false;
	std::filesystem::path status_directory;
	std::optional<bool> checkpoint_usable;
	bool header_attempted = false;
	bool header_written = false;
	const char* setup_failure = nullptr;
	std::string input_script_sha256;
	// Execution index for the next input update. published only advances over confirmed rows, so a
	// correction window stays unpublished until it is re-executed with authoritative inputs.
	u32 next_index = 0;
	// Rolling corrections from a delayed authoritative ledger: the update tape is the received history and
	// the input of tick t becomes known roll_delay ticks later. Unknown ticks in [roll_begin, roll_end) run with the last
	// confirmed input; at boundary T a mismatch at T-roll_delay restores that tick in place and re-executes it through T-1
	// (correction.at/correction.depth are set per correction). Rows are published only once no correction can re-execute them.
	bool rolling = false;
	u32 roll_delay = 0, roll_begin = 0, roll_end = 0, roll_corrections = 0;
	// The game's input ring as this PC last read it, per port (read_input_ring): the entry sample's port in a peer session,
	// each live port in Practice. A tap the input thread wrote into a slot overtaken before the read is kept, not lost.
	std::array<pbd::input_taps::ring_reader, 2> input_rings{};
	std::string stop_file; // PBD_STOP_FILE: the player asks for a graceful end; checked every 16 boundaries.
	// PBD_ROUND_TRANSITIONS=1 (observational): the one-round boundary contract is relaxed at a battle
	// counter reset - the consumer expectation is re-anchored, zero-tick boundaries and scene changes are recorded
	// instead of failing closed. Nothing else changes; rolling/ring semantics are unchanged.
	bool round_transitions = false;
	s32 consumer_offset = 0;
	// PBD_ROUND_LIFECYCLE=1 (production contract): dispatch index = monotonic network frame identity;
	// the game's input-side counter may restart at 0 at a phase change (expectation re-anchored); every boundary is
	// FIGHT (one battle tick) or NON-FIGHT (battle-side consumer frozen, pause clear); witness[1] == initial +
	// fight_count throughout; scenes 17/44 and 16/44 admitted. Anything else fails closed.
	bool round_lifecycle = false;
	static constexpr u32 settle_distrust_caps = 3;
	static constexpr u32 practice_sides_rows = 6;
	// Corrections are confined to fight stretches plus a grace of `delay` boundaries after
	// the last fight boundary (a predicted K.O. stays correctable). Beyond that, non-fight boundaries run in phase
	// LOCKSTEP: authoritative inputs only (peers hold for the remote tick), no prediction, no correction. Reason: in
	// non-fight stretches the frame handshake is driven by the render request thread on the shared frame object
	// 0x106d4a00, which a restore would rewind under it (the result is an RSX timeout).
	u32 last_fight_index = 0;
	// Owner-side publisher of the stats block the connection line reads (pbd_live_stats.h). live_stats is decided once per
	// process with PBD_NET_OVERLAY (pbd::live_stats::publishing()); false = nothing is ever written to the block.
	bool live_stats = false;
	pbd::live_stats::publisher live_pub;
	history_state history;
	checkpoint_state checkpoint;
	roundtrip_state roundtrip;
	correction_state correction;
	battle_clock_state battle_clock;
	undo_state undo;
	fault_state fault;
	peer_state peer;
	session_state session;
	lobby_state lobby;
	spectator_state spectator;
	match_replay_state match_replay;
	replay_state replay;
	takeover_state takeover;
	rewind_state rewind;
	practice_state practice;
	game_overlay_state game_overlay;
	usz history_slot(u32 index) const noexcept
	{
		return history.practice_continuous ? history.practice_window.slot(index) : index;
	}
	bool history_contains(u32 index) const noexcept
	{
		if (history.practice_continuous)
			return history.practice_window.owns(index);
		return index < extent() && (!history.session_history_rolling ||
									   pbd::session_history::buffer<u8, max_updates>::retained(index, published.load(std::memory_order_acquire)));
	}
	update_sample& sample_at(u32 index) noexcept
	{
		return history.samples[history_slot(index)];
	}
	tick_sample& tick_at(u32 index) noexcept
	{
		return history.ticks[history_slot(index)];
	}
	u8& match_at(u32 index) noexcept
	{
		return history.match_ticks[history_slot(index)];
	}
	u8& neutral_at(u32 index) noexcept
	{
		return history.input_neutral_ticks[history_slot(index)];
	}
	u8& start_executed_at(u32 index) noexcept
	{
		return history.session_start_executed_at[history_slot(index)];
	}
	// Guest input counters and the existing wire use signed 32-bit identities. Keep arithmetic below their safety margin.
	u32 extent() const noexcept
	{
		return history.session_continuous ? 0x7fff0000u : static_cast<u32>(input_sequence.size());
	}
	pbd::update_inputs input_at(u32 index) const noexcept
	{
		return index < input_sequence.size() ? input_sequence[index] : pbd::update_inputs{};
	}
	void extend_session_history(bool retain_all = false)
	{
		history.samples.resize(extent(), retain_all);
		history.ticks.resize(extent(), retain_all);
		history.entry_assigned.resize(extent(), retain_all);
		history.live_inputs.resize(extent(), retain_all);
		history.entry_sample.resize(extent(), retain_all);
		history.entry_sampled.resize(extent(), retain_all);
		history.live_fixed.resize(extent(), retain_all);
		history.peer_remote_value.resize(extent(), retain_all);
		history.peer_remote_arrival.resize(extent(), retain_all);
		history.session_start_suppressed.resize(extent(), retain_all);
		history.session_start_masked_at.resize(extent(), retain_all);
		history.session_start_executed_at.resize(extent(), retain_all);
		history.match_ticks.resize(extent(), retain_all);
		history.input_neutral_ticks.resize(extent(), retain_all);
		history.lockstep_ticks.resize(extent(), retain_all);
		if (match_replay.requested)
		{
			history.match_replay_observations.resize(extent(), retain_all);
			history.match_replay_observed.resize(extent(), retain_all);
		}
	}
	u32 history_retired() const noexcept
	{
		if (trace)
			return static_cast<u32>(peer.hash_checked + 1);
		return std::min(written_through.load(std::memory_order_acquire), static_cast<u32>(peer.hash_checked + 1));
	}
	// Nanoseconds since origin on the latch clock.
	u64 now_ns() const noexcept
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - origin).count());
	}
	// The hook callback's exit: the stamp first, because the freeze probe reads in_callback before last_callback_ns.
	void end_callback() noexcept
	{
		{
			std::unique_lock lock(diagnostic_mutex, std::try_to_lock);
			if (lock)
				diagnostic = {generation, diagnostic.sequence + 1, next_index, correction.at, correction.depth, roundtrip.repeat_active};
		}
		last_callback_ns.store(now_ns() | 1, std::memory_order_relaxed);
		in_callback.store(false);
	}
	bool practice_resume_settling() const
	{
		return practice.native.paused && practice.pause.resume_watch.in_flight();
	}
	pbd::practice::controls::pause_scene practice_pause_scene() const
	{
		return {session.in_match && practice.native.screen == 0 && practice.native.pause_valid, practice.native.flag,
			practice.native.paused, practice.native.menu_word,
			practice.menu.open || practice.dummy.state == dummy_state::confirm, practice.pause.requested,
			practice.pause.resume_native || practice.pause.resume_watch.in_flight()};
	}
};
