// test_mostplayed.c -- the Most played list against the real library database.
//
// The regression this pins: the row-id pass of library_index_open() built its
// rows query as "rowid, sortkey" whenever the sort keys were in force -- and
// PLAY_COUNTS has no sortkey column. The statement failed to prepare ("no
// such column: sortkey"), so the handle answered zero rows and Most played
// opened on its "nothing to show" note while every play was counted and every
// other query for the same list was right. Favourites and playlists were
// already excluded from that rule; Most played was the one list whose table
// carries neither a sortkey nor a name order, and it was left out.
//
// The suite links the real library.c and the real SQLite: it opens a
// database on a temporary card, records plays the way device_state.c does at
// track start, and reads the list back the way the media list page and the
// queue builder do. On a fresh database the sort keys are in force from
// library_open() onward, which is the state the bug lived in.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/db/sqlite3.h"
#include "src/system/library/cue.h"
#include "src/system/library/library.h"
#include "src/system/decode/mp4.h"
#include "src/system/decode/apedec.h"
#include "src/system/playback/playlist.h"

static int failures;

#define CHECK(cond)                                                                             \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			failures++;                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
		}                                                                                       \
	} while (0)

// -----------------------------------------------------------------------
// The scan's file-type probes, stubbed: this suite records plays by hand and
// never walks a card. Everything under test -- the schema, the upsert, the
// list queries, the row-id windows -- is the real thing.
// -----------------------------------------------------------------------

bool cue_parse(const char *cue_path, cue_sheet_t *out) {
	(void)cue_path;
	(void)out;
	return false;
}

bool cue_is_sheet(const char *path) {
	(void)path;
	return false;
}

bool cue_wav_has_markers(const char *path) {
	(void)path;
	return false;
}

void cue_virtual_path(const char *cue_path, int track_number, char *out, size_t size) {
	(void)cue_path;
	(void)track_number;
	if (size > 0) {
		out[0] = '\0';
	}
}

mp4_codec_t mp4_audio_codec(const mp4_file_t *m) {
	(void)m;
	return MP4_CODEC_NONE;
}

const unsigned char *mp4_audio_config(const mp4_file_t *m, int *len) {
	(void)m;
	if (len) {
		*len = 0;
	}
	return NULL;
}

int mp4_audio_sample_rate(const mp4_file_t *m) {
	(void)m;
	return 0;
}

bool mp4_file_has_video(const char *path) {
	(void)path;
	return false;
}

bool apedec_probe(const char *filepath, int *rate, int *bits) {
	(void)filepath;
	if (rate) {
		*rate = 0;
	}
	if (bits) {
		*bits = 0;
	}
	return false;
}

bool playlist_is_junk_name(const char *name) {
	(void)name;
	return false;
}

// -----------------------------------------------------------------------

// What library_index_window() hands back, kept for the checks below.
typedef struct {
	int count;
	char name[8][256];
	char path[8][512];
	char artist[8][256];
} rows_t;

static bool collect_cb(const char *name, const char *path, const char *artist, void *user) {
	rows_t *rows = user;
	if (rows->count >= 8) {
		return false;
	}
	int i = rows->count++;
	snprintf(rows->name[i], sizeof(rows->name[i]), "%s", name ? name : "");
	snprintf(rows->path[i], sizeof(rows->path[i]), "%s", path ? path : "");
	snprintf(rows->artist[i], sizeof(rows->artist[i]), "%s", artist ? artist : "");
	return true;
}

// A record of the plays device_state.c makes at track start.
static void record(const char *path, const char *name, const char *artist) {
	library_record_play(path, name, artist);
}

