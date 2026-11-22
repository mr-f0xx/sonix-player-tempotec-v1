#define _GNU_SOURCE 1
#include "spotifymock.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The demo library, see spotifymock.h.
//
// Deliberately small and fixed: 12 liked tracks, three playlists, a search that
// looks through the same songs. The names are invented ones -- there is no
// point borrowing a real artist's for a screen that cannot play it -- and the
// track ids are the shape Spotify's own are (22 base62 characters), so the
// page's id handling is exercised rather than bypassed.
//
// The clock runs off CLOCK_MONOTONIC, so a suspend and resume mid-demo leaves
// the position where it was, as the daemon's own position would.

typedef struct {
	const char *id;
	const char *title;
	const char *artist;
	const char *album;
	int duration_ms;
} mock_track_t;

static const mock_track_t LIKED[] = {
	{"4uLU6hMCjMI75M1A2tKUQC", "Northern Lines", "Marta Vey", "Northern Lines", 214000},
	{"1301WleyT98MSxVHPZCA6M", "Blue Hour", "The Cardamom Set", "Blue Hour", 247000},
	{"7ouMYWpwJ422jRcDASZB7P", "Paper Lanterns", "Yuki Mori", "Small Robots", 198000},
	{"0VjIjW4GlUZAMYd2vXMi3b", "Static Bloom", "Halcyon Field", "Static Bloom", 263000},
	{"3n3Ppam7vgaVa1iaRUc9Lp", "The Long Way Down", "Esko & Rau", "The Long Way Down", 301000},
	{"6habFhsOp2NvshLv26DqMb", "Ferry at Six", "Marta Vey", "Northern Lines", 189000},
	{"2takcwOaAZWiXQijPHIx7B", "Low Cloud", "Aster House", "Weather Systems", 228000},
	{"5ChkMS8OtdzJeqyybCc9R5", "Tidewater", "The Cardamom Set", "Blue Hour", 256000},
	{"1dGr1c8CrMLDpV6mPbImSI", "Signal Hill", "Halcyon Field", "Static Bloom", 205000},
	{"11dFghVXANMlKmJXsNCbNl", "Kettle & Coal", "Esko & Rau", "The Long Way Down", 234000},
	{"7GhIk7Il098yCjg4BQjzvb", "Morning Fog", "Yuki Mori", "Small Robots", 221000},
	{"76dnflXazT61mxfaYJnffD", "Last Train North", "Aster House", "Weather Systems", 279000},
};

#define LIKED_COUNT ((int)(sizeof(LIKED) / sizeof(LIKED[0])))

static const spotify_list_t PLAYLISTS[] = {
	{"37i9dQZF1DXcBWIGoYBM5M", "Long Drive", "demo", 8},
	{"5ABaZgLDz0v2YYqDd4VvKp", "Quiet Mornings", "demo", 6},
	{"2v3qXGqjS4QmA1oB7xYdCz", "Blue Hour Radio", "demo", 5},
};

#define PLAYLIST_COUNT ((int)(sizeof(PLAYLISTS) / sizeof(PLAYLISTS[0])))

// Which liked tracks each playlist holds, as indices into LIKED.
static const int PLAYLIST_TRACKS[][12] = {
	{4, 0, 5, 8, 2, 9, 11, 7},
	{10, 6, 2, 1, 7},
	{1, 7, 3, 8, 6},
};

