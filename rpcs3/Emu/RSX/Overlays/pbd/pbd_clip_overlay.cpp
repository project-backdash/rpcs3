#include "stdafx.h"
#include "pbd_overlays.h"
#include "pbd_clip_drawing.h"
#include "Emu/System.h"

// The clip toast over the fight, in any mode (pbd_clip_drawing.h).

namespace rsx
{
	namespace overlays
	{
		void pbd_clip_overlay::init()
		{
			// The fonts and atlas are prepared here, off the present thread's first toast.
			pbd::preload_fonts();
			pbd::assets::get();
			m_compiled.clear();
			visible = false;
			// The toast reports on the recording itself, so the recorded frames leave it out.
			screen_only = true;
		}

		void pbd_clip_overlay::set_render_viewport(u16 width, u16 height)
		{
			if (width && height)
				m_output_scale = std::min(width / 1280.f, height / 720.f);
		}

		void pbd_clip_overlay::update(u64)
		{
			m_compiled.clear();
			const auto toast = ::pbd::clips::hud::current();
			const u64 now = ::pbd::clips::hud::steady_ms();
			const auto at = toast.kind == ::pbd::clips::hud::toast_kind::none || now < toast.at_ms ? ::pbd::clips::hud::frame{} :
			                                                                                         ::pbd::clips::hud::frame_at(now - toast.at_ms);
			visible = at.shown && !Emu.IsStopped();
			if (!visible)
				return;
			// Rebuilt every frame while it shows: it fades and its recording dot blinks for 2.25 s at most.
			static const pbd::fonts type = pbd::preload_fonts();
			const auto legend = ::pbd::clips::hud::legend_for(::pbd::clips::hud::load(), ::pbd::controllers::read_display());
			pbd::layer out;
			pbd::clip_toast(out, pbd::atlas_ref::pick(m_output_scale, pbd_xbox_glyphs()), type, toast, legend, at);
			out.flush(m_compiled);
		}

		compiled_resource pbd_clip_overlay::get_compiled()
		{
			if (!visible || Emu.IsStopped())
				return {};
			return m_compiled;
		}
	} // namespace overlays
} // namespace rsx
