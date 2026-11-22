#!/usr/bin/env python3
"""Generate the four SD icon sets that match the shipped theme palettes.

The sets live on the card, not in the firmware: copy examples/icon-sets/<set>
to Icons/<set> on the SD card and choose it in Settings -> Appearance ->
Icon set (docs/icon-sets.md). Each folder holds palette.ini -- the seven
function roles the themed glyphs use -- plus one <id>.png per glyph the
firmware can swap; the ids and sizes come from tools/svg_to_lvgl.py's
registry, so the sets follow the icons exactly.

    pip install resvg-py pillow
    python3 tools/gen_icon_sets.py            # write all four sets
    python3 tools/gen_icon_sets.py --check    # verify what is checked in

Every icon is rendered from assets/icons at four times its native size
(capped at 256), then colourised: the SVG's alpha becomes the mask and the
set's colour fills it, so the artwork is identical to the built-in glyph and
only the ink differs. Which colour is decided by ROLE below -- the idle ink
of an icon whose meaning carries one (folders green, alerts red, books
amber, radios purple ...) -- and text_primary everywhere else. State and
accent colours are never baked: the interface still paints those at runtime
through LVGL's recolor, which is why a selected or low-battery state keeps
working on a set just as it does on the built-ins.
"""

import io
import os
import struct
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)

import svg_to_lvgl  # id/size lists only; importing renders nothing

try:
    import resvg_py
    from PIL import Image
except ImportError:
    sys.exit("needs resvg-py and pillow: pip install resvg-py pillow")

REPO_ROOT = os.path.dirname(TOOLS_DIR)
ICON_SRC = os.path.join(REPO_ROOT, "assets", "icons")
OUT_ROOT = os.path.join(REPO_ROOT, "examples", "icon-sets")

# The four palettes, keyed by the folder name the SD card will use -- the
# same names as examples/themes/, so a theme and its set read as one choice.
# `fg` is the set's text_primary: the idle ink of every icon without a role.
SETS = {
    "gruvbox": {
        "fg": "#ebdbb2",
        "green": "#b8bb26",
        "red": "#fb4934",
        "amber": "#fabd2f",
        "blue": "#83a598",
        "cyan": "#8ec07c",
        "purple": "#d3869b",
        "muted": "#a89984",
        "source": "https://github.com/morhetz/gruvbox",
    },
    # Rose Pine has no separate green: foam (its greenest bright hue) carries
    # both the green and cyan roles, and pine stands in for blue.
    "rose-pine": {
        "fg": "#e0def4",
        "green": "#9ccfd8",
        "red": "#eb6f92",
        "amber": "#f6c177",
        "blue": "#31748f",
        "cyan": "#9ccfd8",
        "purple": "#c4a7e7",
        "muted": "#908caa",
        "source": "https://rosepinetheme.com/palette",
    },
    # Dracula's ANSI blue is the purple: the set keeps bd93f9 for purple and
    # lets blue share the cyan hue, where a Wi-Fi glyph wants to read.
    "dracula": {
        "fg": "#f8f8f2",
        "green": "#50fa7b",
        "red": "#ff5555",
        "amber": "#ffb86c",
        "blue": "#8be9fd",
        "cyan": "#8be9fd",
        "purple": "#bd93f9",
        "muted": "#6272a4",
        "source": "https://draculatheme.com/spec",
    },
    "catppuccin-mocha": {
        "fg": "#cdd6f4",
        "green": "#a6e3a1",
        "red": "#f38ba8",
        "amber": "#f9e2af",
        "blue": "#89b4fa",
        "cyan": "#89dceb",
        "purple": "#cba6f7",
        "muted": "#a6adc8",
        "source": "https://catppuccin.com/palette",
    },
}

ROLES = ("green", "red", "amber", "blue", "cyan", "purple", "muted")

# Icons whose idle ink carries a meaning, by id. Everything not listed here
# bakes the set's fg colour; state and accent colours stay runtime decisions.
# The browse-row and menu assignments follow the Tokyo Night tone maps in
# gridpage.c and main_menu.c so a set reads like the themed interface does.
ROLE = {}


