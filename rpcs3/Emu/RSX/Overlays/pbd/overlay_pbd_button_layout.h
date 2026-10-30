#pragma once

// In-game Settings > Controls as Tekken 8's Button Layout, the launcher's Settings > Controls page: the controller as the
// header strip, the fifteen action rows, then Reset to default and Back as centred commands under a firmer rule, on the
// launcher's darker content surface (pbd::settings_menu::controls_view, controls_rect). The focused row takes the gold
// wash, rule and label, and nothing in it moves.

#include "overlay_pbd_draw.h"
#include "Emu/PBD/pbd_settings_runtime.h"

#include <array>
#include <cmath>
#include <string_view>

namespace rsx::overlays::pbd::button_layout
{
	namespace menu = ::pbd::settings_menu;

	inline constexpr f32 inset = 14.f, icon = 18.f, words_x = inset + icon + 12.f, glyph_h = 18.f;
	// The panel's outline and the firmer rules after the four attacks and above the commands.
	inline const color4f edge = rgb(0xE9EEED, .1f), firm_rule = rgb(0xE9EEED, .14f);

	// The header: the controller's name between the chevrons that Left/Right step through.
	inline void header(layer& out, const atlas_ref& atlas, const fonts& type, const menu::rect& box, std::string_view name, bool focused)
	{
		out.rect(layer::b_bars, rgb(0x0B0304, .9f), box.x, box.y, box.w, box.h);
		if (focused)
			gold_selection(out, box.x, box.y, box.w, box.h);
		else
			out.rect(layer::b_lines, edge, box.x, box.y + box.h - 1.f, box.w, 1.f);
		const color4f ink = focused ? gold : text_3;
		const f32 chevron_y = box.y + (box.h - 12.f) / 2.f;
		out.sprite(atlas, pbd_glyphs::chevron_l, box.x + inset, chevron_y, 0.f, ink);
		out.sprite(atlas, pbd_glyphs::chevron_r, box.x + box.w - inset - 7.f, chevron_y, 0.f, ink);
		const auto shown = fit_text(type.label, utf8_to_u32string(name), box.w - 2.f * (inset + 7.f + 12.f));
		out.text(type.label, focused ? gold : text_1, std::round(box.x + (box.w - text_width(type.label, shown)) / 2.f),
			baseline(type.label, box.y + box.h / 2.f), shown);
	}

	// The limb icon: four dots as a square (1 top left, 2 top right, 3 bottom left, 4 bottom right), the action's limbs lit
	// sky, straw, mint and rose.
	inline void limb_icon(layer& out, const atlas_ref& atlas, f32 x, f32 centre, std::string_view action)
	{
		static const std::array<color4f, 4> lit{rgb(0x62B8EC), rgb(0xE9D06A), rgb(0x6FD08E), rgb(0xEE86A7)};
		static const color4f dim = rgb(0xE9EEED, .17f);
		const u32 used = menu::limbs(action);
		for (u32 limb = 0; limb < 4; ++limb)
		{
			const f32 left = x + .5f + 9.f * static_cast<f32>(limb % 2);
			const f32 top = centre - icon / 2.f + .5f + 9.f * static_cast<f32>(limb / 2);
			out.sprite(atlas, pbd_glyphs::limb_dot, left, top, 0.f, (used >> limb) & 1u ? lit[limb] : dim);
		}
	}

	// The button cell of a row waiting for its button: ringed in gold, saying so.
	inline void waiting_cell(layer& out, const fonts& type, f32 x, f32 y, f32 w, f32 h, std::string_view words)
	{
		constexpr f32 ring = 1.5f;
		out.rect(layer::b_bars, rgb(0xF4B63F, .1f), x, y, w, h);
		out.rect(layer::b_lines, gold, x, y, w, ring);
		out.rect(layer::b_lines, gold, x, y + h - ring, w, ring);
		out.rect(layer::b_lines, gold, x, y + ring, ring, h - 2.f * ring);
		out.rect(layer::b_lines, gold, x + w - ring, y + ring, ring, h - 2.f * ring);
		const auto shown = fit_text(type.caption, utf8_to_u32string(words), w - 8.f);
		const f32 at = std::round(x + (w - text_width(type.caption, shown)) / 2.f);
		out.text(type.caption, gold, at, baseline(type.caption, y + h / 2.f), shown);
	}

