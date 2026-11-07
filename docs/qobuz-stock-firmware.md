# Qobuz: how the stock firmware authenticates, and how Sonix gets the same result

## The short answer

The Sonix Qobuz client already uses **the same authentication mechanism as the
official TempoTec V1 firmware**, because it was reconstructed from that
firmware's player binary. The only thing Sonix does not carry is the
*credential pair itself*: the stock `hiby_player` contains HiBy's Qobuz
`app_id`/`app_secret`, and publishing another company's API credentials in an
open-source repository (or in a downloadable image) is something this project
deliberately does not do.

What is now possible instead: credentials can be extracted locally from the
official stock firmware **either directly on the player** (from a stock archive
on the microSD card or downloaded over Wi-Fi on demand) **or on a computer**
(via the pure-Python `extract_streamkeys.py` CLI or opt-in at firmware pack
time). Nothing enters git and public release builds stay completely keyless.

## What the official firmware actually is

The V1 runs HiByOS. Its player is `/usr/bin/hiby_player`, a single stripped
32-bit little-endian MIPS ELF (the same application family HiBy ships on the
R1 and R3 Pro II; the HiBy modding community's firmware work explicitly covers
the "R1, R3 II 2025, R3 Pro II, **and Tempotec V1**" generation, all on the
Ingenic X1600 with the same `.upt` format). The Sonix packer removes that
binary and installs `sonix_player` in its place; everything kernel-side stays
stock.

Independent reverse engineering of this binary family finds that "**TIDAL and
Qobuz client credentials sit in the binary in plain text**" (HiBy R1 V2.0
teardown, §7). The Sonix source reaches the same conclusion:
`src/system/streaming/streamkeys.h` states the keys "live inside
`/usr/bin/hiby_player`", and the Qobuz code cites the offsets it traced in the
V1 binary — the login code at `0x00784bd0`, the signature template at
`0x008bfe40`.

## The mechanism: identical, already implemented

Both clients talk to Qobuz's public REST API directly, with no intermediary
server. The flow, as stock does it and as `src/system/streaming/qobuz.c` does
it:

1. **Application identity.** Every request carries the `app_id`, as the
   `X-App-Id` header and as an `app_id` query parameter.
2. **Login.** `GET /user/login?app_id=...` with three HTTP headers:
   `username`, `password`, and `device_manufacturer_id`, the last of which
   carries the **`app_secret`**. This unusual placement is reproduced
   deliberately — it is what the stock code at `0x00784bd0` sends, and the
   comment in `qobuz_login()` says so. The reply yields a
   `user_auth_token`, which is all Sonix stores; the password is never written
   to disk (stock keeps it in cleartext in `/data/qobuz_info`).
3. **Signed streaming URLs.** Only `track/getFileUrl` is signed.
   `request_sig = MD5(object + method + every other parameter as
   name+value in alphabetical order + request_ts + app_secret)`, with no
   separators. `qobuz.c` builds the query and the signature from one parameter
   list, matching the stock `.rodata` template at `0x008bfe40`.
4. **Everything else** (search, albums, artists, playlists, favourites) needs
   only the app id and the session token.

Sonix makes two security improvements over stock: HTTPS with certificate
verification (stock uses cleartext `http://` and disables peer verification),
and token-only storage. The wire protocol Qobuz sees is otherwise the same.

## Why the pair cannot simply be committed

- **It is not ours to publish.** The credentials identify HiBy's application
  to Qobuz. Qobuz confirmed to the user asking for this feature that they do
  not issue application credentials for personal or open-source integrations;
  the API is partner-only. The only working pair on this device is therefore
  a borrowed vendor pair.
- **App ids and tokens are now bound together.** Since Qobuz's 2024 API
  changes, a session token only works with the app id that issued it. Reusing
  the stock pair consistently — as the extractor does, feeding the same pair
  to the unchanged stock-derived login flow — is what keeps logins working.
- **Publishing speeds revocation.** A secret committed to a public repository
  tends to be rotated by the service, which would break Qobuz for every user
  at once, and creates legal exposure for the project. The existing
  `streaming-keys` design (external file, sealed in private builds, absent
  from published images) exists for exactly this reason.

## The solution: local extraction on-device or on host

### 1. Directly on the player (no computer required)

When Qobuz or Tidal is opened on a keyless build, the page offers **Set up from
stock firmware** (`src/system/streaming/stockkeys.c` and
`src/gui/streaming/stockkeys_ui.c`):

- **Offline from microSD:** If a stock firmware package sits on the card
  (`v1_stock.upt`, `v1_original.upt`, `v1.upt`, `v1.zip`, `v1.zip.part1`+`.part2`,
  `rootfs.squashfs`, or `hiby_player`, at the card root or in `stock/` or
  `firmware/`), the player unpacks the ISO 9660 `.upt` and LZO1X/zlib SquashFS
  4.0 image directly in C and extracts both `[qobuz]` and `[tidal]`. Explicit
  stock filenames (`hiby_player`, `rootfs.squashfs`, `v1_stock.upt`,
  `v1_original.upt`, `v1_stock.zip`, `v1.zip.part1`, or `stock/*`) are also
  picked up automatically at boot.
- **One-tap over Wi-Fi:** If no stock file is on the card and Wi-Fi is
  connected, the player streams and inflates the official stock 1.3 split
  archive (`sonix-packer/firmware-input/v1.zip.part1` + `.part2`) over HTTPS
  into a temporary scratch file on the microSD card (or `/usr/data` when >= 48
  MB is free), extracts `/usr/bin/hiby_player`, and deletes the temporary file.
