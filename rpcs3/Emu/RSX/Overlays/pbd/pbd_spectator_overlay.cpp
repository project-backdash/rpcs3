#include "stdafx.h"
#include "pbd_overlays.h"
#include "overlay_pbd_draw.h"
#include "Emu/System.h"

#include <algorithm>
#include <cmath>
#include <string_view>

// A watcher's screen over the watched game, and the corner chat.

namespace rsx
{
	namespace overlays
	{
		void pbd_corner_chat_overlay::init()
		{
			pbd::preload_fonts();
			pbd::assets::get();
			m_compiled.clear();
			visible = false;
		}

		void pbd_corner_chat_overlay::update(u64)
		{
			using namespace pbd;
			m_snapshot = ::pbd::corner_chat::load();
			visible = !Emu.IsStopped() && m_snapshot.generation == static_cast<u64>(Emu.GetEmulationIdentifier()) && m_snapshot.count;
			m_compiled.clear();
			if (!visible)
				return;
			static const fonts type = preload_fonts();
			// Bottom-left panel (the feedback panel's translucent fill, no box), newest message at the bottom. Each message is
			// its author in gold and the text in light ink, wrapped to two lines; a message fades as a whole and the panel
			// follows the newest one.
			constexpr f32 left = 34.f, width = 420.f, pad = 12.f, pitch = 22.f, bottom = 696.f;
			constexpr usz wrap_lines = 2;
			struct chat_entry
			{
				std::u32string author;
				std::array<std::u32string, wrap_lines> lines{};
				usz count = 0;
				f32 opacity = 0.f;
			};
			std::array<chat_entry, ::pbd::corner_chat::max_lines> messages{};
			const usz count = std::min<usz>(m_snapshot.count, messages.size());
			usz total = 0;
			f32 panel_alpha = 0.f;
			for (usz i = 0; i < count; ++i)
			{
				const auto& line = m_snapshot.lines[i];
				auto& item = messages[i];
				item.opacity = line.opacity;
				if (!line.author.empty())
					item.author = utf8_to_u32string(line.author);
				const f32 name_w = item.author.empty() ? 0.f : text_width(type.caption, item.author) + 8.f;
				const f32 inner = width - 2.f * pad;
				const auto body = utf8_to_u32string(line.author.empty() ? line.text : line.body);
				item.count = std::max<usz>(1, pbd::wrap_text(type.body, body, inner - name_w, inner, item.lines.data(), item.lines.size()));
				total += item.count;
				panel_alpha = std::max(panel_alpha, line.opacity);
			}
			const f32 top = bottom - (2.f * 8.f + pitch * total);
			layer out;
			out.image(layer::b_under, &assets::get().panel, rgb(0xFFFFFF, .62f * panel_alpha), left, top, left + width, bottom);
			out.rect(layer::b_lines, faded(top_edge, panel_alpha), left, top, width, 1.f);
			out.rect(layer::b_lines, faded(edge_dark, panel_alpha), left, bottom - 2.f, width, 2.f);
			f32 centre = top + 8.f + pitch / 2.f;
			for (usz i = 0; i < count; ++i)
			{
				const auto& item = messages[i];
				f32 x = left + pad;
				if (!item.author.empty())
					x += out.text(type.caption, faded(gold, item.opacity), x, baseline(type.caption, centre), item.author) + 8.f;
				for (usz k = 0; k < item.count; ++k)
				{
					out.text(type.body, faded(text_1, item.opacity), k ? left + pad : x, baseline(type.body, centre), item.lines[k]);
					centre += pitch;
				}
			}
			out.flush(m_compiled);
		}

		compiled_resource pbd_corner_chat_overlay::get_compiled()
		{
			if (!visible || Emu.IsStopped() || m_snapshot.generation != static_cast<u64>(Emu.GetEmulationIdentifier()))
				return {};
			return ::pbd::corner_chat::compile_if_current(m_snapshot, [&]()
				{
					return m_compiled;
				});
		}

		using namespace pbd;
		namespace ui = ::pbd::spectator_ui;

		struct watcher_screen_drawing
		{
			layer& out;
			const fonts& type;
			const atlas_ref& atlas;
			const ui::view& screen;
			u64 now_ms;
			static constexpr f32 canvas_w = 1280.f, canvas_h = 720.f, bar_centre = 693.f;
			static constexpr f32 chat_left = 40.f, chat_top = 196.f, chat_width = 480.f, chat_height = 412.f, chat_pad = 14.f, chat_inner = chat_width - 2.f * chat_pad;

