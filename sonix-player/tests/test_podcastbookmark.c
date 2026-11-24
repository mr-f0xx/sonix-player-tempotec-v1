// test_podcastbookmark.c -- per-episode playback bookmarks against SQLite.
//
// The catcher's other half: every episode in the Podcast folder is remembered
// by path, so opening it again carries on where it was left. What has to hold:
// a position is saved, a small move does not rewrite the card, the last few
// seconds of an episode are not a place to come back to, a file that has only
// just opened does not erase the place it was left at, and a file outside the
// podcast folder is never bookmarked.
//
// The store is the real one on a temporary card. The module switch, the config
// key and the "is this in the Podcast folder" test are stubs, because those
// belong to the player and not to the store.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/db/sqlite3.h"
#include "src/system/streaming/podcastbookmark.h"

static int failures;

#define CHECK(cond)                                                                             \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			failures++;                                                                         \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
		}                                                                                       \
	} while (0)

// ---- the parts of the player the store asks, substituted --------------------

bool addons_enabled(const char *id) {
	(void)id;
	return true;
}

bool config_get_bool(const char *section, const char *key, bool fallback) {
	(void)section;
	(void)key;
	return fallback;
}

bool podcastdl_owns(const char *path) {
	return path && strstr(path, "/Podcast/") != NULL;
}

// ---- the card ---------------------------------------------------------------

static char card[512];

static bool bookmark_file_exists(void) {
	char path[600];
	snprintf(path, sizeof(path), "%s/.local/podcast-bookmarks.db", card);
	struct stat st;
	return stat(path, &st) == 0;
}

int main(void) {
	char template[] = "/tmp/sonix-bookmark-XXXXXX";
	char *made = mkdtemp(template);
	if (!made) {
		fprintf(stderr, "FAIL: cannot make a temporary card\n");
		return 1;
	}
	snprintf(card, sizeof(card), "%s", made);

	char episode[700];
	snprintf(episode, sizeof(episode), "%s/Podcast/Show/Episode 1.mp3", card);
	char other[700];
	snprintf(other, sizeof(other), "%s/Music/Album/Track 1.mp3", card);

	podcastbookmark_set_root(card);
	double at = -1.0;

	// Nothing yet: a new card, a new store. Asking is what opens the file.
	CHECK(!bookmark_file_exists());
	CHECK(!podcastbookmark_get(episode, &at));

	// A place is saved and read back.
	podcastbookmark_note(episode, 30.0, 3600.0);
	CHECK(podcastbookmark_get(episode, &at));
	CHECK(at > 29.9 && at < 30.1);
	CHECK(bookmark_file_exists());

	// A move of a couple of seconds is not worth a write, and the old place
	// still answers.
	podcastbookmark_note(episode, 33.0, 3600.0);
	CHECK(podcastbookmark_get(episode, &at));
	CHECK(at > 29.9 && at < 30.1);

	// A move of more than that is.
	podcastbookmark_note(episode, 40.0, 3600.0);
	CHECK(podcastbookmark_get(episode, &at));
	CHECK(at > 39.9 && at < 40.1);

	// The end of the episode is not a place to come back to.
	podcastbookmark_note(episode, 3598.0, 3600.0);
	CHECK(!podcastbookmark_get(episode, &at));

	// A file that has only just opened reports a second or two, and that must
	// not erase the place it was left at -- so it is ignored, not saved.
	podcastbookmark_note(episode, 500.0, 3600.0);
	podcastbookmark_note(episode, 1.0, 3600.0);
	CHECK(podcastbookmark_get(episode, &at));
	CHECK(at > 499.9 && at < 500.1);

	// A length the player does not know: any position is worth keeping.
	podcastbookmark_note(episode, 100.5, 0.0);
	CHECK(podcastbookmark_get(episode, &at));
	CHECK(at > 100.4 && at < 100.6);

	// Outside the podcast folder nothing is written.
	podcastbookmark_note(other, 30.0, 300.0);
	CHECK(!podcastbookmark_get(other, &at));

	// Even a row that somehow exists for another folder does not answer.
	{
		char path[600];
		snprintf(path, sizeof(path), "%s/.local/podcast-bookmarks.db", card);
		sqlite3 *db = NULL;
		if (sqlite3_open(path, &db) == SQLITE_OK) {
			sqlite3_exec(db, "INSERT OR REPLACE INTO BOOKMARK(path,seconds,updated_at) VALUES('no-such-path',55,0)",
						 NULL, NULL, NULL);
			sqlite3_close(db);
		}
	}
	CHECK(!podcastbookmark_get("no-such-path", &at));

	// Clearing forgets it.
	podcastbookmark_clear(episode);
	CHECK(!podcastbookmark_get(episode, &at));

	// An empty root disables the store without touching the card.
	podcastbookmark_set_root("");
	CHECK(!podcastbookmark_get(episode, &at));

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("podcastbookmark: ok\n");
	return 0;
}
