# Subsonic / Gonic on the TempoTec V1

## Setup

1. Connect the V1 to Wi-Fi.
2. Open **Streaming → Subsonic**. The home page is four tiles — **Server settings**, **Test connection**, **Music folders** and **Artists** — two by two, with the picture above its caption in every theme, the same cards the SoundCloud home page draws. There is nothing below them to scroll to. The Streaming section lists Subsonic in both its regular grid and its compact Tokyo Night list.
3. Open **Server settings**.
4. Enter the server base URL, username and password. For example, `https://music.example.com` or `http://192.168.1.20:4747`. A reverse-proxy prefix such as `/gonic` is supported; a pasted trailing `/rest/` is normalized. Use an IPv4 address or DNS name, not a literal IPv6 URL.
5. Choose **Connect / save**. Saving an account stops existing Subsonic playback and invalidates its old browser/queue IDs; select a track again after reconnecting. A worker checks `ping.view`; the UI never waits in a socket call. No Qobuz/Tidal/Podcast application key is required.
6. Browse **Music folders** or **Artists**, then select a track. Tracks from the **current page** become the queue (up to 200); use Previous/Next page for larger directories or albums.

### Quality

| Setting | Stream request | Use |
| --- | --- | --- |
| MP3, 256 (default) | `format=mp3&maxBitRate=256` | Conservative CPU/bandwidth default |
| MP3, 320 | `format=mp3&maxBitRate=320` | Higher bitrate |
| Opus, 256/320 | `format=opus&maxBitRate=256` or `320` | Server-provided Opus; output is 48 kHz |
| Raw | `format=raw`, no `maxBitRate` | Original **FLAC, MP3 or Opus** on a good home network |

The bitrate is a **server-side ceiling**, not a promise that every file will be encoded at that rate. Configure/enable transcoding on Gonic or your compatible server. If the server ignores the requested format, playback refuses the mismatched codec rather than scanning the stream for another decoder.

Raw means the original compressed file, **not PCM**. Recognized suffixes are `flac`, `mp3` and `opus`; Opus stored as `.ogg` should be transcoded too. WAV, MP4/AAC/ALAC, Vorbis, DSD, APE and other originals are not handled by this forward-readable bridge; choose MP3/Opus transcoding instead. Unsupported raw entries are omitted from a page's playback queue, with an error if the selected track itself is unsupported. A quality change applies to the next queue created by selecting a track, not to a queue already playing.

### Credentials and TLS

