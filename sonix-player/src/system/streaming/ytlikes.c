#define _GNU_SOURCE 1
#include "ytlikes.h"

#include "src/system/core/config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The sidecar's own name. It sits beside device_config.ini rather than in it:
// config.c keeps short values only, and a hundred titles are not short values.
#define YTLIKES_FILE "ytmusic_likes.tsv"
// One line: id, timestamp, duration, title, artist, album. The names are capped
// by the struct, so a line can never be longer than this and a longer one read
// back is a damaged file rather than a big entry.
#define YTLIKES_LINE_MAX (YTLIKES_ID_MAX + YTLIKES_TITLE_MAX * 2 + YTLIKES_NAME_MAX + 48)

static ytlike_t likes[YTLIKES_MAX];
static int like_count;
static bool loaded;
// Only the load and the save take it; the array itself is the GUI thread's,
// which is where a heart and the Favourites page both live.
static pthread_mutex_t likes_lock = PTHREAD_MUTEX_INITIALIZER;

static void likes_path(char *out, size_t size) {
	out[0] = '\0';
	const char *cfg = config_path();
	if (!cfg || !cfg[0]) return;
	const char *slash = strrchr(cfg, '/');
	if (slash) snprintf(out, size, "%.*s/%s", (int)(slash - cfg), cfg, YTLIKES_FILE);
	else snprintf(out, size, "%s", YTLIKES_FILE);
}

// A name is one line's field: a tab or a newline in a title would split the
// record, so those become spaces and the file stays readable.
static void clean_field(char *field) {
	for (; *field; field++) {
		if (*field == '\t' || *field == '\r' || *field == '\n') *field = ' ';
	}
}

static void copy_field(char *dst, size_t size, const char *src) {
	if (!dst || !size) return;
	snprintf(dst, size, "%s", src ? src : "");
	clean_field(dst);
}

// The oldest entry is the one that makes room: the end of a newest-first list.
// Removing the last is what keeps a full list honest -- the alternative, a
// silent refusal, looks like the heart not working.
static void drop_oldest(void) {
	if (like_count <= 0) return;
	memmove(&likes[like_count - 2], &likes[like_count - 1], sizeof(likes[0]));
	like_count--;
}

static bool save_locked(void) {
	char path[600], tmp[640];
	likes_path(path, sizeof(path));
	if (!path[0]) return false;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "ytlikes: cannot write %s\n", tmp);
		return false;
	}
	bool ok = true;
	for (int i = 0; i < like_count && ok; i++) {
		const ytlike_t *like = &likes[i];
		if (fprintf(f, "%s\t%ld\t%d\t%s\t%s\t%s\n", like->id, like->liked_at, like->duration,
					like->title, like->artist, like->album) < 0) {
			ok = false;
		}
	}
	if (fclose(f) != 0) ok = false;
	if (!ok || rename(tmp, path) != 0) {
		remove(tmp);
		fprintf(stderr, "ytlikes: the list could not be saved\n");
		return false;
	}
	return true;
}

// One line, split on its tabs. A line that does not start with a plausible
// video id is skipped rather than kept: a damaged record would draw a row that
// cannot play.
static bool parse_line(char *line, ytlike_t *like) {
	memset(like, 0, sizeof(*like));
	char *field[6] = {0};
	int count = 0;
	char *at = line;
	while (count < 6) {
		field[count++] = at;
		char *tab = strchr(at, '\t');
		if (!tab) break;
		*tab = '\0';
		at = tab + 1;
	}
	if (count < 4) return false;
	size_t id_len = strnlen(field[0], YTLIKES_ID_MAX);
	if (id_len != 11 || strpbrk(field[0], " \t/")) return false;
	char *end = NULL;
	long liked_at = strtol(field[1], &end, 10);
	if (!end || end == field[1]) liked_at = 0;
	copy_field(like->id, sizeof(like->id), field[0]);
	like->liked_at = liked_at;
	like->duration = (int)strtol(field[2], NULL, 10);
	copy_field(like->title, sizeof(like->title), field[3]);
	copy_field(like->artist, sizeof(like->artist), count > 4 ? field[4] : "");
	copy_field(like->album, sizeof(like->album), count > 5 ? field[5] : "");
	if (like->duration < 0) like->duration = 0;
	return true;
}

