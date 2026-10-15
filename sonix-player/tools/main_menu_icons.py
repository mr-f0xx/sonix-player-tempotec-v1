#!/usr/bin/env python3
"""Generate native-pixel art for Main menu icons only (no runtime resampling).

Run from any directory:
    pip install resvg-py==0.5.0 pillow==12.3.0
    python3 tools/main_menu_icons.py
    python3 tools/main_menu_icons.py --check

The player builds from the checked-in C file; it needs neither Python nor an
SVG renderer. The original icons.c assets stay untouched for labeled menus,
secondary pages, status bars and the mini-player.

Render SVGs directly at their final size, NOT by enlarging the 30-34 px toolbar
bitmaps. A8 preserves the artwork's alpha (including the faint outer Wi-Fi
arc), costs one byte/pixel, and takes the theme color through LVGL recoloring.
Dark/Light illustrations are resized once, directly from the original PNGs.

The V1's glyphs can shrink with the mini-player's font-dependent height, hence
all integer sizes 1..47. The maximum comes from its header-free OLED grid and
includes both requested reductions. The two regular-panel glyph sizes are
138 (OLED) and 173 (other themes); colored art is 74 (V1) or 173 (regular).
UI tests assert native-size coverage after theme and font reflows. The lookup
returns NULL for an unknown future layout, allowing the existing scaled art
as a fallback rather than a missing image.
"""

import argparse
import io
from pathlib import Path

try:
    from PIL import Image
    import resvg_py
except ImportError as exc:
    raise SystemExit("needs resvg-py and pillow: pip install resvg-py==0.5.0 pillow==12.3.0") from exc

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets" / "icons"
OUTPUT = ROOT / "src" / "gui" / "shell" / "main_menu_icons.c"
GLYPH_SIZES = (*range(1, 48), 138, 173)
COLOR_SIZES = (74, 173)
# Preserve the original six pixels and descriptors as a contiguous prefix of
# the generated file. The new art is appended after them; the map below links
# stable main_menu_button_t identities (display order) to these art slots.
BASE_GLYPHS = (
    "music-2.svg", "radio-player.svg", "wifi-high.svg",
    "files-audiobook.svg", "ellipsis-vertical.svg", "music-settings.svg",
)
BASE_COLORS = (
    "music.png", "streaming.png", "wireless.png",
    "audiobooks.png", "more.png", "settings.png",
)
EXTRA_GLYPHS = (
    "files-book.svg", "usb-audio-out.svg", "files-gamepad.svg", "folder.svg", "bird.svg",
)
EXTRA_COLORS = (
    "ebook.png", "dac.png", "gearboy.png", "file-explorer.png", "flappy-bird.png",
)
BUTTON_ASSET_INDEX = (0, 1, 2, 3, 6, 7, 8, 9, 10, 4, 5)


def render_glyph(filename, size):
    png = resvg_py.svg_to_bytes(
        svg_path=str(ASSETS / filename), width=size, height=size,
        skip_system_fonts=True,
    )
    image = Image.open(io.BytesIO(png)).convert("RGBA")
    assert image.size == (size, size)
    alpha = image.getchannel("A").tobytes()
    # Align rows as well as the array for the 32-bit target's mask blitter.
    stride = (size + 3) & ~3
    rows = [alpha[y * size:(y + 1) * size] + bytes(stride - size) for y in range(size)]
    return b"".join(rows), stride, "A8"


def render_color(filename, size):
    image = Image.open(ASSETS / filename).convert("RGBA")
    if image.size != (size, size):
        # Pillow resizes RGBA through premultiplied alpha, avoiding dark fringes.
        image = image.resize((size, size), Image.Resampling.LANCZOS)
    return image.tobytes("raw", "BGRA"), size * 4, "ARGB8888"


def render_assets(parts, name, filenames, sizes, render):
    descriptors = {}
    total_bytes = 0
    for size in sizes:
        entries = []
        for tile, filename in enumerate(filenames):
            var = f"{name}_{size}_{tile}"
            pixels, stride, fmt = render(filename, size)
            total_bytes += len(pixels)
            lines = ["\t" + ", ".join(f"0x{b:02x}" for b in pixels[i:i + 16]) + ","
                     for i in range(0, len(pixels), 16)]
            parts.append(f"// {filename}, {size}x{size}, {fmt}\n"
                         f"_Alignas(4) static const uint8_t {var}[] = {{\n" +
                         "\n".join(lines) + "\n};\n\n")
            entries.append(
                "\t{.header = {.magic = LV_IMAGE_HEADER_MAGIC, "
                f".cf = LV_COLOR_FORMAT_{fmt}, .w = {size}, .h = {size}, .stride = {stride}" + "}, "
                f".data_size = sizeof({var}), .data = {var}" + "},"
            )
        descriptors[size] = "\n".join(entries)
    return descriptors, total_bytes


