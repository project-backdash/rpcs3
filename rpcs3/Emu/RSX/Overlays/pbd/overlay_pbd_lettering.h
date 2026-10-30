#pragma once

// Lettering baked into an image: words set once in a bundled face with the look the glyph atlas cannot give (a slant, wider
// and thicker letters, a thick outline, a soft shadow or glow, a run in a second colour), rasterised with stb_truetype at
// twice the canvas scale so they
// stay sharp at 1080p and 1440p. The notices letter the game's own pictures this way (the placement plate's "Placing 2/5",
// the gold pill's "Placement Match", the stake words) in a heavier, wider slant than the face draws. Each baked image is kept for the
// process: the renderer caches images by address, and the set of words is small and bounded.

#include "pbd_overlay_canvas.h"
#include "Utilities/File.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rsx::overlays::pbd::lettering
{
	// Canvas units to image pixels.
	inline constexpr f32 bake_scale = 2.f;

	// A bundled face from the executable's fonts folder, read once; never a system face.
	struct face
	{
		std::vector<u8> bytes;
		stbtt_fontinfo info{};
		bool ok = false;
	};
	inline const face& bundled_face(const std::string& file)
	{
		static std::mutex mutex;
		static std::map<std::string, std::unique_ptr<face>> faces;
		std::lock_guard lock(mutex);
		auto& slot = faces[file];
		if (!slot)
		{
			slot = std::make_unique<face>();
			fs::file f(fs::get_executable_dir() + "/fonts/" + file, fs::read + fs::isfile);
			if (f)
				slot->bytes = f.to_vector<u8>();
			slot->ok = !slot->bytes.empty() &&
			           stbtt_InitFont(&slot->info, slot->bytes.data(), stbtt_GetFontOffsetForIndex(slot->bytes.data(), 0)) != 0;
		}
		return *slot;
	}

	// A run of words in one colour, `lead` canvas units after the run before it.
	struct run
	{
		std::u32string text;
		u32 rgb = 0xFFFFFF;
		f32 alpha = 1.f;
		f32 lead = 0.f;
	};

	// The look, in canvas units: the em size, the slant (x per unit of height above the baseline), the tracking (em), the
	// letters widened (`widen` times as wide) and thickened (grown by `embolden`), an outline around them, a soft shadow offset
	// from them and a glow around them (the letters grown by `glow_spread`, then blurred by `glow_blur`). Widening and thickening
	// make the bundled grotesk read as the heavy, wide italic the game letters its plates with.
	struct style
	{
		f32 em = 20.f, slant = 0.f, tracking = 0.f;
		f32 widen = 1.f, embolden = 0.f;
		f32 outline = 0.f;
		u32 outline_rgb = 0x000000;
		f32 outline_alpha = 1.f;
		f32 shadow_dx = 0.f, shadow_dy = 0.f, shadow_blur = 0.f, shadow_alpha = 0.f;
		u32 shadow_rgb = 0x000000;
		f32 glow_spread = 0.f, glow_blur = 0.f, glow_alpha = 0.f;
		u32 glow_rgb = 0x000000;
	};

	// A baked image and where its letters sit, in canvas units from the image's top-left: the pen's start, the baseline and the
	// capitals' top, and the advance of the words.
	struct baked
	{
		std::unique_ptr<raw_rgba> image;
		f32 width = 0.f, height = 0.f, origin_x = 0.f, baseline = 0.f, cap_top = 0.f, advance = 0.f;
	};

	namespace detail
	{
		// A separable box blur of radius r, run twice (close to a Gaussian).
		inline void blur(std::vector<f32>& values, int w, int h, int r)
		{
			if (r <= 0)
				return;
			std::vector<f32> tmp(values.size());
			for (int pass = 0; pass < 2; ++pass)
			{
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						f32 sum = 0.f;
						for (int k = -r; k <= r; ++k)
							sum += values[static_cast<usz>(y) * w + std::clamp(x + k, 0, w - 1)];
						tmp[static_cast<usz>(y) * w + x] = sum / static_cast<f32>(2 * r + 1);
					}
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						f32 sum = 0.f;
						for (int k = -r; k <= r; ++k)
							sum += tmp[static_cast<usz>(std::clamp(y + k, 0, h - 1)) * w + x];
						values[static_cast<usz>(y) * w + x] = sum / static_cast<f32>(2 * r + 1);
					}
			}
		}

		// The letters grown by a disc of radius r (in pixels, fractional edges soft).
		inline std::vector<f32> dilate(const std::vector<f32>& cover, int w, int h, f32 r)
		{
			if (r <= 0.f)
				return cover;
			std::vector<f32> out(cover.size(), 0.f);
			const int reach = static_cast<int>(std::ceil(r));
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					f32 best = 0.f;
					for (int dy = -reach; dy <= reach && best < 1.f; ++dy)
					{
						const int sy = y + dy;
						if (sy < 0 || sy >= h)
							continue;
						for (int dx = -reach; dx <= reach; ++dx)
						{
							const int sx = x + dx;
							if (sx < 0 || sx >= w)
								continue;
							const f32 d = std::sqrt(static_cast<f32>(dx * dx + dy * dy));
							const f32 reach_weight = std::clamp(r + .5f - d, 0.f, 1.f);
							best = std::max(best, cover[static_cast<usz>(sy) * w + sx] * reach_weight);
						}
					}
					out[static_cast<usz>(y) * w + x] = best;
				}
			return out;
		}

		// The letters thickened by a disc of radius r, each grown pixel taking the run of the letter it grew from, so a run in
		// a second colour keeps its colour to its new edge.
		inline void thicken(std::vector<f32>& cover, std::vector<u8>& owner, int w, int h, f32 r)
		{
			if (r <= 0.f)
				return;
			std::vector<f32> grown(cover.size(), 0.f);
			std::vector<u8> grown_owner(owner.size(), 0);
			const int reach = static_cast<int>(std::ceil(r));
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					f32 best = 0.f;
					u8 from = owner[static_cast<usz>(y) * w + x];
					for (int dy = -reach; dy <= reach && best < 1.f; ++dy)
					{
						const int sy = y + dy;
						if (sy < 0 || sy >= h)
							continue;
						for (int dx = -reach; dx <= reach; ++dx)
						{
							const int sx = x + dx;
							if (sx < 0 || sx >= w)
								continue;
							const f32 d = std::sqrt(static_cast<f32>(dx * dx + dy * dy));
							const usz at = static_cast<usz>(sy) * w + sx;
							const f32 value = cover[at] * std::clamp(r + .5f - d, 0.f, 1.f);
							if (value > best)
							{
								best = value;
								from = owner[at];
							}
						}
					}
					grown[static_cast<usz>(y) * w + x] = best;
					grown_owner[static_cast<usz>(y) * w + x] = from;
				}
			cover = std::move(grown);
			owner = std::move(grown_owner);
		}
	} // namespace detail

	// Sets `runs` in `f` with `look`. Null when the face did not load or the words are empty.
	inline baked bake(const face& f, const std::vector<run>& runs, const style& look)
	{
		baked out;
		if (!f.ok || runs.empty())
			return out;
		const f32 px = look.em * bake_scale;
		const f32 scale = stbtt_ScaleForMappingEmToPixels(&f.info, px);
		const f32 scale_x = scale * std::max(.5f, look.widen);
		int ascent = 0, descent = 0, gap = 0;
		stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
		int cap_x0 = 0, cap_y0 = 0, cap_x1 = 0, cap_y1 = 0;
		stbtt_GetCodepointBox(&f.info, 'H', &cap_x0, &cap_y0, &cap_x1, &cap_y1);
		const f32 asc = ascent * scale, desc = -descent * scale, cap = cap_y1 * scale;
		// The pen positions of every letter.
		struct placed
		{
			char32_t c;
			f32 x;
			usz run;
		};
		std::vector<placed> letters;
		f32 pen = 0.f;
		char32_t previous = 0;
		for (usz r = 0; r < runs.size(); ++r)
		{
			if (!letters.empty())
				pen += runs[r].lead * bake_scale;
			for (const char32_t c : runs[r].text)
			{
				if (previous)
					pen += stbtt_GetCodepointKernAdvance(&f.info, static_cast<int>(previous), static_cast<int>(c)) * scale_x;
				letters.push_back({c, pen, r});
				int advance = 0, bearing = 0;
				stbtt_GetCodepointHMetrics(&f.info, static_cast<int>(c), &advance, &bearing);
				pen += advance * scale_x + look.tracking * px;
				previous = c;
			}
		}
		if (letters.empty())
			return out;
		const f32 advance = pen - look.tracking * px;
		const f32 slant_room = look.slant * asc;
		const f32 shadow_room = std::max(std::abs(look.shadow_dx), std::abs(look.shadow_dy)) * bake_scale + look.shadow_blur * bake_scale * 2.f;
		const f32 grown = (look.embolden + look.outline) * bake_scale;
		const f32 pad = std::ceil(grown + std::max((look.glow_spread + 2.f * look.glow_blur) * bake_scale, shadow_room) + 2.f);
		const int w = static_cast<int>(std::ceil(advance + slant_room + 2.f * pad));
		const int h = static_cast<int>(std::ceil(asc + desc + 2.f * pad));
		const f32 base = pad + asc, start = pad;
		std::vector<f32> cover(static_cast<usz>(w) * h, 0.f);
		std::vector<u8> owner(static_cast<usz>(w) * h, 0);
		std::vector<u8> glyph;
		for (const auto& letter : letters)
		{
			const f32 x = start + letter.x;
			const f32 shift = x - std::floor(x);
			int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			stbtt_GetCodepointBitmapBoxSubpixel(&f.info, static_cast<int>(letter.c), scale_x, scale, shift, 0.f, &x0, &y0, &x1, &y1);
			const int gw = x1 - x0, gh = y1 - y0;
			if (gw <= 0 || gh <= 0)
				continue;
			glyph.assign(static_cast<usz>(gw) * gh, 0);
			stbtt_MakeCodepointBitmapSubpixel(&f.info, glyph.data(), gw, gh, gw, scale_x, scale, shift, 0.f, static_cast<int>(letter.c));
			for (int gy = 0; gy < gh; ++gy)
			{
				const f32 y = base + static_cast<f32>(y0 + gy);
				const int row = static_cast<int>(y);
				if (row < 0 || row >= h)
					continue;
				// The slant shears each row by its height above the baseline, spread over two pixels.
				const f32 lean = (base - y - .5f) * look.slant;
				const f32 left = std::floor(x) + static_cast<f32>(x0) + lean;
				const int whole = static_cast<int>(std::floor(left));
				const f32 frac = left - static_cast<f32>(whole);
				for (int gx = 0; gx < gw; ++gx)
				{
					const f32 value = glyph[static_cast<usz>(gy) * gw + gx] / 255.f;
					if (value <= 0.f)
						continue;
					for (int k = 0; k < 2; ++k)
					{
						const int px_x = whole + gx + k;
						if (px_x < 0 || px_x >= w)
							continue;
						const f32 part = value * (k ? frac : 1.f - frac);
						auto& at = cover[static_cast<usz>(row) * w + px_x];
						if (part > 0.f)
							owner[static_cast<usz>(row) * w + px_x] = static_cast<u8>(letter.run);
						at = std::min(1.f, at + part);
					}
				}
			}
		}
		detail::thicken(cover, owner, w, h, look.embolden * bake_scale);
		const auto outline = detail::dilate(cover, w, h, look.outline * bake_scale);
		std::vector<f32> glow;
		if ((look.glow_spread > 0.f || look.glow_blur > 0.f) && look.glow_alpha > 0.f)
		{
			glow = detail::dilate(cover, w, h, look.glow_spread * bake_scale);
			detail::blur(glow, w, h, static_cast<int>(std::round(look.glow_blur * bake_scale)));
		}
		std::vector<f32> shadow;
		if (look.shadow_alpha > 0.f)
		{
			shadow.assign(cover.size(), 0.f);
			const int dx = static_cast<int>(std::round(look.shadow_dx * bake_scale)), dy = static_cast<int>(std::round(look.shadow_dy * bake_scale));
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const int sx = x - dx, sy = y - dy;
					if (sx >= 0 && sx < w && sy >= 0 && sy < h)
						shadow[static_cast<usz>(y) * w + x] = outline[static_cast<usz>(sy) * w + sx];
				}
			detail::blur(shadow, w, h, static_cast<int>(std::round(look.shadow_blur * bake_scale)));
		}
		// Bottom to top: the shadow, the glow, the outline, the letters. Straight alpha out.
		out.image = std::make_unique<raw_rgba>(w, h);
		auto& pixels = out.image->pixels;
		const auto layer_over = [](f32(&dst)[4], color4f src, f32 a)
		{
			if (a <= 0.f)
				return;
			const f32 keep = 1.f - a;
			dst[0] = src.r * a + dst[0] * keep;
			dst[1] = src.g * a + dst[1] * keep;
			dst[2] = src.b * a + dst[2] * keep;
			dst[3] = a + dst[3] * keep;
		};
		for (usz i = 0; i < cover.size(); ++i)
		{
			f32 px_value[4]{}; // premultiplied
			if (!shadow.empty())
				layer_over(px_value, rgb(look.shadow_rgb), shadow[i] * look.shadow_alpha);
			if (!glow.empty())
				layer_over(px_value, rgb(look.glow_rgb), std::min(1.f, glow[i] * 1.6f) * look.glow_alpha);
			if (look.outline > 0.f)
				layer_over(px_value, rgb(look.outline_rgb), outline[i] * look.outline_alpha);
			const auto& ink = runs[owner[i]];
			layer_over(px_value, rgb(ink.rgb), cover[i] * ink.alpha);
			const f32 a = px_value[3];
			u8* p = pixels.data() + i * 4;
			for (int c = 0; c < 3; ++c)
				p[c] = static_cast<u8>(std::clamp(a > 0.f ? px_value[c] / a : 0.f, 0.f, 1.f) * 255.f + .5f);
			p[3] = static_cast<u8>(std::clamp(a, 0.f, 1.f) * 255.f + .5f);
		}
		out.image->dirty = true;
		out.width = static_cast<f32>(w) / bake_scale;
		out.height = static_cast<f32>(h) / bake_scale;
		out.origin_x = start / bake_scale;
		out.baseline = base / bake_scale;
		out.cap_top = (base - cap) / bake_scale;
		out.advance = advance / bake_scale;
		return out;
	}

	// The baked images by key, kept for the process (see the header); built on first use.
	inline const baked& cached(const std::string& key, const std::function<baked()>& make)
	{
		static std::mutex mutex;
		static std::map<std::string, std::unique_ptr<baked>> kept;
		std::lock_guard lock(mutex);
		auto& slot = kept[key];
		if (!slot)
			slot = std::make_unique<baked>(make());
		return *slot;
	}

	// Draws a baked image with its capitals centred on (centre_x, centre_y); returns false when there is none.
	inline bool draw_centred(layer& out, const baked& words, f32 centre_x, f32 centre_y, f32 scale = 1.f, f32 alpha = 1.f)
	{
		if (!words.image)
			return false;
		const f32 caps_mid = (words.cap_top + words.baseline) / 2.f;
		const f32 x = centre_x - (words.origin_x + words.advance / 2.f) * scale;
		const f32 y = centre_y - caps_mid * scale;
		out.image(layer::b_sprites, words.image.get(), rgb(0xFFFFFF, alpha), x, y, x + words.width * scale, y + words.height * scale);
		return true;
	}
} // namespace rsx::overlays::pbd::lettering