	// The buttons that do an action, centred in its cell: the glyphs of the controller in use (an Xbox pad's face buttons in
	// their colours; `large`, the atlas for their size), a little smaller when several would not fit, or a dim dash for none.
	inline void buttons(layer& out, const atlas_ref& large, const fonts& type, f32 x, f32 w, f32 centre, std::string_view value)
	{
		if (value == menu::no_button)
		{
			const auto dash = utf8_to_u32string(value);
			const f32 at = std::round(x + (w - text_width(type.label, dash)) / 2.f);
			out.text(type.label, rgb(0xE9EEED, .4f), at, baseline(type.label, centre), dash);
			return;
		}
		rich_style style{type.label, text_1, glyph_h};
		style.rings = true;
		f32 run = rich_text(nullptr, large, style, 0.f, centre, value);
		if (run > w - 12.f)
		{
			style.glyph_h = 14.f;
			run = rich_text(nullptr, large, style, 0.f, centre, value);
		}
		rich_text(&out, large, style, std::round(x + (w - run) / 2.f), centre, value);
	}

	// Action row i: shading, selection or foot rule, the limb icon, the words and the button cell behind its divider.
	inline void action(layer& out, const atlas_ref& atlas, const atlas_ref& large, const fonts& type, const menu::view& view, u32 i,
		bool focused)
	{
		const auto& row = view.rows[i];
		const auto box = menu::controls_rect(i);
		const f32 centre = box.y + box.h / 2.f, cell_x = box.x + box.w - menu::layout_button_width;
		if (i % 2 == 0)
			out.rect(layer::b_bars, rgb(0xE9EEED, .035f), box.x, box.y, box.w, box.h);
		if (focused)
			gold_selection(out, box.x, box.y, box.w, box.h);
		else
			out.rect(layer::b_lines, i == 4 ? firm_rule : rgb(0xE9EEED, .06f), box.x, box.y + box.h - 1.f, box.w, 1.f);
		out.rect(layer::b_lines, rgb(0xE9EEED, .08f), cell_x, box.y, 1.f, box.h - (focused ? 2.f : 1.f));
		limb_icon(out, atlas, box.x + inset, centre, ::pbd::controllers::settings::assignments[i]);
		const auto words = fit_text(type.label, utf8_to_u32string(row.label), cell_x - box.x - words_x - 8.f);
		out.text(type.label, focused ? gold : text_1, box.x + words_x, baseline(type.label, centre), words);
		if (focused && view.capturing)
			waiting_cell(out, type, cell_x + 1.f, box.y + 1.f, menu::layout_button_width - 2.f, box.h - 2.f, row.value);
		else
			buttons(out, large, type, cell_x + 1.f, menu::layout_button_width - 1.f, centre, row.value);
	}

	// Reset to default or Back: a centred command.
	inline void command(layer& out, const fonts& type, const menu::rect& box, std::string_view label, bool focused)
	{
		if (focused)
		{
			out.rect(layer::b_focus, rgb(0xF4B63F, .08f), box.x, box.y, box.w, box.h);
			out.image(layer::b_lines, &assets::get().gold_rule, white, box.x, box.y + box.h - 2.f, box.x + box.w, box.y + box.h);
		}
		const auto words = utf8_to_u32string(label);
		out.text(type.label, focused ? gold : text_2, std::round(box.x + (box.w - text_width(type.label, words)) / 2.f),
			baseline(type.label, box.y + box.h / 2.f), words);
	}

	// The whole page's panel; `large` is the atlas for the button column's glyphs, drawn at glyph_h.
	inline void draw(layer& out, const atlas_ref& atlas, const atlas_ref& large, const fonts& type, const menu::view& view, u32 focus)
	{
		const auto head = menu::controls_rect(0), foot = menu::controls_rect(menu::controls_back);
		const f32 left = head.x, width = head.w, bottom = foot.y + foot.h;
		out.rect(layer::b_under, rgb(0x090203, .86f), left, head.y, width, bottom - head.y);
		out.rect(layer::b_lines, edge, left, head.y, width, 1.f);
		out.rect(layer::b_lines, edge, left, bottom - 1.f, width, 1.f);
		out.rect(layer::b_lines, edge, left, head.y + 1.f, 1.f, bottom - head.y - 2.f);
		out.rect(layer::b_lines, edge, left + width - 1.f, head.y + 1.f, 1.f, bottom - head.y - 2.f);
		header(out, atlas, type, head, view.rows[0].value, focus == 0);
		for (u32 i = 1; i < menu::controls_reset && i < view.row_count; ++i)
			action(out, atlas, large, type, view, i, i == focus);
		out.rect(layer::b_lines, firm_rule, left, menu::controls_rect(menu::controls_reset).y - menu::layout_rule, width, 1.f);
		for (u32 i = menu::controls_reset; i < view.row_count; ++i)
			command(out, type, menu::controls_rect(i), view.rows[i].label, i == focus);
	}
} // namespace rsx::overlays::pbd::button_layout
