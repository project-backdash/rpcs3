#pragma once

#include "pbd_overlay_canvas.h"

namespace rsx::overlays::pbd
{
	// Text with {glyph} tokens. "{a}+{b}" is a chord and "{a}/{b}" a pair: the "+" and "/" between tokens are sprites.
	// Glyphs are vertically centred on centre_y. With no layer the run is only measured. "{held:a}" draws glyph a in the
	// held tint: a move list's held direction (Tekken's capital "D/F"). `rings`: Xbox face buttons in their colours.
	struct rich_style
	{
		font* f = nullptr;
		color4f color = text_1;
		f32 glyph_h = 16.f;
		color4f glyph_tint = white, held_tint = gold;
		bool rings = false;
	};

	inline f32 rich_text(layer* out, const atlas_ref& atlas, const rich_style& style, f32 x, f32 centre_y, std::string_view s)
	{
		const f32 start = x;
		const f32 base = baseline(style.f, centre_y);
		static const int plus = find_sprite("plus"), slash = find_sprite("slash");
		const auto glyph = [&](int id, bool held = false)
		{
			if (id < 0)
				return;
			const auto size = pbd_glyphs::sizes[id];
			const f32 h = id == plus || id == slash ? size.h : style.glyph_h;
			const f32 w = size.w * h / size.h;
			if (out)
				out->sprite(atlas, id, x, centre_y - h / 2.f, h, held ? style.held_tint : style.glyph_tint);
			x += w;
		};
		usz i = 0;
		std::string plain;
		const auto flush_plain = [&]()
		{
			if (plain.empty())
				return;
			const auto wide = utf8_to_u32string(plain);
			x += out ? out->text(style.f, style.color, x, base, wide) : text_width(style.f, wide);
			plain.clear();
		};
		while (i < s.size())
		{
			if (s[i] == '{')
			{
				const usz close = s.find('}', i);
				if (close != std::string_view::npos)
				{
					flush_plain();
					auto token = s.substr(i + 1, close - i - 1);
					const bool held = token.starts_with("held:");
					if (held)
						token.remove_prefix(5);
					const auto device = atlas.shown ? *atlas.shown : ::pbd::controllers::read_display();
					const auto label = ::pbd::controllers::glyph_label(token, device);
					if (!label.empty())
					{
						if (device.family == "xbox" && (token == "start" || token == "select"))
						{
							glyph(glyph_sprite(token, atlas.xbox, style.rings), held);
							x += 4.f;
						}
						const auto wide = utf8_to_u32string(label);
						const f32 width = text_width(style.f, wide) + 8.f;
						if (out)
						{
							out->rect(layer::b_bars, rgb(0x30343b), x, centre_y - style.glyph_h / 2.f - 2.f, width, style.glyph_h + 4.f);
							out->text(style.f, held ? style.held_tint : style.glyph_tint, x + 4.f, base, wide);
						}
						x += width;
					}
					else
						glyph(glyph_sprite(token, atlas.xbox, style.rings), held);
					i = close + 1;
					// A chord or pair continues straight into the next token.
					if (i + 1 < s.size() && (s[i] == '+' || s[i] == '/') && s[i + 1] == '{')
					{
						const bool chord = s[i] == '+';
						x += chord ? 2.f : 3.f;
						glyph(chord ? plus : slash);
						x += chord ? 2.f : 3.f;
						i++;
					}
					continue;
				}
			}
			plain.push_back(s[i++]);
		}
		flush_plain();
		return x - start;
	}

	// A top-centre band: a content-sized opaque middle with fixed 72-unit ramps at both ends, drawn from the white ramp
	// image as three quads tinted with the layer's colour.
	inline void band_strip(layer& out, layer::bucket b, color4f tint, f32 left, f32 right, f32 y0, f32 y1)
	{
		auto& ramp = assets::get().ramp;
		constexpr f32 r = 72.f;
		const f32 mid0 = left + r, mid1 = std::max(mid0, right - r);
		out.image(b, &ramp, tint, left, y0, mid0, y1, 0.f, 0.f, 1.f, 1.f);
		out.image(b, &ramp, tint, mid0, y0, mid1, y1, 1.f, 0.f, 1.f, 1.f);
		out.image(b, &ramp, tint, mid1, y0, mid1 + r, y1, 1.f, 0.f, 0.f, 1.f);
	}

	// The Button Layout's selection, as the launcher marks a ruled row: the gold wash fading out to the right and the 2-unit gold
	// rule along the row's foot. Nothing in the row moves.
	inline void gold_selection(layer& out, f32 x, f32 y, f32 w, f32 h)
	{
		auto& store = assets::get();
		out.image(layer::b_focus, &store.gold_wash, white, x, y, x + w, y + h);
		out.image(layer::b_lines, &store.gold_rule, white, x, y + h - 2.f, x + w, y + h);
	}