static pthread_mutex_t mock_lock = PTHREAD_MUTEX_INITIALIZER;
static bool inited;
static int cursor; // the place in the active list
static int playing; // 1 playing, 0 paused
static long long position_ms;
static long long last_tick_ms;
static int shuffle;
static int repeat;
static int volume;
static int queue_kind; // 0 liked, 1 playlist, 2 search
static int queue_playlist;
static char queue_search[128];
static int queue_ids[64];
static int queue_len;
static int queue_order[64]; // play order: the queue's indices
static int order_pos, order_len;
static int shuffle_seed;

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void rebuild_order(void) {
	queue_len = 0;
	for (int i = 0; i < 64; i++) {
		queue_order[i] = i;
	}

	if (queue_kind == 0) {
		queue_len = LIKED_COUNT;
		// Identity: the queue's place i is Liked Songs' track i. This has to
		// happen after queue_len is set -- the loop used to run before it, so
		// the whole map stayed zero and every place in the demo's queue played
		// the first track while the counter advanced.
		for (int i = 0; i < queue_len; i++) {
			queue_ids[i] = i;
		}
	} else if (queue_kind == 1) {
		queue_len = PLAYLISTS[queue_playlist].total;
		if (queue_len > (int)(sizeof(PLAYLIST_TRACKS[0]) / sizeof(PLAYLIST_TRACKS[0][0]))) {
			queue_len = (int)(sizeof(PLAYLIST_TRACKS[0]) / sizeof(PLAYLIST_TRACKS[0][0]));
		}
		for (int i = 0; i < queue_len; i++) {
			queue_ids[i] = PLAYLIST_TRACKS[queue_playlist][i];
		}
	} else {
		int n = 0;
		for (int i = 0; i < LIKED_COUNT && n < 64; i++) {
			if (queue_search[0] == '\0' || strcasestr(LIKED[i].title, queue_search) ||
				strcasestr(LIKED[i].artist, queue_search)) {
				queue_ids[n++] = i;
			}
		}
		queue_len = n;
	}

	for (int i = 0; i < 64; i++) {
		queue_order[i] = i;
	}
	if (shuffle) {
		// A fixed-seed Fisher-Yates: the demo's shuffles are reproducible, and
		// the order does not change until the seed does.
		unsigned seed = (unsigned)shuffle_seed * 2654435761u + 1013904223u;
		for (int i = queue_len - 1; i > 0; i--) {
			seed = seed * 1103515245u + 12345u;
			int j = (int)((seed >> 16) % (unsigned)(i + 1));
			int t = queue_order[i];
			queue_order[i] = queue_order[j];
			queue_order[j] = t;
		}
	}
	order_len = queue_len;
	order_pos = 0;
	for (int i = 0; i < order_len; i++) {
		if (queue_order[i] == cursor) {
			order_pos = i;
			break;
		}
	}
}

static const mock_track_t *current(void) {
	if (queue_len <= 0 || cursor < 0 || cursor >= queue_len) {
		return NULL;
	}
	int idx = queue_ids[cursor];
	if (idx < 0 || idx >= LIKED_COUNT) {
		return NULL;
	}
	return &LIKED[idx];
}

static void load(int index, bool play) {
	if (index < 0 || index >= queue_len) {
		return;
	}
	cursor = index;
	for (int i = 0; i < order_len; i++) {
		if (queue_order[i] == index) {
			order_pos = i;
			break;
		}
	}
	position_ms = 0;
	last_tick_ms = now_ms();
	playing = play ? 1 : 0;
}

static void advance(int step) {
	if (order_len <= 0) {
		return;
	}
	int next = order_pos + step;
	if (next >= order_len) {
		if (repeat == 1) {
			next = 0;
		} else if (repeat == 2) {
			load(cursor, playing);
			return;
		} else {
			next = order_len - 1;
			playing = 0;
			return;
		}
	}
	if (next < 0) {
		next = 0;
	}
	order_pos = next;
	load(queue_order[next], true);
}

void spotifymock_init(void) {
	pthread_mutex_lock(&mock_lock);
	if (!inited) {
		inited = true;
		queue_kind = 0;
		queue_playlist = 0;
		volume = 80;
		repeat = 0;
		shuffle = 0;
		playing = 0;
		position_ms = 0;
		cursor = 0;
		rebuild_order();
	}
	pthread_mutex_unlock(&mock_lock);
}

bool spotifymock_available(void) { return inited; }

void spotifymock_tick(void) {
	pthread_mutex_lock(&mock_lock);
	long long t = now_ms();
	long long delta = t - last_tick_ms;
	last_tick_ms = t;
	if (!playing || delta <= 0) {
		pthread_mutex_unlock(&mock_lock);
		return;
	}
	position_ms += delta;
	const mock_track_t *tr = current();
	if (tr && position_ms >= tr->duration_ms) {
		advance(1);
	}
	pthread_mutex_unlock(&mock_lock);
}

bool spotifymock_playing(void) {
	bool p;
	pthread_mutex_lock(&mock_lock);
	p = playing != 0;
	pthread_mutex_unlock(&mock_lock);
	return p;
}

void spotifymock_status(spotify_status_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&mock_lock);
	memset(&out->track, 0, sizeof(out->track));
	out->connected = true;
	out->logged_in = true;
	out->shuffle = shuffle != 0;
	out->repeat = repeat;
	out->volume = volume;
	out->state = playing ? SPOTIFY_STATE_PLAYING : (queue_len > 0 ? SPOTIFY_STATE_PAUSED : SPOTIFY_STATE_READY);
	out->position_ms = (int)position_ms;
	snprintf(out->account, sizeof(out->account), "Demo library");
	snprintf(out->backend, sizeof(out->backend), "mock");
	out->error[0] = '\0';

	const mock_track_t *tr = current();
	if (tr) {
		snprintf(out->track.id, sizeof(out->track.id), "%s", tr->id);
		snprintf(out->track.title, sizeof(out->track.title), "%s", tr->title);
		snprintf(out->track.artist, sizeof(out->track.artist), "%s", tr->artist);
		snprintf(out->track.album, sizeof(out->track.album), "%s", tr->album);
		out->track.cover_url[0] = '\0';
		out->track.duration_ms = tr->duration_ms;
		out->track.index = cursor + 1;
		out->track.count = queue_len;
	}
	pthread_mutex_unlock(&mock_lock);
}