- The password is shown as plain text while editing so you can verify what you entered. Keep the screen private while editing.
- Authentication is Subsonic v1.16.1 token authentication: random 16-byte salt, `MD5(password + salt)`, `u/t/s`, `c=sonix`, `f=json`. The password is never put in a URL.
- Credentials stay in memory unless **Remember credentials** is enabled. The optional `subsonic-credentials.json` beside `device_config.ini` is written atomically with mode **0600**. It contains the password **unencrypted**; do not share it or backups of it. Storage must support the requested permissions.
- Clearing Remember and saving removes the old saved credentials. **Forget server?** stops this service and removes its account from memory and disk.
- Prefer HTTPS with a trusted certificate and a correct player clock. A salted MD5 token can still be replayed and does not make HTTP private. HTTPS-to-HTTP redirects are refused; there is no silent TLS downgrade.
- A server with a self-signed certificate is refused by default. **Accept self-signed certificates** on this settings page (or `[network] tls_verify = relaxed` in `/usr/data/device_config.ini`) accepts a certificate from an authority no bundle knows, and only that: the certificate must still name the server being contacted and its dates must still cover the current date. An expired certificate, one issued to another name, and an HTTPS-to-HTTP redirect stay refusals, with a message saying which. The setting applies to every `https://` connection the player opens, radio included; the switch takes effect on the next connection, without a restart. See [your own server with a self-signed certificate](../README.md#your-own-server-with-a-self-signed-certificate).
- For a private certificate authority, use the existing `[network] ca_bundle` setting/trust store instead: it keeps full verification on. See the [TLS troubleshooting section](../README.md#lastfm-sign-in-tls-certificate-error); it applies to Subsonic too. Do not replace the trust store with an untrusted certificate bundle.
- Playback queue names are credential-free, ephemeral `subsonic://...` source IDs, not HTTP URLs. They and their positions are not restored after reboot. Generic HTTP diagnostic URLs have their query removed.

## Saving music to the SD card

Streaming is not the only way to listen: a track, or the rest of the page it sits on, can be **saved to the card** once and played from there afterwards, without Wi-Fi and without the server. Long-press a track — a press, not a tap — and choose **Save this track** or **Save the whole album**; on an album or folder page the second saves every track from the pressed one to the end of the page, so press the first track for the whole album. **Downloads** on the same menu, or the arrow in the corner of the Subsonic home page, opens the list of everything already saved.

- Files are written as ordinary audio, in the format the **Quality** setting selects: MP3 or Opus transcoded by your server, or the original file when Raw is chosen. A download therefore holds exactly the bytes playback would have streamed, and changing Quality later does not rewrite what is already saved; new downloads use the new setting. Raw files that are not FLAC, MP3 or Opus are refused, as they are for playback.
- They go to `<card>/Music/Subsonic Downloads/<artist>/<album>/<artist> - <title>.<ext>`, named from each track's own tags — the same `Music` folder the other streaming services save into, so they are ordinary local tracks for the library, playlists and the rest of the player. Characters a FAT card cannot store are replaced, and long names are shortened on a character boundary. Each album folder also gets its `cover.jpg`/`cover.png` artwork (once, from the server) and a small hidden marker recording which album it belongs to, so two albums with the same name do not overwrite each other.
- One download runs at a time, in the background: browsing, playback and the rest of the player keep working, and the progress card on the Downloads page shows the track and how far it has got. A second request while one is running is refused rather than queued. Wi-Fi is kept awake while a download runs, however the Wi-Fi policy is set; playback of a saved file is ordinary local playback and does not need the server.
- A file is written beside its final name with a `.part` suffix and only renamed once it is complete and its length matches what the server promised. A cut connection, a cancelled download, or a card removed mid-transfer therefore leaves no half-written track, and a cancelled or failed file is deleted rather than played later. Tracks already saved at the current quality are recognised and skipped, so an album can be downloaded again after adding a track.
- Saved files are permanent: nothing prunes them, and no storage limit is imposed. Delete one from the Downloads page (long-press its row): its `.tags` sidecar goes with it, and an album folder that is left holding only its cover and marker is removed, then its artist folder if that is empty too. Nothing outside the download folder is ever deleted.
- The downloads page has no download button of its own: a save starts where the track is chosen, in the list. Its rows play the saved file and its progress card can cancel the running transfer.

## Transport and playback

```text
GUI browse worker ── bounded JSON REST calls ── metadata/page mailbox

HTTP/TLS producer A ── 1 MiB RAM ring ── local codec callbacks ── existing DSP/ALSA
HTTP/TLS producer B ── 512 KiB next-track pre-cache ── promote to active at handoff
                       (second fixed 1 MiB allocation)
```

- The two rings are allocated lazily; audio caching occupies **2 MiB maximum**, independently of track length. REST response/token arrays, queue metadata, codec memory and worker stacks are additional memory, not part of that figure. No audio is spooled to disk and there is no localhost HTTP proxy.
- Each producer owns all its socket/TLS operations. The decoder only copies RAM and, when necessary, waits on a condition variable. Power-of-two wrap masks avoid 64-bit division on the MIPS read path. There is no per-read allocation or network polling on the audio thread.
- Each new producer/API worker reserves a 256 KiB stack rather than inheriting a multi-MiB default; codec and crash-signal stacks are separate.
- Refill begins at approximately **50%** of the active quota and fills to the high watermark. Otherwise workers sleep. They run at nice 10, not `SCHED_IDLE`, which could starve a needed refill behind UI work.
- Startup primes **512 KiB** or a clean, nonempty short file. At a decode-block boundary, below **64 KiB**, playback waits toward 512 KiB rather than repeatedly decoding tiny arrivals. Stop/replacement can abort waits. Pause interrupts only the block boundary, never an in-progress codec read as fake EOF.
- At approximately **90% playback progress**, the GUI asks the playlist for a nonmutating next-track prediction and fetches its first **512 KiB** in the spare slot. Prefetch starts only when the active reserve is at least 512 KiB or its transfer has completed; an already-started transfer can share bandwidth. Repeat-one uses a fresh instance starting at byte zero; an unknown new shuffle deal cannot be predicted.
- A decoder lease prevents a slot from being reset while a codec still uses it. Stop, local playback and service switches cancel producers without joining them on the UI thread. Generations discard stale data/results.
- Chunked HTTP framing and trailers are removed before audio/ICY data. Framing state survives receive timeouts. Bounded same-connection retries cover transient receive stalls; starvation eventually fails rather than spinning at real-time priority or advancing through a broken queue. Empty bodies, API error pages and truncated Content-Length/chunked transfers fail.
- The initial read/rewind probe is protected from a racing producer. MP3 gets only retained-header rewind/forward skip callbacks; **unknown lengths are never determined by scanning a live stream**. Known Xing/LAME delay/padding is honored. FLAC uses forward/header callbacks; Opus is nonseekable and honors preskip/end trimming. Missing codec lengths use the server duration for progress.
- Playback seeking and local-file waveforms/tag reads are disabled for these RAM sources. Wi-Fi idle/suspend policy recognizes service playback and outstanding HTTP/REST demand.

## Gapless and power: what is and is not guaranteed

The existing ALSA handoff is reused when rate, channels, bit depth and output route match. A ready pre-cache removes the next HTTP request from that critical path. It does **not** pre-initialize a second decoder or resample unrelated formats to force a match.

For a gapless check, enable the player's gapless mode, disable track fades, and use adjacent tracks with matching output parameters. Lossy files/transcodes must carry valid encoder-delay/end-padding information (MP3 Xing/LAME or Opus timing); a live MP3 encoder that omits it can introduce its own padding. FLAC is a useful bit-exact baseline.

A buffer cannot mask an arbitrarily long outage or a connection slower than the audio consumption rate. Short tracks, missing/incorrect server durations, a fresh shuffle round, huge codec headers, route changes and a prefetch that did not finish can still require a startup/rebuffer wait. System DNS resolution is the existing blocking resolver call and is not directly canceled by shutting down a socket.

Worker sleeping, bounded RAM and server transcoding reduce avoidable work, but **no battery-life, CPU-percentage or absolute no-stutter guarantee is claimed**. Those require measurement on the actual V1 and network.

## Validation

See [the test instructions](../sonix-player/tests/README.md) for the real local-HTTP integration harness. It exercises authentication/credentials, browsing/pagination, memory bounds/refill, chunk framing/retry, truncation, cancellation and two-slot ownership. `make subsonicdl` runs the download worker against a real temporary card and a fake transport: names, album folders and their single cover, a cut transfer leaving nothing behind, cancellation, and a delete that empties both folders. `make subsonic-all` runs everything, including the pages in all 11 languages. The optional codec suite uses the actual player decoders and deterministic MP3/FLAC/Opus streams, including large headers and exact Opus trimming. Both suites have ASan/UBSan targets.

These are host tests, not hardware certification. Before shipping a firmware build, test on a V1:

- sustained MP3 256/320, Opus, and raw FLAC playback while browsing/redrawing covers;
- transitions between matching tracks with a known continuous recording, including repeat-one/all and shuffle;
- tracks shorter than ten seconds and albums/directories longer than 200 entries;
- pause/resume, rapid Next/Previous/Stop, switching to local/Tidal/Qobuz/podcast playback, and replacing a track during initial buffering;
- temporary Wi-Fi stalls, server disconnects, malformed responses and a server ignoring the selected codec;
- valid/expired/private-CA HTTPS, rejecting a downgrade redirect, and browsing-away during a pending connection;
- a self-signed HTTPS server with the certificate switch off (refused), on (plays), and with a certificate that names another server or has expired (refused either way);
- a whole-album download on each quality, watching the card's free space and the progress card while playback continues; cancelling mid-transfer; pulling the card during a download; deleting a saved track and its emptied folders; and playing saved files with Wi-Fi off;
- screen-off playback, automatic Wi-Fi/suspend policy, RAM and CPU use, ALSA underrun logs, and battery drain against a local-playback baseline.

The repository's target build/packer discovers the new C sources automatically. The strings are supplied in the one language tree the V1 image ships, `sonix-packer/assets/V1/usr/resource/sonix/language/`.
