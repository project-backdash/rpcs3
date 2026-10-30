#!/usr/bin/env python3
"""Rasterise the Practice UI glyph atlas and write it as a C++ header.

The shapes follow the Practice UI design's vector masters (arrows, button clusters, controller glyphs), redrawn here
with Pillow at 8x supersampling and box-filtered down, one atlas per output scale (x1, x1.5, x2, x3).
The PNG bytes are compiled into the executable, so isolated Practice profiles need no icon files.

Usage: python tools/pbd_practice_glyphs.py [--preview DIR]
Writes rpcs3/Emu/RSX/Overlays/pbd/pbd_glyph_atlas.h relative to the repository root.
"""

import argparse
import io
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(ROOT, "rpcs3", "Emu", "RSX", "Overlays", "PbdFonts")
OUT = os.path.join(ROOT, "rpcs3", "Emu", "RSX", "Overlays", "pbd", "pbd_glyph_atlas.h")
SS = 8
SCALES = [1.0, 1.5, 2.0, 3.0]
# Each generated output scale stays in one data header.
PNG_BYTES_PER_LINE = 256

FILL = (0xE9, 0xEE, 0xED, 255)
INK = (0x14, 0x19, 0x1B, 255)
HALO = (0x0B, 0x03, 0x04, 178)
DPAD_OFF = (0x47, 0x50, 0x52, 255)
DPAD_HUB = (0x2A, 0x31, 0x34, 255)
IDLE_DOT = (0x4E, 0x56, 0x58, 153)
PLUS = (0xAE, 0xBD, 0xBD, 255)
SLASH = (0x8A, 0x9C, 0x9C, 255)
CRIMSON = (0xE3, 0x30, 0x4F, 255)
WHITE = (255, 255, 255, 255)


class Canvas:
    """A supersampled drawing surface addressed in the sprite's art units."""

    def __init__(self, cell_w, cell_h, scale, art_w=None, art_h=None, art_x=0.0, art_y=0.0, art_scale=1.0):
        # cell_w/h: display units; art coordinates map to display units through art_scale and an offset.
        self.px_w = max(1, math.ceil(cell_w * scale))
        self.px_h = max(1, math.ceil(cell_h * scale))
        self.k = SS * scale * art_scale
        self.ox = art_x * SS * scale
        self.oy = art_y * SS * scale
        self.img = Image.new("RGBA", (self.px_w * SS, self.px_h * SS), (0, 0, 0, 0))
        self.draw = ImageDraw.Draw(self.img)

    def p(self, x, y):
        return (self.ox + x * self.k, self.oy + y * self.k)

    def pts(self, points):
        return [self.p(x, y) for x, y in points]

    def poly(self, points, fill):
        self.draw.polygon(self.pts(points), fill=fill)

    def stroke(self, points, width, color, closed=False):
        pts = self.pts(points)
        if closed:
            pts = pts + [pts[0]]
        w = max(1, round(width * self.k))
        self.draw.line(pts, fill=color, width=w, joint="curve")
        r = w / 2
        for x, y in (pts if closed else [pts[0], pts[-1]]):
            self.draw.ellipse((x - r, y - r, x + r, y + r), fill=color)

    def circle(self, cx, cy, r, fill=None, outline=None, width=0.0):
        # SVG strokes are centred on the radius; draw the outer disc in the stroke colour, then the inner fill.
        x, y = self.p(cx, cy)
        if outline is not None and width > 0:
            ro = (r + width / 2) * self.k
            self.draw.ellipse((x - ro, y - ro, x + ro, y + ro), fill=outline)
            ri = (r - width / 2) * self.k
            if fill is not None and ri > 0:
                self.draw.ellipse((x - ri, y - ri, x + ri, y + ri), fill=fill)
            elif fill is None and ri > 0:
                self.draw.ellipse((x - ri, y - ri, x + ri, y + ri), fill=(0, 0, 0, 0))
        elif fill is not None:
            rr = r * self.k
            self.draw.ellipse((x - rr, y - rr, x + rr, y + rr), fill=fill)

    def rect(self, x, y, w, h, fill):
        x0, y0 = self.p(x, y)
        x1, y1 = self.p(x + w, y + h)
        self.draw.rectangle((x0, y0, x1 - 1, y1 - 1), fill=fill)

    def text(self, x, y, s, size, color, weight="600"):
        font = ImageFont.truetype(os.path.join(FONT_DIR, f"Barlow-{weight}.ttf"), max(1, round(size * self.k)))
        self.draw.text(self.p(x, y), s, font=font, fill=color, anchor="ms")

    def result(self):
        small = self.img.convert("RGBa").reduce(SS).convert("RGBA")
        return bleed(small)


