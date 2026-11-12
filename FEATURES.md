# Features

---

## Supported audio formats



`.wav` `.flac` `.mp3` `.ogg` `.opus` `.m4a` `.m4b` `.mp4` `.aac` `.wv` `.wvc` `.ape` `.dsf` `.dff` `.aif` `.aiff` `.aifc` `.caf`

**CUE sheets** split a single ripped disc into up to 99 tracks, taking the format from the file the sheet points at (and finding the audio file by stem when its extension differs from the `FILE` line). WAVs carrying their own RIFF markers work the same way.

**Format & tag tolerance** - Containers and codecs are identified from file contents rather than extensions, so misnamed files scan and play cleanly. FLAC files carrying leading ID3v2 headers, broken or non-standard ID3v2 tags (v2.4 non-syncsafe sizes, unsynchronised UTF-16 frames, zlib-compressed frames, BOM-less UTF-16, chained tags), multi-page Ogg Vorbis/Opus comment headers, and APEv2/ASF/MP4/RIFF/AIFF/CAF metadata are all read reliably.

**WAV and AIFF tags** are read from the ID3 chunk taggers add to them, so titles in any script and the cover come through; a WAV's own INFO list fills in whatever that leaves empty.

## Audio

- **Volume** - The same volume curve as the stock player is used.
- **Remember Volume** Four levels remembered separately, one per output: 3.5 mm, 4.4 mm, USB-C, Bluetooth.
- **Graphic EQ** - 10 bands at 31 Hz to 16 kHz, ±12 dB. Ten built-in presets and
  as many of your own as you like.
- **Parametric EQ** - 10 bands, each with frequency 20 Hz to 20 kHz, gain
  ±15 dB in tenths, Q from 0.10 to 10.00, and five shapes: peak, low shelf,
  high shelf, low-pass, high-pass. Preamp −15 to +6 dB. The page draws the real
  response, computed from the coefficients that are running. Presets are
  Equalizer APO files in the `PEQ` folder of the card: the `ParametricEQ.txt`
  that AutoEq publishes for your headphones loads as it is, and a preset saved
  on the player opens in APO. Two persistent PEQ profiles keep separate bands,
  preamp and on/off states for two pairs of headphones. Select Profile 1 or 2
  on the PEQ page; importing, saving or resetting affects only that profile.
  The numbered **PEQ profile** button in Control Centre switches between them
  without leaving playback. It is added after the existing buttons: swipe
  to the control centre's second page if it is below the first eight, or use
  Settings → More → Control centre to make room for it on a full panel. A long press opens the PEQ page.
  Existing PEQ settings become Profile 1; Profile 2 starts flat and off. Switching is manual and
  preserves each profile's on/off state, rather than enabling EQ automatically.
- **MSEB** - HiBy's MageSound tuning, matched to the stock player: 10
  characteristics driven by 13 filters, with the slider travel selectable
  between 20, 40 and 100.
- The graphic EQ, PEQ and MSEB run on **radio stations** too: the same chain a
  track from the card goes through, applied to the stereo PCM about to be
  written. A station is not a file, but what is tuned is for the headphones.
- **Soundfield** - mid/side width from mono to twice natural, starting at 1.50
  for an audible widening when switched on (1.00 is neutral).
- **Channel balance** - ±20 dB in half-decibel steps.
- **Crossfeed** - level, cutoff and delay, all adjustable.
- **ReplayGain** - off, track or album, read from the tags.
- **Fade** - one to twelve seconds at the track boundary.
- **Gapless** - on by default; the PCM device stays open between tracks of the same shape. A track that ends on its own hands over its queue. A manual skip (next, previous, or a track picked from a list) drops the old queue and keeps the device open, so the next track skips the device open and the DAC mute. A saved `audio.gapless = 0` turns it off.
- **DSD** - DoP by default, with nothing in the chain allowed to touch the
  samples; or converted here to 176.4 kHz PCM. DoP falls back to conversion by
  itself when the device refuses the rate.
- **DAC controls** - four digital filters, DRE, NOS, high gain, and DSD gain
  compensation from 0 to 6 dB.
- **Line out** - drives the jack at a fixed level for an amplifier, refuses
  volume changes while on, and puts your level back on the way out.
- **USB DAC** - the player becomes a USB sound card, 32 kHz to 384 kHz. The
  Control Centre remains available for EQ, PEQ, MSEB, gain and output controls;
  the graphic EQ, PEQ and MSEB process incoming PCM, while DSD passes through
  untouched.
- **USB audio out** - a DAC or USB-C headphones driven with the player as host.
- **External USB DAC** - a switch under Developer options that keeps the
  USB-C port supplying power and acting as host, for a dongle that the
  default dual-role setting never brings up. It is what made a JCally and
  an ESR dongle work on a TempoTec V1; it stays on across reboots until it
  is switched off. **A computer or a charger must not be connected while
  it is on.** ADB, DAC mode and card sharing take the port over from it and
  give it back afterwards, and it is given back automatically before a
  suspend. The port's role is also read back every second while it is held
  and written again if the controller has returned to its own default, which
  it does on its own after an attach or a detach. A dongle that goes quiet is
  never routed around: the track waits for it rather than being played to the
  built-in DAC, and switching the Bluetooth radio off does not take the route
  back either. The dongle's own volume control does most of the attenuation
  rather than being left at full gain with the samples turned down, which is
  what keeps a sensitive pair of IEMs free of background hiss -- and how far
  down that control really works is measured as the dongle is picked up, so the
  knob stays smooth and audible all the way to zero instead of going dead where
  the dongle's output stops following. The older twenty-second **Try external
  USB DAC** trial is still there, underneath it, for a one-off test that leaves
  no setting behind.
- **In-line remote** - the three-button cable remote: one click play/pause, two
  next, three previous.

## Library

