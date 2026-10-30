#pragma once

// Practice's state: its menus, the dummy and its options. Members of pbd::rollback_session::storage
// (pbd_rollback_session_storage.h).

#include "pbd_rollback_session_common.h"
#include "../PBD/pbd_practice_guard.h"
#include "../PBD/pbd_practice_input_delay.h"
#include "../PBD/pbd_practice_move_list.h"
#include "../PBD/pbd_practice_punish.h"

namespace pbd::session_runtime
{
	// Practice dummy recording is host-owned and offline-only. P1 remains the player's ordinary live input; while recording the
	// same controller drives P2 and P1 is neutral. Accepted recordings are kept in eight bounded slots and replayed through the
	// same canonical input word consumed by the game. Scheduler entropy never reads or mutates the guest's RNG.
	struct practice_dummy_slot
	{
		// Opponent-relative words: forward (toward the opponent) in the pad's Right bit, as for a dummy on the left. Each
		// recorded frame is converted with the side DR read that frame's directions with, and each played frame with the
		// side DR will read it with, so forward and back keep their meaning whichever side the dummy stands on.
		std::vector<u16> frames;
		// The dummy's input side on the take's first word, for the logs only (the words are already relative).
		std::optional<pbd::practice::movement::screen_side> side;
		// Trim Neutral skips these at playback; the take itself is kept whole.
		u32 leading_neutral_frames = 0, trailing_neutral_frames = 0;
		// The stop chord's tail (pbd::practice::controls::record_stop): the last frames, which playback always leaves out.
		u32 stop_chord_frames = 0;
		bool trim_neutral = true;
		bool enabled = true;
		std::string name;
		u32 frequency = 1; // 0..10, used as the host scheduler's weight
		u32 interval = 1;  // 0 short, 1 normal, 2 slow, 3 host-side shuffle
		// Moves chosen from the move list (their ids in the dummy's loaded list): while there are any the slot plays one of
		// them, picked at random each time, instead of its recording, which is kept. move_sequence is the move to play next as
		// opponent-relative words like a take's (pbd::practice::dummy_move::move_words); empty while none can be read.
		std::vector<u32> moves;
		std::vector<u16> move_sequence;
	};
	// Playback (state 3, not preview) runs action, recovery gate, gap, ready gate, action... The gap is the Action
	// Interval (practice.dummy.wait, its frames still to play) and is the only phase that plays the movement loop; both
	// gates give neutral until the dummy can act (pbd::practice::movement::recovery_verdict / ready_verdict). A gap's
	// movement hands straight over to its action instead, with no ready gate (pbd::practice::movement::may_flow). With no
	// action to play (no slot chosen), playback is a continuous movement alone and its gap does not end. A hit pauses
	// playback and hands the dummy to the game until it can act (practice.movement.pause, practice_dummy_route).
	enum practice_dummy_phase_kind : u32
	{
		dummy_action = 0,
		dummy_recover = 1,
		dummy_gap = 2,
		dummy_ready = 3
	};
	// The fighters' move entries (fighter record +0xC0, and +0xC4 for the capture file), read once per Practice
	// boundary next to the screen sides; empty when unreadable. The dummy's resting stances are learned from them.
	struct practice_move_sample
	{
		u32 entry = 0, other = 0;
		u64 identity = 0;
	};
	// practice-inputs.jsonl: both ports' words as the game consumed them, one row per change, plus the move entries.
	// Diagnostic only: buffered, bounded, and a failed write only stops the capture (open_diagnostic_file). The mutex
	// covers finish(), which may flush from another thread while the owner appends.
	struct practice_input_capture
	{
		static constexpr u64 limit_bytes = 20ull << 20;
		std::mutex mutex;
		std::FILE* file = nullptr;
		std::string buffer;
		u64 bytes = 0, rows = 0;
		std::atomic<bool> stopped{false};
		std::chrono::steady_clock::time_point flushed{};
		std::array<std::optional<u16>, 2> last{};

		~practice_input_capture()
		{
			if (file)
				std::fclose(file);
		}
		bool open(const std::filesystem::path& path) noexcept
		{
			file = open_diagnostic_file(path);
			return file != nullptr;
		}
		// Rows are worth building only while this holds.
		bool active() const noexcept
		{
			return file && !stopped.load(std::memory_order_relaxed);
		}

