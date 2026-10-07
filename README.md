# Sonix Player – TempoTec Variations V1 Release

A modern, high-performance, open-source replacement firmware and player for the **TempoTec Variations V1** (and V1-A / Variations family), built on **LVGL 9.6**.

This release adapts Sonix Player to the ultra-compact form factor of the TempoTec V1, replacing the legacy stock player application while preserving the official Linux kernel, device drivers, and recovery subsystem intact.

---

## Overview

The TempoTec Variations V1 is an ultra-compact digital audio player (DAP) powered by an Ingenic MIPS SoC and high-grade dual Cirrus Logic DACs. Sonix Player completely replaces the stock user interface with a fast, modern UI engineered specifically for the V1's 240×320 screen geometry.

```
       ┌──────────────────────┐
       │ 10:42        100%  🔋 │  <-- 24 px compact status bar
       ├──────────────────────┤
       │                      │
       │   [ 240×192 Art ]    │  <-- Edge-to-edge album artwork
       │                      │
       ├──────────────────────┤
       │ Track Title          │
       │ Artist Name          │
       │ 01:24 ━━━━━●── 03:45 │  <-- 128 px compact control deck
       │     ⏮   ⏯   ⏭       │
       └──────────────────────┘
```

### Hardware Specifications

| Component | Specification |
|---|---|
| **SoC** | Ingenic X1600 (MIPS32r2 core, o32 ABI, glibc 2.22 compatibility) |
| **Display** | 2.0-inch QVGA TFT panel, **240×320 pixels** portrait |
| **DAC** | Dual Cirrus Logic CS43131 |
| **Audio Outputs** | 3.5 mm single-ended + 4.4 mm balanced headphone jacks |
| **Storage** | One MicroSD card slot (exFAT / FAT32) |
| **Controls** | Side flank buttons (Power, Vol+, Vol-, Play/Pause, Next/Prev) + capacitive touchscreen |
| **Wireless** | 2.4 GHz Wi-Fi (802.11 b/g/n) and bi-directional Bluetooth |
| **USB** | USB Type-C (bidirectional USB DAC & USB audio out) |
| **Firmware Package** | `v1.upt` with accompanying `v1_md5.txt` |

---

## Tailored 240×320 Interface Profile

Rather than blindly scaling down an interface designed for larger screens, Sonix Player features an interface profile crafted specifically for the 240×320 panel:

* **Compact Status Bar & Layout Geometry:** Uses a 24 px status bar, 6 px margins, and 36 px header action targets.
* **Overflow Header Menus:** The main Music screen combines title actions with a dedicated overflow menu (`...`) so long titles are never truncated to `Musi…`.
* **Full-Width Now Playing Screen:** Displays 240 px edge-to-edge cover artwork centre-cropped to 192 px height, paired with a dedicated 128 px control area to eliminate black side bars.
* **Adaptive A–Z Index Strip:** Automatically scales down to ~14 visible letter slots to fit the 320 px vertical height. Pressing anywhere along the strip proportionally navigates the full alphabet with an 88×84 touch preview card.
* **Scrollable 10-Band EQ:** The graphic equaliser card scrolls smoothly horizontally instead of cramming ten sliders into 216 px.
* **Compact Quick Settings:** Retains eight quick toggles arranged in an ergonomic 2×4 sheet.
* **Pixel-Tuned Dialogs & Keyboard:** Bespoke layouts for Date/Time picker rollers, MSEB tuning, PEQ curves, and a 144 px compact on-screen keyboard tray.
* **Button Remapping:** Configured as a clean, single-column scrollable list with direct tap-to-assign actions.

---

## Experimental Status & Safety

The TempoTec V1 port is **experimental**:

* **Stock Kernel Preserved:** The packaging script extracts the official `v1.upt`, leaves the stock Linux kernel (`xImage`) byte-for-byte identical (verified via SHA-256), preserves official hardware driver modules, and installs only the Sonix runtime and model-independent resources.
* **Automated Hash Chain Verification:** Every build validates the ISO image structure, squashfs integrity, chunked md5 hash chain, glibc 2.22 ABI compliance, and system identification metadata.
* **Recovery Safety:** The recovery updater remains unchanged. Always keep an official stock `v1.upt` on a spare MicroSD card in case you want to revert.

---

## Flashing Instructions

