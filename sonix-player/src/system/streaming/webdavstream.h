#ifndef WEBDAVSTREAM_H
#define WEBDAVSTREAM_H

#include "webdav.h"
#include "streambridge.h"
#include "src/system/library/metadata.h"

#include <stdbool.h>
#include <stddef.h>

// Two fixed 1 MiB RAM-ring slots, allocated lazily, for SD-Cache-Off streaming
// of sequential formats (FLAC, MP3, Opus). Queue entries use credential-free
// `webdav://<index>.<ext>` pseudo-paths; HTTP Basic Auth stays in extra_headers.
bool webdavstream_owns(const char *path);
bool webdavstream_queue(const webdav_entry_t *tracks, int count, int start,
						char paths[][96], const char **pointers, int *queued, int *selected,
						char *error, size_t error_size);

// GUI thread, registered as a device_state prepare callback.
bool webdavstream_prepare(char *path, size_t path_size);
void webdavstream_prefetch(const char *path);
void webdavstream_cancel(void);
void webdavstream_reset(void);
bool webdavstream_network_wanted(void);

// Decoder lease on the producer's RAM ring.
streambridge_t *webdavstream_acquire(const char *path, int *duration, int *bitrate);
void webdavstream_release(streambridge_t *bridge);
bool webdavstream_metadata(const char *path, song_metadata_t *out);
bool webdavstream_take_error(char *out, size_t size);

#endif