def bleed(img):
    """Give fully transparent texels the colour of their neighbours so bilinear sampling never darkens edges."""
    alpha = img.getchannel("A")
    prem = img.convert("RGBa").filter(ImageFilter.GaussianBlur(2))
    spread = prem.convert("RGBA")
    mask = alpha.point(lambda a: 255 if a > 0 else 0)
    out = Image.composite(img, spread, mask)
    out.putalpha(alpha)
    return out


# ---------------------------------------------------------------- sprite art (coordinates from the design's SVG masters)
ARROWS = {
    "u": [(12, 0), (24, 12), (24, 13), (16, 13), (16, 24), (8, 24), (8, 13), (0, 13), (0, 12)],
    "d": [(12, 24), (0, 12), (0, 11), (8, 11), (8, 0), (16, 0), (16, 11), (24, 11), (24, 12)],
    "l": [(0, 12), (12, 0), (13, 0), (13, 8), (24, 8), (24, 16), (13, 16), (13, 24), (12, 24)],
    "r": [(24, 12), (12, 24), (11, 24), (11, 16), (0, 16), (0, 8), (11, 8), (11, 0), (12, 0)],
    "ul": [(2, 2), (21, 2), (21.7917, 2.7917), (15.4583, 9.125), (24, 17.6667), (17.6667, 24), (9.125, 15.4583), (2.7917, 21.7917), (2, 21)],
    "ur": [(22, 2), (22, 21), (21.2083, 21.7917), (14.875, 15.4583), (6.3333, 24), (0, 17.6667), (8.5417, 9.125), (2.2083, 2.7917), (3, 2)],
    "dl": [(2, 22), (2, 3), (2.7917, 2.2083), (9.125, 8.5417), (17.6667, 0), (24, 6.3333), (15.4583, 14.875), (21.7917, 21.2083), (21, 22)],
    "dr": [(22, 22), (3, 22), (2.2083, 21.2083), (8.5417, 14.875), (0, 6.3333), (6.3333, 0), (14.875, 8.5417), (21.2083, 2.2083), (22, 3)],
    "n": [(8, 9), (10, 8), (15, 8), (16, 8.5), (16, 15), (14, 16), (9, 16), (8, 15.5)],
}


def draw_arrow(scale, key):
    # 16-unit art box inside an 18-unit cell; the 3-art-unit stroke gives a 1-unit dark line inside and a 1-unit halo outside.
    c = Canvas(18, 18, scale, art_x=1, art_y=1, art_scale=16 / 24)
    w, h = c.img.size
    shape = Image.new("L", (w, h), 0)
    ImageDraw.Draw(shape).polygon(c.pts(ARROWS[key]), fill=255)
    band = Image.new("L", (w, h), 0)
    bd = ImageDraw.Draw(band)
    pts = c.pts(ARROWS[key])
    width = max(1, round(3 * c.k))
    bd.line(pts + [pts[0]], fill=255, width=width, joint="curve")
    r = width / 2
    for x, y in pts:
        bd.ellipse((x - r, y - r, x + r, y + r), fill=255)
    halo = ImageChops.subtract(band, shape)
    line = ImageChops.multiply(band, shape)
    body = ImageChops.subtract(shape, band)
    for color, mask in ((HALO, halo), (INK, line), (FILL, body)):
        layer = Image.new("RGBA", (w, h), color)
        c.img.paste(layer, (0, 0), mask)
    return c.result()


