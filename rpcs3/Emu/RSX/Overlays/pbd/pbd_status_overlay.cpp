#include "stdafx.h"
#include "pbd_overlays.h"
#include "overlay_pbd_draw.h"
#include "overlay_pbd_notices.h"
#include "Emu/PBD/pbd_replay_viewer_menu.h"
#include "Emu/System.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The status line: in a session the connection when it matters and the leave chord, bottom centre; a replay's or the Practice
// dummy's line at the bottom right; and a replay viewer's whole HUD.

namespace rsx
{
	namespace overlays
	{
		// The legacy replay and dummy status strings name pad buttons in words ("SELECT+X: retry"); the status line draws them
		// as glyphs. Whole words only, longest first; the colon after a button becomes a plain space.
		static std::string pbd_button_tokens(std::string_view text)
		{
			static constexpr std::array<std::pair<std::string_view, std::string_view>, 19> words{{{"SELECT+TRIANGLE", "{select}+{triangle}"}, {"SELECT+SQUARE", "{select}+{square}"},
				{"SELECT+X", "{select}+{cross}"}, {"SELECT+O", "{select}+{circle}"},
				{"LEFT/RIGHT", "{dpad:lr}"}, {"UP/DOWN", "{dpad:ud}"},
				{"L1/R1", "{l1}/{r1}"}, {"L2/R2", "{l2}/{r2}"}, {"L3/R3", "{l3}/{r3}"},
				{"START", "{start}"}, {"SELECT", "{select}"}, {"R1", "{r1}"}, {"R2", "{r2}"},
				{"X:", "{cross}"}, {"O:", "{circle}"}, {"LEFT:", "{dpad:l}"}, {"RIGHT:", "{dpad:r}"}, {"DOWN:", "{dpad:d}"},
				{"UP:", "{dpad:u}"}}};
			const auto word_char = [](char c)
			{
				return std::isalnum(static_cast<unsigned char>(c)) != 0;
			};
			std::string out;
			usz i = 0;
			while (i < text.size())
			{
				bool matched = false;
				if (i == 0 || !word_char(text[i - 1]))
				{
					for (const auto& [from, to] : words)
					{
						if (text.substr(i, from.size()) != from)
							continue;
						const usz end = i + from.size();
						if (end < text.size() && word_char(text[end]))
							continue;
						out += to;
						i = end;
						if (i < text.size() && text[i] == ':')
							i++;
						matched = true;
						break;
					}
				}
				if (!matched)
					out.push_back(text[i++]);
			}
			return out;
		}

		void pbd_net_overlay::init()
		{
			// The shared PROJECT BACKDASH fonts (PBDDisplay.otf, Barlow) and glyph atlas, packed here rather than on the present
			// thread's first frame.
			pbd::preload_fonts();
			pbd::notices::type();
			pbd::assets::get();
			m_compiled.clear();
			m_show = false;
			m_next_refresh_us = 0;
			visible = false;
		}

		void pbd_net_overlay::set_render_viewport(u16 width, u16 height)
		{
			if (width && height)
				m_output_scale = std::min(width / 1280.f, height / 720.f);
		}

		namespace viewer = ::pbd::replay_viewer;
		using namespace pbd;

		struct status_drawing
		{
			const ::pbd::live_stats::snapshot& snap;
			const viewer::snapshot& view;
			const fonts& type;
			atlas_ref atlas;
			std::array<viewer::pointer_rectangle, 12> areas{};
			u32 area_count = 0;
			static constexpr f32 line_right = 1224.f, line_centre = 693.f;

			// The leave chord's words while it is held: a Ranked set says the hold forfeits it, Set complete and every other session
			// that it leaves; "Leaving" once the hold is complete.
			std::string leave_content() const
			{
				auto set = ::pbd::set_notices::load();
				const auto mode = set.generation == static_cast<u64>(Emu.GetEmulationIdentifier()) && set.active ? set.mode :
				                                                                                                   ::pbd::set_notices::set_mode::lobby;
				const auto panel = ::pbd::game_overlay::load();
				const bool set_complete = panel.visible && panel.kind == ::pbd::game_overlay::panel_kind::post_set;
				return ::pbd::set_notices::leave_words(mode, set_complete, snap.leave_hold_ms >= ::pbd::live_stats::leave_hold_total_ms);
			}

