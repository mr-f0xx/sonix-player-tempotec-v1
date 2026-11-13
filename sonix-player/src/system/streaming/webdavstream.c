#include "webdavstream.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
	streambridge_t *bridge;
	char path[96];
	bool leased;
	bool error_reported;
} slot_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static slot_t slots[2];
static webdav_entry_t *queue;
static int queue_count;
static unsigned queue_generation;
static int wanted_slot = -1;
static char last_error[192];

static void wipe_bytes(void *buf, size_t len) {
	volatile unsigned char *p = (volatile unsigned char *)buf;
	while (len--) {
		*p++ = 0;
	}
}

bool webdavstream_owns(const char *path) {
	return path && strncmp(path, "webdav://", 9) == 0;
}

static const char *extension(const webdav_entry_t *track) {
	if (strcasecmp(track->suffix, "flac") == 0) {
		return "flac";
	}
	if (strcasecmp(track->suffix, "mp3") == 0) {
		return "mp3";
	}
	if (strcasecmp(track->suffix, "opus") == 0) {
		return "opus";
	}
	return NULL;
}

static int index_of(const char *path) {
	unsigned generation;
	int index, end = 0;
	if (!webdavstream_owns(path) || sscanf(path, "webdav://%u/%d.%*s%n", &generation, &index, &end) != 2 ||
		!end || path[end] || generation != queue_generation || index < 0 || index >= queue_count) {
		return -1;
	}
	const char *ext = extension(&queue[index]);
	char expected[96];
	snprintf(expected, sizeof(expected), "webdav://%u/%d.%s", generation, index, ext ? ext : "");
	return strcmp(path, expected) == 0 ? index : -1;
}

static bool allocate_slots(void) {
	for (int i = 0; i < 2; i++) {
		if (!slots[i].bridge) {
			slots[i].bridge = streambridge_create(STREAMBRIDGE_CAPACITY);
			if (!slots[i].bridge) {
				return false;
			}
		}
	}
	return true;
}

static const char *normalize_bridge_error(const char *err) {
	if (!err || !err[0]) {
		return "";
	}
	if (strcmp(err, "subsonic_stream_refused") == 0) {
		return "webdav_auth_failed";
	}
	if (strcmp(err, "subsonic_https_downgrade") == 0) {
		return "webdav_https_downgrade";
	}
	if (strcmp(err, "subsonic_stream_truncated") == 0) {
		return "webdav_stream_truncated";
	}
	return err;
}

bool webdavstream_queue(const webdav_entry_t *tracks, int count, int start,
						char paths[][96], const char **pointers, int *queued, int *selected,
						char *error, size_t size) {
	if (error && size) {
		error[0] = '\0';
	}
	if (!tracks || !paths || !pointers || !queued || !selected ||
		count <= 0 || count > WEBDAV_QUEUE_MAX || start < 0 || start >= count) {
		return false;
	}
	if (!webdav_configured()) {
		if (error && size) {
			snprintf(error, size, "webdav_not_configured");
		}
		return false;
	}
	if (tracks[start].kind != WEBDAV_TRACK || !extension(&tracks[start])) {
		if (error && size) {
			snprintf(error, size, "webdav_enable_sd_cache_for_format");
		}
		return false;
	}

	webdav_entry_t *copy = calloc((size_t)count, sizeof(*copy));
	if (!copy) {
		if (error && size) {
			snprintf(error, size, "webdav_out_of_memory");
		}
		return false;
	}
	int n = 0, chosen = 0;
	for (int i = 0; i < count; i++) {
		if (tracks[i].kind != WEBDAV_TRACK || !extension(&tracks[i])) {
			continue;
		}
		if (i == start) {
			chosen = n;
		}
		copy[n++] = tracks[i];
	}
	if (n == 0) {
		free(copy);
		if (error && size) {
			snprintf(error, size, "webdav_enable_sd_cache_for_format");
		}
		return false;
	}

	pthread_mutex_lock(&lock);
	if (!allocate_slots()) {
		pthread_mutex_unlock(&lock);
		free(copy);
		if (error && size) {
			snprintf(error, size, "webdav_out_of_memory");
		}
		return false;
	}
	for (int i = 0; i < 2; i++) {
		streambridge_cancel(slots[i].bridge);
		if (!slots[i].leased) {
			slots[i].path[0] = '\0';
		}
	}
	free(queue);
	queue = copy;
	queue_count = n;
	queue_generation++;
	if (!queue_generation) {
		queue_generation++;
	}
	wanted_slot = -1;
	last_error[0] = '\0';
	for (int i = 0; i < n; i++) {
		snprintf(paths[i], 96, "webdav://%u/%d.%s", queue_generation, i, extension(&copy[i]));
		pointers[i] = paths[i];
	}
	*queued = n;
	*selected = chosen;
	pthread_mutex_unlock(&lock);
	return true;
}

