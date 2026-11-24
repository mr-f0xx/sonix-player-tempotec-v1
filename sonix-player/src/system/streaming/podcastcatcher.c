#include "podcastcatcher_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/db/sqlite3.h"

// feeds.txt and the state the catcher keeps between checks.
//
// Both are here rather than in the worker because both are worth testing
// without a network: a file with comments and blank lines has to be read the
// same way on the device and on the bench, and "which episodes are new" is the
// one decision that can fill a card if it is wrong. The worker is the thin
// part on top -- fetch, parse, download.

// ---------------------------------------------------------------------------
// feeds.txt
// ---------------------------------------------------------------------------

static void trim(char *text) {
	size_t start = 0;
	while (text[start] && isspace((unsigned char)text[start])) {
		start++;
	}
	size_t end = strlen(text);
	while (end > start && isspace((unsigned char)text[end - 1])) {
		end--;
	}
	if (start) {
		memmove(text, text + start, end - start);
	}
	text[end - start] = '\0';
}

int podcastcatcher_parse_feeds(const char *text, podcastcatcher_feed_t *out, int max) {
	if (!text || !out || max <= 0) {
		return 0;
	}
	// A file written in Notepad starts with a UTF-8 byte-order mark, which
	// would otherwise make the first address start with three bytes that are
	// not "http".
	if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
		text += 3;
	}
	int count = 0;
	const char *p = text;
	while (*p && count < max) {
		const char *line = p;
		while (*p && *p != '\n') {
			p++;
		}
		size_t len = (size_t)(p - line);
		if (*p == '\n') {
			p++;
		}
		char buf[PODCAST_URL_MAX];
		if (len >= sizeof(buf)) {
			continue; // longer than any address this could fetch; not a line to guess at
		}
		memcpy(buf, line, len);
		buf[len] = '\0';
		trim(buf);
		if (!buf[0] || buf[0] == '#' || buf[0] == ';') {
			continue;
		}
		if (strncasecmp(buf, "http://", 7) != 0 && strncasecmp(buf, "https://", 8) != 0) {
			continue; // a title, a note, anything that is not an address
		}
		snprintf(out[count].url, sizeof(out[count].url), "%s", buf);
		count++;
	}
	return count;
}

// ---------------------------------------------------------------------------
// the state database
// ---------------------------------------------------------------------------

struct podcastcatcher_state {
	sqlite3 *db;
	char path[512];
};

// status in CATCHER_EPISODE
#define STATUS_SEEN 0	   // recorded, and deliberately not taken
#define STATUS_DOWNLOADED 1
#define STATUS_FAILED 2	   // with attempts counting towards the limit
#define STATUS_QUEUED 3	   // taken, download not finished yet

static bool exec(sqlite3 *db, const char *sql) {
	char *error = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "podcastcatcher: %s\n", error ? error : "unknown error");
		sqlite3_free(error);
		return false;
	}
	return true;
}

podcastcatcher_state_t *podcastcatcher_state_open(const char *sd_root) {
	if (!sd_root || !sd_root[0]) {
		return NULL;
	}
	char dir[512];
	snprintf(dir, sizeof(dir), "%.480s/.local", sd_root);
	mkdir(dir, 0755); // already there on a card the player has used; harmless

	podcastcatcher_state_t *st = calloc(1, sizeof(*st));
	if (!st) {
		return NULL;
	}
	snprintf(st->path, sizeof(st->path), "%.480s/.local/podcast-catcher.db", sd_root);
	if (sqlite3_open(st->path, &st->db) != SQLITE_OK) {
		fprintf(stderr, "podcastcatcher: cannot open %s: %s\n", st->path, st->db ? sqlite3_errmsg(st->db) : "?");
		sqlite3_close(st->db);
		free(st);
		return NULL;
	}
	// The same choices as the other card databases, for the same reason: a
	// slow card that can be pulled at any moment.
	exec(st->db, "PRAGMA synchronous=NORMAL");
	exec(st->db, "PRAGMA journal_mode=TRUNCATE");
	exec(st->db, "PRAGMA cache_size=-64");
	exec(st->db, "CREATE TABLE IF NOT EXISTS CATCHER_FEED("
				 "url TEXT PRIMARY KEY, title TEXT, author TEXT, image TEXT, added_at INTEGER DEFAULT 0)");
	exec(st->db, "CREATE TABLE IF NOT EXISTS CATCHER_EPISODE("
				 "feed_url TEXT, episode_id INTEGER, title TEXT, published INTEGER,"
				 "status INTEGER DEFAULT 0, attempts INTEGER DEFAULT 0,"
				 "PRIMARY KEY(feed_url, episode_id))");
	return st;
}