		void flush_locked() noexcept
		{
			if (buffer.empty() || !file || stopped)
			{
				buffer.clear();
				return;
			}
			if (std::fwrite(buffer.data(), 1, buffer.size(), file) != buffer.size() || std::fflush(file) != 0)
				stopped = true;
			bytes += buffer.size();
			buffer.clear();
			flushed = std::chrono::steady_clock::now();
		}
		void append(const std::string& text) noexcept
		{
			try
			{
				std::lock_guard lock(mutex);
				if (stopped || !file)
					return;
				if (bytes + buffer.size() + text.size() > limit_bytes)
				{
					buffer += "{\"type\":\"limit\",\"note\":\"capture stopped at its size limit\"}\n";
					flush_locked();
					stopped = true;
					return;
				}
				buffer += text;
				++rows;
				if (buffer.size() >= (16u << 10) || std::chrono::steady_clock::now() - flushed >= std::chrono::seconds(1))
					flush_locked();
			}
			catch (...)
			{
				stopped = true;
			}
		}
		void flush() noexcept
		{
			try
			{
				std::lock_guard lock(mutex);
				flush_locked();
			}
			catch (...)
			{
			}
		}
	};
	struct practice_play_checkpoint
	{
		bool valid = false, preview = false, have_slot = false;
		u32 slot = 0, position = 0, wait = 0, rng = 0, cycles = 0, movement = 0;
		u32 phase = 0, gate = 0, gap_over = 0, neutral_run = 0, caps = 0, facing_caps = 0, move_gate = 0;
		bool moved = false, movement_started = false, facing_proven = false;
		u32 turn = 0, flow_given = 0, play_first = 0;
		pbd::practice::movement::flow_plan flow{};
	};
	// Practice: its menus, sides, restarts and the player's input.
	struct practice_state
	{
		// Practice rewind uses the same admitted static checkpoint, owner-write journal and one-boundary rollback bridge as
		// replay takeover. R3 replaces the anchor with the current live moment; SELECT+CROSS returns to it. It has no replay
		// input source: after the bridge, the game's ordinary controller word is used again.
		bool rewind = false, rewind_quiet = false;
		u32 remembers = 0;
		// SELECT+Circle as the take's stop chord, in either order.
		pbd::practice::controls::record_stop record_stop{};
		u16 host_pad_prev = 0;
		// The frame before this boundary really ran (set by the Practice controls); counts those frames for the capture.
		bool frame_advanced = false;
		u64 input_frame = 0;
		practice_input_capture inputs;
		pbd::practice::controls::button_map button_map = pbd::practice::controls::defaults;
		pbd::game_overlay::glyph_family glyphs = pbd::game_overlay::glyph_family::playstation;
		pbd::practice::snapshot view{};
		// The last controller takeover notice shown as a Practice toast (pbd::controller_takeover::take_new).
		u64 controller_notice = 0;
		pbd::practice::history_tracker history{};
		// Input Delay Frames: the delay chosen, and the player's words waiting in it (pbd::practice::input_delay).
		u32 input_delay_frames = 0;
		pbd::practice::input_delay input_delay{};
		// Each attacker's latest combo for Attack Info (P1's attacks, then P2's).
		std::array<pbd::practice::combo_tracker, 2> combos{};
		// P1's wavedashes and backdash cancels, for the movement streak pop-up.
		pbd::practice::streak::tracker streak{};
		pbd::practice::precision::tracker precision{};
		// The gap after a blocked attack of the dummy's, for the punish pop-up (pbd::practice::punish).
		pbd::practice::punish::watch punish{};
		// The last select-scene probe line, without its boundary: a line is written only when the game's select state changes.
		std::string selection_state_record;
		std::deque<pbd::practice::native::request> sounds;
		bool boundary_counter_valid = false, record_word_valid = false;
		u32 boundary_counter = 0;
		u16 record_word = 0;
		bool history_pending = false;
		u32 history_index = 0;
		std::array<u16, 2> history_words{};
		std::array<std::optional<pbd::practice::movement::screen_side>, 2> history_sides{};
		// Both fighters' camera-projected screen sides, read once per Practice boundary on the owner thread before that
		// boundary's input conversion. Empty when the read was refused (loading, a tie, no match, unverified code).
		std::optional<pbd::fighter_side::screen_sides> sides;
		// The sides DR converts both fighters' directions with on the next frame (pbd::fighter_side::read_input), read at the
		// same moment. Preferred over the projection, which is refused on most boundaries.
		std::optional<pbd::fighter_side::input_sides> input_sides;
		// Each fighter's last side that was read, for a boundary where neither reading answers.
		std::array<std::optional<pbd::practice::movement::screen_side>, 2> last_sides{};
		// The side the word in practice.record_word is read with, stored with it so the take keeps it as a relative word.
		pbd::practice::movement::screen_side record_side = pbd::practice::movement::screen_side::right;
		practice_play_checkpoint play_before{};
		std::array<u32, 2> health_previous{};
		bool health_valid = false;
		u32 play_mode = 2, interval = 3;
		// Practice Settings' Training Mode. Offense leaves the dummy to the game's own options; Defense shows the recording
		// setup and gives SELECT+Circle to playback in the fight.
		pbd::practice::training_mode training_mode = pbd::practice::training_mode::offense;
		// SELECT has stayed down since the last restart or dummy chord. practice.restart.modifier_held also stays set while
		// another button is held after SELECT lifts; a SELECT pressed again from there is a fresh one (the Exit hold's).
		bool chord_select_held = false;
		// SELECT as a modifier: the player's injected word (port 0) and the recording word (the live controller drives P2).
		pbd::practice::controls::select_modifier select_modifier{}, record_select{};
		pbd::practice::controls::chord_window chord_window{};
		u32 input_leaks = 0;
		session_clock::time_point health_next{};
		// The pause menu: its focus, dropdowns, questions and move list.
		struct menu_state
		{
			bool open = false;
			// Esc opened the in-game Settings menu over the fight: the pause menu opens under it on its Settings category.
			bool on_settings = false;
			// The open pause menu is the one Esc opened under the in-game menu; that menu's Return to Game resumes the fight.
			bool by_escape = false;
			// Menu-only state: an open dropdown on the focused row (and its highlighted option). It does not touch the dummy
			// engine until a choice is confirmed.
			bool dropdown_open = false;
			// The open dropdown shows a list of its own, opened from one of its options (Action After a Hit or Block's Recording:
			// the recorded Dummy Actions). Circle goes back to the first list.
			bool dropdown_sub = false;
			// Practice's in-game move list (pbd_practice_move_list.h), open over the menu to choose a Defense slot's moves or
			// Offense's Action After a Hit or Block moves.
			pbd::practice::move_list_view::state move_list{};
			// The loaded move list (pbd::practice::dummy_move::runtime::epoch) the slots' next moves were read from; another one
			// reads them again.
			u32 slot_moves_epoch = ~0u;
			// Menu-only state: a Yes / No question over the list (Restore Defaults, Character Selection, Stage Selection, Exit
			// Practice) and its highlighted answer. Nothing happens until Yes is confirmed.
			pbd::practice::confirm_kind confirm = pbd::practice::confirm_kind::none;
			u32 confirm_choice = pbd::practice::answer_no;
			// The game's five practice options as Practice first showed them. Practice boots fresh for every session, so these
			// are the values the game starts with; Restore Defaults writes them back.
			std::optional<std::array<u32, pbd::practice::game_option_count>> game_defaults;
			// Pause menu column focus (left rail or right list), and the Dummy Action slot the next opening returns to after
			// leaving the ready-to-record state (umax: open on the rail as usual).
			bool rail_focus = true;
			u32 return_slot = umax;
			u32 dropdown_focus = 0;
			// Playback picks among the filled slots weighted by each slot's frequency (the menu's L1/R1 picker).
			u32 page = 0, row = 0;
			// START held through a pending Restart (or under the restart chord's SELECT) until the menu can take it.
			pbd::practice::controls::menu_start start{};
			u64 generation = 0;
			u64 mouse_revision = 0;
			bool hint_pending = false;
			session_clock::time_point toast_until{};
		} menu;
		// Offense: the dummy's movement and its reaction after a hit or block.
		struct offense_state
		{
			// Offense's own dummy rows: its Movement (Neutral leaves the dummy to the game's Dummy Action) with its Wavedash Speed,
			// and its Action After a Hit or Block. The tape above is the loop of the training mode in view. SELECT+Circle turns the
			// Offense movement off in the fight and on again.
			pbd::practice::movement::preset movement = pbd::practice::movement::preset::none;
			u32 wavedash_speed = pbd::practice::movement::default_wavedash_speed;
			pbd::practice::after_contact after = pbd::practice::after_contact::none;
			// With Action After a Hit or Block = a Dummy Action: its slot (0..7); = a plain movement input: that input. A Moves
			// reaction's moves are pbd::practice::dummy_move::current_settings. The Count and After rows: which contacts count and
			// how many bring the reaction.
			u32 reaction_slot = 0;
			pbd::practice::movement::reaction_input reaction_input = pbd::practice::movement::reaction_input::backdash;
			pbd::practice::contact_counted counted = pbd::practice::contact_counted::either;
			u32 times = pbd::practice::min_reaction_times;
			bool movement_off = false;
		} offense;
		// The dummy's wake-up, guard, counter-hit and throw-escape options.
		struct reactions_state
		{
			pbd::practice::pbd_wakeup_option wakeup = pbd::practice::pbd_wakeup_option::stand_up;
			pbd::practice::pbd_wakeup_controller wakeup_control;
			bool wakeup_suspended = false;
			// Guard All's guard choice (Dummy Action's Guard After First Hit and Random Guard) and Counter Hit's Random, with the
			// host random state their draws come from (pbd::practice::pbd_guard_state, practice_guard_frame).
			pbd::practice::pbd_guard_state guard;
			pbd::practice::pbd_counter_hit_draw counter_hit;
			u32 guard_rng = 0x9e3779b9u;
			// Throw Escape: whether the dummy breaks the player's throws, and the throw it is breaking now.
			pbd::practice::throw_escape_option throw_escape = pbd::practice::throw_escape_option::none;
			pbd::practice::throw_escape_control throw_escape_control;
		} reactions;
		// The dummy's movement preset and what it learns of the fighters' stances.
		struct movement_state
		{
			// Movement Between Actions: the loop chosen for the dummy (Neutral has none), its tape (rebuilt when the choice
			// changes) and the loop cursor.
			pbd::practice::movement::preset preset = pbd::practice::movement::preset::none;
			pbd::practice::movement::input_sequence input_sequence{};
			u32 pos = 0;
			// Defense's Wavedash Speed (crouch dashes a second) and Wavedash End: how a wavedash flows into each action.
			u32 wavedash_speed = pbd::practice::movement::default_wavedash_speed;
			pbd::practice::movement::wavedash_end wavedash_end = pbd::practice::movement::wavedash_end::crouch_dash;
			// Defense's Dash End: how Continuous Forward Dash and Continuous Backdash flow into each action.
			pbd::practice::movement::dash_end dash_end = pbd::practice::movement::dash_end::into_move;
			// Playback's pause while the dummy is hit, knocked down or getting up, with the dummy handed to the game's own Dummy
			// Action meanwhile (pbd::practice::movement::movement_pause).
			pbd::practice::movement::movement_pause pause{};
			std::array<std::optional<practice_move_sample>, 2> moves{};
			std::array<std::optional<std::pair<u32, u32>>, 2> moves_logged{};
			pbd::practice::movement::rest_learner rest{};
			bool started = false;
		} movement;
		// Contacts between the fighters and the reactions counted from them.
		struct contact_state
		{
			// Offense with an Action After a Hit or Block: the contact watch while the game's Dummy Action drives the dummy,
			// whether it is running, the counted contacts so far, and a due reaction the next Practice boundary starts.
			pbd::practice::movement::contact_tracker tracker{};
			pbd::practice::reaction_counter reaction_count{};
			bool watching = false, ready = false;
		} contact;
		// Restarts: their position and variant, pending resets and the wall-restart diagnostics.
		struct restart_state
		{
			std::optional<pbd::game_lifecycle::placement_token> placement;
			// The in-memory takeover checkpoint belongs to this arena and fighter pair only.
			std::optional<pbd::game_lifecycle::scope> moment_scope;
			std::array<pbd::practice::combo_tracker, 2> moment_combos{};
			std::array<u32, 2> moment_health_previous{};
			bool moment_health_valid = false;
			pbd::game_lifecycle::placement position = pbd::game_lifecycle::placement::centre;
			// Restart Settings' Dummy Direction and Switch Positions, which every restart the player makes applies.
			pbd::game_lifecycle::placement_variant variant{};
			std::optional<pbd::game_lifecycle::practice_reset> reset_request;
			u64 reset_serial = 0;
			// Wall-restart diagnostic: the Practice scope (one per stage load) last surveyed, whether its line was written, and
			// the attempts so far while its collection could not be read yet.
			pbd::game_lifecycle::scope wall_scope{};
			bool wall_reported = false;
			u32 wall_attempts = 0, wall_wait = 0;
			// practice_reset_sides diagnostic: the wall restart being followed (0: none), its target, whether its rough pair was
			// written before the pose pass, and the fresh boundaries logged since it. armed is set once the restart's own boundary
			// has passed.
			u64 sides_request = 0;
			u32 sides_target = 0, sides_logged = 0;
			bool sides_armed = false, sides_provisional = false;
			session_clock::time_point reset_deadline{};
			bool modifier_held = false;
			// First boundary at which the pending Restart was refused (umax: not refused yet); bounds its retries.
			u32 reset_retry_from = umax;
		} restart;
		// DR's pause as Practice asks for it and resumes from it.
		struct pause_state
		{
			bool requested = false, injected = false;
			// The boundary the pause press was injected at, umax once DR showed the pause (pbd::practice::controls::pause_press_lost).
			u32 injected_at = umax;
			bool resume_native = false, resume_injected = false;
			// The resume START sent and not yet answered (pbd::practice::controls::resume_watch). While it is in flight DR's pause is
			// on its way out: opening the menu or sending another START then would act on a game that is about to unpause.
			pbd::practice::controls::resume_watch resume_watch{};
			pbd::practice::controls::pause_takeover takeover{};
		} pause;
		// Test only: offsets from the first Practice battle boundary.
		struct test_state
		{
			u32 remember = umax, restart = umax; // test-only offsets from the first Practice battle boundary
			bool remembered = false, restarted = false;
		} test;
		// DR's own pause and the native requests Practice makes.
		struct native_state
		{
			bool paused = true, pause_valid = false;
			// DR's pause flag and its menu word as read at this boundary, and whether the frame before it ran
			// (pbd::practice::controls::native_paused).
			bool flag = true, menu_word = false, ran = false;
			u32 screen = 0;
			std::optional<pbd::practice::native::request> request;
			// The dummy's Dummy Action route is ours (the player's own actions saved in practice_native_action), and whether it is
			// handed back to the game for a get-up (practice_dummy_route).
			bool controlled = false, getup = false;
			std::array<u32, 2> written{10, 10};
			std::array<u32, 2> action{};
		} native;
		// The dummy: its recordings, playback, movement and gates.
		struct dummy_engine_state
		{
			bool diagnostic_write_failed = false;
			bool enabled = false;
			std::array<practice_dummy_slot, 8> slots{};
			dummy_state state = dummy_state::idle;
			u32 slot_index = 0, choice = 0, menu_row = 0;
			u32 play_slot = 0, play_pos = 0, wait = 0;
			u32 rng = 0x6d2b79f5u, recordings = 0, plays = 0, cycles = 0;
			u16 pad = 0, pad_prev = 0;
			bool ignore_until_release = false, mask_ui = false, audio_muted = false;
			bool have_play_slot = false;
			// Playback of the movement once, after a contact: its gap ends with its interval instead of looping on. With
			// practice.dummy.reaction it is the chosen Dummy Action instead, played once and ended when the dummy has recovered.
			bool once = false, reaction = false;
			// The dummy's health dropped on the frame that just ran.
			bool hit = false;
			// Frames since the dummy's health last dropped (held at the settle cap), and whether, with a usable rest reading, it
			// has not yet been read back on a resting stance since: a dummy the pause handed back at its cap may still be lying,
			// and lying is still, so until then nothing it holds is learned or settled (practice_read_moves).
			u32 since_hit = pbd::practice::movement::settle_cap_frames;
			bool recovering = false;
			// The Dummy Action the frame whose word was chosen last runs under (practice_dummy_route): below 10 one of the game's
			// own, 10 Controller (the port word drives it), umax unknown. The next boundary reads it as who drove that frame.
			u32 driver = umax;
			// A hit landed in the current gap with no rest reading to restart its movement from (pbd::practice::movement::may_flow).
			bool gap_hit = false;
			u32 phase = dummy_ready;
			// Frames given in the current gate, frames the gap ran past its interval to finish a loop pass, and the consecutive
			// neutral frames the dummy has had.
			u32 gate = 0, gap_over = 0, neutral_run = 0;
			// A movement that handed over to the action (pbd::practice::movement::flow_plan): the plan, the frames of its lead-in
			// (a Wavedash End, a release) already given before the take, and the take's position the action started at (its
			// leading neutral frames skipped).
			pbd::practice::movement::flow_plan flow{};
			u32 flow_given = 0, play_first = 0;
			// The dummy was seen out of its resting stance since the action started.
			bool moved = false;
			// Consecutive recovery gates the move reading contradicted: the dummy still away from rest at the cap, or never seen
			// away from rest during an action with inputs (unconfirmed; a reading that follows moves shows every move). At
			// settle_distrust_caps the reading is set aside for this character and the fixed waits apply, so a reading that does
			// not follow moves (one that always reads rest would shorten the ready gate to its 10-frame lead-in) costs three
			// cycles at most.
			u32 caps = 0;
			// Whether the dummy is at rest at this boundary; empty when there is no usable reading (nothing learned yet, or the
			// reading set aside). Once the reading is in use, a sample that cannot be read counts as away from rest.
			std::optional<bool> rest;
			// The gap's movement: frames held at the current pass start while the dummy was not ready, and whether the movement
			// has given a frame since the gap began (its first pass also needs the dummy at rest).
			u32 move_gate = 0;
			// A backturned dummy being turned around at a ready gate or a movement pass start (pbd::practice::movement::turn_around).
			pbd::practice::movement::turn_around turn{};

