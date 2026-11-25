#!/usr/bin/env python3
"""Draws a More tile: the turned-back two-tile stack.

A back tile turned a little, a shadow under it, a front tile with a vertical
gradient and a white glyph. The other tile scripts import this module and set
its colours before calling main(), so every More tile is built the same way.
Drawn at four times the size and scaled down, which keeps the edges and the
glyph as clean as the shipped ones.
"""
import io

from PIL import Image, ImageDraw, ImageFilter
import resvg_py

SCALE = 4
SIZE = 140
W = SIZE * SCALE

# The stack, in fractions of the canvas.
SIDE = 0.70
RADIUS = 0.26
BACK_ROTATION = 14
BACK_COLOR = (60, 176, 150)
SHADOW_COLOR = (10, 60, 50, 95)
FRONT_POS = (0.19, 0.22)
FRONT_TOP = (146, 224, 207)
FRONT_BOTTOM = (82, 190, 166)
GLYPH_SHARE = 0.72  # of the front tile's side
STROKE = 2.6  # in the icon's own 24-unit viewBox, so the glyph reads at 72 px


def rounded(size, radius, fill):
    tile = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(tile).rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=fill)
    return tile


def gradient_tile(size, radius):
    tile = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    paint = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(paint)
    for y in range(size):
        t = y / (size - 1)
        color = tuple(int(FRONT_TOP[i] + (FRONT_BOTTOM[i] - FRONT_TOP[i]) * t) for i in range(3)) + (255,)
        draw.line([(0, y), (size, y)], fill=color)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    tile.paste(paint, (0, 0), mask)
    return tile


def main(svg_path, out_path):
    side = int(W * SIDE)
    radius = int(side * RADIUS)

    canvas = Image.new("RGBA", (W, W), (0, 0, 0, 0))

    # The shadow, a soft drop under the front tile's own silhouette.
    shadow = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    shadow.paste(rounded(side, radius, SHADOW_COLOR), (int(W * (FRONT_POS[0] + 0.01)), int(W * (FRONT_POS[1] + 0.05))))
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(2.5 * SCALE / 4)))

    # The back tile, turned under the front one so a corner of it shows above
    # and to the left, the way every other More tile has it.
    back = rounded(side, radius, BACK_COLOR + (255,))
    back = back.rotate(BACK_ROTATION, resample=Image.BICUBIC, expand=True)
    back = back.filter(ImageFilter.GaussianBlur(4 * 0.4 * SCALE / 4))
    canvas.alpha_composite(back, (int(W * 0.086), int(W * 0.086)))

    # The front tile.
    front_pos = (int(W * FRONT_POS[0]), int(W * FRONT_POS[1]))
    canvas.alpha_composite(gradient_tile(side, radius), front_pos)

    # The glyph, white, centred on the front tile.
    svg = open(svg_path, encoding="utf-8").read()
    svg = svg.replace("currentColor", "#ffffff")
    svg = svg.replace('stroke-width="2"', 'stroke-width="%s"' % STROKE)
    glyph_size = int(side * GLYPH_SHARE)
    png = resvg_py.svg_to_bytes(svg_string=svg, width=glyph_size, height=glyph_size)
    glyph = Image.open(io.BytesIO(bytes(png))).convert("RGBA")
    canvas.alpha_composite(glyph, (front_pos[0] + (side - glyph_size) // 2, front_pos[1] + (side - glyph_size) // 2))

    canvas.resize((SIZE, SIZE), Image.LANCZOS).save(out_path)
    print("wrote", out_path, canvas.size, "->", SIZE)
