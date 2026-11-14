#ifndef JELLYFINCACHE_H
#define JELLYFINCACHE_H

#include "jellyfin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// SD-backed Jellyfin audio cache.
//
// Files live under `<sd_root>/.local/jellyfin-cache/`, invisible to the music
// scanner (`library_scan` skips dot-directories). For codecs with progressive
// growfile support in `decode.c` (MP3, FLAC, Opus), downloads prebuffer and
// hand off the remaining bytes to a background completion thread so playback
// starts immediately while the rest streams to the card. Other Direct Play
// formats (M4A, Ogg, WAV, WavPack, APE, AAC, AIFF, DSD) complete on the card
// before returning so every Sonix codec plays reliably with full seeking.
//
// Sidecar files (`<audio>.tags` and `<stem>.jpg`) match the exact format read
// by `metadata.c` and `streamkeep.c`.

#define JELLYFINCACHE_DEFAULT_MAX_MB 512
#define JELLYFINCACHE_MIN_MAX_MB 64
#define JELLYFINCACHE_MAX_MAX_MB (1024 * 1024)
#define JELLYFINCACHE_MIN_FREE (256LL * 1024LL * 1024LL)

// Updates the active SD root (call on startup and on card mount/unmount).
// Passing NULL disables caching until a card is mounted again.
void jellyfincache_set_root(const char *sd_root);

// Returns the active cache directory path, or "" if no card is mounted.
const char *jellyfincache_dir(void);

// Configurable cache cap in MiB (`0` means Unlimited, always keeping >= 256 MiB
// free on the card). Persisted in `[jellyfin] cache_max_mb`.
int jellyfincache_get_max_mb(void);
void jellyfincache_set_max_mb(int mb);
long long jellyfincache_get_max_bytes(void);
void jellyfincache_set_max_bytes(long long max_bytes);

// True if `path` resides inside the Jellyfin SD cache directory.
bool jellyfincache_owns(const char *path);

// Extracts the Jellyfin item ID from a cache file path (e.g.
// `<dir>/<id>__m320.mp3` or `<dir>/<id>.flac` -> `<id>`).
bool jellyfincache_item_id_of(const char *path, char *out, size_t out_size);

// Builds the deterministic cache path for a track at the currently configured
// quality (used as a queue placeholder before the track is downloaded).
bool jellyfincache_placeholder_path(const char *item_id, const char *container_hint, char *out, size_t out_size);

// Returns true and writes the path if a complete cached copy of `item_id`
// (matching the current quality) already exists on the card.
bool jellyfincache_find_playable(const char *item_id, char *out, size_t out_size);

// Ensures `track` is available in the SD cache and writes its path to
// `out_path`. When `wait_complete` is false and the resolved format supports
// progressive `growfile` (MP3, FLAC, Opus), returns as soon as prebuffer is
// ready while a background thread finishes the file. When `wait_complete` is
// true (e.g. offline Keep on card), waits until the full file is on disk.
// Blocks: call from a worker thread only.
bool jellyfincache_get(const jellyfin_entry_t *track, bool wait_complete, char *out_path, size_t out_size, char *error,
					   size_t error_size);

// Quietly prefetches the next track in the queue once the download turn is
// idle. Blocks: call from a low-priority background thread.
void jellyfincache_prefetch(const jellyfin_entry_t *track);

// Protects `path` (usually the currently playing track) from LRU eviction.
void jellyfincache_protect(const char *path);

// Abandons any active download and waits for its background thread to finish.
void jellyfincache_cancel(void);

// True while a foreground or background cache transfer is active.
bool jellyfincache_downloading(void);

// Cache usage summary (tracks count and total bytes on disk).
void jellyfincache_stats(int *track_count, uint64_t *bytes);

// Removes every cached file (except the currently protected/growing track
// unless cancelled first). Returns true if the cache directory is clean.
bool jellyfincache_clear(void);

// Shutdown / reboot cleanup: cancels downloads and removes temporary
// `.incompleto` fragments (keeps valid cached tracks unless `wipe_all`).
void jellyfincache_clear_on_exit(void);

#endif /* JELLYFINCACHE_H */