			void score(f32& score_left)
			{
				const std::string score = ui::score_line(screen);
				if (screen.bar || !score.empty())
					out.image(layer::b_under, &assets::get().scrim, white, 0.f, 628.f, canvas_w, canvas_h);
				// The score line, right-aligned on the bar's line.
				score_left = 1224.f;
				if (!score.empty())
				{
					const auto line = utf8_to_u32string(score);
					score_left = 1224.f - text_width(type.caption, line);
					out.text(type.caption, text_1, score_left, baseline(type.caption, bar_centre), line);
				}
			}

			void bar(f32 score_left)
			{
				// The bar: glyphs and a label per group from the left, a gold count after a label that has one; groups that would
				// reach the score line are left out.
				if (screen.bar)
				{
					const rich_style keys{type.caption, text_1, 16.f};
					const f32 limit = score_left - 32.f;
					f32 cursor = 56.f;
					bool first = true;
					for (const auto& item : ui::bar_items(screen))
					{
						const auto label = utf8_to_u32string(item.label);
						const auto badge = item.badge ? utf8_to_u32string(std::to_string(item.badge)) : std::u32string{};
						const f32 badge_w = badge.empty() ? 0.f : 6.f + text_width(type.caption, badge) + 18.f;
						const f32 width = rich_text(nullptr, atlas, keys, 0.f, bar_centre, item.keys) + 7.f + text_width(type.caption, label) + badge_w;
						const f32 at = first ? cursor : cursor + 26.f;
						if (at + width > limit)
							break;
						const f32 k = rich_text(&out, atlas, keys, at, bar_centre, item.keys);
						const f32 label_w = out.text(type.caption, text_1, at + k + 7.f, baseline(type.caption, bar_centre), label);
						if (!badge.empty())
							tag(out, type.caption, gold, rgb(0x0B0304), at + k + 7.f + label_w + 6.f, false, bar_centre - 10.f, badge);
						cursor = at + width;
						first = false;
					}
				}
			}

			void preview()
			{
				// A new chat line while the chat is closed: author in aqua, the text on one line, fading out after a few seconds.
				if (screen.new_line && !screen.chat)
				{
					const f32 alpha = ui::preview_opacity(screen.new_line_since_ms, now_ms);
					if (alpha > 0.f)
					{
						constexpr f32 x = 40.f, top = 610.f, w = 560.f, h = 42.f, centre = top + h / 2.f;
						out.image(layer::b_under, &assets::get().panel, rgb(0xFFFFFF, .9f * alpha), x, top, x + w, top + h);
						out.rect(layer::b_lines, faded(aqua, alpha), x + 14.f, top + 9.f, 3.f, h - 18.f);
						f32 cursor = x + 27.f;
						if (!screen.new_line->author.empty())
							cursor += out.text(type.label, faded(aqua, alpha), cursor, baseline(type.label, centre), utf8_to_u32string(screen.new_line->author)) + 10.f;
						out.text(type.body, faded(text_1, alpha), cursor, baseline(type.body, centre),
							fit_text(type.body, utf8_to_u32string(screen.new_line->text), std::max(0.f, x + w - 16.f - cursor)));
					}
				}
			}

			void chat_tabs()
			{
				const auto& chat = *screen.chat;
				dialog_frame(out, chat_left, chat_top, chat_width, chat_height);
				const f32 tab_centre = chat_top + 19.f;
				f32 tab_x = chat_left + chat_pad;
				if (chat.tabs.size() > 1)
					tab_x += rich_text(&out, atlas, {type.caption, white, 16.f}, tab_x, tab_centre, "{l1}") + 12.f;
				for (usz i = 0; i < chat.tabs.size(); ++i)
				{
					const bool current = i == chat.tab;
					const f32 tab_w = out.text(type.label, current ? text_1 : text_3, tab_x, baseline(type.label, tab_centre), utf8_to_u32string(chat.tabs[i]));
					if (current)
						out.rect(layer::b_lines, crimson, tab_x, chat_top + 34.f, tab_w, 2.f);
					tab_x += tab_w + 22.f;
				}
				if (chat.tabs.size() > 1)
					rich_text(&out, atlas, {type.caption, white, 16.f}, tab_x - 10.f, tab_centre, "{r1}");
				out.rect(layer::b_lines, top_edge, chat_left, chat_top + 38.f, chat_width, 1.f);
			}

