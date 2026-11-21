#!/usr/bin/env python3
"""Generate the Rosé Pine and Gruvbox icon sets and their LVGL bitmaps.

Run from any directory:

    pip install resvg-py==0.5.0 pillow
    python3 tools/themed_icons.py            # rewrite the generated files
    python3 tools/themed_icons.py --check    # verify they are current

The SVGs these two sets are drawn from are kept beside this file, under
assets/icons/rose-pine/ and assets/icons/gruvbox/, so the drawings can be
opened, compared and edited like every other icon in the repository. This tool
rasterises them into two self-contained C files the player compiles:

    src/gui/shell/icons_rose.c      (icons_rose.h)
    src/gui/shell/icons_gruvbox.c   (icons_gruvbox.h)

Each bitmap is a single white shape stored as 8-bit alpha (LV_COLOR_FORMAT_A8),
so a page tints a themed glyph with the same lv_obj_set_style_image_recolor() it
uses for the shared set. The shared set is ARGB8888, three quarters of it the
same white pixel; these sets are one ink each, so they carry only the alpha and
the recolour paints the ink back. That is a quarter of the firmware bytes for
identical output. A themed set only replaces the glyphs it carries; anything it
does not draw keeps the shared Lucide bitmap, which is why the lookup in
src/gui/shell/theme_icons.c answers NULL for those.

The two sets are not recolours of the shared one. Rosé Pine's is drawn for the
scheme's airy, decorative weight -- a 1.6 px line, round caps, shapes with a
little more curve in them -- and Gruvbox's for the scheme's warm terminal look:
a 1.9 px line on a wider stroke lattice, squarer joins and chunkier
silhouettes. Both keep the 24 px grid of the shared icons, so a page that swaps
one for another does not relayout: the glyphs land in the same box.
"""

import argparse
import io
import os
import re
import sys

try:
    from PIL import Image
    import resvg_py
except ImportError as exc:
    raise SystemExit("needs resvg-py and pillow: pip install resvg-py==0.5.0 pillow") from exc

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(REPO_ROOT, "assets", "icons")
OUT_DIR = os.path.join(REPO_ROOT, "src", "gui", "shell")

# The sizes are the ones the shared icons carry for the same drawing (see
# tools/svg_to_lvgl.py): the player does not scale these at run time, so the two
# files have to line up name for name and pixel for pixel.
SETS = {
    "rose": {
        "prefix": "rose",
        "dir": "rose-pine",
        "file": "icons_rose",
        "header": "icons_rose.h",
        "guard": "ICONS_ROSE_H",
        "title": "Rosé Pine Moon's icon set",
    },
    "gruv": {
        "prefix": "gruvbox",
        "dir": "gruvbox",
        "file": "icons_gruvbox",
        "header": "icons_gruvbox.h",
        "guard": "ICONS_GRUVBOX_H",
        "title": "Gruvbox's icon set",
    },
}


def icon(name, size):
    return name, size


