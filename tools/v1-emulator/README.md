# TempoTec Variations V1 — Interactive Browser Emulator

An interactive browser-based hardware emulator wrapping the real C `sonix-player` firmware (LVGL 9.6, FreeType 2.13.3, SQLite3 library scanner, real SGM31324 RGB LED state machine, real audio decoders, and real single-flank TempoTec V1 button handling) — with **no backend required for WASM mode** and **zero SDL2/X11 requirements**.

Runs in **two modes** from the exact same UI:

1. **Static GitHub Pages / In-Browser WebAssembly Mode** (zero backend server): compiles `sonix-player` via Emscripten pthreads (`sonix_player_wasm.wasm`) and boots any `.sonix-web.zip` firmware bundle right inside your browser, with a Published Builds dropdown, drag-and-drop loader, URL fetcher (`?build=<url>`), and persistent browser IndexedDB storage for virtual microSD tracks, `device_config.ini`, and custom builds.
2. **Local Native Bridge Mode**: wraps a native host binary (`sonix_player_web`) over a Unix socket and Node.js WebSocket server (`tools/v1-emulator/server.mjs`) for instant local C development and one-click hot rebuilds.

## Option A: Static Webpage on GitHub Pages (WebAssembly, Zero Install)

The repository includes `.github/workflows/web-emulator-pages.yml`, which automatically:
- Compiles `sonix-player` to Emscripten WebAssembly (`make -C sonix-player web-wasm`) on every push, PR, tag, and manual dispatch.
- Packs a self-contained **`.sonix-web.zip`** bundle (`manifest.json` + `sonix_player_wasm.js` + `sonix_player_wasm.wasm` + TempoTec V1 `usr/` assets) and uploads it as a GitHub Actions artifact for every commit/PR.
- Deploys the static emulator webpage and build history catalog (`builds/manifest.json`) to **GitHub Pages** on pushes to `main`, tags, or manual dispatch.

The showcase homepage lives one directory above this workbench on GitHub Pages
(`../index.html`). It links out to the emulator instead of embedding it, and the
workbench links back through the **Website** tab in the inspector tab bar (and
**Site** in the mobile bottom navigation). On a local Node bridge, where there
is no sibling showcase, that tab opens the published GitHub Pages site instead.

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

- **GitHub Pages (In-Browser WASM)**: Open the HTTPS GitHub Pages URL directly in iOS Safari (16.4+) or Android Chrome/Firefox. On mobile viewports (`<= 768px`), the TempoTec Variations V1 auto-fits to your screen with compact thumb-friendly side buttons, multi-touch button combos, long-press support, Web Audio unlock, and a bottom navigation bar (`V1`, `Builds`, `Hardware`, `microSD`, `USB`, `Logs`). You can also use **Add to Home Screen** for a full-screen simulator view.
- **Local Wi-Fi (Native Bridge)**: Run `make -C sonix-player run-web-emu` on your desktop and open the printed `Mobile / LAN URL` (`http://<lan-ip>:8080`) on your phone over the same Wi-Fi network.

### Live USB: connect a physical V1

Open the Pages site directly in a recent **Chrome** or another browser with
**WebUSB** support. On Android, the phone must support USB host/OTG; use a
data-capable cable or OTG adapter. Firefox and Safari can use the local editors
but cannot connect over USB. Embedded previews can block USB permission even
when the browser supports it.

1. On the V1, tap **Settings → System → About → Build number** five times if
   needed, then turn on **Settings → Developer options → ADB**. Leave the
   microSD in the player and don't switch to DAC or USB mass-storage mode.
2. Plug in the player, open **Live USB** (**USB** on mobile), and click
   **Connect V1**. Choose the V1's ADB device and accept any debugging prompt on
   the player. If another app is using the ADB interface, close it (for desktop
   `adb`, run `adb kill-server`) and reconnect. On Linux the normal ADB USB
   device permissions still apply; on Windows the ADB interface must be
   accessible to the browser through WinUSB.
3. Use **Disconnect** when finished, then turn ADB off on the V1 if not needed.

