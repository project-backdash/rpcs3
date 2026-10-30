#include "stdafx.h"
#include "pbd_overlays.h"
#include "overlay_pbd_draw.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/Io/pad_config.h"
#include "Emu/Cell/pbd_set_channel.h"
#include "Emu/Cell/pbd_direct_match.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/System.h"

// The PBD overlays' creation order and the helpers they share.

namespace rsx
{
	namespace overlays
	{
		// Net overlay: decided and logged once per process.
		bool pbd_net_overlay_wanted()
		{
			static const bool wanted = []
			{
				const auto& decision = ::pbd::live_stats::net_process_decision();
				if (decision.why == ::pbd::live_stats::note::on)
					rsx_log.notice("PBD_NET_OVERLAY=1: connection line on (host-side only; frame-gate captures force it off)");
				else if (decision.why != ::pbd::live_stats::note::none)
					rsx_log.warning("PBD_NET_OVERLAY off (%s%s)", ::pbd::live_stats::describe(decision.why), decision.variable ? decision.variable : "");
				if (decision.show() && !g_cfg.misc.use_native_interface)
				{
					rsx_log.warning("PBD_NET_OVERLAY ignored: native UI off");
					return false;
				}
				return decision.show();
			}();
			return wanted;
		}

		// The active controller selects Xbox plates for Xbox and Steam Deck, PlayStation plates otherwise. Without an active
		// controller, PBD_PRACTICE_GLYPHS overrides the first player's XInput/PlayStation fallback.
		bool pbd_xbox_glyphs()
		{
			const auto live = ::pbd::controllers::read_display();
			if (live.active)
				return live.family == "xbox" || live.family == "steamdeck";
			static const int forced = []
			{
				const char* value = ::pbd::env::find("PBD_PRACTICE_GLYPHS");
				if (!value || !*value)
					return -1;
				return std::string_view(value) == "xbox" ? 1 : 0;
			}();
			if (forced >= 0)
				return forced == 1;
#ifdef _WIN32
			return g_cfg_input.player[0]->handler.get() == pad_handler::xinput;
#else
			return false;
#endif
		}

		// The label of an Attack Info or Frame Data row, drawn at the left of a cell whose value is right-aligned in the
		// same room. The full label shows while the value leaves space for it; a longer value takes the line's short label,
		// and whichever label is left is cut to the space that remains, so the value never runs into it.
		std::u32string pbd_panel_label(font* f, const ::pbd::practice::panel_line& line, std::string_view label, f32 room)
		{
			constexpr f32 gap = 8.f;
			const f32 space = room - gap - pbd::text_width(f, utf8_to_u32string(line.value));
			const auto full = utf8_to_u32string(label);
			if (line.short_label.empty() || pbd::text_width(f, full) <= space)
				return pbd::fit_text(f, full, space);
			return pbd::fit_text(f, utf8_to_u32string(line.short_label), space);
		}

		void create_pbd_overlays(display_manager& manager, bool net_wanted)
		{
			if (::pbd::practice::requested() && !manager.get<rsx::overlays::pbd_practice_overlay>())
			{
				auto practice_overlay = manager.create<rsx::overlays::pbd_practice_overlay>();
				std::lock_guard lock(manager);
				practice_overlay->init();
			}
			const bool spectator = ::pbd::spectator::requested_directory().has_value();
			if ((::pbd::corner_chat::requested() || spectator) && !manager.get<rsx::overlays::pbd_corner_chat_overlay>())
			{
				auto chat_overlay = manager.create<rsx::overlays::pbd_corner_chat_overlay>();
				std::lock_guard lock(manager);
				chat_overlay->init();
			}
			if (spectator && !manager.get<rsx::overlays::pbd_spectator_overlay>())
			{
				auto watch_overlay = manager.create<rsx::overlays::pbd_spectator_overlay>();
				std::lock_guard lock(manager);
				watch_overlay->init();
			}
			// The select countdown band: only a lobby set chooses fighters in the game's own select. Before the menu overlay, so a
			// launcher panel and the waiting modal draw over it.
			if (::pbd::set_channel::requested_directory() && !manager.get<rsx::overlays::pbd_select_band_overlay>())
			{
				auto band_overlay = manager.create<rsx::overlays::pbd_select_band_overlay>();
				std::lock_guard lock(manager);
				band_overlay->init();
			}
			// The VS cards: a lobby set has the launcher's card data, and so does a watcher (its watch_ui), whose cards draw
			// while the watched game passes its VS screen, over the cover when it is up (pbd::vs_cards::watch_over_cover). A game
			// held ready for a set gets them too, since it binds the set only after this. Before the menu overlay, like the
			// band, and after the spectator overlay, so they draw above the cover.
			if ((::pbd::set_channel::set_screens_requested() || spectator) && !manager.get<rsx::overlays::pbd_vs_cards_overlay>())
			{
				auto cards_overlay = manager.create<rsx::overlays::pbd_vs_cards_overlay>();
				std::lock_guard lock(manager);
				cards_overlay->init();
			}
			// The same Ranked notices draw over live sets and saved replays, including games held before their file binds.
			// Before the menu overlay, like the cards; without a published notice they remain invisible.
			if ((::pbd::set_channel::set_screens_requested() || pbd_direct_match::requested()) && !manager.get<rsx::overlays::pbd_notice_overlay>())
			{
				auto notice_overlay = manager.create<rsx::overlays::pbd_notice_overlay>();
				std::lock_guard lock(manager);
				notice_overlay->init();
			}
			if ((::pbd::game_overlay::requested() || ::pbd::practice::requested()) && !manager.get<rsx::overlays::pbd_game_menu_overlay>())
			{
				auto menu_overlay = manager.create<rsx::overlays::pbd_game_menu_overlay>();
				std::lock_guard lock(manager);
				menu_overlay->init();
			}
			// After the menu overlay, so the search line stays on screen over Practice's pause menu.
			if (::pbd::game_overlay::requested() && !manager.get<rsx::overlays::pbd_search_overlay>())
			{
				auto search_overlay = manager.create<rsx::overlays::pbd_search_overlay>();
				std::lock_guard lock(manager);
				search_overlay->init();
			}
			// After the menu overlay, so the waits draw above the lobby's loading cover.
			if (::pbd::session_wait::requested() && !manager.get<rsx::overlays::pbd_wait_overlay>())
			{
				auto wait_overlay = manager.create<rsx::overlays::pbd_wait_overlay>();
				std::lock_guard lock(manager);
				wait_overlay->init();
			}
			if (net_wanted && !manager.get<rsx::overlays::pbd_net_overlay>())
			{
				auto net_overlay = manager.create<rsx::overlays::pbd_net_overlay>();
				std::lock_guard lock(manager);
				net_overlay->init();
			}

			// Last of the PBD overlays, so the in-game Settings menu draws over every other one.
			if (::pbd::settings_menu::available() && !manager.get<rsx::overlays::pbd_settings_overlay>())
			{
				auto settings_overlay = manager.create<rsx::overlays::pbd_settings_overlay>();
				std::lock_guard lock(manager);
				settings_overlay->init();
			}
		}
	} // namespace overlays
} // namespace rsx