			void progress(layer& out, f32 left, f32 width, f32 centre) const
			{
				const f32 hold = snap.leave_hold_ms ? std::min(1.f, static_cast<f32>(snap.leave_hold_ms) / ::pbd::live_stats::leave_hold_total_ms) : -1.f;
				if (hold < 0.f)
					return;
				out.rect(layer::b_lines, rgb(0xE9EEED, .16f), left, centre + 11.f, width, 2.f);
				out.rect(layer::b_lines, crimson, left, centre + 11.f, width * hold, 2.f);
			}

			// The enabled connection line during a round: signal bars, round trip, delay and recent rollback depth. In a lobby set it
			// shows only while a round is fought (the round call and the fight), never over the intros, a knockout or the
			// held frame between games; and never under a launcher panel, which names the set's connection itself at Set
			// complete, so it is said once.
			void connection(layer& out) const
			{
				namespace at = ::pbd::set_notices::layout;
				const u32 bars = ::pbd::live_stats::net_bars(snap);
				if (!pbd_net_overlay_wanted() || ::pbd::game_overlay::load().visible)
					return;
				const auto set = ::pbd::set_notices::load();
				if (set.active && set.generation == static_cast<u64>(Emu.GetEmulationIdentifier()) &&
					set.now != ::pbd::set_notices::screen::round_intro && set.now != ::pbd::set_notices::screen::fight)
					return;
				notices::net_line(&out, bars, ::pbd::live_stats::net_text(snap), 640.f, at::net_y, notices::align::centre, type.body);
			}

			// The leave chord's line in a lobby set: bottom centre, no box, the crimson line filling under it over the hold.
			void leave_centred(layer& out) const
			{
				namespace at = ::pbd::set_notices::layout;
				const auto words = leave_content();
				const rich_style style{type.body, text_1, 16.f};
				const f32 width = rich_text(nullptr, atlas, style, 0.f, at::leave_y, words);
				const f32 left = std::round(640.f - width / 2.f);
				rich_style shadow{type.body, rgb(0x000000, .8f), 16.f};
				shadow.glyph_tint = rgb(0x000000, .6f);
				rich_text(&out, atlas, shadow, left + 1.f, at::leave_y + 1.5f, words);
				rich_text(&out, atlas, style, left, at::leave_y, words);
				progress(out, left, width, at::leave_y);
			}

			void status_line(compiled_resource& m_compiled, bool replay, bool practice_dummy, bool practice_active, bool leave_line) const
			{
				// Practice keeps its own control bar on the bottom row, so its hold countdown sits one row up (the HUD's
				// empty help-line row).
				const f32 status_centre = practice_active ? 655.f : line_centre;
				layer out;
				// A session's leave chord and its connection sit at the bottom centre with no box.
				if (!replay && !practice_dummy && !practice_active)
				{
					if (leave_line)
						leave_centred(out);
					else
						connection(out);
					out.flush(m_compiled);
					return;
				}
				// Bottom-right status line, no box: a soft band behind it ramps in from the left and runs off the right edge.
				std::string content;
				color4f ink = text_1;
				if (leave_line)
					content = replay && !snap.leave_hold_ms ?
					              "{dpad:l} Play as P1   {dpad:r} Play as P2   {dpad:d} Watch   {start}+{select} Leave (Hold)" :
					              leave_content();
				else if (replay)
				{
					content = pbd_button_tokens(::pbd::live_stats::replay_text(snap));
					ink = snap.takeover_state == 1 ? text_3 : snap.takeover_state == 2 ? stripe :
					                                                                     text_1;
				}
				else if (practice_dummy)
				{
					content = pbd_button_tokens(::pbd::live_stats::practice_dummy_text(snap));
					ink = snap.practice_dummy_state == static_cast<u32>(::pbd::practice::dummy_state::recording) ? stripe : text_1;
				}
				if (content.empty())
					return;
				const rich_style style{type.caption, ink, 16.f};
				const f32 width = rich_text(nullptr, atlas, style, 0.f, status_centre, content);
				const f32 left = std::round(line_right - width);
				rich_text(&out, atlas, style, left, status_centre, content);
				progress(out, left, width, status_centre);
				band_strip(out, layer::b_under, rgb(0x0B0304, .66f), left - 24.f - 72.f, overlay::virtual_width + 72.f, status_centre - 17.f, status_centre + 17.f);
				out.flush(m_compiled);
			}

			void area(f32 x, f32 y, f32 w, f32 h, u32 row)
			{
				if (area_count < areas.size())
					areas[area_count++] = {static_cast<s32>(std::lround(x)), static_cast<s32>(std::lround(y)), static_cast<s32>(std::lround(w)), static_cast<s32>(std::lround(h)), row};
			}

