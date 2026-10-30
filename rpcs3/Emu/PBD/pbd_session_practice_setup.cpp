// Local Practice setup, saved settings and menu channels.
#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_env_parse.h"
#include "pbd_emulator_identity.h"
#include "pbd_replay_standby.h"
#include "pbd_replay_capture.h"
#include "pbd_spectator_standby.h"

#ifdef _WIN32
void pbd::rollback_session::configure_practice_rewind(const wchar_t* practice)
{
	require(std::wstring_view(practice) == L"1", "practice_rewind_invalid");
	auto& s = *m_storage;
	require(s.session.enabled && s.round_lifecycle && !s.peer.enabled && !s.replay.session_replay && !s.replay.controls,
		"practice_rewind_requires_local_session");
	require(!s.checkpoint.capture_directory.empty(), "practice_rewind_needs_probe_dir");
	s.practice.rewind = true;
	if (const auto mapping = pbd::env::find_wide("PBD_PRACTICE_BUTTON_MAP"); mapping && *mapping)
	{
		const std::wstring wide(mapping);
		const std::string narrow(wide.begin(), wide.end());
		require(pbd::practice::controls::parse_mapping(narrow, s.practice.button_map), "practice_button_map_invalid");
	}
	if (const auto glyphs = pbd::env::find_wide("PBD_PRACTICE_GLYPHS"); glyphs && *glyphs)
	{
		const std::wstring_view value(glyphs);
		require(value == L"playstation" || value == L"xbox" || value == L"keyboard", "practice_glyphs_invalid");
		s.practice.glyphs = value == L"xbox"     ? pbd::game_overlay::glyph_family::xbox :
		                    value == L"keyboard" ? pbd::game_overlay::glyph_family::keyboard :
		                                           pbd::game_overlay::glyph_family::playstation;
	}
	s.practice.menu.hint_pending = pbd::env::find("PBD_PRACTICE_HINT") && std::string_view(pbd::env::find("PBD_PRACTICE_HINT")) == "1";
	configure_practice_memory();

	s.replay.takeover_fast = true;
	s.correction.replay_skip_draws = true;
	s.correction.withhold_exact = true;
	s.replay.fast.saved_vblank = g_cfg.video.vblank_rate.get();
	if (const auto test = pbd::env::find_wide("PBD_PRACTICE_REWIND_TEST"); test && *test)
	{
		unsigned remember = 0, restart = 0;
		require(std::swscanf(test, L"%u:%u", &remember, &restart) == 2 && remember >= 1 && restart > remember,
			"practice_rewind_test_invalid");
		s.practice.test.remember = remember;
		s.practice.test.restart = restart;
	}
	const auto control_path = (s.checkpoint.capture_directory / "practice-rewind.jsonl").u8string();
	require(s.replay.control_file.open(std::string(control_path.begin(), control_path.end()), fs::write_new + fs::isfile),
		"cannot_create_practice_rewind_exclusively");
	// The input capture is written whatever the quiet setting, and Practice runs the same when it cannot be opened.
	if (s.practice.inputs.open(s.checkpoint.capture_directory / "practice-inputs.jsonl"))
		s.practice.inputs.append("{\"type\":\"legend\",\"schema\":1,"
								 "\"frame\":\"game frames that really ran, counted from the start of Practice (pauses and menus excluded)\","
								 "\"port\":\"1 is the player (P1), 2 the dummy (P2)\","
								 "\"word\":\"pad word the game read: up 0x1000, right 0x2000, down 0x4000, left 0x8000; 1 0x0080, 2 0x0010, 3 0x0040, 4 0x0020\","
								 "\"dirs\":\"Tekken notation for the fighter's facing: f is toward the opponent, n is neutral\","
								 "\"rows\":\"input: a port's word from that frame on; move: the fighter record's +0xC0 / +0xC4 move-table entries after that frame (rest: the dummy's learned resting stance; facing: whether it faces P1, with its yaw's and its 16-bit facing's angles from P1 in degrees); restart, rewind: a new stretch starts\"}\n");
	pbd_session_log.warning("Practice exact-situation diagnostics (%s).",
		s.practice.test.remember == umax ? "disabled" : "scripted gate");
}

void pbd::rollback_session::configure_practice_dummy(const wchar_t* dummy)
{
	require(std::wstring_view(dummy) == L"1", "practice_dummy_invalid");
	auto& s = *m_storage;
	require(s.practice.rewind && s.session.enabled && s.round_lifecycle && !s.peer.enabled && !s.replay.session_replay && !s.replay.controls,
		"practice_dummy_requires_local_practice");
	s.practice.dummy.enabled = true;
	for (u32 i = 0; i < s.practice.dummy.slots.size(); ++i)
	{
		auto& slot = s.practice.dummy.slots[i];
		slot.frames.reserve(1800);
		slot.name = fmt::format("Slot %u", i + 1);
	}
	s.practice.dummy.pending.words.reserve(1800);
	if (const auto test = pbd::env::find_wide("PBD_PRACTICE_DUMMY_TEST"); test && *test)
	{
		unsigned start = 0, stop = 0, word = 0, end = 0;
		require(std::swscanf(test, L"%u:%u:%u:%u", &start, &stop, &word, &end) == 4 && start >= 1 &&
					stop > start && end > stop && word <= 0xffff,
			"practice_dummy_test_invalid");
		s.practice.dummy.test.start = start;
		s.practice.dummy.test.stop = stop;
		s.practice.dummy.test.word = static_cast<u16>(word);
		s.practice.dummy.test.end = end;
	}
	const auto dummy_path = (s.checkpoint.capture_directory / "practice-dummy.jsonl").u8string();
	require(s.practice.dummy.file.open(std::string(dummy_path.begin(), dummy_path.end()), fs::write_new + fs::isfile),
		"cannot_create_practice_dummy_exclusively");
	pbd_session_log.warning("Practice dummy: eight host-side slots on (%s); SELECT+CIRCLE records, START or SELECT+CIRCLE stops.",
		s.practice.dummy.test.start == umax ? "live controls" : "scripted gate");
}