			void chat_log()
			{
				const auto& chat = *screen.chat;
				// The log: an author in gold and the text wrapped to two lines; a lobby line in the dim ink without an author. As
				// many of the newest lines as fit, the newest at the bottom.
				constexpr f32 pitch = 22.f;
				const f32 log_top = chat_top + 46.f, log_bottom = chat_top + chat_height - 120.f;
				struct log_entry
				{
					std::u32string author;
					std::array<std::u32string, 2> lines{};
					usz count = 0;
					bool system = false;
				};
				std::vector<log_entry> entries;
				usz rows = 0;
				const usz max_rows = static_cast<usz>((log_bottom - log_top) / pitch);
				for (usz i = chat.lines.size(); i-- > 0;)
				{
					const auto& line = chat.lines[i];
					log_entry entry;
					entry.system = line.system;
					if (!line.system && !line.author.empty())
						entry.author = utf8_to_u32string(line.author);
					const f32 name_w = entry.author.empty() ? 0.f : text_width(type.caption, entry.author) + 8.f;
					const auto body = utf8_to_u32string(line.text);
					entry.count = std::max<usz>(1, wrap_text(type.body, body, chat_inner - name_w, chat_inner, entry.lines.data(), entry.lines.size()));
					if (rows + entry.count > max_rows)
						break;
					rows += entry.count;
					entries.push_back(std::move(entry));
				}
				f32 centre = log_bottom - pitch * static_cast<f32>(rows) + pitch / 2.f;
				for (usz i = entries.size(); i-- > 0;)
				{
					const auto& entry = entries[i];
					f32 cx = chat_left + chat_pad;
					if (!entry.author.empty())
						cx += out.text(type.caption, gold, cx, baseline(type.caption, centre), entry.author) + 8.f;
					for (usz k = 0; k < entry.count; ++k)
					{
						out.text(type.body, entry.system ? text_3 : text_1, k ? chat_left + chat_pad : cx, baseline(type.body, centre), entry.lines[k]);
						centre += pitch;
					}
				}
				out.rect(layer::b_lines, top_edge, chat_left + chat_pad, chat_top + chat_height - 116.f, chat_inner, 1.f);
				if (!chat.to.empty())
					out.text(type.caption, text_2, chat_left + chat_pad, baseline(type.caption, chat_top + chat_height - 102.f), fit_text(type.caption, utf8_to_u32string(chat.to), chat_inner));
				out.text(type.body, text_3, chat_left + chat_pad, baseline(type.body, chat_top + chat_height - 80.f), U"Type anything else in the launcher's chat.");
			}

			void chat_quick()
			{
				const auto& chat = *screen.chat;
				// Two rows of Quick lines, scrolled to keep the highlighted reply visible and framed in gold.
				const f32 quick_centre = chat_top + chat_height - 52.f;
				const f32 heading_width = rich_text(&out, atlas, rich_style{type.caption, text_3, 16.f}, chat_left + chat_pad, quick_centre, "{triangle} Quick lines") + 10.f;
				std::vector<std::u32string> labels;
				std::vector<f32> widths;
				for (usz i = 0; i < chat.quick.size(); ++i)
				{
					labels.push_back(fit_text(type.caption, utf8_to_u32string(chat.quick[i]), chat_inner - 20.f));
					widths.push_back(text_width(type.caption, labels.back()) + 20.f);
				}
				for (const auto& chip : ui::quick_layout(widths, screen.highlight, chat_inner, heading_width))
				{
					const f32 qx = chat_left + chat_pad + chip.x;
					const f32 row_centre = quick_centre + 32.f * chip.row;
					const f32 chip_w = widths[chip.index];
					const bool focus = chip.index == screen.highlight;
					const f32 chip_top = row_centre - 14.f;
					const color4f edge = focus ? gold : rgb(0xE9EEED, .2f);
					out.rect(layer::b_bars, focus ? rgb(0xF4B63F, .08f) : row_bar, qx, chip_top, chip_w, 28.f);
					out.rect(layer::b_lines, edge, qx, chip_top, chip_w, 1.f);
					out.rect(layer::b_lines, edge, qx, chip_top + 27.f, chip_w, 1.f);
					out.rect(layer::b_lines, edge, qx, chip_top + 1.f, 1.f, 26.f);
					out.rect(layer::b_lines, edge, qx + chip_w - 1.f, chip_top + 1.f, 1.f, 26.f);
					out.text(type.caption, focus ? gold : text_2, qx + 10.f, baseline(type.caption, row_centre), labels[chip.index]);
				}
			}