def draw_cluster(scale, mask):
    # Diamond of attack buttons: 1 left, 2 top, 3 bottom, 4 right, 5.1 units from the centre.
    c = Canvas(18, 18, scale, art_x=9, art_y=9)
    spots = [(-5.1, 0), (0, -5.1), (0, 5.1), (5.1, 0)]
    for i, (x, y) in enumerate(spots):
        if not mask & (1 << i):
            c.circle(x, y, 2.0, fill=IDLE_DOT)
    for i, (x, y) in enumerate(spots):
        if mask & (1 << i):
            c.circle(x, y, 3.1, fill=FILL, outline=INK, width=1.4)
    return c.result()


def disc(c):
    c.circle(10, 10, 9.55, fill=FILL, outline=INK, width=0.9)


def draw_face(scale, kind):
    c = Canvas(16, 16, scale, art_scale=0.8)
    disc(c)
    if kind == "cross":
        c.stroke([(6.5, 6.5), (13.5, 13.5)], 1.9, INK)
        c.stroke([(13.5, 6.5), (6.5, 13.5)], 1.9, INK)
    elif kind == "circle":
        c.circle(10, 10, 4.3, fill=FILL, outline=INK, width=1.8)
    elif kind == "triangle":
        c.stroke([(10, 5.6), (14.4, 13.3), (5.6, 13.3)], 1.6, INK, closed=True)
    elif kind == "square":
        c.stroke([(6.3, 6.3), (13.7, 6.3), (13.7, 13.7), (6.3, 13.7)], 1.7, INK, closed=True)
    elif kind in ("a", "b", "x", "y"):
        c.text(10, 14.4, kind.upper(), 12.5, INK)
    elif kind == "view":
        c.stroke([(5.5, 6.5), (11.5, 6.5), (11.5, 11.5), (5.5, 11.5)], 1.2, INK, closed=True)
        c.stroke([(8.5, 9), (14.5, 9), (14.5, 14), (8.5, 14)], 1.2, INK, closed=True)
    elif kind == "menu":
        for y in (6.6, 10, 13.4):
            c.stroke([(6, y), (14, y)], 1.4, INK)
    return c.result()


SHOULDERS = {
    "l1": ([(5.5, 2), (28.5, 2), (28.5, 18), (1.5, 18), (1.5, 6)], 15.6, 14.6, 12.5),
    "r1": ([(1.5, 2), (24.5, 2), (28.5, 6), (28.5, 18), (1.5, 18)], 14.4, 14.6, 12.5),
    "l2": ([(8, 1), (28.5, 1), (28.5, 19), (1.5, 19), (1.5, 7.5)], 16.2, 14.4, 11.5),
    "r2": ([(1.5, 1), (22, 1), (28.5, 7.5), (28.5, 19), (1.5, 19)], 13.8, 14.4, 11.5),
}


def draw_shoulder(scale, shape, label):
    c = Canvas(24, 16, scale, art_scale=0.8)
    points, tx, ty, size = SHOULDERS[shape]
    c.poly(points, FILL)
    c.stroke(points, 0.9, INK, closed=True)
    c.text(tx, ty, label, size, INK)
    return c.result()


def draw_stick(scale, label):
    c = Canvas(16, 16, scale, art_scale=0.8)
    disc(c)
    c.circle(10, 10, 8.15, fill=None, outline=INK, width=0.8)
    # The ring above cleared its inside; restore the body inside the ring before the name.
    c.circle(10, 10, 7.75, fill=FILL)
    c.text(10, 14.4, label, 12.5, INK)
    return c.result()


def draw_select(scale):
    c = Canvas(20, 16, scale, art_scale=0.8)
    c.rect(1.45, 4.45, 22.1, 11.1, INK)
    c.rect(1.9, 4.9, 21.2, 10.2, FILL)
    c.rect(6.5, 8.4, 12, 3.2, INK)
    return c.result()


def draw_start(scale):
    c = Canvas(16, 16, scale, art_scale=0.8)
    tri = [(4.6, 3.4), (16.6, 10), (4.6, 16.6)]
    c.poly(tri, FILL)
    c.stroke(tri, 0.9, INK, closed=True)
    return c.result()


