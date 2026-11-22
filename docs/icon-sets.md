# Icon sets

An icon set changes the artwork of the whole interface from the SD card —
no firmware rebuild, no flashing. Four sets ship with this player, one for
each ready-made theme palette:

| Set | Matches | Palette source |
| --- | --- | --- |
| `gruvbox` | examples/themes/gruvbox.ini | [morhetz/gruvbox](https://github.com/morhetz/gruvbox) |
| `rose-pine` | examples/themes/rose-pine.ini | [rosepinetheme.com](https://rosepinetheme.com/palette) |
| `dracula` | examples/themes/dracula.ini | [draculatheme.com/spec](https://draculatheme.com/spec) |
| `catppuccin-mocha` | examples/themes/catppuccin-mocha.ini | [catppuccin.com](https://catppuccin.com/palette) |

## Choosing one

1. Copy a folder from `examples/icon-sets/` to the card:

   ```
   Icons/
     gruvbox/
       palette.ini
       folder.png
       wifi_high.png
       ...
   ```

2. Settings → Appearance → **Icon set** opens a submenu. Every folder under
   `Icons/` is listed there; **Default** is the built-in glyphs. The choice is
   saved, applied at once, and restored on the next boot.

The list is re-read every time the submenu is opened, so a set copied over
USB while the player is running appears the next time you look.

## What a set contains

- **`<id>.png`** — one file per glyph, named after the icon's identifier in
  `src/gui/shell/icons.h` (`folder` → `folder.png`). A file the set doesn't ship
  keeps its built-in artwork, so partial sets are fine. Files are rendered at
  four times the native size (capped at 256 px) and downscaled on load; any
  size at or above the native size works, and RGBA with an alpha channel is
  required — the artwork's shape comes from that alpha.
- **`palette.ini`** — the seven colours the function-matched glyphs wear.
  Every key is optional; a missing one falls back to the active theme's own
  colour.

  ```ini
  green  = #b8bb26   ; folders, downloads, a good state
  red    = #fb4934   ; alerts, removals
  amber  = #fabd2f   ; charging, books, work in progress
  blue   = #83a598   ; Wi-Fi, Bluetooth, external screens
  cyan   = #8ec07c   ; volume, headphones, the audio path
  purple = #d3869b   ; music, albums, stations
  muted  = #a89984   ; chevrons and other chrome
  ```

## How it behaves

- Loading a set replaces the pixels behind the matching glyphs **in
  place**: every screen that already points at them changes with it, and
  unloading (choosing Default) puts the built-ins back exactly.
- The interface's own tinting still runs: the idle recolor (icons drawn in
  the text colour) gives way to the set's artwork on dark themes, while
  accent and state recolors — a selected tab, a low battery, the pressed
  transport button — keep painting their states as they always have. On the
  Light theme the set's art is tinted like any other icon.
- With a set loaded the interface also uses the function-matched layouts
  (the Tokyo Night glyph mode), and `palette.ini` supplies the role colours
  even over Tokyo, Amber or OLED — the artwork on screen brings its own
  colours.
- A folder that can't be read, or one that ships no icons the firmware
  knows, fails without changing anything: the previous choice stays and a
  toast says so.

## Making your own

The easiest way is to copy a shipped set and recolour it — every icon is a
plain RGBA PNG. For a full regeneration from the SVG sources:

```sh
pip install resvg-py pillow
python3 tools/gen_icon_sets.py            # writes examples/icon-sets/
python3 tools/gen_icon_sets.py --check    # verifies palette and file sizes
```

`tools/gen_icon_sets.py` holds the four palettes and the role table (which
icon wears green, which wears amber, ...); `tools/svg_to_lvgl.py` holds the
list of swappable ids, so new glyphs in `assets/icons/` show up in the sets
when it runs. Icons a set doesn't cover always fall back to the built-in
artwork, so a set may grow one file at a time.