- **Sorting across alphabets** - a collation that groups by script, folds case and accents, and skips leading articles unless you turn that off.
- **A–Z strip** - shown when scrolling a list.
- **Search** across tracks, albums and artists.
- **Albums with the same name stay apart** - an album is its name and whose it is: the album artist, the artist when there is none, and the folder when there is neither.
- **Quality badges** under a title: lossy, CD, Hi-Fi, DSD.
- **Remembered file browser** - Browse returns to the last folder and scroll position after a restart, regardless of which track is playing. If no saved folder is available, it falls back to the remembered local track’s folder (when Remember track is on), then Music or the card root.
- **Years, Playlists and Most played** - tiles on the Music page alongside All tracks, Albums, Artists, Album artists, Genres and Browse. Every local track play is counted in the library database, and Most played lists your most-listened tracks in descending play-count order. The Music page tiles can be reordered or hidden via **Music → Settings → Library → Rearrange main page**.
- **Picture viewer** - JPEG (`.jpg`, `.jpeg`) and PNG (`.png`) images can be opened directly from the File Manager.
- **Playlists** - create, rename, reorder, delete. A long press on a row of All tracks, Albums, Artists or Album artists starts a selection, and the chosen tracks, albums or artists go into the queue, the favourites or a playlist together. Inside a playlist or the favourites the selection also takes the chosen tracks out, after asking. In the file browser the same works on tracks and folders, a folder bringing everything under it. A backup writes an extended M3U to the card that other players can read; import reads one back, resolves missing entries by file name, and reports what it found.
- **Deleting tracks from the card** - with **Music → Settings → Library → Allow deleting tracks** on, a selection on a track list offers a trash button, and the chosen files are deleted from the microSD card after a confirmation that names how many there are. The index, the favourites and the play counts let go of them at once instead of waiting for the next scan.
- **Playback modes** - normal, repeat all, repeat one, shuffle, and a continuous shuffle that deals a fresh order each round.
- **Album and folder chaining** - the next album starts when one ends, or the next folder in a depth-first walk when you came in through the browser. With **Play albums back to back** on, the circle-play menu of an album list also offers **Play albums in random order**: one record is drawn and played through, and the ones that follow are drawn at random too.
- **The queue is editable** - a long press on a row starts a selection and the trash button takes the chosen tracks out of the queue (the next one starts if the playing track went). Dragging a row by the grip at its right moves it; the queue on disk follows at once.
- **The local queue survives a reboot**, with positions, and so does the volume. Ephemeral Subsonic queues do not.

## Smart Mix

- **Start from a track** - choose **Start Smart Mix** in a track's menu, its details card, a search result or a one-track list's play menu, then choose **10, 15, 20, 25 or 30 tracks** in the popup (including the starting track). The player builds a mix up to that size in the background, with a progress page and a cancel button. Episodes and streamed cache tracks are not offered as seeds.
- **Saved as a playlist** - on the Playlist page, **Create Smart Mix** walks the library's own album and track lists to choose the starting track, or takes the single track being added to a playlist. After choosing the starting track, the same track-count popup lets you choose 10–30 tracks or cancel without creating anything. Fewer tracks may be returned when similar tracks run out. The mix is written to the card as an ordinary playlist named after the seed (`Smart Mix - <track>`, with a ` (2)` tail when the name is taken), opens as its own track list, and leaves whatever is playing alone. The same action is reachable from selection mode's **Add to playlist**.
- **Built entirely on the player** - after a scan or change check, the same library worker builds missing or outdated Smart Mix data from the scanned tags, as its own phase after the music scan. Disabling Detect changes also suppresses this automatic work when the card returns; a manual library scan still builds it. No computer or network is required; the graph lives beside the index in `.local/genius.db`.
- **Genre-based candidates** - one genre per cluster, ranked by shared album, artist and year. Each track keeps up to 32 candidates, with at most two per artist and three per album; a large library keeps fewer per track (never under four) so the whole neighbour list stays inside a fixed memory budget. Empty tags are treated as ordinary values rather than dropping the track.
- **Progress and background work** - the scan page keeps its tracks-written count and shows Smart Mix indexing separately, with its own completed/total percentage and a time estimate. **Continue in background** leaves the job running and returns to the same page later; the display may turn off while the device stays awake. Cancel stops the active phase and prevents the later phases.
- **Complete generations** - a bounded staging database is synced and then published by a same-directory rename, so a half-written graph never replaces the working one. Cancellation, a changed input, a reached limit or a write error leaves the previous graph in place. Removing the card stops and waits for the worker before unmount, and abandoned staging is discarded on the next build. The generation records the producer, the algorithm and the SHA-256 of the exact normalized input paths and tags, plus the static graph digest, so a graph built from the same inputs elsewhere is recognized without rebuilding.
- **Large libraries** - the graph is built in two streaming walks of the index rather than from a whole-library copy: the first counts the rows and keeps each distinct artist, album and genre once, the second writes one path and one fixed-width record per track. Tags are reduced to their sorted ranks, and the graph is encoded straight into the staging database in 16 KiB pieces. A 25,000-track library therefore builds in a few megabytes of memory and mounts a graph of about five, and a 50,000-track library is supported the same way.
- **Bounded indexing** - up to 65,535 library rows (including ignored empty paths), 6 MiB of path and tag text, 4,096 bytes per field, 12 MiB of builder allocations, a 12 MiB decoded snapshot and a 2 MiB neighbour array. Ranking scores at most 4,096 tracks of a genre per seed, sampling a larger genre with a fixed stride instead of scanning all of it, and keeps the reference producer's order. Reaching a limit preserves the old graph and leaves the library usable; a Smart Mix failure never fails an otherwise successful scan.
- **An ordered queue** - spacing and selection decide the sequence, so shuffle and repeat do not alter a playing mix, and the fixed order survives a reboot.
- Missing data, too few similar tracks and a resource limit each raise their own message, and snapshot memory and graph traversal have explicit limits.

## Now playing

- **Show artist** in the player menu opens the current track’s library artist page; tracks filed under multiple artists offer a choice. Tidal tracks retain their artist search.

