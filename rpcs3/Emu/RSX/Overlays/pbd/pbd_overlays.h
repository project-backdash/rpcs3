#pragma once

// The PROJECT BACKDASH overlays, one per screen element. Each draws only from the snapshot its channel publishes and
// never reads the pad, a file or guest memory. reset_performance_overlay() creates them in drawing order.

#include "Emu/RSX/Overlays/overlays.h"
#include "Emu/Cell/pbd_live_stats.h"
#include "Emu/Cell/pbd_game_overlay.h"
#include "Emu/Cell/pbd_search_badge.h"
#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/Cell/pbd_corner_chat.h"
#include "Emu/PBD/pbd_spectator_channel_input.h"
#include "Emu/Cell/pbd_session_wait.h"
#include "Emu/Cell/pbd_select_band.h"
#include "Emu/Cell/pbd_vs_cards.h"
#include "Emu/PBD/pbd_set_notices.h"
#include "Emu/PBD/pbd_settings_runtime.h"

#include <string>
#include <string_view>

namespace rsx
{
	namespace overlays
	{
		class display_manager;

		// Match status from pbd_live_stats, plus replay controls and Practice pause/leave prompts. PBD_NET_OVERLAY enables
		// match statistics; replay and Practice controls have their own visibility rules. Hidden while stopping.
		struct pbd_net_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;
			void set_render_viewport(u16 width, u16 height) override;

		private:
			compiled_resource m_compiled{};
			f32 m_output_scale = 1.f;
			u64 m_next_refresh_us = 0;
			bool m_show = false;
			bool m_leave_hold = false;
			void refresh_now();
		};

		// Lobby menus from pbd::game_overlay and the pause menu from pbd::practice.
		struct pbd_game_menu_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;

		private:
			::pbd::game_overlay::snapshot m_snapshot{};
			u64 m_next_refresh_us = 0;
			bool m_show = false;
			// Both layouts are built directly as batched draw commands (see overlay_pbd_draw.h).
			compiled_resource m_native_compiled{};
			f32 m_output_scale = 1.f;
			// Set complete's counter flip: the panel command and count it last played for, and when it started (steady ms).
			u64 m_flip_key = 0, m_flip_started_ms = 0;
			void refresh_now();
			void refresh_native(const ::pbd::practice::snapshot& practice);
			void refresh_lobby();

		public:
			void set_render_viewport(u16 width, u16 height) override;
		};

		// Frame data and input history from the Practice snapshot published at simulation boundaries.
		struct pbd_practice_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;

			void set_render_viewport(u16 width, u16 height) override;

		private:
			::pbd::practice::snapshot m_snapshot{};
			compiled_resource m_compiled{};
			f32 m_output_scale = 1.f;
			bool m_show = false;
			void refresh_now(u64 timestamp_us);
		};

		// Fading corner messages from pbd::corner_chat. A stale emulation generation must never draw over a new session.
		struct pbd_corner_chat_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;

		private:
			::pbd::corner_chat::snapshot m_snapshot{};
			compiled_resource m_compiled{};
		};

		// Preparation cover and verified playhead from pbd::spectator; fight-only chat, controls and profiles from
		// pbd::spectator_ui. Preparation covers the game opaquely until the playhead is verified.
		struct pbd_spectator_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;
			void set_render_viewport(u16 width, u16 height) override;

		private:
			compiled_resource m_compiled{};
			f32 m_output_scale = 1.f;
			bool m_cover = true;
			bool m_analyze = false;
			u64 m_screen_revision = 0;
		};

		// Character/stage selection countdown and controls from pbd::select_band; hidden outside selection.
		struct pbd_select_band_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;
			void set_render_viewport(u16 width, u16 height) override;

		private:
			compiled_resource m_compiled{};
			f32 m_output_scale = 1.f, m_compiled_scale = 0.f;
			u64 m_revision = ~u64{0};
			void refresh(const ::pbd::select_band::snapshot& band);
		};

		// Set score and stakes on the VS screen; a covered watcher also sees fighter names and plates.
		// Created before the menu so panels draw above it.
		struct pbd_vs_cards_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;

		private:
			compiled_resource m_compiled{};
			u64 m_revision = ~u64{0};
			void refresh(const ::pbd::vs_cards::snapshot& cards);
		};

		// The Ranked notices over a fight (pbd::set_notices): the gold Placement Match pill on a placing player's character intro
		// and the placement plate over the game's Beginner plate while the game draws it, both from the player's own
		// game files. Draw only: it reads the snapshot the update latch composes at fresh boundaries, never the pad, a file or
		// guest memory. Created before the menu overlay, so a launcher panel and the in-game menus draw over it.
		struct pbd_notice_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;

		private:
			compiled_resource m_compiled{};
			u64 m_revision = ~u64{0};
			void refresh(const ::pbd::set_notices::snapshot& set);
		};

		// Peer loading/Leave dialog from pbd::session_wait. Created after the menu so it covers the lobby loading card.
		struct pbd_wait_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;
			void set_render_viewport(u16 width, u16 height) override;

		private:
			compiled_resource m_compiled{};
			f32 m_output_scale = 1.f;
		};

		// Matchmaking badge from pbd::search_badge. Drawn above the Practice pause menu; hidden while a launcher panel shows.
		struct pbd_search_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;
			void set_render_viewport(u16 width, u16 height) override;

		private:
			compiled_resource m_compiled{};
			f32 m_output_scale = 1.f;
		};

		// Settings view from pbd::settings_menu, drawn above the Practice menu, replay controls and spectator band.
		struct pbd_settings_overlay final : overlay
		{
			void init();
			void update(u64 timestamp_us) override;
			compiled_resource get_compiled() override;
			void set_render_viewport(u16 width, u16 height) override;

		private:
			compiled_resource m_compiled{};
			f32 m_output_scale = 1.f, m_compiled_scale = 0.f;
			u64 m_revision = 0;
			void refresh(const ::pbd::settings_menu::view& view);
			// While the menu is closed in replays and spectating: a controller takeover's toast (pbd_controller_takeover.h).
			// Practice shows its own toast. In offline play, the
			// question which player a controller is (pbd_player_seats.h), and its Versus prompts, panels and menu (pbd_versus.h).
			bool m_practice = false;
			u64 m_notice_key = 0, m_seats_revision = 0, m_versus_revision = 0;
			compiled_resource m_notice_compiled{};
			compiled_resource notices();
		};

		// The net overlay's switch, decided and logged once per process.
		bool pbd_net_overlay_wanted();

		// Creates each PBD overlay this run asks for that does not exist yet, in drawing order.
		void create_pbd_overlays(display_manager& manager, bool net_wanted);

		// Shared by the overlays' own files (pbd_overlays.cpp).
		bool pbd_xbox_glyphs();
		std::u32string pbd_panel_label(font* f, const ::pbd::practice::panel_line& line, std::string_view label, f32 room);
	} // namespace overlays
} // namespace rsx