			void profiles()
			{
				constexpr f32 w = 360.f, gap = 24.f, top = 176.f;
				for (usz side = 0; side < 2; ++side)
				{
					const auto& person = (*screen.profiles)[side];
					const f32 h = 74.f + 26.f * static_cast<f32>(person.lines.size());
					const f32 x = side ? 640.f + gap / 2.f : 640.f - gap / 2.f - w;
					dialog_frame(out, x, top, w, h);
					out.text(type.caption, text_3, x + 20.f, baseline(type.caption, top + 22.f), side ? U"RIGHT SIDE" : U"LEFT SIDE");
					out.text(type.label, text_1, x + 20.f, baseline(type.label, top + 50.f), fit_text(type.label, utf8_to_u32string(person.name), w - 40.f));
					for (usz i = 0; i < person.lines.size(); ++i)
						out.text(type.body, text_2, x + 20.f, baseline(type.body, top + 84.f + 26.f * static_cast<f32>(i)),
							fit_text(type.body, utf8_to_u32string(person.lines[i]), w - 40.f));
				}
			}

			void draw()
			{
				f32 score_left;
				score(score_left);
				bar(score_left);
				preview();
				if (screen.chat)
				{
					chat_tabs();
					chat_log();
					chat_quick();
				}
				else if (screen.profiles)
					profiles();
			}
		};

		void pbd_spectator_overlay::init()
		{
			pbd::preload_fonts();
			pbd::assets::get();
			m_compiled.clear();
			visible = true;
			update(0);
		}

		void pbd_spectator_overlay::set_render_viewport(u16 width, u16 height)
		{
			if (width && height)
				m_output_scale = std::min(width / 1280.f, height / 720.f);
		}

		static void draw_watch_preparation(layer& out, const fonts& type, const ::pbd::spectator::presentation& display)
		{
			const auto centred = [&](font* f, color4f ink, f32 centre_y, std::string_view text)
			{
				const auto wide = utf8_to_u32string(text);
				out.text(f, ink, std::round(640.f - text_width(f, wide) / 2.f), baseline(f, centre_y), wide);
			};
			// Preparation hides the game completely: the panel gradient across the screen with the state centred on it.
			out.image(layer::b_under, &assets::get().panel, white, 0.f, 0.f, overlay::virtual_width, overlay::virtual_height);
			centred(type.caption, text_3, 326.f, "PROJECT BACKDASH");
			// A watch that ended without a fault (a drawn game is replayed under its own number) says why.
			if (!display.notice.empty())
			{
				centred(type.title, text_1, 352.f, "DRAW");
				centred(type.body, text_2, 384.f, display.notice);
			}
			// Between two games of the set (the launcher's waiting control): what the watch waits for.
			else if (!display.waiting_title.empty())
			{
				centred(type.title, text_1, 352.f, display.waiting_title);
				centred(type.body, text_2, 384.f, display.waiting_text);
			}
			else
			{
				centred(type.title, text_1, 352.f, display.catch_up ? "CATCHING UP" : "PREPARING TO WATCH");
				centred(type.body, text_2, 384.f, display.catch_up ? "Catching up with the live match." : "The match appears here in a moment.");
			}
		}

