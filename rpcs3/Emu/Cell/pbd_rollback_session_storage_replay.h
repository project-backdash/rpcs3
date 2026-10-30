#pragma once

// Replays: recording a match, the replay controls and viewer, takeover and rewind. Members of pbd::rollback_session::storage
// (pbd_rollback_session_storage.h).

#include "pbd_rollback_session_common.h"
#include "../PBD/pbd_replay_checkpoint.h"

namespace pbd::session_runtime
{
	// Replay rewind (pbd_replay_rewind.h, pbd_rollback_session_rewind.cpp): in-memory checkpoints taken while the recording
	// plays both sides, one per second of fight plus every round's start, each with its static pages and the undo journal of
	// the owner's first writes up to the next one. Rewind, round jumps and seeks restore them through the takeover restore;
	// fast forward runs the recorded inputs at the fast vblank rate with the sound off. Never written to a file.
	struct rewind_point
	{
		takeover_anchor anchor{};
		std::shared_ptr<pbd::replay_checkpoint::image> sealed;
		bool journal_loaded = false;
		u32 round = 0;
		bool pinned = false;
		u64 bytes = 0;
	};
	// Test only (PBD_REPLAY_CONTROL_TEST): control actions scripted by boundary instead of a controller, for unattended checks.
	// "<boundary>:<action>" separated by commas; actions pause<ms> (pause, resume after ms), step<n> (pause, n single frames, resume),
	// speed<-2..2>, take0 and take<1|2>=<word> (that side plays the constant word).
	struct replay_test_action
	{
		u32 boundary = 0;
		char kind = 0;
		s32 value = 0;
		u16 word = 0;
	};
	// Recording a match's inputs for its replay.
	struct match_replay_state
	{
		// Inputs-only replay publication is opt-in and bound to this load. A second match needs a new checkpoint load and run.
		bool requested = false, attempted = false, refused = false;
		pbd_live_replay live;
		bool context = false, battle_seen = false, stage_sync = false;
		const char* setup_error = nullptr;
		std::filesystem::path clean_state;
		std::string recipe_sha256, state_sha256;
		pbd::match_recording::lifecycle lifecycle;
		u32 segment = 0, next = 0;
		pbd::match_recording::observation end{};
		bool end_pending = false;
	};
	// Replays: session replays, the replay controls and the viewer.
	struct replay_state
	{
		bool vblank = false; // PBD_REPLAY_VBLANK=1: one virtual vblank per replayed tick (Emu/RSX/pbd_replay_pacing.h).
		bool round_reconstructed = false;
		bool session_replay = false;
		std::map<u32, std::vector<std::array<u32, 3>>> inputs; // segment -> (offset, p1, p2) change points, increasing offsets
		std::map<u32, u32> sync;                               // segment -> frame counter the session synced at its start
		// The replay line's round (display only): counted from each rise of the fight flag within the match (last_fight_index: the
		// update before this boundary was a fight step), shown only while that round is being fought.
		u32 round = 0, round_start = 0;
		bool was_fighting = false;
		// Replay takeover (PBD_REPLAY_TAKEOVER=1): control pins the Retry/Return anchor and starts takeover.frames,
		// counted in simulation frames so pause and slow motion never use it up. Expiry holds the boundary for Retry/Return.
		// The viewer controller and replay_pad_steps implement viewer and non-viewer bindings respectively.
		bool takeover = false, takeover_fast = false;
		// Replay controls (PBD_REPLAY_CONTROLS=1): this PC's controller owns playback actions until it takes a fighter.
		// A paused or slowed boundary is held at its first dispatch with no VM lock held. Speed changes the vblank rate,
		// not the simulated frames. A taken side may run past the recorded tape and ends when that match ends.
		bool controls = false, paused = false, step = false, start_clean = false, taken = false;
		bool viewer_enabled = false;
		u32 viewer_bookmarked = 0; // 0 idle, 1 saving, 2 saved, 3 failed
		u32 bookmark_request = 0;
		u64 bookmark_requested_at = 0;
		std::filesystem::path bookmark_reply;
		pbd::replay_viewer::controller viewer{};
		std::vector<pbd::replay_viewer::round_row> viewer_rounds;
		std::array<pbd::replay_viewer::command_history, 2> viewer_history{};
		bool leave_held = false;
		s32 speed = 0;
		u32 control_port = 0; // 0 = the recording plays both sides, 1 = P1, 2 = P2 from this PC's controller
		u32 input_port = 0;   // Public Watch keeps its live controller on port 2, independently of the taken fighter.
		u16 pad = 0, pad_prev = 0;
		u32 end_index = 0; // an untouched replay ends after this many boundaries (0 = at the tape's end)
		u32 steps = 0;
		session_clock::time_point last_release{}, leave_from{};
		fs::file control_file;
		fs::file viewer_event_file;
		// The single-instance rollback oracle (--tick-roll-*) takes the AUTHORITATIVE input of tick t from the tape; in a session replay that is the
		// replay value the tick got at its first dispatch (otherwise the oracle would "correct" every tick towards the silent tape).
		// p1 | p2 << 16 per index.
		std::vector<u32> values;
		// Fast forward to a match or a moment.
		struct fast_state
		{
			// Watching one match of a replay (PBD_REPLAY_FAST_UNTIL_MATCH=<n>, n >= 2): until match n-1 has ended the emulator runs its
			// vblank at replay.fast.rate (the Auto frame limit follows it) with the sound at 0, then both return to what they were. The
			// game computes the same frames at any vblank rate (a replay compares equal frame by frame at 240 Hz), it only gets there sooner.
			u32 until_match = 0, rate = 240;
			// Watching from a moment (PBD_REPLAY_FAST_TO=<match>:<frame>): the same fast forward, until that frame of that match, counted
			// from the match's first battle-scene boundary as the replay's segments are (a replay's absolute indices differ from the
			// recording's by the menus' loading, its offsets within a match do not). Replaces the match target when both are given.
			u32 to_match = 0, to_frame = 0;
			bool active = false;
			s64 saved_vblank = 60;
		} fast;
		// Trying a moment again (PBD_REPLAY_TAKE_AT).
		struct take_state
		{
			// Trying a moment again (PBD_REPLAY_TAKE_AT=<match>:<frame>:<1|2>, replay controls on): the replay pauses at that frame and
			// hands that side to this PC's controller, as SELECT + LEFT / RIGHT would there; the fast forward runs up to it.
			u32 match = 0, frame = 0, port = 0;
			bool done = false;
			u32 resume_ms = 0; // test only (PBD_REPLAY_TAKE_RESUME_MS): resume by itself after that long
		} take;
		// Test only (PBD_REPLAY_CONTROL_TEST): control actions scripted by boundary.
		struct test_state
		{
			std::optional<u16> pad_word;
			std::vector<replay_test_action> actions;
			usz next = 0;
			u32 steps = 0;
			bool step_resume = false, word_set = false;
			u32 choice_pending = 0; // test only: prompt answer scripted before the prompt shows (1 = again, 2 = return)
			u16 word = 0;
			session_clock::time_point resume_at{};
		} test;
	};
	// Replay takeover: Take Control, its anchor, prompts and restores.
	struct takeover_state
	{
		std::atomic<u32> rewind_request{0}, rewind_ack{0};
		u32 rewind_to = 0, rewind_from = 0, rewind_pass = 0, rewind_pages = 0;
		u64 rewind_ns = 0;
		std::array<char, 16> rewind_action{};
		bool audit = false;
		pbd::takeover_phase phase = pbd::takeover_phase::watching;
		u32 port = 0, start = 0, start_frame = 0, match = 0, deadline = 0;
		u32 frames = 600, choice = 0; // prompt choice: 0 = Try again/Yes, 1 = Return to replay/No
		bool confirm_was_paused = false;
		bool timed_take = false;                // PBD_REPLAY_TAKE_SECONDS: the TAKE_AT moment starts a timed takeover (Try again)
		std::vector<std::array<u32, 3>> rounds; // (match, round, match frame of its first fight frame) seen in this run
		u32 next_round = 0;                     // R1: the fast forward runs until this round is being fought (0 = none)
		u32 next_match = 0;                     // Never carry an unavailable next round into another match.
		takeover_host_slot dispatch_slot{};
		takeover_host_slot capture_host{};
		takeover_anchor keyframe{};
		bool capture_pending = false;
		u32 capture_index = 0;
		bool fault_armed = false, redispatch = false, null_input = false, ignore_pad_until_release = false;
		u32 restore_pass = 0;
		std::string refusal;
		// Take Control asked for outside a fight (L3 / R3 during an intro): it starts at the round's first fight boundary.
		bool auto_start = false;
		std::vector<u32> audit_restored;
		// The one-boundary rollback bridge a long rewind re-enters its anchor through.
		struct bridge_state
		{
			// A successful long rewind re-enters its anchor as one ordinary rollback replay tick. That tick is simulation-only
			// and presentation-withheld; live rendering and the chosen input source begin at the following boundary.
			bool active = false;
			u32 from = 0;
			u32 side = 0;
			bool saved_correct = false;
			bool taken = false; // the restored anchor's replay.taken, applied when the bridge hands back to the recording
		} bridge;
	};
	// Replay rewind: in-memory checkpoints, seeks and their costs.
	struct rewind_state
	{
		bool enabled = false;
		u64 budget = 0, bytes = 0;
		std::vector<std::unique_ptr<rewind_point>> points;
		std::unique_ptr<pbd::replay_checkpoint::compressor> compressor;
		bool capture_pending = false, capture_pinned = false;
		u32 capture_index = 0, capture_round = 0;
		takeover_host_slot capture_host{};
		// The host slot as it stood at the start of this boundary's replay controls, before the round tracking: a restore
		// re-enters the boundary after it, which then sees a round start again exactly as the first time.
		takeover_host_slot dispatch_host{};
		// A seek in progress: the fast forward runs to this frame of this match, then the replay pauses there if it was paused.
		bool seeking = false, pause_after = false;
		u32 seek_match = 0, seek_frame = 0;
		// The in-game Settings menu (Esc) paused the replay; closing it resumes.
		bool settings_paused = false;
		u32 captures = 0, evictions = 0;
	};
} // namespace pbd::session_runtime
