# Sonix Player for TempoTec Variations V1

This repository is a community port of the original **[Sonix Player](https://github.com/Jepl4r/sonix-player)** project by **[Jepl4r](https://github.com/Jepl4r)** — a replacement music player for the HiBy R3 Pro II and HiBy R1 — to the **TempoTec Variations V1**. The player and the firmware packer come from that project; this fork adds the V1 board profile, the V1 packaging path and the V1 documentation. See [Credits](#credits).

The firmware builder starts with an official V1 update package, replaces the stock music player and its interface with Sonix, then repackages the update as `v1.upt`.

This is not an official TempoTec or HiBy release. **The port has been tested on physical TempoTec Variations V1 hardware and works perfectly on the tested device.** It targets the **TempoTec Variations V1 only**; compatibility with the V1-A or other players in the Variations family is not established.


## Contents

- [Target device](#target-device)
- [Features](#features)
- [Screenshots](#screenshots)
- [Build a firmware image](#build-a-firmware-image)
- [Install or restore firmware](#install-or-restore-firmware)
- [Streaming credentials](#streaming-credentials)
- [Build from source](#build-from-source)
- [Project files and documentation](#project-files-and-documentation)
- [Credits](#credits)

## Target device

The **TempoTec Variations V1**:

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

The player UI is built with LVGL 9.6 and adapted for the V1's small display. The package is based on vendor firmware; it is **not** a Linux distribution or a kernel replacement.

## Features

The project includes a local music library and file browser, album artwork, playlists, lyrics, audiobooks, DSP controls, an EPUB reader, and a Gearboy Game Boy / Game Boy Color emulator. The player also contains code for streaming and network features such as Bluetooth audio, AirPlay, DLNA and Wi-Fi file transfer.

Local audio decoder inputs include WAV, FLAC, MP3, Ogg Vorbis, Opus, M4A/M4B/MP4, AAC, ALAC, WavPack, APE, AIFF/AIFC, CAF and DSD (DSF/DFF) files. DSP features include a 10-band graphic equalizer, parametric EQ, MSEB, crossfeed, channel balance and ReplayGain.

The interface font is a setting (Settings → Appearance → Font): MiSans, Neon 80s, Roboto Mono, Roboto Condensed, Inter and Barlow Semi Condensed, plus any `.ttf`/`.otf` file put in the `Fonts` folder on the card. Whatever a face is missing — Cyrillic, Greek, Hangul, kanji, Arabic — is drawn by MiSans behind it, so the whole interface stays legible in every language. The four added families are under the SIL Open Font License; see [`sonix-player/assets/fonts/FONT-LICENSES.txt`](sonix-player/assets/fonts/FONT-LICENSES.txt) for the copyright notices and the exact files.

The boot screen is a setting too (Settings → Appearance → Boot screen): **Retrospace**, the default, **Space**, **Travelling in space**, or **Stock**, the firmware's own light and dark pictures. The choice cannot live in the config — the script that draws the logo runs before any filesystem holding a config is mounted — so it is written to the same flash marker the theme uses, and appears from the next power-on.

See [FEATURES.md](FEATURES.md) for a longer feature list. Streaming services also require valid credentials and may depend on third-party service availability.

## Screenshots

Interface previews, rendered at the V1's native 240 × 320 resolution. Click any picture to open it full size. (This is not the default font neither the color)

<table>
  <tr>
    <td align="center">
      <a href="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Main%20menu.png"><img src="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Main%20menu.png?raw=true" width="180" alt="Main menu"></a>
      <br><sub><b>Main menu</b></sub>
    </td>
    <td align="center">
      <a href="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Music.png"><img src="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Music.png?raw=true" width="180" alt="Music"></a>
      <br><sub><b>Music</b></sub>
    </td>
    <td align="center">
      <a href="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/More.png"><img src="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/More.png?raw=true" width="180" alt="More"></a>
      <br><sub><b>More</b></sub>
    </td>
    <td align="center">
      <a href="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Wireless.png"><img src="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Wireless.png?raw=true" width="180" alt="Wireless"></a>
      <br><sub><b>Wireless</b></sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Settings.png"><img src="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Settings.png?raw=true" width="180" alt="Settings"></a>
      <br><sub><b>Settings</b></sub>
    </td>
    <td align="center">
      <a href="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Now%20Playing.png"><img src="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Now%20Playing.png?raw=true" width="180" alt="Now Playing"></a>
      <br><sub><b>Now Playing</b></sub>
    </td>
    <td align="center">
      <a href="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/File%20Browser.png"><img src="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/File%20Browser.png?raw=true" width="180" alt="File Browser"></a>
      <br><sub><b>File Browser</b></sub>
    </td>
    <td align="center">
      <a href="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Steaming.png"><img src="https://github.com/mr-f0xx/Winamp360-AP80-Pro-Max/blob/main/Steaming.png?raw=true" width="180" alt="Streaming"></a>
      <br><sub><b>Streaming</b></sub>
    </td>
  </tr>
</table>

## Build a firmware image

The recommended route is the repository's GitHub Actions workflow. It cross-compiles the MIPS player and launcher, packages them into an update based on official V1 firmware, validates the result and uploads a downloadable artifact.

1. Open the repository's **Actions** tab and select **Build TempoTec V1 firmware**.
2. Choose **Run workflow** on the branch you want to build. The stock firmware URL is prefilled with the [official TempoTec V1 firmware folder](https://drive.google.com/drive/folders/1jbT9lhRvJ8sbJuepnaBpss861mpsYwnv?usp=sharing); change it only if you have another official V1 package to use.
3. Confirm the required checkbox acknowledging that this is an unofficial community build and that you will keep the matching official firmware and checksum available for recovery.
4. When the run succeeds, download the `sonix-tempotec-v1-<commit>` artifact. It contains `v1.upt`, `v1_md5.txt`, the player binaries, and stock/output validation reports.

Artifacts are retained for 14 days. The port has been tested on physical V1 hardware, but Actions validates each generated package automatically rather than installing that specific image on a player. See [`.github/workflows/build-tempotec-v1.yml`](.github/workflows/build-tempotec-v1.yml) for the build and validation steps.

## Install or restore firmware

Only attempt this on a TempoTec Variations V1. Read the warning above before proceeding.

1. Keep the **official V1 `v1.upt` and its matching `v1_md5.txt`** somewhere safe. They are needed if you want to return to stock.
2. Extract the workflow artifact and copy **both** `v1.upt` and `v1_md5.txt` to the root of a microSD card supported by the device.
3. Charge the player adequately, insert the card and start the device's built-in firmware update process. Menu names vary by stock firmware version; follow the device's on-screen instructions and do not interrupt the update.
4. If you want to restore stock firmware, put the original vendor `v1.upt` and its matching checksum file at the card root and use the same update process.

The package builder is designed to preserve the stock `xImage` kernel and leave the existing update/recovery path in place. This is checked during the build, but it is **not** a guarantee that recovery will work on every device or in every failure case.

## Streaming credentials

Credentials for Qobuz, Tidal, Podcast Index and Last.fm are not included in this repository or in the standard workflow artifact. Without usable credentials, those services remain disabled; local playback does not require them.

To try credentials on a build that has no keys embedded, copy `sonix-player/streaming-keys.ini.example` to the root of the microSD card as `streaming-keys.ini`, fill in only the services for which you have authorized credentials, then restart the player. Do not commit or share this file.

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

For a private development build, `sonix-player/tools/seal_streamkeys.py` can place a sealed credentials file in the assets before compilation. This format is **obfuscation, not secure encryption**: the key is compiled into the player binary and can be recovered by someone who analyzes it. Never publish a binary containing credentials you do not intend to disclose.

## Build from source

The GitHub Actions workflow is the easiest option because it also obtains the official stock image and assembles the final package. For a local Debian/Ubuntu build, install the toolchain and firmware utilities first:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential git pkg-config wget curl ca-certificates \
  texinfo bison flex gawk gperf patch xz-utils bzip2 gzip \
  autoconf automake libtool libtool-bin \
  libgmp-dev libmpfr-dev libmpc-dev \
  p7zip-full squashfs-tools genisoimage python3
```

Then build the V1 target binaries. On a first build, Make downloads LVGL and builds the MIPS cross-toolchain and target libraries; this can take a while and requires network access.

```sh
make -C sonix-player target -j"$(nproc)"
```

To package them, download the official V1 `v1.upt` (or the folder/archive containing it), then run:

```sh
chmod +x sonix-packer/*.sh
sonix-packer/prepare_v1_stock.sh /path/to/official-v1-firmware sonix-packer
install -m 0755 sonix-player/sonix_player sonix-packer/sonix_player
install -m 0755 sonix-player/sonix_launch sonix-packer/sonix_launch
sonix-packer/sonix_firmware_packer.sh
```

The generated files are `sonix-packer/v1.upt` and `sonix-packer/v1_md5.txt`. To validate the generated package and confirm that its kernel hash matches the stock package:

```sh
set -euo pipefail
stock_kernel_sha256="$(sonix-packer/verify_upt.sh sonix-packer/v1_original.upt \
  | sed -n 's/^Kernel SHA-256: //p')"
sonix-packer/verify_upt.sh sonix-packer/v1.upt \
  --sonix-v1 --kernel-sha256 "$stock_kernel_sha256"
```

The packer also checks the V1 model identity and builds the root filesystem's update checksum chain. These automated checks do not install or test each generated image on the physical player.

## Project files and documentation

- `sonix-player/` — player sources, LVGL configuration, launcher and build system.
- `sonix-packer/` — stock-image preparation, firmware packaging and validation scripts.
- `FEATURES.md` — detailed feature notes.
- `PATCHES.md` — device-porting and implementation notes.
- `LICENSE` — GNU General Public License, version 3. Third-party components retain their own licenses.

For bug reports, include the exact model, the firmware build/commit and relevant logs. Remove personal data and **never attach streaming credentials**.

## Credits

This port exists because of the original **[Sonix Player](https://github.com/Jepl4r/sonix-player)** project. Almost everything in `sonix-player/` and `sonix-packer/` is that project's work, carried here and adapted; what this fork adds is the TempoTec V1 support and the documentation for it. The original is licensed under the GPL-3.0, and so is this fork.

- **[Jepl4r](https://github.com/Jepl4r)** — author and maintainer of Sonix Player, and the source of the player, the packer and the interface this port is built on.
- **[Tartarus6](https://github.com/Tartarus6)**, **[noisetta](https://github.com/noisetta)**, **[endgame47](https://github.com/endgame47)**, **[hkhrithik007](https://github.com/hkhrithik007)** — the people named in the original project's own "Special thanks" list.
- **[Hinatai](https://github.com/Hinatai)**, **[KrajzegaX](https://github.com/KrajzegaX)**, **[noemdespertis](https://github.com/noemdespertis)** and everyone else on the upstream [contributor list](https://github.com/Jepl4r/sonix-player/graphs/contributors), whose commits this fork inherits.
- The HiBy and TempoTec communities — the people who test, report, translate and explain both projects — who do the work no commit log shows.

Third-party components carried in the tree, or fetched by the build, keep their own authors and licences:

| component | used for |
|---|---|
| [LVGL](https://github.com/lvgl/lvgl) 9.6 | the whole interface |
| [Gearboy](https://github.com/drhelius/Gearboy) by Ignacio Sanchez | the Game Boy core in `sonix-player/src/gb/core/`, kept verbatim |
| [dr_libs](https://github.com/mackron/dr_libs) by David Reid | FLAC and MP3 decoding |
| [stb](https://github.com/nothings/stb) by Sean Barrett | cover art and image read/write |
| [miniz](https://github.com/richgel999/miniz) (RAD Game Tools, Valve, Rich Geldreich) | inflate for covers and EPUB |
| TJpgDec by ChaN | JPEG decoding on the target |
| [SQLite](https://sqlite.org) | the library index |
| [FreeType](https://freetype.org) | fonts at runtime |
| [libogg, libopus and opusfile](https://opus-codec.org) (Xiph.Org) | Opus playback |
| [WavPack](https://www.wavpack.com) by David Bryant | WavPack playback |
| [BlueALSA](https://github.com/arkq/bluez-alsa) by Arkadiusz Bokowy | Bluetooth audio in the HiBy overlay trees, inherited from upstream |
| [Lucide](https://lucide.dev) | the Wi-Fi transfer page glyphs |
| the [Rockbox](https://rockbox.org) toolchain scripts in `sonix-player/rockboxdev/` | the MIPS cross-compiler |

The licence notice for each component sits with it in the source tree; [LICENSE](LICENSE) covers the project as a whole. The official TempoTec V1 firmware used as the base of a build is not part of this repository, and TempoTec, HiBy and the other vendors own their names and firmware.