void pbd::rollback_session::configure_practice()
{
	if (const auto practice = pbd::env::find_wide("PBD_PRACTICE_REWIND"); practice && *practice)
	{
		configure_practice_rewind(practice);
	}
	if (const auto dummy = pbd::env::find_wide("PBD_PRACTICE_DUMMY"); dummy && *dummy)
	{
		configure_practice_dummy(dummy);
	}
	if (const auto channel = pbd::env::find_wide("PBD_MENU_CHANNEL"); channel && *channel)
	{
		auto& s = *m_storage;
		s.game_overlay.dir = std::filesystem::path(std::wstring(channel));
		std::error_code channel_error;
		const bool channel_directory = std::filesystem::is_directory(s.game_overlay.dir, channel_error);
		require(s.session.enabled && g_cfg.misc.use_native_interface && s.game_overlay.dir.is_absolute() && !channel_error && channel_directory,
			"game_overlay_requires_session_directory");
		s.game_overlay.enabled = true;
		s.game_overlay.command = s.game_overlay.dir / "menu.pbd";
		s.game_overlay.search.file = s.game_overlay.dir / "search.pbd";
		pbd::game_overlay::publish({});
		pbd::search_badge::publish({});
		pbd_session_log.notice("Project Backdash in-game menu channel on (bounded local files; no guest-memory access).");
		// The notices' pictures the launcher made from the player's own game files (PBD_NOTICE_ART), read once for the overlays.
		if (const auto art = pbd::notice_art::load())
		{
			if (art->recipe)
				pbd_session_log.notice("Notice pictures: %u of %u read.", art->found, pbd::notice_art::picture_count);
			else
				pbd_session_log.warning("Notice pictures unavailable (%s): the notices draw their lettering alone.",
					pbd::notice_art::directory().empty() ? "PBD_NOTICE_ART names no folder" : "the folder is missing or from another recipe");
		}
	}
	require(!m_storage->practice.rewind_quiet || m_storage->practice.rewind,
		"practice_rewind_quiet_requires_practice");
}

// Practice's memory, when the launcher names its store (PBD_PRACTICE_STORE): local Practice only, never with another PC or a
// lobby set (the in-game Settings menu's gate). The player's own rows apply here, before the first Practice view is published;
// a dummy's rows and slots, the saved game options follow at each fight (practice_steps::memory_fight). A
// store that cannot be used leaves Practice as it is without one.
void pbd::rollback_session::configure_practice_memory()
{
	auto& s = *m_storage;
	m_practice_memory.reset();
	const auto* root = pbd::env::find_wide("PBD_PRACTICE_STORE");
	if (root && *root && pbd::settings_menu::available() && !pbd::env::find("PBD_PEER_ROLE") &&
		!pbd::env::find("PBD_SET_CHANNEL") && std::filesystem::path(root).is_absolute())
	{
		auto memory = std::make_unique<practice_store::memory>();
		memory->root = std::filesystem::path(root);
		auto loaded = practice_store::load_profile(memory->root);
		if (loaded.state == practice_store::load_state::set_aside)
			pbd_session_log.warning("Practice memory: profile.json could not be read (%s); it was set aside.", loaded.error);
		memory->saved = std::move(loaded.value);
		auto library = practice_store::load_library(memory->root);
		for (const auto& refused : library.set_aside)
			pbd_session_log.warning("Practice memory: a recordings file could not be read (%s); it was set aside.", refused);
		memory->library = std::move(library.dummies);
		memory->sets.read(memory->root.parent_path());
		for (const auto& refused : memory->sets.set_aside)
			pbd_session_log.warning("Practice memory: a recording set could not be read (%s); it was set aside.", refused);
		s.practice.training_mode = memory->saved.mode;
		s.practice.view.command_history = memory->saved.view.command_history;
		s.practice.view.attack_info = memory->saved.view.attack_info;
		s.practice.view.frame_data = memory->saved.view.frame_data;
		s.practice.view.control_hints = memory->saved.view.control_hints;
		s.practice.view.movement_streaks = memory->saved.view.movement_streaks;
		s.practice.restart.position = practice_store::live_restart(memory->saved.restart);
		s.practice.restart.variant = {.switched = memory->saved.switch_positions, .dummy_back = memory->saved.dummy_back};
		s.practice.input_delay_frames = memory->saved.input_delay;
		s.practice.reactions.guard.choose(memory->saved.guard);
		s.practice.reactions.counter_hit.choose(memory->saved.counter_hit_random);
		const bool saved = loaded.state == practice_store::load_state::loaded;
		pbd_session_log.notice("Practice memory on (%s profile, %u dummies).", saved ? "saved" : "fresh",
			static_cast<u32>(memory->saved.dummies.size()));
		m_practice_memory = std::move(memory);
	}
	else if (root && *root)
		pbd_session_log.warning("Practice memory: the store is unavailable here (not local Practice, or not an absolute path).");
	s.practice.view.active = true;
	pbd::practice::publish(s.practice.view);
}

#endif
