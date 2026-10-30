#include "stdafx.h"
#include "pbd_overlays.h"
#include "overlay_pbd_draw.h"
#include "pbd_practice_menu_drawing.h"
#include "pbd_challenger_drawing.h"
#include "pbd_lobby_panel_drawing.h"
#include "Emu/Cell/pbd_practice_mouse.h"
#include "Emu/System.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>
#include <string_view>

// The full-screen host panel: the launcher's lobby menus and Practice's pause menu.

namespace rsx
{
	namespace overlays
	{
		void pbd_game_menu_overlay::init()
		{
			// The fonts and atlas are prepared here, off the present thread's first menu frame.
			pbd::preload_fonts();
			pbd::notices::type();
			pbd::assets::get();
			m_native_compiled.clear();
			m_show = false;
			visible = false;
		}

		void pbd_game_menu_overlay::refresh_now()
		{
			m_snapshot = ::pbd::game_overlay::load();
			const auto practice = ::pbd::practice::load();
			const auto draw = ::pbd::game_overlay::choose_draw(m_snapshot, practice.active && practice.native_menu, practice.menu_generation);
			const bool native = draw == ::pbd::game_overlay::menu_draw::practice;
			m_show = draw != ::pbd::game_overlay::menu_draw::hidden && !Emu.IsStopped();
			visible = m_show;
			if (!m_show || !native)
				::pbd::practice::mouse::publish({});
			if (!m_show)
				return;
			if (native)
				refresh_native(practice);
			else
				refresh_lobby();
		}

		void pbd_game_menu_overlay::set_render_viewport(u16 width, u16 height)
		{
			// Layout stays on the 1280x720 canvas; the output scale only picks the glyph atlas drawn at this size.
			if (width && height)
				m_output_scale = std::min(width / 1280.f, height / 720.f);
		}

		namespace menu = ::pbd::game_overlay;
		using namespace pbd;

		void pbd_game_menu_overlay::refresh_lobby()
		{
			static const pbd::fonts type = pbd::preload_fonts();
			const auto atlas = pbd::atlas_ref::pick(m_output_scale, pbd_xbox_glyphs());
			if (m_snapshot.kind == menu::panel_kind::challenger)
			{
				pbd::draw_challenger(m_native_compiled, m_snapshot, type, atlas);
				return;
			}
			// The notices' view of the set, when the latch composed one for this emulation.
			auto set = ::pbd::set_notices::load();
			if (set.generation != static_cast<u64>(Emu.GetEmulationIdentifier()))
				set = {};
			// Set complete's counter flip plays once, from the first frame that shows the rated count.
			const auto now_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
					.count());
			const u64 key = ::pbd::set_notices::flip_key(m_snapshot);
			if (key && key != m_flip_key)
			{
				m_flip_key = key;
				m_flip_started_ms = now_ms;
			}
			pbd::lobby_panel_drawing draw{m_snapshot, set, atlas, key ? now_ms - m_flip_started_ms : 0};
			draw.draw(m_native_compiled);
		}

		void pbd_game_menu_overlay::refresh_native(const ::pbd::practice::snapshot& practice)
		{
			pbd::assets::get();
			static const pbd::fonts type = pbd::preload_fonts();
			pbd::practice_menu_drawing draw{m_snapshot, practice, type, pbd::atlas_ref::pick(m_output_scale, pbd_xbox_glyphs())};
			draw.draw(m_native_compiled);
		}

		void pbd_game_menu_overlay::update(u64 timestamp_us)
		{
			if (Emu.IsStopped())
			{
				m_show = false;
				visible = false;
				return;
			}
			if (timestamp_us >= m_next_refresh_us)
			{
				// Match the input owner's sub-frame command poll; visible choices should not trail controller ownership. The lobby
				// band drains with each rebuild.
				m_next_refresh_us = timestamp_us + 8000;
				refresh_now();
			}
		}

		compiled_resource pbd_game_menu_overlay::get_compiled()
		{
			if (!visible || !m_show || Emu.IsStopped())
				return {};
			return m_native_compiled;
		}
	} // namespace overlays
} // namespace rsx
