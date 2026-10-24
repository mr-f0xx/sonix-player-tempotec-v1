# Features

---

## Supported audio formats



`.wav` `.flac` `.mp3` `.ogg` `.opus` `.m4a` `.m4b` `.mp4` `.aac` `.wv` `.wvc` `.ape` `.dsf` `.dff` `.aif` `.aiff` `.aifc` `.caf`

**CUE sheets** split a single ripped disc into up to 99 tracks, taking the format from the file the sheet points at. WAVs carrying their own RIFF markers work the same way.

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
  on the player opens in APO.
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
- **USB DAC** - the player becomes a USB sound card, 32 kHz to 384 kHz.
- **USB audio out** - a DAC or USB-C headphones driven with the player as host.
- **In-line remote** - the three-button cable remote: one click play/pause, two
  next, three previous.

## Library

- **Sorting across alphabets** - a collation that groups by script, folds case and accents, and skips leading articles unless you turn that off.
- **A–Z strip** - shown when scrolling a list.
- **Search** across tracks, albums and artists.
- **Albums with the same name stay apart** - an album is its name and whose it is: the album artist, the artist when there is none, and the folder when there is neither.
- **Quality badges** under a title: lossy, CD, Hi-Fi, DSD.
- **Years, Playlists and Most played** - tiles on the Music page alongside All tracks, Albums, Artists, Album artists, Genres and Browse. Every local track play is counted in the library database, and Most played lists your most-listened tracks in descending play-count order. The Music page tiles can be reordered or hidden via **Music → Settings → Library → Rearrange main page**.
- **Picture viewer** - JPEG (`.jpg`, `.jpeg`) and PNG (`.png`) images can be opened directly from the File Manager.
- **Playlists** - create, rename, reorder, delete. A long press on a row of All tracks, Albums, Artists or Album artists starts a selection, and the chosen tracks, albums or artists go into the queue, the favourites or a playlist together. Inside a playlist or the favourites the selection also takes the chosen tracks out, after asking. In the file browser the same works on tracks and folders, a folder bringing everything under it. A backup writes an extended M3U to the card that other players can read; import reads one back, resolves missing entries by file name, and reports what it found.
- **Deleting tracks from the card** - with **Music → Settings → Library → Allow deleting tracks** on, a selection on a track list offers a trash button, and the chosen files are deleted from the microSD card after a confirmation that names how many there are. The index, the favourites and the play counts let go of them at once instead of waiting for the next scan.
- **Playback modes** - normal, repeat all, repeat one, shuffle, and a continuous shuffle that deals a fresh order each round.
- **Album and folder chaining** - the next album starts when one ends, or the next folder in a depth-first walk when you came in through the browser. With **Play albums back to back** on, the circle-play menu of an album list also offers **Play albums in random order**: one record is drawn and played through, and the ones that follow are drawn at random too.
- **The queue is editable** - a long press on a row starts a selection and the trash button takes the chosen tracks out of the queue (the next one starts if the playing track went). Dragging a row by the grip at its right moves it; the queue on disk follows at once.
- **The local queue survives a reboot**, with positions, and so does the volume. Ephemeral Subsonic queues do not.

## Now playing

- Artwork edge to edge across the top, and the same picture flipped, blurred and dimmed behind the controls.
- **Alternative layout** - the progress bar becomes a **waveform**
- **Immersive layout** - the picture itself, edge to edge across the whole panel: the sleeve fills the screen and is centre-cropped, at its own brightness, with nothing laid over it. The controls stand on a frosted band: the foot of the same photograph, lightly blurred, dimmed to the darkness the type reads on and tinted so the sleeve's colour survives in it, cut to the deck's rectangle - so the artwork is still seen through the glass, softly, while the names, the bar, the clocks and the transport are read on top; the chevron and the source marks take their chance in white over the sharp part. Anything holding artwork gets one - local files, podcasts, audiobooks, Subsonic and the streaming services alike - and without a picture the page falls back to its own colour with the controls in the theme's ink.
- **Poster layout** - the sleeve scaled to the edges of the screen without the crop Immersive takes: the whole picture from side to side at the width of the panel, held at the top with square corners and nothing stretched, and the deck below it on its own opaque ground rather than over the artwork - as much of the sleeve as the deck leaves room for, at the largest size the screen allows. Anything holding artwork gets one, and without a picture the empty sleeve stands edge to edge in the cover's colour with the controls in the theme's ink; the favourite marker is a heart on this deck, kept a step smaller than the star it stands in for; and the foot of the sleeve fades into the deck over its last pixels instead of meeting the controls at a line.
- **Album Art Max layout** - the sleeve instead of the controls: the picture at the width of the panel with the status bar kept above it, the format, the shuffle mode, the menu and the heart in one row under that bar, and a minimal scrub bar along the foot of the artwork, with the artist, the track name and the place in the queue on a black strip below. There is no transport on the page - the physical keys are the transport, which the status bar's play/pause mark confirms - and double-tapping the left or right side of the picture is the previous or next track. Nothing is written over the photograph and the strip carries everything that names the track: the artist first, in the record's colour, then the title in the theme's ink, each given the width of its own words so that neither scrolls unless it has to. The scrub bar, the heart, the format line and the queue count are all drawn in the colour of the album art, so the page belongs to the record rather than to the theme. The bar is a thin band of colour that deepens towards the playhead, which is a tab rather than a knob. The two clocks appear in the middle of the picture, large, only while a track is being scrubbed (a finger on the bar or a held skip key), with the artwork dimmed behind them. *Scrub bar on the left* moves the bar up the left edge instead, where the thumb travels vertically. The V1's panel only, where every number in it is measured; the other boards keep the standard arrangement.
- **Player layout** - Standard, Waveform, Alternative, Immersive, Poster or Album Art Max, in Settings → Music → Display options; the choice is remembered across restarts.
- **Tint controls from album art** - optional, off by default, in the same Display options: the play/pause button and the played part of the progress bar take the main colour of the playing track's artwork, in every layout, while previous and next keep their colour. The colour is the one already worked out from the decoded artwork, so nothing extra is decoded and playback is untouched; a track with no artwork keeps the theme's colours.
- **Cover Flow** - an album carousel: pull up from the bottom of the main menu and it follows the finger up over the page; drag it back down -- from anywhere on the sheet, or from the grip under the status bar -- and it goes away. With **Appearance → Stock gestures** on it comes down from the top edge of any page instead, and goes back up, as on the original firmware. Off by default in Settings → Music → Display options.
- **Queue**, **Details** and, while an audiobook is loaded, **Chapters**.

