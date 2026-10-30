#pragma once

// observe()'s work at one boundary as member functions of a context that refers to the latch, its state, the owner
// PPU and the stage pc. Included only by the latch's own source files.

#include "pbd_rollback_session_storage.h"
#include <span>
#include "../PBD/pbd_practice_start.h"
#include "../PBD/pbd_practice_memory.h"
#include "../PBD/pbd_env.h"
#include "../PBD/pbd_peer_message.h"

// One admitted boundary call. Only the selected PPU uses this context, and its references expire with the call.
struct pbd::rollback_session::boundary_call
{
	pbd::rollback_session& session_runtime;
	storage& data;
	ppu_thread& ppu;
	u32& pc;

	struct checkpoint_restore_options
	{
		bool readback, strict_readback, count_present_restore;
	};
	struct checkpoint_restore_result
	{
		bool started = false; // A failed write may already have changed guest memory.
		bool undo_ok = false, write_ok = false, voice_refused = false, voice_history_refused = false;
		bool restore_counted = false, readback_checked = false, readback_ok = true;
		u64 written = 0, rewind_ns = 0, readback_ns = 0, readback_diff = 0, readback_excluded = 0;
		u64 suspend_ns = 0;
		const char* suspend_skipped = nullptr;
		std::string readback_spans;
	};
	class checkpoint_voice_lease
	{
		ppu_thread& m_owner;
		bool m_acquired;

	public:
		explicit checkpoint_voice_lease(ppu_thread& owner) noexcept
			: m_owner(owner), m_acquired(pbd::fn_hook::voice_restore_begin(owner)) {}
		~checkpoint_voice_lease() noexcept
		{
			if (m_acquired)
				pbd::fn_hook::voice_restore_end();
		}
		checkpoint_voice_lease(const checkpoint_voice_lease&) = delete;
		checkpoint_voice_lease& operator=(const checkpoint_voice_lease&) = delete;
		bool owns(const ppu_thread& owner) const noexcept
		{
			return m_acquired && &m_owner == &owner;
		}
	};

	// The boundary's work, in order.
	void run();
	bool check_checkpoint_compatibility();
	bool prepare_live_boundary();
	bool open_boundary_presentation();
	void start_match_recording();
	bool reclaim_session_history(u32 index);
	bool claim_practice_history(u32 index);
	bool restore_direct_round(u32 index);
	void finish_fast_playback(u32 index);
	bool run_practice_controls(u32 index);
	void dispatch_tick(u32 index, update_sample& row, session_clock::time_point start);
	bool hold_game_overlay(u32 index);
	bool wait_for_replay_boot();
	bool match_standby();
	bool local_standby();

	// Match replay rows, row publication and mismatch evidence (pbd_rollback_session_rows.cpp).
	void replay_refuse(u32 boundary, const char* reason);
	void replay_events(const pbd::match_recording::events& rows);
	void replay_segment(u32 boundary);
	void replay_publish(u32 through);
	void publish_lobby_through(u32 through);
	void publish_through(u32 through);
	void write_mismatch_bundle(s32 failing);
	u32 write_memdump(const std::filesystem::path& stem, const std::vector<std::pair<u64, u64>>* only = nullptr);

	// The lobby: commands, VS cards, the players' names and ranks in the game's records, the session wait, selection sound and
	// timer, the set start (pbd_rollback_session_lobby.cpp).
	void lobby_poll_command();
	void lobby_display_command(const pbd::set_channel::command& command);
	void lobby_menu_command(const pbd::set_channel::command& command);
	bool lobby_wait_end();
	void publish_vs_cards(u32 scene);
	void publish_set_notices(u32 scene);
	void publish_game_overlay();
	bool store_game_profiles(const std::array<std::string_view, 2>& names, const std::array<u32, 2>& ranks);
	void write_game_profiles(u32 boundary, u32 scene);
	pbd::rank_stakes::game_values plan_rank_screens(const pbd::set_channel::command& command, bool next_set);
	void arm_rank_screens(const pbd::rank_stakes::game_values& values);
	void settle_rank_stakes(u32 segment);
	std::array<u32, 2> with_kept_promotions(std::array<u32, 2> ranks);
	void keep_promoted_ranks(u32 boundary);
	pbd::session_wait::leave_kind session_wait_leave();
	void leave_session_wait(u32 boundary);
	void lobby_selection_audio();
	pbd::select_timer::view select_view();
	bool lobby_set_start();
	void prepare_lobby_clean_start(pbd::lobby_set::clean_start& clean, u32& counter);
	bool admit_lobby_selection();

