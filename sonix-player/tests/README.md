# Subsonic / stream-bridge integration tests

Run from the repository root on a Linux host:

```sh
make -C sonix-player/tests test
make -C sonix-player/tests sanitize
make -C sonix-player/tests swvolume
make -C sonix-player/tests calculator
make -C sonix-player/tests notes
```

These require only a C compiler, Make and Python 3 (plus standard pthread/dl/math libraries). They **do not require LVGL, ALSA, a MIPS toolchain, a music server, a real account, or downloaded audio**.

`run_subsonic_tests.py` starts a temporary threaded localhost HTTP server plus three HTTPS servers carrying the certificates in `tls_fixtures.py`, runs the C binary with a private temporary config directory, verifies its requests, then shuts down. All account values are synthetic. Build products live in ignored `.build/`; response/PCM fixtures are generated in memory rather than added to Git, and the three test certificates are pasted into that Python file rather than downloaded.

## Coverage

The C binary links the **real** HTTP/TLS helpers, REST client, MD5, JSON, configuration, playlist, RAM producer/reader and two-slot manager. `support.c` replaces only unrelated platform scheduling/signal helpers, translations and local-library indexes; it does not replace networking, buffers or decoding.

Checks include:

- correct salted MD5 authentication with encoded UTF-8/special characters, fresh salts, and no password/token query in diagnostic output;
- TLS against real self-signed certificates: `strict` (the default) refuses one, `relaxed` accepts it, a certificate naming another server and an expired one are refused either way, a connection opened under the old mode survives the setting changing under it, and the mode round-trips through `device_config.ini`;
- server URL/port rejection, exact opaque/numeric IDs (including >64-bit IDs), bounded JSON/token parsing, and malformed/oversized/auth-failed responses;
- music folders → indexes → directory and artists → artist → album, and 200-entry pagination;
- optional atomic 0600 credential persistence/round-trip, clearing remembered credentials, reporting a failed Forget instead of false success, rejecting symlinks/truncated identities/embedded NUL;
- fixed RAM capacity, millions of bytes across ring wrap, 50% refill hysteresis, protected initial header read/rewind vs a racing producer, retained-header rewind vs overwritten history, and 512 KiB pre-cache promotion;
- chunk extensions/trailers, transfer framing never reaching the reader, truncation/empty bodies/error pages, and retry after timeouts inside a chunk size, CRLF and trailer;
- cancellation during HTTP header setup, a stalled body, and reader starvation, followed by generation-safe reuse;
- nonmutating playlist next-track look-ahead across repeat/shuffle/ordered modes;
- exclusive decoder leases, both handoff/teardown orders, failed pre-cache retry suppression, repeat-one starting at byte zero, account-change invalidation with an outstanding lease, metadata lookup and unsupported raw codecs.

## Software volume

