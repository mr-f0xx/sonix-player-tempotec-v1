#!/usr/bin/env python3
"""Builds assets/fonts/symbols.ttf, the one-glyph fallback for the explicit mark.

MiSans (and the other faces shipped with the player) have no U+1F170, so a
literal "🅴" in a title would draw as a missing-glyph box. This tiny face carries
just that one glyph, drawn as a squared E in the current text colour. fonts.c
chains it behind every UI face; everything else is answered by the faces that
were already there.

Requires fontTools (pip install fonttools). Run from the repository root:

    python3 tools/make_explicit_font.py

The generated file is committed: neither the firmware build nor the player has
a Python dependency.
"""

import os
import sys

try:
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
except ImportError:
    sys.exit("fonttools is needed: pip install fonttools")

OUT = [
    "sonix-player/assets/fonts/symbols.ttf",
    "sonix-packer/assets/V1/usr/resource/sonix/fonts/symbols.ttf",
]

UPM = 1000
ADVANCE = 800


def contour(pen, points):
    pen.moveTo(points[0])
    for point in points[1:]:
        pen.lineTo(point)
    pen.closePath()


def build():
    pen = TTGlyphPen(None)

    # A square ring: outer contour clockwise, inner one counter-clockwise so
    # it is a hole rather than a filled block.
    contour(pen, [(120, 700), (680, 700), (680, 20), (120, 20)])
    contour(pen, [(200, 620), (200, 100), (600, 100), (600, 620)])

    # The E inside it, drawn as one polygon so the three bars join the stem.
    contour(pen, [
        (270, 180), (540, 180), (540, 260), (350, 260),
        (350, 320), (510, 320), (510, 400), (350, 400),
        (350, 460), (540, 460), (540, 540), (270, 540),
    ])

    glyph = pen.glyph()

    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder([".notdef", "uni1F170"])
    fb.setupCharacterMap({0x1F170: "uni1F170"})
    fb.setupGlyf({".notdef": TTGlyphPen(None).glyph(), "uni1F170": glyph})
    fb.setupHorizontalMetrics({
        ".notdef": (ADVANCE, 0),
        "uni1F170": (ADVANCE, 120),
    })
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({
        "familyName": "Sonix Symbols",
        "styleName": "Regular",
        "uniqueFontIdentifier": "Sonix Symbols explicit mark",
        "fullName": "Sonix Symbols",
        "psName": "SonixSymbols",
    })
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    fb.setupHead(unitsPerEm=UPM)

    return fb


if __name__ == "__main__":
    font = build()
    for path in OUT:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        font.save(path)
        print(f"{path}: {os.path.getsize(path)} bytes")
