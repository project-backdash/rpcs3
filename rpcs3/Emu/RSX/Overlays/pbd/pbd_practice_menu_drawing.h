#pragma once

#include "overlay_pbd_draw.h"
#include "Emu/Cell/pbd_game_overlay.h"
#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/Cell/pbd_practice_mouse.h"

#include <cmath>
#include <span>

namespace rsx::overlays::pbd
{
	struct practice_menu_drawing
	{
		const ::pbd::game_overlay::snapshot& panel;
		const ::pbd::practice::snapshot& practice;
		const fonts& type;
		atlas_ref atlas;
		::pbd::practice::mouse::layout pointer_layout;
		layer out, dropdown_layer, dialog_layer, chrome;
		const u32 rows = std::min<u32>(panel.row_count, static_cast<u32>(::pbd::game_overlay::max_rows));
		static constexpr f32 prompt_centre = 693.f;

		void decision()
		{
			// A decision is a centred message dialog over the dimmed game, like the keep-recording question: the
			// message, optional detail lines (with glyph tokens, 26 units each), then one full-width bar per option with
			// the crimson focus bar on the highlighted one.
			std::array<std::string_view, 4> lines{};
			u32 line_count = 0;
			for (std::string_view rest = practice.dialog_detail; !rest.empty() && line_count < lines.size();)
			{
				const usz cut = rest.find('\n');
				lines[line_count++] = rest.substr(0, cut);
				rest = cut == std::string_view::npos ? std::string_view{} : rest.substr(cut + 1);
			}
			const f32 extra = 26.f * line_count;
			constexpr f32 x = 380.f, w = 520.f;
			const f32 h = 82.f + extra + 30.f * rows + 20.f;
			const f32 y = std::round(360.f - h / 2.f);
			dialog_frame(out, x, y, w, h);
			const auto message = utf8_to_u32string(practice.dialog_title);
			out.text(type.label, text_1, std::round(640.f - text_width(type.label, message) / 2.f), baseline(type.label, y + 42.f), message);
			const rich_style detail_style{type.body, text_2, 16.f};
			for (u32 i = 0; i < line_count; i++)
			{
				const f32 centre = y + 68.f + 26.f * i;
				const f32 line_w = rich_text(nullptr, atlas, detail_style, 0.f, centre, lines[i]);
				rich_text(&out, atlas, detail_style, std::round(640.f - line_w / 2.f), centre, lines[i]);
			}
			for (u32 i = 0; i < rows; i++)
			{
				const f32 top = y + 82.f + extra + 30.f * i;
				option_bar(out, type.label, x + 1.f, top, w - 2.f, utf8_to_u32string(panel.rows[i].label), i == panel.selected, panel.rows[i].enabled);
				if (pointer_layout.row_count < pointer_layout.rows.size())
					pointer_layout.rows[pointer_layout.row_count++] = {x + 1.f, top, w - 2.f, 28.f, practice.mouse_row_base + i};
			}
		}

		void header()
		{
			// Panel: gradient fill, a top hairline and a dark crimson bottom edge, no side edges.
			panel_frame(out, 56.f, ::pbd::practice::menu_panel_top, 1224.f, 612.f);
			out.rect(layer::b_lines, hairline, 56.f, 152.f, 1168.f, 1.f);
			out.rect(layer::b_lines, hairline, 384.f, 153.f, 1.f, 457.f);
			out.text(type.title, text_1, 71.f, baseline(type.title, 132.f), U"PRACTICE");
			// Header right: a menu toast (title and detail) in the brighter colour, otherwise the page's status line.
			const bool toast = !practice.toast.empty() && practice.header == practice.toast;
			std::string header = practice.header;
			for (usz at = header.find('\t'); at != std::string::npos; at = header.find('\t'))
				header.replace(at, 1, "  \xC2\xB7  ");
			for (auto& c : header)
				if (c == '\n')
					c = ' ';
			const rich_style header_style{type.caption, toast ? text_1 : text_3, 16.f};
			const f32 header_w = rich_text(nullptr, atlas, header_style, 0.f, 132.f, header);
			rich_text(&out, atlas, header_style, 1209.f - header_w, 132.f, header);
		}

