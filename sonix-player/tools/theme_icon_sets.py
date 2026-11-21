#!/usr/bin/env python3
"""Draw the Catppuccin Mocha icon set from the Gruvbox geometry.

Rosé Pine and Gruvbox were drawn by hand, file by file, because each is a
different *hand*: Rosé Pine is 1.6 px round-cap line art, Gruvbox a 2 px
square-cap silhouette. Catppuccin is a third hand, not a copy of those:

  * Catppuccin's marks are light and round (1.5 px, round everywhere), the
    crisp pastel line its style guide draws its ports with.

It is derived from the Gruvbox geometry in assets/icons/gruvbox, which is
already a clean 24 px outline drawing of every glyph the player asks for: the
build here re-strokes each file at the lighter weight and re-cuts the marks
where the scheme is distinctive enough to deserve its own drawing (the soft
heart, and so on).

Run from anywhere:

    python3 tools/theme_icon_sets.py            # writes the directory
    python3 tools/theme_icon_sets.py --check     # verifies they are current

The SVGs are the source: tools/themed_icons.py renders them into C.
"""

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(REPO_ROOT, "assets", "icons")
SOURCE_SET = "gruvbox"

SETS = {
    "catppuccin": {
        "dir": "catppuccin-mocha",
        "title": "Catppuccin Mocha",
        # Light and round: the crisp pastel line of the port style guide.
        "stroke_width": "1.5",
        "stroke_linecap": "round",
        "stroke_linejoin": "round",
    },
}

HEADER = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" '
    'width="24" height="24" fill="none" stroke="#ffffff" '
    'stroke-width="{width}" stroke-linecap="{cap}" stroke-linejoin="{join}">\n'
)

# ---------------------------------------------------------------------------
# Marks each scheme draws its own way. The body is SVG (paths, rects, circles)
# in the 24 px box; the header above supplies the stroke. Anything not named
# here keeps the shared geometry, re-stroked.
#
# ---------------------------------------------------------------------------
# Catppuccin Mocha: soft, even, generous curves. The play triangle is rounded
# at all three corners, the heart is a wide plump one, the star is a five-point
# with rounded tips, the wifi arcs are full circles sliced thin.
# ---------------------------------------------------------------------------
CATPPUCCIN = {
    "play.svg": """
  <path fill="#ffffff" stroke="none" d="M8.2 5.1a1.6 1.6 0 0 1 2.44-1.37l9.2 5.9a1.7 1.7 0 0 1 0 2.74l-9.2 5.9A1.6 1.6 0 0 1 8.2 16.9z"/>
""",
    "pause.svg": """
  <rect x="7" y="4.6" width="3.6" height="14.8" rx="1.4" fill="#ffffff" stroke="none"/>
  <rect x="13.4" y="4.6" width="3.6" height="14.8" rx="1.4" fill="#ffffff" stroke="none"/>
""",
    "star.svg": """
  <path d="M12 3.2a.5.5 0 0 1 .9 0l2.1 4.4a2 2 0 0 0 1.53 1.1l4.8.68a.5.5 0 0 1 .28.85l-3.5 3.4a2 2 0 0 0-.57 1.76l.83 4.83a.5.5 0 0 1-.73.53l-4.3-2.27a2 2 0 0 0-1.86 0l-4.3 2.27a.5.5 0 0 1-.73-.53l.83-4.83a2 2 0 0 0-.57-1.76l-3.5-3.4a.5.5 0 0 1 .28-.85l4.8-.68a2 2 0 0 0 1.53-1.1Z"/>
""",
    "star-filled.svg": """
  <path fill="#ffffff" stroke="none" d="M12 3.2a.5.5 0 0 1 .9 0l2.1 4.4a2 2 0 0 0 1.53 1.1l4.8.68a.5.5 0 0 1 .28.85l-3.5 3.4a2 2 0 0 0-.57 1.76l.83 4.83a.5.5 0 0 1-.73.53l-4.3-2.27a2 2 0 0 0-1.86 0l-4.3 2.27a.5.5 0 0 1-.73-.53l.83-4.83a2 2 0 0 0-.57-1.76l-3.5-3.4a.5.5 0 0 1 .28-.85l4.8-.68a2 2 0 0 0 1.53-1.1Z"/>
""",
    "heart.svg": """
  <path d="M12 20.4c-1-.72-8.2-5.6-8.2-10.7a4.7 4.7 0 0 1 8.2-3.1 4.7 4.7 0 0 1 8.2 3.1c0 5.1-7.2 10-8.2 10.7z"/>
""",
    "heart-filled.svg": """
  <path fill="#ffffff" stroke="none" d="M12 20.4c-1-.72-8.2-5.6-8.2-10.7a4.7 4.7 0 0 1 8.2-3.1 4.7 4.7 0 0 1 8.2 3.1c0 5.1-7.2 10-8.2 10.7z"/>
""",
    "chevron-right.svg": """
  <path d="m9.4 5.2 6.6 6.8-6.6 6.8"/>
""",
    "chevron-left.svg": """
  <path d="M14.6 5.2 8 12l6.6 6.8"/>
""",
    "check.svg": """
  <path d="m5 12.6 4.4 4.4L19 6.6"/>
""",
    "close.svg": """
  <path d="m6.6 6.6 10.8 10.8"/>
  <path d="M17.4 6.6 6.6 17.4"/>
""",
    "search.svg": """
  <circle cx="10.8" cy="10.8" r="7"/>
  <path d="m16 16 4.6 4.6"/>
""",
    "folder.svg": """
  <path d="M3.6 7.4a2 2 0 0 1 2-2h3a2 2 0 0 1 1.4.6l1.2 1.2a2 2 0 0 0 1.4.6h7.8a2 2 0 0 1 2 2v8.4a2 2 0 0 1-2 2H5.6a2 2 0 0 1-2-2z"/>
""",
    "music.svg": """
  <path d="M9.2 16.8V6.1a1 1 0 0 1 .77-.97l7-1.75a1 1 0 0 1 1.23.97v10.05"/>
  <circle cx="6.8" cy="17.2" r="2.4"/>
  <circle cx="15.8" cy="14.6" r="2.4"/>
""",
}

