# Spotify on the TempoTec V1

A **Spotify Premium** client for `sonix_player`: your Liked Songs, your playlists, search and a
queue, drawn on the V1's 240×320 screen and played through the player's own DAC — not a phone
app mirrored onto a screen.

It is modelled on [SpotUI](https://github.com/noisetta/spotui-hiby-r3proii) by
**noisetta**, a Spotify client for the HiBy R3 Pro II (MIT — see
[Credits](#credits)). The two-level shape is the same: a small daemon speaks for Spotify, the
player speaks to the daemon.

## Why there is a daemon

Spotify's Web API needs an OAuth application and a browser to grant it, and it will not hand out
audio at all — playback only exists behind Spotify's own encrypted, licensed client protocol,
which is what **librespot** implements. So the V1 does not speak to Spotify itself:

```
   V1 (240×320, MIPS, 64 MB)                    a computer on the same Wi-Fi
 ┌──────────────────────────────┐   unix socket  ┌──────────────────────┐
 │  Streaming ▸ Spotify pages   │◄── commands ──►│  spotifyd            │
 │  spotify.c (the client)      │◄── 44.1k PCM ──│  librespot inside    │
 │  audio_external_* → DAC      │                │  (tools/spotifyd)    │
 └──────────────────────────────┘                └──────────┬───────────┘
                                                            │ Spotify Connect
                                                       ┌────▼─────┐
                                                       │ Spotify  │
                                                       └──────────┘
```

The daemon decodes and hands the player **raw signed 16-bit PCM**; it never opens an audio device
of its own. The player's DAC, output routing, equaliser, gapless handling and volume therefore stay
in charge exactly as they do for a file on the microSD card. Nothing is written to the card while
playing: a track arrives as a stream, the way the radio does.

## Requirements

| | |
| --- | --- |
| **Spotify account** | **Premium.** librespot will not stream for a free account. |
| **A computer** | Linux/macOS/Windows on the same network, or a Raspberry Pi left running. |
| **Network** | Wi-Fi on the V1; the daemon reaches the internet. |
| **Firmware** | This build, with the **Spotify** add-on enabled (see below). |
| **No Spotify app needed** | The V1 browses your library itself. |

## Setup

1. **Enable the add-on.** The V1 ships with Spotify **off**: a tile leading to "no daemon" is
   worse than no tile until somebody has set one up. Turn it on in
   **Settings ▸ Plugins ▸ Streaming services ▸ Spotify** and the tile appears under
   **Streaming**.
2. **Build the daemon** on the computer (see [`tools/spotifyd/PROTOCOL.md`](../sonix-player/tools/spotifyd/PROTOCOL.md)
   for the wire contract and [`sonix-player/tools/spotifyd/README.md`](../sonix-player/tools/spotifyd/README.md)
   for the recipe). It is a Rust program, built outside this firmware's Makefile exactly as
   librespot itself is; the sandbox this firmware is built in has no Rust toolchain, so the
   cross-build is a step you run once, on a PC.
3. **Log in once, on the computer**, so the daemon has credentials to reuse:

   ```sh
   librespot --cache /usr/data/librespot-cache \
             --cache-tool login --username you@example.com
   ```

   The cache (and the volume librespot remembers) lives in that folder; it is what lets the V1
   start playing without a sign-in of its own.
4. **Start the daemon**, by hand or from init:

   ```sh
   /usr/bin/spotifyd
   # or: cp tools/spotifyd/S99spotifyd /etc/init.d/ && /etc/init.d/S99spotifyd start
   ```

   Its default socket is `/var/run/sonix-spotify.sock`; `spotifyd.conf` next to it can move that,
   name the device the phone's Spotify app shows, and choose 96/160/320 kbps.
5. **Open Streaming ▸ Spotify.** The home page is four tiles — **Liked Songs**, **Playlists**,
   **Search** and **Account** — with the account named under them.

### Where the daemon's socket is

The player looks for the socket in the order below and stops at the first that exists:

1. `spotify.socket=` in `[spotify]` in `/usr/data/streaming-keys.ini` (or on the microSD card),
2. `/var/run/sonix-spotify.sock`, the daemon's default.

If it finds no daemon at all, the page says so plainly and offers the **DEMO** library instead —
see below.

## Using it

- **Liked Songs** — the same list the phone shows, newest first, with the owner and track count
  at the top of the page.
- **Playlists** — every playlist in the account, with owner and length; tap one to see its
  tracks.
- **Search** — the magnifier key on the keyboard runs the search; results are tracks, and tapping
  one starts playback from there, with the rest of the results as the queue.
- **Now Playing** — the usual player sheet: play/pause, previous/next, seek, shuffle and repeat.
  Shuffle and repeat are also on the account page, because they are properties of the session, not
  of one track.
- **Account** — who is signed in, which backend is speaking (daemon or demo), the streaming
  quality librespot reports, shuffle, repeat, and a sentence explaining anything that is missing
  (no Wi-Fi, no daemon, not signed in).

Two behaviours worth knowing:

- **The queue lives in the daemon.** The player only reports *where* a track was tapped
  (`PLAYCTX liked|playlist|search …`), and the daemon builds the queue from the same context. That
  is why skipping to the next track on the V1 agrees with the phone.
- **Pause is not stop.** Pausing keeps the PCM channel open and the daemon keeps its place;
  leaving the page or pressing stop closes the channel, tells the daemon to pause, and gives the
  DAC back to the player.

## The DEMO library

With the add-on on and no daemon reachable, the page offers a **DEMO** badge: a small built-in
library (`spotifymock.c`) with three playlists and their tracks. Every page works against it —
browsing, search, queue, shuffle, repeat — but the audio is silence, and the badge says so. It
exists so a fresh device shows the feature rather than an apology, and so the pages can be
tested with no daemon, no credentials and no network.

## Validation

```sh
# The protocol client against a fake daemon on a real socket (8 groups):
make -C sonix-player/tests spotify

# The LVGL pages, headless: 240×320, 11 languages × 5 themes, no overlapping
# label anywhere, no raw translation key, liked/playlists/search/account,
# the DEMO badge, the offline card and the sign-in note:
make -C sonix-player/tests spotify-ui
```

Both run in CI, and `make -C sonix-player/tests ui` runs the page suite alongside every other
page's.

## Attribution

The daemon in `sonix-player/tools/spotifyd` follows SpotUI's design and carries its notes on
librespot plumbing. SpotUI is MIT-licensed; this firmware's own code is under the repository's
licence. librespot is MIT too, and is not vendored here — you install it as a dependency.

## Limitations

- **Premium only.** No plan, no stream.
- **Read-only library.** Liked Songs and playlists are read; nothing on the V1 adds a track to
  your library, because that needs the Web API and its OAuth flow, which SpotUI also declines.
- **One daemon, one player.** The socket is a request/response stream per connection; a second
  PCM connection replaces the first, which is how the player restarts its own stream.
- **The daemon is not in this repository's build.** See `tools/spotifyd/README.md` for the
  cross-build; the firmware ships the tile and the client, not the Rust binary.