			fs::file file;
			bool preview = false;
			// Test only: a scripted recording.
			struct test_state
			{
				u32 start = umax, stop = umax, end = umax;
				u16 word = 0;
				bool started = false, stopped = false, complete = false;
			} test;
			// Whether the dummy faces P1, and how far that reading is trusted.
			struct facing_state
			{
				// Whether the dummy faces P1 at this boundary (pbd::practice::movement::facing_verdict), from its yaw (+0x74) and both
				// fighters' world positions (+0x760 / +0x768); the 16-bit facing (+0x7a) is read beside it for the log and decides only
				// when the yaw cannot be read. Unknown while unreadable or set aside.
				pbd::practice::movement::facing verdict = pbd::practice::movement::facing::unknown;
				std::optional<float> error, error16;
				// Consecutive gates (ready gates and movement pass starts) the facing alone held to their cap. At settle_distrust_caps
				// the facing reading is set aside for this character (a reading that never shows the dummy facing P1 would otherwise
				// cost the cap on every action and every movement pass). Once a gate has ended with the dummy facing P1 the reading is
				// proven for this character and caps no longer count: a dummy left backturned that the turn taps do not turn (one
				// lying face down, say) holds a gate to its cap, and a run of those must not switch the facing check off for the session.
				u32 caps = 0;
				bool proven = false;
			} facing;
			// A take being recorded.
			struct pending_state
			{
				std::vector<u16> words;
				u32 leading = 0, trailing = 0, chord = 0;
				// The dummy's side on the take's first recorded word (the slot keeps it for the logs).
				std::optional<pbd::practice::movement::screen_side> side;
			} pending;
		} dummy;
	};
} // namespace pbd::session_runtime