def draw_dpad(scale, lit):
    c = Canvas(16, 16, scale, art_scale=0.8)
    arms = {
        "u": [(7.2, 7.2), (7.2, 3.4), (10, 0.6), (12.8, 3.4), (12.8, 7.2)],
        "d": [(7.2, 12.8), (7.2, 16.6), (10, 19.4), (12.8, 16.6), (12.8, 12.8)],
        "l": [(7.2, 7.2), (3.4, 7.2), (0.6, 10), (3.4, 12.8), (7.2, 12.8)],
        "r": [(12.8, 7.2), (16.6, 7.2), (19.4, 10), (16.6, 12.8), (12.8, 12.8)],
    }
    for key, pts in arms.items():
        c.poly(pts, FILL if key in lit else DPAD_OFF)
        c.stroke(pts, 0.8, INK, closed=True)
    hub = [(7.2, 7.2), (12.8, 7.2), (12.8, 12.8), (7.2, 12.8)]
    c.poly(hub, DPAD_HUB)
    c.stroke(hub, 0.8, INK, closed=True)
    return c.result()


def draw_chevron(scale, left):
    c = Canvas(7, 12, scale)
    pts = [(5.8, 1), (1.2, 6), (5.8, 11)] if left else [(1.2, 1), (5.8, 6), (1.2, 11)]
    c.stroke(pts, 2, WHITE)
    return c.result()


def draw_plus(scale):
    c = Canvas(8, 8, scale)
    c.stroke([(4, 0.7), (4, 7.3)], 1.6, PLUS)
    c.stroke([(0.7, 4), (7.3, 4)], 1.6, PLUS)
    return c.result()


def draw_slash(scale):
    c = Canvas(6, 14, scale)
    c.stroke([(5.2, 0.8), (0.8, 13.2)], 1.4, SLASH)
    return c.result()


def draw_dot(scale, ring):
    c = Canvas(9, 9, scale)
    if ring:
        c.circle(4.5, 4.5, 3.5, fill=None, outline=SLASH, width=2)
    else:
        c.circle(4.5, 4.5, 4.5, fill=CRIMSON)
    return c.result()


def draw_limb_dot(scale):
    # A plain white disc the overlay tints: one of the four dots of the Button Layout's limb icon.
    c = Canvas(8, 8, scale)
    c.circle(4, 4, 3.6, fill=WHITE)
    return c.result()


# The Xbox face buttons in their own colours, for the Button Layout's button column: a dark disc ringed in the button's
# colour with its letter in it.
XBOX_FACE_COLORS = {
    "a": (0x78, 0xCB, 0x55, 255),
    "b": (0xF2, 0x61, 0x6A, 255),
    "x": (0x55, 0xA8, 0xF2, 255),
    "y": (0xF2, 0xC9, 0x3E, 255),
}
RING_FILL = (0x0D, 0x07, 0x08, 255)


def draw_face_ring(scale, kind):
    c = Canvas(16, 16, scale, art_scale=0.8)
    color = XBOX_FACE_COLORS[kind]
    c.circle(10, 10, 8.9, fill=RING_FILL, outline=color, width=1.9)
    c.text(10, 14.5, kind.upper(), 12.5, color)
    return c.result()


def draw_digit(scale, digit):
    # Barlow 600 at 12 units with a 1-unit #0B0304 outline; the glyph sits in a 7-unit advance starting 1 unit in.
    c = Canvas(9, 16, scale)
    font = ImageFont.truetype(os.path.join(FONT_DIR, "Barlow-600.ttf"), max(1, round(12 * c.k)))
    x, y = c.p(4.5, 12.8)
    c.draw.text((x, y), str(digit), font=font, fill=FILL, anchor="ms",
                stroke_width=max(1, round(1 * c.k)), stroke_fill=(0x0B, 0x03, 0x04, 255))
    return c.result()


DPAD_STATES = ["udlr", "ud", "udl", "udr", "lr", "u", "d", "l", "r"]


