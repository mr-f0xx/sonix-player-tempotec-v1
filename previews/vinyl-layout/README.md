# Vinyl layout preview

Interactive visual concept for the optional **Vinyl** now-playing layout.

```sh
cd previews/vinyl-layout
python3 -m http.server 3000 --bind 0.0.0.0
```

Open http://localhost:3000. The **V1** canvas is exactly **240 × 320**, the
firmware panel size. The **Reference** canvas is **360 × 640** for comparison
with the supplied tall mockup; it is not a second device profile. Display
scale changes only the browser magnification. **Save PNG** exports the screen
at the selected canvas's exact pixel dimensions.

The demo transport, seek bar, favourite, repeat and missing-art fallback are
interactive but play no audio. The record is intentionally static, with clean
2 px concentric grooves in lifted greys so the exposed lower semicircle reads
clearly at the V1's native resolution, and with its small pressing mark matching the reference: a thin rounded stroke leaning down to the right in the exposed lower arc. The exposed arc ends above the deck with a clear gap; it does not overlap the names, seek bar or transport. The album
photograph is cropped from the user's reference for demonstration only and is
not shipped as a firmware asset.

Firmware: `sonix-player/src/gui/nowplaying/vinylayout.[ch]`, wired into
`player.c` as `PLAYER_LAYOUT_VINYL` and offered under Music → Display options.
The browser demo uses the same sizing rules with approximate browser font
metrics; the firmware measures its own faces. Geometry/layering regression: `make -C sonix-player/tests vinylayout`.