# The family table, in the shape the lookup uses: the C name of the themed
# bitmap (icon_rose_<family> / icon_gruvbox_<family>), the drawing under this
# set's folder, the size it is rasterised at, and the shared symbol it stands
# in for.
#
# The shared symbol is what makes the substitution exact: SRC names the bitmap
# in icons.h that a page already asks for, so theme_icon(&icon_play) finds the
# themed play and nothing else. Where a shared name does not exist yet, SHARED
# gives the shared name the bitmap is being given, and the lookup is taught to
# answer with it under that name as well.
#
# Sizes match the shared bitmap of the same drawing, because these are not
# scaled at run time: the two files line up name for name and pixel for pixel,
# and a page that swaps one for the other does not relayout.
ICONS = {
    # ------------------------------------------------------------------
    # The six destinations of the home screen, then the pages behind them.
    # 46 px is the size the icon-only arrangement asks for; the labelled V1
    # layout draws the same six at 22 px, which is the home_* rows below.
    # ------------------------------------------------------------------
    "menu_music": ("music.svg", 46, None),
    "menu_streaming": ("streaming.svg", 46, None),
    "menu_wireless": ("wireless.svg", 46, None),
    "menu_audiobooks": ("audiobooks.svg", 46, None),
    "menu_more": ("more.svg", 46, None),
    "menu_settings": ("settings.svg", 46, None),

    # The same six at the size the labelled V1 tiles draw them. Each is also
    # the native-size art for its tile in the icon-only arrangement, which is
    # why the length of this list is what tools/main_menu_icons.py renders for
    # the shared set.
    "home_music": ("music.svg", 22, "music2"),
    "home_streaming": ("streaming.svg", 22, "radio_player"),
    "home_wireless": ("wireless.svg", 22, "wifi_high"),
    "home_audiobooks": ("audiobooks.svg", 22, "files_audiobook"),
    "home_more": ("more.svg", 22, "ellipsis_vertical"),
    "home_settings": ("settings.svg", 22, "music_settings"),

    # Music page and its lists.
    "all_tracks": ("list-music.svg", 32, None),
    "all_tracks_row": ("list-music.svg", 34, "list_music"),
    "albums": ("album.svg", 32, "album"),
    "artists": ("artist.svg", 32, "artist"),
    "album_artists": ("artist-album.svg", 32, "artist_album"),
    "genres": ("genre.svg", 32, "genre"),
    "years": ("year.svg", 32, "year"),
    "most_played": ("most-played.svg", 32, "most_played"),
    "favourites": ("star.svg", 32, "star"),
    "favourites_filled": ("star-filled.svg", 32, "star_filled"),
    "favourites_corner": ("star.svg", 34, "star_corner"),
    "album_corner": ("album.svg", 34, "album_corner"),
    "browse": ("folder.svg", 32, "folder"),
    "playlists": ("playlists.svg", 32, "files_playlist"),
    "folder": ("folder.svg", 32, "folder"),
    "folder_corner": ("folder.svg", 34, "folder_corner"),

    # Wireless.
    "wifi": ("wifi.svg", 46, "wifi"),
    "wifi_corner": ("wifi.svg", 34, "wifi_high"),
    "bluetooth": ("bluetooth.svg", 46, "bluetooth"),
    "bluetooth_corner": ("bluetooth.svg", 30, "bluetooth_status"),
    "airplay": ("airplay.svg", 46, "airplay_quick"),
    "airplay_page": ("airplay.svg", 128, "airplay_page"),
    "transfer": ("transfer.svg", 46, "wifi_transfer_quick"),
    "sonixlink": ("sonixlink.svg", 46, "sonixlink_quick"),
    "sonixlink_page": ("sonixlink.svg", 128, "sonixlink_page"),
    "dlna": ("dlna.svg", 46, "dlna_quick"),
    "dlna_page": ("dlna.svg", 128, "dlna_page"),
    "wifi_search": ("wifi-search.svg", 36, "wifi_search"),
    "bluetooth_search": ("bluetooth-search.svg", 36, "bluetooth_search"),
    "bluetooth_receiver": ("bluetooth-receiver.svg", 36, "bluetooth_receiver"),
    "change_codec": ("change-codec.svg", 34, "change_codec"),
    # The status bar's Wi-Fi rungs, one drawing per signal level, and the rune
    # at the size the bar draws it rather than the 46 px settings one.
    "wifi_zero": ("wifi-zero.svg", 34, "wifi_zero"),
    "wifi_low": ("wifi-low.svg", 34, "wifi_low"),
    "wifi_high": ("wifi-high.svg", 34, "wifi_high"),
    "wifi_max": ("wifi-max.svg", 34, "wifi_max"),
    "bluetooth_status": ("bluetooth-status.svg", 30, "bluetooth_status"),

    # Streaming.
    "radio": ("radio.svg", 32, "radio_player"),
    "radio_cover": ("radio.svg", 128, "radio_player"),
    "podcasts": ("podcasts.svg", 40, "podcast_list"),
    "podcasts_cover": ("podcasts.svg", 128, "podcast_cover"),
    "tidal": ("tidal.svg", 32, "tidal_mark"),
    "qobuz": ("qobuz.svg", 32, "qobuz_mark"),
    "soundcloud": ("soundcloud.svg", 32, "soundcloud_mark"),
    "jellyfin": ("jellyfin.svg", 32, "jellyfin_mark"),
    "subsonic": ("subsonic.svg", 32, "music_note"),

    # The More page.
    "dac": ("dac.svg", 32, "usbaudioout"),
    "gearboy": ("gearboy.svg", 32, "files_game"),
    "books": ("books.svg", 32, "files_book"),
    "file_manager": ("file-manager.svg", 32, "folder"),
    "games": ("gamepad.svg", 32, "files_game"),
    "bird": ("bird.svg", 32, "bird"),
    "game_2048": ("2048.svg", 32, "game_2048"),
    "snake": ("snake.svg", 32, "game_snake"),
    "dino": ("dino.svg", 32, "game_dino"),
    "calculator": ("calculator.svg", 32, "calculator"),
    "tag_manager": ("tag.svg", 32, "tag_manager"),
    "notes": ("notes.svg", 32, "notes"),
    "tape_deck": ("tape-deck.svg", 32, "audio_waveform"),
    "metronome": ("metronome.svg", 32, "metronome"),
    "pet": ("pet.svg", 32, "pet"),

    # Settings rows.
    "appearance": ("appearance.svg", 30, "ebook_theme"),
    "screen": ("screen.svg", 30, "monitor"),
    "date_time": ("date-time.svg", 30, "sort_date_new"),
    "power": ("power.svg", 30, "power_off"),
    "language": ("language.svg", 30, "sort_az"),
    "plugins": ("plugins.svg", 30, "settings_plugins"),
    "more": ("more-settings.svg", 30, "settings_more"),
    "system": ("system.svg", 30, "music_settings"),
    "developer": ("developer.svg", 30, "terminal"),

    # Transport and the player's own marks.
    "play": ("play.svg", 40, "play"),
    "pause": ("pause.svg", 40, "pause"),
    "play_large": ("play.svg", 48, "play_large"),
    "pause_large": ("pause.svg", 48, "pause_large"),
    "skip_back": ("skip-back.svg", 34, "skip_back"),
    "skip_forward": ("skip-forward.svg", 34, "skip_forward"),
    "skip_back_large": ("skip-back.svg", 42, "skip_back_large"),
    "skip_forward_large": ("skip-forward.svg", 42, "skip_forward_large"),
    "shuffle": ("shuffle.svg", 30, "shuffle"),
    "shuffle_repeat": ("shuffle-repeat.svg", 30, "shuffle_repeat"),
    "repeat_all": ("repeat.svg", 30, "repeat_all"),
    "repeat_one": ("repeat-one.svg", 30, "repeat_one"),
    "repeat_off": ("repeat-off.svg", 30, "repeat_off"),
    "star": ("star.svg", 32, "star"),
    "star_filled": ("star-filled.svg", 32, "star_filled"),
    "heart": ("heart.svg", 32, "heart_outline"),
    "heart_filled": ("heart-filled.svg", 32, "heart_filled"),
    "overflow": ("overflow.svg", 30, "ellipsis_vertical"),
    "play_speed": ("play-speed.svg", 34, "play_speed"),
    "chapter": ("chapter.svg", 34, "chapter"),
    "chapter_bar": ("chapter.svg", 44, "ebook_chapter"),
    "episodes": ("episodes.svg", 34, "podcast_episodes"),
    "loader": ("loader.svg", 26, "loader_small"),
    "loader_big": ("loader.svg", 56, "loader_big"),

    # The status bar.
    "battery": ("battery.svg", 38, "battery"),
    "battery_body": ("battery-body.svg", 38, "battery_charging_body"),
    "battery_bolt": ("battery-bolt.svg", 38, "battery_charging_bolt"),
    "volume_mute": ("volume-mute.svg", 30, "volume_mute"),
    "volume_low": ("volume-low.svg", 30, "volume_low"),
    "volume_high": ("volume-high.svg", 30, "volume_high"),
    "headphones": ("headphones.svg", 26, "headphones"),
    "usb_audio": ("usb-audio.svg", 26, "usbaudioout"),
    "play_status": ("play-status.svg", 26, "play_status"),
    "pause_status": ("pause-status.svg", 26, "pause_status"),
    "airplay_status": ("airplay-status.svg", 26, "airplay_status"),
    "sonixlink_status": ("sonixlink-status.svg", 26, "sonixlink_status"),
    "library_checking": ("library-checking.svg", 26, "library_checking"),
    "library_checked": ("library-checked.svg", 26, "library_checked"),

    # Chrome and the lists' corner buttons.
    "close": ("close.svg", 36, "close"),
    "check": ("check.svg", 36, "check"),
    "search": ("search.svg", 34, "search"),
    "chevron_left": ("chevron-left.svg", 36, "chevron_left"),
    "chevron_right": ("chevron-right.svg", 36, "chevron_right"),
    "chevron_up": ("chevron-up.svg", 32, "chevron_up"),
    "plus": ("plus.svg", 36, "plus"),
    "download": ("download.svg", 34, "download"),
    "sort_az": ("sort-az.svg", 34, "sort_az"),
    "sort_za": ("sort-za.svg", 34, "sort_za"),
    "sort_date_new": ("sort-date-new.svg", 34, "sort_date_new"),
    "sort_date_old": ("sort-date-old.svg", 34, "sort_date_old"),
    "sort_year_asc": ("sort-year-asc.svg", 34, "sort_year_asc"),
    "sort_year_desc": ("sort-year-desc.svg", 34, "sort_year_desc"),
    "sort_played": ("sort-played.svg", 34, "sort_played"),
    "sort_series": ("sort-series.svg", 34, "sort_series"),
    "refresh": ("refresh.svg", 34, "refresh"),
    "trash": ("trash.svg", 34, "trash"),
    "eye": ("eye.svg", 30, "eye"),
    "eye_off": ("eye-off.svg", 30, "eye_off"),
    "equalizer": ("equalizer.svg", 46, "equalizer"),
}