	// The peer link: lockstep and usable-input rules, the local input, send, poll and digests (pbd_rollback_session_peer.cpp).
	u32 hash_mask(s64 t);
	bool phase_lockstep(u32 i);
	bool all_known_at(u32 now);
	bool remote_left(u32 t);
	bool peer_usable(u32 t, u32 now);
	u16 peer_remote_for(u32 t, u32 now);
	u16 filter_local_sample(u16 raw, u32 source);
	u32 match_frame(u32 index);
	u16 scripted_local(u32 t, u32 boundary);
	u16 entry_value(u32 t, u32 back);
	void peer_send(u32 boundary);
	void append_outgoing_inputs(pbd::peer_message::input_message& m, u32 boundary);
	void peer_poll(u32 boundary);
	void peer_path_commands();
	u64 peer_pause_credit()
	{
		const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
		return data.peer.link.recovery.paused(now);
	}
	bool peer_wait_expired(session_clock::time_point from, u64 seconds, u64 initial_credit)
	{
		const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(session_clock::now().time_since_epoch()).count());
		const auto start = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(from.time_since_epoch()).count());
		const auto result = data.peer.link.recovery.timeout(start, now, seconds * 1000000, initial_credit);
		if (result == pbd::peer_recovery::wait_result::recovery_exhausted)
		{
			data.peer.fault = "peer_recovery_timeout";
			return true;
		}
		return result == pbd::peer_recovery::wait_result::deadline;
	}
	bool sync_random_panel(u32 segment_n);
	void take_segment_digest(u32 segment_n);

	// Replay takeover (pbd_rollback_session_takeover.cpp); the checkpoint restore it shares with the correction is defined in
	// this header.
	void save_takeover_host();
	void load_takeover_host(const takeover_host_slot& s);
	bool capture_anchor(takeover_anchor& a, u32 index, const takeover_host_slot& host, bool reserve_journal);
	bool capture_takeover(u32 index);
	void request_takeover_capture(u32 index);
	bool restore_takeover(const char* action, u32 side, u32 from);
	bool restore_anchor(takeover_anchor& a, std::span<takeover_anchor* const> journals, const char* action, u32 side, u32 from);

	// Replay rewind's checkpoints and restores (pbd_rollback_session_rewind.cpp).
	void rewind_schedule(u32 index, bool fighting, bool round_start);
	void rewind_capture(u32 index);
	void compress_rewind_points();
	bool end_spectator_standby(const std::filesystem::path& root, session_clock::time_point from, const char* reason, u32 how);
	u64 trim_rewind_points(u64 total);
	void rewind_clear();
	void rewind_forget_after(u32 index);
	bool rewind_restore(u32 match, u32 frame, u32 round, u32 from, bool exact = false);

	// Replay controls (pbd_rollback_session_replay.cpp).
	replay_control_result replay_controls_at(u32 index);

	// Practice's dummy recording and helpers (pbd_rollback_session_practice.cpp).
	u32 practice_dummy_saved();
	std::string practice_record_name();
	u32 practice_record_log_slot();
	u32 practice_record_row();
	void practice_measure_pending();
	void practice_clear_pending();
	bool practice_can_change_situation();
	bool practice_dummy_available();
	bool practice_dummy_all_zero();
	u64 practice_dummy_digest(const std::vector<u16>& frames);
	void practice_dummy_log(std::string text);
	void practice_dummy_publish();
	u32 practice_dummy_random();
	void practice_read_sides();
	void practice_sides_row(u32 index, u32 after, const char* at, const pbd::game_lifecycle::sides_sample& sample,
		bool provisional);
	void practice_sides_follow(u32 index);
	std::optional<pbd::practice::movement::screen_side> practice_side_of(u32 fighter);
	void practice_capture_inputs();
	void practice_capture_mark(const char* kind, u32 index);
	std::string practice_facing_json();
	void practice_read_moves(u32 index);
	void read_dummy_facing(bool paired);
	void learn_dummy_rest(u32 index, const std::optional<practice_move_sample>& dummy);
	void practice_save_input_sequence(const auto& slot, u32 log_slot, const std::string& name, u32 index);
	void practice_movement_refresh();
	void practice_pick_slot_move(u32 slot_index);
	void practice_slot_moves_refresh();

	// Practice menus and saved game options hold input and pause (pbd_rollback_session_practice.cpp).
	u16 practice_select_word(u16 raw);
	bool practice_memory_pause();

	// Practice's dummy playback (pbd_rollback_session_practice_dummy.cpp).
	bool practice_offense_movement_on();
	bool practice_offense_reacts();
	void practice_watch_contact(u32 index);
	std::optional<u32> release_dummy_route(u32 index, u32 singleton, u32 page, std::array<be_t<u32>, 2>& actions, bool controlled);
	std::optional<u32> apply_dummy_route(u32 index, u32 singleton, std::array<be_t<u32>, 2>& actions, bool driven, bool getup, bool lifted);
	void observe_dummy_playback_contact(u32 index, bool hit);
	bool choose_reaction_slot();
	std::optional<u16> dummy_throw_escape(u32 index);
	std::optional<u16> dummy_wakeup_word(u32 index);
	u32 practice_dummy_route(u32 index);
	void practice_getup_guard();
	void practice_guard_frame(const std::array<u32, 2>& contacts, bool hit);
	void practice_counter_hit_write(bool on);
	bool practice_dummy_choose_slot();
	void practice_dummy_begin_cycle();
	bool practice_dummy_begin_routine();
	bool practice_dummy_start_playback(u32 index);
	void practice_offense_begin(u32 index);
	void practice_restart_contact();
	void practice_restart_after_restore(u32 from);
	void practice_dummy_stop_playback(u32 index, const char* reason);
	void practice_dummy_start_record(u32 index);
	void practice_dummy_arm(u32 index);
	void practice_dummy_stop_record(u32 index, const char* reason, u32 chord_tail = 0);
	void practice_dummy_accept(u32 index, bool autoplay = true);
	u16 practice_dummy_word(u32 index);
	// The dummy's word when no playback cycle runs: none, the word being recorded, or the preview's.
	std::optional<u16> dummy_word_without_cycle(u32 index);

	// Practice's controls at the boundary (pbd_rollback_session_practice_controls.cpp).
	replay_control_result practice_controls_at(u32 index);
	void log_practice_select(u32 index);

	// The original stages, the sound options, the session's start and the in-game overlay (pbd_rollback_session_run.cpp).
	replay_control_result game_overlay_controls_at(u32 index, const auto& keep_peer);
	bool open_original_stages();
	bool apply_sound_options();
	bool session_start_sync();
	void record_start_digest(std::string& listing, u32& other_slot);
	bool wait_start_digest(u32 announce);

	// A watcher's boundary (pbd_rollback_session_lobby.cpp).
	bool spectator_standby();
	bool spectator_start_clean();
	bool spectator_boundary();
	void spectator_pacing(bool fast);

	// run()'s later steps (pbd_rollback_session_run_steps.cpp).
	bool session_segment_barrier();
	bool replay_segment_change(const std::array<u32, 2>& now_scene);
	bool peer_segment_change(const std::array<u32, 2>& now_scene);
	u32 prepare_peer_segment(u32 k, const std::array<u32, 2>& now_scene, u32& counter_now);
	bool peer_segment_ready(u32 n, u32 k, u32& target);
	bool abort_segment_wait(u32 n, u32 k, u32 target, bool emu_stopping, bool left_waiting, bool stop_seen, bool lobby_ended);
	bool wait_peer_segment(u32 n, u32 k, u32& target, bool& held, u64& waited);
	bool sync_segment_state(u32 n, u32 k, u32 counter_now, const std::array<u32, 2>& now_scene);
	bool exit_gate(u32 index);
	bool apply_replay_controls(u32 index);
	bool choose_inputs(u32 index, update_sample& row, session_clock::time_point start);
	void complete_row(u32 index, update_sample& row);
	bool wait_exit_gate(u32 s, u32 b, u32 lag);
	bool exit_gate_decided(u32 s, u32 b, u32 lag);
	bool choose_direct_input(u32 index, update_sample& row);
	void choose_practice_input(u32 index, update_sample& row);
	void choose_session_replay_input(u32 index, update_sample& row);
	void choose_peer_input(u32 index, update_sample& row);
	bool update_replay_round(u32 index, bool fighting);
	void complete_corrected_row(update_sample& row);
	void write_corrected_row(const update_sample& row);
	void complete_takeover_bridge(const update_sample& row);
	void complete_live_row(u32 index, update_sample& row);
	void write_peer_end(u32 index, const update_sample& row);

	// Defined at the end of this header: their return types are deduced, or they take any callable or slot, and several
	// files call them.
	auto peer_first_unusable(u32 now);
	bool restore_checkpoint_memory_leased(const checkpoint_voice_lease& voice_lease, ring_entry& entry, u32 from,
		auto&& apply_undo, checkpoint_restore_options options, checkpoint_restore_result& result);
	bool restore_checkpoint_memory(ring_entry& entry, u32 from, auto&& apply_undo,
		checkpoint_restore_options options, checkpoint_restore_result& result);
	auto practice_slot_frames(const auto& slot);
	auto practice_slot_playable(const auto& slot);
	auto& practice_record_slot();
	auto practice_pending_body();
	auto practice_pending_frames();
	auto practice_dummy_side();
	auto practice_active_movement();
	const char* practice_movement_label();
	auto practice_movement_alone();
	auto practice_dummy_reacting();
	bool practice_move_reactions_on();
};