void podcastcatcher_state_close(podcastcatcher_state_t *st) {
	if (!st) {
		return;
	}
	if (st->db) {
		sqlite3_close(st->db);
	}
	free(st);
}

bool podcastcatcher_state_feed(podcastcatcher_state_t *st, const char *url, const char *title,
							   const char *author, const char *image, bool *first_out) {
	if (!st || !st->db || !url || !url[0]) {
		return false;
	}
	bool first = true;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(st->db, "SELECT 1 FROM CATCHER_FEED WHERE url=?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			first = false;
		}
		sqlite3_finalize(stmt);
	}

	bool ok;
	if (first) {
		ok = sqlite3_prepare_v2(st->db,
								"INSERT INTO CATCHER_FEED(url,title,author,image,added_at)"
								" VALUES(?1,?2,?3,?4,strftime('%s','now'))",
								-1, &stmt, NULL) == SQLITE_OK;
	} else {
		ok = sqlite3_prepare_v2(st->db, "UPDATE CATCHER_FEED SET title=?2,author=?3,image=?4 WHERE url=?1", -1,
								&stmt, NULL) == SQLITE_OK;
	}
	if (ok) {
		sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, title ? title : "", -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 3, author ? author : "", -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 4, image ? image : "", -1, SQLITE_STATIC);
		ok = sqlite3_step(stmt) == SQLITE_DONE;
	}
	if (stmt) {
		sqlite3_finalize(stmt);
	}
	if (first_out) {
		*first_out = first;
	}
	return ok;
}

int podcastcatcher_state_take_new(podcastcatcher_state_t *st, const char *feed_url, bool first,
								  const podcast_episode_t *episodes, int count, int limit, int *indexes) {
	if (!st || !st->db || !feed_url || !feed_url[0] || !episodes || count <= 0 || limit <= 0 || !indexes) {
		return 0;
	}

	sqlite3_stmt *sel = NULL;
	sqlite3_stmt *ins = NULL;
	sqlite3_stmt *upd = NULL;
	bool prepared =
		sqlite3_prepare_v2(st->db, "SELECT status,attempts FROM CATCHER_EPISODE WHERE feed_url=?1 AND episode_id=?2",
						   -1, &sel, NULL) == SQLITE_OK &&
		sqlite3_prepare_v2(st->db,
						   "INSERT OR IGNORE INTO CATCHER_EPISODE(feed_url,episode_id,title,published,status,attempts)"
						   " VALUES(?1,?2,?3,?4,?5,0)",
						   -1, &ins, NULL) == SQLITE_OK &&
		sqlite3_prepare_v2(st->db, "UPDATE CATCHER_EPISODE SET status=?3 WHERE feed_url=?1 AND episode_id=?2", -1,
						   &upd, NULL) == SQLITE_OK;

	int taken = 0;
	if (prepared) {
		for (int i = 0; i < count; i++) {
			const podcast_episode_t *e = &episodes[i];
			if (e->id <= 0) {
				continue;
			}

			int status = -1;
			int attempts = 0;
			sqlite3_reset(sel);
			sqlite3_bind_text(sel, 1, feed_url, -1, SQLITE_STATIC);
			sqlite3_bind_int64(sel, 2, e->id);
			if (sqlite3_step(sel) == SQLITE_ROW) {
				status = sqlite3_column_int(sel, 0);
				attempts = sqlite3_column_int(sel, 1);
			}

			// Already on the card, or seen on an earlier check and passed over:
			// not new, and taking it now would be a surprise.
			if (status == STATUS_DOWNLOADED || status == STATUS_SEEN) {
				continue;
			}
			// A failure is worth another try, but not for ever.
			if (status == STATUS_FAILED && attempts >= PODCASTCATCHER_MAX_ATTEMPTS) {
				continue;
			}

			if (taken >= limit) {
				// The first check records what it passed over, so a feed that
				// was added with years of back catalogue does not offer the
				// same old episodes again next time.
				if (first && status < 0) {
					sqlite3_reset(ins);
					sqlite3_bind_text(ins, 1, feed_url, -1, SQLITE_STATIC);
					sqlite3_bind_int64(ins, 2, e->id);
					sqlite3_bind_text(ins, 3, e->title, -1, SQLITE_STATIC);
					sqlite3_bind_int64(ins, 4, e->published);
					sqlite3_bind_int(ins, 5, STATUS_SEEN);
					sqlite3_step(ins);
				}
				continue;
			}

			if (status < 0) {
				sqlite3_reset(ins);
				sqlite3_bind_text(ins, 1, feed_url, -1, SQLITE_STATIC);
				sqlite3_bind_int64(ins, 2, e->id);
				sqlite3_bind_text(ins, 3, e->title, -1, SQLITE_STATIC);
				sqlite3_bind_int64(ins, 4, e->published);
				sqlite3_bind_int(ins, 5, STATUS_QUEUED);
				sqlite3_step(ins);
			} else {
				sqlite3_reset(upd);
				sqlite3_bind_text(upd, 1, feed_url, -1, SQLITE_STATIC);
				sqlite3_bind_int64(upd, 2, e->id);
				sqlite3_bind_int(upd, 3, STATUS_QUEUED);
				sqlite3_step(upd);
			}
			indexes[taken++] = i;
		}
	}

	if (sel) {
		sqlite3_finalize(sel);
	}
	if (ins) {
		sqlite3_finalize(ins);
	}
	if (upd) {
		sqlite3_finalize(upd);
	}
	return taken;
}

