#define _GNU_SOURCE 1
#include "ytpodcastplayed.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "src/system/core/config.h"

#define YTPODCASTPLAYED_MAX 1000
#define YTPODCASTPLAYED_ID_LEN 12

static char played_ids[YTPODCASTPLAYED_MAX][YTPODCASTPLAYED_ID_LEN];
static int played_count;
static bool loaded;
static pthread_mutex_t played_lock = PTHREAD_MUTEX_INITIALIZER;

static bool valid_id(const char *id) {
	if (!id || strlen(id) != YTPODCASTPLAYED_ID_LEN - 1) return false;
	for (int i = 0; i < YTPODCASTPLAYED_ID_LEN - 1; i++) {
		unsigned char c = (unsigned char)id[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			  (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
	}
	return true;
}

static bool played_path(char *out, size_t size) {
	const char *config = config_path();
	if (!config || !config[0]) return false;
	const char *slash = strrchr(config, '/');
	int written;
	if (slash) {
		written = snprintf(out, size, "%.*s/ytmusic_podcast_played.tsv",
						   (int)(slash - config), config);
	} else {
		written = snprintf(out, size, "ytmusic_podcast_played.tsv");
	}
	return written >= 0 && (size_t)written < size;
}

static int find_id(const char *id) {
	for (int i = 0; i < played_count; i++) {
		if (strcmp(played_ids[i], id) == 0) return i;
	}
	return -1;
}

void ytpodcastplayed_init(void) {
	pthread_mutex_lock(&played_lock);
	played_count = 0;
	loaded = true;
	char path[600];
	FILE *file = played_path(path, sizeof(path)) ? fopen(path, "r") : NULL;
	if (file) {
		char id[YTPODCASTPLAYED_ID_LEN];
		while (played_count < YTPODCASTPLAYED_MAX && fgets(id, sizeof(id), file)) {
			char *end = strpbrk(id, "\r\n");
			if (end) *end = '\0';
			if (valid_id(id) && find_id(id) < 0) {
				snprintf(played_ids[played_count++], YTPODCASTPLAYED_ID_LEN, "%s", id);
			}
		}
		fclose(file);
	}
	pthread_mutex_unlock(&played_lock);
}

static void ensure_loaded(void) {
	if (!loaded) ytpodcastplayed_init();
}

bool ytpodcastplayed_get(const char *video_id) {
	if (!valid_id(video_id)) return false;
	ensure_loaded();
	pthread_mutex_lock(&played_lock);
	bool played = find_id(video_id) >= 0;
	pthread_mutex_unlock(&played_lock);
	return played;
}

bool ytpodcastplayed_set(const char *video_id, bool played) {
	if (!valid_id(video_id)) return false;
	ensure_loaded();
	char path[600], temp[640];
	if (!played_path(path, sizeof(path)) || snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) {
		return false;
	}
	pthread_mutex_lock(&played_lock);
	int at = find_id(video_id);
	bool added = false;
	bool removed = false;
	if (played && at < 0) {
		if (played_count == YTPODCASTPLAYED_MAX) {
			pthread_mutex_unlock(&played_lock);
			fprintf(stderr, "ytpodcastplayed: history is full\n");
			return false;
		}
		snprintf(played_ids[played_count++], YTPODCASTPLAYED_ID_LEN, "%s", video_id);
		added = true;
	} else if (!played && at >= 0) {
		memmove(&played_ids[at], &played_ids[at + 1],
				sizeof(played_ids[0]) * (size_t)(played_count - at - 1));
		played_count--;
		removed = true;
	}

	FILE *file = fopen(temp, "w");
	if (!file) {
		fprintf(stderr, "ytpodcastplayed: cannot write %s\n", temp);
		goto failed;
	}
	bool ok = true;
	for (int i = 0; i < played_count && ok; i++) {
		ok = fprintf(file, "%s\n", played_ids[i]) > 0;
	}
	if (fclose(file) != 0) ok = false;
	if (!ok || rename(temp, path) != 0) {
		remove(temp);
		fprintf(stderr, "ytpodcastplayed: could not save history\n");
		goto failed;
	}
	pthread_mutex_unlock(&played_lock);
	return true;

failed:
	if (added) {
		played_count--;
		played_ids[played_count][0] = '\0';
	} else if (removed) {
		memmove(&played_ids[at + 1], &played_ids[at],
				sizeof(played_ids[0]) * (size_t)(played_count - at));
		snprintf(played_ids[at], YTPODCASTPLAYED_ID_LEN, "%s", video_id);
		played_count++;
	}
	pthread_mutex_unlock(&played_lock);
	return false;
}
