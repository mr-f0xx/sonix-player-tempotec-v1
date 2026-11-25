# Showcase website & GitHub Pages notes

The static showcase lives at the repository root:

- `index.html` — hero with the **YOUR PLAYER. Amplified.** headline, the real TempoTec V1 photo shown straight on (no tilted render or orange panel), the **Your music, ready when you are** pitch, release strip, feature / fork-addition sections, comparison, install steps and a single link out to the emulator.
- `assets/style.css`, `assets/app.js`, `assets/tempotec-v1.png` — self-contained site assets. No frameworks, no build step. The device screen is composited from `screenshots/ember/now-playing.png`, which the workflow stages alongside the site.
- `coi-serviceworker.js` — copied to the site root for WASM integrations; the workbench itself registers its own copy from `./emulator/`.
- `tools/v1-emulator/public/` — the original emulator workbench. The deployed site serves it at `./emulator/`; its **Website** tab (desktop inspector tab bar and mobile bottom nav) links back to `../index.html`. In local native mode, where no sibling showcase is served, that tab falls back to the published GitHub Pages URL.

`.github/workflows/web-emulator-pages.yml` compiles the WASM firmware, tests the workbench at its original staged layout, then moves it to `dist/pages/emulator/`, copies the showcase files to `dist/pages/` and publishes that directory to GitHub Pages. Enable **Settings → Pages → Build and deployment → Source → GitHub Actions**.

## Two GitHub Pages gotchas

1. **SharedArrayBuffer / pthreads.** GitHub Pages does not allow custom `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers. `coi-serviceworker.js` is present at the site root and inside `emulator/`; the workbench registers its own copy for its scope, adding COOP/COEP/CORP to same-origin responses so Emscripten pthreads work on static HTTPS hosting, including iOS Safari. Keep the two copies in sync.
2. **Relative paths only.** Project sites are served from `https://<owner>.github.io/<repo>/`, not the domain root. Every local URL in the showcase is relative (`./assets/…`, `./emulator/…`, `./screenshots/…`); the workbench's catalog fetch is relative (`./builds/manifest.json`) and its Website tab uses `../index.html`. Do not replace them with absolute `/assets/…` paths.

## Live release data

`assets/app.js` calls the public GitHub Releases API, picks the `v1.upt` asset and fills in the version, date, short release notes and the primary download button. If the API is unavailable or rate-limited, the button falls back to `https://github.com/mr-f0xx/sonix-player-tempotec-v1/releases/latest`, which always serves the newest asset.

## Local preview

```sh
SITE=tools/v1-emulator/dist/preview
mkdir -p "$SITE/emulator" "$SITE/screenshots/ember"
cp index.html coi-serviceworker.js "$SITE/"
cp -R assets "$SITE/assets"
cp screenshots/ember/now-playing.png "$SITE/screenshots/ember/"
cp tools/v1-emulator/public/* "$SITE/emulator/"
python3 -m http.server 8080 --bind 0.0.0.0 --directory "$SITE"
```

The showcase and its links work immediately. To boot real firmware in the workbench, download the `sonix-v1-web-wasm-*` Actions artifact, extract the inner `.sonix-web.zip`, drop it into `emulator/builds/`, and write an `emulator/builds/manifest.json` with a `builds` entry pointing at that relative URL (or load the ZIP through the workbench's **Firmware Builds** panel). Multi-threaded WASM needs HTTPS or `localhost`; a plain LAN HTTP URL cannot provide SharedArrayBuffer.