# The six home tiles as the icon-only V1 arrangement draws them. The shared set
# renders these at every integer size (tools/main_menu_icons.py); a themed set
# only needs the one the compact layout actually uses, 22 px, which is already
# in ICONS as home_*.
MENU_TILES = (
    "home_music", "home_streaming", "home_wireless",
    "home_audiobooks", "home_more", "home_settings",
)

HEADER = (
    "/*\n"
    " * GENERATED FILE -- do not edit by hand.\n"
    " * Produced from the SVGs in assets/icons/{folder} by tools/themed_icons.py.\n"
    " *\n"
    " * {title}. White glyphs stored as 8-bit alpha (LV_COLOR_FORMAT_A8):\n"
    " * tint them with lv_obj_set_style_image_recolor(), as the shared set is\n"
    " * tinted. A page that finds no themed glyph here keeps the shared Lucide\n"
    " * bitmap instead.\n"
    " */\n\n"
)


def render(svg_path, size):
    """SVG -> one alpha byte per pixel, the layout LVGL's A8 wants.

    Every glyph in these sets is a single ink: the drawings are white all the
    way down and only their alpha varies. Stored as ARGB8888 that is four bytes
    per pixel, three of which are the same constant in every icon -- 3.68 MB
    across the four sets in the firmware image. A8 keeps the alpha and drops
    the white, and the recolour every call site already applies paints the ink
    back (lv_draw_sw_img.c hands an A8 source straight to the blend colour, so
    image_recolor still works).
    """
    png = resvg_py.svg_to_bytes(
        svg_path=svg_path, width=size, height=size, skip_system_fonts=True
    )
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    if img.size != (size, size):
        img = img.resize((size, size), Image.Resampling.LANCZOS)
    return img.getchannel("A").tobytes(), img.width, img.height