1. **Format MicroSD Card:** Use a FAT32 or exFAT formatted MicroSD card.
2. **Copy Firmware Files:** Copy both `v1.upt` and `v1_md5.txt` directly to the **root** of the MicroSD card (slot 1).
3. **Insert & Charge:** Insert the card into your TempoTec V1. Ensure battery level is **above 30%**.
4. **Initiate Update:**
   * **From Stock Firmware:** Go to `System Settings` > `Firmware Update` (or `Update from SD card`).
   * **From Sonix Player:** Go to `Settings` > `System` > `Update firmware` > `From SD card`.
5. **Wait for Completion:** The screen will display *Upgrading...* followed by *Succeeded*, then automatically reboot into Sonix Player.

> **Restoring Stock Firmware:** If you ever wish to revert to stock firmware, copy the official TempoTec `v1.upt` and its `v1_md5.txt` to the root of your MicroSD card and repeat the update procedure.

---

## Streaming Services and Their Keys

Qobuz, Tidal, Podcast Index and Last.fm all refuse to answer a client that
cannot identify itself, and the credentials that identify one are **not in this
repository**. A firmware built from this source — and every `v1.upt` published
from it — therefore starts with those services switched off, and their pages say
so instead of failing halfway through a login:

> *Streaming keys not found. Copy streaming-keys.ini to the microSD card and restart.*

That message is not a fault and it is not specific to development builds. It
means the one file below is missing, and it is fixed with that one file.

### The file

`streaming-keys.ini` is a plain INI, one section per service. Fill in only the
services you have credentials for; the others stay off and say so.

```ini
[qobuz]
app_id = 
app_secret = 

[tidal]
client_id = 
client_secret = 

[podcast]
api_key = 
api_secret = 

[lastfm]
api_key = 
api_secret = 
```

Where the values come from:

| Service | Where the pair comes from |
|---|---|
| **Podcast Index** | Free, requested on [podcastindex.org](https://podcastindex.org) — this one identifies *this player*, and it is yours to ask for. |
| **Last.fm** | Free, an API account on [last.fm/api](https://www.last.fm/api). |
| **Qobuz** / **Tidal** | No public sign-up: both are issued to hardware partners. The pair the device already uses sits inside `usr/bin/hiby_player` in the stock firmware (`app_id` as a digit string, `app_secret` as 32 hex characters; Tidal's client secret is assembled at run time, so it never appears as one string). They are the manufacturer's keys, not this project's, which is exactly why the source does not carry them — publishing them would publish someone else's contract. Extracting them from a device you own and using them on that device is your decision to make. |

### Three ways to install it

**1. On the microSD card** — no rebuild, no reflash, and the way to go for a
firmware you downloaded. Copy `streaming-keys.ini` to the **root** of the card
and restart the player. It is read once the card is mounted, before the first
screen is drawn, and the log says which services came on.

> A firmware built **before** the card was searched looks in the image only. On
> one of those, use method 3, or reflash with a newer build.

**2. Sealed into an image you build yourself** — what a published firmware
should carry, so that nobody who unpacks the image can read the keys:

```sh
cd sonix-player
cp streaming-keys.ini.example streaming-keys.ini   # then fill it in
python3 tools/seal_streamkeys.py seal              # writes streaming-keys.bin into every assets tree
```

`streaming-keys.key` is made on the first run and is what the player is
subsequently built with; the packer warns if a tree would ship the plain `.ini`
and drops it. See the header of `tools/seal_streamkeys.py` for the format and
for what the sealing does and does not protect against.

**3. Anywhere else** — over ADB, for a device whose card you would rather not
touch. `Settings` > `Developer options` > `ADB` turns the daemon on; that row
appears once the build number on `Settings` > `System` has been tapped five
times.

```sh
adb push streaming-keys.ini /usr/data/streaming-keys.ini
adb shell "printf '[streaming]\nkeys_file = /usr/data/streaming-keys.ini\n' >> /usr/data/device_config.ini"
adb shell reboot
```

`keys_file` wins over both other places, which is also the quick path the day a
service rotates its credentials.

### Checking what happened

The player logs one line either way, and never a value from the file:

```
streamkeys: from /mnt/sd_0/streaming-keys.ini -- Qobuz yes, Tidal no, Podcast no, Last.fm no
```

or, when nothing was found:

```
streamkeys: no /usr/resource/sonix/components/streaming-keys.bin, Tidal, Qobuz, podcasts and Last.fm stay off
```

With the log switched on (`Settings` > `Developer options` > `Log to microSD`)
that line is in `sonix_player.log` at the card's root. A `.bin` sealed with a
different key is reported as such and ignored rather than guessed at.

### Keyboard Shortcuts (Simulating Hardware Buttons)

| Key | TempoTec V1 Action | Description |
|---|---|---|
| `p` | Power button | Tap to toggle screen, hold to open power menu |
| `u` | Volume Up | Increases audio volume |
| `i` | Volume Down | Decreases audio volume |
| `n` | Play / Pause | Toggles track playback |
| `b` | Previous Track | Skips to previous track |
| `m` | Next Track | Skips to next track |

---

## Features

### Audio Format Support

Plays virtually all lossless and lossy audio formats up to 384 kHz / 32-bit and native DSD:

* **Lossless / Hi-Res:** `.flac`, `.wav`, `.aif`, `.aiff`, `.aifc`, `.caf`, `.wv` (WavPack), `.wvc`, `.ape` (Monkey's Audio), `.alac`
* **Lossy Formats:** `.mp3`, `.ogg`, `.opus`, `.m4a`, `.m4b`, `.mp4`, `.aac`
* **DSD Audio:** `.dsf` and `.dff` files via DoP (DSD over PCM) or internal high-quality 176.4 kHz conversion
* **CUE Sheets:** Full `.cue` sheet parsing splitting single-file album rips into individual tracks

### Audio Engine & DSP

* **Graphic EQ:** 10-band equaliser (31 Hz – 16 kHz, ±12 dB) with presets and horizontal scrolling card.
* **Parametric EQ (PEQ):** 10 fully parametric bands (20 Hz – 20 kHz, gain ±15 dB, Q 0.10–10.00, Peak/Shelf/Low-Pass/High-Pass) with real-time response curve rendering and Equalizer APO / AutoEq import.
* **MSEB:** MageSound Eighteen sound tuning algorithm matched to hardware characteristics.
* **Soundfield & Crossfeed:** Adjustable spatial width from mono to 200% stereo, plus headphone crossfeed.
* **Channel Balance:** ±20 dB balance control in 0.5 dB increments.
* **Playback Controls:** Bit-perfect gapless playback, ReplayGain (track & album), adjustable track fade (1–12 s).
* **Hardware Output Routing:** Independent volume level memory for 3.5 mm, 4.4 mm, and USB outputs; Line Out mode; USB DAC mode (32–384 kHz).

### Library & Organization

* Ultra-fast SQLite-backed media indexer and tag reader (supporting UTF-8 and multilingual metadata).
* Embedded and folder album art extraction with smooth coverflow browsing.
* Embedded and external `.lrc` synchronized lyrics display.
* Full audiobook resume support with chapter markers (`.m4b` and ID3 chapter frames).
* File browser with direct folder playback, favorites, and custom playlists.

### Wireless & Streaming

* **Wi-Fi Web Transfer:** Built-in HTTP server allows uploading, downloading, and managing music on the player from any web browser.
* **Streaming Services:** Native client support for Tidal, Qobuz, Podcast Index, and internet radio.
* **Remote Streaming:** DLNA digital media renderer and Apple AirPlay audio receiver.
* **Last.fm Scrobbler:** Caches scrobbles offline and submits them when Wi-Fi connects.
* **Bluetooth Audio:** High-resolution wireless playback supporting SBC, AAC, aptX, and LDAC.

### Built-in Applications

* **Gearboy (Game Boy / Game Boy Color):** Integrated full-speed Game Boy emulator with on-screen button controls and save state support.
* **EPUB E-Book Reader:** Built-in reader supporting EPUB reflowable text, customizable font sizing, margins, and night mode themes.

---

## Credits & Acknowledgements

* **Sonix Player Team & Contributors:** [@Tartarus6](https://github.com/Tartarus6), [@noisetta](https://github.com/noisetta), [@endgame47](https://github.com/endgame47), [@hkhrithik007](https://github.com/hkhrithik007)
* **LVGL:** Lightweight and Versatile Graphics Library
* **Rockbox Project:** Cross-compiler toolchain foundation
* **Gearboy:** Game Boy emulator core by Ignacio Sanchez