SPECIALS = {"catppuccin": CATPPUCCIN}

STROKE_ATTRS = re.compile(
    r'\s*stroke-width="[^"]*"|\s*stroke-linecap="[^"]*"|\s*stroke-linejoin="[^"]*"'
)


def body_of(svg_text):
    """Everything between the opening <svg ...> line and </svg>."""
    lines = svg_text.splitlines()
    return "\n".join(lines[1:]).replace("</svg>", "").strip("\n")


def restroke(svg_text, cfg):
    """Re-stroke a file: new width, cap and join, same geometry."""
    head = HEADER.format(
        width=cfg["stroke_width"], cap=cfg["stroke_linecap"], join=cfg["stroke_linejoin"]
    )
    body = STROKE_ATTRS.sub("", body_of(svg_text)).rstrip()
    return head + body + "\n</svg>\n"


def generate(set_key):
    cfg = SETS[set_key]
    src_dir = os.path.join(ICON_DIR, SOURCE_SET)
    out_dir = os.path.join(ICON_DIR, cfg["dir"])
    specials = SPECIALS[set_key]
    files = {}

    for name in sorted(os.listdir(src_dir)):
        if not name.endswith(".svg"):
            continue
        source = open(os.path.join(src_dir, name), encoding="utf-8").read()
        if name in specials:
            head = HEADER.format(
                width=cfg["stroke_width"],
                cap=cfg["stroke_linecap"],
                join=cfg["stroke_linejoin"],
            )
            files[name] = head + specials[name].lstrip("\n").rstrip() + "\n</svg>\n"
        else:
            files[name] = restroke(source, cfg)

    # A special with no counterpart in the source set is a mistake worth hearing.
    unknown = sorted(set(specials) - set(files))
    return cfg, files, unknown


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify without writing")
    args = parser.parse_args()

    failures = 0
    for key in SETS:
        cfg, files, unknown = generate(key)
        for name in unknown:
            print("unknown special for %s: %s" % (key, name))
            failures += 1
        out_dir = os.path.join(ICON_DIR, cfg["dir"])
        if not args.check:
            os.makedirs(out_dir, exist_ok=True)
        extra = sorted(set(os.listdir(out_dir)) - set(files)) if os.path.isdir(out_dir) else []
        for name in extra:
            print("stale file in %s: %s" % (cfg["dir"], name))
            failures += 1
        for name, text in sorted(files.items()):
            path = os.path.join(out_dir, name)
            if args.check:
                current = open(path, encoding="utf-8").read() if os.path.exists(path) else None
                if current != text:
                    print("stale: %s" % os.path.relpath(path, REPO_ROOT))
                    failures += 1
            else:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
        if not args.check:
            print("wrote %d SVGs to %s" % (len(files), os.path.relpath(out_dir, REPO_ROOT)))

    if args.check:
        print("PASS themed SVG sets" if failures == 0 else "FAIL themed SVG sets")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