def write_descriptors(parts, name, sizes, filenames, descriptors):
    parts.append(f"static const lv_image_dsc_t {name}_icons[{len(sizes)}][{len(filenames)}] = {{\n")
    for size in sizes:
        parts.append("\t{\n" + descriptors[size] + "\n\t},\n")
    parts.append("};\n\n")


def generate():
    base_count = len(BASE_GLYPHS)
    assert base_count == len(BASE_COLORS)
    assert len(EXTRA_GLYPHS) == len(EXTRA_COLORS)
    assert len(BUTTON_ASSET_INDEX) == base_count + len(EXTRA_GLYPHS)
    assert sorted(BUTTON_ASSET_INDEX) == list(range(len(BUTTON_ASSET_INDEX)))
    parts = [
        "/* GENERATED by tools/main_menu_icons.py -- do not edit.\n"
        " * Native-pixel home art only; icons.c remains the source for other UI. */\n"
        '#include "main_menu_icons.h"\n'
        '#include "main_menu.h"\n\n'
        f'_Static_assert(MAIN_MENU_BUTTON_COUNT == {len(BUTTON_ASSET_INDEX)}, "Update the home icon assets");\n\n',
    ]
    total_bytes = 0
    # Keep the original six in precisely the old order: glyph pixels, colored
    # pixels, then both descriptor arrays. Only after that append the new art.
    base = {}
    for name, files, sizes, render in (
        ("glyph", BASE_GLYPHS, GLYPH_SIZES, render_glyph),
        ("color", BASE_COLORS, COLOR_SIZES, render_color),
    ):
        base[name], count = render_assets(parts, name, files, sizes, render)
        total_bytes += count
    write_descriptors(parts, "glyph", GLYPH_SIZES, BASE_GLYPHS, base["glyph"])
    write_descriptors(parts, "color", COLOR_SIZES, BASE_COLORS, base["color"])

    extra = {}
    for name, files, sizes, render in (
        ("extra_glyph", EXTRA_GLYPHS, GLYPH_SIZES, render_glyph),
        ("extra_color", EXTRA_COLORS, COLOR_SIZES, render_color),
    ):
        extra[name], count = render_assets(parts, name, files, sizes, render)
        total_bytes += count
    write_descriptors(parts, "extra_glyph", GLYPH_SIZES, EXTRA_GLYPHS, extra["extra_glyph"])
    write_descriptors(parts, "extra_color", COLOR_SIZES, EXTRA_COLORS, extra["extra_color"])

    parts.append("// main_menu_button_t -> art slot (the original six stay first)\n"
                 f"static const uint8_t asset_index[MAIN_MENU_BUTTON_COUNT] = {{{', '.join(map(str, BUTTON_ASSET_INDEX))}}};\n\n")
    parts.append(f"""const lv_image_dsc_t *main_menu_icon_native(int button, bool glyph, int size) {{
\tif (button < 0 || button >= MAIN_MENU_BUTTON_COUNT) {{
\t\treturn NULL;
\t}}
\tint art = asset_index[button];
\tif (glyph) {{
\t\tint row = size >= 1 && size <= 47 ? size - 1 : size == 138 ? 47 : size == 173 ? 48 : -1;
\t\tif (row >= 0) {{
\t\t\treturn art < {base_count} ? &glyph_icons[row][art] : &extra_glyph_icons[row][art - {base_count}];
\t\t}}
\t}} else {{
\t\tint row = size == 74 ? 0 : size == 173 ? 1 : -1;
\t\tif (row >= 0) {{
\t\t\treturn art < {base_count} ? &color_icons[row][art] : &extra_color_icons[row][art - {base_count}];
\t\t}}
\t}}
\treturn NULL;
}}
""")
    return "".join(parts), total_bytes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify without rewriting the generated file")
    args = parser.parse_args()
    text, total_bytes = generate()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != text:
            raise SystemExit(f"stale native menu art: run {Path(__file__).name}")
        print(f"PASS native menu art ({total_bytes:,} pixel bytes)")
    else:
        OUTPUT.write_text(text)
        print(f"wrote {OUTPUT} ({total_bytes:,} pixel bytes)")


if __name__ == "__main__":
    main()