def role(*ids, tone):
    for i in ids:
        ROLE[i] = tone


# Green: things that are fine, saved, or arriving.
role(
    "check", "circle_check", "library_checked", "podcast_downloaded",
    "bookmark_check", "download", "autoeq_update",
    "folder", "folder_corner", "folder_root", "folder_new",
    "battery", "battery_small", "battery_charging_body",
    "files_game", "wifi_transfer_quick", "quality_cd",
    "artist", "sc_reposts_glyph", "sub_artists_glyph",
    tone="green",
)
# Red: stopping, failing, removing, favourite-love marks in service rows.
role(
    "trash", "circle_alert", "list_x", "star_x", "playlist_overwrite_alert",
    "quality_dsd", "sc_likes_glyph", "qobuz_favourites_glyph",
    tone="red",
)
# Amber: charging, reading, work in progress.
role(
    "zap", "battery_charging_bolt", "airpods_charge",
    "files_audiobook", "files_book", "book_headphones",
    "book_headphones_row", "book_finished", "chapter", "bookmark",
    "year", "genre", "library_checking",
    "ebook_chapter", "ebook_font", "ebook_theme", "ebook_turn_fast",
    "ebook_turn_slide", "ebook_turn_vertical", "ebook_cover",
    "ebook_cover_small",
    "sonixlink_quick", "quality_hifi", "qobuz_genres_glyph",
    tone="amber",
)
# Blue: radios in the air -- Wi-Fi, Bluetooth, a second screen, a DNLA room.
role(
    "wifi", "wifi_zero", "wifi_low", "wifi_high", "wifi_max", "wifi_search",
    "bluetooth", "bluetooth_status", "bluetooth_search",
    "bluetooth_receiver", "bluetooth_connecting",
    "airplane_quick", "monitor", "dlna_quick", "artist_album",
    "sub_server_glyph", "sub_test_glyph", "sc_account_glyph",
    "qobuz_albums_glyph",
    tone="blue",
)
# Cyan: the audio path itself -- output, headphones, processing, podcasts.
role(
    "volume_mute", "volume_low", "volume_high",
    "headphones", "headphones_big", "equalizer", "mseb", "lineout", "nos",
    "audio_waveform", "usbaudioout",
    "airplay_status", "airplay_page", "airplay_quick",
    "peq_quick", "crossfeed_quick", "soundfield_quick", "replaygain_quick",
    "tag_manager", "tidal_mark",
    "music2", "list_music",
    "podcast_list", "podcast_cover", "podcast_episodes", "radio_quality",
    "quality_lossy", "sub_folders_glyph",
    tone="cyan",
)
# Purple: the library and the radio dial -- music, albums, stations, queues.
role(
    "music_note", "now_playing",
    "radio_player", "radio_recent", "radio_custom",
    "radio_language_glyph", "radio_country_glyph", "radio_genre_glyph",
    "radio_flylo_glyph",
    "album", "album_corner", "album_big", "most_played", "qobuz_mark",
    "qobuz_playlists_glyph", "sc_playlists_glyph",
    tone="purple",
)
# Muted: chrome that should sit behind the content it frames.
role(
    "music_settings", "chevron_left", "chevron_right", "chevron_up",
    "grip", "reorder", "control_center_line", "ellipsis_vertical",
    "loader_small", "loader_big",
    tone="muted",
)


def registry_entries():
    """(id, native size, svg file) for every swappable glyph."""
    out = []
    for filename, name, size in svg_to_lvgl.ICONS:
        if filename.endswith(".svg"):
            out.append((name, size, filename))
    for filename, name, size in svg_to_lvgl.COLOR_ICONS:
        if filename.endswith(".svg"):
            out.append((name, size, filename))
    return out


