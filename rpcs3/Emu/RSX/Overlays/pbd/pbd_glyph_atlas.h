#pragma once

#include "pbd_glyph_atlas_x1.h"
#include "pbd_glyph_atlas_x1_5.h"
#include "pbd_glyph_atlas_x2.h"
#include "pbd_glyph_atlas_x3.h"

namespace pbd_glyphs
{
	inline constexpr std::array<atlas, 4> atlases{{
		{1.0f, 256, 116, png_x1, sizeof(png_x1), uvs_x1},
		{1.5f, 256, 248, png_x1_5, sizeof(png_x1_5), uvs_x1_5},
		{2.0f, 512, 212, png_x2, sizeof(png_x2), uvs_x2},
		{3.0f, 512, 420, png_x3, sizeof(png_x3), uvs_x3},
	}};
} // namespace pbd_glyphs