static bool episode_status_set(podcastcatcher_state_t *st, const char *feed_url, long long episode_id, int status,
							   bool count_attempt) {
	if (!st || !st->db || !feed_url) {
		return false;
	}
	sqlite3_stmt *stmt = NULL;
	bool ok = false;
	if (sqlite3_prepare_v2(st->db, "UPDATE CATCHER_EPISODE SET status=?3, attempts=attempts+?4"
								   " WHERE feed_url=?1 AND episode_id=?2",
						   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, feed_url, -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt, 2, episode_id);
		sqlite3_bind_int(stmt, 3, status);
		sqlite3_bind_int(stmt, 4, count_attempt ? 1 : 0);
		ok = sqlite3_step(stmt) == SQLITE_DONE;
		sqlite3_finalize(stmt);
	}
	return ok;
}

void podcastcatcher_state_downloaded(podcastcatcher_state_t *st, const char *feed_url, long long episode_id) {
	episode_status_set(st, feed_url, episode_id, STATUS_DOWNLOADED, false);
}

void podcastcatcher_state_failed(podcastcatcher_state_t *st, const char *feed_url, long long episode_id) {
	episode_status_set(st, feed_url, episode_id, STATUS_FAILED, true);
}

void podcastcatcher_state_prune(podcastcatcher_state_t *st, const podcastcatcher_feed_t *feeds, int count) {
	if (!st || !st->db) {
		return;
	}
	char gone[PODCASTCATCHER_MAX_FEEDS][PODCAST_URL_MAX];
	int gone_count = 0;

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(st->db, "SELECT url FROM CATCHER_FEED", -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW && gone_count < PODCASTCATCHER_MAX_FEEDS) {
			const unsigned char *url = sqlite3_column_text(stmt, 0);
			if (!url || !url[0]) {
				continue;
			}
			bool kept = false;
			for (int i = 0; i < count; i++) {
				if (strcmp((const char *)url, feeds[i].url) == 0) {
					kept = true;
					break;
				}
			}
			if (!kept) {
				snprintf(gone[gone_count], sizeof(gone[gone_count]), "%s", (const char *)url);
				gone_count++;
			}
		}
		sqlite3_finalize(stmt);
	}

	for (int i = 0; i < gone_count; i++) {
		if (sqlite3_prepare_v2(st->db, "DELETE FROM CATCHER_EPISODE WHERE feed_url=?1", -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, gone[i], -1, SQLITE_STATIC);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(st->db, "DELETE FROM CATCHER_FEED WHERE url=?1", -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, gone[i], -1, SQLITE_STATIC);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}
	}
}