		void categories()
		{
			// The active category has a crimson bar while the rail has focus and a gold underline while the list has focus.
			// Unavailable categories are dimmed; clicking a category focuses it and shows its list.
			const int hovered_page = ::pbd::practice::mouse::hovered_page(pointer_layout.owner);
			for (u32 i = 0; i < ::pbd::practice::max_pages; i++)
			{
				if (practice.pages[i].empty())
					continue;
				const f32 top = 160.f + 30.f * i;
				const bool active = i == practice.page;
				const bool rail_focus = active && practice.rail_focus;
				const bool hover = practice.mouse_pages && hovered_page == static_cast<int>(i);
				const bool unavailable = practice.page_unavailable[i];
				if (rail_focus)
					focus_bar(out, 56.f, top, 316.f, 28.f);
				const color4f name_color = unavailable     ? (rail_focus ? rgb(0xFFFFFF, .62f) : rgb(0x8A9C9C, hover ? .8f : .62f)) :
				                           rail_focus      ? white :
				                           active || hover ? text_1 :
				                                             text_2;
				out.text(type.label, name_color, 71.f, baseline(type.label, top + 14.f), utf8_to_u32string(practice.pages[i]));
				if (!practice.page_notes[i].empty())
				{
					const auto note = utf8_to_u32string(practice.page_notes[i]);
					out.text(type.body, rail_focus ? rgb(0xFFFFFF, .75f) : text_3, 358.f - text_width(type.body, note), baseline(type.body, top + 14.f), note);
				}
				if (active && !rail_focus)
					out.rect(layer::b_lines, gold, 56.f, top + 25.f, 312.f, 3.f);
				if (practice.mouse_pages && pointer_layout.page_count < pointer_layout.pages.size())
					pointer_layout.pages[pointer_layout.page_count++] = {56.f, top, 316.f, 28.f, i};
			}
		}

		void frequency(const ::pbd::game_overlay::row& row, f32 top, bool focus, bool parent)
		{
			// A filled dummy slot's frequency (0 excludes it from playback). Only the focused row shows the L1/R1 picker;
			// other rows, including an open dropdown's parent, show the number alone.
			const auto number = utf8_to_u32string(std::to_string(*row.frequency));
			out.text(type.caption, focus || parent ? text_1 : text_3, 618.f, baseline(type.caption, top + 14.f), U"Frequency");
			if (focus)
			{
				rich_text(&out, atlas, {type.caption, white, 16.f}, 696.f, top + 14.f, "{l1}");
				out.sprite(atlas, pbd_glyphs::chevron_l, 729.f, top + 8.f);
				out.text(type.label, white, std::round(752.f - text_width(type.label, number) / 2.f), baseline(type.label, top + 14.f), number);
				out.sprite(atlas, pbd_glyphs::chevron_r, 768.f, top + 8.f);
				rich_text(&out, atlas, {type.caption, white, 16.f}, 784.f, top + 14.f, "{r1}");
				pointer_layout.steppers[0] = {696.f, top, 40.f, 28.f, 0};
				pointer_layout.steppers[1] = {768.f, top, 40.f, 28.f, 1};
				pointer_layout.stepper_count = 2;
			}
			else
			{
				const color4f number_color = parent || *row.frequency != 1 ? text_1 : aqua;
				out.text(type.caption, number_color, std::round(752.f - text_width(type.caption, number) / 2.f), baseline(type.caption, top + 14.f), number);
			}
		}

		void list_row(u32 i, f32 top, bool list_open)
		{
			const auto& row = panel.rows[i];
			// While a dropdown is open its row keeps a white label and the gold underline up to the option stack.
			const bool parent = list_open && i == practice.menu_dropdown.row;
			const bool focus = i == panel.selected && !list_open && !practice.rail_focus;
			out.rect(layer::b_bars, row_bar, 396.f, top, 812.f, 28.f);
			if (focus)
				focus_bar(out, 396.f, top, 812.f, 28.f);
			out.text(type.label, parent ? white : label_color(row.enabled, focus), 415.f, baseline(type.label, top + 14.f), utf8_to_u32string(panel.rows[i].label));
			if (parent)
				out.rect(layer::b_lines, gold, 396.f, top + 25.f, 849.f - 396.f, 3.f);
			if (!row.detail.empty())
			{
				const bool unavailable = !row.enabled || row.detail == "Unavailable";
				const color4f value_color = unavailable ? (focus ? rgb(0xFFFFFF, .62f) : text_3) : row.is_default ? aqua :
				                                                                                                    text_1;
				const auto value = utf8_to_u32string(row.detail);
				out.text(type.label, value_color, 1044.f - text_width(type.label, value) / 2.f, baseline(type.label, top + 14.f), value);
			}
			if (row.frequency)
				frequency(row, top, focus, parent);
			if (pointer_layout.row_count < pointer_layout.rows.size())
				pointer_layout.rows[pointer_layout.row_count++] = {396.f, top, 812.f, 28.f, practice.mouse_row_base + i};
		}

