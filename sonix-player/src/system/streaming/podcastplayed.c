#include "podcastplayed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/utils.h"
#include "src/system/db/sqlite3.h"

// Heard podcast episodes, in the database the followed podcasts already use:
// <card>/.local/podcast.db.
//
// A table of its own rather than a column on PODCAST_SUBS: that one is per
// podcast, this one is per episode, and an episode belongs to a podcast the
// user may never have followed. It also means the follow star and the played
// marks cannot break each other's writes.
//
// See the header for why the set is held in memory and keyed by episode id.

// A ceiling on what a corrupt or hand-inflated database can take out of the
// RAM playback needs. Ten thousand episodes is several years of listening to
// several podcasts; past it the oldest reads are dropped rather than the newest
// refused, and the log says so.
#define PLAYED_MAX 10000

static char db_path[512];
// Which file the set in memory was last read from or written to.
static file_identity_t db_identity;

// Sorted, so a lookup is a binary search and a change is one memmove.
static long long *ids;
static int id_count;
static int id_cap;

static bool exec(sqlite3 *db, const char *sql) {
	char *error = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "podcastplayed: %s\n", error ? error : "unknown error");
		sqlite3_free(error);
		return false;
	}
	return true;
}

static sqlite3 *open_db(void) {
	if (!db_path[0]) {
		return NULL;
	}
	sqlite3 *db = NULL;
	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "podcastplayed: cannot open %s: %s\n", db_path, sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}
	// The same choices as the library and audiobook databases, for the same
	// reason: a slow card that can be pulled at any moment.
	exec(db, "PRAGMA synchronous=NORMAL");
	exec(db, "PRAGMA journal_mode=TRUNCATE");
	exec(db, "PRAGMA cache_size=-64");
	exec(db, "CREATE TABLE IF NOT EXISTS PODCAST_PLAYED(episode_id INTEGER PRIMARY KEY)");
	return db;
}

// ---------------------------------------------------------------------------
// the set in memory
// ---------------------------------------------------------------------------

static void set_clear(void) {
	id_count = 0;
}

// Where `id` is, or would be.
static int lower_bound(long long id) {
	int lo = 0, hi = id_count;
	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;
		if (ids[mid] < id) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

static bool set_has(long long id) {
	int at = lower_bound(id);
	return at < id_count && ids[at] == id;
}

// Room for one more, wherever the caller is about to put it. False when the
// ceiling is reached and nothing may be added.
static bool set_room(void) {
	if (id_count < id_cap) {
		return true;
	}
	if (id_cap >= PLAYED_MAX) {
		return false;
	}
	int cap = id_cap ? id_cap * 2 : 64;
	if (cap > PLAYED_MAX) {
		cap = PLAYED_MAX;
	}
	long long *grown = realloc(ids, sizeof(*ids) * (size_t)cap);
	if (!grown) {
		return false;
	}
	ids = grown;
	id_cap = cap;
	return true;
}

static bool set_insert(long long id) {
	int at = lower_bound(id);
	if (at < id_count && ids[at] == id) {
		return true;
	}
	if (!set_room()) {
		fprintf(stderr, "podcastplayed: %d episodes already marked; this one cannot be kept\n", id_count);
		return false;
	}
	memmove(&ids[at + 1], &ids[at], sizeof(*ids) * (size_t)(id_count - at));
	ids[at] = id;
	id_count++;
	return true;
}

static void set_remove(long long id) {
	int at = lower_bound(id);
	if (at >= id_count || ids[at] != id) {
		return;
	}
	memmove(&ids[at], &ids[at + 1], sizeof(*ids) * (size_t)(id_count - at - 1));
	id_count--;
}

// ---------------------------------------------------------------------------
// the database
// ---------------------------------------------------------------------------

static void load(void) {
	set_clear();
	sqlite3 *db = open_db();
	if (!db) {
		return;
	}
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT episode_id FROM PODCAST_PLAYED ORDER BY episode_id", -1, &stmt, NULL) ==
		SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			long long id = (long long)sqlite3_column_int64(stmt, 0);
			if (id <= 0) {
				continue;
			}
			if (!set_room()) {
				fprintf(stderr, "podcastplayed: more than %d episodes marked; the rest are ignored\n", PLAYED_MAX);
				break;
			}
			ids[id_count++] = id;
		}
		sqlite3_finalize(stmt);
	}
	sqlite3_close(db);
	file_identity_read(db_path, &db_identity);
}

// One row, written the moment the set changed: a synced card write of a few
// bytes, which is what "remember this" has to cost.
static void save_one(long long id, bool played) {
	sqlite3 *db = open_db();
	if (!db) {
		return;
	}
	sqlite3_stmt *stmt = NULL;
	const char *sql = played ? "INSERT OR IGNORE INTO PODCAST_PLAYED(episode_id) VALUES(?)"
							 : "DELETE FROM PODCAST_PLAYED WHERE episode_id=?";
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
		if (sqlite3_step(stmt) != SQLITE_DONE) {
			fprintf(stderr, "podcastplayed: cannot write episode %lld: %s\n", id, sqlite3_errmsg(db));
		}
		sqlite3_finalize(stmt);
	}
	sqlite3_close(db);
	// The write made this process the file's author: the next reload compares
	// against what it now is, not against a file that changed under it.
	file_identity_read(db_path, &db_identity);
}

// ---------------------------------------------------------------------------
// the interface
// ---------------------------------------------------------------------------

void podcastplayed_set_root(const char *sd_root) {
	if (!sd_root || !*sd_root) {
		db_path[0] = '\0';
		set_clear();
		return;
	}
	// The same hidden folder as everything else: the podcast cache, the
	// library, the followed list.
	char dir[512];
	snprintf(dir, sizeof(dir), "%.480s/.local", sd_root);
	mkdir(dir, 0777);
	char next[512];
	snprintf(next, sizeof(next), "%.480s/podcast.db", dir);
	if (strcmp(next, db_path) == 0) {
		return; // the same card: the set in memory is already that card's
	}
	snprintf(db_path, sizeof(db_path), "%s", next);
	load();
}

void podcastplayed_reload_if_replaced(void) {
	if (!db_path[0] || !file_identity_changed(db_path, &db_identity)) {
		return;
	}
	printf("podcastplayed: %s was deleted or replaced; reading it again\n", db_path);
	// The folder may have gone with it.
	char dir[sizeof(db_path)];
	snprintf(dir, sizeof(dir), "%s", db_path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0777);
	}
	load();
}

bool podcastplayed_get(long long episode_id) {
	if (episode_id <= 0) {
		return false;
	}
	return set_has(episode_id);
}

void podcastplayed_set(long long episode_id, bool played) {
	if (episode_id <= 0) {
		return;
	}
	if (played) {
		if (set_has(episode_id)) {
			return; // already there: no write for a mark that is already true
		}
		if (!set_insert(episode_id)) {
			return; // nowhere to keep it: the database must not disagree
		}
	} else {
		if (!set_has(episode_id)) {
			return;
		}
		set_remove(episode_id);
	}
	save_one(episode_id, played);
}