def sprites():
    """(name, display w, display h, draw function) in atlas order; the C++ enum follows this order."""
    out = []
    for key in ["u", "d", "l", "r", "ul", "ur", "dl", "dr", "n"]:
        out.append((f"arrow_{key}", 18, 18, lambda s, k=key: draw_arrow(s, k)))
    for m in range(16):
        out.append((f"cluster_{m}", 18, 18, lambda s, m=m: draw_cluster(s, m)))
    for d in range(10):
        out.append((f"digit_{d}", 9, 16, lambda s, d=d: draw_digit(s, d)))
    for kind in ["cross", "circle", "triangle", "square"]:
        out.append((kind, 16, 16, lambda s, k=kind: draw_face(s, k)))
    for kind in ["l1", "r1", "l2", "r2"]:
        out.append((kind, 24, 16, lambda s, k=kind: draw_shoulder(s, k, k.upper())))
    out.append(("l3", 16, 16, lambda s: draw_stick(s, "L3")))
    out.append(("r3", 16, 16, lambda s: draw_stick(s, "R3")))
    out.append(("select", 20, 16, draw_select))
    out.append(("start", 16, 16, draw_start))
    for lit in DPAD_STATES:
        out.append((f"dpad_{lit}", 16, 16, lambda s, l=lit: draw_dpad(s, l)))
    out.append(("chevron_l", 7, 12, lambda s: draw_chevron(s, True)))
    out.append(("chevron_r", 7, 12, lambda s: draw_chevron(s, False)))
    out.append(("plus", 8, 8, draw_plus))
    out.append(("slash", 6, 14, draw_slash))
    out.append(("rec_dot", 9, 9, lambda s: draw_dot(s, False)))
    out.append(("rec_ring", 9, 9, lambda s: draw_dot(s, True)))
    # Xbox family: same plates, letters and names in place of PlayStation symbols.
    for kind in ["a", "b", "x", "y"]:
        out.append((f"xbox_{kind}", 16, 16, lambda s, k=kind: draw_face(s, k)))
    for shape, label in [("l1", "LB"), ("r1", "RB"), ("l2", "LT"), ("r2", "RT")]:
        out.append((f"xbox_{label.lower()}", 24, 16, lambda s, a=shape, b=label: draw_shoulder(s, a, b)))
    out.append(("xbox_ls", 16, 16, lambda s: draw_stick(s, "LS")))
    out.append(("xbox_rs", 16, 16, lambda s: draw_stick(s, "RS")))
    out.append(("xbox_view", 16, 16, lambda s: draw_face(s, "view")))
    out.append(("xbox_menu", 16, 16, lambda s: draw_face(s, "menu")))
    # The Button Layout: the limb icon's dot and the coloured Xbox face buttons.
    out.append(("limb_dot", 8, 8, draw_limb_dot))
    for kind in ["a", "b", "x", "y"]:
        out.append((f"xbox_{kind}_ring", 16, 16, lambda s, k=kind: draw_face_ring(s, k)))
    return out


def pack(images, width):
    """Shelf packing with a 1-texel transparent gutter around every cell."""
    order = sorted(range(len(images)), key=lambda i: (-images[i].size[1], i))
    x = y = 0
    shelf = 0
    places = [None] * len(images)
    for i in order:
        w, h = images[i].size
        if x + w + 2 > width:
            x = 0
            y += shelf
            shelf = 0
        places[i] = (x + 1, y + 1)
        x += w + 2
        shelf = max(shelf, h + 2)
    height = y + shelf
    height = (height + 3) // 4 * 4
    return places, height


def build(scale, preview_dir=None):
    table = sprites()
    images = [fn(scale) for _, _, _, fn in table]
    width = 256 if scale <= 1.5 else 512
    places, height = pack(images, width)
    atlas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    uvs = []
    for (name, w, h, _), img, (px, py) in zip(table, images, places):
        atlas.paste(img, (px, py))
        # UVs cover exactly the display size at this scale, so a fractional cell never stretches its art.
        uvs.append((px / width, py / height, (px + w * scale) / width, (py + h * scale) / height))
    buf = io.BytesIO()
    atlas.save(buf, format="PNG", optimize=True)
    if preview_dir:
        os.makedirs(preview_dir, exist_ok=True)
        atlas.save(os.path.join(preview_dir, f"pbd_glyphs_x{scale:g}.png"))
    return width, height, buf.getvalue(), uvs, table


