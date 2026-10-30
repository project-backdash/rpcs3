# The PBD unit tests in rpcs3_test. They build with C++ exceptions, since a failed check throws to end its test (see
# pbd_test.h); the rest of the target builds without them.
set(PBD_TESTS
    ${CMAKE_CURRENT_LIST_DIR}/practice_moment_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/match_bind_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay_entry_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay_resource_reference_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay_bank_offset_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay_carry_codec_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay_actor_records_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay_difference_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay_switches_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/peer_auth_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/peer_log_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/peer_wait_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/character_rule_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pick_transition_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/controller_takeover_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/controller_mapper_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/player_seats_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/versus_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/input_taps_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/clip_buffer_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/corner_chat_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/env_parse_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/env_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/frame_tags_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/game_host_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/game_overlay_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/game_profile_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/search_badge_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/hidden_selection_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/json_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/lobby_set_driver_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/match_recording_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/memory_dump_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/output_gain_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/peer_message_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/peer_message_conformance_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_controls_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_dummy_state_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_floor_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_guard_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_history_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_menu_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_movement_presets_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_movement_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_random_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_stage_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_store_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_sets_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_streak_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_precision_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_walls_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/presentation_rows_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/function_hook_config_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ppu_object_name_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/raw_keyboard_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rematch_json_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rematch_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/render_scale_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay_viewer_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay_rewind_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/select_band_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/select_timer_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/session_history_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/session_policy_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/session_start_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/undo_restore_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/set_channel_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/set_notices_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/settings_menu_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/setup_digest_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/skip_guard_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sound_options_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/spectator_channel_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/spectator_standby_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/spectator_ui_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/stage_rule_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/stage_sync_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/vs_cards_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/portable_parser_random_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/spectator_packet_random_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/set_channel_parser_audit_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/spectator_parser_audit_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/replay_game_file_parser_audit_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/json_reader_randomized_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/input_script_format_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/input_live_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/owner_flags_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_wakeup_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_throw_escape_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_lesson_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_readiness_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_punish_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_restart_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/practice_input_delay_test.cpp
)

# The peer address test uses Winsock.
if(WIN32)
    list(APPEND PBD_TESTS ${CMAKE_CURRENT_LIST_DIR}/peer_endpoint_test.cpp)
endif()

target_sources(rpcs3_test PRIVATE ${PBD_TESTS})

set_source_files_properties(${PBD_TESTS} PROPERTIES
    COMPILE_FLAGS "$<IF:$<CXX_COMPILER_ID:MSVC>,/EHsc,-fexceptions>"
    SKIP_PRECOMPILE_HEADERS ON
)