			void replay_history(layer& hud) const
			{
				for (usz side = 0; side < 2; side++)
				{
					if (!viewer::history_visible(view.flags, side))
						continue;
					const auto& rows = view.history[side];
					u32 count = 0;
					while (count < rows.size() && rows[count].held)
						count++;
					history_column(hud, atlas, side, count, [&](u32 i)
						{
							return history_row{rows[i].input, rows[i].held};
						});
				}
			}

			void replay_frames(layer& hud) const
			{
				if ((view.flags & 10u) == 10u && !view.fast)
					for (usz side = 0; side < 2; ++side)
					{
						const auto placed = ::pbd::practice::place_panels(side, viewer::history_visible(view.flags, side), false, true, ::pbd::practice::replay_panels_bottom);
						const auto& rect = *placed.frame_data;
						auto lines = ::pbd::practice::frame_data_text(view.fighters[side]);
						lines[1].label = "Frame Advantage";
						hud.rect(layer::b_under, rgb(0x000000, .34f), rect.x, rect.y, rect.w, rect.h);
						hud.rect(layer::b_lines, top_edge, rect.x, rect.y, rect.w, 1.f);
						hud.rect(layer::b_lines, hairline, rect.x, rect.y + rect.h - 1.f, rect.w, 1.f);
						for (usz i = 0; i < lines.size(); ++i)
						{
							const auto cell = ::pbd::practice::panel_line_rect(rect, i, lines.size(), lines.size());
							const f32 centre = cell.y + cell.h / 2.f;
							hud.text(type.caption, text_2, cell.x + 11.f, baseline(type.caption, centre), pbd_panel_label(type.caption, lines[i], lines[i].label, cell.w - 22.f));
							const auto value = utf8_to_u32string(lines[i].value);
							hud.text(type.caption, lines[i].value == ::pbd::practice::unknown_value ? text_3 : text_1,
								cell.x + cell.w - 11.f - text_width(type.caption, value), baseline(type.caption, centre), value);
						}
					}
			}

			void replay_badge(layer& hud) const
			{
				if (const auto badge = viewer::badge_text(view); !badge.empty())
				{
					// Take-control tag above the controlled side's history: crimson while you control it, slate while armed.
					const bool right_side = view.port == 2;
					const bool active = view.takeover == ::pbd::takeover_phase::controlling;
					const f32 anchor = right_side ? 1252.f : 34.f;
					const auto text = utf8_to_u32string(badge);
					const f32 w = pbd::tag(hud, type.caption, active ? crimson : rgb(0x2A3134, .82f), active ? white : text_1, anchor, right_side, 124.f, text);
					if (!active)
					{
						const f32 x = right_side ? anchor - w : anchor;
						const color4f rim = rgb(0xAEBDBD, .6f);
						hud.rect(layer::b_lines, rim, x, 124.f, w, 1.f);
						hud.rect(layer::b_lines, rim, x, 143.f, w, 1.f);
						hud.rect(layer::b_lines, rim, x, 125.f, 1.f, 18.f);
						hud.rect(layer::b_lines, rim, x + w - 1.f, 125.f, 1.f, 18.f);
					}
				}
			}

			void replay_playback(layer& hud) const
			{
				if (view.fast)
				{
					// Fast-forward: a top-centre band in place of the playback controls.
					const auto text = utf8_to_u32string(viewer::status_line(view));
					const f32 content = text_width(type.caption, text);
					const f32 left = std::round(640.f - content / 2.f) - 72.f, right = left + content + 144.f;
					band_strip(hud, layer::b_under, band_fill, left, right, 92.f, 126.f);
					band_strip(hud, layer::b_lines, rgb(0xE9EEED, .16f), left, right, 92.f, 93.f);
					hud.text(type.caption, text_1, left + 72.f, baseline(type.caption, 109.f), text);
				}
				else if (view.paused)
				{
					// Paused mark, bottom centre: two bars on a translucent square.
					hud.rect(layer::b_under, rgb(0x0B0304, .55f), 618.f, 584.f, 44.f, 44.f);
					hud.rect(layer::b_bars, white, 630.f, 596.f, 6.f, 20.f);
					hud.rect(layer::b_bars, white, 644.f, 596.f, 6.f, 20.f);
				}
			}

