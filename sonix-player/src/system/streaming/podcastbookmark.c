#include "podcastbookmark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/addons/addons.h"
#include "src/system/core/config.h"
#include "src/system/db/sqlite3.h"
#include "src/system/streaming/podcastdl.h"

// One row per episode file: <card>/.local/podcast-bookmarks.db.
//
// A database of its own rather than a table in podcast.db, because the
// followed podcasts and the heard marks are keyed by catalogue episode id and
// this is keyed by path -- a file copied onto the card by hand has an id of
// zero and still deserves to come back where it was left. The two kinds of
// key do not belong in one table.
//
// Opened for each call rather than held: the calls are a write every ten
// seconds at most and a read when an episode starts, and an open handle would
// have to notice the card being pulled and put back, which the other card
// databases already do at some length.

static char db_path[512];

static bool enabled(void) {
	// The module switch is what makes this feature exist at all; the config
	// key is the user's way of having the downloads without the bookmarks.
	return addons_enabled("podcastcatcher") && config_get_bool("podcast_catcher", "auto_bookmark", true);
}

static sqlite3 *open_db(void) {
	if (!db_path[0]) {
		return NULL;
	}
	sqlite3 *db = NULL;
	if (sqlite3_open(db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "podcastbookmark: cannot open %s: %s\n", db_path, db ? sqlite3_errmsg(db) : "?");
		sqlite3_close(db);
		return NULL;
	}
	char *error = NULL;
	if (sqlite3_exec(db,
					 "CREATE TABLE IF NOT EXISTS BOOKMARK("
					 "path TEXT PRIMARY KEY, seconds REAL, updated_at INT)",
					 NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "podcastbookmark: %s\n", error ? error : "unknown error");
		sqlite3_free(error);
		sqlite3_close(db);
		return NULL;
	}
	return db;
}

void podcastbookmark_set_root(const char *sd_root) {
	if (!sd_root || !sd_root[0]) {
		db_path[0] = '\0';
		return;
	}
	char dir[512];
	snprintf(dir, sizeof(dir), "%.480s/.local", sd_root);
	mkdir(dir, 0755);
	snprintf(db_path, sizeof(db_path), "%.480s/.local/podcast-bookmarks.db", sd_root);
}

// The last position written for the file on screen, so the ten-second
// checkpoint does not become ten seconds of card writes for a position that
// moved by less than the buffer it is worth saving.
static char last_path[512];
static double last_seconds = -1.0;

void podcastbookmark_note(const char *path, double seconds, double total) {
	if (!path || !path[0] || !enabled() || !podcastdl_owns(path)) {
		return;
	}
	// A file that has only just opened reports zero; writing that would throw
	// away the place the episode was actually left at.
	if (seconds < 2.0) {
		return;
	}
	// Within a few seconds of the end there is nothing to come back to.
	if (total > 0.0 && seconds >= total - 5.0) {
		podcastbookmark_clear(path);
		last_path[0] = '\0';
		last_seconds = -1.0;
		return;
	}
	if (strcmp(last_path, path) == 0 && last_seconds >= 0.0 && seconds - last_seconds < 5.0 &&
		last_seconds - seconds < 5.0) {
		return;
	}

	sqlite3 *db = open_db();
	if (!db) {
		return;
	}
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO BOOKMARK(path,seconds,updated_at)"
							  " VALUES(?1,?2,strftime('%s','now'))",
						   -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_bind_double(stmt, 2, seconds);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
		snprintf(last_path, sizeof(last_path), "%s", path);
		last_seconds = seconds;
	}
	sqlite3_close(db);
}

bool podcastbookmark_get(const char *path, double *seconds) {
	if (!path || !path[0] || !seconds || !enabled() || !podcastdl_owns(path)) {
		return false;
	}
	sqlite3 *db = open_db();
	if (!db) {
		return false;
	}
	bool found = false;
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT seconds FROM BOOKMARK WHERE path=?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			double at = sqlite3_column_double(stmt, 0);
			if (at > 1.0) {
				*seconds = at;
				found = true;
			}
		}
		sqlite3_finalize(stmt);
	}
	sqlite3_close(db);
	return found;
}

void podcastbookmark_clear(const char *path) {
	if (!path || !path[0] || !db_path[0] || !enabled() || !podcastdl_owns(path)) {
		return;
	}
	sqlite3 *db = open_db();
	if (!db) {
		return;
	}
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "DELETE FROM BOOKMARK WHERE path=?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	sqlite3_close(db);
	if (strcmp(last_path, path) == 0) {
		last_path[0] = '\0';
		last_seconds = -1.0;
	}
}