// The rest of one boundary once its tick index and row are known: handle() runs the boundary's stages; fail() ends the
// run at a guard and soft_fail() records a round-transition guard and goes on. index and row refer to run()'s locals.
struct pbd::rollback_session::tick_dispatch : boundary_call
{
	const u32& index;
	update_sample& row;
	bool pbd_fight_frame = false;

	void fail(const char* message);
	void soft_fail(const char* message);
	void handle(bool soft_mode);
	void commit_spectator();

	// handle()'s parts, in the order they run: the entry stage and its steps, then the input stages.
	void entry_stage(bool soft_mode);
	bool prepare_battle_clock();
	bool adapt_frame_clock();
	bool read_boundary_input(bool soft_mode, bool& round_reset);
	bool verify_boundary_scene(bool scene_read, const std::array<be_t<u32>, 2>& scene_now, bool soft_mode);
	bool advance_native_match();
	bool verify_direct_replay();
	bool verify_spectator_replay();
	void write_boundary_snapshots();
	void record_match_options_digest();
	void record_match_stage_digest();
	void record_match_transition(bool battle, const std::array<be_t<u32>, 2>& scene_now);
	void read_lobby_battle(pbd::match_recording::observation& observed, pbd::lobby_set::context& context, const std::array<be_t<u32>, 2>& scene_now);
	void read_match_replay_context();
	void session_scenes(bool scene_read, const std::array<be_t<u32>, 2>& scene_now);
	bool lobby_set_observation(bool scene_read, const std::array<be_t<u32>, 2>& scene_now);
	void match_replay_observation(bool scene_read, const std::array<be_t<u32>, 2>& scene_now);
	bool tick_record(bool round_reset, const std::array<be_t<u32>, 2>& scene_now, bool soft_mode);
	bool correction();
	void close_replayed_interval();
	bool peer_boundary(bool& roll_trigger);
	bool auto_input_delay();
	void assign_phase_delay(bool lockstep);
	void sync_timing(session_clock::time_point hold_from, bool lockstep, double& one_way_us,
		session_clock::time_point& stall_until, double& sync_error);
	bool restore_window(bool roll_trigger);
	bool apply_correction_undo();
	void write_correction_summary(const char* reason);
	void wait_correction_renderer();
	void wait_correction_locks();
	bool restore_correction_memory(ring_entry& entry);
	void rearm_correction_entry(ring_entry& entry);
	void start_corrected_execution(const ring_entry& entry);
	bool resume_correction_clock(const ring_entry& entry);
	void adjust_peer_timeline(bool error_valid, double error, bool lockstep);
	void balance_peer_timeline(session_clock::time_point hold_from, bool lockstep, session_clock::time_point& stall_until);
	bool control_hold();
	bool read_tick_counters(tick_sample& tick, bool soft_mode);
	void hash_tick_combat(tick_sample& tick, bool relaxed_scope);
	void hash_tick_generator(tick_sample& tick, bool relaxed_scope);
	void hash_tick_effects(tick_sample& tick);
	bool verify_tick_phase(tick_sample& tick, bool pbd_round_cut, bool soft_mode);
	void capture_tick_checkpoint(tick_sample& tick);
	void reset_undo_journal(ring_entry& entry);
	void arm_tick_checkpoint(tick_sample& tick, ring_entry& entry);
	void capture_replay_checkpoints(tick_sample& tick);
	void capture_held_checkpoint(const char* phase);
	void write_roundtrip_record(const std::string& text);
	void capture_roundtrip_origin();
	void resume_roundtrip_tick();
	bool restore_roundtrip_origin();
	bool repeat_branch_input();
	bool roundtrip_compare();
	void read_replay_score(pbd::match_recording::observation& observed);
	void input_stages(bool soft_mode);
	bool read_input_counter(bool soft_mode);
	bool inject_input(u32 port, port_sample& sample, u32 raw);
	bool verify_input(u32 port, port_sample& sample, u32 raw);
	void read_practice_input(u32 port, const port_sample& sample, u32& raw);
	void apply_practice_pause_input(u32 port, u32& raw, bool& practice_press);
};

