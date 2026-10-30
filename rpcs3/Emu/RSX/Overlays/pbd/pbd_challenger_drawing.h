#pragma once

// A NEW CHALLENGER over Practice while the launcher searches (and Quick Match's automatic join): the game's own word art
// over the dimmed fight, no band; the challenger as a card (the fighter's caption, the outlined name, the game's plate); the
// two choices as name-plate rows with the accept clock on the default; the small gold Placement Match pill under them when
// joining counts toward the player's placement; the focused choice's line; the prompts with the pad's glyphs.

#include "overlay_pbd_notices.h"

#include <chrono>
#include <cmath>
#include <string>

namespace rsx::overlays::pbd
{
	// An older launcher names the challenger in the subtitle only ("Kai · Sergei Dragunov"): the name, then the fighter.
	inline notices::card_view challenger_card(const ::pbd::game_overlay::snapshot& panel)
	{
		if (panel.card.present)
			return {panel.card.character, panel.card.name, panel.card.shown};
		const std::string_view subtitle = panel.subtitle;
		const auto dot = subtitle.find(" \xC2\xB7 ");
		if (dot == std::string_view::npos)
			return {{}, std::string(subtitle), {}};
		return {::pbd::set_notices::upper(subtitle.substr(dot + 4)), std::string(subtitle.substr(0, dot)), {}};
	}

	inline void draw_challenger(compiled_resource& compiled, const ::pbd::game_overlay::snapshot& panel, const fonts&, atlas_ref atlas)
	{
		namespace at = ::pbd::set_notices::layout;
		layer out;
		out.rect(layer::b_under, rgb(0x050203, .68f), 0.f, 0.f, 1280.f, 720.f);
		if (const auto* word = notices::picture(::pbd::notice_art::picture::new_challenger))
			out.image(layer::b_bars, word, white, at::challenger_art_x, at::challenger_art_y, at::challenger_art_x + at::challenger_art_w,
				at::challenger_art_y + at::challenger_art_h);
		else
			notices::chrome(&out, 640.f, at::challenger_art_y + at::challenger_art_h / 2.f, "A NEW CHALLENGER", 52.f, false, notices::align::centre);
		notices::card(out, challenger_card(panel), 640.f, notices::align::centre, at::challenger_caption_y, at::challenger_name_y,
			at::challenger_plate_top);
		// Under the card: the challenger's status once the player answered, else a connection worth a word (amber or red).
		const f32 under = at::challenger_plate_top + at::rank_h + 14.f;
		if (!panel.rival.empty())
			notices::status_dot_at(out, 1, panel.rival, 640.f, under, notices::align::centre);
		else if (panel.ping.present && panel.ping.bars < 4)
			notices::net_line(&out, panel.ping.bars, std::to_string(panel.ping.ms) + " ms", 640.f, under, notices::align::centre);
		// The choices, the accept clock on its row.
		const auto counted = ::pbd::set_notices::countdown_row(panel);
		std::string seconds;
		if (counted)
		{
			const auto now = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch())
					.count());
			seconds = std::to_string(::pbd::game_overlay::countdown_at(panel.timer, now).seconds);
		}
		const u32 count = std::min<u32>(panel.row_count, ::pbd::game_overlay::max_rows);
		for (u32 i = 0; i < count; ++i)
		{
			const auto& row = panel.rows[i];
			notices::row_view view;
			view.label = row.label;
			view.enabled = row.enabled;
			view.chosen = panel.chosen == i;
			view.focus = i == panel.selected && row.enabled && !view.chosen;
			// The accept clock on its row, until the player chose it (ACCEPTED).
			view.end_is_count = counted == i && !(view.chosen && !row.detail.empty());
			view.end = view.end_is_count ? seconds : row.detail;
			notices::plate_row(out, atlas, view, at::challenger_rows_x, at::challenger_rows_top + at::row_h * static_cast<f32>(i), at::challenger_rows_w,
				notices::align::centre, 0, i + 1 == count);
		}
		f32 below = at::challenger_rows_top + at::row_h * static_cast<f32>(count);
		if (panel.placement_pill)
		{
			notices::draw_pill(out, at::challenger_pill_x, at::challenger_pill_y, at::challenger_pill_w, at::challenger_pill_h);
			below = at::challenger_pill_y + at::challenger_pill_h;
		}
		// The focused choice's line (DECLINE's: two declines pause Ranked), and an automatic join's status.
		const bool open = std::any_of(panel.rows.begin(), panel.rows.begin() + count, [](const auto& row)
			{
				return row.enabled;
			});
		if (panel.selected < count && panel.rows[panel.selected].enabled && !panel.rows[panel.selected].help.empty())
			notices::line(out, 640.f, below + 14.f, panel.rows[panel.selected].help, notices::align::centre, text_2);
		if (!panel.status.empty())
			notices::line(out, 640.f, count ? below + 40.f : at::challenger_rows_top + 16.f, panel.status, notices::align::centre, text_2);
		if (open)
		{
			const auto keys = ::pbd::set_notices::prompts(panel);
			std::vector<prompt_item> items;
			for (const auto& [glyphs, label] : keys)
				items.push_back({glyphs, label});
			notices::prompts(out, atlas, items, false);
		}
		compiled.clear();
		out.flush(compiled);
	}
} // namespace rsx::overlays::pbd