def c_array(data, per_line=12):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = ", ".join("0x%02x" % b for b in data[i:i + per_line])
        lines.append("\t" + chunk + ",")
    return "\n".join(lines)


# Where the shared set's own bitmaps live, and the one section of icons.c that
# names them: the sizes below are read from there so the check cannot drift
# from the code it is checking.
SHARED_SOURCES = {
    "src/gui/shell/icons.c": "icons.c",
    "src/gui/streaming/jellyfin_icon.c": "jellyfin_icon.c",
    "src/gui/streaming/soundcloud_icon.c": "soundcloud_icon.c",
    "src/gui/pet/peticon.c": "peticon.c",
}


def shared_sizes():
    """icon_<name> -> longest side, read from the shared bitmap definitions."""
    sizes = {}
    for path in SHARED_SOURCES:
        full = os.path.join(REPO_ROOT, path)
        if not os.path.exists(full):
            continue
        text = open(full, encoding="utf-8", errors="replace").read()
        for m in re.finditer(
            r"const lv_image_dsc_t (icon_\w+) = \{\s*\.header = \{[^}]*?"
            r"\.w = (\d+),\s*\.h = (\d+)",
            text,
            re.S,
        ):
            sizes[m.group(1)] = max(int(m.group(2)), int(m.group(3)))
    return sizes


