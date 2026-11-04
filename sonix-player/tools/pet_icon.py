#!/usr/bin/env python3
"""Draws the pet's own icons: a Pikachu's head, once in colour and once in white.

One drawing, in a 64-unit box, makes both:

  * assets/icons/pet.svg -- the coloured head, in the six colours of the pixel
    art the pet itself is drawn from (src/gui/pet/pikachu.c). tools/
    make_more_tiles.py puts it on the violet tile Dark and Light show in the
    More menu, and tools/svg_to_lvgl.py embeds that tile as icon_menu_pet.
  * src/gui/pet/peticon.c -- the same head as one white shape with the face
    cut out of it, in the same ARGB8888 white-with-alpha form as the rest of
    the icon set, so the theme tints it with lv_obj_set_style_image_recolor()
    like every other glyph. That is icon_pet, for the function-glyph themes.

The More menu's other glyphs are generated from assets/icons by
tools/svg_to_lvgl.py, which needs cairo and rewrites the 26 MB icons.c for a
single new picture; the white glyph is drawn here instead and written to its
own small file.

    pip install pillow
    python3 tools/pet_icon.py

The output is checked in; building the player needs neither Python nor Pillow.
"""

import math
import os

from PIL import Image, ImageDraw

SUPERSAMPLE = 8
SIZE = 112
BOX = 64.0  # the drawing's own units

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "src", "gui", "pet", "peticon.c")
OUT_H = os.path.join(ROOT, "src", "gui", "pet", "peticon.h")
OUT_SVG = os.path.join(ROOT, "assets", "icons", "pet.svg")

# The picture's six colours (pikachu.c).
INK = "#252923"
YELLOW = "#F4D601"
RED = "#F40202"
WHITE = "#F4FBF3"

# ---- the drawing, in a 64 x 64 box -------------------------------------------
#
# The left ear is a leaf: a base hidden behind the head, a point that leans out
# and up, and a black tip. The right ear is the left one mirrored.
EAR_TIP = (7.5, 2.5)
EAR_BASE_OUT = (12.5, 34.0)
EAR_BASE_IN = (28.0, 28.5)
EAR_TIP_CUT = 10.5  # how far down the ear the black stops, along its axis
EAR_GAP = 2.8  # the white one's gap between the tip and the rest of the ear
HEAD = (32.0, 41.0, 23.0, 17.5)  # centre x, centre y, radius x, radius y
EYES = [(21.5, 39.0), (42.5, 39.0)]
EYE_R = 3.3
GLINTS = [(22.7, 37.6), (43.7, 37.6)]
GLINT_R = 1.2
CHEEKS = [(15.2, 46.2), (48.8, 46.2)]
CHEEK_R = 4.4
MONO_EYE_R = 2.7  # the white one's holes
MONO_CHEEK_R = 3.5
NOSE = (32.0, 43.6, 1.3, 0.9)
MOUTH = [(27.0, 46.4), (29.5, 50.0), (32.0, 46.4), (34.5, 50.0), (37.0, 46.4)]  # two quadratics


def mirror(point):
    return (BOX - point[0], point[1])


def ear_curves(mirrored):
    """The ear as two cubic Beziers: up the outer edge to the point, down the inner."""
    tx, ty = EAR_TIP
    bo, bi = EAR_BASE_OUT, EAR_BASE_IN
    curves = [
        (bo, (bo[0] - 3.5, 22.0), (tx - 0.5, 12.0), (tx, ty)),
        ((tx, ty), (tx + 9.0, ty + 2.4), (bi[0] - 4.5, 14.5), bi),
    ]
    if mirrored:
        curves = [tuple(mirror(p) for p in c) for c in curves]
    return curves


def bezier(curve, steps=24):
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = curve
    points = []
    for i in range(steps + 1):
        t = i / steps
        a, b, c, d = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t**2, t**3
        points.append((a * x0 + b * x1 + c * x2 + d * x3, a * y0 + b * y1 + c * y2 + d * y3))
    return points


def ear_outline(mirrored):
    first, second = ear_curves(mirrored)
    return bezier(first) + bezier(second)[1:]


