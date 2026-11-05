// test_podcastplayed.c -- the heard-episode store against the real SQLite.
//
// The list of episodes a listener has finished is what tells a podcast page
// which rows to dim and tick, so it has three jobs that are easy to get subtly
// wrong: keep the set sorted (the lookup is a binary search over it), keep it
// across a restart (the database is the only copy once the player goes off),
// and keep it honest when the card is swapped under the player.
//
// The database here is the real one, on a temporary card; the only thing
// stubbed is the file identity helper, which lives in utils.c and drags in the
// crash handler's platform code. Its contract -- deleted or replaced counts as
// changed -- is reproduced with stat().

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/utils.h"
#include "src/system/db/sqlite3.h"
#include "src/system/streaming/podcastplayed.h"

static int failures;

#define CHECK(cond)                                                                             \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			failures++;                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
		}                                                                                       \
	} while (0)

// ---------------------------------------------------------------------------
// utils.c's file identity, without the rest of utils.c
// ---------------------------------------------------------------------------

void file_identity_read(const char *path, file_identity_t *out) {
	memset(out, 0, sizeof(*out));
	struct stat st;
	if (stat(path, &st) == 0) {
		out->exists = true;
		out->dev = st.st_dev;
		out->ino = st.st_ino;
	}
}

bool file_identity_changed(const char *path, const file_identity_t *was) {
	file_identity_t now;
	file_identity_read(path, &now);
	if (now.exists != was->exists) {
		return true;
	}
	if (!now.exists) {
		return false;
	}
	return now.dev != was->dev || now.ino != was->ino;
}

// ---------------------------------------------------------------------------
// the card
// ---------------------------------------------------------------------------

static char card[512];

static int rows_in_db(void) {
	char path[600];
	snprintf(path, sizeof(path), "%s/.local/podcast.db", card);
	sqlite3 *db = NULL;
	if (sqlite3_open(path, &db) != SQLITE_OK) {
		sqlite3_close(db);
		return -1;
	}
	sqlite3_stmt *stmt = NULL;
	int rows = -1;
	if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM PODCAST_PLAYED", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			rows = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}
	sqlite3_close(db);
	return rows;
}

// A fresh player: whatever the store has in memory is dropped, and the card is
// read again from disk the way it is at boot.
static void restart(void) {
	podcastplayed_set_root(NULL);
	podcastplayed_set_root(card);
}

int main(void) {
	const char *tmp = getenv("TMPDIR");
	if (!tmp || !*tmp) {
		tmp = getenv("TEMP");
	}
	if (!tmp || !*tmp) {
		tmp = ".";
	}
	snprintf(card, sizeof(card), "%s/sonix-played-%ld", tmp, (long)getpid());
	// A directory of its own, and a database left by an earlier run of the same
	// process number does not get to answer for this one.
	{
		char stale[600];
		snprintf(stale, sizeof(stale), "%s/.local/podcast.db", card);
		remove(stale);
	}
	mkdir(card, 0700);
	struct stat made;
	if (stat(card, &made) != 0) {
		fprintf(stderr, "FAIL: cannot make a temporary card under %s\n", tmp);
		return 1;
	}

	// A new card. The schema is made when the card is attached -- podcastsubs
	// makes the same database at boot -- but no episode is marked, and asking
	// about one leaves it that way.
	podcastplayed_set_root(card);
	CHECK(!podcastplayed_get(16795090626LL));
	CHECK(rows_in_db() == 0);

	// 64-bit ids, because that is what the catalogue hands out and a 32-bit
	// store would collapse them onto one value.
	podcastplayed_set(16795090626LL, true);
	CHECK(podcastplayed_get(16795090626LL));
	CHECK(rows_in_db() == 1);

	// Marking again is not a second row: the tick is a fact, not a counter.
	podcastplayed_set(16795090626LL, true);
	CHECK(rows_in_db() == 1);

	// Across a restart: the database is the only copy.
	restart();
	CHECK(podcastplayed_get(16795090626LL));
	CHECK(!podcastplayed_get(16795090627LL));

	// Unmarking removes the row, and the removal survives a restart too.
	podcastplayed_set(16795090626LL, true);
	podcastplayed_set(16795090626LL, false);
	CHECK(!podcastplayed_get(16795090626LL));
	CHECK(rows_in_db() == 0);
	restart();
	CHECK(!podcastplayed_get(16795090626LL));

	// Unmarking something never marked is a no-op, not a write and not a crash.
	podcastplayed_set(99999999999LL, false);
	CHECK(rows_in_db() == 0);

	// An id that names no episode is refused by both doors: there is nothing to
	// keep the mark under, and a row for 0 would be one every hand-copied file
	// shares.
	podcastplayed_set(0, true);
	podcastplayed_set(-7, true);
	CHECK(!podcastplayed_get(0));
	CHECK(rows_in_db() == 0);

	// The set is kept sorted for a binary search, so it must survive being fed
	// in an order SQLite will not hand back: insert descending, read in random
	// order, then check every one after a restart as well.
	enum { MANY = 500 };
	for (int i = MANY; i >= 1; i--) {
		podcastplayed_set(20000000000LL + (long long)i * 7919, true);
	}
	for (int i = 1; i <= MANY; i++) {
		CHECK(podcastplayed_get(20000000000LL + (long long)i * 7919));
	}
	CHECK(rows_in_db() == MANY);
	restart();
	for (int i = MANY; i >= 1; i--) {
		CHECK(podcastplayed_get(20000000000LL + (long long)i * 7919));
	}
	// ...and one that was never in it still is not, between two that were.
	CHECK(!podcastplayed_get(20000000000LL + 3 * 7919 + 1));

	// A card pulled out and put back: the database is gone, so the marks are
	// gone -- the store must notice rather than answering from the old set.
	char path[600];
	snprintf(path, sizeof(path), "%s/.local/podcast.db", card);
	CHECK(remove(path) == 0);
	podcastplayed_reload_if_replaced();
	CHECK(!podcastplayed_get(20000000000LL + 1 * 7919));
	CHECK(!podcastplayed_get(20000000000LL + MANY * 7919));

	// And the folder the delete took with it is made again, so the next mark has
	// somewhere to land.
	podcastplayed_set(30000000001LL, true);
	CHECK(podcastplayed_get(30000000001LL));
	restart();
	CHECK(podcastplayed_get(30000000001LL));

	// A path with no card behind it is not an error: the store simply has
	// nothing, which is what a player with no card has.
	podcastplayed_set_root(NULL);
	CHECK(!podcastplayed_get(30000000001LL));

	if (failures == 0) {
		printf("all podcastplayed checks passed\n");
	}
	return failures == 0 ? 0 : 1;
}