void ytlikes_init(void) {
	pthread_mutex_lock(&likes_lock);
	like_count = 0;
	loaded = true;
	char path[600];
	likes_path(path, sizeof(path));
	FILE *f = path[0] ? fopen(path, "r") : NULL;
	if (!f) {
		// No list yet is the ordinary state of a fresh player, not an error.
		pthread_mutex_unlock(&likes_lock);
		return;
	}
	char line[YTLIKES_LINE_MAX];
	while (like_count < YTLIKES_MAX && fgets(line, sizeof(line), f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		if (!line[0] || line[0] == '#') continue;
		ytlike_t like;
		if (parse_line(line, &like)) likes[like_count++] = like;
	}
	fclose(f);
	pthread_mutex_unlock(&likes_lock);
	fprintf(stderr, "ytlikes: %d liked song%s on the player\n", like_count, like_count == 1 ? "" : "s");
}

// A caller that never ran init (a test, or a page built before the service
// start) reads an empty list rather than whatever the array held.
static void ensure_loaded(void) {
	if (!loaded) ytlikes_init();
}

int ytlikes_count(void) {
	ensure_loaded();
	return like_count;
}

const ytlike_t *ytlikes_at(int index) {
	ensure_loaded();
	if (index < 0 || index >= like_count) return NULL;
	return &likes[index];
}

int ytlikes_snapshot(ytlike_t *out, int limit) {
	ensure_loaded();
	if (!out || limit <= 0) return 0;
	if (limit > YTLIKES_MAX) limit = YTLIKES_MAX;
	pthread_mutex_lock(&likes_lock);
	int count = like_count < limit ? like_count : limit;
	memcpy(out, likes, sizeof(likes[0]) * (size_t)count);
	pthread_mutex_unlock(&likes_lock);
	return count;
}

static int like_index(const char *id) {
	if (!id || !id[0]) return -1;
	for (int i = 0; i < like_count; i++) {
		if (strcmp(likes[i].id, id) == 0) return i;
	}
	return -1;
}

const ytlike_t *ytlikes_find(const char *id) {
	int at = like_index(id);
	return at < 0 ? NULL : &likes[at];
}

bool ytlikes_contains(const char *id) { return like_index(id) >= 0; }

bool ytlikes_add(const ytlike_t *like) {
	ensure_loaded();
	if (!like || strnlen(like->id, YTLIKES_ID_MAX) != 11) return false;
	// Taken by value first: the caller may be handing over an entry of this very
	// list, which the shift below would move under it.
	ytlike_t entry = *like;
	pthread_mutex_lock(&likes_lock);
	// One record per song: hearting it again refreshes the names the row will
	// show and moves it to the front instead of listing it twice.
	int at = like_index(entry.id);
	if (at >= 0) {
		memmove(&likes[1], &likes[0], sizeof(likes[0]) * (size_t)at);
	} else {
		if (like_count == YTLIKES_MAX) drop_oldest();
		memmove(&likes[1], &likes[0], sizeof(likes[0]) * (size_t)like_count);
		like_count++;
	}
	memset(&likes[0], 0, sizeof(likes[0]));
	copy_field(likes[0].id, sizeof(likes[0].id), entry.id);
	copy_field(likes[0].title, sizeof(likes[0].title), entry.title);
	copy_field(likes[0].artist, sizeof(likes[0].artist), entry.artist);
	copy_field(likes[0].album, sizeof(likes[0].album), entry.album);
	likes[0].duration = entry.duration > 0 ? entry.duration : 0;
	likes[0].liked_at = entry.liked_at > 0 ? entry.liked_at : (long)time(NULL);
	char liked_id[YTLIKES_ID_MAX];
	copy_field(liked_id, sizeof(liked_id), likes[0].id);
	int count = like_count;
	bool saved = save_locked();
	pthread_mutex_unlock(&likes_lock);
	if (!saved) return false;
	fprintf(stderr, "ytlikes: liked %s (%d on the player)\n", liked_id, count);
	return true;
}

bool ytlikes_remove(const char *id) {
	ensure_loaded();
	pthread_mutex_lock(&likes_lock);
	int at = like_index(id);
	if (at < 0) {
		pthread_mutex_unlock(&likes_lock);
		return false;
	}
	memmove(&likes[at], &likes[at + 1], sizeof(likes[0]) * (size_t)(like_count - at - 1));
	like_count--;
	memset(&likes[like_count], 0, sizeof(likes[0]));
	int count = like_count;
	bool saved = save_locked();
	pthread_mutex_unlock(&likes_lock);
	if (!saved) return false;
	fprintf(stderr, "ytlikes: unliked %s (%d on the player)\n", id, count);
	return true;
}

bool ytlikes_toggle(const ytlike_t *like) {
	if (!like) return false;
	if (ytlikes_contains(like->id)) {
		ytlikes_remove(like->id);
		return false;
	}
	ytlikes_add(like);
	return true;
}