static void test_records_and_lists(void) {
	// Two plays of the first track, one of the second: the list is the
	// counts in descending order.
	record("/card/album/01.mp3", "Track One", "Artist A");
	record("/card/album/01.mp3", "Track One", "Artist A");
	record("/card/album/02.mp3", "Track Two", "Artist A");

	CHECK(library_play_count("/card/album/01.mp3") == 2);
	CHECK(library_play_count("/card/album/02.mp3") == 1);
	CHECK(library_play_count("/card/album/99.mp3") == 0);

	// The page's own path: the handle the media list opens. Before the fix
	// this came back with zero rows on any database with the sort keys in
	// force -- which is every ordinary boot.
	library_index_t *ix =
		library_index_open(LIBRARY_LIST_MOST_PLAYED, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
	CHECK(ix != NULL);
	CHECK(library_index_count(ix) == 2);

	rows_t rows = {0};
	CHECK(library_index_window(ix, 0, 8, collect_cb, &rows) == 2);
	CHECK(rows.count == 2);
	CHECK(strcmp(rows.name[0], "Track One") == 0);
	CHECK(strcmp(rows.path[0], "/card/album/01.mp3") == 0);
	CHECK(strcmp(rows.artist[0], "Artist A") == 0);
	CHECK(strcmp(rows.name[1], "Track Two") == 0);
	CHECK(strcmp(rows.path[1], "/card/album/02.mp3") == 0);

	// The queue builder reads the same list through the streaming reader;
	// the two answers have to agree row for row.
	rows_t streamed = {0};
	CHECK(library_for_each_ordered(LIBRARY_LIST_MOST_PLAYED, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT,
								  collect_cb, &streamed) == 2);
	CHECK(streamed.count == 2);
	CHECK(strcmp(streamed.name[0], rows.name[0]) == 0);
	CHECK(strcmp(streamed.path[0], rows.path[0]) == 0);
	CHECK(strcmp(streamed.name[1], rows.name[1]) == 0);
	CHECK(strcmp(streamed.path[1], rows.path[1]) == 0);

	library_index_close(ix);
}

static void test_order_follows_counts(void) {
	uint32_t before = library_play_counts_revision();

	// A second play of the other track ties them at two, and the newer one
	// comes first.
	record("/card/album/02.mp3", "Track Two", "Artist A");
	CHECK(library_play_counts_revision() != before);

	// A play is not a new list: the queue built from the old handle keeps
	// its row ids (see library_record_play), and the page notices through
	// the revision counter rather than through staleness.
	library_index_t *ix =
		library_index_open(LIBRARY_LIST_MOST_PLAYED, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
	CHECK(ix != NULL);
	CHECK(!library_index_stale(ix));
	CHECK(library_index_count(ix) == 2);

	rows_t rows = {0};
	CHECK(library_index_window(ix, 0, 8, collect_cb, &rows) == 2);
	CHECK(strcmp(rows.name[0], "Track Two") == 0);
	CHECK(strcmp(rows.name[1], "Track One") == 0);
	library_index_close(ix);
}

static void test_counts_survive_reopen(void) {
	// The user's own case: rescan and restart change nothing. The counts
	// live in their own table and come back with the database.
	library_close();
	CHECK(!library_is_open());

	char db[512];
	snprintf(db, sizeof(db), "%s", getenv("SONIX_TEST_DB"));
	CHECK(library_open(db));
	CHECK(library_is_open());

	CHECK(library_play_count("/card/album/01.mp3") == 2);
	CHECK(library_play_count("/card/album/02.mp3") == 2);

	library_index_t *ix =
		library_index_open(LIBRARY_LIST_MOST_PLAYED, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
	CHECK(ix != NULL);
	CHECK(library_index_count(ix) == 2);
	library_index_close(ix);
}

static void test_replay_from_start_counts_again(void) {
	// The list is a ranking of listens: a track played again from the top
	// moves up, and a row the player cannot attribute to a local file (an
	// empty path) is not a listen at all.
	uint32_t before = library_play_counts_revision();
	library_record_play("", "No Path", "Nobody");
	library_record_play(NULL, "No Path", "Nobody");
	CHECK(library_play_counts_revision() == before);

	record("/card/album/01.mp3", "Track One", "Artist A");
	CHECK(library_play_count("/card/album/01.mp3") == 3);

	library_index_t *ix =
		library_index_open(LIBRARY_LIST_MOST_PLAYED, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
	CHECK(ix != NULL);
	rows_t rows = {0};
	CHECK(library_index_window(ix, 0, 8, collect_cb, &rows) == 2);
	CHECK(strcmp(rows.name[0], "Track One") == 0); // 3 plays beats 2
	library_index_close(ix);
}

static void test_track_artists(const char *path) {
	char names[7][LIBRARY_ARTIST_NAME_MAX];
	library_close();
	sqlite3 *db = NULL;
	CHECK(sqlite3_open(path, &db) == SQLITE_OK);
	const char *sql =
		"INSERT INTO MEDIA_TABLE(id,path,artist) VALUES(100,'/single','Solo'),"
		"(101,'/split','First feat. Second'),(102,'/empty','');"
		"INSERT INTO ARTIST_LINK(path,artist) VALUES('/split','First'),('/split','Second');";
	CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(db);
	CHECK(library_open(path));
	CHECK(library_track_artists("/single", names, 7) == 1);
	CHECK(strcmp(names[0], "Solo") == 0);
	CHECK(library_track_artists("/split", names, 7) == 2);
	CHECK(strcmp(names[0], "First") == 0 && strcmp(names[1], "Second") == 0);
	CHECK(library_track_artists("/split", names, 1) == 1);
	CHECK(library_track_artists("/empty", names, 7) == 0);
	CHECK(library_track_artists("/missing", names, 7) == 0);
	CHECK(library_track_artists(NULL, names, 7) == 0);
}

int main(void) {
	char dir[] = "/tmp/sonix-mostplayed-test-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}

	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/device_config.ini", dir);
	config_init(config_path);

	// The "card": a database of its own in the temporary directory.
	char db[512];
	snprintf(db, sizeof(db), "%s/library.db", dir);
	setenv("SONIX_TEST_DB", db, 1);
	CHECK(library_open(db));
	CHECK(library_is_open());

	test_records_and_lists();
	test_order_follows_counts();
	test_counts_survive_reopen();
	test_replay_from_start_counts_again();
	test_track_artists(db);

	library_close();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("mostplayed: plays are counted and the list is in the order they were earned\n");
	return 0;
}
