#pragma once

// Batched drawing for the PROJECT BACKDASH overlays (Practice, the lobby panels and the bands over the game). Each layer collects quads into a few draw commands: one per
// colour for plain rectangles, one per font and colour for text, one per image and tint for sprites and gradients. The
// glyph atlas and gradient images are built once per process from data compiled into the executable, so isolated
// Practice profiles need no icon files and the renderer's image cache (keyed by address) never sees a stale pointer.

#include "Emu/RSX/Overlays/overlay_controls.h"
#include "pbd_glyph_atlas.h"
#include "Emu/PBD/pbd_controller_display.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rsx::overlays::pbd
{
	inline color4f rgb(u32 value, f32 alpha = 1.f)
	{
		return {((value >> 16) & 0xff) / 255.f, ((value >> 8) & 0xff) / 255.f, (value & 0xff) / 255.f, alpha};
	}

	inline color4f faded(color4f c, f32 alpha)
	{
		return {c.r, c.g, c.b, c.a * alpha};
	}

	// Palette (the launcher's colour tokens, so in-game panels match it). Runtime constants rather than constexpr: color4f keeps its
	// channels in an anonymous struct inside an anonymous union, which compilers do not all accept in constant evaluation.
	inline const color4f text_1 = rgb(0xE9EEED);
	inline const color4f text_2 = rgb(0xAEBDBD);
	inline const color4f text_3 = rgb(0x8A9C9C);
	inline const color4f white = rgb(0xFFFFFF);
	inline const color4f aqua = rgb(0x8FC3CA);
	inline const color4f gold = rgb(0xF4B63F);
	inline const color4f crimson = rgb(0xE3304F);
	inline const color4f stripe = rgb(0xEB5470);
	inline const color4f edge_dark = rgb(0x5A0E18);
	inline const color4f band_fill = rgb(0x0B0304, .84f);
	inline const color4f hairline = rgb(0xE9EEED, .08f);
	inline const color4f top_edge = rgb(0xE9EEED, .14f);
	inline const color4f row_bar = rgb(0xE9EEED, .04f);
	inline const color4f separator = rgb(0xE9EEED, .18f);

	// Font sizes are points; the overlay rasterises ceil(pt x 4/3) px over the font's ascent+descent (1.2 em for both
	// families), so these give the target em sizes: Barlow 16 / 14 / 15 and PBD Display 20 / 24.
	inline constexpr const char* display_font = "PBDDisplay.otf";
	inline constexpr const char* label_font = "Barlow-600.ttf";
	inline constexpr const char* body_font = "Barlow-400.ttf";
	inline constexpr int title_pt = 18, timer_pt = 21, label_pt = 14, caption_pt = 13, body_pt = 13;

	struct fonts
	{
		font* title = nullptr;   // PBD Display 20
		font* timer = nullptr;   // PBD Display 24
		font* label = nullptr;   // Barlow 600 16
		font* caption = nullptr; // Barlow 600 14
		font* body = nullptr;    // Barlow 400 15
	};

	// Loads the fixed set of sizes and packs their Latin page now, so the first menu open does not pack glyph atlases
	// on the present thread.
	inline fonts preload_fonts()
	{
		fonts out;
		out.title = fontmgr::get(display_font, title_pt);
		out.timer = fontmgr::get(display_font, timer_pt);
		out.label = fontmgr::get(label_font, label_pt);
		out.caption = fontmgr::get(label_font, caption_pt);
		out.body = fontmgr::get(body_font, body_pt);
		for (font* f : {out.title, out.timer, out.label, out.caption, out.body})
		{
			f32 x = 0.f, y = 0.f;
			f->get_char(U'A', x, y);
			// The chat ellipsis and the spectator en dash live on page 0x20; packing it here keeps the first cut chat
			// line or spectator band from growing the glyph texture mid-match.
			f->get_char(char32_t{0x2026}, x, y);
		}
		return out;
	}

	// Baseline that centres the cap height on a line centre (cap height is 0.583 of the rasterised pixel size).
	inline f32 baseline(const font* f, f32 centre_y)
	{
		return centre_y + f->get_size_px() * .2917f;
	}

	inline f32 text_width(font* f, std::u32string_view text)
	{
		f32 x = 0.f, y = 0.f;
		for (const char32_t c : text)
			f->get_char(c, x, y);
		return x;
	}

	struct raw_rgba final : image_info_base
	{
		std::vector<u8> pixels;

		raw_rgba(int width, int height)
		{
			w = width;
			h = height;
			channels = 4;
			bpp = 4;
			pixels.resize(static_cast<usz>(width) * height * 4);
		}

		const u8* get_data() const override
		{
			return pixels.data();
		}

		struct stop
		{
			f32 at;
			u32 color;
			f32 alpha;
		};

		// Linear interpolation in sRGB values along the image's long axis.
		void gradient(std::initializer_list<stop> stops)
		{
			const int n = std::max(w, h);
			const std::vector<stop> list(stops);
			for (int i = 0; i < n; i++)
			{
				const f32 t = n > 1 ? static_cast<f32>(i) / (n - 1) : 0.f;
				usz k = 0;
				while (k + 2 < list.size() && t > list[k + 1].at)
					k++;
				const auto& a = list[k];
				const auto& b = list[std::min(k + 1, list.size() - 1)];
				const f32 span = b.at - a.at;
				const f32 f = span > 0.f ? std::clamp((t - a.at) / span, 0.f, 1.f) : 0.f;
				const color4f ca = rgb(a.color, a.alpha), cb = rgb(b.color, b.alpha);
				const color4f c = {ca.r + (cb.r - ca.r) * f, ca.g + (cb.g - ca.g) * f, ca.b + (cb.b - ca.b) * f, ca.a + (cb.a - ca.a) * f};
				u8* p = pixels.data() + static_cast<usz>(i) * 4;
				p[0] = static_cast<u8>(c.r * 255.f + .5f);
				p[1] = static_cast<u8>(c.g * 255.f + .5f);
				p[2] = static_cast<u8>(c.b * 255.f + .5f);
				p[3] = static_cast<u8>(c.a * 255.f + .5f);
			}
			dirty = true;
		}
	};

	struct assets
	{
		raw_rgba panel{1, 256};     // menu panel, dialogs and the feedback panel (tinted .62)
		raw_rgba dropdown{1, 128};  // dropdown option stack
		raw_rgba focus{256, 1};     // crimson focus bar
		raw_rgba glow{1, 16};       // crimson glow ramp, 0 at the top
		raw_rgba scrim{1, 92};      // bottom scrim under the control bar
		raw_rgba ramp{72, 1};       // white 0 -> 1, the 3-slice source for top-centre bands
		raw_rgba gold_wash{256, 1}; // a selected Button Layout row's wash, fading out to the right
		raw_rgba gold_rule{256, 1}; // and the rule under it
		std::array<std::unique_ptr<image_info>, pbd_glyphs::atlases.size()> atlases{};

		assets()
		{
			panel.gradient({{0.f, 0x0B0304, 1.f}, {.52f, 0x110407, 1.f}, {.86f, 0x22070D, 1.f}, {1.f, 0x2F0810, 1.f}});
			dropdown.gradient({{0.f, 0x170609, 1.f}, {.6f, 0x1E070C, 1.f}, {1.f, 0x2A0910, 1.f}});
			focus.gradient({{0.f, 0xCC2847, 1.f}, {.42f, 0xB42343, 1.f}, {.58f, 0xD23A58, 1.f}, {.66f, 0xC0304F, 1.f}, {.84f, 0x8E1B33, 1.f}, {1.f, 0x5E1224, 1.f}});
			glow.gradient({{0.f, 0xE3304F, 0.f}, {1.f, 0xE3304F, .25f}});
			scrim.gradient({{0.f, 0x0B0304, 0.f}, {64.f / 91.f, 0x0B0304, .66f}, {1.f, 0x0B0304, .74f}});
			ramp.gradient({{0.f, 0xFFFFFF, 0.f}, {1.f, 0xFFFFFF, 1.f}});
			gold_wash.gradient({{0.f, 0xF4B63F, .14f}, {.38f, 0xF4B63F, .05f}, {.72f, 0xF4B63F, 0.f}, {1.f, 0xF4B63F, 0.f}});
			gold_rule.gradient({{0.f, 0xF4B63F, 1.f}, {.3f, 0xF4B63F, .62f}, {1.f, 0xF4B63F, .14f}});
			for (usz i = 0; i < atlases.size(); i++)
			{
				const auto& desc = pbd_glyphs::atlases[i];
				atlases[i] = std::make_unique<image_info>(std::vector<u8>(desc.png, desc.png + desc.png_size));
			}
		}

		static assets& get()
		{
			// Process lifetime: overlays and the renderer's per-address image cache may outlive any one overlay.
			static assets* instance = new assets();
			return *instance;
		}
	};

	// The atlas drawn for the current output: the smallest one at or above the output scale (canvas units -> pixels).
	struct atlas_ref
	{
		const image_info* image = nullptr;
		const pbd_glyphs::uv* uvs = nullptr;
		bool xbox = false;
		// The controller whose buttons the glyphs show; otherwise the current local controller.
		const ::pbd::controllers::display* shown = nullptr;

		static atlas_ref pick(f32 output_scale, bool xbox)
		{
			auto& store = assets::get();
			usz chosen = pbd_glyphs::atlases.size() - 1;
			for (usz i = 0; i < pbd_glyphs::atlases.size(); i++)
			{
				if (pbd_glyphs::atlases[i].scale + .01f >= output_scale)
				{
					chosen = i;
					break;
				}
			}
			return {store.atlases[chosen].get(), pbd_glyphs::atlases[chosen].uvs, xbox};
		}
	};

	inline int find_sprite(std::string_view name)
	{
		for (usz i = 0; i < pbd_glyphs::names.size(); i++)
			if (name == pbd_glyphs::names[i])
				return static_cast<int>(i);
		return -1;
	}

	// Token names follow the PlayStation pad ("cross", "l1", "select", "dpad:udlr"); the Xbox family swaps the plates, and with
	// `rings` draws its face buttons as their coloured rings (the Button Layout's).
	inline int glyph_sprite(std::string_view token, bool xbox, bool rings = false)
	{
		if (token.starts_with("dpad:"))
			return find_sprite("dpad_" + std::string(token.substr(5)));
		if (xbox && rings)
		{
			static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> ringed{
				{{"cross", "xbox_a_ring"}, {"circle", "xbox_b_ring"}, {"square", "xbox_x_ring"}, {"triangle", "xbox_y_ring"}}};
			for (const auto& [from, to] : ringed)
				if (token == from)
					return find_sprite(to);
		}
		if (xbox)
		{
			static constexpr std::array<std::pair<std::string_view, std::string_view>, 12> swap{{{"cross", "xbox_a"}, {"circle", "xbox_b"}, {"square", "xbox_x"}, {"triangle", "xbox_y"},
				{"l1", "xbox_lb"}, {"r1", "xbox_rb"}, {"l2", "xbox_lt"}, {"r2", "xbox_rt"},
				{"l3", "xbox_ls"}, {"r3", "xbox_rs"}, {"select", "xbox_view"}, {"start", "xbox_menu"}}};
			for (const auto& [from, to] : swap)
				if (token == from)
					return find_sprite(to);
		}
		return find_sprite(token);
	}

	class layer
	{
	public:
		// Draw order inside a layer, bottom to top.
		enum bucket : u8
		{
			b_under,
			b_bars,
			b_focus,
			b_lines,
			b_text,
			b_sprites,
			bucket_count
		};

		void rect(bucket b, color4f c, f32 x, f32 y, f32 w, f32 h)
		{
			if (w <= 0.f || h <= 0.f || c.a <= 0.f)
				return;
			push(find(b, image_resource_id::none, nullptr, nullptr, c), x, y, x + w, y + h, 0.f, 0.f, 1.f, 1.f);
		}

		void image(bucket b, const image_info_base* img, color4f tint, f32 x0, f32 y0, f32 x1, f32 y1,
			f32 u0 = 0.f, f32 v0 = 0.f, f32 u1 = 1.f, f32 v1 = 1.f, f32 slant = 0.f)
		{
			if (!img || x1 <= x0 || y1 <= y0 || tint.a <= 0.f)
				return;
			auto& cmd = find(b, image_resource_id::raw_image, img, nullptr, tint);
			const usz first = cmd.verts.size();
			push(cmd, x0, y0, x1, y1, u0, v0, u1, v1);
			for (usz i = first; i < cmd.verts.size(); ++i)
				cmd.verts[i].x() += (y1 - cmd.verts[i].y()) * slant;
		}

		// A sprite at its own size scaled to height h (0 keeps the atlas size). Returns the drawn width.
		f32 sprite(const atlas_ref& atlas, int id, f32 x, f32 y, f32 h = 0.f, color4f tint = white)
		{
			if (id < 0 || id >= pbd_glyphs::sprite_count || !atlas.image)
				return 0.f;
			const auto size = pbd_glyphs::sizes[id];
			const f32 height = h > 0.f ? h : size.h;
			const f32 width = size.w * height / size.h;
			const auto& uv = atlas.uvs[id];
			image(b_sprites, atlas.image, tint, x, y, x + width, y + height, uv.u0, uv.v0, uv.u1, uv.v1);
			return width;
		}

		// Text on a baseline; returns the advance.
		f32 text(font* f, color4f c, f32 x, f32 base, std::u32string_view s, f32 scale = 1.f, f32 slant = 0.f)
		{
			if (!f || s.empty())
				return 0.f;
			// render_text drops spaces at the start of a line, so a segment such as " restarts here" after a glyph
			// would sit flush against it. Advance past the leading spaces here and render the rest.
			usz lead = 0;
			while (lead < s.size() && s[lead] == U' ')
				lead++;
			const f32 full = text_width(f, s) * scale;
			if (lead == s.size())
				return full;
			x += text_width(f, s.substr(0, lead)) * scale;
			const std::u32string copy(s.substr(lead));
			auto verts = f->render_text(copy.c_str());
			if (c.a > 0.f && !verts.empty())
			{
				auto& cmd = find(b_text, image_resource_id::font_file, nullptr, f, c);
				for (auto& v : verts)
				{
					v.x() = x + (v.x() - v.y() * slant) * scale;
					v.y() = base + v.y() * scale;
				}
				cmd.verts.insert(cmd.verts.end(), verts.begin(), verts.end());
			}
			return full;
		}

		// Text as text() draws it, cut to the canvas rows y0..y1: each glyph quad is clipped at the band's edges with its texture
		// rows and its slanted sides interpolated, so a word can be drawn in horizontal bands of different colours (the notices'
		// chrome headlines).
		void text_band(font* f, color4f c, f32 x, f32 base, std::u32string_view s, f32 scale, f32 slant, f32 y0, f32 y1)
		{
			if (!f || s.empty() || c.a <= 0.f || y1 <= y0)
				return;
			usz lead = 0;
			while (lead < s.size() && s[lead] == U' ')
				lead++;
			if (lead == s.size())
				return;
			x += text_width(f, s.substr(0, lead)) * scale;
			const std::u32string copy(s.substr(lead));
			auto verts = f->render_text(copy.c_str());
			if (verts.empty())
				return;
			auto& cmd = find(b_text, image_resource_id::font_file, nullptr, f, c);
			for (usz q = 0; q + 3 < verts.size(); q += 4)
			{
				std::array<vertex, 4> quad{verts[q], verts[q + 1], verts[q + 2], verts[q + 3]};
				for (auto& v : quad)
				{
					v.x() = x + (v.x() - v.y() * slant) * scale;
					v.y() = base + v.y() * scale;
				}
				const f32 top = quad[0].y(), bottom = quad[2].y();
				const f32 cut_top = std::max(top, y0), cut_bottom = std::min(bottom, y1);
				if (bottom <= top || cut_bottom <= cut_top)
					continue;
				const auto at = [&](vertex upper, vertex lower, f32 y)
				{
					const f32 t = (y - top) / (bottom - top);
					vertex v = upper;
					v.x() = upper.x() + (lower.x() - upper.x()) * t;
					v.y() = y;
					v.w() = upper.w() + (lower.w() - upper.w()) * t;
					return v;
				};
				cmd.verts.push_back(at(quad[0], quad[2], cut_top));
				cmd.verts.push_back(at(quad[1], quad[3], cut_top));
				cmd.verts.push_back(at(quad[0], quad[2], cut_bottom));
				cmd.verts.push_back(at(quad[1], quad[3], cut_bottom));
			}
		}

		void flush(compiled_resource& out)
		{
			for (auto& list : m_buckets)
				for (auto& cmd : list)
					if (!cmd.verts.empty())
						out.append(cmd);
		}

	private:
		std::array<std::vector<compiled_resource::command>, bucket_count> m_buckets{};

		compiled_resource::command& find(bucket b, u8 texture, const void* external, font* f, color4f c)
		{
			for (auto& cmd : m_buckets[b])
			{
				if (cmd.config.texture_ref == texture && cmd.config.external_data_ref == external && cmd.config.font_ref == f && cmd.config.color == c)
					return cmd;
			}
			compiled_resource::command cmd;
			if (f)
				cmd.config.set_font(f);
			else
				cmd.config.set_image_resource(texture);
			cmd.config.external_data_ref = external;
			cmd.config.color = c;
			m_buckets[b].push_back(std::move(cmd));
			return m_buckets[b].back();
		}

		static void push(compiled_resource::command& cmd, f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1)
		{
			cmd.verts.emplace_back(x0, y0, u0, v0);
			cmd.verts.emplace_back(x1, y0, u1, v0);
			cmd.verts.emplace_back(x0, y1, u0, v1);
			cmd.verts.emplace_back(x1, y1, u1, v1);
		}
	};

} // namespace rsx::overlays::pbd
