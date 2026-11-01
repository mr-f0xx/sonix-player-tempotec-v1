#!/usr/bin/env python3
"""Generate the Pet and Tag Manager tiles for Dark and Light.

Requires pillow and resvg-py. Run this, then tools/svg_to_lvgl.py. The two
drawings use the same turned-back-tile stack as the metronome and the games, so
the newest More destinations sit with the older ones instead of showing a bare
white glyph or borrowing the File Manager's folder.

The monochrome glyphs for function-glyph themes stay separate: icon_pet is in
peticon.c, and icon_tag_manager is the pencil glyph in icons.c.
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_metronome_tile as tile

ICONS = Path(__file__).resolve().parents[1] / "assets" / "icons"

# Back, front gradient top, front gradient bottom. The pet keeps the violet
# more_tokyo_icon() gives it; the tag editor is teal, well away from the File
# Manager's amber folder.
PALETTES = {
    "pet": ((118, 82, 194), (192, 164, 245), (151, 111, 219)),
    "tag-manager": ((37, 145, 156), (124, 223, 220), (55, 181, 188)),
}


def main():
    for name, (back, top, bottom) in PALETTES.items():
        tile.BACK_COLOR = back
        tile.FRONT_TOP = top
        tile.FRONT_BOTTOM = bottom
        tile.SHADOW_COLOR = tuple(c // 4 for c in back) + (95,)
        tile.STROKE = 2.2
        tile.main(str(ICONS / f"{name}.svg"), str(ICONS / f"{name}.png"))


if __name__ == "__main__":
    main()
