# Showcase website & GitHub Pages notes

The static showcase lives at the repository root:

- `index.html` — hero, live emulator frame, feature cards, install steps and release download panel.
- `assets/css/styles.css`, `assets/js/main.js`, `assets/favicon.svg`, `assets/screenshots/` — self-contained assets. No frameworks, no build step.
- `coi-serviceworker.js` — must be served from the **site root** (keep in sync with `tools/v1-emulator/public/coi-serviceworker.js`).
- `tools/v1-emulator/public/` — the actual emulator workbench. It is embedded in the hero iframe from `./emulator/index.html?embed=1&mode=wasm`.

`.github/workflows/web-emulator-pages.yml` compiles the WASM firmware, stages the workbench at `dist/pages/emulator/` (including its `builds/` catalog), copies the showcase files to `dist/pages/`, and publishes that directory to GitHub Pages. Enable **Settings → Pages → Build and deployment → Source → GitHub Actions**.

## Two GitHub Pages gotchas

1. **SharedArrayBuffer / pthreads.** GitHub Pages does not allow custom `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers. `coi-serviceworker.js` is loaded by `index.html` at the root of the site: it registers a service worker with root scope, which adds COOP/COEP/CORP to same-origin responses and makes both the top page and the iframe cross-origin isolated. Emscripten pthreads then work on static HTTPS hosting, including iOS Safari. The workbench carries its own copy for standalone use; its script deliberately skips registration inside an iframe, so the root worker governs the embedded view. If isolation fails, use the **Full workbench** link above the emulator.
2. **Relative paths only.** Project sites are served from `https://<owner>.github.io/<repo>/`, not the domain root. Every local URL in the showcase is relative (`./assets/…`, `./emulator/…`, `./coi-serviceworker.js`); the workbench's own catalog fetch is relative (`./builds/manifest.json`). Do not replace those with absolute `/assets/…` paths.

## Live release data

`assets/js/main.js` calls the public GitHub Releases API for the newest release, picks the `v1.upt` asset and fills in the version, date, size and both primary download buttons. If the API is unavailable or rate-limited, the buttons fall back to `https://github.com/mr-f0xx/sonix-player-tempotec-v1/releases/latest`, which always serves the newest asset.

## Local preview

```sh
SITE=tools/v1-emulator/dist/preview
mkdir -p "$SITE/emulator"
cp index.html coi-serviceworker.js "$SITE/"
cp -R assets "$SITE/assets"
cp tools/v1-emulator/public/* "$SITE/emulator/"
python3 -m http.server 8080 --bind 0.0.0.0 --directory "$SITE"
```

The layout and release panel work immediately. To boot the real firmware in the iframe, download the `sonix-v1-web-wasm-*` Actions artifact, extract the inner `.sonix-web.zip`, drop it into `emulator/builds/`, and write an `emulator/builds/manifest.json` with a `builds` entry pointing at that relative URL (or load the ZIP through the workbench's **Firmware Builds** panel). Multi-threaded WASM needs HTTPS or `localhost`; a plain LAN HTTP URL cannot provide SharedArrayBuffer.
