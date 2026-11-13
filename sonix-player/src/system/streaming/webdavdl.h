#ifndef WEBDAVDL_H
#define WEBDAVDL_H

#include "webdav.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	WEBDAVDL_IDLE = 0,
	WEBDAVDL_RUNNING,
	WEBDAVDL_DONE,
	WEBDAVDL_FAILED,
	WEBDAVDL_CANCELLED,
} webdavdl_state_t;

typedef struct {
	webdavdl_state_t state;
	int current;
	int total;
	int completed;
	int failed;
	uint64_t bytes_done;
	uint64_t bytes_total;
	char title[WEBDAV_TITLE_MAX];
	char error[128];
} webdavdl_progress_t;

typedef struct {
	char path[512];
	char title[WEBDAV_TITLE_MAX];
	char folder[WEBDAV_TITLE_MAX];
	long long modified;
} webdavdl_entry_t;

const char *webdavdl_root(void);
bool webdavdl_owns(const char *path);
void webdavdl_safe_name(const char *raw, const char *fallback, char *out, size_t size);

bool webdavdl_path(const webdav_entry_t *track, char *out, size_t size);
bool webdavdl_exists(const webdav_entry_t *track);

// `index >= 0` saves a single track (`tracks[index]`); `index < 0` saves all
// audio tracks in `tracks[0..count-1]`.
bool webdavdl_start_tracks(const webdav_entry_t *tracks, int count, int index,
						   char *error, size_t error_size);

// Recursively browses `folder_rel_path` (up to depth 4, max WEBDAV_QUEUE_MAX
// tracks) on the background worker and saves every audio track to SD.
bool webdavdl_start_folder(const char *folder_rel_path, const char *folder_title,
						   char *error, size_t error_size);

void webdavdl_cancel(void);
void webdavdl_progress(webdavdl_progress_t *out);
bool webdavdl_busy(void);
void webdavdl_acknowledge(void);

int webdavdl_history(webdavdl_entry_t *entries, int max);
bool webdavdl_delete(const char *path);

#endif
