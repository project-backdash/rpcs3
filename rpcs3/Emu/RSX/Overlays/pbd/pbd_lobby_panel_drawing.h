#pragma once

// The launcher's lobby panels over the held game, and the loading cover, in the Ranked notices' look (pbd_set_notices.h,
// overlay_pbd_notices.h): no box. Between games and at Set complete the game's HUD comes back as the score: each player's name
// band and plate where the life bars were, the set's score at the top, the game's stake word under a threatened plate; the
// headline and the rows sit on this player's own side, the prompts at its foot and the other player's status at the opposite
// edge with that player's dot. Set complete shows one headline per state and the placement counter with its lamps and flip.

#include "overlay_pbd_notices.h"
#include "Emu/Cell/pbd_session_wait.h"

#include <chrono>
#include <string>
#include <vector>

namespace rsx::overlays::pbd
{
	struct lobby_panel_drawing
	{
		const ::pbd::game_overlay::snapshot& panel;
		const ::pbd::set_notices::snapshot& set; // inactive when the latch has composed none for this emulation
		atlas_ref atlas;
		u64 flip_elapsed_ms = 0;
		layer out;

		using look = ::pbd::set_notices::panel_look;
		static constexpr f32 rank_plate_w = 156.f, rank_plate_h = 78.f, line_plate_w = 100.f, line_plate_h = 50.f;

		// This player's side (0 left, 1 right; the left without one) and the other's.
		u32 own() const
		{
			return panel.you == 'R' ? 1u : 0u;
		}
		u32 other() const
		{
			return own() ^ 1u;
		}
		bool right() const
		{
			return own() == 1;
		}
		notices::align side_align() const
		{
			return right() ? notices::align::right : notices::align::left;
		}
		// x from this player's edge, mirrored on the right; `w` is the width of what starts there.
		f32 from_edge(f32 x, f32 w = 0.f) const
		{
			return right() ? 1280.f - x - w : x;
		}
		f32 edge(f32 x) const
		{
			return right() ? 1280.f - x : x;
		}

		void cover()
		{
			const auto* flames = notices::picture(::pbd::notice_art::picture::select_flames);
			if (flames)
				out.image(layer::b_under, flames, rgb(0x666666), 0.f, 0.f, 1280.f, 720.f);
			else
				out.rect(layer::b_under, rgb(0x0B0304), 0.f, 0.f, 1280.f, 720.f);
			// Outside a lobby set (Practice's start) the cover carries its caller's headline and line.
			if (!set.active)
			{
				notices::chrome(&out, 640.f, ::pbd::set_notices::layout::cover_headline_y, ::pbd::set_notices::upper(panel.title),
					::pbd::set_notices::layout::wait_em, false, notices::align::centre);
				notices::line(out, 640.f, ::pbd::set_notices::layout::cover_headline_y + 46.f, panel.subtitle, notices::align::centre, text_2);
				return;
			}
			namespace at = ::pbd::set_notices::layout;
			// A wait or the controller notice takes the middle (the wait overlay draws it); the caption names the game then.
			const bool waiting = ::pbd::session_wait::showing().load(std::memory_order_acquire) ||
			                     ::pbd::session_wait::controller_wanted().load(std::memory_order_acquire);
			if (!waiting)
			{
				const auto tag = ::pbd::set_notices::mode_tag(set.mode);
				const auto& f = notices::type();
				if (!tag.empty())
				{
					const auto words = utf8_to_u32string(tag);
					const f32 scale = notices::scale_for(f.row, 14.f);
					notices::tracked(&out, f.row, rgb(0x000000, .8f), 641.f, at::cover_tag_y + 1.f, words, scale, .24f, notices::align::centre);
					notices::tracked(&out, f.row, gold, 640.f, at::cover_tag_y, words, scale, .24f, notices::align::centre);
				}
				notices::chrome(&out, 640.f, at::cover_headline_y, ::pbd::set_notices::game_headline(set.game_number), at::cover_headline_em, false,
					notices::align::centre);
			}
			notices::caption(out, 640.f, at::cover_caption_y, ::pbd::set_notices::cover_caption(set, waiting), notices::align::centre);
			cards(at::card_caption_y, at::card_name_y, at::card_plate_top);
		}

