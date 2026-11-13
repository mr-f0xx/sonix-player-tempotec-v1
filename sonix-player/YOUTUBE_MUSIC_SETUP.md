# YouTube Music setup

The player talks to YouTube Music the way the web client does: its unofficial
InnerTube interface (`music.youtube.com/youtubei/v1`) with the `WEB_REMIX`
client. That interface is not a public API, and Google can change it without
notice.

Two things about it shape this page:

- **Browsing needs no account.** Search, Explore, Top songs and podcast search
  are public pages and answer whether or not anyone has signed in.
- **The library needs a real browser session.** Liked songs, your playlists and
  your podcast subscriptions belong to an account, and the only credential
  InnerTube honours for them is the cookie a signed-in browser sends.

So an account is attached one way — by importing a browser session — and the
page works without one.

## Building with the InnerTube key

Every request to that interface carries a key. It is the one the
`music.youtube.com` page hands every visitor — not an account credential and not
anybody's secret — but it is Google's, and a key baked into a public image is one
Google can rotate out from under every build of it. It is therefore not in the
repository: the build supplies it.

```sh
# once, on a machine that builds images repeatedly
printf '%s' 'AIza...' > sonix-player/.ytmusic-key      # gitignored

# or per build, from the environment
YTMUSIC_API_KEY='AIza...' make -C sonix-player target

# or on the command line
make -C sonix-player target YTMUSIC_API_KEY='AIza...'
```

In CI, put it in a repository secret and pass it to the build step as
`YTMUSIC_API_KEY`, which is what `.github/workflows/build-tempotec-v1.yml` does
whenever a `YTMUSIC_API_KEY` secret exists.

A build with none of those still compiles and every other feature works; YouTube
Music then answers `ytmusic_api_key_missing` at its first request — a toast that
says the build has no key, and one line on stderr naming this section — instead
of sending a request the endpoint can only refuse, which used to arrive at the
page as a search that simply found nothing. Unless a key is on the card, which
is the next section and the one that involves neither a rebuild nor a
repository.

## The key on the microSD card (nothing shared, nothing rebuilt)

The same value can come from the keys file instead of the build. It is read
from `[ytmusic] api_key`, either from `streaming-keys.ini` at the root of the
card or from the image's own copy of that file, and the card fills in what the
image does not carry. That is the way to keep a key entirely to yourself: it is
never compiled, never committed, never uploaded to a build service, and it
stays in a file only your player reads.

**Getting the value.** It is the key `music.youtube.com` hands every visitor,
so it is read straight off that page — no account, no consent screen, no Google
Cloud project:

1. Open <https://music.youtube.com> in a desktop browser (signing in is not
   needed for the key).
2. Open the page source with `Ctrl+U` (`Cmd+U` on a Mac), or press `F12` and
   open the **Console** tab.
3. Search for `INNERTUBE_API_KEY`. The value beside it is the long `AIzaSy...`
   string. In the console, `ytcfg.get('INNERTUBE_API_KEY')` prints the same
   thing.

**Putting it on the card.** Create or edit `streaming-keys.ini` in the root of
the microSD card and add:

```ini
[ytmusic]
api_key = AIzaSy...
```

Copy the card back into the player and restart. If the card already carries a
`streaming-keys.ini`, add the `[ytmusic]` section to it — the other sections
and their keys are read exactly as before. A key from either file wins over
one compiled into the build, so it also replaces a baked-in key that has gone
stale.

## The account: import a session from the browser

This is the only credential the music endpoints accept, and the only account the
player offers. There is no "sign in with Google" button: the device token that
flow produced was refused by the music endpoints, so a button for it could only
promise something it could not deliver. Nothing here is needed to search,
explore, play or keep favourites on the player itself.

1. Sign in to <https://music.youtube.com> in a desktop browser.
2. Export the site's cookies with the browser's own export (a `cookies.txt` in
   the Netscape format, which is what "Get cookies.txt LOCALLY" and friends
   produce). Or, by hand: open the developer tools, go to **Network**, reload
   the page, pick any request to `music.youtube.com` and copy the whole `Cookie`
   request header value — one line is enough.
3. Save it as `ytmusic_session.txt` in the root of the microSD card. Both shapes
   are read: every `youtube.com` / `google.com` line of a Netscape export is
   collected into a `Cookie` header, and anything else is taken as one header
   value already.
