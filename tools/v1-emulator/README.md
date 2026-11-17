# TempoTec Variations V1 — Interactive Browser Emulator

An interactive browser-based hardware emulator wrapping the real C `sonix-player` firmware (LVGL 9.6, FreeType 2.13.3, SQLite3 library scanner, real SGM31324 RGB LED state machine, real audio decoders, and real single-flank TempoTec V1 button handling) — with **zero npm dependencies** and **zero SDL2/X11 requirements**.

Runs in **two modes** from the exact same UI:

1. **Static GitHub Pages / In-Browser WebAssembly Mode** (zero backend server): compiles `sonix-player` via Emscripten pthreads (`sonix_player_wasm.wasm`) and boots any `.sonix-web.zip` firmware bundle right inside your browser, with a Published Builds dropdown, drag-and-drop loader, URL fetcher (`?build=<url>`), and persistent browser IndexedDB storage for virtual microSD tracks, `device_config.ini`, and custom builds.
2. **Local Native Bridge Mode**: wraps a native host binary (`sonix_player_web`) over a Unix socket and Node.js WebSocket server (`tools/v1-emulator/server.mjs`) for instant local C development and one-click hot rebuilds.

## Option A: Static Webpage on GitHub Pages (WebAssembly, Zero Install)

The repository includes `.github/workflows/web-emulator-pages.yml`, which automatically:
- Compiles `sonix-player` to Emscripten WebAssembly (`make -C sonix-player web-wasm`) on every push, PR, tag, and manual dispatch.
- Packs a self-contained **`.sonix-web.zip`** bundle (`manifest.json` + `sonix_player_wasm.js` + `sonix_player_wasm.wasm` + TempoTec V1 `usr/` assets) and uploads it as a GitHub Actions artifact for every commit/PR.
- Deploys the static emulator webpage and build history catalog (`builds/manifest.json`) to **GitHub Pages** on pushes to `main`, tags, or manual dispatch.

### One-Time Repository Setup for GitHub Pages

In your GitHub repository: **Settings → Pages → Build and deployment → Source → GitHub Actions**. Once enabled, pushes to `main` (or running the `TempoTec V1 Web Emulator (WASM & GitHub Pages)` workflow manually) publish the clickable site at:

```text
https://<owner>.github.io/<repo>/
```

### Loading Any Sonix Firmware Build in the Browser

Open the **Firmware Builds** tab in the emulator:
- **Published Sonix Builds**: pick any CI-published build from `builds/manifest.json` (or previously imported custom builds saved in IndexedDB) and click **Boot**.
- **Drag & Drop / Browse `.sonix-web.zip`**: drop any `.sonix-web.zip` artifact downloaded from GitHub Actions, Releases, or built locally onto the dropzone (or anywhere on the page).
- **Load From URL**: paste a direct HTTPS URL to any `.sonix-web.zip` bundle, or share a direct link using `?build=https://.../sonix-v1-....sonix-web.zip`.

> On GitHub Pages, `coi-serviceworker.js` automatically attaches `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`, and `Cross-Origin-Resource-Policy: cross-origin` headers on first visit so `SharedArrayBuffer` / WebAssembly pthreads work on static HTTPS hosting across desktop and mobile browsers (including iOS Safari 16.4+, Android Chrome, and Firefox Mobile).

### Mobile Devices (iPhone, iPad, Android)

- **GitHub Pages (In-Browser WASM)**: Open the HTTPS GitHub Pages URL directly in iOS Safari (16.4+) or Android Chrome/Firefox. On mobile viewports (`<= 768px`), the TempoTec Variations V1 auto-fits to your screen with compact thumb-friendly side buttons, multi-touch button combos, long-press support, Web Audio unlock, and a bottom navigation bar (`V1`, `Builds`, `Hardware`, `microSD`, `Logs`). You can also use **Add to Home Screen** for a full-screen simulator view.
- **Local Wi-Fi (Native Bridge)**: Run `make -C sonix-player run-web-emu` on your desktop and open the printed `Mobile / LAN URL` (`http://<lan-ip>:8080`) on your phone over the same Wi-Fi network.

### Building a `.sonix-web.zip` Bundle Locally (Emscripten)

With [Emscripten](https://emscripten.org/) (`emcc` / `em++`) on your `PATH`:

```bash
# Build sonix_player_wasm.{js,wasm} and pack tools/v1-emulator/dist/*.sonix-web.zip
make -C sonix-player -j$(nproc) web-wasm-bundle

# Or also stage a ready-to-host static site (identical to GitHub Pages output)
node tools/v1-emulator/pack-wasm-bundle.mjs --stage-pages tools/v1-emulator/dist/pages
```

You can also test WASM mode directly against `server.mjs` by visiting **http://localhost:8080/?mode=wasm** (it serves `tools/v1-emulator/dist/` at `/builds/*` with COOP/COEP headers).

## Option B: Local Native Mode (Node.js + Native C Binary)

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
  - `webemu.c` / `webemu.h`: Native headless LVGL 9.6 RGB565 display driver, pointer input ring buffer, synthetic flank buttons, and telemetry/PCM framing over `SONIX_WEB_EMU_SOCK`.
  - `webemu_wasm.c`: Emscripten WebAssembly backend dispatching RGB565 frames, JSON hardware telemetry, and ALSA PCM chunks directly into JS (`Module._sonixBridge`) with exported pointer/key/screenshot/quit controls.
  - `alsa/asoundlib.h` + `webemu_alsa.c`: Virtual ALSA PCM/CTL shim that forwards software-volume-scaled S16LE PCM chunks to the browser (used by both native and WASM builds).
  - `wasm_compat/`: Minimal Linux/POSIX compatibility headers (`linux/input.h`, `linux/i2c-dev.h`, `sys/eventfd.h`, etc.) included only by `make web-wasm`.
- **`tools/v1-emulator/pack-wasm-bundle.mjs`**: Zero-dependency `.sonix-web.zip` packager and `--stage-pages` GitHub Pages site + `builds/manifest.json` catalog generator.
- **`tools/v1-emulator/server.mjs`**: Pure Node.js HTTP + RFC 6455 WebSocket server managing `sonix_player_web`, virtual sysfs battery files, virtual microSD REST API, `/builds/*` WASM catalog serving, and Unix socket frame/telemetry/audio multiplexing.
- **`tools/v1-emulator/public/`**: Static HTML/CSS/JS workbench UI (`index.html`, `styles.css`, `app.js`, `wasm-runtime.js`, `coi-serviceworker.js`) that auto-detects static GitHub Pages (WASM) vs local Node server mode.
- **`.github/workflows/web-emulator-pages.yml`**: CI workflow building `.sonix-web.zip` bundles with Emscripten and deploying the static site to GitHub Pages.