// The steps of practice_controls_at() at one boundary: the pause menu and its settings, the native calls they queue,
// and the pad reading and menu publishing they share. index and restored refer to practice_controls_at()'s tick
// index and its flag for a restore made at this boundary.
struct pbd::rollback_session::practice_steps : boundary_call
{
	u32& index;
	bool& restored;

	// Restart Position choices in menu order; the placement enum also has the round-start value the menu never offers.
	static constexpr std::array<pbd::game_lifecycle::placement, 7> practice_restart_choices{
		pbd::game_lifecycle::placement::centre, pbd::game_lifecycle::placement::remembered,
		pbd::game_lifecycle::placement::left_wall, pbd::game_lifecycle::placement::right_wall,
		pbd::game_lifecycle::placement::foreground_wall, pbd::game_lifecycle::placement::background_wall,
		pbd::game_lifecycle::placement::random};
	// The game's five practice options (Dummy Action, Counter Hit, Game Input Display, Game Attack Data, Game Hit
	// Analysis) at their offsets in DR's practice object, with the largest value each accepts.
	static constexpr std::array<u32, pbd::practice::game_option_count> practice_option_offsets{0x6c, 0x30, 0x3c, 0x14, 0x18};
	static constexpr std::array<u32, pbd::practice::game_option_count> practice_option_maximum = pbd::practice::game_option_maximum;
	// A value row's dropdown: its options and the one in effect. Action rows (Resume, Restart, Remember, Exit, a slot
	// while a take is being recorded) have none, so Confirm acts on them directly.
	using practice_choice_list = pbd::practice::menu::choice_list;

