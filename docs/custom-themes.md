# Custom SD-card theme

Copy [`sonix-player/examples/custom-theme.ini`](../sonix-player/examples/custom-theme.ini)
onto the SD card in **`Themes/`** (keep any `.ini` filename) (create `Themes` at the card root).
Edit it with a text editor, safely eject the card, then open **Settings →
Appearance → Custom**, then tap the filename to select it. To see subsequent edits, return to Appearance (or tap
Custom again). The choice persists across restarts as long as the card and
selected file is available at startup; if not, the player starts in Dark. Selecting
Custom with a missing or invalid file shows an error and keeps the current
appearance. Editing a selected Custom theme while the player is running takes
effect when Appearance is reopened; invalid edits leave the last valid palette
in memory.

## Ready-made palettes

Four complete palettes ship in [`sonix-player/examples/themes/`](../sonix-player/examples/themes/),
one file per scheme, each mapped from the official colours of its theme:

| File | Scheme | Source |
| --- | --- | --- |
| [`gruvbox.ini`](../sonix-player/examples/themes/gruvbox.ini) | Gruvbox (dark) | [gruvbox.org](https://gruvbox.org/) |
| [`rose-pine.ini`](../sonix-player/examples/themes/rose-pine.ini) | Rosé Pine (Main) | [rosepinetheme.com/palette](https://rosepinetheme.com/palette) |
| [`dracula.ini`](../sonix-player/examples/themes/dracula.ini) | Dracula (Classic) | [draculatheme.com/spec](https://draculatheme.com/spec) |
| [`catppuccin-mocha.ini`](../sonix-player/examples/themes/catppuccin-mocha.ini) | Catppuccin Mocha | [catppuccin.com/palette](https://catppuccin.com/palette) |

Copy as many as you want into **`Themes/`**, keeping their original `.ini` names.
In Settings → Appearance, open **Custom theme** to see the files without their
`.ini` extensions; choose one to apply it. `catppuccin-mocha.ini` is displayed
as **Catppuccin**. Only one is active at a time, and its filename is remembered
across restarts. Existing `Themes/custom.ini` cards continue to work. Each file
is a complete eight-key palette, so no editing is required; tweak any value if
you like (for example Dracula's `text_secondary` is its deliberately dim
Comment grey). The `sonix-player/tools/validate_theme_ini.py` script checks a
file against the player's parser.

Every key in `[theme]` is required, each with a six-digit `#RRGGBB` value.
Lines starting with `;` or `#` are comments. `screen_bg` is the page,
`surface` the cards and rows, `surface_pressed` their pressed state, `panel`
the status bar and player controls, `cover_bg` the empty cover, `text_primary`
and `text_secondary` the foreground, and `accent` the highlight. Custom uses
the standard dark layout and one accent for functional icons. Some player
chrome and semantic highlights use built-in colours; this is a palette theme,
not an icon/font replacement. The **Tokyo layout for custom themes** switch in
Appearance opts Custom into the same function-matched icon and compact
layouts that Tokyo Night, Amber on Black and OLED use — with or without an SD
icon set selected. Dynamic tint, if enabled in Appearance,
can alter the background and surface colours further. Accent presets override
the file’s `accent` when a non-default preset is selected; select the first
accent circle to use the file’s own colour.