		void legend(const std::array<f32, ::pbd::game_overlay::max_rows>& tops)
		{
			const u32 count = std::min<u32>(practice.list_legend.count, static_cast<u32>(::pbd::practice::max_prompts));
			const rich_style keys_style{type.caption, text_1, 16.f};
			constexpr f32 left = 415.f, right = 1189.f, key_gap = 7.f, line_pitch = 26.f;
			const f32 bottom = tops[rows - 1] + 28.f;
			f32 centre = bottom + 38.f;
			std::array<f32, ::pbd::practice::max_prompts> key_w{}, label_w{};
			f32 total = 0.f;
			for (u32 i = 0; i < count; i++)
			{
				const auto& item = practice.list_legend.items[i];
				key_w[i] = rich_text(nullptr, atlas, keys_style, 0.f, centre, item.keys);
				label_w[i] = text_width(type.caption, utf8_to_u32string(item.label));
				total += key_w[i] + key_gap + label_w[i];
			}
			const f32 gap = count > 1 ? std::clamp((right - left - total) / (count - 1), 10.f, 26.f) : 0.f;
			out.rect(layer::b_lines, hairline, left, bottom + 16.f, right - left, 1.f);
			f32 x = left;
			for (u32 i = 0; i < count; i++)
			{
				const auto& item = practice.list_legend.items[i];
				if (x > left && x + key_w[i] + key_gap + label_w[i] > right)
				{
					x = left;
					centre += line_pitch;
				}
				rich_text(&out, atlas, keys_style, x, centre, item.keys);
				out.text(type.caption, text_2, x + key_w[i] + key_gap, baseline(type.caption, centre), utf8_to_u32string(item.label));
				x += key_w[i] + key_gap + label_w[i] + gap;
			}
		}

		void dropdown(const std::array<f32, ::pbd::game_overlay::max_rows>& tops)
		{
			const auto& dropdown = practice.menu_dropdown;
			// Option stack at the value column, overhanging the panel's right edge. The option in effect lines up with
			// its row where the panel allows; a hairline sits above the stack and the crimson edge below it.
			const u32 count = std::min<u32>(dropdown.count, static_cast<u32>(::pbd::practice::max_options));
			const f32 height = 30.f * count;
			const f32 stack = ::pbd::practice::dropdown_top(tops[dropdown.row], dropdown.current, count);
			constexpr f32 left = 849.f, width = 409.f;
			dropdown_layer.image(layer::b_under, &assets::get().dropdown, white, left, stack, left + width, stack + height);
			dropdown_layer.rect(layer::b_lines, top_edge, left, stack - 1.f, width, 1.f);
			dropdown_layer.rect(layer::b_lines, edge_dark, left, stack + height, width, 2.f);
			for (u32 j = 0; j < count; j++)
			{
				const auto& option = dropdown.options[j];
				const f32 top = stack + 30.f * j;
				const bool focus = j == dropdown.focus;
				if (focus)
					focus_bar(dropdown_layer, left, top, width, 30.f);
				const color4f option_color = !option.enabled ? label_color(false, focus) : focus         ? white :
				                                                                       option.is_default ? aqua :
				                                                                                           text_1;
				dropdown_layer.text(type.label, option_color, 863.f, baseline(type.label, top + 15.f), utf8_to_u32string(option.text));
				if (!option.note.empty())
				{
					const auto note = utf8_to_u32string(option.note);
					dropdown_layer.text(type.body, focus ? rgb(0xFFFFFF, .75f) : text_3, 1244.f - text_width(type.body, note), baseline(type.body, top + 15.f), note);
				}
				if (pointer_layout.option_count < pointer_layout.options.size())
					pointer_layout.options[pointer_layout.option_count++] = {left, top, width, 30.f, j};
			}
			pointer_layout.modal = true;
		}