			void replay_dialog(layer& menu, const viewer::menu_view& layout)
			{
				// Confirm dialog: the question, optional body lines, then one option bar per choice.
				u32 lines = 0;
				while (lines < layout.lines.size() && !layout.lines[lines].empty())
					lines++;
				constexpr f32 x = 380.f, w = 520.f;
				const f32 options_top = 82.f + 24.f * lines;
				const f32 h = options_top + 30.f * layout.count + 20.f;
				const f32 y = std::round(360.f - h / 2.f);
				dialog_frame(menu, x, y, w, h);
				const auto message = utf8_to_u32string(layout.title);
				menu.text(type.label, text_1, 640.f - text_width(type.label, message) / 2.f, baseline(type.label, y + 42.f), message);
				for (u32 i = 0; i < lines; i++)
				{
					const auto line = utf8_to_u32string(layout.lines[i]);
					menu.text(type.body, text_2, 640.f - text_width(type.body, line) / 2.f, baseline(type.body, y + 70.f + 24.f * i), line);
				}
				for (u32 i = 0; i < layout.count; i++)
				{
					const f32 top = y + options_top + 30.f * i;
					option_bar(menu, type.label, x + 1.f, top, w - 2.f, utf8_to_u32string(layout.rows[i].label), i == layout.focus);
					area(x + 1.f, top, w - 2.f, 28.f, i);
				}
			}

			void replay_list(layer& menu, const viewer::menu_view& layout)
			{
				auto& store = assets::get();
				// One-column options panel: title strip, then rows on a 30-unit pitch with the value centred in the value column
				// (defaults in aqua). A page without rows shows its note instead.
				constexpr f32 x = 340.f, w = 600.f, value_centre = x + w - 112.f;
				const f32 h = 48.f + 30.f * std::max<u32>(layout.count, 1) + 10.f;
				const f32 y = std::round(340.f - h / 2.f);
				menu.image(layer::b_under, &store.panel, white, x, y, x + w, y + h);
				menu.rect(layer::b_lines, top_edge, x, y, w, 1.f);
				menu.rect(layer::b_lines, hairline, x, y + 40.f, w, 1.f);
				menu.rect(layer::b_lines, edge_dark, x, y + h - 2.f, w, 2.f);
				menu.text(type.title, text_1, x + 15.f, baseline(type.title, y + 20.f), utf8_to_u32string(layout.title));
				if (!layout.note.empty())
					menu.text(type.body, text_3, x + 31.f, baseline(type.body, y + 62.f), utf8_to_u32string(layout.note));
				for (u32 i = 0; i < layout.count; i++)
				{
					const auto& row = layout.rows[i];
					const u32 choice = layout.first + i;
					const f32 top = y + 48.f + 30.f * i;
					const bool focus = choice == layout.focus;
					menu.rect(layer::b_bars, row_bar, x + 12.f, top, w - 24.f, 28.f);
					if (focus)
						focus_bar(menu, x + 12.f, top, w - 24.f, 28.f);
					menu.text(type.label, label_color(true, focus), x + 31.f, baseline(type.label, top + 14.f), utf8_to_u32string(row.label));
					if (!row.value.empty())
					{
						const auto value = utf8_to_u32string(row.value);
						const color4f ink = row.value_tone == viewer::tone::default_value ? aqua : text_1;
						menu.text(type.label, ink, value_centre - text_width(type.label, value) / 2.f, baseline(type.label, top + 14.f), value);
					}
					area(x + 12.f, top, w - 24.f, 28.f, choice);
				}
			}

			void replay_chrome(layer& chrome)
			{
				auto& store = assets::get();
				// Control bar on the bottom scrim, the help line above it, and the replay readout (or the leave countdown) at the right.
				chrome.image(layer::b_under, &store.scrim, white, 0.f, 628.f, overlay::virtual_width, overlay::virtual_height);
				if (const auto help = viewer::help_line(view); !help.empty())
				{
					const rich_style help_style{type.body, aqua, 16.f};
					const f32 help_w = rich_text(&chrome, atlas, help_style, 56.f, 647.f, help);
					chrome.rect(layer::b_lines, rgb(0x8FC3CA, .38f), 56.f, 658.f, help_w + 4.f, 1.f);
				}
				const std::string readout = snap.leave_hold_ms ? leave_content() : view.fast ? std::string{} :
				                                                                               viewer::status_line(view);
				const rich_style readout_style{type.caption, snap.leave_hold_ms ? text_1 : text_2, 16.f};
				const f32 readout_w = readout.empty() ? 0.f : rich_text(nullptr, atlas, readout_style, 0.f, line_centre, readout);
				const f32 readout_x = std::round(line_right - readout_w);
				if (!readout.empty())
				{
					rich_text(&chrome, atlas, readout_style, readout_x, line_centre, readout);
					progress(chrome, readout_x, readout_w, line_centre);
				}
				const auto bar = viewer::prompts(view);
				std::array<prompt_item, 8> items{};
				const u32 count = std::min<u32>(bar.count, static_cast<u32>(items.size()));
				for (u32 i = 0; i < count; i++)
					items[i] = {bar.items[i].keys, bar.items[i].label};
				prompt_bar(chrome, atlas, type.caption, 56.f, line_centre, readout.empty() ? line_right : readout_x - 26.f,
					std::span<const prompt_item>(items.data(), count), [&](usz i, f32 at, f32 width)
					{
						if (bar.items[i].pointer != viewer::pointer_none)
							area(at, line_centre - 12.f, width, 24.f, bar.items[i].pointer);
					});
			}

