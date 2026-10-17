# Spotify on the TempoTec V1

The V1 plays Spotify through **q2-librespot**, the librespot fork written for the Shanling Q2 by [diamondBond](https://github.com/diamondBond/q2-librespot) (branch `shanlingq2`). It is a Spotify Connect receiver: your phone picks the player in its device list and casts to it. The fork adds three interfaces an unmodified librespot does not have — a status file, a control socket and a browse socket — and those are the whole reason it fits here: through them the player's own interface can show what is playing, browse your library, and drive the transport.

None of Spotify's protocol is implemented in this firmware. The login, the Connect session, the audio keys and the Ogg Vorbis decoding are the receiver's; the player drives it and reads its PCM. Nothing about the receiver is flashed either — it lives on the microSD card.

**Status: this half is written and host-tested, but no ready-made V1 receiver binary exists yet.** The upstream project publishes builds for the Q2, and a V1 build has to be cross-compiled for `mipsel-unknown-linux-musl` soft-float (see [Building the receiver](#building-the-receiver)). Until you put such a binary on the card, **Streaming → Spotify** says the receiver is missing instead of offering a switch.

## What it looks like on the player

**Streaming → Spotify** is the switch:

- Off until you turn it on. The receiver is a separate process and a separate login, so it is not started at boot.
- Turning it on requires a Wi-Fi connection; without one the page says so and points at Wi-Fi settings.
- If the card has no receiver, the page says that instead, with the path it looked in.
- While it is on, the page shows the current track's artwork, title, artist, album and a progress bar — all read from the receiver's status file.
- The receiver's own output and the player's runtime files are all in `/tmp`, so nothing the player writes goes on the card except the receiver's cache.

The rows below the switch are the browser, over the Q2SPOT/1 socket:

| Row | Sends | Shows |
| --- | --- | --- |
| Liked songs | `liked` | Your saved tracks |
| Playlists | `playlists` | Your playlists; one opens its tracks |
| Albums | `albums` | Saved albums; one opens its tracks |
| Artists | `artists` | Followed artists; one opens its albums |
| Queue | `queue` | What the receiver will play next on the phone |
| Downloaded | `downloaded` | Complete tracks in the card cache, replayable with no network |
| Search | `search <tracks\|albums\|artists\|playlists> <term>` | The result list |
| Receiver settings | `settings`, `account` | Shuffle, repeat, account, the player's own settings |
| — | `caps` | The backend version — how the player knows the receiver is new enough |

Rows come 50 at a time. A list longer than that gets a **next page** row; the cursor the receiver answered with is sent back as the request's last argument, and **previous page** walks the other way. Back leaves a list one step at a time — from the second page to the first, from a playlist's tracks to the list that named it — and only at the top does it leave the page.

Selecting a track plays it **in its list**: `play <context> <position> <uri>`. That is what makes Next on the player continue the album rather than the single track. Selecting from the Queue sends `skip` instead, because the phone's queue is already the context.

Shuffle and repeat are the receiver's own state, shown and changed on the receiver settings page. Repeat cycles **off → the list → one track**.

### Keys, DAC and volume

While Spotify is what is playing, the player's own transport keys drive it: play/pause, next and previous go to the receiver as one-byte datagrams. The moment local playback takes the DAC, the keys go back to local playback.

The audio path is the AirPlay path, not ALSA: the receiver is started with `--backend pipe --device /tmp/sonix-spotify.pcm`, writes S16 44.1 kHz stereo into that FIFO, and the player reads it and hands it to the DAC through the same external-PCM route AirPlay uses. So the V1's volume, jack detection, DAC hand-over and gapless-within-a-source behavior are the player's, exactly as they are for any other source. **librespot is given `--volume-ctrl fixed --initial-volume 100` on purpose** so it does not put a second, invisible volume on top of the player's.

Three seconds after audio stops arriving, the player closes the DAC and hands the output back; a phone that starts casting again re-opens it. That is also what makes switching to a local file or another service work — asking for the DAC calls the sink's release path, which pauses the receiver and returns the output.

### When it stops by itself

- **Idle stop** (off by default, or 5 min / 15 min / 1 h) stops the receiver after that long with nothing playing, once it *has* played and with the page shut. The receiver's saved login makes the next start instant, and casting from the phone turns it back on.
- **Power off, reboot, factory reset and firmware update** stop the receiver and wait for it to leave, so its cache and its credential file are flushed and the card is not unmounted under a running process.
- The receiver is kept alive while the browser is on screen or a request is in flight, and Wi-Fi is kept up while a request is outstanding.
- If the receiver dies on its own, the page says so and shows the tail of its log.

### The receiver's process

The receiver is a separate process, started from the player with the card's folder as its working directory. Four things about it matter if something goes wrong:

- **It is below the interface.** It runs at `nice 10` and in its own session, so it can never take the core from the thread that draws the screen, and it dies with the player (`PR_SET_PDEATHSIG`) rather than being left holding the card under an unmount.
- **The kernel kills it first.** The player asks to be the last process killed when memory runs out (`oom_score_adj -500`, see `main.c`); the receiver inherits nothing of that and sets `500` for itself before it starts. A receiver that does not fit in what the device has left is killed and reported on the page, instead of the kernel taking something the device needs.
- **It does not inherit the player's descriptors**, and its own log is capped: `/tmp/sonix-spotify.log` lives on the tmpfs, which is RAM, and a receiver told to be verbose (`debug = true`) can write faster than anything can read. Past 256 KiB the file keeps its tail — the last thing a receiver said is what explains it stopping — and starts again.
- **The first line of every start is in the player's log**: `spotify: receiver pid …, rss …, free …, oom_score_adj …, nice …`. That is the line a report about a slow or frozen device is read from.

## Layout on the card

```text
<card>/
  .spotify/
    librespot        the receiver, executable, mipsel soft-float
    cache/           created by the player; the saved login and cached audio
```

`librespot` must be at that exact path and executable — the page checks for it before offering the switch. `cache/` is created on the first start; it holds the credentials that make the second start not ask the phone to pair again, and (with the cache size above zero) complete tracks for the **Downloaded** list to replay offline.

Everything else is runtime state under `/tmp` and is recreated on every start:

| Path | Who writes it |
| --- | --- |
| `/tmp/sonix-spotify.state` and `.state.jpg` | the receiver: what is playing, and the artwork |
| `/tmp/sonix-spotify.sock` | the receiver: the control socket |
| `/tmp/sonix-spotify.browse.sock` | the receiver: the browse socket |
| `/tmp/sonix-spotify.pcm` | the FIFO the player reads the audio from |
| `/tmp/sonix-spotify.log` | the receiver's own output, and where the error message comes from |

A leftover socket file after a crash is normal: the receiver unlinks and re-binds them at startup.

## Receiver settings

The **Receiver settings** page keeps the player's own settings in `device_config.ini`; the rest of that page is the receiver's and is asked for over the browse socket.

| Row | What it does |
| --- | --- |
| Account | Who is signed in, or that nobody is, from the `account` op |
| Shuffle | `shuffle on` / `shuffle off`; tap to toggle |
| Repeat | `repeat off` / `context` / `track`; tap to cycle |
| Bitrate | `--bitrate 96 / 160 / 320`, default **160** kbit/s |
| Card cache | `--cache-size-limit`: off, 1 GB or 8 GB (default), of cached audio |
| Device name | `--name`: what your phone shows in its device list |
| Turn off when idle | the idle stop above: never, 5 min, 15 min or 1 h |
| Sign out | `signout`, behind a confirmation; removes the saved login, keeps downloads |

The same settings, and two the page does not show, live in `device_config.ini`:

```ini
[spotify]
bitrate = 160          ; 96, 160 or 320 kbit/s
cache_mb = 1024        ; MiB of cached audio; 0 turns --disable-audio-cache on
idle_stop = 0          ; seconds with nothing playing before stopping; 0 = never
name =                 ; empty means the player's own name from system-info.json
extra =                ; extra receiver words, verbatim, spaces splitting them
rt_priority = 10       ; the sink thread's real-time priority; 0 = no RT
debug = false          ; true adds -vv to the receiver's arguments
```

`extra` is for the receiver's other options — a proxy, normalisation, a different discovery mode — split on whitespace, at most eight words, no quoting.

## If the device freezes

A freeze is the one failure this device cannot report about itself, so the player writes down what it can before and during one, and the first job is to get that out of it.

**Collect the log.** Turn on **Developer options → Log to microSD**, delete `.local/sonix_player.log` from the card, turn the Spotify switch on again, and wait for the freeze. The log survives it, because it is flushed to the card about once a second. If the device has to be restarted, send the log **as it is after it comes back**: the kernel's own account of the freeze — out-of-memory kills, the interface watchdog's dump, `/proc/last_kmsg` — is copied into the top of the next run's log. [`docs/collecting-logs.md`](collecting-logs.md) has the short version and the bundle script for the rest.

What the log will then say, depending on the cause:

| In the log | What it means |
| --- | --- |
| `spotify: receiver pid …: rss … kB, free … kB` with a small `free` | the device had no room for the receiver; the kernel's kill of it appears in the next start's `kernel:` lines |
| `watchdog: … MemAvailable … Dirty/Writeback …` and a thread dump | the interface stalled: the thread it names is the one to look at — `spotify sink` is the sink, `Scheduler`/`mmc` waits are the card |
| `spotify: the output is behind; … frames dropped` repeating | the card is not keeping up with the audio; the log flushes are part of the same load |
| `sched: RT throttling activated`, a second or two after `thread: spotify sink runs at RT priority` | a real-time thread is spinning rather than waiting, and the kernel is taking the core back from the real-time class for the rest of every period — what is left is what the interface gets. Try `rt_priority = 0` first: it is the remedy for this row. |
| nothing after the switch and no `receiver pid` line at all | the player did not get as far as starting the receiver: that is a bug in the player's own enable path, and the log is the report |

The switch used to freeze the V1 with exactly the fourth line: one or two seconds after `thread: spotify sink runs at RT priority`, and with `spotify: receiver pid …` either side of it. The sink's two waits are timed waits on condvars that are statically initialized, which fixes the clock they are read against at `CLOCK_REALTIME`; that file computed its deadlines on `CLOCK_MONOTONIC`, and on a device whose wall clock is set from its battery-backed RTC every deadline was a billion seconds in the past. Every wait returned at once. The playback thread did not sleep, it spun — at real-time priority, on the one core — and the interface was left with what the kernel's RT throttling left it. Both waits come from `deadline_in_ms()` now, the helper `utils.c` keeps for this exact reason, and no file computes its own. `tests/test_spotify.c` measures the CPU an idle receiver stack costs, so a wait that stops waiting fails the suite instead of the device.

**Things to try, in order,** with the log from each:

- `[spotify] cache_mb = 0` in `device_config.ini`: nothing is written to the card while streaming. Every other streaming feature on this player works this way on purpose (see [`docs/subsonic.md`](subsonic.md)), and a slow card filling with dirty pages is the most likely way to make this device unresponsive.
- `[spotify] rt_priority = 0`: the sink's playback thread leaves the real-time class, so nothing it does can starve the interface — a spinning real-time thread is what `sched: RT throttling activated` in the log means. Audio may stutter on a busy device; the interface will not.
- `[spotify] debug = false` (the default): `-vv` makes the receiver write its own trace into the tmpfs, which is RAM.
- **Turn the switch off and give it a minute** before pulling the card: the receiver flushes its cache and its saved login on the way out.

If it freezes with the card's `.spotify` folder renamed away, the switch refuses to move and says *missing receiver* — that is the expected answer, not the bug. The freeze is in starting the receiver, and the log above is what says which part of it.

## Building the receiver

The fork is built static and soft-float for the Q2, which is also a MIPS `mipsel` soft-float device, and the same instructions work for the V1:

```sh
git clone --branch shanlingq2 https://github.com/diamondBond/q2-librespot
cd q2-librespot
# contrib/shanlingq2/README: mipsel-unknown-linux-musl via -Zbuild-std,
# mipsel-linux-muslsf-cross for the linker, nan2008.py to set EF_MIPS_NAN2008.
```

Copy the resulting binary to `<card>/.spotify/librespot`, `chmod +x` it, and the page's "missing receiver" message goes away. The `contrib/shanlingq2/launcher.sh` restart loop and `aplay.sh` sink from that folder are **not** used here: the player starts the receiver itself and reads the FIFO, so the receiver is run directly rather than behind a launcher.

The receiver reports a backend version (`q2-v1.4` for this branch) over the `caps` op; the player only needs the three interfaces above to exist.

## Validation

`sonix-player/tests/test_spotify.c` covers the protocol client, the status file, the control socket, the service's start/stop/idle/ownership rules and the sink's ring and DAC hand-over, against a shell stand-in receiver and a stub audio layer; `make -C tests spotify` runs it -- the stand-in receiver records the niceness and the `oom_score_adj` the spawn gave it, the receiver's log is checked to be trimmed back to its own tail, and an idle receiver stack -- service and sink up, nothing to play -- is measured against the CPU it is allowed to cost (which is what a timed wait that stops waiting does not pass: see the freezes section below). That measurement is why `core/utils.c` is linked into that suite for real rather than stubbed. `sonix-player/tests/ui/test_spotify_page.c` covers the page itself on both board profiles with a stubbed `spotify.h` and shell, and records every op and argument the page sends; `make -C tests ui` runs it.

Both are host tests: they do not certify real-time scheduling, the receiver's audio quality or Wi-Fi behavior on the device. Before relying on it, check on a V1:

- the receiver starts from the card and pairs with a phone; the second start reuses the saved login;
- playback through the FIFO: volume, jack detection and switching between 3.5 mm and 4.4 mm still behave, and so do local playback, AirPlay and Bluetooth in between;
- the transport keys: play/pause, next and previous while Spotify owns playback, and that they go back to local playback after switching away;
- browsing a large playlist from the second page backwards, and a search in each of the four categories;
- the idle stop firing, and a phone casting again turning it back on;
- power-off, reboot and firmware update with the receiver running — the process should be gone and the card clean;
- CPU use and battery drain against a local-playback baseline, at each bitrate, including with the card cache on.