def ear_axis(mirrored):
    """The point, a unit vector down the ear, and the one across it."""
    tip = EAR_TIP
    base = ((EAR_BASE_OUT[0] + EAR_BASE_IN[0]) / 2, (EAR_BASE_OUT[1] + EAR_BASE_IN[1]) / 2)
    if mirrored:
        tip, base = mirror(tip), mirror(base)
    length = math.hypot(base[0] - tip[0], base[1] - tip[1])
    down = ((base[0] - tip[0]) / length, (base[1] - tip[1]) / length)
    return tip, down, (-down[1], down[0])


def tip_cap(mirrored):
    """The black end of the ear: everything on the point's side of a line across it."""
    tip, down, across = ear_axis(mirrored)
    cx, cy = tip[0] + down[0] * EAR_TIP_CUT, tip[1] + down[1] * EAR_TIP_CUT
    far = 30.0
    return [
        (cx - across[0] * far, cy - across[1] * far),
        (cx + across[0] * far, cy + across[1] * far),
        (cx + across[0] * far - down[0] * far, cy + across[1] * far - down[1] * far),
        (cx - across[0] * far - down[0] * far, cy - across[1] * far - down[1] * far),
    ]


def path_d(points):
    return "M " + " L ".join("%.2f %.2f" % p for p in points) + " Z"


# ---- the coloured head --------------------------------------------------------


def svg():
    hx, hy, hrx, hry = HEAD
    nx, ny, nrx, nry = NOSE
    left, right = path_d(ear_outline(False)), path_d(ear_outline(True))
    cap_l, cap_r = path_d(tip_cap(False)), path_d(tip_cap(True))
    m = MOUTH
    mouth = "M %.1f %.1f Q %.1f %.1f %.1f %.1f Q %.1f %.1f %.1f %.1f" % (
        m[0] + m[1] + m[2] + m[3] + m[4]
    )
    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">',
        "  <defs>",
        '    <clipPath id="ear-l"><path d="%s"/></clipPath>' % left,
        '    <clipPath id="ear-r"><path d="%s"/></clipPath>' % right,
        "  </defs>",
        '  <g stroke="%s" stroke-width="2.4" stroke-linejoin="round" stroke-linecap="round">' % INK,
        '    <path d="%s" fill="%s"/>' % (left, YELLOW),
        '    <path d="%s" fill="%s"/>' % (right, YELLOW),
        '    <path d="%s" fill="%s" stroke="none" clip-path="url(#ear-l)"/>' % (cap_l, INK),
        '    <path d="%s" fill="%s" stroke="none" clip-path="url(#ear-r)"/>' % (cap_r, INK),
        '    <ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="%.1f" fill="%s"/>' % (hx, hy, hrx, hry, YELLOW),
        "  </g>",
    ]
    for cx, cy in EYES:
        parts.append('  <circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"/>' % (cx, cy, EYE_R, INK))
    for cx, cy in GLINTS:
        parts.append('  <circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"/>' % (cx, cy, GLINT_R, WHITE))
    for cx, cy in CHEEKS:
        parts.append('  <circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"/>' % (cx, cy, CHEEK_R, RED))
    parts.append('  <ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="%.1f" fill="%s"/>' % (nx, ny, nrx, nry, INK))
    parts.append(
        '  <path d="%s" fill="none" stroke="%s" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>'
        % (mouth, INK)
    )
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


# ---- the white one ------------------------------------------------------------