			void replay_hud(compiled_resource& compiled)
			{
				layer hud, menu, chrome;
				replay_history(hud);
				replay_frames(hud);
				replay_badge(hud);
				replay_playback(hud);
				const auto layout = viewer::menu_layout(view);
				if (layout.kind != viewer::menu_view::shape::none)
					menu.rect(layer::b_under, rgb(0x000000, .55f), 0.f, 0.f, overlay::virtual_width, overlay::virtual_height);
				if (layout.kind == viewer::menu_view::shape::dialog)
					replay_dialog(menu, layout);
				else if (layout.kind == viewer::menu_view::shape::list)
					replay_list(menu, layout);
				replay_chrome(chrome);
				viewer::pointer_ui.publish(view, areas, area_count);
				hud.flush(compiled);
				menu.flush(compiled);
				chrome.flush(compiled);
			}
		};

		void pbd_net_overlay::refresh_now()
		{
			const auto snap = ::pbd::live_stats::load();
			const bool running = snap.generation && !snap.end_reason && !Emu.IsStopped();
			// The leave prompt replaces the connection line while Select is held, in menus as well as matches. Controlled replays
			// and local Practice dummy controls use the same host-side line without connection bars.
			const bool leaving = running && snap.leave_select;
			m_leave_hold = leaving || (running && snap.leave_hold_ms);
			const bool replay = running && snap.replay_state;
			const bool practice = snap.mode == ::pbd::live_stats::mode_practice;
			const bool practice_active = practice && ::pbd::practice::load().active;
			const bool practice_dummy = running && practice && snap.practice_dummy_on && !practice_active;
			const auto view = viewer::presentation.load();
			const bool in_viewer = running && view.enabled;
			// Practice lists Exit (Hold) on its own control bar, so only the hold countdown shows here.
			const bool leave_line = leaving && (!practice_active || snap.leave_hold_ms);
			const bool show = leave_line || replay || in_viewer || practice_dummy || (running && snap.in_match && !practice);
			m_show = show;
			visible = show;
			m_compiled.clear();
			if (!show)
				return;

			assets::get();
			static const fonts type = preload_fonts();
			status_drawing draw{snap, view, type, atlas_ref::pick(m_output_scale, pbd_xbox_glyphs())};
			if (in_viewer)
				draw.replay_hud(m_compiled);
			else
				draw.status_line(m_compiled, replay, practice_dummy, practice_active, leave_line);
		}

		void pbd_net_overlay::update(u64 timestamp_us)
		{
			// Called by the renderer once per composited frame (withheld frames never get here); four refreshes a second outside a leave hold.
			if (Emu.IsStopped())
			{
				m_show = false;
				visible = false;
				return;
			}
			// The renderer calls get_compiled() for every view before this, and that already rebuilt the viewer HUD this frame.
			if (::pbd::replay_viewer::presentation.enabled.load(std::memory_order_relaxed))
				return;
			if (timestamp_us >= m_next_refresh_us)
			{
				refresh_now();
				// The leave countdown and its progress bar move continuously, so they refresh every frame like the viewer.
				m_next_refresh_us = timestamp_us + (m_leave_hold ? 16000 : ::pbd::live_stats::refresh_interval_us);
			}
		}

		compiled_resource pbd_net_overlay::get_compiled()
		{
			// Renderers normally update overlays after drawing. A paused viewer must compose its current host menu and hit areas.
			if (::pbd::replay_viewer::presentation.enabled.load(std::memory_order_relaxed))
				refresh_now();
			if (!visible || !m_show || Emu.IsStopped())
			{
				return {};
			}
			return m_compiled;
		}
	} // namespace overlays
} // namespace rsx