	// Crimson focus: glow ramps 10 units above and below, the bar image and the brighter 4-unit left stripe.
	inline void focus_bar(layer& out, f32 x, f32 y, f32 w, f32 h)
	{
		auto& store = assets::get();
		const color4f glow_tint = white;
		out.image(layer::b_focus, &store.glow, glow_tint, x, y - 10.f, x + w, y, 0.f, 0.f, 1.f, 1.f);
		out.image(layer::b_focus, &store.glow, glow_tint, x, y + h, x + w, y + h + 10.f, 0.f, 1.f, 1.f, 0.f);
		out.image(layer::b_focus, &store.focus, white, x, y, x + w, y + h);
		out.rect(layer::b_lines, stripe, x, y, 4.f, h);
	}

	// Control bar: icon groups and labels from the left edge, dropping groups that would pass the right limit. placed(i, x, w)
	// receives the span of every group drawn, so pointer targets come from the same measurement as the drawing.
	struct prompt_item
	{
		std::string_view keys;
		std::string_view label;
	};

	template <typename Items, typename Placed>
	f32 prompt_bar(layer& out, const atlas_ref& atlas, font* f, f32 x, f32 centre_y, f32 right_limit, const Items& items, Placed&& placed)
	{
		const rich_style keys{f, text_1, 16.f};
		f32 cursor = x;
		bool first = true;
		usz index = 0;
		for (const auto& item : items)
		{
			const auto label = utf8_to_u32string(item.label);
			const f32 width = rich_text(nullptr, atlas, keys, 0.f, centre_y, item.keys) + 7.f + text_width(f, label);
			const f32 at = first ? cursor : cursor + 26.f;
			if (at + width > right_limit)
				break;
			const f32 k = rich_text(&out, atlas, keys, at, centre_y, item.keys);
			out.text(f, text_1, at + k + 7.f, baseline(f, centre_y), label);
			placed(index++, at, width);
			cursor = at + width;
			first = false;
		}
		return cursor;
	}

	template <typename Items>
	f32 prompt_bar(layer& out, const atlas_ref& atlas, font* f, f32 x, f32 centre_y, f32 right_limit, const Items& items)
	{
		return prompt_bar(out, atlas, f, x, centre_y, right_limit, items, [](usz, f32, f32) {});
	}

	// Label colour of a row or dialog option: white on the focus bar, dimmed when unavailable.
	inline color4f label_color(bool enabled, bool focus)
	{
		return enabled ? (focus ? white : text_2) : (focus ? rgb(0xFFFFFF, .66f) : rgb(0x8A9C9C, .7f));
	}

	// Dialog frame: panel fill, a 1-unit outline on all sides and the dark crimson 2-unit bottom edge.
	inline void dialog_frame(layer& out, f32 x, f32 y, f32 w, f32 h)
	{
		out.image(layer::b_under, &assets::get().panel, white, x, y, x + w, y + h);
		out.rect(layer::b_lines, top_edge, x, y, w, 1.f);
		out.rect(layer::b_lines, top_edge, x, y + 1.f, 1.f, h - 3.f);
		out.rect(layer::b_lines, top_edge, x + w - 1.f, y + 1.f, 1.f, h - 3.f);
		out.rect(layer::b_lines, edge_dark, x, y + h - 2.f, w, 2.f);
	}

	// A 28-unit dialog option: row bar, the focus bar when focused, label centred on the bar.
	inline void option_bar(layer& out, font* f, f32 x, f32 top, f32 w, std::u32string_view text, bool focus, bool enabled = true)
	{
		out.rect(layer::b_bars, row_bar, x, top, w, 28.f);
		if (focus)
			focus_bar(out, x, top, w, 28.f);
		out.text(f, label_color(enabled, focus), x + w / 2.f - text_width(f, text) / 2.f, baseline(f, top + 14.f), text);
	}

	// A solid 20-unit tag with 9 units of padding (the Practice control tag, the replay take-control badge). x is the
	// left edge, or the right edge when right_aligned. Returns the drawn width.
	inline f32 tag(layer& out, font* f, color4f fill, color4f ink, f32 x, bool right_aligned, f32 top, std::u32string_view text)
	{
		const f32 w = text_width(f, text) + 18.f;
		const f32 left = right_aligned ? x - w : x;
		out.rect(layer::b_bars, fill, left, top, w, 20.f);
		out.text(f, ink, left + 9.f, baseline(f, top + 10.f), text);
		return w;
	}