def glyph():
    s = SIZE * SUPERSAMPLE
    k = s / BOX
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)
    clear = (0, 0, 0, 0)

    def px(points):
        return [(x * k, y * k) for x, y in points]

    def ellipse(cx, cy, rx, ry):
        return [(cx - rx) * k, (cy - ry) * k, (cx + rx) * k, (cy + ry) * k]

    # The ears and the head are one shape.
    for mirrored in (False, True):
        draw.polygon(px(ear_outline(mirrored)), fill=white)
    draw.ellipse(ellipse(*HEAD), fill=white)

    # The black tips, as a gap across each ear: one colour has no black, so the
    # tip is set off from the rest of the ear instead. The glyph is shown at
    # twenty-odd pixels in the themes that use it, where anything much thinner
    # than this is gone.
    for mirrored in (False, True):
        tip, down, across = ear_axis(mirrored)
        cx, cy = tip[0] + down[0] * EAR_TIP_CUT, tip[1] + down[1] * EAR_TIP_CUT
        half = 9.0
        draw.line(
            px([(cx - across[0] * half, cy - across[1] * half), (cx + across[0] * half, cy + across[1] * half)]),
            fill=clear,
            width=int(EAR_GAP * k),
        )

    # Eyes, cheeks and the little mouth, cut out of the silhouette. The eyes are
    # a size smaller than the colour ones and the cheeks, drawn in a hair from
    # the edge, a size larger than the eyes, so the two pairs are not mistaken
    # for each other; there is a rim of white left round every hole.
    for cx, cy in EYES:
        draw.ellipse(ellipse(cx, cy, MONO_EYE_R, MONO_EYE_R), fill=clear)
    for cx, cy in CHEEKS:
        draw.ellipse(ellipse(cx + (1.0 if cx < 32 else -1.0), cy, MONO_CHEEK_R, MONO_CHEEK_R), fill=clear)
    m = MOUTH
    mouth = bezier_quad(m[0], m[1], m[2]) + bezier_quad(m[2], m[3], m[4])[1:]
    draw.line(px(mouth), fill=clear, width=int(1.7 * k), joint="curve")
    for end in (m[0], m[4]):
        draw.ellipse(ellipse(end[0], end[1], 0.85, 0.85), fill=clear)

    return img.resize((SIZE, SIZE), Image.LANCZOS)


def bezier_quad(p0, p1, p2, steps=16):
    points = []
    for i in range(steps + 1):
        t = i / steps
        points.append(
            (
                (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t**2 * p2[0],
                (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t**2 * p2[1],
            )
        )
    return points


def emit():
    img = glyph()
    pixels = img.load()
    data = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = pixels[x, y]
            assert r == g == b, "the glyph is one colour"
            data += bytes((b, g, r, a))

    lines = []
    lines.append("/*")
    lines.append(" * GENERATED FILE -- do not edit by hand.")
    lines.append(" * Produced by tools/pet_icon.py. White ARGB8888, tinted by the theme.")
    lines.append(" */")
    lines.append("")
    lines.append('#include "peticon.h"')
    lines.append("")
    lines.append("// The pet's own glyph, 112x112: a Pikachu's head -- two ears and a round face --")
    lines.append("// with the eyes, cheeks and mouth cut out of the silhouette.")
    lines.append("static const uint8_t pet_icon_data[] = {")
    for i in range(0, len(data), 16):
        chunk = ", ".join("0x%02x" % b for b in data[i : i + 16])
        lines.append("\t" + chunk + ",")
    lines.append("};")
    lines.append("")
    lines.append("const lv_image_dsc_t icon_pet = {")
    lines.append("\t.header = {")
    lines.append("\t\t.magic = LV_IMAGE_HEADER_MAGIC,")
    lines.append("\t\t.cf = LV_COLOR_FORMAT_ARGB8888,")
    lines.append("\t\t.w = %d," % SIZE)
    lines.append("\t\t.h = %d," % SIZE)
    lines.append("\t\t.stride = %d," % (SIZE * 4))
    lines.append("\t},")
    lines.append("\t.data_size = sizeof(pet_icon_data),")
    lines.append("\t.data = pet_icon_data,")
    lines.append("};")
    lines.append("")

    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write(
            "#ifndef GUI_PET_PETICON_H\n"
            "#define GUI_PET_PETICON_H\n"
            "\n"
            '#include "lvgl/lvgl.h"\n'
            "\n"
            "// Generated by tools/pet_icon.py; see the comment there.\n"
            "extern const lv_image_dsc_t icon_pet;\n"
            "\n"
            "#endif /* GUI_PET_PETICON_H */\n"
        )

    with open(OUT_SVG, "w", encoding="utf-8") as f:
        f.write(svg())


if __name__ == "__main__":
    emit()
