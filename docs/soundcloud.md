# SoundCloud on the TempoTec V1

Native SoundCloud API v2 client for `sonix_player`, supporting anonymous browsing/playback as well as **Free**, **SoundCloud Go**, and **SoundCloud Go+** accounts.

## Setup

1. Enable **SoundCloud** on **Settings → Plugins** (fresh installs start with optional streaming services off; existing updated devices keep it on) and restart if prompted.
2. Connect the V1 to Wi-Fi.
3. Open **Streaming → SoundCloud**. Both the standard tile grid and the compact list view include SoundCloud.
4. **Anonymous mode**: you can immediately **Search tracks** and play public uploads without an account. On first API use, the client automatically discovers and caches a valid public SoundCloud web `client_id` (and rotates it automatically on `401` expiry).
5. **Account mode (Free / Go / Go+)**: open **Account & Settings** inside SoundCloud (or place credentials on the microSD card — see below) to unlock **Likes**, **Reposts**, **Playlists**, Now Playing / Quick Panel **Like** toggling, and **Go+ High Quality (AAC 256 kbps)** streams.

### Signing in (`oauth_token` / `username` / `streaming-keys.ini`)

SoundCloud closed new public OAuth application registrations, so personal accounts authenticate with your session `oauth_token` (or browse public Likes/Reposts/Playlists by profile URL, `@handle`, `on.soundcloud.com` shortlink, or `username`, or configure `[soundcloud]` in `streaming-keys.ini` on the microSD card):

1. Sign in to `https://soundcloud.com` in a desktop browser.
2. Open Developer Tools → **Application** (Chrome/Edge) or **Storage** (Firefox) → **Cookies** → `https://soundcloud.com`, and copy the value of the **`oauth_token`** cookie (for example `2-123456-78901234-abcdef...`).
3. Either enter it on **Streaming → SoundCloud → Account & Settings → OAuth token** and tap **Connect & save**, or add a `[soundcloud]` section to `/mnt/sd_0/streaming-keys.ini` (or `/usr/data/streaming-keys.ini`):

```ini
[soundcloud]
oauth_token = 2-123456-78901234-xxxxxxxxxxxxxxxx
username =
# Optional override; leave blank for automatic discovery and rotation:
client_id =
```

### Credentials and security

- When **Save credentials on device** is enabled (default), credentials are written atomically (`.tmp` + `fsync` + `rename`) to `/usr/data/soundcloud-credentials.json` with mode **`0600`** (`rw-------`). Clearing **Save credentials on device** removes any saved disk file while keeping the active session in memory until power-off or **Forget account**.
- **Forget account** and **Factory reset** clear in-memory session state, delete `soundcloud-credentials.json`, and wipe cached SoundCloud tracks.
- Request headers include `Origin: https://soundcloud.com`, `Referer: https://soundcloud.com/`, and a browser-compatible `User-Agent`.
- **Strict OAuth host isolation**: `Authorization: OAuth <token>` is attached **only** when the target URL authority/host matches `api-v2.soundcloud.com`, `*.soundcloud.com`, or the configured API base — never when fetching HLS manifests, segments, AES keys, or artwork from CDN hosts (such as `*.sndcdn.com`). Tokens are never written to logs.

## Audio quality and subscription tiers

| Setting | Preferred stream | Container / decoder | Notes |
| --- | --- | --- | --- |
| **Low (`Opus 64k`)** | `opus_64k` (`ogg` HLS) | Ogg Opus (`.opus`) via `opusdec` + `growfile` | Lowest bandwidth; 48 kHz output |
| **Standard (`MP3 128k`, default)** | `mp3_128k` (progressive HTTP or HLS) | MP3 (`.mp3`) via `dr_mp3` + `growfile` | Universal compatibility across Free / Go / Go+ |
| **High (`AAC 256k`)** | `aac_256k` (`mp4` HLS, Go+) | Unwrapped to ADTS AAC (`.aac`) via `fdk-aac` | Requires Go+ entitlement and `/usr/lib/libfdk-aac.so.2` on device; falls back automatically to MP3 128k / Opus 64k otherwise |

### Preview (`snipped`) handling, multi-transcoding fallback, and playlist hydration

- SoundCloud tracks frequently advertise a Go+-only `snipped` (30-second preview) transcoding alongside full-length transcodings, or legacy `mp3_1_0` / `abr_*` format URLs that return HTTP `404` while `opus_0_0` / `aac_160k` HLS streams on the same track succeed. Stream resolution skips broken `abr_*` presets, attaches `track_authorization` when present, prefers non-snipped full-length streams first, and automatically falls back across all ranked transcodings (and from progressive HTTP to HLS if the CDN fails) before reporting an error. Tracks marked `BLOCK` fail cleanly with a localized notice.
- SoundCloud API v2 playlist objects return full metadata for the first tracks and lightweight `{ "id": ... }` stubs for the remainder. `soundcloud_playlist_tracks()` automatically hydrates stub IDs in batches of up to 50 via `/tracks?ids=...` while preserving the original playlist order.
- All SoundCloud track, user, and playlist IDs are stored as 64-bit `long long` values (`%lld`) so IDs above `2,147,483,647` work identically on 32-bit MIPS (`o32` ABI) and 64-bit hosts.

## Disk cache and streaming pipeline

Cached audio, `.tags` metadata sidecars, and `-t500x500.jpg` artwork live on the microSD card under:

```text
/mnt/sd_0/.local/soundcloud-cache/
```

- **Configurable LRU ceiling**: defaults to **512 MB** and can be extended directly on the player under **Streaming → SoundCloud → Account settings → SD cache limit** to **1 GB**, **2 GB**, **4 GB**, **8 GB**, or **Unlimited** (persisted as `[soundcloud] cache_max_mb` in `device_config.ini`, where `0` means Unlimited). Lowering the limit prunes oldest cached track bundles (`.mp3`/`.opus`/`.aac`, `.tags`, `.jpg`) immediately while protecting the currently playing or actively downloading track, and eviction always preserves at least **256 MB** of free space on the microSD card. The cache directory is excluded from local library indexing.
- **Progressive & HLS streaming**:
  - Progressive MP3 streams announce via `growfile` once 64 KiB (or EOF) has arrived so playback starts without waiting for the full file.
  - HLS playlists support master-variant selection, `#EXT-X-KEY:METHOD=AES-128` CBC segment decryption (explicit `IV=` or segment-sequence IV with PKCS#7 unpadding), MPEG-TS (`0x47` sync) elementary-stream extraction, duplicate ID3v2 header stripping on MP3 segments `> 0`, growing `.opus` playback with `.tags` `duration=` fallback, and `#EXT-X-MAP` (`ftyp`/`moov` `esds` + `trex`) + `moof`/`mdat` (`trun`) fragmented-MP4 unwrapping into ADTS `.aac`.
- **Queue prefetch & Keep**: while a SoundCloud track plays, the next SoundCloud track in the queue is prefetched in the background via `streamturn` once the active download finishes. Completed tracks also work with **Keep** (`streamkeep`), Now Playing / Quick Panel **Like** toggling, and the Now Playing SoundCloud source badge.

## Validation

Run the automated SoundCloud unit, sanitizer, and headless LVGL UI test suites from `sonix-player/tests`:

```sh
make -C sonix-player/tests soundcloud
make -C sonix-player/tests soundcloud-ui
make -C sonix-player/tests sanitize
```