def scale_tag(scale):
    return f"{scale:g}".replace(".", "_")


def write_header(name, lines):
    with open(os.path.join(os.path.dirname(OUT), name), "w", newline="\n") as f:
        f.write("\n".join(lines))


def emit_scale(scale, result):
    width, height, png, uvs, _ = result
    lines = ["#pragma once", "", '#include "pbd_glyph_atlas_types.h"', "", "namespace pbd_glyphs", "{"]
    tag = scale_tag(scale)
    lines.append(f"inline constexpr uv uvs_x{tag}[sprite_count] = {{")
    for u0, v0, u1, v1 in uvs:
        lines.append(f"\t{{{u0:.7f}f, {v0:.7f}f, {u1:.7f}f, {v1:.7f}f}},")
    lines += ["};", ""]
    lines.append("// clang-format off")
    lines.append(f"inline constexpr std::uint8_t png_x{tag}[{len(png)}] = {{")
    for i in range(0, len(png), PNG_BYTES_PER_LINE):
        lines.append("\t" + ",".join(str(b) for b in png[i:i + PNG_BYTES_PER_LINE]) + ",")
    lines += ["};", ""]
    lines.append("// clang-format on")
    lines += ["} // namespace pbd_glyphs", ""]
    write_header(f"pbd_glyph_atlas_x{tag}.h", lines)


def emit(results):
    table = results[0][4]
    lines = [
        "#pragma once",
        "",
        "// Generated by tools/pbd_practice_glyphs.py from the Practice UI glyph masters. Do not edit by hand.",
        "// One RGBA PNG atlas per output scale; UVs address each sprite's exact display rectangle at that scale.",
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace pbd_glyphs",
        "{",
        "enum sprite : std::uint8_t",
        "{",
    ]
    for name, _, _, _ in table:
        lines.append(f"\t{name},")
    lines += ["\tsprite_count", "};", ""]
    lines.append("struct size { std::uint8_t w, h; }; // display units on the 1280x720 canvas")
    lines.append("inline constexpr std::array<size, sprite_count> sizes{{")
    lines.append("\t" + ", ".join(f"{{{w}, {h}}}" for _, w, h, _ in table))
    lines += ["}};", ""]
    lines.append("inline constexpr std::array<const char*, sprite_count> names{")
    lines.append("\t" + ", ".join(f'"{name}"' for name, _, _, _ in table))
    lines += ["};", ""]
    lines.append("struct uv { float u0, v0, u1, v1; };")
    lines.append("struct atlas { float scale; std::uint16_t width, height; const std::uint8_t* png; std::size_t png_size; const uv* uvs; };")
    lines.append("")
    write_header("pbd_glyph_atlas_types.h", lines + ["} // namespace pbd_glyphs", ""])
    lines = ["#pragma once", ""]
    for scale, result in zip(SCALES, results):
        emit_scale(scale, result)
        lines.append(f'#include "pbd_glyph_atlas_x{scale_tag(scale)}.h"')
    lines += ["", "namespace pbd_glyphs", "{"]
    lines.append(f"inline constexpr std::array<atlas, {len(SCALES)}> atlases{{{{")
    for scale, (width, height, png, _, _) in zip(SCALES, results):
        tag = scale_tag(scale)
        lines.append(f"\t{{{scale:.1f}f, {width}, {height}, png_x{tag}, sizeof(png_x{tag}), uvs_x{tag}}},")
    lines += ["}};", "} // namespace pbd_glyphs", ""]
    write_header(os.path.basename(OUT), lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", help="also save the atlases as PNG files in this directory")
    args = parser.parse_args()
    results = [build(s, args.preview) for s in SCALES]
    emit(results)
    for s, (w, h, png, _, table) in zip(SCALES, results):
        print(f"x{s:g}: {w}x{h}, {len(png)} bytes, {len(table)} sprites")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
