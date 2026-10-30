#pragma once

// A lobby set's, a watcher's and the launcher menu channel's state. Members of pbd::rollback_session::storage
// (pbd_rollback_session_storage.h).

#include "pbd_rollback_session_common.h"

namespace pbd::session_runtime
{
	// A lobby set: its channel, selection, select countdown, VS cards and standby.
	struct lobby_state
	{
		std::optional<std::filesystem::path> match_standby;
		std::optional<bool> match_spectator_from_start;
		bool local_standby_done = false;
		std::unique_ptr<pbd::set_channel::channel> channel;
		std::unique_ptr<pbd::lobby_set::driver> set;
		std::unique_ptr<pbd::hidden_selection::driver> selection;
		pbd::rematch::native_load rematch;
		// The hidden selection holds the in-game menu's mute (game_overlay.audio_muted) while it runs.
		bool selection_audio_muted = false;
		// Fixed picks (PBD_LOBBY_PRESELECT=1) use the stock resource loader, behind the loading card. In-game sets use the
		// visible select and its countdowns; their repicks steer only the kept fighter and stage through lobby.selection.
		bool preselect = false;
		std::optional<pbd::select_timer::timer> select_timer;
		// The last local sample that composed the countdown band (pbd::select_band); a repeated source redraws nothing.
		std::optional<u32> select_band_source;
		// The launcher's vs_cards set command shows the VS cards (pbd::vs_cards). Until it arrives, or after it hid them, no VS
		// card is drawn.
		bool vs_cards_on = false;
		// A Ranked set's stakes from the launcher's rank_stakes set command (pbd::rank_stakes), for the running set only: next_set
		// drops them until the new set's arrive.
		std::optional<pbd::rank_stakes::stakes> rank_stakes;
		// Stakes both peers announced at the last scene barrier, used to plan the next game's rank screens; next_set clears them.
		std::optional<pbd::rank_stakes::stakes> agreed_stakes;
		bool vs_cards_shown = false; // the last composition was on a VS screen
		// The Ranked notices (pbd::set_notices) from the launcher's vs_cards and rank_stakes: where the set came from, each side's
		// fighter caption and each placing side's count (next_set drops the counts until the new set's stakes arrive); whether this
		// game is a drawn game played again; whether a fresh boundary of this game has shown its VS screen, which lifts the
		// loading cover, and its fight, which ends its first character intro. The display commands' invalid optional fields are
		// logged once.
		pbd::set_channel::card_mode vs_mode = pbd::set_channel::card_mode::none;
		std::array<std::string, 2> vs_characters;
		std::array<std::optional<pbd::rank_stakes::placement>, 2> placements;
		bool game_replay = false, vs_seen = false, fight_seen = false, ignored_logged = false;
		bool start_attempted = false;
		bool battle_seen = false;
		std::optional<pbd::set_channel::command> pending_command;
		u64 menu_generation = 0;
		session_clock::time_point next_poll{};
		// The first finish() caller saves the termination facts; finalization reads them after all callers have joined.
		pbd::lobby_set::end_facts end;
		std::atomic<bool> leave_decided{false};
		pbd_direct_match direct_match;
		bool direct_match_end_pending = false;
	};
	// Spectating a lobby game.
	struct spectator_state
	{
		std::unique_ptr<pbd::spectator::local_channel> channel;
		// A standby watcher's channel until the launcher binds it to a set (pbd_rollback_session_lobby.cpp).
		std::optional<std::filesystem::path> standby;
		std::optional<pbd::spectator::frame> frame;
		std::array<u16, 2> expected{};
		bool was_battle = false, intro_seen = false;
		int saved_vblank = 60;
		bool saved_frame_limit_disabled = false, saved_skip_draws = false;
		bool fast = false, quiet = false;
		u32 game = 0;
		fs::file execution_file;
	};
	// The launcher-to-game menu channel.
	struct game_overlay_state
	{

		// Shared launcher-to-game menu channel. Only the owner update thread mutates this state; the RSX thread receives a copy through
		// pbd::game_overlay::publish. A claimed controller remains neutral to the guest until every menu button is released; in a
		// peer session that neutral word is this PC's frozen sample (filter_local_sample), so both games execute it.
		bool enabled = false, claim = false, ignore_until_release = false, audio_muted = false;
		std::filesystem::path dir, command;
		pbd::game_overlay::snapshot panel{};
		// The selected= of the newest launcher file, which may differ from the panel's highlight once the player moved it
		// (pbd::game_overlay::republished_highlight).
		u32 file_selected = 0;
		session_clock::time_point next_poll{};
		// Write time of the newest command file that parsed; an unchanged file is not opened again (see poll_command).
		std::filesystem::file_time_type read_written{};
		bool read_valid = false;
		u64 event_serial = 0;
		u16 pad_prev = 0;
		// Buttons the claim saw down that this PC's admitted samples have not yet shown released (pbd::game_overlay::hold_buttons,
		// admit_local_sample). Read only at a peer session's local admission.
		u16 hold_buttons = 0;
		std::string last_error;
		// search.pbd beside menu.pbd: the matchmaking search badge (pbd::search_badge).
		pbd::search_badge::reader search{};
	};
} // namespace pbd::session_runtime
