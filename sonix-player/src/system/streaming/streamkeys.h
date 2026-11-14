#ifndef STREAMKEYS_H
#define STREAMKEYS_H

#include <stdbool.h>

// The streaming services' application credentials, read from a file on the
// device rather than written in here.
//
// Qobuz and Tidal offer no way to talk to their APIs without a pair of
// application keys, and the ones this device has are HiBy's: they live inside
// /usr/bin/hiby_player, Tidal's even assembled byte by byte at run time so it
// never appears as a string. They are HiBy's keys, not this project's, and open
// source that carries them publishes them.
//
// So they sit in a file on the rootfs, which whoever assembles the firmware puts
// alongside the other resource files. It is written as an INI:
//
//     [qobuz]
//     app_id = ...
//     app_secret = ...
//
//     [tidal]
//     client_id = ...
//     client_secret = ...
//
//     [podcast]
//     api_key = ...
//     api_secret = ...
//
//     [lastfm]
//     api_key = ...
//     api_secret = ...
//
//     [ytmusic]
//     api_key = ...
//
// and shipped sealed, as /usr/resource/sonix/components/streaming-keys.bin, by
// tools/seal_streamkeys.py. The key that opens it is not in the source: the
// tool keeps it in streaming-keys.key, outside git, and the Makefile compiles
// it into the player. That keeps the keys out of an unpacked image; it does not
// keep them from whoever takes the binary apart.
//
// A plain streaming-keys.ini next to it is still read when there is no .bin,
// for the simulator and for builds made without a key. Which of the two a file
// is, is decided by its first bytes, not its name.
//
// A third place is the microSD card: streaming-keys.bin or streaming-keys.ini
// at the card's root, read by streamkeys_load_card() once the card is mounted.
// It is there because an image published for anyone to download ships without
// keys -- there is no other way it could be published -- and the person who
// installs it should be able to turn streaming on without building a firmware.
// Copy one file to the card and restart.
//
// A private build does not have to hand-write the Qobuz pair at all: it sits
// as plain strings inside the stock hiby_player, and
// tools/extract_streamkeys.py (wired into the packer on
// SONIX_EMBED_STOCK_KEYS=1) pulls it out of the official firmware while that
// binary is unpacked and writes the file above. That keeps the credentials out
// of git while giving a personal image the same out-of-the-box Qobuz login the
// stock firmware has. See docs/qobuz-stock-firmware.md; such an image is not
// for redistribution.
//
// Without any of them, or without a section, the matching service disables
// itself and its menu entry says so instead of failing halfway through a login.
// Podcasts is the exception: it has a second directory that needs no key, so
// a missing [podcast] section changes where it looks rather than turning it
// off. See podcast.h.
//
// The path can be changed with [streaming] keys_file in device_config.ini, to
// keep the keys on the card instead of the read-only rootfs -- the easy path
// the day a service changes its keys.

#include "src/system/core/respath.h"

#define STREAMKEYS_PATH SONIX_RESOURCE_DIR "/components/streaming-keys.bin"
#define STREAMKEYS_PLAIN_PATH SONIX_RESOURCE_DIR "/components/streaming-keys.ini"

#ifdef HOST_BUILD
#define STREAMKEYS_DATA_DEFAULT_PATH "usr/data/streaming-keys.ini"
#else
#define STREAMKEYS_DATA_DEFAULT_PATH "/usr/data/streaming-keys.ini"
#endif

// The two names looked for at the card's root, in this order.
#define STREAMKEYS_CARD_BIN "streaming-keys.bin"
#define STREAMKEYS_CARD_INI "streaming-keys.ini"

// Writable internal-storage path for credentials extracted on the device
// (/usr/data/streaming-keys.ini on target; SONIX_STREAMKEYS_DATA overrides it).
const char *streamkeys_data_path(void);

// Reads the file. Call once at startup, after config_init(). A missing file is
// not an error.
void streamkeys_init(void);

// Looks for the same two files at the root of the microSD card: .bin first,
// then .ini. Call once, after the card is mounted, which is long after
// streamkeys_init() has run -- nothing mounts the card before the player does,
// so at startup there is nothing to read there yet.
//
// The card only ever fills a gap: a key already in the image (or in
// /usr/data/streaming-keys.ini) wins, and the card supplies the ones it does
// not carry -- a build with no InnerTube key of its own gets it from the card
// this way, with no rebuild and no reflash.
//
// True when the card supplied something the build did not already have, which
// is the caller's signal to hand the keys to the services that read them at
// startup.
bool streamkeys_load_card(const char *card_root);

// Re-reads /usr/data/streaming-keys.ini and the microSD card so credentials
// extracted at run time take effect immediately without a restart.
bool streamkeys_reload(const char *card_root);

// Atomically writes extracted [qobuz] and [tidal] pairs to /usr/data/streaming-keys.ini
// and (when card_root is non-empty) <card_root>/streaming-keys.ini, preserving
// any existing [podcast] or [lastfm] entries. True if at least one write succeeded.
bool streamkeys_write_extracted(const char *qobuz_id, const char *qobuz_sec, const char *tidal_id,
								const char *tidal_sec, const char *card_root);

// NULL when absent. The pointers stay valid for the life of the program.
const char *streamkeys_qobuz_app_id(void);
const char *streamkeys_qobuz_app_secret(void);
const char *streamkeys_tidal_client_id(void);
const char *streamkeys_tidal_client_secret(void);
// Podcast Index, and the one pair of these that is optional. It identifies
// this player rather than a HiBy application, is requested on podcastindex.org
// -- whose terms do not allow a free email address, so it has to be an address
// on a domain of your own -- and each request is signed with the SHA-1 of
// key + secret + time.
//
// Without it Podcasts still works: podcast.c asks Apple's directory instead,
// which needs no key, and this pair is what selects Podcast Index for whoever
// has one. The other three services have no second directory to fall back to.
const char *streamkeys_podcast_key(void);
const char *streamkeys_podcast_secret(void);
// Last.fm. An API account of this player, requested on last.fm/api: it signs
// every call with the MD5 of the sorted parameters plus the secret.
const char *streamkeys_lastfm_key(void);
const char *streamkeys_lastfm_secret(void);
// SoundCloud, all optional: client_id is auto-discovered from SoundCloud's web
// assets when omitted; username or oauth_token preconfigures the user's free or
// paid (Go / Go+) account from the card without typing on the screen.
const char *streamkeys_soundcloud_client_id(void);
const char *streamkeys_soundcloud_oauth_token(void);
const char *streamkeys_soundcloud_username(void);
// YouTube Music, optional: the InnerTube key the music.youtube.com page hands
// every visitor, which this player sends on its requests as a web client. It is
// not an account credential -- without it the YouTube Music page still works,
// but it is the one value a build that carries no key must be given, by build
// (make YTMUSIC_API_KEY=...) or on the card. The card only ever fills a gap:
// a key already in the image or in /usr/data wins, the card supplies the rest.
const char *streamkeys_ytmusic_api_key(void);

// Where they were read from, for the log and the information page. NULL when
// none were found.
const char *streamkeys_source(void);

#endif /* STREAMKEYS_H */