4. On the player open **Streaming → YouTube Music → the cog → Import a session
   from the card** and tap it.

The card copy is deleted once the session has been saved. The player keeps it in
`/usr/data/ytmusic_session.ini` with mode 0600, beside `device_config.ini`, and
reloads it at boot. The value must contain `SAPISID`, `__Secure-3PAPISID` or
`__Secure-1PAPISID`; anything else is rejected rather than stored.

A cookie expires when the browser session ends or when Google rotates it. If
the account's liked songs stop appearing in Favourites, import a fresh one;
locally hearted songs remain available either way.

## What the page does

- **Search** — songs. A row plays; an album or playlist row opens its own page.
- **Explore** — the store's front page, one row per carousel ("New albums &
  singles", "Trending", …). Tapping a carousel lists its cards without another
  request.
- **Top songs** — the charts page.
- **Favourites** — with a session imported, the list combines your account's
  liked songs with songs and podcast episodes hearted on the player. Without a
  session, it shows only the player's own likes. The now-playing heart and the
  row menu's *Add to favourites* write a local like; they do not change the
  Google account. Local likes live in `/usr/data/ytmusic_likes.tsv` beside
  `device_config.ini` — on internal storage, so they survive reboots and
  updates. A like is a record of the video id, title, artist and album, not of
  a cache file, which is why it no longer disappears when the playback cache is
  swept. The list holds up to a hundred local likes, newest first.
- **Podcasts** — a podcast search. A show opens its episode list. Episode rows
  show the creator, can be sorted newest/oldest, and offer podcast-only skip
  and playback-speed controls. An episode is marked listened when it finishes;
  its row menu can also mark it listened or unlistened. These controls do not
  change ordinary YouTube Music tracks.
- **Artwork** — every row draws the sleeve its entry came with, the same shared
  cover loader the Tidal, Qobuz and Podcasts tabs use. The images are kept on
  the card under `.local/qobuz-art`, so a second look at a list downloads
  nothing.

Selecting a track resolves an audio-only AAC stream, caches it temporarily under
`/tmp/sonix-ytmusic`, and hands that file to the player's existing decoder, which
reads both flat and fragmented (DASH) MP4 — the form YouTube serves. The bytes
are fetched in 256 KB ranges rather than as one long transfer: an unsigned
`googlevideo` address is throttled to about 32 KB/s when read straight through,
and not throttled at all when it is read in ranges, which is the difference
between a couple of seconds and a couple of minutes for one track.

A short or empty range reply is **not** the end of the file. Invidious passes the
bytes on from Google and hands back less than was asked for whenever its own
upstream pauses; taking that for an ending is what used to finish a three-minute
track after a minute, at a file size the player then reported as a 35 kbit/s
stream. The download now asks again from the exact byte it reached, stops only
on a `416` (or after six empty replies in a row), and checks the total against
the `clen` the instance named before calling the file whole.

## Settings

All of these are on **YouTube Music → the cog**:

- **Sleep timer** — the music transport's own timer, the one the card and Tidal
  share, so a stretch set here stops a song at the same point it stops an album.
  A YouTube Music track plays through the same player, so it is the same countdown.
- **Audio quality** — *High* takes the 256 kbit/s AAC stream (itag 141) where the
  instance offers one and the 128 kbit/s one elsewhere; *Standard* pins 128, which
  is half the bytes and the way back when a public instance is too slow to carry
  the better one.
- **Recommendations for** — the country whose store answers, and the language the
  row names come back in: *Worldwide (US)*, *Poland*, *Poland, English rows*,
  *Czechia*, *Germany*, *United Kingdom*. Both parts go into the InnerTube
  `gl` / `hl` of every search, browse and player request, so choosing Poland
  while reading English gives Polish charts with English titles.
- **Home rows** — a page of its own: up and down move a row, and the switch
  beside it decides whether the home page shows it at all. Hiding Explore is the
  point; the order is kept in the config and read again whenever the home page is
  built.

## While a song is loading

A song starts as soon as enough of it has arrived, and the rest keeps downloading
behind the player. "Enough" is an eighth of the file -- between 192 kB and 1 MB,
whichever the instance's own size allows -- and never before the container's
`moov` index is in that part, because a decoder cannot read a track whose sample
table has not arrived. So a song begins in seconds rather than after the whole
transfer, and a slow instance costs a pause at the start instead of a minute of
nothing.

