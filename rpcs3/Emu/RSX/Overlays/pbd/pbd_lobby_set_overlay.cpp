#include "stdafx.h"
#include "pbd_overlays.h"
#include "overlay_pbd_draw.h"
#include "overlay_pbd_notices.h"
#include "Emu/System.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>

// The notices over a lobby set's own screens: the waits, the fade to black at the hand-off, the select countdown, the VS
// cards and the search line.

namespace rsx
{
	namespace overlays
	{
		void pbd_wait_overlay::init()
		{
			pbd::preload_fonts();
			pbd::notices::type();
			pbd::assets::get();
			m_compiled.clear();
			visible = false;
		}

		void pbd_wait_overlay::set_render_viewport(u16 width, u16 height)
		{
			// Layout stays on the 1280x720 canvas; the output scale only picks the glyph atlas drawn at this size.
			if (width && height)
				m_output_scale = std::min(width / 1280.f, height / 720.f);
		}

		// The black the picture fades to once the lobby has ended the set, over everything the set drew (this overlay comes
		// after the launcher's panel), 0..1.
		static f32 pbd_handoff_shade()
		{
			const auto now = std::chrono::steady_clock::now().time_since_epoch();
			return ::pbd::set_notices::handoff_shade(static_cast<u64>(Emu.GetEmulationIdentifier()),
				static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
		}

		void pbd_wait_overlay::update(u64)
		{
			using namespace pbd;
			namespace at = ::pbd::set_notices::layout;
			const bool waiting = ::pbd::session_wait::showing().load(std::memory_order_acquire);
			const bool controller = ::pbd::session_wait::controller_wanted().load(std::memory_order_acquire);
			const f32 handoff = Emu.IsStopped() ? 0.f : pbd_handoff_shade();
			visible = !Emu.IsStopped() && (waiting || controller || handoff > 0.f);
			m_compiled.clear();
			if (!visible)
				return;
			if (handoff > 0.f)
			{
				layer out;
				out.rect(layer::b_under, rgb(0x000000, handoff), 0.f, 0.f, virtual_width, virtual_height);
				out.flush(m_compiled);
				return;
			}
			const auto atlas = atlas_ref::pick(m_output_scale, pbd_xbox_glyphs());
			// No box: the headline sits on the loading cover when it is up (the cover gives it the middle), and over the held game,
			// dimmed, otherwise.
			const auto panel = ::pbd::game_overlay::load();
			const bool cover = panel.visible && panel.selection_transition;
			layer out;
			if (controller && !waiting)
			{
				if (!cover)
					out.rect(layer::b_under, rgb(0x050203, .55f), 0.f, 0.f, virtual_width, virtual_height);
				notices::chrome(&out, 640.f, at::controller_headline_y, ::pbd::session_wait::controller_title, at::wait_em, false, notices::align::centre);
				notices::line(out, 640.f, at::controller_line_y, ::pbd::session_wait::controller_line, notices::align::centre, text_2);
				out.flush(m_compiled);
				return;
			}
			// The other player's name as the set names it, else the launcher's.
			const auto set = ::pbd::set_notices::load();
			const bool named = set.active && set.generation == static_cast<u64>(Emu.GetEmulationIdentifier());
			const std::string name = named ? set.sides[set.local ^ 1].name : ::pbd::session_wait::peer_name();
			const bool reconnecting = ::pbd::session_wait::recovering().load(std::memory_order_acquire);
			const u32 waited = ::pbd::session_wait::seconds().load(std::memory_order_acquire);
			const auto leave = ::pbd::session_wait::leave().load(std::memory_order_acquire);
			const bool leave_open = ::pbd::session_wait::leave_open(waited, leave);
			if (reconnecting)
				out.rect(layer::b_under, rgb(0x050203, .5f), 0.f, 0.f, virtual_width, virtual_height);
			else if (!cover)
				out.rect(layer::b_under, rgb(0x050203, .55f), 0.f, 0.f, virtual_width, virtual_height);
			const f32 headline_y = reconnecting ? at::reconnect_headline_y : at::wait_headline_y;
			const f32 line_y = reconnecting ? at::reconnect_line_y : at::wait_line_y;
			notices::chrome(&out, 640.f, headline_y, ::pbd::session_wait::title(name, reconnecting), at::wait_em, false, notices::align::centre);
			// The gold line fills toward the moment Leave opens, the waited time at caption size beside it.
			const auto clock = ::pbd::session_wait::elapsed(waited);
			const auto& f = notices::type();
			const f32 clock_w = notices::tracked(nullptr, f.caption, text_2, 0.f, line_y, utf8_to_u32string(clock), 1.f, .12f);
			const f32 left = std::round(640.f - (at::wait_track_w + 12.f + clock_w) / 2.f);
			const f32 share = std::min(1.f, static_cast<f32>(waited) / static_cast<f32>(::pbd::session_wait::leave_after_seconds));
			out.rect(layer::b_lines, rgb(0xE9EEED, .18f), left, line_y - 1.f, at::wait_track_w, 2.f);
			out.rect(layer::b_lines, gold, left, line_y - 1.f, at::wait_track_w * share, 2.f);
			notices::tracked(&out, f.caption, rgb(0x000000, .8f), left + at::wait_track_w + 13.f, line_y + 1.f, utf8_to_u32string(clock), 1.f, .12f);
			notices::tracked(&out, f.caption, text_2, left + at::wait_track_w + 12.f, line_y, utf8_to_u32string(clock), 1.f, .12f);
			if (leave_open)
			{
				// The way out, centred at the foot, with the two-second hold filling under it.
				const auto words = ::pbd::session_wait::leave_prompt(leave);
				const rich_style style{f.prompt ? f.prompt : f.line, text_1, 16.f};
				if (style.f)
				{
					const f32 width = rich_text(nullptr, atlas, style, 0.f, at::prompt_y, words);
					const f32 x = std::round(640.f - width / 2.f);
					rich_text(&out, atlas, style, x, at::prompt_y, words);
					const f32 held = std::min(1.f, static_cast<f32>(::pbd::session_wait::leave_held_ms().load(std::memory_order_acquire)) /
													   static_cast<f32>(::pbd::session_wait::leave_hold.count()));
					if (held > 0.f)
					{
						out.rect(layer::b_lines, rgb(0xE9EEED, .18f), x, at::prompt_y + 13.f, width, 2.f);
						out.rect(layer::b_lines, crimson, x, at::prompt_y + 13.f, width * held, 2.f);
					}
				}
			}
			out.flush(m_compiled);
		}

		compiled_resource pbd_wait_overlay::get_compiled()
		{
			// The latch clears the flags the moment both PCs go on; a frame compiled before that never shows after it.
			if (!visible || Emu.IsStopped() ||
				!(::pbd::session_wait::showing().load(std::memory_order_acquire) || ::pbd::session_wait::controller_wanted().load(std::memory_order_acquire) ||
					pbd_handoff_shade() > 0.f))
				return {};
			return m_compiled;
		}

		void pbd_select_band_overlay::init()
		{
			pbd::preload_fonts();
			pbd::assets::get();
			m_compiled.clear();
			m_revision = ~u64{0};
			visible = false;
		}

		void pbd_select_band_overlay::set_render_viewport(u16 width, u16 height)
		{
			// Layout stays on the 1280x720 canvas; the output scale only picks the glyph atlas drawn at this size.
			if (width && height)
				m_output_scale = std::min(width / 1280.f, height / 720.f);
		}

		void pbd_select_band_overlay::update(u64)
		{
			if (Emu.IsStopped())
			{
				visible = false;
				m_compiled.clear();
				return;
			}
			// The latch publishes a new band only when a drawn value changed, so the draw list is rebuilt at most once a boundary.
			const u64 revision = ::pbd::select_band::revision();
			if (revision == m_revision && m_compiled_scale == m_output_scale)
				return;
			m_revision = revision;
			refresh(::pbd::select_band::load());
		}

		void pbd_select_band_overlay::refresh(const ::pbd::select_band::snapshot& band)
		{
			using namespace pbd;
			m_compiled.clear();
			m_compiled_scale = m_output_scale;
			visible = band.visible && band.generation == static_cast<u64>(Emu.GetEmulationIdentifier());
			if (!visible)
				return;
			static const fonts type = preload_fonts();
			const auto atlas = atlas_ref::pick(m_output_scale, pbd_xbox_glyphs());
			// The select countdown over the game's own select (the shared countdown band), and for the stage picker a lighter
			// second band under it with the stage select's controls. No game art: the game's own select stays visible around it.
			layer out;
			const f32 share = band.total ? static_cast<f32>(std::min(band.frames, band.total)) / static_cast<f32>(band.total) : 0.f;
			countdown_band(out, type, utf8_to_u32string(band.title), two_digits(band.seconds), utf8_to_u32string(band.detail), band.detail_waiting, share);
			if (!band.hint.empty())
			{
				constexpr f32 hint_top = 98.f, hint_bottom = 126.f, hint_centre = (hint_top + hint_bottom) / 2.f;
				const rich_style hint_style{type.body, text_2, 16.f};
				const f32 hint_w = rich_text(nullptr, atlas, hint_style, 0.f, hint_centre, band.hint);
				const f32 hint_left = std::round(640.f - hint_w / 2.f) - 72.f;
				band_strip(out, layer::b_under, rgb(0x0B0304, .74f), hint_left, hint_left + hint_w + 144.f, hint_top, hint_bottom);
				rich_text(&out, atlas, hint_style, hint_left + 72.f, hint_centre, band.hint);
			}
			out.flush(m_compiled);
		}

		compiled_resource pbd_select_band_overlay::get_compiled()
		{
			if (Emu.IsStopped())
				return {};
			// A changed band is rebuilt before it is composed, so a band hidden after the last update never shows another frame.
			if (::pbd::select_band::revision() != m_revision)
				update(0);
			if (!visible)
				return {};
			return m_compiled;
		}

		void pbd_vs_cards_overlay::init()
		{
			pbd::preload_fonts();
			pbd::notices::type();
			pbd::assets::get();
			m_compiled.clear();
			m_revision = ~u64{0};
			visible = false;
		}

		void pbd_vs_cards_overlay::update(u64)
		{
			if (Emu.IsStopped())
			{
				visible = false;
				m_compiled.clear();
				return;
			}
			// The latch publishes new cards only when a drawn value changed, so the draw list is rebuilt at most once a boundary.
			const u64 revision = ::pbd::vs_cards::revision();
			if (revision == m_revision)
				return;
			m_revision = revision;
			refresh(::pbd::vs_cards::load());
		}

		void pbd_vs_cards_overlay::refresh(const ::pbd::vs_cards::snapshot& cards)
		{
			using namespace pbd;
			namespace at = ::pbd::set_notices::layout;
			m_compiled.clear();
			visible = cards.visible && cards.generation == static_cast<u64>(Emu.GetEmulationIdentifier());
			if (!visible)
				return;
			// The score and the set's length in the middle (no game number: the score says it and the game prints its own), and over
			// a card the game's own stake word when this game can end the set that way for that player. Over a watcher's cover,
			// which hides the game's VS screen, the foot is darkened a little and each card shows where the cover showed it. Over
			// the game's own VS screen its portraits already carry the names and plates, so nothing covers them: the score sits in
			// the gap between the game's two W/L bars, the length under them and each stake word just above the game's name bar.
			layer out;
			if (cards.over_cover)
				out.image(layer::b_under, &notices::shades::get().bottom, rgb(0xFFFFFF, .75f), 0.f, 570.f, virtual_width, virtual_height);
			if (cards.games)
				notices::score(out, 640.f, at::vs_score_y, (*cards.games)[0], (*cards.games)[1], at::vs_score_em);
			notices::caption(out, 640.f, at::vs_caption_y, ::pbd::set_notices::first_to_caption(cards.first_to), notices::align::centre);
			for (u32 side = 0; side < 2; side++)
			{
				const auto& card = cards.cards[side];
				const f32 edge = side ? at::right_edge : at::margin;
				if (cards.over_cover)
					notices::card(out, {card.character, card.name, card.shown}, edge, side ? notices::align::right : notices::align::left, at::card_caption_y,
						at::card_name_y, at::card_plate_top);
				notices::draw_stake(out, card.stake, edge, cards.over_cover ? at::vs_stake_top : at::vs_game_stake_top, side == 1);
			}
			out.flush(m_compiled);
		}

		compiled_resource pbd_vs_cards_overlay::get_compiled()
		{
			if (Emu.IsStopped())
				return {};
			// Changed cards are rebuilt before they are composed, so cards hidden after the last update never show another frame.
			if (::pbd::vs_cards::revision() != m_revision)
				update(0);
			// A watcher's in-game menu (Esc) dims the screen and puts its help line and control bar along the bottom, over the
			// cards' text; the cards give way to it as the watcher's own screen does. A lobby set has no such menu.
			if (!visible || ::pbd::settings_menu::shown())
				return {};
			return m_compiled;
		}

		void pbd_notice_overlay::init()
		{
			pbd::notices::type();
			pbd::assets::get();
			m_compiled.clear();
			m_revision = ~u64{0};
			visible = false;
		}

		void pbd_notice_overlay::update(u64)
		{
			if (Emu.IsStopped())
			{
				visible = false;
				m_compiled.clear();
				return;
			}
			// The latch publishes a new snapshot only when a drawn value changed, so the draw list is rebuilt at most once a boundary.
			const u64 revision = ::pbd::set_notices::revision();
			if (revision == m_revision)
				return;
			m_revision = revision;
			refresh(::pbd::set_notices::load());
		}

		void pbd_notice_overlay::refresh(const ::pbd::set_notices::snapshot& set)
		{
			using namespace pbd;
			namespace at = ::pbd::set_notices::layout;
			m_compiled.clear();
			visible = set.active && set.generation == static_cast<u64>(Emu.GetEmulationIdentifier());
			if (!visible)
				return;
			// The lettering is set the first time the set shows a placing player (on the loading cover), never in the middle of
			// an intro: setting the pill's words takes a few frames' time on this thread.
			for (const auto& side : set.sides)
				if (side.shown.shown == notices::plate::kind::placing && set.mode == ::pbd::set_notices::set_mode::ranked)
				{
					notices::pill_letters();
					notices::placing_letters(side.shown.done, side.shown.total);
				}
			layer out;
			// The gold pill at the game's own pill slot on a placing player's intro; nothing without its picture.
			if (::pbd::set_notices::intro_pill(set) && notices::picture(::pbd::notice_art::picture::placement_match))
				notices::draw_pill(out, at::pill_x, at::pill_y, at::pill_w, at::pill_h);
			// The placement plate over the game's Beginner plate while the game draws it; without the stone the game's plate stays.
			if (notices::picture(::pbd::notice_art::picture::placement_stone))
				for (u32 side = 0; side < 2; ++side)
					if (::pbd::set_notices::life_plate(set, side))
						notices::draw_plate(out, set.sides[side].shown, side ? at::life_plate_right - at::placing_w : at::life_plate_left, at::life_plate_top,
							at::placing_w, at::placing_h);
			out.flush(m_compiled);
			visible = !m_compiled.draw_commands.empty();
		}

		compiled_resource pbd_notice_overlay::get_compiled()
		{
			if (Emu.IsStopped())
				return {};
			// A changed snapshot is rebuilt before it is composed, so a notice the fight has left never shows another frame.
			if (::pbd::set_notices::revision() != m_revision)
				update(0);
			// The in-game menu (Esc) dims the screen over the fight; the notices give way to it.
			if (!visible || ::pbd::settings_menu::shown())
				return {};
			return m_compiled;
		}

		// The search line: eight dots turning once a second, SEARCHING in the display face, the mode (with the fighter's
		// placement count in a Ranked search, the count in gold) and the time searched in the display face, as one prompt-style
		// line with no box. The time keeps the width of "88:88", so the line does not move as the seconds count.
		static void draw_search_line(pbd::layer& out, const pbd::atlas_ref& atlas, const ::pbd::search_badge::search& search,
			::pbd::search_badge::place at, u64 now_ms)
		{
			using namespace pbd;
			const auto& f = notices::type();
			if (!f.prompt || !f.line)
				return;
			constexpr f32 gap = 12.f, icon = 18.f;
			const bool ranked = search.mode == ::pbd::search_badge::search_mode::ranked;
			const auto words = ::pbd::set_notices::search_words(ranked, search.placement ?
																			std::optional(std::pair{search.placement->done, search.placement->total}) :
																			std::nullopt);
			const auto mode = utf8_to_u32string(words.mode), count = utf8_to_u32string(words.count);
			const auto time = utf8_to_u32string(::pbd::search_badge::elapsed(search.started_ms, now_ms));
			const auto title = std::u32string(U"SEARCHING");
			const f32 tag_scale = notices::scale_for(f.row, 14.f), time_scale = notices::scale_for(f.row_end, 17.f);
			// The mode at the prompts' 15, the size of the other words along the foot.
			const f32 mode_scale = notices::scale_for(f.line, 15.f);
			const f32 title_w = notices::tracked(nullptr, f.row, text_1, 0.f, at.centre, title, tag_scale, .24f);
			const f32 mode_w = (text_width(f.line, mode) + text_width(f.line, count)) * mode_scale;
			const f32 time_box = f.row_end ? text_width(f.row_end, U"88:88") * time_scale : 0.f;
			const f32 width = icon + gap + title_w + gap + mode_w + gap + time_box;
			f32 x = at.right_aligned ? at.x - width : at.x;
			const f32 centre = at.centre;
			const f32 icon_x = x + icon / 2.f;
			const u64 head = now_ms / 125 % 8;
			for (u64 i = 0; i < 8; i++)
			{
				const f32 angle = static_cast<f32>(i) * 0.7853982f;
				const f32 alpha = std::max(.16f, 1.f - .12f * static_cast<f32>((head + 8 - i) % 8));
				const f32 dot_x = icon_x + 6.4f * std::sin(angle) - 1.6f, dot_y = centre - 6.4f * std::cos(angle) - 1.6f;
				out.sprite(atlas, pbd_glyphs::rec_dot, dot_x, dot_y, 3.2f, rgb(0xE9EEED, alpha));
			}
			x += icon + gap;
			notices::tracked(&out, f.row, rgb(0x000000, .8f), x + 1.f, centre + 1.f, title, tag_scale, .24f);
			x += notices::tracked(&out, f.row, text_1, x, centre, title, tag_scale, .24f) + gap;
			const auto shadowed = [&](font* face, color4f ink, std::u32string_view text, f32 scale = 1.f)
			{
				const f32 base = centre + face->get_size_px() * .2917f * scale;
				out.text(face, rgb(0x000000, .8f), x + 1.f, base + 1.5f, text, scale);
				x += out.text(face, ink, x, base, text, scale);
			};
			shadowed(f.line, text_2, mode, mode_scale);
			if (!count.empty())
				shadowed(f.line, rgb(0xFFD24A), count, mode_scale);
			x += gap;
			if (f.row_end)
				shadowed(f.row_end, text_1, time, time_scale);
		}

		void pbd_search_overlay::init()
		{
			pbd::preload_fonts();
			pbd::notices::type();
			pbd::assets::get();
			m_compiled.clear();
			visible = false;
		}

		void pbd_search_overlay::set_render_viewport(u16 width, u16 height)
		{
			if (width && height)
				m_output_scale = std::min(width / 1280.f, height / 720.f);
		}

		void pbd_search_overlay::update(u64)
		{
			using namespace pbd;
			const auto search = ::pbd::search_badge::load();
			const auto panel = ::pbd::game_overlay::load();
			visible = !Emu.IsStopped() && ::pbd::search_badge::shows(search, panel.visible && !panel.practice);
			m_compiled.clear();
			if (!visible)
				return;
			const auto practice = ::pbd::practice::load();
			const auto now = std::chrono::system_clock::now().time_since_epoch();
			layer out;
			draw_search_line(out, atlas_ref::pick(m_output_scale, pbd_xbox_glyphs()), search, ::pbd::search_badge::placement(practice.native_menu),
				static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
			out.flush(m_compiled);
		}

		compiled_resource pbd_search_overlay::get_compiled()
		{
			if (!visible || Emu.IsStopped())
				return {};
			return m_compiled;
		}
	} // namespace overlays
} // namespace rsx