static int choose_slot(const char *path, bool prefetch) {
	int index = index_of(path);
	if (index < 0) {
		snprintf(last_error, sizeof(last_error), "webdav_invalid_path");
		return -1;
	}
	for (int i = 0; i < 2; i++) {
		if (!slots[i].leased && strcmp(slots[i].path, path) == 0) {
			streambridge_state_t state;
			streambridge_state(slots[i].bridge, &state);
			if (prefetch && state.failed) {
				return -1;
			}
			if (!state.canceled && !state.failed) {
				if (!prefetch) {
					streambridge_promote(slots[i].bridge);
				}
				return i;
			}
		}
	}
	int free_slot = -1;
	for (int i = 0; i < 2; i++) {
		if (!slots[i].leased && (prefetch ? i != wanted_slot : true)) {
			free_slot = i;
			if (!slots[i].path[0]) {
				break;
			}
		}
	}
	if (free_slot < 0) {
		return -1;
	}

	char url[WEBDAV_URL_MAX];
	char auth[WEBDAV_AUTH_HEADER_MAX];
	char error[192] = "";
	bool ok = webdav_file_request_info(queue[index].path, url, sizeof(url), auth, sizeof(auth), error, sizeof(error)) &&
			  streambridge_start_ex(slots[free_slot].bridge, url, auth[0] ? auth : NULL, prefetch);
	wipe_bytes(url, sizeof(url));
	wipe_bytes(auth, sizeof(auth));
	if (!ok) {
		snprintf(last_error, sizeof(last_error), "%s", error[0] ? error : "webdav_stream_failed");
		return -1;
	}
	snprintf(slots[free_slot].path, sizeof(slots[free_slot].path), "%s", path);
	slots[free_slot].error_reported = false;
	return free_slot;
}

static void cancel_locked(void) {
	for (int i = 0; i < 2; i++) {
		if (slots[i].bridge) {
			streambridge_cancel(slots[i].bridge);
		}
		if (!slots[i].leased) {
			slots[i].path[0] = '\0';
		}
	}
	wanted_slot = -1;
	last_error[0] = '\0';
}

bool webdavstream_prepare(char *path, size_t path_size) {
	(void)path_size;
	if (!webdavstream_owns(path)) {
		webdavstream_cancel();
		return true;
	}
	pthread_mutex_lock(&lock);
	wanted_slot = choose_slot(path, false);
	if (wanted_slot < 0 && !last_error[0]) {
		snprintf(last_error, sizeof(last_error), "webdav_stream_failed");
	}
	for (int i = 0; i < 2; i++) {
		if (i != wanted_slot && !slots[i].leased) {
			streambridge_cancel(slots[i].bridge);
			slots[i].path[0] = '\0';
		}
	}
	pthread_mutex_unlock(&lock);
	return true;
}

