#include "stdafx.h"
#include "pbd_overlays.h"
#include "overlay_pbd_draw.h"
#include "overlay_pbd_button_layout.h"
#include "overlay_pbd_versus.h"
#include "Emu/Cell/pbd_controller_takeover.h"
#include "Emu/PBD/pbd_player_seats.h"
#include "Emu/System.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>

// The in-game Settings menu, and the notices it shows while closed.

namespace rsx
{
	namespace overlays
	{
		void pbd_settings_overlay::init()
		{
			// Fonts and the glyph atlas are prepared here, off the present thread's first menu frame.
			pbd::preload_fonts();
			pbd::assets::get();
			m_compiled.clear();
			m_revision = 0;
			m_practice = ::pbd::practice::requested();
			visible = false;
		}

		void pbd_settings_overlay::set_render_viewport(u16 width, u16 height)
		{
			// Layout stays on the 1280x720 canvas; the output scale only picks the glyph atlas drawn at this size.
			if (width && height)
				m_output_scale = std::min(width / 1280.f, height / 720.f);
		}

		void pbd_settings_overlay::update(u64)
		{
			// Called once per composed frame. The game window asks for UI-only flips only while frames stop arriving here.
			::pbd::settings_menu::composed_ms.store(static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
														std::chrono::steady_clock::now().time_since_epoch())
															.count()),
				std::memory_order_relaxed);
		}

		compiled_resource pbd_settings_overlay::get_compiled()
		{
			// Rebuilt here rather than in update(), which the renderer calls after drawing: a choice shows on the next frame.
			if (Emu.IsStopped())
			{
				visible = false;
				m_revision = 0;
				return {};
			}
			if (!::pbd::settings_menu::shown())
			{
				m_revision = 0;
				const auto notice = m_practice ? compiled_resource{} : notices();
				visible = !notice.draw_commands.empty();
				return notice;
			}
			const auto view = ::pbd::settings_menu::load();
			visible = view.open;
			if (!view.open)
				return {};
			if (view.revision != m_revision || m_compiled_scale != m_output_scale)
			{
				m_revision = view.revision;
				m_compiled_scale = m_output_scale;
				refresh(view);
			}
			return m_compiled;
		}

		// Offline play's question which player a controller is (pbd_player_seats.h): over the game dimmed, the controller's
		// name, Player 1 and Player 2 with the controller each has now, and its Button setup.
		static void seats_question(pbd::layer& out, const pbd::fonts& type, const pbd::atlas_ref& atlas, const ::pbd::controllers::seats::view& seats)
		{
			using namespace pbd;
			const auto& q = seats.question;
			constexpr f32 x = 360.f, y = 214.f, w = 560.f, h = 272.f, pad = 24.f, gap = 16.f;
			out.rect(layer::b_under, rgb(0x000000, .45f), 0.f, 0.f, overlay::virtual_width, overlay::virtual_height);
			dialog_frame(out, x, y, w, h);
			out.text(type.title, text_1, x + pad, baseline(type.title, y + 30.f), U"PLAYERS");
			out.text(type.label, aqua, x + pad, baseline(type.label, y + 62.f), fit_text(type.label, utf8_to_u32string(q.name), w - 2.f * pad));
			const f32 box_w = (w - 2.f * pad - gap) / 2.f, box_top = y + 86.f, box_h = 64.f;
			for (unsigned i = 0; i < ::pbd::controllers::seats::players; ++i)
			{
				const f32 box_x = x + pad + static_cast<f32>(i) * (box_w + gap);
				const bool focus = !q.setup_focused && q.player == i;
				out.rect(layer::b_bars, row_bar, box_x, box_top, box_w, box_h);
				if (focus)
					focus_bar(out, box_x, box_top, box_w, box_h);
				out.text(type.label, label_color(true, focus), box_x + 14.f, baseline(type.label, box_top + 20.f), i ? U"PLAYER 2" : U"PLAYER 1");
				const auto& now = seats.names[i];
				const auto holder = now.empty() ? std::u32string(U"No controller") : utf8_to_u32string(now);
				out.text(type.caption, focus ? rgb(0xFFFFFF, .75f) : text_3, box_x + 14.f, baseline(type.caption, box_top + 44.f),
					fit_text(type.caption, holder, box_w - 28.f));
			}
			option_bar(out, type.label, x + pad, y + 168.f, w - 2.f * pad, U"Button setup", q.setup_focused);
			std::vector<prompt_item> prompts{{"{dpad:lr}", "Player"}, {"{dpad:ud}", "Button setup"}};
			prompts.push_back({"{cross}", "Confirm"});
			prompts.push_back({"{circle}", "Cancel"});
			prompt_bar(out, atlas, type.caption, x + pad, y + h - 26.f, x + w - pad, prompts);
		}

		compiled_resource pbd_settings_overlay::notices()
		{
			using namespace pbd;
			// A controller takeover's toast and offline play's player question and Versus layer
			// (pbd_versus.h), rebuilt only when one of them changes.
			const u64 now = ::pbd::controller_takeover::steady_ms();
			const auto notice = ::pbd::controller_takeover::current();
			const bool toast = !notice.text.empty() && now < notice.until_ms;
			const auto seats = ::pbd::controllers::seats::read();
			const bool asking = seats.question.open;
			const auto [versus, versus_revision] = ::pbd::versus::read();
			const bool playing_versus = versus.select || versus.menu || versus.setup[0].open || versus.setup[1].open || !versus.banner.empty();
			if (!toast && !asking && !playing_versus)
				return {};
			const u64 key = toast ? notice.serial : 0;
			const u64 seats_revision = asking ? seats.revision : 0;
			const u64 shown_versus = playing_versus ? versus_revision : 0;
			if (key != m_notice_key || seats_revision != m_seats_revision || shown_versus != m_versus_revision)
			{
				m_notice_key = key;
				m_seats_revision = seats_revision;
				m_versus_revision = shown_versus;
				static const fonts type = preload_fonts();
				// The Versus layer is flushed first, so the player question draws whole over its panels.
				layer under, out;
				const auto atlas = atlas_ref::pick(m_output_scale, pbd_xbox_glyphs());
				if (playing_versus)
					pbd::versus::draw(under, atlas, atlas_ref::pick(m_output_scale * 1.5f, atlas.xbox), type, versus);
				if (asking)
					seats_question(out, type, atlas, seats);
				if (toast)
					toast_band(out, type.caption, utf8_to_u32string(notice.text));
				m_notice_compiled.clear();
				under.flush(m_notice_compiled);
				out.flush(m_notice_compiled);
			}
			return m_notice_compiled;
		}

		namespace menu = ::pbd::settings_menu;
		using namespace pbd;

		struct settings_drawing
		{
			const menu::view& view;
			const fonts& type;
			atlas_ref atlas;
			f32 output_scale;
			layer out, dropdown_layer, chrome;

			void background()
			{
				// The game stays visible under a 55% dim, as under the Practice menu; the scrim keeps the help line and control bar legible.
				out.rect(layer::b_under, rgb(0x000000, .55f), 0.f, 0.f, overlay::virtual_width, overlay::virtual_height);
				out.image(layer::b_under, &assets::get().scrim, white, 0.f, 628.f, overlay::virtual_width, overlay::virtual_height);
				// Practice's panel: gradient fill, a top hairline, the dark crimson bottom edge, the header rule and the column rule.
				panel_frame(out, 56.f, 112.f, 1224.f, 612.f);
				out.rect(layer::b_lines, hairline, 56.f, 152.f, 1168.f, 1.f);
				out.rect(layer::b_lines, hairline, 384.f, 153.f, 1.f, 457.f);
				out.text(type.title, text_1, 71.f, baseline(type.title, 132.f), view.top ? U"MENU" : view.controls ? U"CONTROLS" :
																													 U"SETTINGS");
				const auto context = utf8_to_u32string(view.context);
				out.text(type.caption, text_3, 1209.f - text_width(type.caption, context), baseline(type.caption, 132.f), context);
			}

			void top_rows()
			{
				// The top level: the pause menu's rows in the list column, the two that end this run set apart below. The left
				// column says what those two do to the run in view.
				std::array<std::u32string, 5> lines{};
				const usz count = wrap_text(type.body, utf8_to_u32string(menu::leave_note(view.context)), 300.f, 300.f, lines.data(), lines.size());
				for (usz i = 0; i < count; ++i)
					out.text(type.body, text_2, 71.f, baseline(type.body, 176.f + 22.f * static_cast<f32>(i)), lines[i]);
				const rich_style note{type.body, text_3, 16.f};
				rich_text(&out, atlas, note, 71.f, 176.f + 22.f * static_cast<f32>(count) + 16.f, "{circle} or Esc returns to the game.");
				const u32 rows = static_cast<u32>(view.entries.size());
				const u32 focus = rows ? std::min(view.top_focus, rows - 1) : 0;
				for (u32 i = 0; i < rows; ++i)
				{
					const f32 top = menu::entry_top(i, rows);
					const bool focused = i == focus;
					out.rect(layer::b_bars, row_bar, menu::list_left, top, menu::list_width, menu::row_height);
					if (focused)
						focus_bar(out, menu::list_left, top, menu::list_width, menu::row_height);
					// Once Return to Launcher or Exit Game is chosen the other rows dim: the menu takes no more input.
					out.text(type.label, label_color(!view.leaving || focused, focused), menu::label_x, baseline(type.label, top + 14.f),
						utf8_to_u32string(view.entries[i]));
				}
			}

			void controls_help()
			{
				// One rich_text per line: a glyph after a line break in one run would be placed after the whole run's width.
				const rich_style note{type.body, text_2, 16.f};
				constexpr std::array<std::string_view, 5> lines{"Choose an action, then press", "the button you want for it.", "",
					"{dpad:lr} changes the controller.", "{circle} goes back."};
				for (usz i = 0; i < lines.size(); ++i)
					rich_text(&out, atlas, note, 71.f, 176.f + 22.f * static_cast<f32>(i), lines[i]);
				// In a two-PC set, the connection's route and frames, which the fight no longer shows.
				const auto snap = ::pbd::live_stats::load();
				if (snap.generation && snap.mode == ::pbd::live_stats::mode_session)
					rich_text(&out, atlas, {type.body, text_1, 16.f}, 71.f, 176.f + 22.f * static_cast<f32>(lines.size() + 1),
						::pbd::set_notices::connection_words(snap.input_delay, snap.depth_recent, snap.relayed != 0));
			}

			void timing_legend()
			{
				// Left column: when each kind of change takes effect, in the words of the rows' tags.
				struct legend_line
				{
					std::u32string_view tag, rows;
				};
				static constexpr std::array<legend_line, 3> legend{{{U"Now", U"Volume"}, {U"On close", U"Resolution, Window, VSync"}, {U"Next session", U"Audio Output, Display"}}};
				f32 y = 176.f;
				out.text(type.label, text_1, 71.f, baseline(type.label, y), U"When changes apply");
				y += 34.f;
				for (const auto& line : legend)
				{
					out.text(type.caption, aqua, 71.f, baseline(type.caption, y), line.tag);
					out.text(type.body, text_2, 71.f, baseline(type.body, y + 22.f), line.rows);
					y += 54.f;
				}
				const rich_style note{type.body, text_3, 16.f};
				rich_text(&out, atlas, note, 71.f, y + 8.f, view.has_top ? "{circle} or Esc goes back to the menu." : "{circle} or Esc closes Settings.");
			}

			void rows()
			{
				// Rows: label, the tag saying when a change applies (gold while one waits), and the value centred in the value column.
				const u32 focus = std::min(view.focus, view.row_count - 1);
				if (view.controls)
				{
					const auto large = atlas_ref::pick(output_scale * pbd::button_layout::glyph_h / 16.f, atlas.xbox);
					pbd::button_layout::draw(out, atlas, large, type, view, focus);
				}
				for (u32 i = 0; i < view.row_count && !view.top && !view.controls; ++i)
				{
					const auto& row = view.rows[i];
					const f32 top = menu::row_top(i);
					// While a list is open its row keeps a white label and the gold underline up to the option stack.
					const bool parent = view.dropdown && i == focus;
					const bool focused = i == focus && !view.dropdown;
					out.rect(layer::b_bars, row_bar, menu::list_left, top, menu::list_width, menu::row_height);
					if (focused)
						focus_bar(out, menu::list_left, top, menu::list_width, menu::row_height);
					out.text(type.label, parent ? white : label_color(true, focused), menu::label_x, baseline(type.label, top + 14.f), utf8_to_u32string(row.label));
					if (parent)
						out.rect(layer::b_lines, gold, menu::list_left, top + 25.f, menu::dropdown_left - menu::list_left, 3.f);
					const auto when = menu::timing_text(row.when, row.pending);
					if (!when.empty())
					{
						const color4f tag_color = row.pending ? gold : focused ? rgb(0xFFFFFF, .75f) :
						                                                         text_3;
						out.text(type.caption, tag_color, menu::timing_x, baseline(type.caption, top + 14.f), utf8_to_u32string(when));
					}
					if (!row.value.empty())
					{
						const auto value = fit_text(type.label, utf8_to_u32string(row.value), menu::value_width);
						out.text(type.label, focused || parent ? white : text_1, std::round(menu::value_centre - text_width(type.label, value) / 2.f),
							baseline(type.label, top + 14.f), value);
					}
					if (focused && static_cast<menu::row>(i) == menu::row::volume)
					{
						// The focused Volume row carries its < and > (Left/Right, or a click on either).
						const auto left = menu::stepper_rect(false), right = menu::stepper_rect(true);
						out.sprite(atlas, pbd_glyphs::chevron_l, left.x + (left.w - 7.f) / 2.f, top + 8.f);
						out.sprite(atlas, pbd_glyphs::chevron_r, right.x + (right.w - 7.f) / 2.f, top + 8.f);
					}
				}
			}

			void dropdown()
			{
				if (view.dropdown && !view.top)
				{
					// Option stack at the value column, overhanging the panel's right edge, as in the Practice menu: the option in effect
					// in aqua, the highlighted one on the focus bar.
					const u32 count = static_cast<u32>(std::min(view.options.size(), menu::max_options));
					const f32 height = menu::option_pitch * count;
					const f32 stack = view.stack_top;
					constexpr f32 left = menu::dropdown_left, width = menu::dropdown_width;
					dropdown_layer.image(layer::b_under, &assets::get().dropdown, white, left, stack, left + width, stack + height);
					dropdown_layer.rect(layer::b_lines, top_edge, left, stack - 1.f, width, 1.f);
					dropdown_layer.rect(layer::b_lines, edge_dark, left, stack + height, width, 2.f);
					for (u32 j = 0; j < count; ++j)
					{
						const f32 top = stack + menu::option_pitch * j;
						const bool focused = j == view.option_focus;
						const bool current = j == view.option_current;
						if (focused)
							focus_bar(dropdown_layer, left, top, width, menu::option_pitch);
						const auto label = fit_text(type.label, utf8_to_u32string(view.options[j].label), width - 110.f);
						dropdown_layer.text(type.label, focused ? white : current ? aqua :
																					text_1,
							left + 14.f, baseline(type.label, top + 15.f), label);
						if (current)
						{
							constexpr std::u32string_view note = U"Selected";
							dropdown_layer.text(type.body, focused ? rgb(0xFFFFFF, .75f) : text_3, left + width - 14.f - text_width(type.body, note),
								baseline(type.body, top + 15.f), note);
						}
					}
				}
			}

			void controls()
			{
				// Help line for the focused row, with a faint underline as wide as the text.
				help_line(chrome, atlas, type, view.help, view.controls);
				// Control bar: only the buttons the menu acts on in its current state.
				std::vector<prompt_item> prompts;
				prompts.reserve(view.prompts.size());
				for (const auto& item : view.prompts)
					prompts.push_back({item.keys, item.label});
				prompt_bar(chrome, atlas, type.caption, 56.f, 693.f, 1224.f, prompts);
			}

			void draw(compiled_resource& compiled)
			{
				background();
				if (view.top)
					top_rows();
				else if (view.controls)
					controls_help();
				else
					timing_legend();
				rows();
				dropdown();
				controls();
				compiled.clear();
				out.flush(compiled);
				dropdown_layer.flush(compiled);
				chrome.flush(compiled);
			}
		};

		void pbd_settings_overlay::refresh(const ::pbd::settings_menu::view& view)
		{
			assets::get();
			static const fonts type = preload_fonts();
			settings_drawing draw{view, type, atlas_ref::pick(m_output_scale, pbd_xbox_glyphs()), m_output_scale};
			draw.draw(m_compiled);
		}
	} // namespace overlays
} // namespace rsx