- **Dual persistence, zero reboot:** Extracted credentials are written
  atomically to both `/usr/data/streaming-keys.ini` (so they survive swapping
  microSD cards, and are cleared on factory reset) and
  `<sd_root>/streaming-keys.ini` when a writable card is mounted, preserving
  any existing `[podcast]` or `[lastfm]` sections. Both Qobuz and Tidal reload
  immediately into their **Sign in** screens without restarting.

### 2. Standalone pure-Python CLI (`extract_streamkeys.py`)

`sonix-player/tools/extract_streamkeys.py` reads the credentials out of a
stock `.upt`, `.zip`, `.zip.part1` (+ `.part2`), `.squashfs`, unpacked rootfs
directory, or `hiby_player` ELF using **only the Python 3 standard library**
(with built-in ISO 9660 Joliet, SquashFS 4.0, and LZO1X/zlib readers — neither
`7z` nor `unsquashfs` is required). It locates the Qobuz anchor strings —
`qobuz.com/api.json`, `device_manufacturer_id`, `trackgetFileUrl`,
`request_sig`, `X-App-Id` — and the Tidal OAuth2 anchors + MIPS32 stack-built
`client_id`. It never guesses silently: an ambiguous binary makes it stop and
list the candidates so the pair can be forced explicitly. Values are masked in
its output.

### 3. One-command use through the packer (private builds)

```sh
SONIX_EMBED_STOCK_KEYS=1 ./sonix_firmware_packer.sh
```

While the stock rootfs is unpacked and before `hiby_player` is removed, the
packer runs the extractor and places the result in
`usr/resource/sonix/components/`:

- **with** a `streaming-keys.key` (produced by building the player once): a
  sealed `streaming-keys.bin`, the format `streamkeys.c` reads first;
- **without** one: a plain `streaming-keys.ini`, which a keyless build reads
  anyway, with a loud warning that the image now contains readable
  credentials.

The resulting image behaves like the stock firmware for Qobuz and Tidal: sign in
with your account, no card-side file, no manual keys. Builds without the flag
remain keyless for public distribution.

### Standalone CLI examples

```sh
# Default input: bundled sonix-packer/firmware-input/v1.zip.part1 (+ .part2)
python3 sonix-player/tools/extract_streamkeys.py -o streaming-keys.ini

# Or pass a .upt, .zip, .squashfs, unpacked rootfs, or hiby_player directly
python3 sonix-player/tools/extract_streamkeys.py path/to/v1.upt -o streaming-keys.ini

# Inspect what was found without writing anything
python3 sonix-player/tools/extract_streamkeys.py --check
python3 sonix-player/tools/extract_streamkeys.py --list-candidates path/to/v1.upt

# Force a pair if a future binary defeats the heuristic
python3 sonix-player/tools/extract_streamkeys.py path/to/hiby_player \
    --app-id 123456789 --app-secret 0123456789abcdef0123456789abcdef
```

The Python and C tests in `sonix-player/tests/test_extract_streamkeys.py`,
`sonix-player/tests/test_stockkeys.c`, and
`sonix-player/tests/ui/test_stockkeys_ui.c` (run with
`make -C sonix-player/tests extractkeys stockkeys-ui`) exercise both the
synthetic ELF/archive fixtures and end-to-end extraction from the bundled stock
1.3 split archive.

## Rules and limitations

- **Private builds only.** An image produced with `SONIX_EMBED_STOCK_KEYS=1`
  carries HiBy's API credentials, sealed or not. Do not upload it to a release
  or share it; the default CI workflow leaves the flag off for this reason.
- **Tidal is now extracted too.** Self-serve `developer.tidal.com` keys only
  speak Tidal's OpenAPI v2 and fail device login with `"Client is not a Limited
  Input Device client."` In the stock V1 `hiby_player`, Tidal's 44-character
  base64 `client_secret` sits in `.rodata` next to the OAuth2 device-code
  strings while the 16-character OAuth2 `client_id` is assembled byte-by-byte
  via MIPS32 immediate `sb` stores right before loading the `"client_id"` form
  parameter (distinct from a separate 16-character legacy `x-tidal-token`).
  `extract_streamkeys.py` reconstructs and disambiguates that `client_id` and
  writes both `[qobuz]` and `[tidal]` (or either alone with `--service`).
  During login, `tidal.c` also automatically falls back from scope
  `"r_usr w_usr"` to `"r_usr"` when a read-only device key rejects `w_usr`.
  Podcast Index and Last.fm use freely requested keys and are unaffected.
- **The heuristic can age.** It relies on the documented binary layout. If a
  future stock binary changes it, extraction fails loudly rather than guessing;
  `--list-candidates` plus the manual flags (`--app-id`/`--app-secret` and
  `--tidal-client-id`/`--tidal-client-secret`), or the card file, keep a path
  open.
- **Qobuz can rotate or revoke.** Nothing here makes the borrowed pair
  permanent. The stock firmware would break in the same event; the mechanism
  does not depend on any endpoint Sonix controls.

## Sources

- `src/system/streaming/qobuz.c` and `qobuz.h` — the stock-derived client,
  including the binary offsets traced in the V1 `hiby_player`.
- `src/system/streaming/streamkeys.h` — why credentials live outside the
  source, and the sealed/plain/card loading order.
- HiBy R1 firmware V2.0 teardown (bidhata/Hiby-R1-Mod, §7 and §9): independent
  Ghidra-verified analysis of the same `hiby_player` family; plain-text
  streaming credentials, `www.qobuz.com/api.json/0.2` endpoint.
- hiby-modding/hiby_os_crack: documents the shared HiByOS `.upt` platform
  across HiBy R1/R3 devices and the TempoTec V1.
- Qobuz API documentation (third-party mirror, csngoh/api-documentation):
  partner-only credentials, `request_sig` MD5 construction, format ids
  5/6/7/27 used unchanged by Sonix.