The page shows a card with a spinner, the progress in percent and a bar while
that happens, driven by the downloader's own byte counter -- so a slow start is
visibly working rather than indistinguishable from a tap that did nothing. The
card takes the taps while it is up, so a second press cannot start a second
transfer behind the first. The percent appears as soon as the source says how
large the file is, and the card goes the moment the player starts or the transfer
fails. A transfer that dies after playback began stops the song where the bytes
did and says so, rather than looking like a short track.

The album in the folder path comes from the row YouTube sent (its second column
is `artist • album • duration`) and, where a page does not name it -- an album's
own track list, a playlist -- from the title of the list the song was opened in.
Only when neither exists does the file land under `Unknown album`.

## Saving a song to the card

Once a track has played, open the now-playing menu (the track's own menu, the
same place Tidal and Qobuz offer it) and choose **Keep on card**; a song's row
has the same **Download** entry behind its three dots, without playing it first.
Either way the file is copied out of the temporary cache into
`<card>/Music/YouTube Music/<artist>/<album>/<title>.m4a`, with its `.tags`
sidecar and its sleeve beside it, so the next library scan lists it as an
ordinary track and it plays with no network at all.

## Playback: the device, an instance, or a relay

Google demands a browser-grade attestation — a PO token its JavaScript computes —
before it hands out media, and this device has no JavaScript engine. The
phone-app player request still answers with plain `https` audio addresses, but
when this device fetched one Google refused every media request with 403 —
ranged or whole, and whatever the address. Those refusals are in the player's
own log as `http: media answered 403: …`, with Google's reason in the body. So
the device's own request is no longer the first answer, and a track does not
depend on it.

The order a track is played in, and all of it is done by the player itself: no
PC, no laptop, no phone.

1. **The imported browser session**, when there is one: the web music player
   knows the account's region and, with Premium, its own stream.
2. **An instance** — a Piped or Invidious server. An Invidious instance fetches
   the audio from Google and passes the bytes on through
   `/latest_version?id=…&itag=…&local=true`, so the player only ever talks to
   the instance. This is the path that plays today: measured against a real
   track, `yt.chocolatemoo53.com` answered the formats reply and then served the
   audio in ranges (206 with the matching `Content-Range`, a 3.4 MB track).
3. **The device itself**, through the phone-app player — kept for the day Google
   stops refusing what it hands out.
4. **The relay** — `tools/ytmusic-relay.py` on a machine of your own. Last,
   because it needs another machine.

A source that resolves an address the download is then refused does not end the
attempt: the downloader moves to the next source in the list. Every attempt is
in the log — `asking …`, `fetching from …`, `… kept 0 bytes (failed, …)`,
`played from …` — and the media status is followed by the body of the refusal,
so a failed play names the host that refused instead of saying "could not reach
the stream".

The download is finished when the server has nothing left to send, not when a
reply is short: a proxy handing back less than was asked for is normal, and
taking that for the end of the file is what once cut a three-minute track off
after a minute — at a file size the player then reported as a 35 kbps stream.
So the next range is asked for from where the last one stopped, a dropped
connection gets one more try at the same offset, and the result is checked
against the byte count the instance's own reply gives (`clen`), which the log
names along with the format it chose: `offers itag 140 at 128 kbps`.

The sleeve comes from the row: its thumbnail is fetched at 320 px and written
beside the cached track as `<id>.jpg`, which is where the player looks for a
file's artwork — so now-playing shows the cover the list showed, and not the
placeholder glyph.

**Playback source** in the settings page (Streaming → YouTube Music → the cog)
shows what is in force and opens the list: *Automatic* (the default, the order
above), *This device only*, *Relay on my network*, or any instance. A pinned
source is tried first and the rest are still tried after it, because a pinned
host being down should not mean silence.

### Instances

```ini
[ytmusic]
source = https://invidious.example      # pin one; "auto" is the default
instances = inv.example, piped.example   # replace the built-in list
```

in `/usr/data/device_config.ini` (not `streaming-keys.ini`, which holds the
Tidal and Qobuz keys) — the same file the player writes its own settings to.
`instances` takes a comma or space separated list, replaces the built-in hosts
instead of adding to them, and accepts a bare host name, read as `https://`.