| Tool | What it does on the real device |
| --- | --- |
| **Parametric EQ** | Draw the response with draggable bands, or set frequency, gain, Q, and preamp numerically. Open an existing single-curve APO preset or export one locally. **Save new preset to V1** writes `PEQ/<name>.txt`; select it on the V1 to hear it. The graph is a 48 kHz estimate, not a live DSP readout. |
| **Tape Deck pads** | Assign or swap WAVs across 16 pads. Save as a numbered folder in `Drum Kits/` and select it with **Tape Deck → Pads → Kits → Load drum kit**. Use mono or stereo PCM WAV (8–96 kHz, 8/16/24/32-bit, up to 16 MiB per pad). This doesn't change the current kit or a saved sequence. |
| **Playlists** | Open an M3U/M3U8 file from the browser or from the card root, including V1 backups in `Playlist/`. To edit an existing library playlist, choose **Backup** in its V1 menu first. Edit or reorder track paths, save a **new** M3U at the card root, then use **Playlists → Import** on the V1. The browser doesn't edit the library database. |
| **Debug log** | Enable **Log to microSD** on the player, reproduce the issue, then click **Download debug log**. This reads `.local/sonix_player.log` (up to 32 MiB). For larger logs or the full diagnostic bundle, see [Collecting logs](../../docs/collecting-logs.md). Review logs for private paths before sharing. |

The browser uses ADB over WebUSB because the V1 firmware doesn't expose a USB
serial gadget for WebSerial. Device transfers stay between the browser and the
player: there is no server upload, and the ADB key (if needed by the device)
is stored by the browser for this site. Use a trusted site and only load
trusted firmware bundles. Saves require a new name and never intentionally
replace an existing file. If USB is unplugged mid-write, a hidden
`.sonix-remote-*` staging file or folder can be left on the card; it can be
removed once no transfer is active. The editors don't touch the emulator's
virtual microSD. Export or save your edits before reloading the page.

#### Test on hardware before publishing

- With the card mounted, connect a real V1 from a desktop WebUSB browser. Save
  a fresh preset and confirm it appears and applies on the V1. Try the same
  name again and verify the original stays intact.
- Save a kit with pads 01 and 16 populated and verify the gaps and sounds on
  the player. Back up a playlist, open it in the browser, reorder its tracks,
  save a new name, and import it on the player.
- Download a nonempty log and compare with one copied directly from the card.
  Unplug during a transfer and verify the UI disables device actions and can
  reconnect. Repeat the connection and a small transfer in Android Chrome with
  an OTG cable. The automated tests use simulated ADB, not physical hardware.

### Building a `.sonix-web.zip` Bundle Locally (Emscripten)

