# Jellyfin on the TempoTec V1

Native Jellyfin music client for `sonix_player`, connecting directly to your self-hosted Jellyfin server over HTTP or HTTPS with zero third-party API keys.

## Setup

1. Ensure **Jellyfin** is enabled on **Settings → Plugins** (starts enabled by default on both fresh installs and updates).
2. Connect the V1 to Wi-Fi.
3. Open **Streaming → Jellyfin** (available in both the tile grid and compact list views).
4. Open **Server settings** (last tile on the Jellyfin home grid) and enter your **Server address** (for example `http://192.168.1.50:8096` or `https://music.example.com/jellyfin`). If your home server uses a self-signed HTTPS certificate, toggle **Self-signed TLS** on.
5. Sign in using either:
   - **Quick Connect** (recommended on the 2-inch touchscreen): tap **Quick Connect** to show a 6-digit code and a scannable QR code pointing to `<server>/web/#/quickconnect`. Approve the code from your phone or desktop browser and the player signs in automatically.
   - **Username & password**: fill in **Username** and **Password**, then tap **Sign in & save**.

### Credentials and security

- **Token-only persistence**: your password is used once during `POST /Users/AuthenticateByName` and is **never written to disk**. When **Remember session** is enabled (default), only the server address, user id, username, stable device id, selected music library, and revocable Jellyfin `AccessToken` are saved atomically (`.tmp` + `fsync` + `rename`) to `/usr/data/jellyfin-credentials.json` with mode **`0600`** (`rw-------`).
- Turning **Remember session** off deletes the saved file while keeping the current session in memory until power-off or **Forget server**.
- **Forget server** calls `POST /Sessions/Logout` best-effort on the server, clears in-memory state, deletes `/usr/data/jellyfin-credentials.json`, and wipes the transient SD cache. **Factory reset** also removes `jellyfin-credentials.json` and the transient cache.
- **Strict host isolation**: `Authorization: MediaBrowser Client="SonixPlayer", Device="TempoTec V1", DeviceId="...", Version="1.0", Token="..."` is attached **only** when the request matches the configured server's scheme, host, and port. Access tokens are never placed in URL query strings or written to logs.

## Browsing and playback

The Jellyfin home page is a two-page 2×3 tile grid plus a top-right **Search** button:

- **Folders** — browse your music folder hierarchy (scoped to the chosen **Music library** if set).
- **Playlists** — browse server playlists (queried globally across your Jellyfin account even when a specific Music library is selected) and play or save them offline.
- **Artists** — browse Album Artists → Albums → Tracks.
- **Albums** — browse Albums with a title-bar toggle between **A–Z** (`SortBy=SortName`) and **Recently added** (`SortBy=DateCreated`).
- **Genres** — browse Music Genres → Albums → Tracks.
- **Favourites** — browse favourite tracks and sync live favourite toggles from Now Playing, Quick Panel, and track long-press menus (`POST`/`DELETE /Users/<id>/FavoriteItems/<itemId>`).
- **Saved** — browse, play, and delete tracks saved offline to `<sd_root>/Music/Jellyfin` with no network required.
- **Server settings** — server address, Quick Connect, username/password, Music library picker, streaming quality, SD cache limit, clear cache, playback reporting toggle, test connection, and forget server.

## Audio quality and Direct Play

| Setting | Server endpoint | Container / decoder | Notes |
| --- | --- | --- | --- |
| **Original (default)** | Direct Play (`static=true`) | Native Sonix codecs (FLAC, MP3, Opus, Ogg, M4A/ALAC/AAC, WAV, WavPack, APE, AIFF, DSD) | Bit-perfect original files; unsupported containers (e.g. `.wma`) automatically fall back to MP3 320 kbps |
| **MP3 320** | Server transcode (`320 kbps`) | `.mp3` via `dr_mp3` + `growfile` | Fast progressive start |
| **MP3 192** | Server transcode (`192 kbps`) | `.mp3` via `dr_mp3` + `growfile` | Lower bandwidth |
| **Opus 128** | Server transcode (`128 kbps`, Ogg) | `.opus` via `opusdec` + `growfile` | High efficiency at 48 kHz |

- All track durations are decoded from 64-bit Jellyfin `RunTimeTicks` (`ticks / 10,000,000`) so long DJ sets and concerts (> 35 minutes) never overflow 32-bit integers on MIPS32 (`o32`).
- **Playback reporting**: when **Report playback** is enabled (default), the player posts `/Sessions/Playing`, `/Sessions/Playing/Progress` (with 64-bit `PositionTicks` and pause state), and `/Sessions/Playing/Stopped` asynchronously so Jellyfin dashboards, play counts, and ListenBrainz/Last.fm server plugins stay in sync without blocking audio or UI threads.

## MicroSD cache and offline saves

Transient playback cache files (audio, `.tags` metadata sidecars, and `.jpg` artwork) live on the microSD card under:

```text
/mnt/sd_0/.local/jellyfin-cache/
```

- **Progressive playback & prefetch**: MP3, FLAC, and Opus streams start playback as soon as prebuffer arrives via `growfile` while a background thread completes the file; other Direct Play formats complete on the card before starting so seeking works reliably across all codecs. At 80% of the current track, the next queued Jellyfin track prefetches quietly via `streamturn`.
- **Configurable LRU ceiling**: defaults to **512 MB**, adjustable under **Server settings → SD cache limit** (**512 MB**, **1 GB**, **2 GB**, **4 GB**, **8 GB**, or **Unlimited**; persisted as `[jellyfin] cache_max_mb`), always keeping at least **256 MB** free on the card. The transient cache is excluded from local library indexing and wiped cleanly on exit, reboot, or factory reset.
- **Offline saves (`Keep on card`)**: long-press any track to save it, or tap the download corner button inside an album, playlist, or folder to save the whole page offline. Saved tracks copy permanently into `<sd_root>/Music/Jellyfin/<artist>/<album>/` alongside `.tags` and cover artwork via `streamkeep`, surviving cache clears and working offline from both **Jellyfin → Saved** and the local Music library.

## Validation

Run the automated Jellyfin unit, sanitizer, and headless LVGL UI test suites from `sonix-player/tests`:

```sh
make -C sonix-player/tests jellyfin
make -C sonix-player/tests jellyfin-ui
make -C sonix-player/tests sanitize
```
