#pragma once

#include "overlay_pbd_draw.h"
#include "Emu/PBD/pbd_clip_hud.h"

#include <array>
#include <cmath>
#include <string>
#include <string_view>

// The clip keys in the pause menus' control bar and the clip toast under the timer (pbd_clip_hud.h).

namespace rsx::overlays::pbd
{
	namespace clip_hud = ::pbd::clips::hud;

	inline const color4f key_ink = rgb(0x14191B);

	// A named pad button's icon, drawn in the keycap's ink before its name: the touchpad's outline, Share's arrow over a
	// tray, Capture's framed dot. Returns its width, 0 for a name without one.
	inline f32 clip_icon(layer* out, std::string_view name, f32 x, f32 centre, f32 alpha)
	{
		const color4f ink = faded(key_ink, alpha);
		if (name == "Touchpad")
		{
			if (out)
			{
				const f32 y = centre - 4.f;
				out->rect(layer::b_lines, ink, x, y, 12.f, 1.2f);
				out->rect(layer::b_lines, ink, x, y, 1.2f, 6.f);
				out->rect(layer::b_lines, ink, x + 10.8f, y, 1.2f, 6.f);
				out->rect(layer::b_lines, ink, x + 1.f, y + 6.8f, 10.f, 1.2f);
			}
			return 12.f;
		}
		if (name == "Share")
		{
			if (out)
			{
				const f32 y = centre - 5.f;
				out->rect(layer::b_lines, ink, x + 4.f, y + .5f, 2.f, 1.f);
				out->rect(layer::b_lines, ink, x + 3.f, y + 1.5f, 4.f, 1.f);
				out->rect(layer::b_lines, ink, x + 2.f, y + 2.5f, 6.f, 1.f);
				out->rect(layer::b_lines, ink, x + 4.35f, y + 3.5f, 1.3f, 3.f);
				out->rect(layer::b_lines, ink, x + .5f, y + 5.4f, 1.2f, 3.5f);
				out->rect(layer::b_lines, ink, x + 8.3f, y + 5.4f, 1.2f, 3.5f);
				out->rect(layer::b_lines, ink, x + .5f, y + 8.3f, 9.f, 1.2f);
			}
			return 10.f;
		}
		if (name == "Capture")
		{
			if (out)
			{
				const f32 y = centre - 5.f;
				out->rect(layer::b_lines, ink, x + .7f, y + .7f, 8.6f, 1.2f);
				out->rect(layer::b_lines, ink, x + .7f, y + 8.1f, 8.6f, 1.2f);
				out->rect(layer::b_lines, ink, x + .7f, y + .7f, 1.2f, 8.6f);
				out->rect(layer::b_lines, ink, x + 8.1f, y + .7f, 1.2f, 8.6f);
				out->rect(layer::b_lines, ink, x + 3.f, y + 3.f, 4.f, 4.f);
			}
			return 10.f;
		}
		return 0.f;
	}

	// A keycap: a light plate with a dark rim, a shaded foot and dark ink, like the face-button glyphs; 18 units tall, at
	// least 20 wide. Returns its width.
	inline f32 keycap(layer* out, font* f, f32 x, f32 centre, std::string_view name, f32 alpha)
	{
		const auto text = utf8_to_u32string(name);
		const f32 icon = clip_icon(nullptr, name, 0.f, 0.f, 1.f);
		const f32 inner = (icon > 0.f ? icon + 4.f : 0.f) + text_width(f, text);
		const f32 w = std::max(20.f, inner + 12.f), h = 18.f, top = centre - h / 2.f;
		if (out)
		{
			out->rect(layer::b_bars, faded(key_ink, alpha), x, top, w, h);
			out->rect(layer::b_focus, faded(text_1, alpha), x + 1.f, top + 1.f, w - 2.f, h - 2.f);
			out->rect(layer::b_lines, faded(text_2, alpha), x + 1.f, top + h - 3.f, w - 2.f, 2.f);
			f32 at = x + std::round((w - inner) / 2.f);
			if (icon > 0.f)
				at += clip_icon(out, name, at, centre - 1.f, alpha) + 4.f;
			out->text(f, faded(key_ink, alpha), at, baseline(f, centre - 1.f), text);
		}
		return w;
	}

	// One prompt's keys from x, centred on `centre`: "Hold" in the quiet ink for a held pad button, then the keyboard keys as
	// keycaps joined by "+", the named pad button on its keycap, or the glyph of the pad button's job. With no layer they are
	// only measured. Returns the width.
	inline f32 clip_keys(layer* out, const atlas_ref& atlas, const fonts& type, font* hold_font, f32 x, f32 centre, const clip_hud::keys& keys,
		f32 alpha = 1.f)
	{
		f32 at = x;
		if (keys.hold)
		{
			constexpr std::u32string_view hold = U"Hold";
			if (out)
				out->text(hold_font, faded(text_3, alpha), at, baseline(hold_font, centre), hold);
			at += text_width(hold_font, hold) + 5.f;
		}
		if (keys.pad && keys.text.starts_with('{'))
		{
			rich_style glyph{type.caption, faded(text_1, alpha), 16.f};
			glyph.glyph_tint = faded(white, alpha);
			return at + rich_text(out, atlas, glyph, at, centre, keys.text) - x;
		}
		if (keys.pad)
			return at + keycap(out, type.caption, at, centre, keys.text, alpha) - x;
		bool first = true;
		for (const auto part : clip_hud::key_parts(keys.text))
		{
			if (!first)
			{
				constexpr std::u32string_view plus = U"+";
				if (out)
					out->text(type.caption, faded(text_2, alpha), at + 2.f, baseline(type.caption, centre), plus);
				at += text_width(type.caption, plus) + 4.f;
			}
			at += keycap(out, type.caption, at, centre, part, alpha);
			first = false;
		}
		return at - x;
	}

