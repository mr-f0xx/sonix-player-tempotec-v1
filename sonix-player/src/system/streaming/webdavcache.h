#ifndef WEBDAVCACHE_H
#define WEBDAVCACHE_H

#include "webdav.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEBDAVCACHE_DEFAULT_MAX_MB 512

// Sets the SD card root (`<sd_root>/.local/webdav-cache`), or NULL on unmount.
void webdavcache_set_root(const char *sd_root);
const char *webdavcache_dir(void);

bool webdavcache_owns(const char *path);
bool webdavcache_placeholder(const webdav_entry_t *track, char *out, size_t size);
bool webdavcache_is_placeholder(const char *path);

// True when the complete file is already on disk with no `.incompleto` marker.
bool webdavcache_cached(const webdav_entry_t *track, char *out, size_t size);

// Fetches `track` into `<sd>/.local/webdav-cache/<hex32>.<ext>`.
// Sequential formats (FLAC, MP3, Opus) return once STREAM_PREBUFFER (512 KiB)
// is on disk so `growfile` can play while the rest downloads; seek-on-open
// formats wait interruptibly for the complete file.
bool webdavcache_get(const webdav_entry_t *track, char *out_path, size_t out_size,
					 char *error, size_t error_size);

void webdavcache_cancel(void);
bool webdavcache_downloading(void);

// Maximum cache size in MiB (0 = unlimited, still bounded by 256 MiB free SD floor).
int webdavcache_max_mb(void);
void webdavcache_set_max_mb(int mb);

void webdavcache_stats(int *files_out, uint64_t *bytes_out);
bool webdavcache_clear(int *removed_out);
void webdavcache_clear_on_exit(void);
void webdavcache_prune_incomplete(void);

#endif