def shared_sides_cached():
    """shared_sizes() once per run; the generator asks for it per glyph."""
    global _SHARED_SIDES
    if _SHARED_SIDES is None:
        _SHARED_SIDES = shared_sizes()
    return _SHARED_SIDES


_SHARED_SIDES = None


def check_aliases():
    """Report aliases whose drawing is cut at a size the shared bitmap is not.

    A themed row that names a shared symbol answers for every call site that
    asks for that symbol, and those sites were written against the shared
    bitmap's pixel size (they scale it by its own header). An alias drawn at
    another size therefore resamples -- a 128 px service mark standing in for a
    24 px row comes out a smudge, and the other way round a 22 px tile mark
    blown up to fill a list row. This is a warning, not a failure: the sizes
    are worth knowing about, and one of them is deliberate (the 128 px radio
    cover answers icon_radio_player's 128 px mark at the same size).
    """
    sizes = shared_sizes()
    wrong = []
    for family, (_, px, shared) in ICONS.items():
        if not shared:
            continue
        side = sizes.get("icon_" + shared)
        if side is not None and side != px:
            wrong.append((family, shared, px, side))
    return wrong


def generate(key):
    cfg = SETS[key]
    folder = cfg["dir"]
    prefix = cfg["prefix"]
    parts_c = []
    parts_h = []
    total = 0

    names = []
    # shared symbol -> the family whose drawing is cut at that symbol's own
    # size. A set can draw one shape twice (the 32 px Radio row mark and the
    # 128 px Radio cover are both radio.svg), and a shape can also be drawn at
    # a size of its own for a tile (the 22 px home_st_render rows). Only one may
    # answer for the shared symbol by name, or a caller asking for the 128 px
    # cover would be handed the 32 px row mark and scale it up four times.
    by_shared = {}
    for family, (filename, size, shared) in ICONS.items():
        path = os.path.join(ICON_DIR, folder, filename)
        if not os.path.exists(path):
            raise SystemExit("missing icon: %s" % path)
        data, w, h = render(path, size)
        total += len(data)
        name = "icon_%s_%s" % (prefix, family)
        parts_c.append(
            "// %s, %dx%d\n"
            "static const uint8_t %s_data[] = {\n%s\n};\n\n"
            "const lv_image_dsc_t %s = {\n"
            "\t.header = {\n"
            "\t\t.magic = LV_IMAGE_HEADER_MAGIC,\n"
            "\t\t.cf = LV_COLOR_FORMAT_A8,\n"
            "\t\t.w = %d,\n"
            "\t\t.h = %d,\n"
            "\t\t.stride = %d,\n"
            "\t},\n"
            "\t.data_size = sizeof(%s_data),\n"
            "\t.data = %s_data,\n"
            "};\n"
            % (filename, w, h, name, c_array(data), name, w, h, w, name, name)
        )
        parts_h.append("extern const lv_image_dsc_t %s;" % name)
        names.append((family, shared))
        if shared and shared not in by_shared:
            by_shared[shared] = family

    # The table the lookup walks: one row per glyph, naming both bitmaps. Rows
    # with a shared name mean "this drawing also lands here", for the places
    # that ask for a bitmap under a name of its own (the corner sizes, the
    # status rungs, the standalone service marks).
    # Which drawing answers each shared symbol. The rule is the smallest
    # drawing that still covers the symbol: a caller scaling *up* to its target
    # is what resamples, and a tile drawn at 22 px wants the 22 px cut, not a
    # 32 px one shrunk to fit. Only when every drawing of the shape is smaller
    # than the symbol does the largest answer instead -- the Streaming tile's
    # 22 px art has to serve the 128 px placeholder behind it, and a bitmap
    # blown up is worse than one shrunk.
    candidates = {}
    for family, shared in names:
        if shared:
            candidates.setdefault(shared, []).append(family)
    alias_for = {}
    for shared, families in candidates.items():
        symbol_side = shared_sides_cached().get("icon_" + shared)
        if symbol_side is None:
            # Nothing to match against: the biggest drawing gives the most to
            # scale from.
            alias_for[shared] = max(families, key=lambda f: ICONS[f][1])
            continue
        # A drawing at least as large as the symbol is preferred (a caller
        # shrinking it keeps every edge it drew); among those the smallest wins,
        # then the one closest to the symbol. With none large enough, the
        # largest available answers -- see the note in theme_icons.h.
        alias_for[shared] = min(
            families,
            key=lambda f: (ICONS[f][1] < symbol_side, abs(ICONS[f][1] - symbol_side)),
        )

    rows = []
    for family, shared in names:
        themed = "&icon_%s_%s" % (prefix, family)
        if not shared or alias_for.get(shared) != family:
            # No shared bitmap carries this drawing on its own (the 46 px menu
            # art), or another size of the same shape answers better: the
            # caller only ever asks for the themed copy, or finds it under the
            # family that does.
            rows.append("\t{%s, NULL}," % themed)
        else:
            rows.append("\t{%s, &icon_%s}," % (themed, shared))
    table = (
        "const theme_icon_row_t theme_%s_map[] = {\n" % prefix
        + "\n".join(rows)
        + "\n};\n\n"
        "const size_t theme_%s_map_count = sizeof(theme_%s_map) / sizeof(theme_%s_map[0]);\n"
        % (prefix, prefix, prefix)
    )

    tiles = "\n".join(
        "\t&icon_%s_%s," % (prefix, family) for family in MENU_TILES
    )
    tile_table = (
        "\n// The six home tiles in this set's own art, in tile order.\n"
        "const lv_image_dsc_t *const theme_%s_tiles[6] = {\n" % prefix
        + tiles
        + "\n};\n"
    )

    header = HEADER.format(folder=folder, title=cfg["title"])
    source = (
        header
        + '#include "%s"\n\n#include <stdint.h>\n#include <stddef.h>\n\n'
        '#include "icons.h"\n'
        '#include "src/gui/pet/peticon.h"\n'
        '#include "src/gui/streaming/jellyfin_icon.h"\n'
        '#include "src/gui/streaming/soundcloud_icon.h"\n\n'
        % cfg["header"]
        + "\n".join(parts_c)
        + table
        + tile_table
    )
    decls = (
        header
        + "#ifndef %s\n#define %s\n\n" % (cfg["guard"], cfg["guard"])
        + '#include "lvgl/lvgl.h"\n'
        + "#include <stddef.h>\n\n"
        + '#include "theme_icons.h"\n\n'
        + "\n".join(parts_h)
        + '\n\n// The generated table, row per glyph. The row type and the lookup'
        + ' live\n// in theme_icons.h, which every consumer includes first.\n'
        'extern const theme_icon_row_t theme_%s_map[];\n' % prefix
        + 'extern const size_t theme_%s_map_count;\n' % prefix
        + '\n// The six home tiles in this set\'s own art, in tile order.\n'
        + 'extern const lv_image_dsc_t *const theme_%s_tiles[6];\n' % prefix
        + "\n\n#endif // %s\n" % cfg["guard"]
    )
    return source, decls, total