`make -C sonix-player/tests swvolume` checks the arithmetic of the software
volume -- the whole level law for a USB DAC with no control of its own. The
gain must mean exactly the decibels the USB route asked for, mute must be
exact, a deeply attenuated stream must stay quiet, clean and in range (rounded
and dithered, not truncated into grit), and the scaling must apply only while
the sound is leaving over the USB-C port. The three routing answers (Bluetooth,
a card on the port, that card's volume control) are stubbed; the curve, the
coefficients and the scaling are the real ones.

## Real codec integration

Install the normal host development libraries (Debian/Ubuntu example):

```sh
sudo apt-get install build-essential python3 pkg-config libopusfile-dev libwavpack-dev
make -C sonix-player/tests codecs
make -C sonix-player/tests sanitize-codecs
```

The codec suite links the actual player decoder dispatcher and bundled `dr_mp3`/`dr_flac`, plus **real libopusfile/libopus/libogg**. Other player codecs are also compiled/linked normally, not replaced with fake decoders. `PKG_CONFIG`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS` and `BUILD` can be overridden for a custom dependency installation.

`media_fixtures.py` creates deterministic silent MP3 frames, lossless FLAC verbatim frames with real CRCs, and Ogg Opus silence packets with valid page CRCs and preskip/end trimming. No ffmpeg/encoder command or binary media fixture is needed. Tests check exact PCM frame counts (including MP3 Xing/LAME delay/padding and Opus preskip/end trim), buffered-source identity, seek/source-span exclusions, codec mismatch rejection and canceled-source opens. Large ID3/FLAC headers and >1 MiB FLAC transfers exercise forward header skipping and ring wrap without a file cache.

A no-ID3 MP3 fixture is intentional: the bundled `dr_mp3` discards its first ten bytes when given a NULL seek callback, losing the first audio frame. The production bridge supplies **limited retained-header seeks** and refuses an unknown-length whole-stream count; this test detects both regressions.

Both sanitizer targets use AddressSanitizer and UndefinedBehaviorSanitizer. At `-O1`, GCC reports pre-existing intentionally bounded/truncating MP4 tag copies; `sanitize-codecs` leaves that warning category nonfatal, while other warnings remain fatal. The core suite keeps all warnings fatal.

## Limits

The fixture proves transport, byte/sample counts and ownership behavior on the host. Scheduling helpers are stubs and there is **no ALSA/device loop** here, so these tests do not certify real-time scheduling, gapless analog output, Wi-Fi performance, target CPU use or battery life. The TLS cases run against the OpenSSL the host has (the player's own client code, `dlopen`ed exactly as on the device, sees the host's libssl 1.1/3.x); with no loadable OpenSSL they print `SKIP TLS` and the rest of the suite still runs. Real-network trust, redirects and certificate cancellation, plus UI interaction, should also be checked on hardware. See [the V1 checklist](../../docs/subsonic.md#validation).

## Headless main-menu appearance tests

```sh
# Only needed if the player build has not already fetched LVGL:
git clone --depth 1 -b v9.6.0 https://github.com/lvgl/lvgl.git sonix-player/lvgl
make -C sonix-player/tests ui -j"$(nproc)"
```

This optional suite links the real LVGL layout engine, main menu, shared grid,
Appearance controls, themes, language loader and configuration store. It uses
`tests/ui/lv_conf.h` to disable SDL, FreeType and device drivers, so a compiler
and the LVGL checkout are sufficient; no audio libraries or display are needed.
Deterministic built-in font metrics and unrelated navigation/playback services
are substituted, rather than running the player or touching device hardware.

It checks the default and saved **Main menu icons only** setting on both panel
sizes, all five themes, exact icon sizes and centering, glow-free icon-only
tiles, unchanged secondary-menu tiles, restoration of the labeled layout and
its decorative glows, real switch callbacks and config save/reload,
theme/accent/tint/font changes, hidden-label translation, and the six tile
destinations. Both language asset trees are used. Icon-only images must have
native-size sources and no scaling transform, including after font-metric
reflows. Actual LVGL snapshots verify pixel-for-pixel alpha edges, theme tints
and opaque pixels in ARGB8888 and the panel's RGB565 format; geometry alone
cannot catch a small bitmap being enlarged into a blurry icon.

To inspect rendered screens, optionally set `SONIX_MENU_SNAPSHOTS` to an output
prefix (its parent directory must exist), for example:

```sh
SONIX_MENU_SNAPSHOTS=.build/menu make -C sonix-player/tests ui
```

This writes ignored `.build/menu-<width>-theme<kind>.ppm` files using LVGL's
software renderer. To regenerate the main-menu-only native art, see
`tools/main_menu_icons.py`; Python image tools are not required to build or
run the player/tests. The host-build workflow runs this suite as well. These
checks do not replace visual and touch testing on a physical V1.

## Headless screensaver artwork tests

The same `ui` target also builds `test_screensaver`, which links the real
screensaver, scrolling labels, themes, settings card helpers, language loader
and configuration store. The player that lends the artwork, the cover loader
and the device services are substituted. The test drives the loader by hand,
so it decides when a decode lands, what size it is, or that it found nothing.

It checks the **Album art: Fill / Framed** choice on both panel sizes, default
and saved:

- Fill is unchanged: the sleeve still covers its whole area, and it never
  asks for a decode. On the V1 that always means enlarging it.
- Framed asks for one copy of the artwork at exactly the frame's size (160 px
  on the V1, 400 px on the 480-wide panels), from the path the player read
  the artwork from. It asks as the panel goes dark, once the player has the
  artwork, and never again after a theme change or a failed decode.
- Until that copy lands, the player's sleeve stands in, never enlarged; a
  non-square Immersive crop is not used as a stand-in.
- The frame is centred in the area above the strip, with the margins, the
  player's rounded corners and no strip backdrop.
- The page behind the frame is right in all five themes.
- Small pictures are shown at their own size.
- Track changes, abandoned decodes, switching back to Fill, switching to
  Pictures, dismissing and switching off release every borrowed and owned
  buffer.

Rendered RGB565 snapshots verify that every pixel of the frame outside its
rounded corners is the decoded picture's own pixel: drawn one to one, not
shifted, not resampled. The test pictures are one-pixel checkerboards, so any
scaling fails. The three new texts are checked in every language file, and
the captioned pill row is checked to relabel on a language switch.

The same test checks the size limit the screensaver hands the Screensaver
folder. It reads the size the empty-folder note promises from every language
file, requires them all to agree, and requires the limit to match: 480x800 on
the V1, which ships the R1's texts, and the panel's own size elsewhere.

Set `SONIX_SCREENSAVER_SNAPSHOTS` to an output prefix to keep the rendered
screens as ignored `.ppm` files, for example:

```sh
SONIX_SCREENSAVER_SNAPSHOTS=.build/saver make -C sonix-player/tests ui
```

## Calculator arithmetic and the notes folder

`make -C sonix-player/tests calculator` and `make -C sonix-player/tests notes`
build and run the two new pages' plain-C halves: no LVGL, no screen, the same
files the firmware compiles.

The harness supplies two font metrics, a small one and a larger one, so the
page can be checked to draw the calculator's symbols a step up instead of
merely differently styled.

`test_calculator` drives the model through the twenty keys: the display
following every key, a digit after a result starting a new sum, chained
operators applied as they are pressed, the sign on a result and on a number
being typed, percent, backspace to zero, ten digits and no more, the decimal
point entered once, division by zero reported and cleared, and a number too
wide for the display rounded down through the fallback formats instead of
being cut off.

`test_notes` covers the folder on a temporary card: what counts as a note
(including the folder wearing a note's name and the `.tmp` left by a write
that was interrupted), the A-Z, case-insensitive, digit-aware order, the name
a note is written under, names that must not reach the filesystem, the next
free name for a new note, what the editor will hold -- counted in characters
rather than bytes, so a note of three-byte letters is measured by the letters
-- and reading the files a computer leaves behind: UTF-8 with a byte order
mark, UTF-16, CRLF, tabs, Windows-1252 accents, control characters, an empty
file, and a file too long for the editor.

## Headless calculator and notes page tests

The `ui` target also builds `test_tools_pages`, which lays the two new pages
out on the real renderer at both panel sizes, with the Game Boy core compiled
in as the firmware does, so the More menu is laid out with the seven entries
the device has.

It checks that both destinations are on the More page in every theme -- the
tiles of Dark and Light, and the V1's single-column list under the palettes
built on function glyphs -- that nothing on either page is drawn outside the
panel, that every key is at least the size of the shell's own corner buttons
and every notes row a finger target, that the calculator's symbol keys are
drawn in the larger, bold font while its digits and the decimal point are not
and that every caption stays inside the key it is drawn on, that the calculator's display follows the
keys actually tapped and comes back holding the sum it was left with, that the
error state is drawn in the language's own words, that a note opens with its
file's text and its name as the title, that opening one is reading it -- no
keyboard, no caret, a tap on the text that changes nothing, and nothing
written to the card -- that the corner pencil is what turns the page into an
editor and the keyboard comes up with it, that leaving the editor with writing
unsaved leaves the writing on the card, that a note at the editor's ceiling
opens in the editor in a fraction of a second and whole, that a note past it
is shown by the text viewer instead, and that a new note is written and listed
before a single character is typed into it. Every language file is also
required to carry the two pages' strings: `tr()` falls back to the key it was
given, so a missing translation shows the tag itself rather than failing.

To inspect the pages, set `SONIX_PAGE_SNAPSHOTS` to an output prefix:

```sh
SONIX_PAGE_SNAPSHOTS=.build/page make -C sonix-player/tests ui
```

This writes ignored `.build/page-<board>-<page>-<theme>.ppm` files: one per
page, per More-page shape, per theme that draws one and per panel size.

## Screensaver folder rules

`make -C sonix-player/tests pictures` builds and runs `test_screensaverpics`,
plain C with no LVGL. It links the real folder code (`screensaverpics.c`) and
JPEG header reader (`jpeginfo.c`), and checks them on synthetic JPEGs in a
temporary card. Only headers matter here, so each file has a JPEG's segment
structure, with filler where the picture data would be.

- **Headers:** the size and coding are read even when the frame header sits
  behind about 93 KB of an editor's metadata (Exif with a thumbnail, a colour
  profile, XMP, Photoshop resources). The metadata filler contains frame
  markers of its own, as a real Exif thumbnail does, so a reader that looked
  inside a segment would be caught. Fill bytes, truncated files, scans with no
  frame, non-JPEGs and both extensions in either case are covered too.
- **Folder rules:** pictures over the limit, landscape or progressive are
  turned away, and so are hidden files (including a Mac's `._` companions),
  other names and missing files. Pictures made to the V1's note (480x800) are
  found with the V1's limit and not with its panel size, which is the fault
  this guards against.
- **Random pick:** it only ever returns a usable picture and reaches all of
  them. It avoids the picture already up while there is another, and falls
  back to it when it is the only one. Twenty `._` files beside one picture
  never make it miss.

## Spotify

`make -C sonix-player/tests spotify` builds and runs `test_spotify` -- the
receiver's three interfaces as the player's code sees them, with a shell
stand-in receiver and a stub audio layer, no Spotify account and no network.

- **Protocol:** the Q2SPOT/1 client's escaping, oversized argument refusal, the
  8 KiB request ceiling, the 64 KiB reply ceiling, the "closed without a
  trailing newline" last row, mismatched request ids, field counts, the 50-row
  limit, connect failure and the deadline.
- **Status file:** every field of the `key=value` file, `at`-based position
  advancing, `cover=0/1`, truncation and a half-written file.
- **Control socket:** each of the five datagrams, and a socket that is not
  there.
- **Service:** start/stop, the ownership latch and the DAC hand-back, a child
  that dies on its own being reported with its log tail, the idle stop needing
  a page to be shut and the receiver to have played, and the shutdown paths
  waiting for the process to leave. The receiver's process is checked from
  inside itself -- the stand-in records the `oom_score_adj` and the niceness the
  spawn gave it, so a change that stops running it below the interface, or that
  lets it inherit the player's own protection from the OOM killer, fails here --
  and the log it writes into the tmpfs is checked to be trimmed back to its own
  tail.
- **Cost of waiting:** with the service up and the stand-in sending no audio,
  this process's own CPU over a set window is measured against a ceiling. The
  sink's playback thread asks to be real time, so a timed wait whose deadline is
  on the wrong clock is not a warm device but a frozen one: the wait returns at
  once, the thread spins, and the kernel throttles the whole real-time class --
  which is what froze a V1 at the switch. `core/utils.c` is linked here for real
  rather than stubbed for the same reason: the clock `deadline_in_ms()` computes
  on is the thing being held.
- **Sink:** the ring's wrap, frame alignment, largest-popped-oldest on a stall,
  the widen-to-32 retry, and the three ways the output is released -- including
  a release asked for while the ring is full, which has to be answered by the
  same pass rather than by the one that finds nothing to play.

The page is covered separately by the `ui` target's `test_spotify_page`, which
links the real page, rows, scrolling labels, themes and language loader with
`spotify.h`, the shell and the keyboard stubbed. It records every op and
argument the page sends, so the checks are about the protocol the page speaks:
the switch refusing with no receiver or no Wi-Fi, the canned status reaching
the title/artist/album lines, each browser row's op, a track row's
`play <context> <position> <uri>`, a queue row's `skip`, the cursor as the last
argument of a next-page request, back walking up the lists one page at a time
before leaving the page, the settings page's pills, sign-out's confirmation,
and a search's category and term. Both language asset trees and both panel
sizes are used.

Neither suite is hardware certification: there is no real receiver, no ALSA
device and no Wi-Fi in either one. See
[the V1 checklist](../../docs/spotify.md#validation).