		// Both players' cards where the VS screen shows them.
		void cards(f32 caption_y, f32 name_y, f32 plate_top)
		{
			namespace at = ::pbd::set_notices::layout;
			for (u32 side = 0; side < 2; ++side)
			{
				const auto& player = set.sides[side];
				notices::card(out, {player.character, player.name, player.shown}, side ? at::right_edge : at::margin,
					side ? notices::align::right : notices::align::left, caption_y, name_y, plate_top);
			}
		}

		// The HUD between games and at Set complete: the name bands, the plates (yours steps aside when Set complete shows it),
		// the stake words between games, the score and its caption.
		void hud(look shown)
		{
			namespace at = ::pbd::set_notices::layout;
			const auto& score = panel.score;
			const std::array<std::string_view, 2> names{score.left, score.right};
			const std::array<u32, 2> wins{score.wins(false), score.wins(true)};
			for (u32 side = 0; side < 2; ++side)
			{
				notices::name_band(out, side, names[side]);
				if (!set.active || (side == own() && ::pbd::set_notices::own_plate_aside(shown)) ||
					(side == other() && ::pbd::set_notices::opponent_result_shown(panel)))
					continue;
				const auto& plate_shown = set.sides[side].shown;
				const f32 w = plate_shown.shown == notices::plate::kind::placing ? at::placing_w : at::rank_w;
				const f32 x = side ? at::life_plate_right - w : at::life_plate_left;
				notices::draw_plate(out, plate_shown, x, at::hud_plate_top, w, at::rank_h);
				if (shown == look::between || shown == look::question)
					notices::draw_stake(out, ::pbd::set_notices::stake_for(set, side, wins), side ? 1280.f - at::hud_stake_edge : at::hud_stake_edge,
						at::hud_stake_top, side == 1);
			}
			notices::score(out, 640.f, at::hud_score_y, wins[0], wins[1], at::hud_score_em);
			notices::caption(out, 640.f, at::hud_caption_y, ::pbd::set_notices::score_caption(panel), notices::align::centre);
		}

		void headline(f32 centre, std::string_view words, bool gold_words = false, f32 em = ::pbd::set_notices::layout::headline_em)
		{
			notices::chrome(&out, edge(::pbd::set_notices::layout::menu_x), centre, words, em, gold_words, side_align());
		}