- Artwork edge to edge across the top, and the same picture flipped, blurred and dimmed behind the controls. Embedded or folder covers up to 32 MB are memory-mapped directly from the card, pre-fetched for the upcoming track, and cached as JPEGs (up to 1,000 Now Playing sleeves alongside list thumbnails) with a dedicated cache-lookup worker for near-instant skips.
- **Alternative layout** - the progress bar becomes a **waveform**
- **Immersive layout** - the artwork is centre-cropped to fill the space above the controls, while a dimmed, blurred vertical reflection of its lower edge forms the background beneath them. Works with any source that has artwork; without it the player falls back to the theme.
- **Album art shape** - choose Rounded, Square or Vinyl (a circle) in Settings → Music → Display options. Applies to the inset sleeves in Standard, Waveform and Alternative; edge-to-edge layouts retain their own shape.
- **Poster layout** - the sleeve scaled to the edges of the screen without the crop Immersive takes: the whole picture from side to side at the width of the panel, held at the top with square corners and nothing stretched, and the deck below it on its own opaque ground rather than over the artwork - as much of the sleeve as the deck leaves room for, at the largest size the screen allows. Anything holding artwork gets one, and without a picture the empty sleeve stands edge to edge in the cover's colour with the controls in the theme's ink; the favourite marker is a heart on this deck, kept a step smaller than the star it stands in for; and the foot of the sleeve fades into the deck over its last pixels instead of meeting the controls at a line.
- **Album Art Max layout** - the sleeve instead of the controls: the picture at the width of the panel with the status bar kept above it, the format, the shuffle mode, the menu and the heart in one row under that bar, and a minimal scrub bar along the foot of the artwork, with the artist, the track name and the place in the queue on a black strip below. There is no transport on the page - the physical keys are the transport, which the status bar's play/pause mark confirms - and double-tapping the left or right side of the picture is the previous or next track. Nothing is written over the photograph and the strip carries everything that names the track: the artist first, in the record's colour, then the title in the theme's ink, each given the width of its own words so that neither scrolls unless it has to. The scrub bar, the heart, the format line and the queue count are all drawn in the colour of the album art, so the page belongs to the record rather than to the theme. The bar is a thin band of colour that deepens towards the playhead, which is a tab rather than a knob. The two clocks appear in the middle of the picture, large, only while a track is being scrubbed (a finger on the bar or a held skip key), with the artwork dimmed behind them. *Scrub bar on the left* moves the bar up the left edge instead, where the thumb travels vertically. The V1's panel only, where every number in it is measured; the other boards keep the standard arrangement.
- **Vinyl layout** - a thin white rounded border around the sleeve, with a static black record peeking out below it and a compact deck (names, seek bar and transport) underneath. The record's exposed lower arc has clean 2 px concentric grooves in lifted greys and a slightly larger centre label peeking below the sleeve; its small pressing mark matches the reference: a thin rounded stroke leaning down to the right in the exposed lower arc. The exposed record ends above the deck with a clear gap, so it never overlaps the text, seek bar or buttons. The record is drawn once per layout/theme change rather than animated, to keep redraws and battery use down. The V1's 240×320 panel only.
- **Sticker layout** - a pop-art neon arrangement on a deep violet-black ground (`#080510`): the rounded sleeve wears a straight hot-pink border (`#ff2b93`) and a outer cyan border (`#18e0f8`) over a counter-tilted (`-6°`) double neon frame, with a centre CD spindle hole (`#ffe600` on `#080510`) and four corner stickers (tilted yellow queue badge at top-left, tilted cyan format badge at top-right, pink shuffle/repeat disc at bottom-left, and golden-yellow favourite heart disc at bottom-right). Below the sleeve, the track title sits on a tilted hot-pink tape strip (`-2.5°`), the artist on a counter-tilted bright-yellow tape strip (`+2.0°`), the album title on a cyan line underneath, and the seek bar inline between the two cyan clocks (hot-pink played fill, yellow knob, dark-purple track) above the ringed hot-pink/yellow play button. The V1's 240×320 panel only.
- **Player layout** - Standard, Waveform, Alternative, Immersive, Poster, Album Art Max, Vinyl or Sticker, in Settings → Music → Display options; the choice is remembered across restarts.
- **Tint controls from album art** - optional, off by default, in the same Display options: the play/pause button and the played part of the progress bar take the main colour of the playing track's artwork, in every layout, while previous and next keep their colour. The colour is the one already worked out from the decoded artwork, so nothing extra is decoded and playback is untouched; a track with no artwork keeps the theme's colours.
- **Legacy play button** - optional, off by default, in the same Display options: the play/pause button sits on a filled circle as big as the button, as it did in the original player, instead of standing as a bare glyph. The circle is white over artwork and in the dark palettes and the palette's grey on the light page, with the glyph in the accent. Where the accent would wash out on it - a yellow, lime or green accent, Tokyo Night's violet, OLED's white - the circle takes the accent and the glyph turns black (or white where that reads better), and Amber on Black and OLED always fill it with their colour. With album-derived tint (including Waveform), the circle stays white and the glyph takes the album colour, even if the tint is pale. A finger on it darkens it.
- **The control centre** - pulled down from the status bar, or up from the bottom edge with **Appearance → Stock gestures** on: the round quick controls, the brightness bar under them -- or the volume, with **Screen → Volume in control centre** -- and the playing track over its transport. **Tap the title or the artist to open Now Playing** for what is playing, and the control centre closes on the way: the player comes up over the page, or is simply revealed when it was already open behind the panel. The two lines do nothing while the card says there is nothing playing.
  **A second page of round buttons** is a swipe away -- a pair of dots under the brightness bar shows which page you are on, and tapping them turns the page too. A fresh install ships the second page filled in with ten toggles for things that used to be several taps deep in Settings: **Airplane mode** (Wi-Fi and Bluetooth off together; the next tap puts back only the radios that were on), **Crossfeed**, **Soundfield**, **ReplayGain** (off, or back to the mode you used last), **Mono**, **Key lock** (lock the keys when the screen is off), **LED**, **Rotate screen**, **Screen off** and the **Power menu**. A long press opens the Settings page behind a toggle that has one. Screen off and the Power menu are actions rather than switches and are never lit. Which buttons are on which page is set in **Settings → More → Control centre**: the first eight in use are page one, the next eight page two. Players that already have a saved layout keep it exactly as it is; the new buttons wait in that page's "not in use" list until you add them. **Edit mode** rearranges the tiles in place: long-press the card where there is no button (the margins, the gaps between buttons, the dots) and every tile gets a ring and the slider turns into a **Done** button. Tap a tile to pick it up and tap another to swap the two -- across pages too, swiping or tapping the dots to reach the other page; tap the picked-up tile again, or empty space, to put it down. Each swap is saved at once, and the panel never opens in edit mode. Adding and removing tiles stays in Settings → More → Control centre. A button for a module switched off in Plugins is not drawn, and the ones after it close up the gap.
- **Cover Flow** - an album carousel: pull up from the bottom of the main menu and it follows the finger up over the page; drag it back down -- from anywhere on the sheet, or from the grip under the status bar -- and it goes away. With **Appearance → Stock gestures** on it comes down from the top edge of any page instead, and goes back up, as on the original firmware. Off by default in Settings → Music → Display options.
- **Lyrics and LRCLIB** - swipe left on the artwork to show the playing track's lyrics, read from a sidecar `.lrc` or `.txt` file beside the track or from its embedded tags; synced lyrics follow the song line by line, tapping a line seeks there, and the screen stays lit while reading. On a local track with no lyrics, the same swipe offers to look them up on **LRCLIB** (and offers to turn Wi-Fi on first when Wi-Fi is off, starting the search automatically once connected), lists the ranked synced and plain matches with a full-text preview, and saves the chosen lyrics atomically as `<track>.lrc` beside the file; while lyrics are open, the corner search button looks up alternatives on LRCLIB to replace them.
- **Queue**, **Details** and, while an audiobook is loaded, **Chapters**.

## Streaming