	// A toast band, top centre (y 156..190): the text over a content-sized band with a hairline along its top and a
	// crimson edge along its bottom.
	inline void toast_band(layer& out, font* f, std::u32string_view text)
	{
		const f32 width = text_width(f, text);
		const f32 left = std::round(640.f - width / 2.f) - 72.f, right = left + width + 144.f;
		band_strip(out, layer::b_under, band_fill, left, right, 156.f, 190.f);
		band_strip(out, layer::b_lines, rgb(0xE9EEED, .16f), left, right, 156.f, 157.f);
		band_strip(out, layer::b_lines, rgb(0xE3304F, .75f), left, right, 188.f, 190.f);
		out.text(f, text_1, left + 72.f, baseline(f, 173.f), text);
	}

	// The longest prefix of text that fits max_w, ending in an ellipsis when cut.
	inline std::u32string fit_text(font* f, std::u32string_view text, f32 max_w)
	{
		if (text_width(f, text) <= max_w)
			return std::u32string(text);
		constexpr char32_t ellipsis = 0x2026;
		const f32 dots = text_width(f, std::u32string_view(&ellipsis, 1));
		f32 x = 0.f, y = 0.f;
		usz n = 0;
		for (; n < text.size(); n++)
		{
			f32 nx = x, ny = y;
			f->get_char(text[n], nx, ny);
			if (nx + dots > max_w)
				break;
			x = nx;
			y = ny;
		}
		return std::u32string(text.substr(0, n)) + ellipsis;
	}

	// Greedy wrap at spaces (or mid-word when a word alone is too wide). The first line may be narrower than the rest; the
	// last allowed line ends in an ellipsis when text remains. Returns the number of lines written.
	inline usz wrap_text(font* f, std::u32string_view text, f32 first_w, f32 rest_w, std::u32string* lines, usz max_lines)
	{
		usz count = 0, start = 0;
		while (start < text.size() && count < max_lines)
		{
			const f32 limit = count ? rest_w : first_w;
			if (count + 1 == max_lines)
			{
				lines[count++] = fit_text(f, text.substr(start), limit);
				break;
			}
			f32 x = 0.f, y = 0.f;
			usz end = start, space = std::u32string_view::npos;
			while (end < text.size())
			{
				f32 nx = x, ny = y;
				f->get_char(text[end], nx, ny);
				if (nx > limit && end > start)
					break;
				if (text[end] == U' ')
					space = end;
				x = nx;
				y = ny;
				end++;
			}
			if (end < text.size() && space != std::u32string_view::npos && space > start)
				end = space;
			lines[count++] = std::u32string(text.substr(start, end - start));
			start = end;
			while (start < text.size() && text[start] == U' ')
				start++;
		}
		return count;
	}

	// The Practice panel frame: the panel fill, a 1-unit top hairline at y0 and the 2-unit dark crimson edge ending at y1; no
	// side edges.
	inline void panel_frame(layer& out, f32 x0, f32 y0, f32 x1, f32 y1)
	{
		out.image(layer::b_under, &assets::get().panel, white, x0, y0, x1, y1);
		out.rect(layer::b_lines, top_edge, x0, y0, x1 - x0, 1.f);
		out.rect(layer::b_lines, edge_dark, x0, y1 - 2.f, x1 - x0, 2.f);
	}

	// The help line above the control bar: aqua text with glyph tokens at x 56, centred on y 647, over a faint underline as
	// wide as the text. `rings`: Xbox face buttons in their colours, as the Button Layout draws them.
	inline void help_line(layer& chrome, const atlas_ref& atlas, const fonts& type, std::string_view help, bool rings = false)
	{
		if (help.empty())
			return;
		rich_style style{type.body, aqua, 16.f};
		style.rings = rings;
		const f32 width = rich_text(&chrome, atlas, style, 56.f, 647.f, help);
		chrome.rect(layer::b_lines, rgb(0x8FC3CA, .38f), 56.f, 658.f, width + 4.f, 1.f);
	}

	// Whole seconds as a countdown shows them: two digits ("07").
	inline std::u32string two_digits(u32 value)
	{
		const u32 shown = std::min<u32>(value, 99);
		return {static_cast<char32_t>(U'0' + shown / 10), static_cast<char32_t>(U'0' + shown % 10)};
	}