Both keys can be set from the player instead: Streaming → YouTube Music → the
cog → **Playback source**, which lists *Automatic*, *This device only*, *Relay on
my network* and every instance. Tapping one stores it and marks it *On*.

Measured when the built-in list was last checked, against a real track: every
Piped host on the published list (including `pipedapi.kavin.rocks`) answered
502, 403 or nothing at all, and of the Invidious hosts, `inv.nadeko.net` and
`invidious.tiekoetter.com` answered 403, `invidious.nerdvpn.de` answered 401 —
only `yt.chocolatemoo53.com` answered the API *and* served audio. That is why it
is first in the built-in list, and why a host of your own, pinned in the
settings, is the reliable version of this: instances come and go in weeks.

What an instance can contribute depends on what it does with the bytes. A Piped
instance answers `/streams/<id>` with addresses on Google's own hosts, which
belong to the instance's address and are refused when fetched from here — so a
Piped instance only helps when its answer already points at itself. An Invidious
instance is asked for its formats reply, and the bytes are then fetched through
its own `/latest_version?id=…&itag=…&local=true`, which makes the instance fetch
the audio and pass it on: that is the shape that works from another machine.

That last step is a redirect chain, not a file: `/latest_version` answers `302`
to a `companion/…` address on a second host of the same instance, and that one
answers `302` to the `videoplayback` address it proxies — 2256 characters when
this was measured, `expire` and signature included. The player follows both hops
and keeps the `Range` header across them. It also no longer follows a truncated
one: a `Location` longer than the address buffer is refused with
`http_redirect_too_long`, because cutting that signed address at 2047 characters
— what the old 2048-byte buffer did — makes the host answer `500`, and that used
to reach the screen as "could not reach the stream" from every instance, however
healthy.

Nothing here logs in to an instance: the player never sends a Google password
anywhere, and the only account it knows is the imported browser session above.
An instance that answers 401 or 403 is simply passed over, and the next source
is asked. Self-hosting Invidious is what makes this permanent, and its address
then goes in `instances` or in **Playback source**.

### The relay

The relay is for anyone who would rather run one job on their own machine than
depend on an instance. `sonix-player/tools/ytmusic-relay.py` is a single file,
on any machine the player can reach. It needs `yt-dlp`, which runs the
attestation:

```sh
pip install yt-dlp
python ytmusic-relay.py            # listens on 0.0.0.0:8010
```

It answers the two endpoints the player asks for. `/streams/<video-id>` is the
Piped-shaped reply — title, artist, duration, and the one audio stream to fetch —
and `/audio/<video-id>` is those audio bytes, fetched by the relay and passed on.

That second endpoint is the point: the media URL `yt-dlp` resolves is bound to
the address that resolved it, and it is several thousand characters long,
past what the player's own URL buffer holds. A short URL on the LAN has neither
problem, and the audio-only filter stays where it was: the relay offers m4a/AAC
only, and never fetches the video.

What the player does with the reply: an audio address on the relay itself is
fetched as it stands, and one that points anywhere else — a public instance's
`googlevideo` URL — is not fetched at all, because fetching it cannot work; the
player asks the relay for `/audio/<video-id>` instead. A relay that answers with
neither is named as that ("This relay hands out YouTube addresses the player
cannot fetch"), rather than blamed as a broken stream, and the device log records
the address that was tried and why it failed. The relay's own console prints a
line per request (`relay: "GET /audio/..." 200 -`), which is the first place to
look when a track does not start. When every source has been refused, the
message says so ("YouTube refused the audio and no instance answered") instead
of asking for a relay, because a relay is no longer required.

Search and chart results inside explicitly marked ad, sponsored or promoted
renderers are skipped, and before playback the player checks that YouTube
returned the requested video and marked it playable. Only audio formats are
used, never ad-placement media. This is a narrow audio-playback safeguard, not a
network-wide ad blocker, and it does not include SponsorBlock.

## When a page comes back empty

An account Google will not honour is answered with an empty page rather than an
error. For the public pages the player notices that and asks again as a
signed-out visitor, so search, Explore and Top songs keep working. Favourites
has no such fallback: a signed-in player whose session cannot serve the library
is told to import one, and an imported session that has expired — they do —
does it with a fresh one.
