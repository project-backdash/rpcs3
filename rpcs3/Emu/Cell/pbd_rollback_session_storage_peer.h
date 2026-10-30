#pragma once

// The peer link's and the session's state. Members of pbd::rollback_session::storage
// (pbd_rollback_session_storage.h).

#include "pbd_rollback_session_common.h"
#include "../PBD/pbd_peer_key_pipe.h"
#include "../PBD/pbd_peer_input_trace.h"
#include "../PBD/pbd_peer_log.h"
#include "../PBD/pbd_peer_recovery.h"
#include "../PBD/pbd_set_publication.h"

namespace pbd::session_runtime
{
	struct rx_packet
	{
		u64 arrival_us = 0;
		u64 auth_epoch = 0;
		pbd::peer_endpoint::endpoint source{};
		u16 size = 0;
		std::array<char, pbd::peer_auth::maximum_bytes> bytes{};
	};
	// SINGLE-INSTANCE SESSION REPLAY: PBD_SESSION_REPLAY=<file>, made from the logs
	// of a two-peer session. Without a peer the latch still starts a new segment at every scene change
	// (outside a match or into / out of one, like the barrier) and applies the session's per-segment menu sync (frame counter from the
	// file's "sync" lines, the generator reseed from the session seed), and both ports take the inputs the game saw in that session BY
	// SEGMENT AND OFFSET ("seg <segment> <offset> <p1> <p2>" change points), so loading screens of a different length on this host do not
	// shift the picks or the fights. Single instance only (no peer); the other replay settings are the session's (PBD_MATCH_SEED, stage sync).
	// MISMATCH BUNDLE: a peer keeps the final bytes of the three combat regions of its last 128 ticks (index
	// tagged) and, when a confirmed-state mismatch is detected, writes those of the failing tick and the 16 before it to
	// mismatch-<tick>.bin (DRMM0001: tick, then per kept tick: index, region 0 / 1 / 2 bytes), so both peers leave the differing bytes
	// behind; a replay of the session on one host does not always reproduce a mismatch, so the bytes of the failing run are the evidence.
	struct region_slot
	{
		u32 tag = 0;
		std::array<u8, 0x11a0> r0{};
		std::array<u8, 0xc54> r1{};
		std::array<u8, 0x80> r2{};
	};
	// The peer link: transport, pacing, confirmed hashes and input delay.
	struct peer_state
	{
		// Peer mode: this process owns pad port peer.role; the other port's inputs arrive only over UDP.
		// Inside [roll_begin, roll_end) a received tick t is usable at boundaries >= t + peer.delay (rollback window); outside
		// it the peers run lockstep (hold until the remote tick has arrived).
		bool enabled = false;
		bool use_arrived = false; // PBD_PEER_USE_ARRIVED=1: an arrived remote value is usable at once
		u64 wait_ns = 0;          // PBD_PEER_WAIT_MS: wait this long for the current tick's remote input before predicting
		u32 input_delay = 0;      // PBD_PEER_INPUT_DELAY: owned input of tick t = device read of tick t-1-D, published D ahead
		// Pacing.
		s32 remote_newest = -1; // newest remote tick received so far
		u32 wait_skipped = 0;
		u32 wait_corrected_at = ~u32{0}; // boundary whose peer correction was triggered: its post-correction pass does not wait
		s32 remote_confirmed = -1;       // Newest tick the OTHER peer reports as confirmed (it holds all of this peer's ticks up to it)
		u64 remote_confirmed_us = 0;     // when that report last moved (or the first datagram arrived)
		u32 resend_toggle = 0;
		// Bound freshness waits around the next datagram's expected arrival; PBD_PEER_WAIT_DUE=0 uses the full wait.
		bool wait_due = true;
		// Phase delay (PBD_PEER_PHASE_DELAY, default on): docs/design/netcode-phase-delay.md.
		bool phase_delay = true;
		u32 lock_delay = 0; // floor of L within the current lockstep phase (0 outside one)
		bool lock_phase = false;
		bool lock_at_select = false; // the current lockstep phase is character select (no delay ceiling there)
		// Predicted fight start (PBD_PEER_FIGHT_START, PBD_PEER_FIGHT_PREDICT): docs/design/netcode-fight-start-prediction.md.
		u32 fight_start = 0;
		u32 fight_mode = 2;
		// Automatic delay uses the smallest opening round trip plus a jitter allowance, announced by role 0.
		// Sessions decide/apply at start + 30/+40; other runs at roll_begin - 20/-4, before prediction begins.
		bool delay_auto = false, delay_applied = false;
		u32 delay_auto_max = 3; // PBD_PEER_AUTO_MAX_DELAY (0..5): the largest delay the negotiation may choose
		// Fixed delays below 4 use opening delay 4, then switch through the same announcement as automatic delay.
		// The larger opening delay keeps lockstep from slowing to one tick per (one-way time / D).
		// PBD_PEER_OPENING_DELAY=0 keeps the configured delay from the first tick.
		s32 delay_forced = -1;
		u32 delay_announce = 0; // decided delay + 1; 0 = not decided / not received
		u32 session = 0;        // PBD_PEER_SESSION: transport messages carry it; another session's packets fail the run.
		u32 role = 0, delay = 0, scan_from = 0, holds = 0, corrections = 0;
		u32 max_prediction = 0; // Hold once index - first_unusable reaches this (PBD_PEER_MAX_PREDICTION; default ring - 1).
		u64 hold_ns = 0;
		s32 confirmed = -1, hash_checked = -1;
		// Received hashes for ticks not yet confirmed here, a ring of 16 by tick, checked once confirmed.
		std::array<u32, 16> pending_hash_tick{};
		std::array<std::array<u64, 3>, 16> pending_hash{};
		u32 hash_deferred = 0;
		const char* fault = nullptr;
		pbd::peer_log::buffered_file<pbd::peer_log::diagnostic_file> file;
		std::unique_ptr<pbd::peer_input_trace::queue<>> input_trace;
		std::unique_ptr<std::FILE, int (*)(std::FILE*)> input_trace_file{nullptr, &std::fclose};
		// The transport: socket, addresses, authentication and the endpoint it learns.
		struct link_state
		{
			pbd::peer_auth::state auth;
#ifdef _WIN32
			std::optional<pbd::peer_key_pipe::descriptor> key_pipe;
#endif
			std::string auth_set_id;
			// Internet play: endpoint learning, round-trip measurement, scheduled input delay.
			bool learn_endpoint = false;                // PBD_PEER_LEARN_ENDPOINT=1: adopt the other peer's address:port from its messages
			bool relayed = false;                       // PBD_PEER_ROUTE=relay: the datagrams go through the relay (display only)
			pbd::peer_endpoint::shared_endpoint target; // current send target, IPv4 or IPv6 (network byte order)
			u32 endpoint_changes = 0;
			// Datagrams not from the current send target are dropped (and counted) before anything in them is read; learning
			// adopts a new address:port only until the first valid message from the target (learn once). PBD_PEER_LEARN_ALWAYS=1 follows
			// any valid source instead.
			bool learn_always = false;
			std::atomic<bool> target_confirmed{false};
			u32 dropped_foreign = 0;
			u64 last_recv_send_us = 0, last_recv_at_us = 0; // newest sender timestamp received, and the local clock at receipt
			// LAN/internet: the other peer's IPv4 address or IPv6 literal, ASCII. Default loopback.
			std::array<char, 64> address{'1', '2', '7', '.', '0', '.', '0', '.', '1'};
			u32 local_port = 0, remote_port = 0;
			u64 socket_handle = ~u64{0}, sent = 0, received = 0, duplicates = 0;
#ifdef _WIN32
			bool wsa_started = false;
			std::mutex socket_mutex;
			// Call only after the receiver and admitted owner callbacks have drained. End notices share this lock with close.
			void release_transport() noexcept
			{
				std::lock_guard socket_lock(socket_mutex);
				if (socket_handle != ~u64{0})
				{
					::closesocket(static_cast<SOCKET>(socket_handle));
					socket_handle = ~u64{0};
				}
				if (wsa_started)
				{
					::WSACleanup();
					wsa_started = false;
				}
			}
#endif
			// The receiver thread's view of the other PC: its last valid datagram (session_clock microseconds) and whether it ended or
			// fell silent (peer_notice). Only the failstop receiver keeps them.
			std::atomic<u64> last_valid_us{0};
			std::atomic<bool> stop_noticed{false};
			std::atomic<bool> terminal_idle{false};
			pbd::peer_recovery::state recovery;
			std::unique_ptr<pbd::set_channel::command_reader> path_reader;
			u64 path_poll_us = 0;
			bool path_probe_turn = false;
		} link;
		// The receive thread and its queue.
		struct rx_state
		{
			bool enabled = true; // PBD_PEER_RX_THREAD=0: datagrams are read (and stamped) at the boundary poll
			std::mutex mutex;
			std::deque<rx_packet> queue;
			std::thread thread;
			std::atomic<bool> stop{false};
			std::atomic<u32> overflow{0};
		} rx;
		// Round-trip measurements.
		struct rtt_state
		{
			u64 last_us = 0, min_us = ~u64{0}, max_us = 0, sum_us = 0;
			u32 count = 0, logged = 0;
			std::array<u32, 128> ring{};
			u32 ring_count = 0;
		} rtt;
		// Time sync, frame advantage and their stalls.
		struct sync_state
		{
			u32 catchups = 0;
			// PBD_PEER_BALANCE=1 with time sync disabled: catch up below -2 ticks or stall above 3, at least 30 boundaries apart.
			// Advantage smooths own boundary minus the remote boundary advanced by message age and half the round trip.
			bool balance = false;
			double adv_ewma = 0.0;
			bool adv_valid = false;
			// Time sync (PBD_PEER_SYNC, default on; 0 = off): docs/design/netcode-time-sync.md.
			bool enabled = true;
			s32 remote_adv_milli = 0; // the other peer's smoothed advantage (its sign: + = IT is ahead), from its newest message
			bool remote_adv_valid = false;
			u32 evaluated = 0, slews = 0;
			u32 adv_sampled_at = ~u32{0};
			// Live boundaries that entered the hold loop, except the one just corrected (peer_end: wait_shallow_boundaries).
			u32 wait_shallow_boundaries = 0;
			// Stamps of the sync catch-up frame and of the opening alignment hold (see their sites).
			u32 last_catchup = 0, last_align = 0, align_holds = 0;
			s64 requested_us = 0;
			u32 last_stall = 0, stalls = 0;
			u32 remote_boundary = 0;    // the other peer's boundary in its newest message (newest tick - its input delay)
			u64 remote_boundary_us = 0; // when that message arrived
		} sync;
		// This PC's live controller input: the entry sample and the push model's fixed ticks.
		struct live_state
		{
			// Live controller plumbing: the owned pad port passes the real device value through instead of taking the tape
			// value. Off unless PBD_PEER_LIVE=1.
			bool enabled = false;
			bool pad_script = false; // A validated local pad-poll tape enters the ordinary sampled key ring.
			// ENTRY SAMPLES AT A DELAY SWITCH: when the negotiated delay shortens by k, k samples have no tick of their own. Their BUTTON
			// bits (game word 0x0fff; 0xf000 are the directions) are merged into the next fixed tick, so a tap lands late, not never.
			u32 entry_consumed_through = 0;
			// Bootstrap admission (on with the phase delay): the session-options handshake can run before
			// boundary 0 has sampled the controller. Do not publish/fix neutral owned ticks from those metadata-only datagrams;
			// the first entry sample then fixes its ordinary target without rewriting any value already sent to the peer.
			bool bootstrap_sample = false;
			u32 assigned_through = 0; // newest tick whose owned value is fixed by the push model
			bool assigned_any = false;
			u32 fills = 0, redundant = 0, deferred = 0, forced = 0, shrinking = 0;
			u32 bootstrap_withheld = 0, bootstrap_admitted = 0;
			u16 suppress = 0;
			u32 predrops = 0, predrops_changed = 0, suppressed_values = 0;
			// Edges of buttons released inside the dropped window: docs/design/netcode-fight-start-prediction.md.
			u16 released = 0, edge_carry = 0;
			u32 edges_deferred = 0, beyond_script = 0;
			bool entry_consumed_any = false;
			u32 entry_merged = 0;
			// ENTRY SAMPLE (PBD_PEER_ENTRY_SAMPLE, default on). With it off the owned input of tick t
			// is the device word the game read at the latch site of tick t-1-D: the value of boundary b is published only at
			// boundary b+1, one whole frame of input delay that buys no network margin (configured D plays as D+1 frames).
			// With it on the newest word of the game's
			// own input ring (what its reader 0x120a78 would return: ring[port][(producer - 1) & 15]) is sampled ONCE at the first
			// entry of boundary b, frozen, assigned to tick b + D, published in that boundary's datagram and injected at tick b + D.
			// A "sample" row per boundary is the proof chain (sample boundary -> tick -> injected value in the update row).
			bool entry_sample = true;
		} live;
	};
	// Session mode: menus and matches in one index space, segments, start policy and leaving.
	struct session_state
	{
		// SESSION MODE (PBD_SESSION=1): the session starts from a LOCAL character-select savestate (each peer its own)
		// and runs menus and matches in ONE index space. A boundary whose scene selector is not a battle scene (17 or 16) is a MENU
		// boundary: phase lockstep (never predicted, never corrected), no scene admission, the pause flag and input-counter restarts
		// tolerated; leaving the battle scene ends a match, not the session. The start state's counters and player hash are adopted as
		// observed (two installs differ there by construction); the fight contract (one battle tick per fight boundary, battle-side
		// consumer == initial + fight count) is unchanged.
		bool enabled = false;
		bool in_match = false;
		// Session admission policy.
		bool mask_start = false, failstop = false;
		u64 start_masked = 0, start_in_battle_executed = 0;
		// Start reaches only native intro and win-screen dispatches; a withheld press stays withheld until release.
		bool start_skip = false;
		u64 start_fight_masked = 0, start_passed = 0;
		std::atomic<bool> end_sent{false};
		// Leaving in game (filter_local_sample): consecutive admitted local samples with Start + Select held, and the next sample
		// index that may count (a sample admitted again through another path never counts twice). Select itself never reaches
		// the game in a session; session.selects_masked counts the samples it was removed from.
		u32 leave_hold = 0, leave_next_source = 0;
		bool leave_start_held = false; // Start stays removed after the combination until it is released
		u64 selects_masked = 0;
		// An end asked for between rows (the leave hold, leaving VS mode), taken at the next completed row like the stop file.
		u32 pending_end = 0; // finish_reason; collecting = none
		// First boundary of the current run of scene-0 boundaries (session_left_vs_boundaries), ~0u outside one.
		u32 scene0_since = ~0u;
		u32 matches = 0, match_start = 0;
		u32 menu_hash_mask = 0; // PBD_SESSION_MENU_HASH: combat regions (bit k = region k) enforced on MENU ticks; match ticks: all three
		u32 seed = 0;
		bool seed_set = false;
		u32 generator_syncs = 0;
		bool start_synced = false; // the session's first frame: counter and generator set from the session seed (see the barrier block)
		// RANDOM-PANEL SYNC:
		// the character-select init (0xafc64) draws each side's random-panel character with libc rand() % N (0xafd58 -> sw[side]+4 =
		// 0x10093f5c / 0x10093fd4) BEFORE a first-run checkpoint is captured, so every checkpoint carries its own value and a confirm on the
		// random slot (cursor 0x18) would start the fight with a different character on each peer.
		// At the session start and at every barrier INTO character select both peers set the two words from the
		// session seed (after the generator reseed; the roulette 0xb0a58 then steps them identically). Mechanism only: a value the game itself
		// would have drawn; off outside sessions.
		u32 random_panel_syncs = 0;
		std::vector<region_slot> region_ring;
		bool mismatch_bundle_written = false;
		bool dump_matches = false; // PBD_SESSION_DUMP_MATCH=1: full memory dumps at each match's first frame and 300 frames later
		// PBD_SESSION_MATCH_SNAPSHOT: at each match's first frame, a DRMD001 dump
		// of only the listed ranges, "begin-end,begin-end" in hex; "1" = the game's data segment 0x10000000-0x10800000.
		std::vector<std::pair<u64, u64>> snapshot_ranges;
		// Segments: the barrier's scenes, indices and counters, this PC's and the other's.
		struct segments_state
		{
			// Session ALIGNMENT BARRIER (peers): loading screens take a different number of frames on each host (scene 8 can take
			// 16 frames on one and 20 on another), so outside a match every scene change starts a new SEGMENT: the peer that gets there
			// first holds before executing the segment's first boundary, both announce (segment number, first index, scene) in every
			// datagram, and both execute that boundary at the SAME index max(own, other's); the indices a peer skips are never
			// executed there (published at once; no correction can reach them). While the other peer is still in the previous segment its
			// inputs for indices past its announced exit count as neutral. Segment scenes must agree (else session_segment_mismatch).
			u32 number = 0, index = 0, scene = 0;
			std::array<u32, 2> last_scene{};
			bool last_scene_valid = false;
			u32 peer_newest = 0;                                          // newest segment the other peer has announced
			std::array<u32, 8> peer_number{}, peer_index{}, peer_scene{}; // by segment % 8 (current and previous per datagram)
			u32 prev_index = 0, prev_scene = 0;
			// Menu sync: at every barrier both peers set the game-flow frame counter 0x1008f07c to the larger
			// of the two announced values (monotone on both peers: the game compares it with cached frame numbers), and at a barrier INTO a menu
			// scene both reseed the shared generator (S = seed(session, segment), flag 0 = srand) so random select and the stage roulette draw
			// the same values on both. The match-start reseed itself is the fn hook's (PBD_MATCH_SEED).
			u32 counter = 0, prev_counter = 0;
			std::array<u32, 8> peer_counter{};
			// Local stakes at the current/previous segment start, and peer stakes by segment % 8. Commands plan only from
			// matching words at the same barrier, since the launchers can deliver their stakes at different times.
			u32 rank_stakes = 0, prev_rank_stakes = 0;
			std::array<u32, 8> peer_rank_stakes{};
		} segments;
		// The exit gate and the settle hold of the pre-battle screens.
		struct gate_state
		{
			u32 settle_ms = 0, settle_scene = 12; // see the settle hold in the barrier block (off by default: it does not equalise scene 12)
			// EXIT GATE (PBD_SESSION_EXIT_GATE=1 with fn probes 0x52288 0x52290 0x117c70 0x117c78): the pre-battle screens 8
			// and 12 end on the first frame their queued files are ready (data slots in state 7), which depends on host file I/O and inflate speed, so
			// each peer would run them for a different number of frames and objects that already tick there would drift (on the Antarctic
			// stage the LCG2 state about 1000 ticks into the fight). With the gate the fn hook records the first index at which the screen's LAST
			// readiness test finds its slot ready, both peers exchange it in every datagram and the test is held failing (the slot's state word is masked
			// for that one call) until index max(ready_A, ready_B) + lag: both screens end on the same frame.
			bool enabled = false;
			u32 lag = 8, seg = 0, ready = 0; // gated segment, first ready index + 1 (0 = not yet)
			u32 through = 0;                 // readiness tests of every index below this value are recorded (sent to the other peer)
			bool opened = false;
			std::array<u32, 8> peer_ready_seg{}, peer_ready{}, peer_through_seg{}, peer_through{}; // other peer's, by segment % 8
		} gate;
		// The stage, segment and options digests both peers exchange, and the button maps.
		struct digests_state
		{
			// STAGE DIGEST: at each match start both peers digest the random parameter of every mob controller
			// created outside the battle (flee radius, controller +0x48; pbd::fn_hook::stage_controllers) and exchange it in every datagram; a
			// difference fails the session at once (session_stage_mismatch) instead of about 1000 fight frames later.
			u32 stage_match = 0, stage = 0, stage_count = 0;
			std::array<u32, 8> peer_stage_match{}, peer_stage{};
			// SEGMENT STATE DIGEST: after the syncs of every segment (session start included) both peers digest the
			// menu state a match depends on (picks 0x10093f50/54, cursors 0x10093f58/0x10093fd0, random-panel words 0x10093f5c/0x10093fd4, stage
			// id 0x1008f088, game-flow counter 0x1008f07c, the word at 0x100934a0, generator word) and exchange it (in every datagram, by segment);
			// a difference fails the session at that segment (session_segment_state_mismatch) instead of ~220 ticks later at match start.
			// The session start's digest also covers the game save image and both player records (pbd_setup_digest.h); a difference
			// there is session_setup_mismatch, and a digest that could not read them all is session_setup_unreadable.
			u32 segment_seg = 0, segment = 0;
			std::array<u32, 8> peer_segment_seg{}, peer_segment{};
			// OPTIONS DIGEST:
			// the options that shape a VS fight are digested at the session start (the option
			// SOURCES the lockstep menus replay inputs from, plus the save-data options, the character-select cursors and handicaps) and at every
			// match start (the applied values: rounds, time, guard damage, handicaps, CPU flags / human mask, applied vitality, button maps) and
			// exchanged in every datagram; a difference ends the session (session_options_mismatch). Sequence number 0x10000 = session start,
			// m = match m. BUTTON MAPS: different configurations are legitimate, so at the start barrier each peer sends the 32-byte map of the
			// side it plays (0x100500cc + 32 * role) and installs the other peer's map in the other side's slot: both peers map both ports alike.
			bool enabled = true; // PBD_SESSION_OPTIONS=0 turns the digests and the map exchange off
			u32 options_seq = 0, options = 0;
			std::array<u32, 8> peer_options_seq{}, peer_options{};
			std::array<u8, 32> own_map{}, peer_map{};
			bool own_map_valid = false, peer_map_valid = false, maps_installed = false;
		} digests;
	};
} // namespace pbd::session_runtime