	std::optional<replay_control_result> opening_request();
	void inspect_opening_request(pbd::practice::start::state& entry, session_clock::time_point now);
	void prepare_opening_selection(pbd::practice::start::state& entry, session_clock::time_point now, bool context, u32 mode, u32 scene, u32 pending);
	void advance_opening_selection(pbd::practice::start::state& entry, session_clock::time_point now, bool context, u32 mode, u32 scene, u32 pending);
	void publish_opening_result(pbd::practice::start::state& entry, u32 mode);
	void opening_refuse(pbd::practice::start::state& entry, session_clock::time_point now, const char* reason);
	void observe_frame();
	void update_movement_streak();
	void update_input_precision();
	void discard_unplayed_input(const practice_pause_observation& pause, bool advancing);
	void update_input_history(bool advancing);
	void update_damage(const practice_pause_observation& pause, bool advancing, bool counter_jump);
	void publish_frame_data(const practice_pause_observation& pause);
	void judge_punishment(const practice_pause_observation& pause);
	std::optional<replay_control_result> observe_stage_load();
	void scripted_recording();
	void update_dummy_moves();
	std::optional<replay_control_result> apply_reset();
	void abandon_reset(const char* reason);
	void complete_reset(const pbd::game_lifecycle::practice_reset& command, const pbd::game_lifecycle::receipt& receipt);
	void log_reset_receipt(const pbd::game_lifecycle::practice_reset& command, const pbd::game_lifecycle::receipt& receipt);
	void practice_wall_report();
	std::string write_wall_polygons(const pbd::game_lifecycle::scope& lifetime, const pbd::game_lifecycle::wall_report& report, bool general, bool all);
	void trace_pad(u16 host, u16 pad, u16 pressed);
	void log_health();
	void close_panel();
	void resume_native();
	void leave();
	void queue_native(pbd::practice::native::operation kind, u32 value = 0,
		pbd::practice::native::option choice = pbd::practice::native::option::action);
	void queue_sound(pbd::practice::native::sound value);
	u32 pointer_context();
	auto lifecycle_scope();
	void lesson_boundary();
	pbd::practice::pbd_lesson_game read_lesson_game();
	void remember_position();
	bool remembered_moment_ready();
	void queue_reset(pbd::game_lifecycle::placement target, pbd::game_lifecycle::placement_variant variant);
	void restart_chord(u16 chord);
	void open_panel();
	u32 practice_row_count();
	u32 practice_row_step(bool down);
	auto practice_slot_name(u32 slot);
	std::optional<u32> practice_defense_row(u32 row);
	std::optional<u32> practice_focused_slot();
	bool practice_frequency_row();
	std::optional<std::array<u32, pbd::practice::game_option_count>> practice_game_values();
	practice_choice_list practice_choices(u32 row);
	std::optional<u32> practice_sub_list_opener(u32 row);
	std::string practice_reaction_name();
	std::string practice_moves_label(const std::vector<u32>& ids);
	void practice_open_move_list(u32 target);
	void practice_close_move_list();
	void practice_move_list_input(u16 pressed, bool select);
	void log_display();
	void practice_set_training_mode(pbd::practice::training_mode mode);
	void practice_choose(u32 row, u32 option);
	bool choose_offense_reaction(u32 option);
	bool choose_training_setting(u32 row, u32 option);
	bool choose_offense_movement(u32 row, u32 option);
	bool choose_restart_setting(u32 row, u32 option);
	bool choose_display_setting(u32 row, u32 option);
	void choose_defense_setting(u32 row, u32 option);
	bool choose_defense_movement(u32 row, u32 option);
	bool clear_dummy_slot(u32 row);
	bool practice_dummy_settled();
	void practice_dummy_settle(const char* reason);
	pbd::practice::selection_gate practice_selection_gate();
	void restore_defaults();
	void restore_offense_defaults();
	bool shown_row_enabled(u32 row);
	void open_confirm(pbd::practice::confirm_kind kind);
	void queue_selection(bool characters);
	void answer_confirm(bool yes);
	void publish();
	void read_pad();
	void update_pause_requests();
	bool practice_leave_held(bool select, bool start, session_clock::time_point now);
	std::array<bool, 4> requested_practice_actions(u16 pressed, bool select, bool start);
	bool wait_practice_reset(u16 pad, bool select, u16 chord_pressed);
	bool stop_recording_action(bool start_press, bool stop_chord, bool takeover, std::size_t stop_chord_tail);
	void open_requested_panel(u16 pad, bool start_press, bool settings_pause, bool takeover);
	void stop_unreadable_dummy(u16 pad, session_clock::time_point now);
	bool release_practice_input(u16 pad, bool select, u16 chord_pressed);
	bool read_without_pause(u16 pressed);
	void select_circle_chord(session_clock::time_point now);
	void mouse_option(const pbd::practice::mouse::command& mouse, u16& pressed, bool list_menu, bool& accepted);
	void mouse_prompt(const pbd::practice::mouse::command& mouse, u16& pressed, bool list_menu, bool& accepted);
	void mouse_row(const pbd::practice::mouse::command& mouse, u16& pressed, bool list_menu, bool& accepted);
	bool navigate_dropdown(u16 pressed);
	bool navigate_categories(u16 pressed);
	void activate_practice_row();

