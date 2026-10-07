# Sonix Player for TempoTec Variations V1

Sonix Player is an experimental community port of the Sonix music player to the **TempoTec Variations V1**. The firmware builder starts with an official V1 update package, replaces the stock music player and its interface with Sonix, then repackages the update as `v1.upt`.

This is not an official TempoTec or HiBy release. It targets the **TempoTec Variations V1 only**; compatibility with the V1-A or other players in the Variations family is not established.

## Contents

- [Target device](#target-device)
- [Features](#features)
- [Build a firmware image](#build-a-firmware-image)
- [Install or restore firmware](#install-or-restore-firmware)
- [Streaming credentials](#streaming-credentials)
- [Build from source](#build-from-source)
- [Project files and documentation](#project-files-and-documentation)

## Target device

| Component | Target |
| --- | --- |
| Device | TempoTec Variations V1 |
| SoC | Ingenic X1600, MIPS32r2 / o32 |
| Display | 240 × 320 portrait |
| Audio | Dual Cirrus Logic CS43131; 3.5 mm single-ended and 4.4 mm balanced outputs |
| Update package | `v1.upt` with companion `v1_md5.txt` |

The player UI is built with LVGL 9.6 and adapted for the V1's small display. The package is based on vendor firmware; it is **not** a Linux distribution or a kernel replacement.

## Features

The project includes a local music library and file browser, album artwork, playlists, lyrics, audiobooks, DSP controls, an EPUB reader, and a Gearboy Game Boy / Game Boy Color emulator. The player also contains code for streaming and network features such as Bluetooth audio, AirPlay, DLNA and Wi-Fi file transfer.

Local audio decoder inputs include WAV, FLAC, MP3, Ogg Vorbis, Opus, M4A/M4B/MP4, AAC, ALAC, WavPack, APE, AIFF/AIFC, CAF and DSD (DSF/DFF) files. DSP features include a 10-band graphic equalizer, parametric EQ, MSEB, crossfeed, channel balance and ReplayGain.

See [FEATURES.md](FEATURES.md) for a longer feature list. These are software capabilities, not a claim that every feature has been tested on the V1 hardware. Streaming services also require valid credentials and may depend on third-party service availability.

## Build a firmware image

The recommended route is the repository's GitHub Actions workflow. It cross-compiles the MIPS player and launcher, packages them into an update based on official V1 firmware, validates the result and uploads a downloadable artifact.

1. Open the repository's **Actions** tab and select **Build experimental TempoTec V1 firmware**.
2. Choose **Run workflow** on the branch you want to build. The stock firmware URL is prefilled with the [official TempoTec V1 firmware folder](https://drive.google.com/drive/folders/1jbT9lhRvJ8sbJuepnaBpss861mpsYwnv?usp=sharing); change it only if you have another official V1 package to use.
3. Confirm the required checkbox stating that you understand the image is experimental and not hardware-validated.
4. When the run succeeds, download the `sonix-tempotec-v1-<commit>` artifact. It contains `v1.upt`, `v1_md5.txt`, the player binaries, and stock/output validation reports.

Artifacts are retained for 14 days. A successful Actions run is not a hardware-tested release. See [`.github/workflows/build-tempotec-v1.yml`](.github/workflows/build-tempotec-v1.yml) for the build and validation steps.

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

The packer also checks the V1 model identity and builds the root filesystem's update checksum chain. These checks do not replace testing on the physical player.

## Project files and documentation

- `sonix-player/` — player sources, LVGL configuration, launcher and build system.
- `sonix-packer/` — stock-image preparation, firmware packaging and validation scripts.
- `FEATURES.md` — detailed feature notes.
- `PATCHES.md` — device-porting and implementation notes.
- `LICENSE` — GNU General Public License, version 3. Third-party components retain their own licenses.

For bug reports, include the exact model, the firmware build/commit and relevant logs. Remove personal data and **never attach streaming credentials**.