static bool matches(const char *query, const mock_track_t *tr) {
	if (!query[0]) {
		return true;
	}
	return strcasestr(tr->title, query) || strcasestr(tr->artist, query) || strcasestr(tr->album, query);
}

// "T\t<id>\t<title>\t<artist>\t<album>\t<cover>\t<duration>"
static int track_line(char *out, size_t out_size, const mock_track_t *tr) {
	return snprintf(out, out_size, "T\t%s\t%s\t%s\t%s\t\t%d", tr->id, tr->title, tr->artist, tr->album,
					tr->duration_ms);
}

bool spotifymock_command(const char *line_in, char *out, size_t out_size, bool whole_reply) {
	if (out && out_size) {
		out[0] = '\0';
	}
	(void)whole_reply;

	char line[512];
	snprintf(line, sizeof(line), "%s", line_in ? line_in : "");

	char *save = NULL;
	char *cmd = strtok_r(line, " ", &save);
	if (!cmd) {
		return false;
	}
	char *arg1 = strtok_r(NULL, " ", &save);
	char *arg2 = strtok_r(NULL, " ", &save);
	char *arg3 = strtok_r(NULL, " ", &save);

	pthread_mutex_lock(&mock_lock);

	if (strcmp(cmd, "TOGGLE") == 0) {
		playing = !playing;
		last_tick_ms = now_ms();
	} else if (strcmp(cmd, "PAUSE") == 0) {
		playing = 0;
	} else if (strcmp(cmd, "RESUME") == 0 || strcmp(cmd, "PLAY") == 0) {
		if (arg1 && strcmp(cmd, "PLAY") == 0) {
			for (int i = 0; i < LIKED_COUNT; i++) {
				if (strcmp(LIKED[i].id, arg1) == 0) {
					queue_kind = 0;
					rebuild_order();
					cursor = i;
					load(i, true);
					break;
				}
			}
		} else {
			if (queue_len == 0) {
				queue_kind = 0;
				rebuild_order();
			}
			playing = 1;
			last_tick_ms = now_ms();
		}
	} else if (strcmp(cmd, "NEXT") == 0) {
		advance(1);
	} else if (strcmp(cmd, "PREV") == 0) {
		advance(-1);
	} else if (strcmp(cmd, "SEEK") == 0) {
		if (arg1) {
			position_ms = atoll(arg1);
			last_tick_ms = now_ms();
		}
	} else if (strcmp(cmd, "SHUFFLE") == 0) {
		shuffle = arg1 && atoi(arg1) != 0;
		int keep = cursor;
		rebuild_order();
		cursor = keep;
		for (int i = 0; i < order_len; i++) {
			if (queue_order[i] == cursor) {
				order_pos = i;
				break;
			}
		}
	} else if (strcmp(cmd, "REPEAT") == 0) {
		repeat = arg1 ? atoi(arg1) : 0;
	} else if (strcmp(cmd, "VOLUME") == 0) {
		volume = arg1 ? atoi(arg1) : volume;
	} else if (strcmp(cmd, "PLAYCTX") == 0) {
		if (arg1 && strcmp(arg1, "playlist") == 0 && arg2) {
			for (int i = 0; i < PLAYLIST_COUNT; i++) {
				if (strcmp(PLAYLISTS[i].id, arg2) == 0) {
					queue_kind = 1;
					queue_playlist = i;
					break;
				}
			}
		} else if (arg1 && strcmp(arg1, "search") == 0) {
			queue_kind = 2;
			snprintf(queue_search, sizeof(queue_search), "%s", arg2 ? arg2 : "");
		} else {
			queue_kind = 0;
		}
		rebuild_order();
		if (arg3) {
			for (int i = 0; i < queue_len; i++) {
				if (strcmp(queue_ids[i] >= 0 && queue_ids[i] < LIKED_COUNT ? LIKED[queue_ids[i]].id : "", arg3) == 0) {
					load(i, true);
					break;
				}
			}
		}
	} else if (strcmp(cmd, "LIKED") == 0) {
		int offset = arg1 ? atoi(arg1) : 0;
		int limit = arg2 ? atoi(arg2) : 20;
		size_t used = 0;
		char tl[512];
		int n = 0;
		for (int i = offset; i < LIKED_COUNT && n < limit; i++, n++) {
			int len = track_line(tl, sizeof(tl), &LIKED[i]);
			if (out && out_size && used + (size_t)len + 2 < out_size) {
				memcpy(out + used, tl, (size_t)len);
				used += (size_t)len;
				out[used++] = '\n';
				out[used] = '\0';
			}
		}
		if (out && out_size && used + 24 < out_size) {
			used += (size_t)snprintf(out + used, out_size - used, "N\t%d\n", LIKED_COUNT);
		}
	} else if (strcmp(cmd, "PLAYLISTS") == 0) {
		size_t used = 0;
		for (int i = 0; i < PLAYLIST_COUNT; i++) {
			int len = snprintf(out + used, out_size > used ? out_size - used : 0, "P\t%s\t%s\t%s\t%d\n",
							   PLAYLISTS[i].id, PLAYLISTS[i].name, PLAYLISTS[i].owner, PLAYLISTS[i].total);
			if (len > 0 && out_size > used) {
				used += (size_t)len;
			}
		}
	} else if (strcmp(cmd, "PLAYLIST") == 0) {
		int which = -1;
		for (int i = 0; i < PLAYLIST_COUNT; i++) {
			if (arg1 && strcmp(PLAYLISTS[i].id, arg1) == 0) {
				which = i;
			}
		}
		int offset = arg2 ? atoi(arg2) : 0;
		int limit = arg3 ? atoi(arg3) : 20;
		size_t used = 0;
		char tl[512];
		if (which >= 0) {
			int total = PLAYLISTS[which].total;
			for (int i = offset; i < total && i < (int)(sizeof(PLAYLIST_TRACKS[0]) / sizeof(PLAYLIST_TRACKS[0][0])) &&
								 i < offset + limit;
				 i++) {
				int idx = PLAYLIST_TRACKS[which][i];
				if (idx < 0 || idx >= LIKED_COUNT) {
					continue;
				}
				int len = track_line(tl, sizeof(tl), &LIKED[idx]);
				if (out && out_size && used + (size_t)len + 2 < out_size) {
					memcpy(out + used, tl, (size_t)len);
					used += (size_t)len;
					out[used++] = '\n';
					out[used] = '\0';
				}
			}
			if (out && out_size && used + 24 < out_size) {
				used += (size_t)snprintf(out + used, out_size - used, "N\t%d\n", total);
			}
		} else {
			return false;
		}
	} else if (strcmp(cmd, "SEARCH") == 0) {
		// The query is one argument in the real protocol; the mock splits at
		// the first space it was given, which is what the page sends.
		int offset = arg2 ? atoi(arg2) : 0;
		int limit = arg3 ? atoi(arg3) : 20;
		size_t used = 0;
		char tl[512];
		int n = 0;
		for (int i = 0; i < LIKED_COUNT && n < limit; i++) {
			if (!matches(arg1 ? arg1 : "", &LIKED[i])) {
				continue;
			}
			if (n >= offset) {
				int len = track_line(tl, sizeof(tl), &LIKED[i]);
				if (out && out_size && used + (size_t)len + 2 < out_size) {
					memcpy(out + used, tl, (size_t)len);
					used += (size_t)len;
					out[used++] = '\n';
					out[used] = '\0';
				}
			}
			n++;
		}
		if (out && out_size && used + 24 < out_size) {
			used += (size_t)snprintf(out + used, out_size - used, "N\t%d\n", n);
		}
	} else if (strcmp(cmd, "QUEUE") == 0) {
		if (out && out_size) {
			snprintf(out, out_size, "Q\t%d\t%d\n", cursor, queue_len);
		}
	} else if (strcmp(cmd, "STATUS") == 0) {
		spotify_status_t st;
		memset(&st, 0, sizeof(st));
		pthread_mutex_unlock(&mock_lock);
		spotifymock_status(&st);
		if (out && out_size) {
			snprintf(out, out_size,
					 "STATE state=%d pos=%d dur=%d shuffle=%d repeat=%d volume=%d index=%d count=%d\n"
					 "TRACK %s\t%s\t%s\t%s\t\t%d\n",
					 (int)st.state, st.position_ms, st.track.duration_ms, st.shuffle ? 1 : 0, st.repeat, st.volume,
					 st.track.index, st.track.count, st.track.id, st.track.title, st.track.artist, st.track.album,
					 st.track.duration_ms);
		}
		return true;
	} else {
		pthread_mutex_unlock(&mock_lock);
		return false;
	}

	pthread_mutex_unlock(&mock_lock);
	return true;
}