		void confirm()
		{
			// A Yes / No question stacked over the menu: a second dim over everything drawn so far, then the dialog
			// frame with the message, an optional detail line and one bar per answer. Only the answers and the
			// control bar answer the pointer while it is open.
			const auto& question = practice.confirm;
			dialog_layer.rect(layer::b_under, rgb(0x000000, .5f), 0.f, 0.f, overlay::virtual_width, overlay::virtual_height);
			const f32 extra = question.detail.empty() ? 0.f : 26.f;
			constexpr f32 x = 380.f, w = 520.f;
			constexpr u32 answer_rows = ::pbd::practice::answer_count;
			const f32 h = 82.f + extra + 30.f * answer_rows + 20.f;
			const f32 y = std::round(360.f - h / 2.f);
			dialog_frame(dialog_layer, x, y, w, h);
			const auto message = utf8_to_u32string(question.message);
			dialog_layer.text(type.label, text_1, std::round(640.f - text_width(type.label, message) / 2.f), baseline(type.label, y + 42.f), message);
			if (!question.detail.empty())
			{
				const rich_style detail_style{type.body, text_2, 16.f};
				const f32 detail_w = rich_text(nullptr, atlas, detail_style, 0.f, y + 68.f, question.detail);
				rich_text(&dialog_layer, atlas, detail_style, std::round(640.f - detail_w / 2.f), y + 68.f, question.detail);
			}
			pointer_layout.option_count = 0;
			for (u32 i = 0; i < answer_rows; i++)
			{
				const f32 top = y + 82.f + extra + 30.f * i;
				option_bar(dialog_layer, type.label, x + 1.f, top, w - 2.f, utf8_to_u32string(::pbd::practice::answers[i]), i == question.focus);
				if (pointer_layout.option_count < pointer_layout.options.size())
					pointer_layout.options[pointer_layout.option_count++] = {x + 1.f, top, w - 2.f, 28.f, i};
			}
			pointer_layout.stepper_count = 0;
			pointer_layout.modal = true;
		}

		void move_row(u32 i)
		{
			const auto& moves = practice.move_list;
			constexpr f32 left = 71.f, width = 1138.f, command_x = 700.f;
			const auto& line = moves.lines[i];
			const f32 top = 160.f + 30.f * i;
			if (line.header)
			{
				auto title = utf8_to_u32string(line.name);
				for (auto& c : title)
					if (c >= U'a' && c <= U'z')
						c = static_cast<char32_t>(c - U'a' + U'A');
				dialog_layer.text(type.caption, gold, left, baseline(type.caption, top + 16.f), title);
				dialog_layer.rect(layer::b_lines, rgb(0xF4B63F, .35f), left, top + 27.f, width, 1.f);
				return;
			}
			dialog_layer.rect(layer::b_bars, row_bar, left, top, width, 28.f);
			if (line.focus)
				focus_bar(dialog_layer, left, top, width, 28.f);
			// On: a filled aqua box and the name in white; off: an empty box.
			constexpr f32 box = 14.f;
			const f32 bx = left + 16.f, by = top + 7.f;
			const color4f box_ink = !line.selectable ? rgb(0x8A9C9C, .45f) : line.focus ? white :
			                                                                              text_2;
			if (line.on)
				dialog_layer.rect(layer::b_lines, line.focus ? white : aqua, bx, by, box, box);
			else
			{
				dialog_layer.rect(layer::b_lines, box_ink, bx, by, box, 1.f);
				dialog_layer.rect(layer::b_lines, box_ink, bx, by + box - 1.f, box, 1.f);
				dialog_layer.rect(layer::b_lines, box_ink, bx, by, 1.f, box);
				dialog_layer.rect(layer::b_lines, box_ink, bx + box - 1.f, by, 1.f, box);
			}
			const color4f name_ink = line.on && !line.focus ? white : label_color(line.selectable, line.focus);
			dialog_layer.text(type.label, name_ink, bx + box + 14.f,
				baseline(type.label, top + 14.f), fit_text(type.label, utf8_to_u32string(line.name), command_x - bx - box - 30.f));
			const color4f command_ink = !line.selectable ? rgb(0x8A9C9C, .62f) : line.focus ? white :
			                                                                 line.on        ? aqua :
			                                                                                  text_2;
			// The command in the game's arrow and button glyphs, a held direction in gold; its Tekken notation text
			// when it has no glyphs or they would run into the On/Off column.
			const f32 command_w = left + width - 110.f - command_x;
			const rich_style glyph_style{type.label, command_ink, 18.f, line.selectable ? white : command_ink, line.selectable ? gold : command_ink};
			if (!line.glyphs.empty() && rich_text(nullptr, atlas, glyph_style, command_x, top + 14.f, line.glyphs) <= command_w)
				rich_text(&dialog_layer, atlas, glyph_style, command_x, top + 14.f, line.glyphs);
			else
				dialog_layer.text(type.label, command_ink, command_x, baseline(type.label, top + 14.f),
					fit_text(type.label, utf8_to_u32string(line.command), command_w));
			const std::u32string_view state = !line.selectable ? U"Unavailable" : line.on ? U"On" :
			                                                                                U"Off";
			const color4f state_ink = line.focus ? white : !line.selectable ? rgb(0x8A9C9C, .62f) :
			                                           line.on              ? aqua :
			                                                                  text_3;
			dialog_layer.text(type.body, state_ink, left + width - 14.f - text_width(type.body, state), baseline(type.body, top + 14.f), state);
		}

