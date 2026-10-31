#!/usr/bin/env python3
"""Generate the Dark/Light More game tiles using the metronome's stack style.

Requires pillow and resvg-py. Run this, then tools/svg_to_lvgl.py.
Monochrome SVGs remain separate for function-glyph themes and compact lists.
"""
from pathlib import Path

import make_metronome_tile as tile

ICONS = Path(__file__).resolve().parents[1] / "assets" / "icons"
# Back, front gradient top, front gradient bottom: blue, green, amber.
PALETTES = {
    "2048": ((58, 113, 208), (128, 190, 249), (73, 137, 222)),
    "snake": ((55, 157, 91), (144, 224, 162), (77, 184, 112)),
    "dino": ((209, 139, 42), (250, 210, 126), (233, 167, 64)),
}


def main():
    for game, (back, top, bottom) in PALETTES.items():
        tile.BACK_COLOR = back
        tile.FRONT_TOP = top
        tile.FRONT_BOTTOM = bottom
        tile.SHADOW_COLOR = tuple(c // 4 for c in back) + (95,)
        tile.STROKE = 2  # preserve the games' original silhouettes
        tile.main(str(ICONS / f"game-{game}.svg"), str(ICONS / f"game-{game}.png"))


if __name__ == "__main__":
    main()