	// read_pad()'s parts: the mouse on the menu, the take's question, the menu's controls and the dummy chords.
	void menu_pointer(u16 pad, u16& pressed, bool settings_claim);
	bool confirm_dialog(u16 pressed);
	bool menu_navigation(u16 pressed, bool select, bool settings_resume);
	void record_chord(u16 pressed, bool select, u16 chord_pressed, session_clock::time_point now);

	struct native_menu;
	void publish_menu_identity();
	u32 publish_dummy_state();
	void publish_categories();
	void publish_control_hints(bool situation_ready, bool remembered_ready, bool r3_remembers);
	void publish_native_menu(bool decision, bool remembered_ready, bool situation_ready, u32 timer_frames);
	std::optional<replay_control_result> native_calls(bool& guest_called);
	std::optional<replay_control_result> native_outcome(const pbd::practice::native::request& command,
		const pbd::practice::native::result& result, bool silent);

	// Practice's memory at a fight and its saves (pbd_rollback_session_practice.cpp).
	void memory_boundary();
	void memory_fight();
	void memory_dummy(u32 character);
	void memory_restore();
	void memory_restore_done(const char* reason);
	void memory_save();
	practice_choice_list recording_set_choices(bool loading);
	void recording_set_choose(bool loading, u32 position);
	bool recording_set_name_input(u16 pressed);
	void recording_set_save();
	void recording_set_apply();
	void recording_set_poll();
	pbd::practice_store::dummy_rows memory_rows();
	pbd::practice_store::recordings memory_slots(u32 character);
	void memory_apply(const pbd::practice_store::dummy_rows& rows, const pbd::practice_store::recordings& slots);
};