- **Subsonic / Gonic** - a new Streaming destination for your own server. Test the connection, browse music folders or artists/albums in pages of 200, and play the page as a queue. MP3 256/320, Opus, or original FLAC/MP3/Opus; transcoding runs on the server. Two fixed 1 MiB RAM rings isolate HTTP/TLS from decoding, refill near 50%, and pre-cache 512 KiB of the upcoming track near 90%. No growing disk cache or playback seeking; compatible-format gapless uses the existing ALSA handoff. Remembering credentials is optional and stores an unencrypted private file. See [setup and limitations](docs/subsonic.md).
- **Tidal** - sign in with the code on screen or with the QR-Code; Low, High, CD and Hi-res quality. Search, albums, artists, playlists, favourites, favourite artists, favourite playlists (the ones somebody else made that the account keeps) and the featured lists. Favourites and playlists can be managed from the player; the V1-sized page keeps the last eight searches, lets home rows be hidden and reordered, removes playlist tracks by position, and selects several rows for one action. First pages of lists are cached on the card for half a day, API and sync requests are serialized, and playback downloads can be queued, cancelled or saved permanently under `Music/Tidal/<artist>/<album>`. The playback cache lives on the card with free-space checks and a 1 GiB ceiling.
- **Qobuz** - sign in with the account; MP3 320, FLAC 16/44.1, 24/96 and 24/192. Search, albums, artists, playlists, favourites, featured lists and new releases by genre. Favourites and playlists can be managed from the player. Track and album row menus can download MP3 320, FLAC 16/44.1 or hi-res FLAC (up to 24/192) for offline listening; Qobuz may supply a lower quality depending on the release and subscription. The download arrow opens live progress and groups the latest 100 saved tracks by album for offline browsing. New downloads are stored under `Music/Qobuz Downloads` in album folders named for the artist, album and year; metadata is stored in Sonix `.tags` sidecars and artwork as `cover.jpg` or `cover.png` when available. Album progress is estimated by track count. Downloads require a writable card and an eligible Qobuz subscription; unavailable tracks are skipped and reported.
- **Podcasts** - the Podcast Index catalogue: search, trending, follow up to 100
shows kept on the device. Downloaded episodes land in the card's `Podcast`
folder, which also takes podcasts copied there by hand — see
[Podcasts on the card](#podcasts-on-the-card). The followed list is ordered by
how much of each podcast has been listened to on this device, most first, with
A–Z and Z–A beside it under Podcast settings → Order; the chart and a podcast's
episodes are kept on the card, so a tab opens on the names it showed last time.
A row's menu has **Select**: tick several podcasts and follow or unfollow them
as a group from the corner.
- **Episodes heard and unheard** - an episode played to its end wears a ticked
badge on its cover in the list and its text steps back, kept in `podcast.db` by
episode id, so it reads the same whether it was streamed or downloaded. A long press marks one heard (or heard
and-not) by hand, for what was finished somewhere else or marked by mistake.
The order button in the heading turns an episode list round: newest first, or
oldest first, remembered in the config and shared with the card's list.
- **The chart follows a region** - the catalogue filters its trending chart by
language and offers no country filter, so the language chosen under Podcast
settings → Trending region is what "trending where I am" means here: the
interface's own language unless another is picked, for a listener who wants the
podcasts of a place they are not. The choice is the one shared list the radio
and the other services write, and a chart with nothing in it falls back to
English rather than emptying the page.
- **Radio** - the radio-browser.info directory by language, country or genre,
plus search, favourites, recently played, and **My stations** read from a
`radio.txt` you write yourself in the card root. MP3, AAC and FLAC (native,
mid-stream join, and chained Ogg FLAC with song titles, up to 24-bit), HTTPS with real
certificate checking (plus an opt-in relaxed mode for a server of your own with
a self-signed certificate), and HLS.


## Wireless

- **Bluetooth out** - the whole stack is brought up by the player rather than by
  the firmware's scripts. Pair, connect, forget; the codec list comes from what
  the connected device announces and the best is chosen at each connection.
  Paired devices can be renamed locally or restored to their original name; the
  remote device itself is not changed. Search runs for up to 30 seconds using
  classic Bluetooth inquiry, without hiding devices that have not announced
  their audio services yet. During the search the player is also discoverable,
  so a car head unit can initiate pairing; stopping the search restores the
  previous visibility setting. For Tesla, start Search on the player, then use
  **Bluetooth → Add New Device → Start Search** in the car. Car interoperability
  still needs validation on the actual vehicle.
- **AirPods** - battery per earbud and for the case, model detection, and noise
  control: off, cancellation, transparency, adaptive.
- **Bluetooth in** - a phone or laptop connects and the player becomes its
  speaker, decoding through its own DAC. The incoming codec, rate and depth are
  shown. The receiving stream's software volume is switched on every
  time a stream appears, so the phone's volume keys scale the sound on the player
  instead of only muting it at zero. aptX gets deeper buffers than the other
  codecs because it is the one that ran dry in the logs
  (`receiver_aptx_capture_ms`, `receiver_aptx_output_ms` in `[bluetooth]`; 0
  turns each off). The graphic EQ, MSEB and PEQ process the incoming audio.
  **The control centre works over the receiver**: pull it down from the status
  bar (or up from the bottom edge, if Appearance has the stock gestures on) as
  anywhere else for volume, brightness, EQ, MSEB and PEQ on/off, and the other
  toggles. Its card shows the phone's track and the codec in place of the local
  transport; the side keys still go to the phone. A long press on **EQ**,
  **MSEB** or **PEQ** opens its page *without leaving the receiver*: it keeps
  playing behind it, and backing out lands on the receiver page again (a swipe
  home or a jump to another section ends it, as leaving the receiver always
  has). Bluetooth and airplane mode are not offered while it lasts, and any
  other long press asks before it leaves, since that would silence the phone.
  Cover Flow stays shut.
- **AirPlay** - the player appears as an AirPlay target.
- **DLNA** - the player appears as a renderer.
- **Wi-Fi** - scan and join the networks in the air, and hidden ones too:
  "Add hidden network" at the foot of the Wi-Fi page takes a name and
  passphrase typed by hand (blank for an open network) and probes for the
  network by name, since it never appears in the scan list.
- **Wi-Fi transfer** - a browser on the same network gets the card: upload
  files and whole folders, rename, move, delete, download, show hidden files.
  Reachable at `sonix-transfer.local`.
- **SonixLink** - a control protocol for a phone: the phone fetches the index
  once and queries it locally instead of paging over the wire. Playback state,
  queue, favourites, artwork and commands.
- **USB mass storage** - connect the device to a PC or a Smartphone to manage the files on the SD Card.

## Podcasts on the card

> Podcasts kept on the card live in the `Podcast` folder, one folder per
> podcast. A folder can be filled by downloading episodes in the Podcasts
> section, or by copying files onto the card from a computer — both are listed
> together behind the section's corner button, and neither needs a network.

- **Title and date from the tags** - a copied episode is listed under its
  title tag and dated by its date tag, at whatever precision the tagger wrote
  it ("2026", "March 2026" or "12 March 2026"), the podcast's episodes newest
  first. What the tags leave out falls back to the file's name and its date on
  the card. A downloaded episode keeps the downloader's word instead: the
  title the catalogue gave it and the publication date stamped on the file.
- **Cover from the folder, else from the tags** - a podcast shows its
  `cover.jpg` (or `cover.png`) beside the episodes, and with no picture there,
  the artwork embedded in the episode files' own tags.
- **Played as podcasts** - the now-playing page marks an episode from this
  folder as a podcast, with its own skip buttons and sleep timer.
- **Heard marks and order here too** - a downloaded episode carries the same
  tick and the same dimming as the streamed one, and the long press marks it
  heard or unheard. A file copied onto the card by hand has no catalogue id, so
  it offers only Delete. The heading's order button turns this list round, and
  the setting is the one the online episode list uses.

## Audiobooks

> Audiobooks go in the `Audiobooks` folder, in any subfolder: `Audiobooks/Author/Series/...` is read as the author and the series when the tags do not say.

- **Library, Series, Authors and Continue** - four sections, each sorted A-Z, Z-A or by date; the library also by recently listened, a series by the number of each book in it. Continue holds the books started and not finished, last listened first; the finished ones are behind the button beside the options.
- **A book is a file or a folder** - an `.m4b`, an `.mp3`, `.opus`, `.ogg` or `.flac` with chapters, or a single audio file is a book; audio files of one folder with the same album tag are one book whose files are its chapters, `CD 1` / `Disc 2` subfolders included, so several books side by side in a folder stay apart.
- **Author and series from the tags** - album artist or artist, `SERIES` / `SERIES-PART` and the movement name and number, in MP4, ID3v2 and Vorbis tags.
- The position is remembered per book, and survives a rescan.
- Chapters from the MP4 container, from ID3 frames, or from the `CHAPTERxxx` comments of Opus, Ogg Vorbis and FLAC.
- The two transport buttons are set independently to 10, 30 or 60 seconds.
- Speed 0.5x to 2.0x with the pitch preserved.
- Stop at the end of a chapter, and rewind a few seconds after a pause.
- **Sleep timer** - set a timer with the desired duration and the playback is paused automatically after it elapses.

## EPUB reader

> `.epub` files need to be inside the `Ebook` folder.

- Support for EPUB 2 and 3.
- Headings, emphasis, lists, quotes, rules, images, alignment and the table of
  contents.
- Text size from 14 to 50, line spacing, word spacing, margins, three reading themes (Paper,
  Sepia, Night) and three page-turn animations. Display backlight brightness can
  be adjusted from the reader menu without leaving the book.
- **Bookmarks** - double-tap the top of a page; up to 64 per book.
- A **reading bar** along the bottom with chapter, page, progress, battery and
  clock, each one optional.
- **Hardware page keys** - Previous and Next turn pages while a book is open;
  in vertical mode they move one screen at a time and cross chapter boundaries
  at the ends. Play/Pause and volume remain available for background audio.

## Tools

> **Calculator**, **Notes**, **Metronome** and **Pet** are in the **More**
> menu, under the file manager. All four are drawn for the panel they are on:
> on the 240x320 V1 the twenty keys stay a finger's width, and each page --
> display, keypad and labels -- fits without scrolling.

- **Calculator** - a display and the twenty keys of an ordinary pocket
  calculator: the four operations, sign, percent, backspace and clear. The sum
  in progress is kept while you leave the page and come back. The keys are
  laid out from the panel's own measurements, so the page fills the V1's 240×320
  screen. The operator keys are drawn a size up and in bold: a
  face sets a multiplication sign a little over half the height of the digits
  it sits beside and a hyphen a tenth of it, so at the digits' own size the
  operator column reads as specks next to the numbers it works on.
- **Notes** - the `Notes` folder of the card, listed: every text file in it,
  one row per note, newest names where the sort puts them, and a row at the
  top that makes a new one. A note opens **to be read**: the text fills the
  page, there is no keyboard, and a tap on the text scrolls it rather than
  placing a caret. The pencil in the corner is what turns the page into an
  editor -- the keyboard comes up, the note makes room for it, and the pencil
  becomes the check that puts the writing away again. While a note is being
  written the file goes back to the card when you press the check, when you
  leave the page, and a couple of seconds after you stop typing, so a note
  survives a flat battery; a note that was only read is left exactly as it is.
  The folder is read every time the page is opened, so a note written on a
  computer turns up without a restart.
  > A note longer than the editor holds (16,384 characters, which is far
  > past anything typed on the glass keyboard) is shown by the file manager's
  > text viewer when it is tapped, with a line at the end saying so, rather
  > than opened in the editor -- writing it there would stop at the limit and
  > cut the rest of the file off. Notes are read without a wait: the editor
  > is filled in one assignment rather than a character at a time.
  > Notes live in `Notes` on the card, beside `Ebook`, `Audiobooks`, `Games`
  > and `PEQ`. `.txt`, `.md`, `.lrc`, `.nfo` and `.ini` files are notes,
  > written as UTF-8; a file saved as UTF-16 or Windows-1252 is read and shown
  > correctly too, and a note longer than the editor holds is offered to the
  > file manager's viewer instead of being cut short here.
- **Metronome** - a tempo from **30 to 250 BPM**, a bar of **one to seven
  beats**, and a click on every beat with the first of the bar accented and
  lit. The tempo is dragged on a slider or taken from the finger with **Tap**,
  which averages the last few taps rather than the last gap alone, so one
  hurried tap does not move the tempo a dozen beats. A tap slower than 30 or
  faster than 250 leaves the tempo at that end of the scale instead of outside
  it, and the button works while the click is running. The click is synthesised
  in the player -- two short bursts, a bright one on the downbeat -- so there is
  nothing extra to ship, and it is played through the same output as the music:
  starting it stops local playback and the radio, as the emulator does. The
  dots follow the beat the audio thread has actually written, so what is on the
  glass cannot drift from what is in the ear. The tempo and the bar are
  remembered across visits and across a reboot; leaving the page stops the
  click, because the page is the only place it can be stopped from.

- **Pet** - a small creature fed by the music it can actually be heard by: a
  local track, a stream, internet radio or a phone over Bluetooth, at a volume
  at or above 10 percent. The pet is a **Pikachu**, drawn as pixel art in the
  six colours of the picture it was taken from, and it answers to a name you
  give it (it starts as Pikachu); the name can be changed whenever you like from
  the pencil in the corner. There is no egg: it is there from the first minute,
  and it grows through four stages (hatchling, child, teen, adult) -- the
  hatchling is drawn at two pixels a cell and the rest at three, so the same
  picture starts small and round and has room to stand up in later.
  It keeps a daily streak, earns **Musical Notes** for the minutes it heard and
  spends them in a wardrobe on hats, headphones, a scarf, a crown, two room
  themes and streak freezes. A **15-minute daily goal** builds the streak and a
  bought freeze covers one missed day. Achievements for listening late, hearing
  an album through, five genres in a day and half an hour without a pause are
  worth extra notes. **The last thirty days give it a character**: a month of
  pop and dance makes an **Upbeat** pet whose energy fills faster and who turns
  up a note or two on the floor as the music goes by, a month of jazz and
  classical makes a **Calm** one with a drifting soundscape on its card, and a
  month of indie, alternative and metal makes a **Curious** one that finds
  something -- notes, and now and then a streak freeze -- after forty-five
  minutes of listening without a break, once a day. Under three days of
  listening it has no character yet, and the page says nothing. A genre tag also
  nudges how it moves: a slow sway for jazz, ambient or lo-fi, a faster bob for
  rock, metal and electronic, and a breath no faster than two seconds even
  then. **Once a day it offers a tap-along**: sixteen beats of the metronome's
  tempo, tapped on the glass, worth up to a dozen notes and a freeze for a clean
  run. And it has a limit on flicking through a library: **more than five
  skipped tracks in ten minutes** and it is dizzy for fifteen -- still fed by
  whatever is playing, but earning nothing until it clears. It never dies, and nothing it does
  punishes a player for not listening: forty-eight hours without music is only
  "sleepy", and the first track wakes it. The page is one borderless card: the
  name and stage top left, **hearts and energy top right**, the animal in the
  middle -- **pat it by touching it** -- the track it is feeding on along the
  bottom, a close corner bottom right, and the day's minutes and the streak in
  two lines under the card. A pencil opens the name, a star the
  wardrobe; both are sheets over the page, and the chevron closes them in turn.
  Its state lives in the settings file and survives a reboot; the only cost is
  one callback a second in the interface loop that is already running, and
  nothing at all when the page is not open. The creature is one picture, in two
  sizes -- 75 KB of read-only data in all -- drawn pixel for pixel and never
  scaled; what it feels and what it wears are a few cells drawn over it on the
  same grid, and the route behind it is one more object drawn from stamps.
  It moves in two frames rather than in an interpolation, and the page redraws
  only what changed: a meter that has not moved is not touched between one
  second and the next. While music plays, the line at the foot of the card says
  what it is feeding on beside a three-bar equalizer. It stands on a route --
  a sky that comes up out of the card, hills, a round tree and a pine, tall
  grass and flowers, a platform under its feet -- and its face is the
  picture's own, except when it is fed. **While music plays it smiles**:
  its eyes go round and bright, its mouth opens on a pink tongue, it steps from
  side to side and sparks fly off its cheeks. (A scarf or collar is over its
  chin, so there the smile stays small.)
  The rest of the time the mood is something added beside the picture: eyes
  shut and a little "z z" when it is asleep, a blue sweat drop when it is sad,
  three gold stars when it is dizzy, hearts over it when it is blissful, and,
  while music is playing, a pair of notes in the air beside it. **It is alive
  between its breaths as well**: it blinks every few seconds, and when nobody
  is touching it, it hops, leans or shakes of its own accord every ten seconds
  or so. On a dark card the black tip of its far ear, which stands above the
  sky, is drawn in true black, a shade darker than the card, because the
  picture's own black is as light as the card there. **It
  answers the keys as well as the glass**: a step up on
  the volume and it jumps, a step down and it ducks, Previous and Next and it
  leans the way they go, and a tap makes it hop with a bobbing heart, even
  during the pat's reward cooldown. **Hold a finger on it for two
  seconds** and three hearts fill under it; the third pays the day's groom,
  once a day. A genre tag dresses it for the track it is hearing -- mirror
  shades for the electronic family, a gold chain for hip hop, a studded collar
  for rock, a cup on the floor for the calm ones. What the wardrobe has put on it -- a party
  hat, a crown, headphones or a scarf -- is drawn over the picture the same way,
  one thing at a time.
  **A Poké Ball sits in the corner**: in it are the day's fortune, one of
  six drawn from the date and worth fifty notes once it has been carried out; an
  hourly note game -- one note crossing one line, six times, five notes for
  turning up and one for each note caught; and the shelf, where what the
  listening turns up goes: every half hour of audible music finds a gold note,
  a cassette, a sticker or a record on the floor, and a tap puts it on the
  shelf, three at most; the ball's button is gold, with a sparkle, while
  something is waiting in it. The card also knows what time it is: a sun in its
  sky by day, and after eight in the evening a night sky with a moon, stars and
  a small lamp, with the month deciding what falls through the air -- rain in
  spring, under a grey sky, leaves in autumn, snow in winter, nothing in
  summer. The two room themes are skies of their own, a sunset and a night.

## Games

> All four are in the **More** menu. They are played on the glass, they leave the
> music alone, and each one keeps its best score in the config file, so a record
> survives a reboot.

- **Flappy Bird** - the original art and physics: a tap is a flap, day and night
  trade places every ten points, and the medals are the real ones. The pictures
  live in the resource tree, so they travel inside the image.
- **2048** - the rules everyone knows, on a board sized to the screen instead of
  to a phone. A swipe sends every tile to that wall, two of a kind make the next
  number, and a new tile turns up after each move. Tiles are drawn with steps of
  the accent's own colour, so the board goes with the theme you picked. The board
  and the score are stored when you leave the page: a puzzle left half-finished is
  still there when you come back. One move can be taken back, because a swipe on
  glass is easily read wrongly, and the game lets you keep going past 2048.
- **Snake** - nineteen cells across, one apple at a time, and a snake that crawls
  along the path it has taken instead of jumping from cell to cell. A tap on either
  half of the board turns a quarter; a swipe says which way you mean. The card it
  opens with offers the walls too: with them the wall ends the run, without them
  the far side of the board is the near side.
- **Dino** - one control. A tap jumps, a finger held down ducks, and a press in the
  air brings the runner down sooner. Cacti come in ones, twos and threes, the birds
  at four heights, and every hundred points it turns to night, with a moon in it.
  The gap between obstacles is always the same distance, so what speeds up is the
  time you get to see them coming.
- 2048, Snake and Dino are drawn with the interface's own pieces: they add no art
  to the image and no space on the card. Each has a glyph of its own in the icon
  set, so the More menu shows the animal the dino game draws, the body of the
  snake, and a board with one merge in it - cut on the same 24 grid, with the same
  line and the same single colour as everything else there, which is what lets the
  theme tint them the way it tints the gamepad they replaced.

## Game Boy

> The `.gb` and `.gbc` ROMs need to be inside the `Games` folder, each in a separate folder: `GB` for Game Boy and `GBC` for Game Boy Color.
> The boot ROMS need to be inside the `Games/Bios` folder, dmg_boot.bin for Game Boy cgb_boot.bin for Game Boy Color. (This step is optional).

- Games show their real cartridge names, matched by CRC32 against the libretro
  databases.
- Save states, battery RAM, five palettes, shaders and GBC color
  correction. Mute game sound in Gearboy settings (also accessible from the
  in-game pause menu) to keep listening to music started before the game. The
  setting is remembered; enabling game sound pauses the current track, and
  muting it again or quitting resumes a track that was playing.
- On the V1 the picture fills the panel width at 240x216 with crisp
  nearest-neighbour scaling, leaving a compact touch D-pad and A/B buttons
  below. The physical Play/Pause button acts as Game Boy Start, and the skip
  buttons act as Select, so neither needs an on-screen button. The shader
  overlays (the pixel grid of the real LCD panels) live inside a 3x upscale
  sized for a 480-pixel panel and are not offered on the V1; palettes and GBC
  color correction do apply.

## Settings

- **Appearance** - dark and light, plus the V1 palettes Tokyo Night, Amber on Black and OLED (true black with no grey cards, white as its accent, and a main menu without the title and tagline); twelve accent colors, clock position, a dynamic tint that borrows the accent's hue for backgrounds and cards, a toggle to show/hide the battery percentage, and **rounded screen corners** (off by default). While they are on, a slider under the switch sets how round: 2 to 20 px in steps of 2, so the shell can read as boxy as the V1 does in the hand. It starts at 16 px, sized to the 2" 240×320 panel's glass, which is what the corners always were; the choice is kept while the corners are off.
- **Appearance → Browse on home screen** - replaces the Music tile with Browse, opening the card’s file browser at its last browsed folder and scroll position. On first use it opens the remembered local track’s folder when **Remember track** is enabled, otherwise `Music` (or the card root if it is absent). Missing folders fall back safely. The chevron and the root button still walk up to the rest of the card. Off by default; Music remains available when switched off.
- **Appearance → Show Books on home screen** - replaces the Audiobooks tile with Books, opening the EPUB shelf directly from home. Off by default; Audiobooks remain available when switched off. Switched on, the two swap places in **More** as well: its Books row becomes Audiobooks, so both libraries stay one tap away and neither is listed twice.
- **Appearance → Main menu icons only** - optionally hide the six main-menu tile labels and show larger, centered icons. Dedicated native-pixel art avoids stretching small toolbar glyphs or repeatedly filtering the colored illustrations. Applies immediately in every theme and is saved across restarts; other menus, the status bar and the now-playing strip are unchanged. Off by default.
- **Appearance → Stock gestures** - moves the two sheets to the edges the original firmware pulls them from: Cover Flow down from the top edge of any page, and the control centre up from the bottom of the screen. Off by default, which keeps this player's own arrangement — the control centre pulled down from the status bar, Cover Flow pulled up from the bottom of the main menu — so an update never moves a gesture that is already in the fingers. Both sheets pick the new edges up at the next pull, with no restart; the grip each one is dragged out by moves with them, and the second page of control-centre buttons is turned to the same way in both arrangements, by a swipe or a tap on the dots under the slider.
- **Appearance → Boot screen** - the picture the next power-on opens with: Retrospace (the default), Space, Travelling in space, Stock, the firmware's own light and dark pictures, or **Custom**, one of your own. The choice is written to the flash marker the boot script reads, so it takes effect at the next boot.
  > **Custom** is a JPEG or PNG from the `Bootlogo` folder in the root of the card, picked from the list under **Custom picture** on the same page. Any size will do: it is scaled to fill the panel and cropped where it overflows, so a landscape photograph boots as a full-screen picture rather than a band between two black bars. The chosen picture is copied into the player, which is why it still boots with the card out — and why a change to the file on the card means choosing it again.
- **Appearance → Font** - the face the whole interface is drawn with: MiSans (the default), Neon 80s and, where the image carries them, Roboto or Roboto Mono. Text can also be sized Normal or Large. Any `.ttf` or `.otf` file put in the `Fonts` folder on the card is offered beside those, so people can install their own; a face without Cyrillic, Hangul, kanji or Greek still draws the whole interface, because MiSans stays behind it for the letters it does not have.
- **Screen** - brightness (5% to 100%), screen-off timeout, double-tap to wake, screensaver,
  and 180° rotation with the touch rotated with it.
- **Screen → Volume in control centre** - what the slider under the control centre's round buttons does. Off, the default, it is the brightness bar, exactly where the stock firmware has it and where this player has always had it. On, the same bar sets the output volume instead: the sun becomes a speaker that changes with the level (silent, quiet, loud) and the bar turns red past 55%, the warning colour the volume pill over the player uses. It is the same level the volume keys, the status bar and the pill carry. The switch takes effect at the next pull, with no restart.
- **Screen → Volume as voltage** - shows the volume as what the headphone socket puts out instead of its place on the 0–100 scale: `502mV` where the status bar and the volume pill over the player read `88` (3.5 mm jack, low gain). Off by default, and off, nothing changes. On, the reading is in volts RMS to three figures, in `V`, `mV` or `µV` as the level calls for: on the 3.5 mm jack in low gain it runs from `39.9µV` at 1 to `1.00V` at 100, and in high gain to `2.00V`. It follows the gain switch and, while a DSD track plays as DoP, the DSD gain compensation, so one figure means one level whichever of them is in force. The player cannot tell its two sockets apart, so the socket the figure is for is picked with two pills under the switch: **3.5 mm**, the default, or **4.4 mm**, which reads twice as high. Bluetooth headphones and a USB-C DAC set the level themselves and keep the 0–100; the reading goes back to volts when the sound comes back to a socket. Everything else still goes by the scale: the keys and the slider move it one step at a time, the speaker glyph changes at the same levels, the pill's reading turns red past 55, and the volume limit is still set on the scale. A voltage is about twice as wide as the number, so the pill's scale is about a quarter shorter, and a centred clock steps aside for it in the status bar. Both settings take effect at once, with no restart.
  > The figure is **nominal: worked out, not measured**. It is the most TempoTec publishes for each socket (2 Vrms on 3.5 mm, 4 Vrms on 4.4 mm), taken to be the top of the high-gain scale, less the attenuation the DAC has been given: the level of a full-scale sine with nothing plugged in. Music sits below it by however far the recording does, and so does anything done to the samples on the way (ReplayGain, the equaliser's preamp, channel balance). A low-impedance load pulls the top of the balanced scale down as well: the 215 mW published for 32 Ω is 2.6 V across it, not 4. It has not been checked against a meter, so read it as a guide to how levels compare rather than as a measurement.
- **Screensaver visualizer** - in Settings → Screen, enable Screensaver visualizer to replace Album artwork with a live spectrum. Two styles, picked with pills under the switch: **Neon** draws eight wide segmented bars in the theme’s accent colour on black over a matching rail; **LED** draws the classic bench analyzer — sixteen narrower bars of horizontal segments on a dark red field that glows toward the floor. Every slot of every bar is drawn all the time: unlit ones as dark red ghosts of the segment to come, lit ones blazing orange, the bottom few brightening to white where the light pools at the floor, with a warm glow behind each lit stack that shows in the gaps between its segments and a bright cap riding each bar’s peak and falling behind it as the music moves on. The bars are weighted perceptually, so the bass end keeps the movement the treble has, however the mix leans. The track information and clock remain visible. Pictures mode is unchanged. The bars rest when audio is paused or silent, and while nothing is playing the rail is replaced by a musical note in the theme's accent colour, centred in the field — the line comes back when playback resumes.
- **Screensaver** - choose to show the cover of the current playing track or pictures from the `Screensaver` folder. On the V1's Tokyo Night, Amber on Black and OLED themes the artwork fills the whole panel and the title, artist and clock are read on a translucent field of the palette's own colour over its foot; Dark and Light keep the blurred strip. Under **Album art**, **Fill** keeps that full-bleed look (the default). **Framed** shows the whole cover instead, at its own resolution, in a rounded frame with margins round it, above the title, artist and clock. That frame is 160 px on the V1. The cover is decoded for exactly that size and never enlarged, so it never turns into blocks.
  > Pictures must be baseline (not progressive) JPEGs, portrait, and no larger than 480x800. The V1 crops and scales them to fill its 240x320 screen. For the sharpest result with nothing cropped, make them 240x320 or 480x640, the screen's own 3:4 shape.

- **Power** - standby, LED behaviour, automatic power-off from thirty seconds to
  six hours, and a charge limit from 80 to 100 %.
- **The screen, while music plays over Bluetooth** - the screen comes back
  instantly and silently in that one case. Powering the panel up is what the
  board notices: the unblank resets the glass, brings its rails back up and
  re-initialises it, and a stream that is being encoded for a pair of headphones,
  or decoded from a phone, is heard to stutter over it. So with a Bluetooth
  stream playing, the screen going dark takes the backlight to zero and leaves
  the panel powered, holding its last frame, and the wake is only the light
  coming back. Nothing playing, nothing held: the panel goes down for real a few
  seconds after the music stops, and before any standby. It costs a few tens of
  milliwatts while a stream lasts, and `[screen] panel_hold = 0` in
  `device_config.ini` turns it off for a card that would rather have them.
- **Battery** - the last few percent are managed rather than run out. At **5 %**
  the player warns: a red card, held for eight seconds, and the panel is brought
  back up first if it was dark, because a warning nobody sees is not one. That
  wake is brief: ten seconds later the panel goes dark again, unless the screen
  has been touched in the meantime. At **1 %** it switches itself off the
  ordinary way - position written, databases closed, card unmounted - instead of
  letting the cell drop out from under it. Both work with the screen off and in
  standby, and neither happens while the charger is in, whether the cell is
  taking a charge or is held at the limit. Below 10 % the player also refuses to
  suspend to RAM: a sleeping player cannot watch its own gauge, and the clean
  shutdown at one percent is worth more than the sleep. The warning is given
  once per discharge; it comes back after the battery has been charged past
  7 %.
- **Charging with the player off** - a player that was switched off and is then
  put on a charger comes up on a battery screen rather than the library: the
  level as a filling battery, the percentage, and whether the charger is still
  charging, finished, or gone. It darkens itself after ten seconds
  (`[power] charge_screen_seconds` in `device_config.ini`, minimum five);
  **press power** to bring it back, **hold power** to start the player after
  all. The screen is told apart from an ordinary boot by a marker the player
  keeps on the writable partition while it is running -- and drops as it
  switches itself off -- so a reboot, a crash or a firmware update still starts
  the player as usual. `[power] charge_screen = 0` in `device_config.ini` turns
  the screen off entirely.
- **Power → Hold power for the menu** - what a held power button does. On, the
  default, the hold brings up the power menu and switching off or rebooting
  takes a slide to confirm. Off, the same hold switches the player off where it
  stands, with nothing on the screen to read or to confirm - for a player that
  lives in a pocket, where the hand holding it wants to know the music has
  stopped without looking at it. The menu is the only place the interface offers
  a reboot, so that comes back with the option.
- **LED effects** - the status light is driven
  directly rather than through the firmware's preset patterns: it fades in on
  power-on, cross-fades between colours, breathes red while charging, turns
  green when the charge is done and fades out on the way down. Brightness (1-5)
  and transition speed (20 ms to 2 s) are set under Power.
- **Date and time** - first-run setup guides you to connect to Wi-Fi, then sets
  the clock automatically (including the time zone, estimated from the internet
  connection, and daylight-saving rules) or lets you set it by hand; the clock
  is written through to the RTC, with a 24-hour clock option.
- **Language** - English, Italiano, Deutsch, Español, Français, Nederlands,
  Polski, Português BR, Українська, Русский, 日本語. The eleven language files are read from the
  resource folder, so dropping another `.ini` in beside them adds a language.
  In Japanese the interface draws its kana, kanji and CJK punctuation from the
  bundled Rodin face rather than MiSans, whose kanji are the Chinese shapes.
  Titles and tags come out in whatever script the interface font covers too -
  Cyrillic, Greek, kana, kanji, Hangul, Thai and Arabic, Arabic right to left -
  so a Russian, Ukrainian or Japanese album title still reads.
- **Remap buttons** - the three media buttons and the two volume buttons, each
  assignable to nothing, play/pause, previous, next, volume up or volume down.
- **Keyboard layout** - ten layouts: English, Italian, German, Spanish, French,
  Russian, Dutch, Polish, Portuguese and Ukrainian.
- **Screenshots** - under More: volume up and the power key save the screen to
  the card.
- **System** - device, DAC, serial number, card space, OS version, firmware
  update from a `.upt` on the card, and factory reset.
- **Developer options** - unlocked by tapping the build number: ADB, **External
  USB DAC** (see *USB audio out* above) with the timed trial under it, log to
  the card, database log, disabling the software volume attenuation, and a
  page of running processes and memory.

### Player status bar

On the TempoTec V1, Music > Display options can show a status bar above the
player. The player fits in the remaining space; Album Art Max already includes
the status bar. The option is off by default. The bar automatically follows the
layout’s background: its blurred backdrop where used, the theme panel for
Poster, or black for Vinyl. If a blurred layout has no artwork, it falls back
to the theme.

The independent Album-colored status bar switch tints the volume, playback,
Wi-Fi, Bluetooth and battery icons with the cover’s dominant color. The battery
fill and charging bolt follow the album color too; turn the option off to
restore the normal battery status colors.

### Hi-res artwork badge

Music > Display options offers an optional HI-RES badge on the now-playing
artwork. Choose Off, Top right or Top left separately for each player layout;
it is off by default. A shared Badge design choice offers the original
two-line HI-RES text (the default), one-line HI-RES, HR, or the official
gold-and-black Hi-Res Audio logo. Changing the design does not change which
layouts show the badge or where they put it. It appears for lossless audio
above 16-bit or 48 kHz, and for DSD, once the stream's format is known. Lossy
audio and podcasts do not show it. When a source badge occupies the right
corner, the hi-res badge uses the left corner instead. Album Art Max reserves
its corners for text and has no badge. Waveform's Standard fallback uses the
Standard setting.

### Both control-centre sliders

Settings > Screen > Both sliders shows separate brightness and volume rails in
the control centre. The setting is off by default; turning it off restores the
previous choice of which single slider to use. On the V1, the quick buttons and
playback controls remain accessible while the track text is hidden to make room
for two full-width, finger-sized targets. USB DAC status remains visible.
