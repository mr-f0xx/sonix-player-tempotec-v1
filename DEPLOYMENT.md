# Showcase website & GitHub Pages notes

The static showcase lives at the repository root:

- `index.html` — hero with a V1 chassis and device screenshot, feature cards, install steps and release download panel. It links out to the emulator; it does **not** embed it.
- `assets/css/styles.css`, `assets/js/main.js`, `assets/favicon.svg`, `assets/screenshots/` — self-contained assets. No frameworks, no build step.
- `coi-serviceworker.js` — served from the site root; keeps the project origin cross-origin isolated.
- `tools/v1-emulator/public/` — the original emulator workbench. The deployed site serves it at `./emulator/`; its **Website** tab (desktop inspector tab bar and mobile bottom nav) links back to `../index.html`. In local native mode, where no sibling showcase is served, that link falls back to the published GitHub Pages URL.

`.github/workflows/web-emulator-pages.yml` compiles the WASM firmware, stages the workbench at `dist/pages/emulator/` (including its `builds/` catalog), copies the showcase files to `dist/pages/`, and publishes that directory to GitHub Pages. Enable **Settings → Pages → Build and deployment → Source → GitHub Actions**.

## Two GitHub Pages gotchas

1. **SharedArrayBuffer / pthreads.** GitHub Pages does not allow custom `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers. `coi-serviceworker.js` is present at the site root and inside `emulator/`; each registers a service worker for its own scope, adding COOP/COEP/CORP to same-origin responses. The workbench is therefore cross-origin isolated when opened on its own, so Emscripten pthreads work on static HTTPS hosting, including iOS Safari. Keep the two copies in sync.
2. **Relative paths only.** Project sites are served from `https://<owner>.github.io/<repo>/`, not the domain root. Every local URL in the showcase is relative (`./assets/…`, `./emulator/…`, `./coi-serviceworker.js`); the workbench's catalog fetch is relative (`./builds/manifest.json`) and its Website tab uses `../index.html`. Do not replace them with absolute `/assets/…` paths.

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

The showcase and its links work immediately. To boot real firmware in the workbench, download the `sonix-v1-web-wasm-*` Actions artifact, extract the inner `.sonix-web.zip`, drop it into `emulator/builds/`, and write an `emulator/builds/manifest.json` with a `builds` entry pointing at that relative URL (or load the ZIP through the workbench's **Firmware Builds** panel). Multi-threaded WASM needs HTTPS or `localhost`; a plain LAN HTTP URL cannot provide SharedArrayBuffer.
