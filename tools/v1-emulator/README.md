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

### Testing a firmware change

- **Latest published firmware:** open the site normally. It boots the catalog's `defaultBuildId` (or the newest `builtAt` in an older catalog). While following latest, a visible tab checks the catalog every minute and on focus. **Boot latest** checks immediately, even if the bundle was rebuilt at the same URL.
- **Your branch or PR:** open the **TempoTec V1 Web Emulator (WASM & GitHub Pages)** Actions run for the commit you want. Download **`sonix-v1-web-wasm-<short SHA>`**. In **Firmware Builds → Test your web build**, browse to or drop the downloaded artifact ZIP **as is**. The inner `.sonix-web.zip` also works. Ordinary branch/PR builds do not replace the public Pages site.
- **Uncommitted edits:** in local native mode click **Rebuild & boot local** after each edit (no Emscripten needed). To test WebAssembly instead, run `make -C sonix-player -j$(nproc) web-wasm-bundle` after each change and drop the new `tools/v1-emulator/dist/*.sonix-web.zip`. You can select the same filename again. A local server opened at `/?mode=wasm` can follow these bundles from `dist/` directly.
- **A specific revision:** choose a published or previously imported build and click **Boot selected**, paste a direct bundle URL and click **Fetch**, or share `?build=<URL-encoded bundle URL>`. Remote hosts must allow CORS. These actions pin that build for the session; only **Boot latest** resumes following updates. An ordinary page reload without `?build` goes back to latest in WASM mode.

The result in **Firmware Builds** shows the booted revision and build time **after the firmware sends its first display frame**. Load errors stay in that panel and Live Logs; a failed explicit link does not fall back to unrelated firmware. Imported builds, virtual microSD and config use browser storage (memory only if storage is restricted).

**Previewing network screens:** In the player’s **Streaming** menu, open **Network folders → WebDAV** (or **FTP**) to try the profile fields and on-screen keyboard, or open **Listen Together** to inspect its controls. In WebAssembly, **Connect**, **Start Hosting**, and **Join Stream** are disabled: no server is contacted and no peers are discovered. Use the device or the local native bridge to exercise the native networking code. Older imported builds keep the menus they were compiled with.

**Which ZIP?** `sonix-tempotec-v1-…` and release `v1.upt` files are MIPS device updates, not browser programs. Use the **web** artifact from the same commit; the emulator runs that revision of the player, not the OTA kernel. For a device Actions artifact, `BUILD-REPORT.txt` gives the commit to use as `build_ref` in the web-emulator workflow (leave **deploy_pages** unchecked for a personal test). Only import web bundles you trust: they contain executable JavaScript as well as WASM.

> On GitHub Pages, `coi-serviceworker.js` automatically attaches `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`, and `Cross-Origin-Resource-Policy: cross-origin` headers on first visit so `SharedArrayBuffer` / WebAssembly pthreads work on static HTTPS hosting across desktop and mobile browsers. If an embedded preview is not isolated, use **Open emulator in a new tab** and select your bundle there. Plain HTTP over a LAN can use the native bridge, not WebAssembly pthreads.

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

Or start the bridge directly; on startup it runs incremental `make web-emu` before booting, even if a binary already exists:

```bash
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
- **Live Console & Hot Rebuild**: Real-time filtered `stdout`/`stderr`/emulator/build log viewer and a one-click **Rebuild & boot local** button that runs `make -C sonix-player web-emu` and restarts the emulator.

## Checking the loader

The emulator itself has no npm dependencies. Loader tests use Node 22; the browser smoke test also uses Playwright Core (test-only) and an installed Chrome. The Pages workflow runs it against the compiled firmware before publishing.

```bash
node --test tools/v1-emulator/tests/firmware.test.mjs
npm ci --prefix tools/v1-emulator/tests
SONIX_TEST_BUNDLE=/absolute/path/to/build.sonix-web.zip npm test --prefix tools/v1-emulator/tests
```

Use `CHROME_BIN=/path/to/chromium` if Chrome isn't in its standard location.

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