		void move_list()
		{
			// The move list over the menu, laid out as the game's own command list: a second dim, a full panel with the
			// title and what the moves are for, then one line per row: a category title in gold, or a move with its
			// on/off box, name and command, the highlighted one on the focus bar. Only the control bar and the wheel
			// answer the pointer; a click anywhere else closes the list, as Circle does.
			const auto& moves = practice.move_list;
			dialog_layer.rect(layer::b_under, rgb(0x000000, .5f), 0.f, 0.f, overlay::virtual_width, overlay::virtual_height);
			panel_frame(dialog_layer, 56.f, ::pbd::practice::menu_panel_top, 1224.f, 612.f);
			dialog_layer.rect(layer::b_lines, hairline, 56.f, 152.f, 1168.f, 1.f);
			dialog_layer.text(type.title, text_1, 71.f, baseline(type.title, 132.f), U"MOVE LIST");
			const auto heading = utf8_to_u32string(moves.title + "  \xC2\xB7  " + moves.detail);
			const auto position = utf8_to_u32string(moves.position);
			const f32 position_w = text_width(type.caption, position);
			dialog_layer.text(type.caption, text_3, 1209.f - position_w, baseline(type.caption, 132.f), position);
			dialog_layer.text(type.caption, text_1, 1209.f - position_w - 24.f - text_width(type.caption, heading), baseline(type.caption, 132.f), heading);
			for (u32 i = 0; i < moves.count && i < moves.lines.size(); i++)
				move_row(i);
			// More rows above or below the ones in view: a short gold rule at that edge of the list.
			if (moves.more_above)
				dialog_layer.rect(layer::b_lines, gold, 640.f - 40.f, 155.f, 80.f, 2.f);
			if (moves.more_below)
				dialog_layer.rect(layer::b_lines, gold, 640.f - 40.f, 160.f + 30.f * static_cast<f32>(moves.count) + 2.f, 80.f, 2.f);
			pointer_layout.option_count = 0;
			pointer_layout.stepper_count = 0;
			pointer_layout.modal = true;
		}

		void help()
		{
			const auto& dropdown = practice.menu_dropdown;
			std::string_view help;
			if (panel.selected < rows)
				help = panel.rows[panel.selected].help;
			if (practice.layout == ::pbd::practice::menu_layout::list && practice.rail_focus)
				help = practice.rail_help;
			if (practice.layout == ::pbd::practice::menu_layout::list && dropdown.open && dropdown.focus < dropdown.count &&
				dropdown.focus < dropdown.options.size() && !dropdown.options[dropdown.focus].help.empty())
				help = dropdown.options[dropdown.focus].help;
			if (practice.layout == ::pbd::practice::menu_layout::list && practice.move_list.open)
				help = practice.move_list.help;
			help_line(chrome, atlas, type, help);
		}

		void prompt_area(usz i, f32 x0, f32 x1, u32 value)
		{
			if (pointer_layout.prompt_count < pointer_layout.prompts.size())
				pointer_layout.prompts[pointer_layout.prompt_count++] = {{x0, prompt_centre - 13.f, x1 - x0, 26.f, value}, static_cast<u32>(i)};
		}

