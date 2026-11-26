# Showcase website & GitHub Pages notes

The static showcase lives at the repository root:

- `index.html` — hero with the **YOUR PLAYER. Amplified.** headline, the real TempoTec V1 photo shown straight on (no tilted render or orange panel), the **Your music, ready when you are** pitch, release strip, feature / fork-addition sections, comparison, install steps, support/donation links and a single link out to the emulator.
- `assets/style.css`, `assets/app.js`, `assets/i18n.js`, `assets/i18n-extra.js`, `assets/tempotec-v1.png` — self-contained site assets. No frameworks, no build step. `i18n.js` and `i18n-extra.js` offer the same eleven languages as the firmware and translate the interface copy, with English fallback. The device screen crossfades between the Amber (Ember) and Tokyo Night screenshots in `screenshots/ember/` and `screenshots/tokyonight/`, which the workflow stages alongside the site.
- `coi-serviceworker.js` — copied to the site root for WASM integrations; the workbench itself registers its own copy from `./emulator/`.
- `tools/v1-emulator/public/` — the original emulator workbench. The deployed site serves it at `./emulator/`; its **Website** tab (desktop inspector tab bar and mobile bottom nav) links back to `../index.html`. In local native mode, where no sibling showcase is served, that tab falls back to the published GitHub Pages URL.

`.github/workflows/web-emulator-pages.yml` compiles the WASM firmware, tests the workbench at its original staged layout, then moves it to `dist/pages/emulator/`, copies the showcase files to `dist/pages/` and publishes that directory to GitHub Pages. Enable **Settings → Pages → Build and deployment → Source → GitHub Actions**.

## Two GitHub Pages gotchas

1. **SharedArrayBuffer / pthreads.** GitHub Pages does not allow custom `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers. `coi-serviceworker.js` is present at the site root and inside `emulator/`; the workbench registers its own copy for its scope, adding COOP/COEP/CORP to same-origin responses so Emscripten pthreads work on static HTTPS hosting, including iOS Safari. Keep the two copies in sync.
2. **Relative paths only.** Project sites are served from `https://<owner>.github.io/<repo>/`, not the domain root. Every local URL in the showcase is relative (`./assets/…`, `./emulator/…`, `./screenshots/…`); the workbench's catalog fetch is relative (`./builds/manifest.json`) and its Website tab uses `../index.html`. Do not replace them with absolute `/assets/…` paths.

## Live release data

`assets/app.js` reads the same-origin `assets/release.json` snapshot first, then calls the public GitHub Releases API to refresh it. It picks the `v1.upt` asset and fills in the version, date, short release notes and the primary download button. The workflow regenerates the snapshot on every deploy, so the strip still works when the API is rate-limited or blocked. If neither source is usable, the button falls back to `https://github.com/mr-f0xx/sonix-player-tempotec-v1/releases/latest`.

Two rules keep the page from ever advertising a version it is not shipping:

1. **The static HTML never pins a tag.** `index.html` links to `releases/latest/download/v1.upt` and `releases/latest/download/Icon.sets.Themes.zip`, which GitHub redirects to whichever release is newest. `app.js` then upgrades those links to the exact asset URLs of the live release. A pinned `releases/download/<tag>/…` in the HTML is a bug: if the snapshot is stale (or JavaScript is off) that link can only ever offer the old file. `tools/v1-emulator/tests/release-snapshot.test.mjs` fails if one comes back.
2. **A publish never moves backwards.** The *Refresh latest release snapshot* step only replaces `assets/release.json` when the live tag is strictly newer than the committed one, so a main run that started before a release was published cannot republish the older snapshot.

### Publishing is main-only

The `github-pages` environment admits the `main` branch only (Settings → Environments → github-pages → Deployment branches), so a tag push can build but never publish. 1.7.7 showed why that matters: its tag run died at *Deploy to GitHub Pages* with `Tag "1.7.7" is not allowed to deploy to github-pages` after the release was already cut, and the newest successful deploy predated the release, so the live page kept announcing 1.7.6 until the next push to main. The workflow now:

- deploys only from `main` (a tag still builds and uploads its `.sonix-web.zip` artifact),
- requests a `main` deploy from a tag run, so the release gets published without waiting for an unrelated push, and
- waits for the tagged release (and its `v1.upt`) before snapshotting it, because a tag run fires a moment before the release exists.

If you add a new tag policy to the environment instead, keep the deploy conditions in sync with it.

## Local preview

```sh
SITE=tools/v1-emulator/dist/preview
mkdir -p "$SITE/emulator" "$SITE/screenshots/ember" "$SITE/screenshots/tokyonight"
cp index.html coi-serviceworker.js "$SITE/"
cp -R assets/. "$SITE/assets/"
cp screenshots/ember/*.png "$SITE/screenshots/ember/"
cp screenshots/tokyonight/*.png "$SITE/screenshots/tokyonight/"
cp tools/v1-emulator/public/* "$SITE/emulator/"
python3 -m http.server 8080 --bind 0.0.0.0 --directory "$SITE"
```

The showcase and its links work immediately. To boot real firmware in the workbench, download the `sonix-v1-web-wasm-*` Actions artifact, extract the inner `.sonix-web.zip`, drop it into `emulator/builds/`, and write an `emulator/builds/manifest.json` with a `builds` entry pointing at that relative URL (or load the ZIP through the workbench's **Firmware Builds** panel). Multi-threaded WASM needs HTTPS or `localhost`; a plain LAN HTTP URL cannot provide SharedArrayBuffer.
