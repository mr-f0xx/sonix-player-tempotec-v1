# spotifyd — the daemon half of Spotify on the V1

The player's Spotify pages are useless without something to speak for Spotify: the
V1 cannot decode Spotify's audio stream, and Spotify's Web API has no audio in
it. This program is that something. It runs on a computer on the same network,
signs in with **librespot**, browses your library, keeps the queue, and writes
raw PCM to the player over a UNIX-domain socket — the player's own DAC, volume,
equaliser and output routing stay in charge.

The contract between the two halves is [`PROTOCOL.md`](PROTOCOL.md). Read it
before changing either end.

| File | What it is |
| --- | --- |
| `src/protocol.rs` | The wire format in code: replies, STATE, list pages. No librespot, no I/O, with `cargo test` tests. |
| `src/main.rs` | Config, socket, command dispatch, the PCM handover. Also no librespot. |
| `src/librespot_glue.rs` | The librespot calls — session, browsing, playback, the PCM sink. |
| `Cargo.toml` | Pins `librespot = "0.6"`. |
| `S99spotifyd` | init.d script, so the daemon is up before the player is. |
| `spotifyd.conf.example` | Every setting, with the defaults. |

## Status: what is built and what is not

**This has never been compiled in this repository,** and that is deliberate: the
firmware is built with a MIPS cross-toolchain that contains no Rust, and this
program does not run on the player. What is here is the recipe and the protocol,
checked in so the player and the daemon cannot drift apart:

- `src/protocol.rs` and `src/main.rs` are ordinary Rust with no librespot in
  them; `cargo test` runs the protocol tests (a tab inside a title, an empty
  cover, a reply that must end in `END`, a page too long for the player's read
  window).
- `src/librespot_glue.rs` is written against librespot 0.6's shape. librespot's
  API moves between minor releases; expect a handful of compile errors on the
  first `cargo build` and fix them by the names in the comments. The protocol
  will not have moved, and that is the part the V1 is built against.

## Building it on a PC

```sh
# Rust, if you have not got it:
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

cd sonix-player/tools/spotifyd   # this directory, from a checkout of this repo
cargo test                       # the protocol tests: no network, no account
cargo build --release
sudo install -m 755 target/release/spotifyd /usr/bin/spotifyd
```

A Raspberry Pi works; so does any Linux box that stays awake. macOS works for
building and running, with the same paths if you make them.

### Logging in once

librespot does the Spotify sign-in, not this program:

```sh
mkdir -p /usr/data/librespot-cache
librespot --cache /usr/data/librespot-cache --cache-tool login --username you@example.com
```

That writes credentials into the cache directory. `spotifyd` reads them from
there — there is no password in the daemon, and the V1 never sees one. The cache
also holds the volume librespot remembers.

**Premium is required.** librespot will sign in with a free account and then
refuse to stream.

### Installing

```sh
sudo install -m 644 spotifyd.conf.example /usr/data/spotifyd.conf   # then edit
sudo install -m 755 S99spotifyd /etc/init.d/S99spotifyd
sudo /etc/init.d/S99spotifyd start
```

The daemon creates `/var/run/sonix-spotify.sock` by default. That is the path the
player looks for; `socket=` in `spotifyd.conf` moves both.

### On the V1

1. **Settings ▸ Plugins ▸ Streaming services ▸ Spotify** — on. The tile appears
   under **Streaming** (it is off out of the box, because a tile that leads to
   "no daemon" is worse than no tile).
2. Open **Streaming ▸ Spotify**. With the daemon up, the home page lists your
   Liked Songs and playlists. Without it, the page offers the **DEMO** library
   so the screens are still worth looking at.

## How the pieces talk

```
LIKED        ->  Playlist::get_liked()          ->  N <total>, T <track>..., END
PLAYLISTS    ->  Playlist::get_playlists()      ->  P <id> <name> <owner> <n>...
PLAYLIST     ->  Playlist::get(id)              ->  N <total>, T <track>..., END
SEARCH       ->  Track::search(query)           ->  N <total>, T <track>..., END
PLAY         ->  Player::load(track)            ->  END
PLAYCTX      ->  Player::set_available_context  ->  END
                 then load at the tapped track
PCM          ->  Sink::write(frames)            ->  SNXPCM1, "44100 2 16", samples
```

`PLAYCTX` is why the phone and the player agree: the *queue* is librespot's, and
the player only reports where a track was tapped (`liked`, a playlist id, or the
last search). Skip to the next track on the V1 and you skip the same one the
phone would.

The sink drops frames while no PCM channel is open rather than blocking the
decoder, and clears itself when the player closes the socket. The player
re-opens PCM when it wants sound again and seeks to where it should be, which is
why a pause on the V1 survives the daemon carrying on.

## Troubleshooting

| Symptom | What it means |
| --- | --- |
| The page says **no daemon** | The socket is not there: is `spotifyd` running, and is `socket=` the path the player looks for? `/var/run/sonix-spotify.sock` is the default on both ends. |
| `AUTH none` on the page | The daemon has no session: run the `librespot --cache-tool login` step with the cache directory it was configured with. |
| Tracks list but nothing plays | The account is not Premium, or `bitrate=` asks for a tier it does not have. Try `bitrate=160`. |
| Audio plays on the computer instead | That is stock `librespot` with its own ALSA backend; `spotifyd` never opens an audio device. Check you started this binary, not `librespot`. |
| Titles are blank for a second | Metadata is fetched before the track loads, so this should not happen; if it does, the session is falling back to its metadata cache. |

## Licence

MIT. The design follows [SpotUI for the HiBy R3 Pro II](https://github.com/noisetta/spotui-hiby-r3proii)
by noisetta (MIT): the split into a librespot daemon and a player UI, the
library-only browsing (no library writes, because those need the Web API's
OAuth flow), and the queue controls are all its shape. librespot is MIT and is
not vendored here — you install it as a dependency.