def write_file(path, text):
    with open(path, "w") as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser(
        description="Generate the Rosé Pine and Gruvbox icon bitmaps."
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the generated files are current without rewriting them",
    )
    args = parser.parse_args()

    if not args.check:
        # Say it out loud whenever the sets are rebuilt: an alias drawn at a
        # size the shared bitmap is not will resample at every call site that
        # sizes the glyph by its pixels.
        for family, shared, px, side in check_aliases():
            print(
                "note: %s draws icon_%s at %d px, the shared bitmap is %d"
                % (family, shared, px, side)
            )

    failures = 0
    for key, cfg in SETS.items():
        source, decls, total = generate(key)
        out_c = os.path.join(OUT_DIR, cfg["file"] + ".c")
        out_h = os.path.join(OUT_DIR, cfg["header"])
        if args.check:
            for path, text in ((out_c, source), (out_h, decls)):
                current = open(path).read() if os.path.exists(path) else None
                if current != text:
                    failures += 1
                    print("stale: %s" % os.path.relpath(path, REPO_ROOT))
        else:
            write_file(out_c, source)
            write_file(out_h, decls)
            print(
                "wrote %s and %s (%s, %d glyphs, %s pixel bytes)"
                % (
                    os.path.relpath(out_c, REPO_ROOT),
                    os.path.relpath(out_h, REPO_ROOT),
                    cfg["title"],
                    len(ICONS),
                    format(total, ","),
                )
            )

    if args.check:
        if failures:
            raise SystemExit("%d stale file(s): run %s" % (failures, "themed_icons.py"))
        print("PASS themed icons (%d glyphs per set)" % len(ICONS))


if __name__ == "__main__":
    main()