		void prompt_target(usz i, f32 at, f32 width, int hovered_prompt)
		{
			const rich_style key_style{type.caption, text_1, 16.f};
			const auto& item = practice.menu_prompts.items[i];
			if (item.action == ::pbd::practice::prompt_action::none)
				return;
			if (hovered_prompt == static_cast<int>(i))
				chrome.rect(layer::b_lines, rgb(0xE9EEED, .7f), at, prompt_centre + 11.f, width, 1.f);
			const u32 value = static_cast<u32>(item.action);
			// Targets reach 4 units past the group on each side and 13 above and below the bar's centre line.
			const f32 left = at - 4.f, right = at + width + 4.f;

			const std::string_view keys = item.keys;
			const usz slash = keys.find("}/{");
			if (slash == std::string_view::npos)
			{
				prompt_area(i, left, right, value);
				return;
			}
			// The split sits in the middle of the "/" sprite (6 wide with 3 on each side) after the first key.
			const f32 split = at + rich_text(nullptr, atlas, key_style, 0.f, prompt_centre, keys.substr(0, slash + 1)) + 6.f;
			prompt_area(i, left, split, value);
			prompt_area(i, split, right, value | ::pbd::practice::prompt_second_key);
		}

		void prompts()
		{
			// Control bar. Every group the latch can act on is a pointer target spanning its glyphs and label; a pair splits
			// at the "/" so each key keeps its own meaning (L1 lowers the frequency, R1 raises it). The hovered group is
			// underlined. Targets come from the same measurement as the drawing.
			std::array<prompt_item, ::pbd::practice::max_prompts> prompts{};
			const u32 prompt_count = std::min<u32>(practice.menu_prompts.count, static_cast<u32>(prompts.size()));
			for (u32 i = 0; i < prompt_count; i++)
				prompts[i] = {practice.menu_prompts.items[i].keys, practice.menu_prompts.items[i].label};
			const int hovered_prompt = ::pbd::practice::mouse::hovered_prompt(pointer_layout.owner);
			prompt_bar(chrome, atlas, type.caption, 56.f, prompt_centre, 1224.f, std::span<const prompt_item>(prompts.data(), prompt_count), [&](usz i, f32 at, f32 width)
				{
					prompt_target(i, at, width, hovered_prompt);
				});
		}

		void list()
		{
			header();
			categories();
			// Rows keep their requested gaps on the 30-unit pitch.
			std::array<f32, ::pbd::game_overlay::max_rows> tops{};
			for (u32 i = 0, y = 160; i < rows; i++)
			{
				y += panel.rows[i].gap;
				tops[i] = static_cast<f32>(y);
				y += 30;
			}
			const auto& options = practice.menu_dropdown;
			const bool list_open = options.open && options.row < rows && options.count;
			for (u32 i = 0; i < rows; i++)
				list_row(i, tops[i], list_open);
			if (rows && practice.list_legend.count)
				legend(tops);
			if (!rows && !practice.list_note.empty())
			{
				const rich_style note_style{type.body, text_3, 16.f};
				rich_text(&out, atlas, note_style, 415.f, 174.f, practice.list_note);
			}
			if (list_open)
				dropdown(tops);
			if (practice.confirm.open)
				confirm();
			if (practice.move_list.open)
				move_list();
		}

		void draw(compiled_resource& compiled)
		{
			pointer_layout.owner = {practice.menu_generation, practice.mouse_context, practice.mouse_emulation, practice.mouse_revision};
			pointer_layout.active = true;
			pointer_layout.width = overlay::virtual_width;
			pointer_layout.height = overlay::virtual_height;
			out.rect(layer::b_under, rgb(0x000000, .55f), 0.f, 0.f, overlay::virtual_width, overlay::virtual_height);
			out.image(layer::b_under, &assets::get().scrim, white, 0.f, 628.f, overlay::virtual_width, overlay::virtual_height);
			if (practice.layout == ::pbd::practice::menu_layout::decision)
				decision();
			else
				list();
			help();
			prompts();
			compiled.clear();
			out.flush(compiled);
			dropdown_layer.flush(compiled);
			dialog_layer.flush(compiled);
			chrome.flush(compiled);
			::pbd::practice::mouse::publish(pointer_layout);
		}
	};
} // namespace rsx::overlays::pbd
