#pragma once

// The Ranked and Quick Match notices' drawing kit, in the Versus overlay's voice: italic chrome headlines in the display
// face, the game's own pictures (the rank plates, the placement plate cut from the Beginner stone, the round-win lamps, the gold
// pill, the word art) read from the player's own game files, name-plate rows with the gold rank plate as the chosen row,
// glyph prompts along the foot with no box, and the other player's status at the opposite edge with that side's dot. Nothing is
// drawn in a box. Text drawn in the display face falls back to nothing, never to another face (fonts_ready).

#include "overlay_pbd_draw.h"
#include "overlay_pbd_lettering.h"
#include "Emu/PBD/pbd_set_notices.h"
#include "Emu/Cell/pbd_notice_art.h"
#include "Emu/Cell/pbd_live_stats.h"

#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rsx::overlays::pbd::notices
{
	namespace model = ::pbd::set_notices;
	namespace at = ::pbd::set_notices::layout;
	namespace art = ::pbd::notice_art;
	using plate = ::pbd::game_overlay::plate;

	// The players' colours on the dots and the row mark (P1 and P2).
	inline const std::array<color4f, 2> side_color{rgb(0xFF3D57), rgb(0x3D8BFF)};
	inline const color4f row_ink = rgb(0xE9EEED, .86f), chosen_ink = rgb(0x1B1106), caption_ink = rgb(0xAEBDBD);

	// Whether a bundled face is in the executable's fonts folder. Checked once per face and logged when missing: the notices
	// then leave out the words in that face rather than let the font manager take a system face.
	inline bool bundled(std::string_view file)
	{
		static std::mutex mutex;
		static std::vector<std::pair<std::string, bool>> known;
		std::lock_guard lock(mutex);
		for (const auto& [name, ok] : known)
			if (name == file)
				return ok;
		const bool ok = fs::is_file(fs::get_executable_dir() + "/fonts/" + std::string(file));
		if (!ok)
			rsx_log.error("PBD notices: the bundled face %s is missing; its words are left out.", file);
		known.emplace_back(std::string(file), ok);
		return ok;
	}

	// The notices' faces, packed once. The display face is rasterised at one size and scaled to each headline; every size here
	// packs its Latin page into one glyph texture.
	struct faces
	{
		font* chrome = nullptr;  // PBD Display, em 40: the headlines, scaled
		font* name = nullptr;    // PBD Display, em 28: the names on the cards
		font* row = nullptr;     // PBD Display, em 15: the rows, the tags and the name bands
		font* row_end = nullptr; // PBD Display, em 18: a row's countdown
		font* caption = nullptr; // Barlow 600, em 12.5: the captions, upper case and tracked
		font* line = nullptr;    // Barlow 600, em 16: the one line under a headline, the statuses
		font* prompt = nullptr;  // Barlow 400, em 14.4: the prompts
		bool display = false;    // the display face is bundled
	};
	inline const faces& type()
	{
		static const faces value = []
		{
			faces out;
			out.display = bundled(display_font);
			if (out.display)
			{
				out.chrome = fontmgr::get(display_font, 36);
				out.name = fontmgr::get(display_font, 25);
				out.row = fontmgr::get(display_font, 13);
				out.row_end = fontmgr::get(display_font, 16);
			}
			const bool barlow = bundled(label_font) && bundled(body_font);
			if (barlow)
			{
				out.caption = fontmgr::get(label_font, 11);
				out.line = fontmgr::get(label_font, 14);
				out.prompt = fontmgr::get(body_font, 13);
			}
			for (font* f : {out.chrome, out.name, out.row, out.row_end, out.caption, out.line, out.prompt})
			{
				if (!f)
					continue;
				f32 x = 0.f, y = 0.f;
				f->get_char(U'A', x, y);
				f->get_char(char32_t{0x2013}, x, y);
			}
			return out;
		}();
		return value;
	}

	// A face's em in canvas units, and the scale that draws it at `em`.
	inline f32 em_of(const font* f)
	{
		return f ? f->get_size_px() / 1.2f : 0.f;
	}
	inline f32 scale_for(const font* f, f32 em)
	{
		return f ? em / em_of(f) : 1.f;
	}

	enum class align : u8
	{
		left,
		centre,
		right
	};
	inline f32 place_x(f32 x, f32 width, align a)
	{
		return a == align::left ? x : a == align::centre ? std::round(x - width / 2.f) :
		                                                   x - width;
	}

	// Text with tracking (letter spacing, in em) drawn glyph by glyph, its capitals centred on `centre`; returns its width.
	inline f32 tracked(layer* out, font* f, color4f ink, f32 x, f32 centre, std::u32string_view text, f32 scale, f32 tracking, align a = align::left,
		f32 slant = 0.f)
	{
		if (!f || text.empty())
			return 0.f;
		const f32 space = tracking * em_of(f) * scale;
		f32 width = 0.f;
		for (const char32_t c : text)
			width += text_width(f, std::u32string_view(&c, 1)) * scale + space;
		width -= space;
		if (!out)
			return width;
		f32 pen = place_x(x, width, a);
		const f32 base = centre + f->get_size_px() * .2917f * scale;
		for (const char32_t c : text)
			pen += out->text(f, ink, pen, base, std::u32string_view(&c, 1), scale, slant) + space;
		return width;
	}

	// A caption: Barlow upper case, tracked 0.2 em, in the secondary ink with a soft shadow; returns its width. The letters are
	// drawn twice, a third of a unit apart, which gives the bundled Barlow 600 the weight of the captions' Barlow 700.
	inline f32 caption(layer& out, f32 x, f32 centre, std::string_view words, align a = align::left, color4f ink = caption_ink)
	{
		const auto& f = type();
		const auto text = utf8_to_u32string(::pbd::set_notices::upper(words));
		const f32 scale = scale_for(f.caption, 12.f);
		const f32 width = tracked(nullptr, f.caption, ink, x, centre, text, scale, .2f);
		const f32 left = place_x(x, width, a);
		tracked(&out, f.caption, rgb(0x000000, .75f * ink.a), left + 1.f, centre + 1.f, text, scale, .2f);
		tracked(&out, f.caption, ink, left, centre, text, scale, .2f);
		tracked(&out, f.caption, ink, left + .33f, centre, text, scale, .2f);
		return width;
	}

	// One line of Barlow under a headline, with the soft shadow that keeps it readable over the game, upright or leaning like
	// the game's own white italic (`slant`); returns its width.
	inline f32 line(layer& out, f32 x, f32 centre, std::string_view words, align a = align::left, color4f ink = text_1, f32 slant = 0.f)
	{
		const auto& f = type();
		if (!f.line || words.empty())
			return 0.f;
		const auto text = utf8_to_u32string(words);
		const f32 width = text_width(f.line, text);
		const f32 left = place_x(x, width, a), base = baseline(f.line, centre);
		out.text(f.line, rgb(0x000000, .8f * ink.a), left + 1.f, base + 1.5f, text, 1.f, slant);
		out.text(f.line, ink, left, base, text, 1.f, slant);
		return width;
	}

	// The chrome headline: italic display caps in the select screen's white-to-steel (or gold) gradient, cut into bands over the
	// line, with a dark rim and a drop shadow. `em` is its size; returns its width.
	inline f32 chrome(layer* out, f32 x, f32 centre, std::string_view words, f32 em, bool gold = false, align a = align::left, bool upright = false)
	{
		const auto& f = type();
		if (!f.chrome || words.empty())
			return 0.f;
		const auto text = utf8_to_u32string(words);
		const f32 scale = scale_for(f.chrome, em), slant = upright ? 0.f : .2f;
		const f32 width = text_width(f.chrome, text) * scale;
		if (!out)
			return width;
		const f32 left = place_x(x, width, a), base = centre + f.chrome->get_size_px() * .2917f * scale;
		// The rim and the drop shadow.
		out->text(f.chrome, rgb(0x000000, .85f), left, base + 2.f, text, scale, slant);
		for (const auto& [dx, dy] : {std::pair{-1.f, 0.f}, std::pair{1.f, 0.f}, std::pair{0.f, -1.f}, std::pair{0.f, 1.f}})
			out->text(f.chrome, rgb(0x000000, .9f), left + dx, base + dy, text, scale, slant);
		// The gradient runs over the line box (1.1 em); eight bands over the letters.
		struct stop
		{
			f32 at;
			u32 colour;
		};
		static constexpr std::array<stop, 4> steel{{{0.f, 0xFFFFFF}, {.44f, 0xFFFFFF}, {.56f, 0xD9DDE3}, {1.f, 0x9AA3AD}}};
		static constexpr std::array<stop, 5> warm{{{0.f, 0xFFF7D6}, {.36f, 0xFFE28E}, {.54f, 0xF2B844}, {.76f, 0xC98A22}, {1.f, 0x8F5F17}}};
		const auto sample = [&](f32 t)
		{
			const auto blend = [&](const auto& stops)
			{
				usz k = 0;
				while (k + 2 < stops.size() && t > stops[k + 1].at)
					k++;
				const f32 span = stops[k + 1].at - stops[k].at;
				const f32 w = span > 0.f ? std::clamp((t - stops[k].at) / span, 0.f, 1.f) : 0.f;
				const color4f c0 = rgb(stops[k].colour), c1 = rgb(stops[k + 1].colour);
				return color4f{c0.r + (c1.r - c0.r) * w, c0.g + (c1.g - c0.g) * w, c0.b + (c1.b - c0.b) * w, 1.f};
			};
			return gold ? blend(warm) : blend(steel);
		};
		const f32 box_top = centre - .55f * em, box_h = 1.1f * em;
		constexpr int bands = 8;
		const f32 top = centre - .5f * em, bottom = centre + .5f * em;
		for (int i = 0; i < bands; ++i)
		{
			const f32 y0 = top + (bottom - top) * static_cast<f32>(i) / bands, y1 = top + (bottom - top) * static_cast<f32>(i + 1) / bands;
			const f32 mid = (y0 + y1) / 2.f;
			out->text_band(f.chrome, sample((mid - box_top) / box_h), left, base, text, scale, slant, i ? y0 : -1e9f, i + 1 < bands ? y1 : 1e9f);
		}
		return width;
	}

	// A score in the chrome face, upright: the two numbers and between them a dash drawn as a short slanted bar at the
	// middle of the capitals (not the face's own dash).
	inline void score(layer& out, f32 centre_x, f32 centre_y, u32 left, u32 right, f32 em)
	{
		const auto lhs = std::to_string(left), rhs = std::to_string(right);
		const f32 gap = .22f * em, dash_w = .5f * em, dash_h = .12f * em;
		const f32 left_w = chrome(nullptr, 0.f, 0.f, lhs, em, false, align::left, true), right_w = chrome(nullptr, 0.f, 0.f, rhs, em, false, align::left, true);
		if (left_w <= 0.f)
			return;
		const f32 x = std::round(centre_x - (left_w + gap + dash_w + gap + right_w) / 2.f);
		chrome(&out, x, centre_y, lhs, em, false, align::left, true);
		chrome(&out, x + left_w + 2.f * gap + dash_w, centre_y, rhs, em, false, align::left, true);
		const f32 dash_x = x + left_w + gap, dash_y = centre_y - dash_h / 2.f;
		const auto& ramp = assets::get().ramp;
		out.image(layer::b_sprites, &ramp, rgb(0x000000, .9f), dash_x - 1.f, dash_y - 1.f, dash_x + dash_w + 1.f, dash_y + dash_h + 1.f, 1.f, 0.f, 1.f, 1.f, .36f);
		out.image(layer::b_sprites, &ramp, rgb(0xECEEF1), dash_x, dash_y, dash_x + dash_w, dash_y + dash_h, 1.f, 0.f, 1.f, 1.f, .36f);
	}

	// A name over the game's art: display caps in white with a dark rim and shadow, no fill behind it.
	inline f32 outlined_name(layer& out, f32 x, f32 centre, std::string_view words, f32 em, align a)
	{
		const auto& f = type();
		if (!f.name || words.empty())
			return 0.f;
		const auto text = utf8_to_u32string(::pbd::set_notices::upper(words));
		const f32 scale = scale_for(f.name, em), spacing = .1f;
		const f32 width = tracked(nullptr, f.name, white, x, centre, text, scale, spacing);
		const f32 left = place_x(x, width, a);
		tracked(&out, f.name, rgb(0x000000, .8f), left, centre + 3.f, text, scale, spacing);
		for (const auto& [dx, dy] : {std::pair{-2.f, 0.f}, std::pair{2.f, 0.f}, std::pair{0.f, -2.f}, std::pair{0.f, 2.f}, std::pair{1.5f, 1.5f},
				 std::pair{-1.5f, -1.5f}, std::pair{1.5f, -1.5f}, std::pair{-1.5f, 1.5f}})
			tracked(&out, f.name, rgb(0x000000), left + dx, centre + dy, text, scale, spacing);
		tracked(&out, f.name, white, left, centre, text, scale, spacing);
		return width;
	}

	// The game's pictures, decoded once from the bytes the latch read and kept for the process (the renderer caches images by
	// address). Null when the picture is missing.
	inline const image_info* picture(u32 id)
	{
		static std::mutex mutex;
		static std::array<std::unique_ptr<image_info>, art::picture_count> decoded{};
		static std::array<bool, art::picture_count> tried{};
		if (id >= art::picture_count)
			return nullptr;
		std::lock_guard lock(mutex);
		if (!tried[id])
		{
			if (const auto bytes = art::get(id))
			{
				tried[id] = true;
				decoded[id] = std::make_unique<image_info>(*bytes);
			}
		}
		const auto& slot = decoded[id];
		return slot && slot->w > 0 && slot->h > 0 && slot->get_data() ? slot.get() : nullptr;
	}
	inline const image_info* picture(art::picture id)
	{
		return picture(static_cast<u32>(id));
	}
	inline const image_info* rank_badge(u32 rank)
	{
		static std::mutex mutex;
		static std::array<std::unique_ptr<image_info>, ::pbd::game_overlay::max_rank_index + 1> decoded{};
		if (rank > ::pbd::game_overlay::max_rank_index)
			return nullptr;
		std::lock_guard lock(mutex);
		auto& slot = decoded[rank];
		if (!slot)
			if (const auto bytes = art::rank_badge(rank))
				slot = std::make_unique<image_info>(*bytes);
		return slot && slot->w > 0 && slot->h > 0 && slot->get_data() ? slot.get() : nullptr;
	}

	// The placement plate's lettering: "Placing" in white and the count in gold, leaning 12 degrees with a thick black outline
	// and a soft shadow, as the game letters "3rd dan". The game's lettering is a heavy, wide italic: Barlow 600 thickened by
	// 1 and widened 12 % comes close to it, as the launcher letters the same plate. Baked once per count at Set complete's size.
	inline const lettering::baked& placing_letters(u32 done, u32 total)
	{
		return lettering::cached("placing-" + std::to_string(done) + "/" + std::to_string(total), [&]
			{
				lettering::style look;
				look.em = 30.2f;
				look.slant = .21f;
				look.tracking = .015f;
				look.widen = 1.12f;
				look.embolden = 1.f;
				look.outline = 3.2f;
				look.shadow_dx = 1.8f;
				look.shadow_dy = 2.3f;
				look.shadow_blur = .9f;
				look.shadow_alpha = .7f;
				return lettering::bake(lettering::bundled_face(label_font),
					{{U"Placing ", 0xFFFFFF}, {utf8_to_u32string(std::to_string(done) + "/" + std::to_string(total)), 0xFFD24A, 1.f, 3.7f}}, look);
			});
	}

	// A plate at (x, y) in a w x h slot: the game's rank plate, or the placement plate (the stone and its lettering). Without the
	// picture, the plate's words alone, never a box.
	inline void draw_plate(layer& out, const plate& shown, f32 x, f32 y, f32 w, f32 h, f32 alpha = 1.f)
	{
		if (shown.shown == plate::kind::rank)
		{
			if (const auto* badge = rank_badge(shown.rank))
			{
				const f32 fit = std::min(w / static_cast<f32>(badge->w), h / static_cast<f32>(badge->h));
				const f32 bw = badge->w * fit, bh = badge->h * fit;
				const f32 bx = x + (w - bw) / 2.f, by = y + (h - bh) / 2.f;
				out.image(layer::b_bars, badge, rgb(0xFFFFFF, alpha), bx, by, bx + bw, by + bh);
				return;
			}
			const auto name = std::string(::pbd::game_profile::rank_names[::pbd::game_profile::rank_byte(shown.rank)]);
			line(out, x + w / 2.f, y + h / 2.f, name, align::centre, rgb(0xFFFFFF, alpha));
			return;
		}
		if (shown.shown != plate::kind::placing)
			return;
		if (const auto* stone = picture(art::picture::placement_stone))
			out.image(layer::b_bars, stone, rgb(0xFFFFFF, alpha), x, y, x + w, y + h);
		// The lettering is baked at Set complete's size and fits 436 of the stone's 492 width; its capitals sit a touch below
		// the stone's middle, as on the game's own plates.
		const auto& words = placing_letters(shown.done, shown.total);
		const f32 scale = std::min(h / at::big_placing_h, w * 436.f / 492.f / std::max(1.f, words.advance));
		if (!lettering::draw_centred(out, words, x + w * .5f, y + h * .525f, scale, alpha))
			line(out, x + w / 2.f, y + h / 2.f, ::pbd::set_notices::placing_words(shown.done, shown.total), align::centre, rgb(0xFFFFFF, alpha));
	}

	// The round-win lamps for a placement count: `total` lamps, `lit` of them lit, the newest one with the game's flash. Nothing
	// for a total without its own holder (4 and 5 have one) or when the pictures are missing.
	inline void draw_lamps(layer& out, u32 total, u32 lit, bool flash, f32 x, f32 y, f32 w, f32 h)
	{
		if (const auto id = art::lamps(total, lit, flash))
			if (const auto* lamps = picture(*id))
				out.image(layer::b_sprites, lamps, white, x, y, x + w, y + h);
	}

	// "Placement Match" on the gold pill in the plates' lettering (Barlow 600 thickened and widened 14 %), leaning 12 degrees,
	// crisp, with a faint warm halo and a brown shade, within the game's lettering width (724 of the pill's 1000).
	inline const lettering::baked& pill_letters()
	{
		return lettering::cached("pill", []
			{
				lettering::style look;
				look.em = 52.5f;
				look.slant = .21f;
				look.tracking = .033f;
				look.widen = 1.14f;
				look.embolden = 2.3f;
				look.glow_blur = 2.7f;
				look.glow_rgb = 0xFFF6D6;
				look.glow_alpha = .1f;
				look.shadow_dx = 2.f;
				look.shadow_dy = 2.7f;
				look.shadow_alpha = .55f;
				look.shadow_rgb = 0x5A3400;
				return lettering::bake(lettering::bundled_face(label_font), {{U"Placement Match", 0xFFFFFF}}, look);
			});
	}

	// The gold Placement Match pill in the w x h slot of the game's 1000 x 400 pill texture: the picture and its lettering, or
	// the words alone in gold caps without the picture.
	inline void draw_pill(layer& out, f32 x, f32 y, f32 w, f32 h)
	{
		const f32 unit = w / 1000.f;
		if (const auto* pill = picture(art::picture::placement_match))
		{
			out.image(layer::b_bars, pill, white, x, y, x + w, y + h);
			// Baked at the intro's size; fitted to the game's lettering width, 4 right of the pill's middle and 2 above it.
			const auto& words = pill_letters();
			const f32 fit = 724.f * unit / std::max(1.f, words.advance);
			lettering::draw_centred(out, words, x + 504.f * unit, y + 198.f * unit, std::min(w / at::pill_w, fit));
			return;
		}
		const auto& f = type();
		const auto words = utf8_to_u32string("PLACEMENT MATCH");
		const f32 scale = scale_for(f.line, std::max(12.f, 60.f * unit));
		tracked(&out, f.line, gold, x + w / 2.f, y + h / 2.f, words, scale, .12f, align::centre);
	}

	// The game's stake words where we place them, in its condensed bold italic (Barlow 600 narrowed and thickened): white, a
	// little see-through, tracked, leaning, in the game's green glow for a promotion chance and its dark red for a demotion
	// risk. `edge` is the left or right end, `top` the image's top.
	inline const lettering::baked& stake_letters(model::stake_word word)
	{
		const bool promotion = word == model::stake_word::promotion;
		return lettering::cached(promotion ? "stake-promotion" : "stake-demotion", [&]
			{
				lettering::style look;
				look.em = 21.f;
				look.slant = .21f;
				look.tracking = .08f;
				look.widen = .86f;
				look.embolden = .5f;
				look.glow_spread = .6f;
				look.glow_blur = 1.f;
				look.glow_rgb = promotion ? 0x36BE46 : 0x3A0E14;
				look.glow_alpha = 1.f;
				return lettering::bake(lettering::bundled_face(label_font), {{utf8_to_u32string(model::stake_words(word)), 0xF6F7F3, .92f}}, look);
			});
	}
	inline void draw_stake(layer& out, model::stake_word word, f32 edge, f32 top, bool right_aligned)
	{
		if (word == model::stake_word::none)
			return;
		const auto& words = stake_letters(word);
		if (!words.image)
			return;
		const f32 centre_y = top + at::stake_h / 2.f;
		const f32 centre_x = right_aligned ? edge - words.advance / 2.f : edge + words.advance / 2.f;
		lettering::draw_centred(out, words, centre_x, centre_y);
	}

	// A wide soft shade: an image of one row (or column) of alpha stops, stretched.
	struct shades
	{
		raw_rgba left{256, 1}, left_deep{256, 1}, top{1, 128}, bottom{1, 128}, row{256, 1}, row_mid{256, 1}, band{256, 1};
		raw_rgba selection{1, 64};
		shades()
		{
			left.gradient({{0.f, 0x060203, .9f}, {.3f, 0x060203, .72f}, {.58f, 0x060203, .25f}, {.8f, 0x060203, 0.f}, {1.f, 0x060203, 0.f}});
			left_deep.gradient({{0.f, 0x060203, .94f}, {.34f, 0x060203, .86f}, {.5f, 0x060203, .5f}, {.7f, 0x060203, 0.f}, {1.f, 0x060203, 0.f}});
			top.gradient({{0.f, 0x060203, .78f}, {.55f, 0x060203, .5f}, {1.f, 0x060203, 0.f}});
			bottom.gradient({{0.f, 0x060203, 0.f}, {1.f, 0x060203, .72f}});
			row.gradient({{0.f, 0x08080A, .95f}, {.8f, 0x08080A, .95f}, {1.f, 0x08080A, 0.f}});
			row_mid.gradient({{0.f, 0x08080A, 0.f}, {.18f, 0x08080A, .95f}, {.82f, 0x08080A, .95f}, {1.f, 0x08080A, 0.f}});
			band.gradient({{0.f, 0x0A0A0C, .8f}, {.7f, 0x0A0A0C, .62f}, {1.f, 0x0A0A0C, 0.f}});
			selection.gradient({{0.f, 0xFFF0B8, 1.f}, {.3f, 0xF2C766, 1.f}, {.62f, 0xC99031, 1.f}, {1.f, 0x8F5F17, 1.f}});
		}
		static const shades& get()
		{
			// Process lifetime: the renderer caches images by address.
			static const shades* instance = new shades();
			return *instance;
		}
	};

	// The game held behind a menu: a light shade, the top and foot darkened, and the menu's side darkened toward it.
	inline void held_frame(layer& out, f32 shade, int menu_side, bool deep = false)
	{
		const auto& s = shades::get();
		out.rect(layer::b_under, rgb(0x050203, shade), 0.f, 0.f, 1280.f, 720.f);
		out.image(layer::b_under, &s.top, white, 0.f, 0.f, 1280.f, 220.f);
		out.image(layer::b_under, &s.bottom, white, 0.f, 610.f, 1280.f, 720.f);
		const auto& side = deep ? s.left_deep : s.left;
		if (menu_side == 0)
			out.image(layer::b_under, &side, white, 0.f, 0.f, 1280.f, 720.f);
		else if (menu_side == 1)
			out.image(layer::b_under, &side, white, 0.f, 0.f, 1280.f, 720.f, 1.f, 0.f, 0.f, 1.f);
	}

	// A player's name band where the life bar was: the game's dark name row with the name in tracked display caps.
	inline void name_band(layer& out, u32 side, std::string_view name)
	{
		const auto& s = shades::get();
		const f32 x = side ? at::right_edge - at::band_w : at::margin, y = at::band_top;
		out.image(layer::b_under, &s.band, white, x, y, x + at::band_w, y + at::band_h, side ? 1.f : 0.f, 0.f, side ? 0.f : 1.f, 1.f);
		const auto& f = type();
		const auto text = utf8_to_u32string(::pbd::set_notices::upper(name));
		const f32 centre = y + at::band_h / 2.f;
		tracked(&out, f.row, rgb(0x000000, .8f), (side ? x + at::band_w - 16.f : x + 16.f) + 1.f, centre + 1.f, text, 1.f, .2f, side ? align::right : align::left);
		tracked(&out, f.row, white, side ? x + at::band_w - 16.f : x + 16.f, centre, text, 1.f, .2f, side ? align::right : align::left);
	}

	// A menu row as the game's dark name plate; the chosen row is the gold rank plate leaning like the game's, with the player's
	// mark beside it. `end` is drawn at the row's far end: a countdown in the display face, or a caption.
	struct row_view
	{
		std::string label, end;
		bool enabled = true, focus = false, chosen = false, end_is_count = false;
	};
	inline void plate_row(layer& out, const atlas_ref& atlas, const row_view& row, f32 x, f32 top, f32 w, align side, u32 player, bool last)
	{
		const auto& s = shades::get();
		const auto& f = type();
		const f32 h = at::row_h;
		if (side == align::centre)
			out.image(layer::b_bars, &s.row_mid, white, x, top, x + w, top + h);
		else
		{
			const bool right = side == align::right;
			out.image(layer::b_bars, &s.row, white, x, top, x + w, top + h, right ? 1.f : 0.f, 0.f, right ? 0.f : 1.f, 1.f);
		}
		out.rect(layer::b_lines, rgb(0xFFFFFF, .16f), x, top, w, 1.f);
		if (last)
			out.rect(layer::b_lines, rgb(0xFFFFFF, .10f), x, top + h, w, 1.f);
		const bool gold_row = row.chosen || (row.focus && row.enabled);
		if (gold_row)
		{
			const f32 start = x + (side == align::right ? -5.f : 5.f);
			out.image(layer::b_focus, &s.selection, white, start, top + 3.f, start + w, top + h - 3.f, 0.f, 0.f, 1.f, 1.f, .25f);
			out.rect(layer::b_lines, rgb(0xFFF5D5, .7f), start + 6.f, top + 3.f, w - 8.f, 1.f);
			out.rect(layer::b_lines, rgb(0x3A2405, .85f), start, top + h - 4.f, w - 2.f, 1.f);
		}
		if (row.focus && row.enabled)
			out.sprite(atlas, find_sprite(side == align::right ? "arrow_l" : "arrow_r"), side == align::right ? x + w + 4.f : x - 16.f,
				top + (h - 12.f) / 2.f, 12.f, side_color[player & 1]);
		const color4f ink = gold_row ? chosen_ink : row.enabled ? row_ink :
		                                                          rgb(0xE9EEED, .38f);
		const auto label = utf8_to_u32string(::pbd::set_notices::upper(row.label));
		const f32 centre = top + h / 2.f + .5f;
		const f32 slant = gold_row ? .2f : 0.f;
		const f32 label_w = tracked(nullptr, f.row, ink, 0.f, centre, label, 1.f, .09f);
		const f32 label_x = side == align::right ? x + w - 20.f - label_w : side == align::centre ? std::round(x + (w - label_w) / 2.f) :
		                                                                                            x + 20.f;
		tracked(&out, f.row, ink, label_x, centre, label, 1.f, .09f, align::left, slant);
		if (row.end.empty())
			return;
		const f32 end_x = side == align::right ? x + 22.f : x + w - 22.f;
		const align end_align = side == align::right ? align::left : align::right;
		if (row.end_is_count)
			tracked(&out, f.row_end, ink, end_x, centre, utf8_to_u32string(row.end), 1.f, .04f, end_align);
		else
			caption(out, end_x, centre, row.end, end_align, gold_row ? rgb(0x1B1106, .9f) : caption_ink);
	}

	// Prompts at the foot on one side: glyph and Barlow with no box.
	inline void prompts(layer& out, const atlas_ref& atlas, const std::vector<prompt_item>& items, bool right)
	{
		const auto& f = type();
		font* face = f.prompt;
		if (!face || items.empty())
			return;
		const rich_style keys{face, text_1, 16.f};
		if (!right)
		{
			prompt_bar(out, atlas, face, at::margin, at::prompt_y, 1280.f - at::margin, items);
			return;
		}
		f32 width = 0.f;
		for (usz i = 0; i < items.size(); ++i)
			width += (i ? 26.f : 0.f) + rich_text(nullptr, atlas, keys, 0.f, at::prompt_y, items[i].keys) + 7.f + text_width(face, utf8_to_u32string(items[i].label));
		prompt_bar(out, atlas, face, at::right_edge - width, at::prompt_y, at::right_edge + 1.f, items);
	}

	// A player's status with that player's dot before it, aligned at x.
	inline void status_dot_at(layer& out, u32 player, std::string_view words, f32 x, f32 centre, align a)
	{
		const auto& f = type();
		if (!f.prompt || words.empty())
			return;
		const auto text = utf8_to_u32string(words);
		const f32 width = 14.f + text_width(f.prompt, text);
		const f32 left = place_x(x, width, a) + 14.f;
		out.rect(layer::b_lines, rgb(0x000000, .6f), left - 15.f, centre - 5.f, 10.f, 10.f);
		out.rect(layer::b_lines, side_color[player & 1], left - 14.f, centre - 4.f, 8.f, 8.f);
		out.text(f.prompt, rgb(0x000000, .8f), left + 1.f, baseline(f.prompt, centre) + 1.5f, text);
		out.text(f.prompt, text_1, left, baseline(f.prompt, centre), text);
	}

	// The other player's status at the opposite edge of the foot, with that player's dot.
	inline void status_dot(layer& out, u32 player, std::string_view words, bool right, f32 centre = at::prompt_y)
	{
		status_dot_at(out, player, words, right ? at::right_edge : at::margin, centre, right ? align::right : align::left);
	}

	// A connection: 0..4 bars (green, amber or red) and the words beside them; returns its width.
	inline f32 net_line(layer* out, u32 bars, std::string_view words, f32 x, f32 centre, align a, font* fallback = nullptr)
	{
		const auto& f = type();
		font* line = ::pbd::live_stats::connection_face(f.line, fallback);
		const auto text = utf8_to_u32string(words);
		constexpr f32 bars_w = 4 * 3.f + 3 * 2.f;
		const f32 width = bars_w + 7.f + (line ? text_width(line, text) * .82f : 0.f);
		if (!out)
			return width;
		const f32 left = place_x(x, width, a);
		const auto signal = model::signal_of(bars);
		const color4f lit = signal == model::signal::good ? rgb(0x3DDC84) : signal == model::signal::fair ? rgb(0xFFA629) :
		                                                                                                    rgb(0xFF4D4D);
		for (u32 i = 0; i < 4; ++i)
		{
			const f32 bh = 4.f + 3.f * i;
			out->rect(layer::b_bars, rgb(0x000000, .6f), left + i * 5.f - .5f, centre + 6.5f - bh - .5f, 4.f, bh + 1.f);
			out->rect(layer::b_bars, i < bars ? lit : rgb(0xFFFFFF, .25f), left + i * 5.f, centre + 6.5f - bh, 3.f, bh);
		}
		if (line)
		{
			const f32 tx = left + bars_w + 7.f, base = centre + line->get_size_px() * .2917f * .82f;
			out->text(line, rgb(0x000000, .8f), tx + 1.f, base + 1.5f, text, .82f);
			out->text(line, text_1, tx, base, text, .82f);
		}
		return width;
	}

	// A card on the cover, the VS screen or a challenger: the fighter's caption, the name and the plate, at a side's edge
	// (left or right aligned) or centred on x.
	struct card_view
	{
		std::string character, name;
		plate shown;
	};
	inline void card(layer& out, const card_view& view, f32 x, align a, f32 caption_y, f32 name_y, f32 plate_top)
	{
		caption(out, a == align::left ? x + 2.f : a == align::right ? x - 2.f :
																	  x,
			caption_y, view.character, a);
		outlined_name(out, x, name_y, view.name, at::card_name_em, a);
		const bool placing = view.shown.shown == plate::kind::placing;
		const f32 w = placing ? at::placing_w : at::rank_w;
		const f32 px = a == align::left ? x - at::card_plate_inset : a == align::right ? x + at::card_plate_inset - w :
		                                                                                 std::round(x - w / 2.f);
		draw_plate(out, view.shown, px, plate_top, w, at::rank_h);
	}
} // namespace rsx::overlays::pbd::notices