def render_size(size):
    """Four times native, capped at 256 on the long side, never below 1x."""
    w, h = size if isinstance(size, tuple) else (size, size)
    scale = 4 if max(w, h) * 4 <= 256 else max(1, 256 // max(w, h))
    return w * scale, h * scale


def render_glyph(filename, width, height):
    """The SVG's alpha at the target size, as an L-mode image."""
    png = resvg_py.svg_to_bytes(
        svg_path=os.path.join(ICON_SRC, filename),
        width=width,
        height=height,
        skip_system_fonts=True,
    )
    image = Image.open(io.BytesIO(png)).convert("RGBA")
    if image.size != (width, height):
        image = image.resize((width, height), Image.Resampling.LANCZOS)
    return image.getchannel("A")


def colorize(alpha, hex_color):
    """Fill the mask with one colour: the set's ink for this icon."""
    rgb = tuple(int(hex_color[i:i + 2], 16) for i in (1, 3, 5))
    image = Image.new("RGBA", alpha.size, rgb + (0,))
    image.putalpha(alpha)
    return image


def palette_text(name, spec):
    title = name.replace("-", " ").title()
    lines = [
        "; %s icon set -- the roles the function-matched glyphs use, from" % title,
        "; %s. Values are #RRGGBB; a missing role falls back to the theme." % spec["source"],
        "; The idle ink of every other icon is the set's text colour (%s)." % spec["fg"],
        "",
    ]
    for r in ROLES:
        lines.append("%s = %s" % (r, spec[r]))
    return "\n".join(lines) + "\n"


def generate_set(name):
    spec = SETS[name]
    out_dir = os.path.join(OUT_ROOT, name)
    os.makedirs(out_dir, exist_ok=True)

    with open(os.path.join(out_dir, "palette.ini"), "w") as f:
        f.write(palette_text(name, spec))

    entries = registry_entries()
    for ident, size, filename in entries:
        width, height = render_size(size)
        alpha = render_glyph(filename, width, height)
        ink = spec[ROLE.get(ident, "fg")] if ident in ROLE else spec["fg"]
        colorize(alpha, ink).save(
            os.path.join(out_dir, "%s.png" % ident), optimize=True
        )
    print("%s: %d icons + palette.ini" % (name, len(entries)))


def check_set(name):
    spec = SETS[name]
    out_dir = os.path.join(OUT_ROOT, name)
    problems = []

    palette_path = os.path.join(out_dir, "palette.ini")
    if not os.path.isfile(palette_path):
        problems.append("palette.ini missing")
    else:
        seen = {}
        with open(palette_path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith(";"):
                    continue
                if "=" not in line:
                    problems.append("palette line without =: %r" % line)
                    continue
                key, value = (part.strip() for part in line.split("=", 1))
                if key in seen:
                    problems.append("palette key twice: %s" % key)
                seen[key] = value
        for r in ROLES:
            value = seen.get(r)
            if value is None:
                problems.append("palette missing %s" % r)
            elif not (len(value) == 7 and value[0] == "#" and
                      all(c in "0123456789abcdefABCDEF" for c in value[1:])):
                problems.append("palette %s is not #RRGGBB: %r" % (r, value))
            elif value.lower() != spec[r].lower():
                problems.append("palette %s is %s, the theme says %s" %
                                (r, value, spec[r]))

    for ident, size, _filename in registry_entries():
        path = os.path.join(out_dir, "%s.png" % ident)
        if not os.path.isfile(path):
            problems.append("%s.png missing" % ident)
            continue
        with open(path, "rb") as f:
            head = f.read(26)
        if len(head) < 26 or head[12:16] != b"IHDR":
            problems.append("%s.png is not a PNG" % ident)
            continue
        width, height = struct.unpack(">II", head[16:24])
        want = render_size(size)
        if (width, height) != want:
            problems.append("%s.png is %dx%d, want %dx%d" %
                            ((ident, width, height) + want))

    if problems:
        for line in problems[:20]:
            print("  %s: %s" % (name, line))
        if len(problems) > 20:
            print("  %s: ... and %d more" % (name, len(problems) - 20))
        return False
    print("%s: ok (%d icons)" % (name, len(registry_entries())))
    return True


def main():
    if "--check" in sys.argv:
        ok = all(check_set(name) for name in SETS)
        sys.exit(0 if ok else 1)
    for name in SETS:
        generate_set(name)
    print("copy examples/icon-sets/<set> to Icons/<set> on the SD card")


if __name__ == "__main__":
    main()