		// The rows on this player's side from `top`; returns the bottom of the last.
		f32 rows(f32 top)
		{
			namespace at = ::pbd::set_notices::layout;
			const auto counted = ::pbd::set_notices::countdown_row(panel);
			std::string seconds;
			if (counted)
			{
				const auto now_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now().time_since_epoch())
						.count());
				seconds = std::to_string(::pbd::game_overlay::countdown_at(panel.timer, now_ms).seconds);
			}
			const f32 x = from_edge(at::menu_x, at::row_w);
			const u32 count = std::min<u32>(panel.row_count, ::pbd::game_overlay::max_rows);
			for (u32 i = 0; i < count; ++i)
			{
				const auto& row = panel.rows[i];
				notices::row_view view;
				view.label = row.label;
				view.enabled = row.enabled;
				view.chosen = panel.chosen == i;
				view.focus = i == panel.selected && row.enabled && !view.chosen;
				// The countdown on its row, unless that row was chosen and says so (YOUR CHOICE).
				if (counted == i && !(view.chosen && !row.detail.empty()))
				{
					view.end = seconds;
					view.end_is_count = true;
				}
				else
					view.end = row.detail;
				notices::plate_row(out, atlas, view, x, top + at::row_h * static_cast<f32>(i), at::row_w, side_align(), own(), i + 1 == count);
			}
			return top + at::row_h * static_cast<f32>(count);
		}

		// Set complete's big placement plate with its lamps and what is left, the counter flipping once the set is rated.
		void placement()
		{
			namespace at = ::pbd::set_notices::layout;
			const auto& count = panel.placement;
			const auto flip = ::pbd::set_notices::flip_at(count, flip_elapsed_ms);
			// The count it flips to is set before the flip, so the flip itself never waits for its lettering.
			notices::placing_letters(count.after, count.total);
			const notices::plate big{notices::plate::kind::placing, 0, flip.count, count.total};
			notices::draw_plate(out, big, from_edge(at::placing_plate_x, at::big_placing_w), at::placing_plate_top, at::big_placing_w, at::big_placing_h);
			notices::draw_lamps(out, count.total, flip.lamps, flip.flash, from_edge(at::placing_lamps_x, at::big_lamps_w), at::placing_lamps_top,
				at::big_lamps_w, at::big_lamps_h);
			if (flip.to_go > 0.f)
				notices::caption(out, edge(84.f), at::placing_caption_y, ::pbd::set_notices::sets_to_go(count.after, count.total), side_align(),
					rgb(0xAEBDBD, flip.to_go));
			else if (!panel.subtitle.empty())
				notices::line(out, edge(84.f), at::placing_caption_y, panel.subtitle, side_align(), text_2);
		}

		// RANK UP or RANK ATTAINED and the rank's plate on one line, the rating as its caption.
		void rank_headline()
		{
			namespace at = ::pbd::set_notices::layout;
			const auto words = ::pbd::set_notices::headline(panel);
			const f32 text_w = notices::chrome(nullptr, 0.f, 0.f, words, at::rank_em, true);
			const notices::plate shown{notices::plate::kind::rank, panel.rank.rank, 0, 0};
			if (!right())
			{
				notices::chrome(&out, at::menu_x, at::rank_line_y, words, at::rank_em, true);
				notices::draw_plate(out, shown, at::menu_x + text_w + 2.f, at::rank_line_y - rank_plate_h / 2.f, rank_plate_w, rank_plate_h);
			}
			else
			{
				const f32 plate_x = 1280.f - at::menu_x + 4.f - rank_plate_w;
				notices::draw_plate(out, shown, plate_x, at::rank_line_y - rank_plate_h / 2.f, rank_plate_w, rank_plate_h);
				notices::chrome(&out, plate_x - 4.f, at::rank_line_y, words, at::rank_em, true, notices::align::right);
			}
			rating(edge(at::menu_x + 6.f), at::rank_caption_y);
		}

		// The other player's settled plate and MMR or placement count, mirrored across the same result canvas.
		void opponent_result()
		{
			if (!::pbd::set_notices::opponent_result_shown(panel))
				return;
			namespace at = ::pbd::set_notices::layout;
			const auto& result = panel.opponent;
			const bool on_right = other() == 1;
			const auto align = on_right ? notices::align::right : notices::align::left;
			const auto x = [on_right](f32 inset, f32 width = 0.f)
			{
				return on_right ? 1280.f - inset - width : inset;
			};
			if (result.shown.shown == notices::plate::kind::placing)
			{
				notices::draw_plate(out, result.shown, x(at::placing_plate_x, at::big_placing_w), at::placing_plate_top, at::big_placing_w, at::big_placing_h);
				notices::draw_lamps(out, result.shown.total, result.shown.done, false, x(at::placing_lamps_x, at::big_lamps_w), at::placing_lamps_top,
					at::big_lamps_w, at::big_lamps_h);
				notices::caption(out, x(84.f), at::placing_caption_y, ::pbd::set_notices::opponent_caption(result), align);
				return;
			}
			notices::draw_plate(out, result.shown, x(at::menu_x, at::big_rank_w), at::rank_line_y - at::big_rank_h / 2.f, at::big_rank_w, at::big_rank_h);
			notices::caption(out, x(at::menu_x + 6.f), at::rank_caption_y, ::pbd::set_notices::opponent_caption(result), align);
		}

		// "MMR 1301" and its change after it (green up, red down), from this player's edge.
		void rating(f32 x, f32 centre)
		{
			const auto mmr = ::pbd::set_notices::rating_caption(panel.rank);
			const auto change = ::pbd::set_notices::rating_change(panel.rank);
			const color4f change_ink = panel.rank.change < 0 ? rgb(0xFF9470) : rgb(0x7EE2A2);
			if (!right())
			{
				const f32 w = notices::caption(out, x, centre, mmr);
				if (!change.empty())
					notices::caption(out, x + w + 10.f, centre, change, notices::align::left, change_ink);
				return;
			}
			const f32 w = change.empty() ? 0.f : notices::caption(out, x, centre, change, notices::align::right, change_ink);
			notices::caption(out, x - w - (w > 0.f ? 10.f : 0.f), centre, mmr, notices::align::right);
		}

		// A rated set that left the rank (the plate and the rating) or took it down ("Rank down", the new plate, then the rating).
		f32 rank_line()
		{
			namespace at = ::pbd::set_notices::layout;
			const notices::plate shown{notices::plate::kind::rank, panel.rank.rank, 0, 0};
			const bool down = panel.rank.kind == ::pbd::game_overlay::rank_change::down;
			const f32 centre = at::result_line_y;
			f32 x = edge(at::menu_x + 4.f);
			if (down)
			{
				// In the game's white italic, as its own "Promoted to" is set.
				const f32 w = notices::line(out, x, centre, "Rank down", side_align(), text_1, .2f);
				x = right() ? x - w - 8.f : x + w + 8.f;
			}
			notices::draw_plate(out, shown, right() ? x - line_plate_w : x, centre - line_plate_h / 2.f, line_plate_w, line_plate_h);
			if (down)
			{
				rating(edge(at::menu_x + 6.f), centre + 34.f);
				return at::result_rows_top + 34.f;
			}
			rating(right() ? x - line_plate_w - 8.f : x + line_plate_w + 8.f, centre);
			return at::result_rows_top;
		}

		// The foot: this player's prompts, and at the opposite edge the other player's status and the set's connection.
		void foot()
		{
			namespace at = ::pbd::set_notices::layout;
			std::vector<prompt_item> items;
			const auto keys = ::pbd::set_notices::prompts(panel);
			for (const auto& [glyphs, label] : keys)
				items.push_back({glyphs, label});
			notices::prompts(out, atlas, items, right());
			const auto rival = ::pbd::set_notices::rival_line(panel);
			f32 centre = at::prompt_y;
			if (!rival.empty())
			{
				notices::status_dot(out, other(), rival, !right(), centre);
				centre -= 26.f;
			}
			if (panel.link.present)
				notices::net_line(&out, panel.link.bars, panel.link.words, right() ? at::margin : at::right_edge, centre,
					right() ? notices::align::left : notices::align::right);
		}

		void draw(compiled_resource& compiled)
		{
			namespace at = ::pbd::set_notices::layout;
			const auto shown = ::pbd::set_notices::look_of(panel);
			if (shown == look::cover)
			{
				cover();
				compiled.clear();
				out.flush(compiled);
				return;
			}
			const bool scored = panel.score.present;
			notices::held_frame(out, ::pbd::set_notices::held_shade(shown, scored), static_cast<int>(own()), shown == look::rank_headline);
			if (scored)
				hud(shown);
			f32 rows_top = at::between_rows_top;
			switch (shown)
			{
			case look::question:
				headline(at::between_headline_y, ::pbd::set_notices::upper(panel.question.message));
				notices::line(out, edge(at::menu_x + 4.f), at::confirm_line_y, panel.question.detail, side_align());
				rows_top = at::confirm_rows_top;
				break;
			case look::placement:
				headline(at::result_headline_y, ::pbd::set_notices::headline(panel));
				placement();
				rows_top = at::placing_rows_top;
				break;
			case look::rank_headline:
				rank_headline();
				rows_top = at::rank_rows_top;
				break;
			case look::rank_line:
				headline(at::result_headline_y, ::pbd::set_notices::headline(panel));
				rows_top = rank_line();
				break;
			case look::result:
				headline(at::result_headline_y, ::pbd::set_notices::headline(panel));
				if (!panel.subtitle.empty())
					notices::line(out, edge(at::menu_x + 6.f), at::result_line_y, panel.subtitle, side_align());
				rows_top = at::result_rows_top;
				break;
			default:
				headline(at::between_headline_y, ::pbd::set_notices::headline(panel));
				if (!panel.subtitle.empty())
				{
					notices::line(out, edge(at::menu_x + 4.f), at::confirm_line_y, panel.subtitle, side_align());
					rows_top = at::confirm_rows_top;
				}
				break;
			}
			opponent_result();
			const f32 bottom = rows(rows_top);
			// Under the rows: the focused row's help, then the panel's status ("Searching").
			f32 next = bottom + (panel.row_count ? 22.f : 0.f);
			if (notices::type().line && panel.selected < panel.row_count && panel.rows[panel.selected].enabled && !panel.rows[panel.selected].help.empty())
			{
				const rich_style help{notices::type().line, text_2, 16.f};
				const f32 width = rich_text(nullptr, atlas, help, 0.f, next, panel.rows[panel.selected].help);
				rich_text(&out, atlas, help, right() ? edge(at::menu_x + 4.f) - width : edge(at::menu_x + 4.f), next, panel.rows[panel.selected].help);
				next += 28.f;
			}
			if (!panel.status.empty())
				notices::line(out, edge(at::menu_x + 4.f), next, panel.status, side_align(), text_2);
			foot();
			compiled.clear();
			out.flush(compiled);
		}
	};
} // namespace rsx::overlays::pbd
