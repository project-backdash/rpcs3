#pragma once

// Offline play's Versus over the game's character select (pbd::versus): the button prompts, a panel asking for a controller on
// a side without one, each player's Controller Setup on that player's side in the player's colour (Tekken 8's), its Check
// Button Input, the menu after each game and the line saying what the select is doing for the players.

#include "overlay_pbd_draw.h"
#include "overlay_pbd_button_layout.h"
#include "Emu/Cell/pbd_versus.h"

#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace rsx::overlays::pbd::versus
{
	namespace model = ::pbd::versus;
	namespace menu = ::pbd::settings_menu;

	// Player 1's red and player 2's blue.
	inline const std::array<color4f, 2> player_color{rgb(0xE3304F), rgb(0x3A7BD5)};
	// Solid: the select's own words under a panel never show through it.
	inline const color4f panel_fill = rgb(0x090203), panel_edge = rgb(0xE9EEED, .1f);

	inline void outline(layer& out, color4f c, f32 x, f32 y, f32 w, f32 h)
	{
		out.rect(layer::b_lines, c, x, y, w, 1.f);
		out.rect(layer::b_lines, c, x, y + h - 1.f, w, 1.f);
		out.rect(layer::b_lines, c, x, y + 1.f, 1.f, h - 2.f);
		out.rect(layer::b_lines, c, x + w - 1.f, y + 1.f, 1.f, h - 2.f);
	}

	// A panel's player strip: the colour band with the player's name, and `title` at its right.
	inline void player_strip(layer& out, const fonts& type, u32 side, f32 x, f32 y, f32 w, f32 h, std::u32string_view title)
	{
		out.rect(layer::b_bars, player_color[side], x, y, w, h);
		out.rect(layer::b_lines, rgb(0xFFFFFF, .18f), x, y, w, 1.f);
		out.text(type.title, white, x + 14.f, baseline(type.title, y + h / 2.f), side ? U"PLAYER 2" : U"PLAYER 1");
		if (!title.empty())
			out.text(type.caption, rgb(0xFFFFFF, .85f), x + w - 14.f - text_width(type.caption, title), baseline(type.caption, y + h / 2.f), title);
	}

	// `atlas` showing one player's own controller: its family's plates, or its keys. A side without a controller shows the
	// PlayStation plates.
	inline atlas_ref player_atlas(const atlas_ref& atlas, const ::pbd::controllers::display& shown)
	{
		atlas_ref mine = atlas;
		mine.shown = &shown;
		mine.xbox = shown.active && (shown.family == "xbox" || shown.family == "steamdeck");
		return mine;
	}

	// The prompts, right-aligned on a line: measured first, then drawn.
	inline void prompts_right(layer& out, const atlas_ref& atlas, font* f, f32 right, f32 centre, const std::vector<prompt_item>& items)
	{
		const rich_style keys{f, text_1, 16.f};
		f32 width = 0.f;
		for (usz i = 0; i < items.size(); ++i)
			width += (i ? 26.f : 0.f) + rich_text(nullptr, atlas, keys, 0.f, centre, items[i].keys) + 7.f + text_width(f, utf8_to_u32string(items[i].label));
		prompt_bar(out, atlas, f, right - width, centre, right + 1.f, items);
	}

	// The select's button prompts along the foot of the screen, over the scrim the menus use: player 1's at the left and player
	// 2's at the right, each in the buttons of that player's own controller. A side without a controller, or with its
	// Controller Setup open, has none.
	inline void select_prompts(layer& out, const atlas_ref& atlas, const fonts& type, const std::array<::pbd::controllers::display, 2>& players,
		const model::view& v)
	{
		out.image(layer::b_under, &assets::get().scrim, white, 0.f, 640.f, overlay::virtual_width, overlay::virtual_height);
		const std::vector<prompt_item> items{{"{cross}/{square}/{circle}/{r1}", "Costume"}, {"{triangle}", "Controller Setup"}, {"{select}", "Players"}};
		if (!v.empty[0] && !v.setup[0].open)
			prompt_bar(out, player_atlas(atlas, players[0]), type.caption, 56.f, 693.f, 628.f, items);
		if (!v.empty[1] && !v.setup[1].open)
			prompts_right(out, player_atlas(atlas, players[1]), type.caption, 1224.f, 693.f, items);
	}

	inline f32 plate_text(layer& out, font* face, color4f ink, f32 x, f32 centre, std::u32string_view words,
		f32 scale = 1.f, f32 slant = 0.f)
	{
		const f32 base = centre + face->get_size_px() * .2917f * scale;
		out.text(face, rgb(0x000000, .9f), x + 1.f, base + 2.f, words, scale, slant);
		return out.text(face, ink, x, base, words, scale, slant);
	}

	// The arcade join call stays in the empty player's own name-plate column.
	inline void empty_side(layer& out, const atlas_ref& atlas, const fonts& type, u32 side)
	{
		constexpr f32 left = 94.f, right = 1186.f, centre = 218.f, scale = 1.65f;
		const rich_style key{type.label, white, 24.f};
		const f32 press_w = text_width(type.title, U"PRESS") * scale;
		const f32 join_w = text_width(type.title, U"TO JOIN") * scale;
		const f32 key_w = rich_text(nullptr, atlas, key, 0.f, centre, "{start}");
		const f32 width = press_w + 12.f + key_w + 12.f + join_w;
		const f32 x = side ? right - width : left;
		const auto player = side ? U"PLAYER 2" : U"PLAYER 1";
		plate_text(out, type.title, player_color[side], side ? right - text_width(type.title, player) * .7f : x, 191.f, player, .7f);
		plate_text(out, type.title, white, x, centre, U"PRESS", scale, .2f);
		rich_text(&out, atlas, key, x + press_w + 12.f, centre, "{start}");
		plate_text(out, type.title, white, x + press_w + 24.f + key_w, centre, U"TO JOIN", scale, .2f);
		const std::vector<prompt_item> prompt{{"{start}", side ? "Join as Player 2" : "Join as Player 1"}};
		if (side)
			prompts_right(out, atlas, type.body, 1224.f, 693.f, prompt);
		else
			prompt_bar(out, atlas, type.body, 56.f, 693.f, 628.f, prompt);
	}

	// Controller Setup's geometry: a column on the player's own side, as tall as the select allows.
	inline constexpr f32 setup_w = 420.f, setup_top = 56.f, strip_h = 34.f, name_h = 28.f, row_h = 24.f, command_h = 26.f, rule_gap = 6.f;
	inline f32 setup_x(u32 side)
	{
		return side ? 1280.f - 40.f - setup_w : 40.f;
	}
	inline f32 rows_top()
	{
		return setup_top + strip_h + name_h;
	}
	inline f32 commands_top()
	{
		return rows_top() + row_h * model::action_rows + rule_gap;
	}
	inline f32 setup_bottom()
	{
		return commands_top() + command_h * (model::setup_rows - model::action_rows) + 66.f;
	}

	// An action row: the limb icon, its words and its buttons behind the divider, as the Button Layout draws them.
	inline void action_row(layer& out, const atlas_ref& atlas, const atlas_ref& large, const fonts& type, const model::setup_view& v, u32 row,
		f32 x)
	{
		const f32 top = rows_top() + row_h * row, centre = top + row_h / 2.f, cell_x = x + setup_w - menu::layout_button_width;
		const bool focused = v.focus == row;
		if (row % 2 == 0)
			out.rect(layer::b_bars, rgb(0xE9EEED, .035f), x, top, setup_w, row_h);
		if (focused)
			gold_selection(out, x, top, setup_w, row_h);
		else
			out.rect(layer::b_lines, row == 3 ? button_layout::firm_rule : rgb(0xE9EEED, .06f), x, top + row_h - 1.f, setup_w, 1.f);
		out.rect(layer::b_lines, rgb(0xE9EEED, .08f), cell_x, top, 1.f, row_h - (focused ? 2.f : 1.f));
		button_layout::limb_icon(out, atlas, x + button_layout::inset, centre, model::action_of(row));
		const auto words = fit_text(type.label, utf8_to_u32string(menu::action_words(model::action_of(row))), cell_x - x - button_layout::words_x - 8.f);
		out.text(type.label, focused ? gold : text_1, x + button_layout::words_x, baseline(type.label, centre), words);
		button_layout::buttons(out, large, type, cell_x + 1.f, menu::layout_button_width - 1.f, centre, v.buttons[row]);
	}

	// Check Button Input: the stick's eight directions around its centre, and the four attacks as Tekken numbers them (1 2 over
	// 3 4), each lit while held, the attacks by the layout being set up. Under them, the buttons held now.
	inline void check_input(layer& out, const atlas_ref& atlas, const atlas_ref& large, const fonts& type, const model::setup_view& v, u32 side,
		f32 x)
	{
		const f32 top = rows_top() + 40.f;
		out.text(type.label, text_2, x + 24.f, baseline(type.label, top - 16.f), U"Press any button.");
		// The stick: a 3 x 3 of tiles, the held direction lit (opposites cancel), the centre lit with none.
		static constexpr std::array<std::array<u8, 3>, 3> arrows{{{pbd_glyphs::arrow_ul, pbd_glyphs::arrow_u, pbd_glyphs::arrow_ur},
			{pbd_glyphs::arrow_l, pbd_glyphs::arrow_n, pbd_glyphs::arrow_r}, {pbd_glyphs::arrow_dl, pbd_glyphs::arrow_d, pbd_glyphs::arrow_dr}}};
		const bool up = v.live & model::up, down = v.live & model::down, left = v.live & model::left, right = v.live & model::right;
		const usz lit_row = up && !down ? 0 : down && !up ? 2 :
		                                                    1,
				  lit_col = left && !right ? 0 : right && !left ? 2 :
		                                                          1;
		constexpr f32 tile = 44.f, gap = 6.f;
		const f32 stick_x = x + 28.f, stick_y = top + 8.f;
		for (usz r = 0; r < 3; ++r)
			for (usz c = 0; c < 3; ++c)
			{
				const f32 tx = stick_x + (tile + gap) * c, ty = stick_y + (tile + gap) * r;
				const bool lit = r == lit_row && c == lit_col;
				out.rect(layer::b_bars, lit ? player_color[side] : rgb(0xE9EEED, .05f), tx, ty, tile, tile);
				outline(out, lit ? rgb(0xFFFFFF, .35f) : rgb(0xE9EEED, .12f), tx, ty, tile, tile);
				out.sprite(large, arrows[r][c], tx + (tile - 24.f) / 2.f, ty + (tile - 24.f) / 2.f, 24.f, lit ? white : rgb(0xE9EEED, .35f));
			}
		// The attacks, in the limb icon's colours.
		static const std::array<color4f, 4> limb{rgb(0x62B8EC), rgb(0xE9D06A), rgb(0x6FD08E), rgb(0xEE86A7)};
		static constexpr std::array<std::u32string_view, 4> names{U"LP", U"RP", U"LK", U"RK"};
		constexpr f32 key = 64.f, key_gap = 10.f;
		const f32 keys_x = x + setup_w - 28.f - 2.f * key - key_gap, keys_y = stick_y + (3.f * tile + 2.f * gap - 2.f * key - key_gap) / 2.f;
		for (u32 i = 0; i < 4; ++i)
		{
			const f32 kx = keys_x + (key + key_gap) * static_cast<f32>(i % 2), ky = keys_y + (key + key_gap) * static_cast<f32>(i / 2);
			const bool lit = (v.attacks >> i) & 1u;
			out.rect(layer::b_bars, lit ? limb[i] : rgb(0xE9EEED, .05f), kx, ky, key, key);
			outline(out, lit ? rgb(0xFFFFFF, .4f) : faded(limb[i], .45f), kx, ky, key, key);
			const auto label = std::u32string(names[i]);
			out.text(type.title, lit ? rgb(0x0B0304) : faded(limb[i], .8f), std::round(kx + (key - text_width(type.title, label)) / 2.f),
				baseline(type.title, ky + key / 2.f), label);
		}
		// The buttons held now, by their own names.
		std::string held;
		static constexpr std::array<std::pair<u16, std::string_view>, 12> tokens{{{0x80, "{square}"}, {0x10, "{triangle}"}, {0x40, "{cross}"},
			{0x20, "{circle}"}, {0x04, "{l1}"}, {0x08, "{r1}"}, {0x01, "{l2}"}, {0x02, "{r2}"}, {0x0200, "{l3}"}, {0x0400, "{r3}"},
			{0x0800, "{start}"}, {0x0100, "{select}"}}};
		for (const auto& [bit, token] : tokens)
			if (v.live & bit)
				held += std::string(held.empty() ? "" : " ") + std::string(token);
		const f32 held_y = stick_y + 3.f * tile + 2.f * gap + 34.f;
		out.rect(layer::b_lines, rgb(0xE9EEED, .08f), x + 24.f, held_y - 20.f, setup_w - 48.f, 1.f);
		const rich_style style{type.label, text_2, 20.f};
		rich_text(&out, large, style, x + 24.f, held_y, held.empty() ? "No button held" : held);
	}

	inline void setup_panel(layer& out, const atlas_ref& atlas, const atlas_ref& large, const fonts& type, const model::setup_view& v, u32 side)
	{
		const f32 x = setup_x(side), bottom = setup_bottom();
		out.rect(layer::b_under, panel_fill, x, setup_top, setup_w, bottom - setup_top);
		outline(out, panel_edge, x, setup_top, setup_w, bottom - setup_top);
		out.rect(layer::b_lines, player_color[side], x, setup_top, 2.f, bottom - setup_top);
		player_strip(out, type, side, x, setup_top, setup_w, strip_h, v.checking ? U"CHECK BUTTON INPUT" : U"CONTROLLER SETUP");
		const auto name = fit_text(type.label, utf8_to_u32string(v.name), setup_w - 28.f);
		out.text(type.label, aqua, x + 14.f, baseline(type.label, setup_top + strip_h + name_h / 2.f), name);
		std::string help;
		std::vector<prompt_item> prompts;
		if (v.checking)
		{
			check_input(out, atlas, large, type, v, side, x);
			prompts = {{"{select}", "Quit"}};
		}
		else
		{
			for (u32 row = 0; row < model::action_rows; ++row)
				action_row(out, atlas, large, type, v, row, x);
			out.rect(layer::b_lines, button_layout::firm_rule, x, commands_top() - rule_gap / 2.f, setup_w, 1.f);
			for (u32 i = 0; i < model::command_labels.size(); ++i)
			{
				const ::pbd::settings_menu::rect box{x, commands_top() + command_h * i, setup_w, command_h};
				button_layout::command(out, type, box, model::command_labels[i], v.focus == model::action_rows + i);
			}
			if (v.focus < model::action_rows)
			{
				const auto named = menu::action_words(model::action_of(v.focus));
				help = v.refused ? "That button is this attack's only one: give it another first." : "Press the button you want for " + named + ".";
				prompts = {{"{dpad:ud}", "Move"}};
				if (model::removable(v.focus))
					prompts.push_back({"{select}", "Remove Button Mapping"});
				prompts.push_back({"{start}", "Done"});
			}
			else
				prompts = {{"{dpad:ud}", "Move"}, {"{cross}", "Select"}, {"{circle}", "Done"}};
		}
		const f32 foot = bottom - 22.f;
		if (!help.empty())
		{
			const rich_style note{type.caption, aqua, 16.f};
			rich_text(&out, atlas, note, x + 14.f, foot - 26.f, help);
		}
		prompt_bar(out, atlas, type.caption, x + 14.f, foot, x + setup_w - 10.f, prompts);
	}

	// The losing player's menu and the other player's waiting notice.
	inline void next_menu(layer& out, const atlas_ref& atlas, const fonts& type, u32 focus, u32 side)
	{
		struct plates
		{
			raw_rgba row{128, 1}, selection{1, 64};
			plates()
			{
				row.gradient({{0.f, 0x08080A, 1.f}, {.82f, 0x08080A, 1.f}, {1.f, 0x08080A, 0.f}});
				selection.gradient({{0.f, 0xFFF0B8, 1.f}, {.3f, 0xF2C766, 1.f}, {.62f, 0xC99031, 1.f}, {1.f, 0x8F5F17, 1.f}});
			}
		};
		// The renderer caches images by address for the life of the process.
		static const auto* plate = new plates;
		constexpr f32 w = 396.f, pitch = 33.f, y = 190.f, text_scale = .75f;
		const f32 x = side ? 1186.f - w : 94.f;
		const auto player = side ? U"PLAYER 2" : U"PLAYER 1";
		plate_text(out, type.title, player_color[side], side ? x + w - text_width(type.title, player) * .7f : x + 3.f, 125.f, player, .7f);
		plate_text(out, type.title, white, side ? x + w - text_width(type.title, U"NEXT GAME") * 1.8f : x, 158.f, U"NEXT GAME", 1.8f, .2f);
		static constexpr std::array<std::u32string_view, model::next_count> labels{
			U"REMATCH", U"REMATCH \u00b7 RANDOM STAGE", U"CHARACTER SELECT", U"STAGE SELECT", U"NEW PLAYERS", U"EXIT TO LAUNCHER"};
		for (u32 i = 0; i < model::next_count; ++i)
		{
			const f32 top = y + pitch * i;
			out.image(layer::b_bars, &plate->row, white, x, top, x + w, top + pitch, side ? 1.f : 0.f, 0.f, side ? 0.f : 1.f, 1.f);
			out.rect(layer::b_lines, rgb(0xFFFFFF, .16f), x, top, w, 1.f);
			if (i == focus)
			{
				const f32 start = x + (side ? -4.f : 4.f);
				out.image(layer::b_focus, &plate->selection, white, start, top + 3.f, start + w - 8.f, top + pitch - 3.f,
					0.f, 0.f, 1.f, 1.f, .25f);
				out.rect(layer::b_lines, rgb(0xFFF5D5, .7f), start + 6.f, top + 3.f, w - 8.f, 1.f);
				out.rect(layer::b_lines, rgb(0x3A2405, .85f), start, top + pitch - 4.f, w - 8.f, 1.f);
				out.sprite(atlas, find_sprite(side ? "arrow_l" : "arrow_r"), side ? x + w + 2.f : x - 16.f,
					top + (pitch - 12.f) / 2.f, 12.f, player_color[side]);
			}
			const f32 tx = side ? x + w - 19.f - text_width(type.title, labels[i]) * text_scale : x + 19.f;
			out.text(type.title, i == focus ? rgb(0x1B1106) : rgb(0xE9EEED, .86f), tx,
				top + pitch / 2.f + type.title->get_size_px() * .2917f * text_scale, labels[i], text_scale, i == focus ? .2f : 0.f);
		}
		out.rect(layer::b_lines, rgb(0xFFFFFF, .1f), x, y + pitch * model::next_count, w, 1.f);
		out.image(layer::b_under, &assets::get().scrim, white, 0.f, 640.f, overlay::virtual_width, overlay::virtual_height);
		const std::vector<prompt_item> prompts{{"{dpad:ud}", "Move"}, {"{cross}", "Choose"}};
		if (side)
			prompts_right(out, atlas, type.body, 1224.f, 693.f, prompts);
		else
			prompt_bar(out, atlas, type.body, 56.f, 693.f, 628.f, prompts);
		const auto waiting = side ? U"Player 2 is choosing" : U"Player 1 is choosing";
		const f32 waiting_x = side ? 56.f : 1224.f - text_width(type.body, waiting);
		out.rect(layer::b_lines, player_color[side], waiting_x - 13.f, 690.f, 6.f, 6.f);
		plate_text(out, type.body, text_1, waiting_x, 693.f, waiting);
	}

	inline void draw(layer& out, const atlas_ref& atlas, const atlas_ref& large, const fonts& type, const model::view& v)
	{
		const auto players = ::pbd::controllers::read_players();
		if (v.menu)
			next_menu(out, player_atlas(atlas, players[v.menu_player]), type, v.menu_focus, v.menu_player);
		for (u32 side = 0; side < model::sides; ++side)
		{
			if (v.setup[side].open)
				setup_panel(out, player_atlas(atlas, players[side]), player_atlas(large, players[side]), type, v.setup[side], side);
			// Start fills the free players in order, so only the next one to join is asked: player 1, then player 2.
			else if (v.select && v.empty[side] && (side == 0 || !v.empty[0]))
				empty_side(out, player_atlas(atlas, players[side]), type, side);
		}
		if (v.select)
			select_prompts(out, atlas, type, players, v);
		if (v.leave_player < model::sides)
		{
			const auto mine = player_atlas(atlas, players[v.leave_player]);
			const rich_style line{type.caption, text_1, 16.f};
			constexpr std::string_view words = "{start}+{select} Leaving Versus";
			const f32 width = rich_text(nullptr, mine, line, 0.f, 0.f, words);
			band_strip(out, layer::b_under, panel_fill, (1280.f - width) / 2.f - 18.f, (1280.f + width) / 2.f + 18.f, 16.f, 54.f);
			rich_text(&out, mine, line, (1280.f - width) / 2.f, 35.f, words);
		}
		else if (!v.banner.empty())
			toast_band(out, type.caption, utf8_to_u32string(v.banner));
	}
} // namespace rsx::overlays::pbd::versus