With [Emscripten](https://emscripten.org/) (`emcc` / `em++`) on your `PATH`:

```bash
# Build sonix_player_wasm.{js,wasm} and pack tools/v1-emulator/dist/*.sonix-web.zip
make -C sonix-player -j$(nproc) web-wasm-bundle

# Build the optional USB transport, then stage the static site as CI does
npm ci --prefix tools/v1-emulator/remote
npm run build --prefix tools/v1-emulator/remote
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

- **TempoTec Variations V1 Device Frame**: Scaled interactive 240×320 RGB565 touch screen (`Fit`, `Real size`, `1×`, `1.25×`, `1.5×`, `2×`) supporting clicks, drags, kinetic scrolling, mouse-wheel scrolling, backlight dimming, display sleep/wake, and 180° screen rotation. The front chin spans the full glass width and carries the vertical-pill RGB LED in the bottom-left corner and the brushed `V1` wordmark in the bottom-right. Both are placed to match the Sonix Player 1.7.1.1 banner, as fractions of the glass panel: the LED starts 10.1 % in from the left and its lower end sits 13.3 % above the bottom (a 2.3 % × 11.5 % capsule with a tight halo), and the wordmark's silver ink is 17.8 % of the width, ending 11.7 % in from the right and 13.7 % up — the same centre line as the LED.
- **Real size (true hardware dimensions)**: The chassis is modelled in the panel's own device pixels — 0.127 mm each, so the 2.0″ 240×320 LCD is 30.48 × 40.64 mm inside the real 41.8 × 87.2 × 13.75 mm body. **Real size** draws the player life size. Because a browser's CSS millimetre is a virtual unit (1/96 in whatever the display is), the scale is solved from the best millimetre reading available, in this order:
  1. **The panel itself**, for phones and tablets: a portrait CSS viewport plus `devicePixelRatio` identifies the display (a 390 px viewport at DPR 3 is a 460 ppi iPhone screen), so the density is the real one and the draw is genuinely 1:1. The readout names the panel.
  2. **The display's reported DPI**, if the browser exposes something other than 96 dpi (measured off a hidden 1 in probe, which also folds in OS scaling and browser zoom).
  3. **The 96 dpi CSS reference** on ordinary desktops. This is a guess, so the badge reads `Screen ≈1:1` and the readout says the size is approximate instead of claiming a bare 1:1.

  The badge and readout report the millimetres actually rendered, and on a phone the stage scrolls to the life-size player rather than shrinking it — the drawn dimensions stay correct, so the mock-up can be held against the real device.
- **Fixed scales**: `1×`, `1.25×`, `1.5×`, `2×` are relative to the panel's *native* 240×320 pixels, i.e. 0.73/0.91/1.09/1.46 CSS px per device px, drawing 175/218/262/350 px panels. They keep the apparent sizes the emulator has always had now that the chassis is proportioned like the real V1 (it used to be modelled at 266×515 px instead of 329×687). Phones open in **Fit**; desktop opens at 1.25×.
- **Authentic Rear Shell**: Flipping the device shows the real back from owner photos — the moulded antenna cap across the top ~21%, the smooth aluminium battery door whose edge steps *up* to the right with a copper chamfer insert bedded in the riser, the exposed silver Torx screw, the ~45° milled rib field (crosshatch diamond finish) fading into shadow toward the lower left, and the TempoTec piano-key badge and wordmark printed bottom-to-top up the lower right.
- **Physical Single-Flank Buttons**: Click, hold, or use keyboard shortcuts for `Power`, `Vol+`, `Vol−`, `Prev`, `Play/Pause`, and `Next`, including long-press Power Menu (~2s), hold-Power-to-boot while off, `Vol+ + Power` screenshot combo, and full in-firmware button remapping.
- **Live SGM31324 RGB LED**: Driven directly by `src/system/device/led.c` and `led_i2c.c` (steady and hardware breathing modes, 11-step current ladder, sample-rate and charging indicators).
- **Battery & Charger Simulator**: Real-time 0–100% slider, quick presets, and `Discharging` / `Charging` / `Full` / `Not charging` controls wired to `SONIX_BATTERY_CAPACITY` and `SONIX_BATTERY_STATUS`, plus one-click launch of the offline Charge Screen (`SONIX_CHARGE_SCREEN=1`).
- **Live Audio Output (Virtual ALSA DAC)**: Decoded stereo PCM streams from `sonix_player_web` over WebSocket into the browser's Web Audio API. Toggle **Audio: On** in the header to listen.
- **Screenshots**: Instant crisp 240×320 PNG download (`Screenshot PNG` / key `S`) plus firmware-native `Device Shot` saving PNGs into the virtual microSD `/Screenshots/`.
- **Virtual microSD & Demo Tracks**: Auto-seeds isolated runtime storage in `tools/v1-emulator/.runtime/` with 44.1 kHz and 96 kHz Hi-Res WAV demo tracks and sample notes. Upload your own audio, books, themes, fonts, or GB/GBC ROMs via drag-and-drop.
- **Live Console & Hot Rebuild**: Real-time filtered `stdout`/`stderr`/emulator/build log viewer and a one-click **Rebuild & boot local** button that runs `make -C sonix-player web-emu` and restarts the emulator.

## Checking the loader

The emulator core and native bridge have no npm dependencies. Building the optional WebUSB transport installs pinned dependencies in `remote/`. Loader tests use Node 22; the browser smoke test also uses Playwright Core (test-only) and an installed Chrome. The Pages workflow runs it against the compiled firmware before publishing.

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
