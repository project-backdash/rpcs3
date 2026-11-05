#pragma once

#include "overlay_pbd_versus.h"
#include "Emu/PBD/pbd_costume_panel.h"

namespace rsx::overlays::pbd::costume
{
	struct art
	{
		std::array<raw_rgba, 2> panels{raw_rgba{1, 256}, raw_rgba{1, 256}};
		raw_rgba orange{1, 32};
		art()
		{
			panels[0].gradient({{0.f, 0xA53842, .98f}, {.04f, 0xAF161E, .98f}, {.22f, 0x961018, .98f}, {.55f, 0x50090C, .98f}, {.92f, 0x000000, .98f}});
			panels[1].gradient({{0.f, 0x3182B5, .98f}, {.04f, 0x1571AF, .98f}, {.22f, 0x116096, .98f}, {.55f, 0x093250, .98f}, {.92f, 0x000000, .98f}});
			orange.gradient({{0.f, 0xFFD76A, 1.f}, {.3f, 0xFF9E08, 1.f}, {.5f, 0xC44A00, 1.f}, {.6f, 0xE65D00, 1.f}, {1.f, 0x7B2100, 1.f}});
		}
		static art& get()
		{
			static art* value = new art;
			return *value;
		}
	};
	inline const image_info* thumbnail(const ::pbd::costume_select::card& card, std::size_t index)
	{
		static std::array<std::vector<u8>, 15> bytes;
		static std::array<std::unique_ptr<image_info>, 15> images;
		if (index >= bytes.size() || card.picture.empty())
			return nullptr;
		if (bytes[index] != card.picture)
		{
			bytes[index] = card.picture;
			images[index] = std::make_unique<image_info>(bytes[index]);
		}
		return images[index].get();
	}
	inline void draw(layer& out, const atlas_ref& atlas, const fonts& type, const ::pbd::costume_panel::snapshot& view)
	{
		if (view.visible && view.editor)
		{
			out.rect(layer::b_under, rgb(0x000000, .94f), 325.f, 190.f, 630.f, 325.f);
			versus::outline(out, rgb(0xFF8E00), 325.f, 190.f, 630.f, 325.f);
			out.text(type.title, white, 350.f, 230.f, view.overwrite ? U"REPLACE THIS CUSTOM?" : U"SAVE CUSTOM COSTUME", .9f, .18f);
			const std::array<std::string, 3> rows{"Save to Custom " + std::to_string(view.save_slot), "Keep editing", "Leave without saving"};
			for (u32 row = 0; row < rows.size(); ++row)
			{
				if (row == view.save_row)
					out.rect(layer::b_bars, rgb(0xB84F00), 345.f, 258.f + row * 57.f, 590.f, 48.f);
				out.text(type.title, row == 0 && !view.save_ready ? text_2 : white, 365.f, 292.f + row * 57.f, utf8_to_u32string(rows[row]), .8f, .18f);
			}
			const auto footer = view.overwrite  ? (view.replace ? U"REPLACE     /     Keep this slot" : U"Replace     /     KEEP THIS SLOT") :
			                    view.save_ready ? U"Left / Right: slot     Cross: choose     Circle: edit" :
			                                      U"Preparing picture...";
			out.text(type.caption, white, 350.f, 486.f, footer, .7f, .18f);
			return;
		}
		if (!view.visible || view.side > 1 || view.cards.empty())
			return;
		const auto focus = std::min<std::size_t>(view.focus, view.cards.size() - 1);
		const auto& chosen = view.cards[focus];
		if (view.open)
		{
			static font* caption_font = fontmgr::get("Barlow-700.ttf", 10);
			const f32 x = view.side ? 650.f : 26.f, y = 16.f, w = 604.f;
			out.rect(layer::b_under, rgb(0x000000, .9f), x, y, w * .74f, 30.f);
			out.text(type.title, white, x + 22.f, baseline(type.title, y + 15.f), U"PRESET", .8f, .18f);
			out.text(type.title, versus::player_color[view.side], x + 120.f, baseline(type.title, y + 15.f), view.side ? U"PLAYER 2" : U"PLAYER 1", .75f, .18f);
			out.image(layer::b_under, &art::get().panels[view.side], white, x, y + 30.f, x + w, 333.f);
			versus::outline(out, rgb(0xC9C7C4), x, y + 30.f, w, 287.f);
			out.rect(layer::b_lines, white, x, y + 30.f, w, 3.f);
			out.text(type.timer, rgb(0x111111), x + 18.f, 81.f, utf8_to_u32string(::pbd::costume_panel::fighter_name(chosen.look.row)), 1.f, .18f);
			out.text(type.timer, rgb(0xF4F6F1), x + 17.f, 79.f, utf8_to_u32string(::pbd::costume_panel::fighter_name(chosen.look.row)), 1.f, .18f);
			const std::size_t first = focus > 2 ? std::min(focus - 2, view.cards.size() > 5 ? view.cards.size() - 5 : 0) : 0;
			f32 card_x = x + 18.f;
			for (std::size_t i = first; i < std::min(first + 5, view.cards.size()); ++i)
			{
				const bool selected = i == focus;
				const f32 card_y = selected ? 98.f : 107.f, card_w = selected ? 106.f : 96.f, card_h = selected ? 178.f : 160.f;
				if (i > first && view.cards[i].slot && !view.cards[i - 1].slot)
				{
					out.rect(layer::b_lines, rgb(0xC9C7C4, .6f), card_x + 2.f, 110.f, 1.f, 150.f);
					card_x += 12.f;
				}
				out.rect(layer::b_bars, rgb(view.side ? 0x102030 : 0x2A1416), card_x, card_y, card_w, card_h);
				versus::outline(out, selected ? rgb(0xFF8E00) : rgb(0xC9C7C4, .55f), card_x, card_y, card_w, card_h);
				const auto name = utf8_to_u32string(::pbd::costume_panel::label(view.cards[i]));
				const auto number = utf8_to_u32string(std::to_string(view.cards[i].slot ? *view.cards[i].slot + 1 : i + 1));
				if (const auto* photo = thumbnail(view.cards[i], i))
					out.image(layer::b_bars, photo, white, card_x + 2.f, card_y + 2.f, card_x + card_w - 2.f, card_y + card_h - 26.f);
				else
					out.text(type.timer, rgb(0xFFFFFF, .22f), card_x + (card_w - text_width(type.timer, number) * 1.5f) / 2.f, card_y + 88.f, number, 1.5f, .18f);
				if (selected)
					out.image(layer::b_bars, &art::get().orange, white, card_x, card_y + card_h - 26.f, card_x + card_w, card_y + card_h);
				else
					out.rect(layer::b_bars, rgb(0x000000, .82f), card_x, card_y + card_h - 22.f, card_w, 22.f);
				out.text(caption_font, white, card_x + (card_w - text_width(caption_font, name)) / 2.f, baseline(caption_font, card_y + card_h - 12.f), name);
				card_x += card_w + 8.f;
			}
			out.rect(layer::b_lines, rgb(0xFFFFFF, .22f), x + 16.f, 290.f, w - 32.f, 1.f);
			out.text(type.title, white, x + 16.f, baseline(type.title, 309.f), utf8_to_u32string(::pbd::costume_panel::label(chosen)), .8f, .18f);
			const auto position = utf8_to_u32string(std::to_string(focus + 1) + " / " + std::to_string(view.cards.size()));
			out.text(caption_font, text_2, x + w - 16.f - text_width(caption_font, position), baseline(caption_font, 309.f), position);
		}
		const std::vector<prompt_item> prompts = view.open ?
		                                             std::vector<prompt_item>{{"{dpad:lr}", "Costume"}, {"{l1}/{r1}", "Default / Custom"}, {"{cross}", "Confirm"}, {"{circle}", "Back"}} :
		                                             std::vector<prompt_item>{{"{cross}", "Default 01"}, {"{square}", "Costumes"}, {"{triangle}", "Controller Setup"}};
		out.image(layer::b_under, &assets::get().scrim, white, 0.f, 670.f, 1280.f, 720.f);
		versus::prompts_right(out, atlas, type.caption, 1236.f, 693.f, prompts);
	}
} // namespace rsx::overlays::pbd::costume