		static void draw_watcher_status(layer& out, const fonts& type, const ::pbd::spectator::presentation& display)
		{
			// Watcher status: a top-centre band under the game's own HUD. The mode, the players with the set score between them,
			// then how far behind live the view runs (or the analyzed game, frame and sync state), separated by thin rules.
			const std::string dot = "  \xC2\xB7  ";
			struct run
			{
				std::u32string text;
				font* f = nullptr;
				color4f ink;
			};
			std::array<run, 3> runs{};
			usz count = 0;
			runs[count++] = {utf8_to_u32string(display.analyze ? "Analyze" : "Watching"), type.caption, text_3};
			if (display.left_name.empty() && display.right_name.empty())
				runs[count++] = {U"Waiting for the next set", type.caption, text_1};
			else
				runs[count++] = {utf8_to_u32string(display.left_name + "   " + std::to_string(display.left_score) + " \xE2\x80\x93 " +
												   std::to_string(display.right_score) + "   " + display.right_name),
					type.caption, text_1};
			if (display.analyze)
			{
				const char* sync = display.sync_mask == 7 ? "Combat sync verified" : display.sync_mask ? "Selected sync verified" :
				                                                                                         "Menu inputs verified";
				runs[count++] = {utf8_to_u32string("Game " + std::to_string(display.native_game) + dot + "Frame " + std::to_string(display.native_through) + dot + sync),
					type.body, text_2};
			}
			else if (!display.broadcast)
				runs[count++] = {utf8_to_u32string("About " + std::to_string((display.delay_frames + 30) / 60) + " s behind"), type.body, text_2};
			constexpr f32 gap = 10.f, centre = 109.f;
			f32 content = 0.f;
			for (usz i = 0; i < count; ++i)
				content += text_width(runs[i].f, runs[i].text) + (i ? 2.f * gap + 1.f : 0.f);
			const f32 left = std::round(640.f - content / 2.f) - 72.f, right = left + content + 144.f;
			band_strip(out, layer::b_under, band_fill, left, right, 92.f, 126.f);
			band_strip(out, layer::b_lines, rgb(0xE9EEED, .16f), left, right, 92.f, 93.f);
			f32 x = left + 72.f;
			for (usz i = 0; i < count; ++i)
			{
				if (i)
				{
					out.rect(layer::b_lines, separator, x + gap, centre - 8.f, 1.f, 16.f);
					x += 2.f * gap + 1.f;
				}
				x += out.text(runs[i].f, runs[i].ink, x, baseline(runs[i].f, centre), runs[i].text);
			}
		}

		static void draw_analysis_history(layer& out, const atlas_ref& atlas, const ::pbd::spectator::presentation& display)
		{
			// Both pads' executed history in the Practice columns, newest on top. A count in gold marks a run whose applied
			// word differs from the recorded one.
			for (u32 port = 0; port < 2; ++port)
			{
				const u32 runs_held = std::min<u32>(display.history_count[port], ::pbd::spectator::presentation::history_runs);
				history_column(out, atlas, port, runs_held, [&](u32 i)
					{
						const auto& held = display.history[port][runs_held - 1 - i];
						return history_row{held.applied, held.frames, held.held != held.applied ? gold : white};
					});
			}
		}

		void pbd_spectator_overlay::update(u64)
		{
			if (Emu.IsStopped())
			{
				visible = false;
				return;
			}
			const auto display = ::pbd::spectator::load_presentation(static_cast<u64>(Emu.GetEmulationIdentifier()));
			visible = display.active;
			m_compiled.clear();
			if (!visible)
				return;
			m_cover = display.hidden;
			m_analyze = !m_cover && display.analyze && display.native_through != 0;
			static const fonts type = preload_fonts();
			layer out;
			if (m_cover)
			{
				draw_watch_preparation(out, type, display);
				out.flush(m_compiled);
				return;
			}

			// A launcher that sends the watcher's own screen (watch_ui) gets the spectator HUD instead of the status band,
			// and only while a fight is on screen: nothing in the intros, at a K.O. or between rounds. The revision is read before
			// the view, so a view published meanwhile is drawn by the next update (get_compiled checks it).
			m_screen_revision = ::pbd::spectator_ui::revision();
			const auto screen = ::pbd::spectator_ui::load(static_cast<u64>(Emu.GetEmulationIdentifier()));
			if (screen.present)
			{
				if (screen.shown && !display.broadcast)
				{
					const auto atlas = atlas_ref::pick(m_output_scale, pbd_xbox_glyphs());
					watcher_screen_drawing draw{out, type, atlas, screen, ::pbd::spectator_ui::steady_ms()};
					draw.draw();
				}
			}
			else
				draw_watcher_status(out, type, display);
			if (m_analyze)
				draw_analysis_history(out, atlas_ref::pick(m_output_scale, pbd_xbox_glyphs()), display);
			out.flush(m_compiled);
		}

		compiled_resource pbd_spectator_overlay::get_compiled()
		{
			if (Emu.IsStopped())
				return {};
			// A newly requested hide cannot compose a stale exposed frame between update and compilation.
			const auto display = ::pbd::spectator::load_presentation(static_cast<u64>(Emu.GetEmulationIdentifier()));
			if (display.active != visible || display.hidden != m_cover || m_analyze != (!display.hidden && display.analyze && display.native_through != 0) ||
				m_screen_revision != ::pbd::spectator_ui::revision())
				update(0);
			return m_compiled;
		}
	} // namespace overlays
} // namespace rsx