void webdavstream_prefetch(const char *path) {
	pthread_mutex_lock(&lock);
	if (!webdavstream_owns(path)) {
		for (int i = 0; i < 2; i++) {
			if (i != wanted_slot && !slots[i].leased) {
				streambridge_cancel(slots[i].bridge);
				slots[i].path[0] = '\0';
			}
		}
		pthread_mutex_unlock(&lock);
		return;
	}
	if (wanted_slot >= 0) {
		streambridge_state_t current;
		streambridge_state(slots[wanted_slot].bridge, &current);
		if (!current.failed && !current.canceled && (current.eof || current.buffered >= STREAMBRIDGE_PRECACHE)) {
			choose_slot(path, true);
		}
	}
	pthread_mutex_unlock(&lock);
}

void webdavstream_cancel(void) {
	pthread_mutex_lock(&lock);
	cancel_locked();
	pthread_mutex_unlock(&lock);
}

void webdavstream_reset(void) {
	pthread_mutex_lock(&lock);
	cancel_locked();
	free(queue);
	queue = NULL;
	queue_count = 0;
	pthread_mutex_unlock(&lock);
}

bool webdavstream_network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool wanted = false;
	for (int i = 0; i < 2; i++) {
		if (slots[i].path[0] && slots[i].bridge) {
			streambridge_state_t state;
			streambridge_state(slots[i].bridge, &state);
			wanted |= !state.canceled && !state.failed && !state.eof;
		}
	}
	pthread_mutex_unlock(&lock);
	return wanted;
}

streambridge_t *webdavstream_acquire(const char *path, int *duration, int *bitrate) {
	pthread_mutex_lock(&lock);
	int index = index_of(path);
	int slot = wanted_slot;
	streambridge_t *bridge = NULL;
	if (index >= 0 && slot >= 0 && !slots[slot].leased && strcmp(slots[slot].path, path) == 0) {
		slots[slot].leased = true;
		bridge = slots[slot].bridge;
		if (duration) {
			*duration = 0;
		}
		if (bitrate) {
			*bitrate = 0;
		}
	}
	pthread_mutex_unlock(&lock);
	return bridge;
}

void webdavstream_release(streambridge_t *bridge) {
	pthread_mutex_lock(&lock);
	for (int i = 0; i < 2; i++) {
		if (slots[i].bridge == bridge) {
			streambridge_state_t state;
			streambridge_state(bridge, &state);
			if (state.failed && !state.canceled && !slots[i].error_reported) {
				snprintf(last_error, sizeof(last_error), "%s", normalize_bridge_error(state.error));
			}
			streambridge_cancel(bridge);
			slots[i].leased = false;
			slots[i].path[0] = '\0';
			break;
		}
	}
	pthread_mutex_unlock(&lock);
}

bool webdavstream_metadata(const char *path, song_metadata_t *out) {
	if (!webdavstream_owns(path) || !out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&lock);
	int index = index_of(path);
	if (index >= 0) {
		const webdav_entry_t *track = &queue[index];
		snprintf(out->title, sizeof(out->title), "%s", track->title);
		char *dot = strrchr(out->title, '.');
		if (dot && dot != out->title && webdav_is_audio_suffix(dot + 1)) {
			*dot = '\0';
		}
		snprintf(out->album, sizeof(out->album), "%s", track->album);
		out->has_tags = true;
	}
	pthread_mutex_unlock(&lock);
	return true;
}

bool webdavstream_take_error(char *out, size_t size) {
	pthread_mutex_lock(&lock);
	if (!last_error[0] && wanted_slot >= 0 && slots[wanted_slot].bridge) {
		slot_t *slot = &slots[wanted_slot];
		streambridge_state_t state;
		streambridge_state(slot->bridge, &state);
		if (state.failed && !state.canceled && !slot->error_reported) {
			snprintf(last_error, sizeof(last_error), "%s", normalize_bridge_error(state.error));
			slot->error_reported = true;
		}
	}
	bool have = last_error[0] != '\0';
	if (have && out && size) {
		snprintf(out, size, "%s", last_error);
		last_error[0] = '\0';
	}
	pthread_mutex_unlock(&lock);
	return have;
}