	// One clip action's keys in the control bar: the keyboard keys, then a quiet "/" and the pad's clip button when the pad in
	// use has one. With no layer they are only measured. Returns the width.
	inline f32 clip_choices(layer* out, const atlas_ref& atlas, const fonts& type, f32 x, f32 centre, const clip_hud::keys& keys,
		const clip_hud::keys& button)
	{
		f32 at = x + clip_keys(out, atlas, type, type.caption, x, centre, keys);
		if (button.text.empty())
			return at - x;
		constexpr std::u32string_view slash = U"/";
		if (out)
			out->text(type.caption, text_3, at + 5.f, baseline(type.caption, centre), slash);
		at += text_width(type.caption, slash) + 10.f;
		return at + clip_keys(out, atlas, type, type.caption, at, centre, button) - x;
	}

	// The clip keys at the right end of a pause menu's control bar, right-aligned at `right` on the bar's centre line: a thin
	// separator, then "<keys> Save clip" and "<keys> Record", labelled quieter than the menu's own prompts. The keyboard keys
	// always show, and the pad's clip button joins them while a pad that has one is in use. Returns the legend's left edge,
	// where the menu's prompts stop 26 units short, or `right` when nothing shows.
	inline f32 clip_legend(layer* out, const atlas_ref& atlas, const fonts& type, const clip_hud::legend& legend, f32 right, f32 centre)
	{
		if (!legend.shown)
			return right;
		struct group
		{
			const clip_hud::keys& keys;
			const clip_hud::keys& button;
			std::u32string_view label;
		};
		const std::array<group, 2> groups{{{legend.save, legend.save_button, U"Save clip"}, {legend.record, legend.record_button, U"Record"}}};
		constexpr f32 group_gap = 26.f, label_gap = 7.f;
		f32 width = 1.f;
		for (const auto& item : groups)
			if (!item.keys.text.empty())
				width += group_gap + clip_choices(nullptr, atlas, type, 0.f, centre, item.keys, item.button) + label_gap + text_width(type.caption, item.label);
		const f32 left = right - width;
		if (!out)
			return left;
		out->rect(layer::b_lines, separator, left, centre - 8.f, 1.f, 16.f);
		f32 x = left + 1.f;
		for (const auto& item : groups)
		{
			if (item.keys.text.empty())
				continue;
			x += group_gap;
			x += clip_choices(out, atlas, type, x, centre, item.keys, item.button) + label_gap;
			x += out->text(type.caption, text_2, x, baseline(type.caption, centre), item.label);
		}
		return left;
	}

	// The clip toast in the Practice toast's slot, centred under the timer (y 156..190), clear of the life bars, names, plates
	// and combo counters: the band with its 72-unit ramps and content inset 84, a hairline along the top and a crimson edge
	// along the bottom. The title in the display face, then after a thin separator the clip's length, the recording's length,
	// or the keys that stop the recording on the device in use, whose dot blinks before the title.
	inline void clip_toast(layer& out, const atlas_ref& atlas, const fonts& type, const clip_hud::toast& toast, const clip_hud::legend& legend,
		const clip_hud::frame& at)
	{
		const f32 a = at.alpha, centre = 173.f + at.offset, top = 156.f + at.offset;
		constexpr f32 gap = 10.f, dot = 9.f, inset = 84.f;
		constexpr std::u32string_view stop = U"to stop";
		const bool recording = toast.kind == clip_hud::toast_kind::recording;
		const auto title = utf8_to_u32string(clip_hud::title(toast.kind));
		const auto detail = utf8_to_u32string(toast.kind == clip_hud::toast_kind::clip_saved      ? clip_hud::length_text(toast.seconds) :
											  toast.kind == clip_hud::toast_kind::recording_saved ? clip_hud::duration_text(toast.length_ms) :
																									std::string());
		const clip_hud::keys& stopper = clip_hud::stop_keys(legend);
		const bool stop_keys = recording && !stopper.text.empty();
		const f32 detail_w = stop_keys ? clip_keys(nullptr, atlas, type, type.label, 0.f, centre, stopper) + 6.f + text_width(type.label, stop) :
		                                 text_width(type.label, detail);
		const f32 content = (recording ? dot + gap : 0.f) + text_width(type.title, title) + (detail_w > 0.f ? gap + 1.f + gap + detail_w : 0.f);
		const f32 left = std::round(640.f - content / 2.f) - inset, right = left + content + 2.f * inset;
		band_strip(out, layer::b_under, faded(band_fill, a), left, right, top, top + 34.f);
		band_strip(out, layer::b_lines, rgb(0xE9EEED, .16f * a), left, right, top, top + 1.f);
		band_strip(out, layer::b_lines, rgb(0xE3304F, .75f * a), left, right, top + 32.f, top + 34.f);
		f32 x = left + inset;
		if (recording)
		{
			if (at.dot)
				out.sprite(atlas, pbd_glyphs::rec_dot, x, centre - dot / 2.f, dot, faded(white, a));
			x += dot + gap;
		}
		x += out.text(type.title, faded(text_1, a), x, baseline(type.title, centre), title);
		if (detail_w <= 0.f)
			return;
		x += gap;
		out.rect(layer::b_lines, faded(separator, a), x, centre - 8.f, 1.f, 16.f);
		x += 1.f + gap;
		if (stop_keys)
		{
			x += clip_keys(&out, atlas, type, type.label, x, centre, stopper, a) + 6.f;
			out.text(type.label, faded(text_2, a), x, baseline(type.label, centre), stop);
		}
		else
			out.text(type.label, faded(text_2, a), x, baseline(type.label, centre), detail);
	}
} // namespace rsx::overlays::pbd