// The steps of replay_controls_at() at one boundary: pause, frame step, speed, the viewer's round and side choices
// and the controller taking a side. index and fighting_now refer to replay_controls_at()'s tick index and its
// reading of whether a fight is running.
struct pbd::rollback_session::replay_steps : boundary_call
{
	u32& index;
	const bool& fighting_now;

	void log_event(const char* kind, s32 value);
	void apply_speed();
	void set_pause(bool paused);
	void set_speed(s32 speed);
	void take(u32 port);
	void publish();
	void start_takeover(u32 port);
	void arm_takeover(u32 port);
	void show_arm_prompt(u32 port, bool was_paused);
	void cancel_arm_prompt();
	void show_prompt(u16 held = pbd_local_pad_word());
	void jump(const char* action, u32 match, u32 frame, u32 side);
	void try_again();
	void return_to_replay();
	void previous_round();
	void next_round();
	void viewer_action(u16 pad);
	void prepare_control_boundary();
	void take_requested_moment();
	bool scripted_actions();
	void scripted_action(const replay_test_action& a);
	bool scripted_playback_action(const replay_test_action& a);
	bool finish_takeover_action();
	replay_control_result wait_control_release();
	void bookmark_situation();
	void read_pad();
	static u16 keyboard_word() noexcept;
	static u64 now_ms() noexcept;

	// Rewind, fast forward, round jumps and instant Take Control (pbd_rollback_session_rewind.cpp).
	void begin_fast_to(u32 match, u32 frame);
	void seek_to(u32 frame, bool exact = false);
	void seek_by(s32 delta);
	void go_to_round(u32 round);
	void take_over(u32 side);
	bool seek_poll();
	void settings_pause();
};

// The playback steps of practice_dummy_word() that end a reaction or start the next cycle. index refers to
// practice_dummy_word()'s tick index.
struct pbd::rollback_session::dummy_word_steps : boundary_call
{
	u32& index;

	void end_reaction(const char* reason);
	bool next_cycle();
};

#include "../PBD/pbd_session_boundary_templates.h"
