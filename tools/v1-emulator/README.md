# TempoTec Variations V1 — Interactive Browser Emulator

An interactive browser-based hardware emulator wrapping the real native C `sonix-player` firmware (LVGL 9.6, FreeType fonts, real SGM31324 RGB LED state machine, real audio decoders, and real single-flank TempoTec V1 button handling) — with **zero npm dependencies** and **zero SDL2/X11 requirements**.

Works natively on Linux, WSL2, macOS, and remote/container dev environments.

## Quick Start

From the repository root:

```bash
make -C sonix-player run-web-emu
```

Or build and start in separate steps:

```bash
make -C sonix-player -j$(nproc) web-emu
node tools/v1-emulator/server.mjs
```

Then open **http://localhost:8080** in your browser (override the port with `PORT=9000 node tools/v1-emulator/server.mjs`).

> If `libfreetype-dev` is not installed on the host, `make web-emu` automatically clones and builds static FreeType (`VER-2-13-3`) into `sonix-player/freetype-host-src/`. System `opusfile` and `wavpack` are auto-detected via `pkg-config` and gracefully stubbed when absent; all other decoders (MP3, FLAC, Vorbis, WAV/AIFF/CAF, ALAC, APE, MP4/AAC, DSD) are vendored in-tree.

## Features

- **TempoTec Variations V1 Device Frame**: Scaled interactive 240×320 RGB565 touch screen (`1×`, `1.5×`, `2×`, `2.5×`) supporting clicks, drags, kinetic scrolling, mouse-wheel scrolling, backlight dimming, display sleep/wake, and 180° screen rotation.
- **Physical Single-Flank Buttons**: Click, hold, or use keyboard shortcuts for `Power`, `Vol+`, `Vol−`, `Prev`, `Play/Pause`, and `Next`, including long-press Power Menu (~2s), hold-Power-to-boot while off, `Vol+ + Power` screenshot combo, and full in-firmware button remapping.
- **Live SGM31324 RGB LED**: Driven directly by `src/system/device/led.c` and `led_i2c.c` (steady and hardware breathing modes, 11-step current ladder, sample-rate and charging indicators).
- **Battery & Charger Simulator**: Real-time 0–100% slider, quick presets, and `Discharging` / `Charging` / `Full` / `Not charging` controls wired to `SONIX_BATTERY_CAPACITY` and `SONIX_BATTERY_STATUS`, plus one-click launch of the offline Charge Screen (`SONIX_CHARGE_SCREEN=1`).
- **Live Audio Output (Virtual ALSA DAC)**: Decoded stereo PCM streams from `sonix_player_web` over WebSocket into the browser's Web Audio API. Toggle **Audio: On** in the header to listen.
- **Screenshots**: Instant crisp 240×320 PNG download (`Screenshot PNG` / key `S`) plus firmware-native `Device Shot` saving PNGs into the virtual microSD `/Screenshots/`.
- **Virtual microSD & Demo Tracks**: Auto-seeds isolated runtime storage in `tools/v1-emulator/.runtime/` with 44.1 kHz and 96 kHz Hi-Res WAV demo tracks and sample notes. Upload your own audio, books, themes, fonts, or GB/GBC ROMs via drag-and-drop.
- **Live Console & Hot Rebuild**: Real-time filtered `stdout`/`stderr`/emulator/build log viewer and a one-click **Rebuild** button that runs `make -C sonix-player web-emu` and restarts the emulator.

## Keyboard Shortcuts

| Action | Keys |
|---|---|
| Power (tap: sleep/wake, hold ~2s: Power Menu) | `P` |
| Volume Up / Down | `↑` / `↓` or `+` / `-` |
| Previous / Play-Pause / Next | `←` / `Space` / `→` |
| Instant PNG Screenshot | `S` |

## Architecture

- **`sonix-player/webemu/`**: Self-contained `HOST_WEB_EMU` bridge kept outside `src/` so `make host` (SDL2) and `make target` (MIPS32r2) never compile it.
  - `webemu.c` / `webemu.h`: Headless LVGL 9.6 RGB565 display driver, pointer input ring buffer, synthetic flank buttons, and telemetry/PCM framing over `SONIX_WEB_EMU_SOCK`.
  - `alsa/asoundlib.h` + `webemu_alsa.c`: Virtual ALSA PCM/CTL shim that forwards software-volume-scaled S16LE PCM chunks to the browser.
- **`tools/v1-emulator/server.mjs`**: Pure Node.js HTTP + RFC 6455 WebSocket server managing `sonix_player_web`, virtual sysfs battery files, virtual microSD REST API, and Unix socket frame/telemetry/audio multiplexing.
- **`tools/v1-emulator/public/`**: Static HTML/CSS/JS workbench UI (`index.html`, `styles.css`, `app.js`).