	// The countdown band, top centre (y 52..92): the title, the seconds in the display face between thin
	// rules and an optional detail (in the secondary ink while the other player is still choosing), over a crimson bar along the
	// band's bottom edge that drains with `share` (the part of the countdown left).
	inline void countdown_band(layer& out, const fonts& type, std::u32string_view title, std::u32string_view seconds, std::u32string_view detail,
		bool detail_waiting, f32 share)
	{
		constexpr f32 gap = 12.f, timer_box = 48.f, top = 52.f, bottom = 92.f, centre = (top + bottom) / 2.f;
		const f32 content = text_width(type.label, title) + gap + 1.f + gap + timer_box +
		                    (detail.empty() ? 0.f : gap + 1.f + gap + text_width(type.caption, detail));
		const f32 left = std::round(640.f - content / 2.f) - 72.f, right = left + content + 144.f;
		band_strip(out, layer::b_under, band_fill, left, right, top, bottom);
		band_strip(out, layer::b_lines, rgb(0xE9EEED, .16f), left, right, top, top + 1.f);
		f32 x = left + 72.f;
		x += out.text(type.label, text_1, x, baseline(type.label, centre), title) + gap;
		out.rect(layer::b_lines, separator, x, centre - 9.f, 1.f, 18.f);
		x += 1.f + gap;
		out.text(type.timer, text_1, x + (timer_box - text_width(type.timer, seconds)) / 2.f, baseline(type.timer, centre), seconds);
		x += timer_box;
		if (!detail.empty())
		{
			x += gap;
			out.rect(layer::b_lines, separator, x, centre - 9.f, 1.f, 18.f);
			x += 1.f + gap;
			out.text(type.caption, detail_waiting ? text_2 : text_1, x, baseline(type.caption, centre), detail);
		}
		const f32 track = right - left - 144.f;
		out.rect(layer::b_lines, rgb(0xE9EEED, .16f), left + 72.f, bottom - 2.f, track, 2.f);
		out.rect(layer::b_lines, crimson, left + 72.f, bottom - 2.f, track * std::clamp(share, 0.f, 1.f), 2.f);
	}

	// Command history column (Practice, Replay and spectator Analyze): no panel, newest row on top, rows centred on
	// y 166 + 22n. A row is the outlined arrow (the neutral glyph when no direction is held), the attack-button cluster when
	// buttons are held and the held-frame count right-aligned in outlined atlas digits, over a light rule with a brighter
	// tab under the count. All three views pass screen-direction masks; opposite directions cancel.
	struct history_row
	{
		u16 mask = 0;
		u32 frames = 0;
		color4f count_tint = white;
	};

	inline constexpr u32 history_max_rows = 20;

	template <typename Row>
	void history_column(layer& out, const atlas_ref& atlas, usz side, u32 count, Row&& row)
	{
		static constexpr std::array<std::array<u8, 3>, 3> arrows{{{pbd_glyphs::arrow_ul, pbd_glyphs::arrow_u, pbd_glyphs::arrow_ur},
			{pbd_glyphs::arrow_l, pbd_glyphs::arrow_n, pbd_glyphs::arrow_r},
			{pbd_glyphs::arrow_dl, pbd_glyphs::arrow_d, pbd_glyphs::arrow_dr}}};
		const f32 x0 = side ? 1132.f : 34.f, x1 = x0 + 120.f;
		const f32 arrow_x = side ? 1197.f : 99.f, cluster_x = side ? 1217.f : 119.f, count_right = side ? 1250.f : 152.f;
		count = std::min(count, history_max_rows);
		for (u32 i = 0; i < count; i++)
		{
			const history_row entry = row(i);
			const f32 cy = 166.f + 22.f * i;
			out.rect(layer::b_lines, rgb(0xE9EEED, .55f), x0, cy + 10.f, x1 - x0, 1.f);
			out.rect(layer::b_lines, rgb(0x0B0304, .5f), x0, cy + 11.f, x1 - x0, 1.f);
			out.rect(layer::b_lines, rgb(0xE9EEED, .92f), count_right - 24.f, cy + 9.f, x1 - count_right + 24.f, 2.f);
			const u16 mask = entry.mask;
			const bool up = mask & 0x1000, down = mask & 0x4000, left = mask & 0x8000, right = mask & 0x2000;
			const usz v = up && !down ? 0 : down && !up ? 2 :
			                                              1;
			const usz h = left && !right ? 0 : right && !left ? 2 :
			                                                    1;
			out.sprite(atlas, arrows[v][h], arrow_x - 9.f, cy - 9.f);
			const u32 buttons = ((mask & 0x0080) ? 1u : 0u) | ((mask & 0x0010) ? 2u : 0u) | ((mask & 0x0040) ? 4u : 0u) | ((mask & 0x0020) ? 8u : 0u);
			if (buttons)
				out.sprite(atlas, pbd_glyphs::cluster_0 + buttons, cluster_x - 9.f, cy - 9.f);
			const std::string digits = std::to_string(std::min<u32>(entry.frames, 999));
			for (usz k = 0; k < digits.size(); k++)
			{
				const f32 cell = count_right - 7.f * (digits.size() - k) - 1.f;
				out.sprite(atlas, pbd_glyphs::digit_0 + (digits[k] - '0'), cell, cy - 8.f, 0.f, entry.count_tint);
			}
		}
	}
} // namespace rsx::overlays::pbd