## Streaming

- **Subsonic / Gonic** - a new Streaming destination for your own server. Test the connection, browse music folders or artists/albums in pages of 200, and play the page as a queue. MP3 256/320, Opus, or original FLAC/MP3/Opus; transcoding runs on the server. Two fixed 1 MiB RAM rings isolate HTTP/TLS from decoding, refill near 50%, and pre-cache 512 KiB of the upcoming track near 90%. No growing disk cache or playback seeking; compatible-format gapless uses the existing ALSA handoff. Remembering credentials is optional and stores an unencrypted private file. See [setup and limitations](docs/subsonic.md).
- **Tidal** - sign in with the code on screen or with the QR-Code; Low, High, CD and Hi-res quality. Search, albums, artists, playlists, favourites and the featured lists. Favourites and playlists can be managed from the player.
- **Qobuz** - sign in with the account; MP3 320, FLAC 16/44.1, 24/96 and 24/192. Search, albums, artists, playlists, favourites and the featured lists. Favourites and playlists can be managed from the player.
- **Podcasts** - the Podcast Index catalogue: search, trending, follow up to 100
shows kept on the device. Downloaded episodes land in the card's `Podcast`
folder, which also takes podcasts copied there by hand — see
[Podcasts on the card](#podcasts-on-the-card).
- **Radio** - the radio-browser.info directory by language, country or genre,
plus search, favourites, recently played, and **My stations** read from a
`radio.txt` you write yourself in the card root. MP3 and AAC, HTTPS with real
certificate checking (plus an opt-in relaxed mode for a server of your own with
a self-signed certificate), and HLS.


## Wireless

- **Bluetooth out** - the whole stack is brought up by the player rather than by
  the firmware's scripts. Pair, connect, forget; the codec list comes from what
  the connected device announces and the best is chosen at each connection.
  Paired devices can be renamed locally or restored to their original name; the
  remote device itself is not changed.
- **AirPods** - battery per earbud and for the case, model detection, and noise
  control: off, cancellation, transparency, adaptive.
- **Bluetooth in** - a phone or laptop connects and the player becomes its
  speaker, decoding through its own DAC. The incoming codec, rate and depth are
  shown.
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

> **Calculator** and **Notes** are in the **More** menu, under the file
> manager. Both are drawn for the panel they are on: on the 240x320 V1 the
> twenty keys stay a finger's width, and the whole page -- display, keypad and
> labels -- fits without scrolling.

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

## Games

> **Flappy Bird** is in the **More** menu. It is played on the glass, it leaves
> the music alone, and it keeps its best score in the config file, so a record
> survives a reboot.

- **Flappy Bird** - the original art and physics: a tap is a flap, day and night
  trade places every ten points, and the medals are the real ones. The pictures
  live in the resource tree, so they travel inside the image.

## Game Boy

> The `.gb` and `.gbc` ROMs need to be inside the `Games` folder, each in a separate folder: `GB` for Game Boy and `GBC` for Game Boy Color.
> The boot ROMS need to be inside the `Games/Bios` folder, dmg_boot.bin for Game Boy cgb_boot.bin for Game Boy Color. (This step is optional).

- Games show their real cartridge names, matched by CRC32 against the libretro
  databases.
- Save states, battery RAM, five palettes, shaders and GBC color
  correction.
- On the V1 the picture fills the panel width at 240x216 with crisp
  nearest-neighbour scaling, leaving a compact touch D-pad and A/B buttons
  below. The physical Play/Pause button acts as Game Boy Start, and the skip
  buttons act as Select, so neither needs an on-screen button. The shader
  overlays (the pixel grid of the real LCD panels) live inside a 3x upscale
  sized for a 480-pixel panel and are not offered on the V1; palettes and GBC
  color correction do apply.

## Settings

- **Appearance** - dark and light, plus the V1 palettes Tokyo Night, Amber on Black and OLED (true black with no grey cards, white as its accent, and a main menu without the title and tagline); twelve accent colors, clock position, a dynamic tint that borrows the accent's hue for backgrounds and cards, a toggle to show/hide the battery percentage, and **rounded screen corners** (off by default). The arc is 16 px, sized to the 2" 240×320 panel's glass.
- **Appearance → Browse on home screen** - replaces the Music tile with Browse, opening the card’s file browser in the card’s `Music` folder directly; a card without that folder still opens at its root. The chevron and the root button still walk up to the rest of the card. Off by default; Music remains available when switched off.
- **Appearance → Show Books on home screen** - replaces the Audiobooks tile with Books, opening the EPUB shelf directly from home. Off by default; Audiobooks remain available when switched off. Switched on, the two swap places in **More** as well: its Books row becomes Audiobooks, so both libraries stay one tap away and neither is listed twice.
- **Appearance → Main menu icons only** - optionally hide the six main-menu tile labels and show larger, centered icons. Dedicated native-pixel art avoids stretching small toolbar glyphs or repeatedly filtering the colored illustrations. Applies immediately in every theme and is saved across restarts; other menus, the status bar and the now-playing strip are unchanged. Off by default.
- **Appearance → Stock gestures** - moves the two sheets to the edges the original firmware pulls them from: Cover Flow down from the top edge of any page, and the control centre up from the bottom of the screen. Off by default, which keeps this player's own arrangement — the control centre pulled down from the status bar, Cover Flow pulled up from the bottom of the main menu — so an update never moves a gesture that is already in the fingers. Both sheets pick the new edges up at the next pull, with no restart; the grip each one is dragged out by moves with them, and in the stock arrangement the extra control-centre buttons are unfolded with a tap on the handle under the brightness slider rather than by dragging the card down.
- **Appearance → Boot screen** - the picture the next power-on opens with: Retrospace (the default), Space, Travelling in space, Stock, the firmware's own light and dark pictures, or **Custom**, one of your own. The choice is written to the flash marker the boot script reads, so it takes effect at the next boot.
  > **Custom** is a JPEG or PNG from the `Bootlogo` folder in the root of the card, picked from the list under **Custom picture** on the same page. Any size will do: it is scaled to fill the panel and cropped where it overflows, so a landscape photograph boots as a full-screen picture rather than a band between two black bars. The chosen picture is copied into the player, which is why it still boots with the card out — and why a change to the file on the card means choosing it again.
- **Appearance → Font** - the face the whole interface is drawn with: MiSans (the default), Neon 80s and, where the image carries them, Roboto or Roboto Mono. Text can also be sized Normal or Large. Any `.ttf` or `.otf` file put in the `Fonts` folder on the card is offered beside those, so people can install their own; a face without Cyrillic, Hangul, kanji or Greek still draws the whole interface, because MiSans stays behind it for the letters it does not have.
- **Screen** - brightness (5% to 100%), screen-off timeout, double-tap to wake, screensaver,
  and 180° rotation with the touch rotated with it.
- **Screensaver** - choose to show the cover of the current playing track or pictures from the `Screensaver` folder. On the V1's Tokyo Night, Amber on Black and OLED themes the artwork fills the whole panel and the title, artist and clock are read on a translucent field of the palette's own colour over its foot; Dark and Light keep the blurred strip. Under **Album art**, **Fill** keeps that full-bleed look (the default). **Framed** shows the whole cover instead, at its own resolution, in a rounded frame with margins round it, above the title, artist and clock. That frame is 160 px on the V1. The cover is decoded for exactly that size and never enlarged, so it never turns into blocks.
  > Pictures must be baseline (not progressive) JPEGs, portrait, and no larger than 480x800. The V1 crops and scales them to fill its 240x320 screen. For the sharpest result with nothing cropped, make them 240x320 or 480x640, the screen's own 3:4 shape.

- **Power** - standby, LED behaviour, automatic power-off from thirty seconds to
  six hours, and a charge limit from 80 to 100 %.
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
  Polski, Português BR, Українська. The nine language files are read from the
  resource folder, so dropping another `.ini` in beside them adds a language.
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
- **Developer options** - unlocked by tapping the build number: ADB, log to the
  card, database log, disabling the software volume attenuation, and a page of
  running processes and memory.
