// test_explicit.c -- the iTunes advisory flag from tag to list.
//
// The parser itself is covered in test_tags.c against real M4A fixtures. This
// suite covers the other half: the flag has to survive in the index, an album
// has to be marked when any one of its tracks is, and the marker the lists
// draw must not become part of the name they use to open a record.
//
// Plain C: the real library.c and SQLite, with only the file-type probes
// library.c does not need here stubbed out (as in test_mostplayed.c).

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/library/explicitmark.h"
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

static void test_marker(void) {
	char out[16];

	explicit_decorate(out, sizeof(out), "Anti", false);
	CHECK(strcmp(out, "Anti") == 0);
	explicit_decorate(out, sizeof(out), "Anti", true);
	CHECK(strcmp(out, "Anti" EXPLICIT_MARK) == 0);

	// A name that fills the buffer is shortened, not allowed to push the
	// marker past the end. A buffer too small for the mark just keeps the name.
	explicit_decorate(out, sizeof(out), "ABCDEFGHIJKLMNO", true);
	CHECK(strlen(out) < sizeof(out));
	CHECK(strstr(out, EXPLICIT_MARK) != NULL);
	explicit_decorate(out, 3, "Anti", true);
	CHECK(strcmp(out, "An") == 0);
	explicit_decorate(out, sizeof(out), NULL, false);
	CHECK(out[0] == '\0');
}

static void write_rows(const char *db_path) {
	sqlite3 *db = NULL;
	CHECK(sqlite3_open(db_path, &db) == SQLITE_OK);
	if (!db) {
		return;
	}
	// Two records of the same name, one explicit: the album value carries a
	// key and the two must not be told apart by the marker.
	const char *sql =
		"INSERT INTO MEDIA_TABLE(path,name,album,artist,album_artist,explicit,album_key) VALUES"
		"('/card/Anti/01.m4a','BBHMM','Anti','Rihanna','Rihanna',1,'key-a'),"
		"('/card/Anti/02.m4a','Clean Song','Anti','Rihanna','Rihanna',0,'key-a'),"
		"('/card/Other/Anti/01.m4a','Other Song','Anti','Someone','Someone',0,'key-b'),"
		"('/card/Clean/01.m4a','Clean Track','Clean','Somebody','Somebody',0,'key-c');";
	CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(db);
}

static void test_index(const char *db_path) {
	CHECK(library_open(db_path));
	CHECK(library_is_open());

	CHECK(library_track_explicit("/card/Anti/01.m4a"));
	CHECK(!library_track_explicit("/card/Anti/02.m4a"));
	CHECK(!library_track_explicit("/card/Clean/01.m4a"));
	CHECK(!library_track_explicit("/card/no-such-file.m4a"));
	CHECK(!library_track_explicit(NULL));

	char value[64];
	snprintf(value, sizeof(value), "Anti%ckey-a", LIBRARY_ALBUM_KEY_SEP);
	CHECK(library_album_explicit(value)); // one track of the record is enough
	snprintf(value, sizeof(value), "Anti%ckey-b", LIBRARY_ALBUM_KEY_SEP);
	CHECK(!library_album_explicit(value));
	snprintf(value, sizeof(value), "Clean%ckey-c", LIBRARY_ALBUM_KEY_SEP);
	CHECK(!library_album_explicit(value));
	CHECK(library_album_explicit("Anti")); // an old, key-less value
	CHECK(!library_album_explicit("Nothing"));
	CHECK(!library_album_explicit(NULL));

	library_close();
}

static void test_older_database(void) {
	// A card indexed by a build without the column still opens; those tracks
	// are simply unmarked until the next scan reads their tags again.
	char dir[] = "/tmp/sonix-explicit-old-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	char path[600];
	snprintf(path, sizeof(path), "%s/library.db", dir);

	sqlite3 *db = NULL;
	CHECK(sqlite3_open(path, &db) == SQLITE_OK);
	if (db) {
		CHECK(sqlite3_exec(db,
						   "CREATE TABLE MEDIA_TABLE(id INT,path TEXT,name TEXT,album TEXT,artist TEXT,"
						   "album_artist TEXT,year INT,genre TEXT,sortkey TEXT,cue_id INT,"
						   "PRIMARY KEY(id,path,cue_id));"
						   "INSERT INTO MEDIA_TABLE(path,name,album) VALUES('/old.m4a','Old','Old Album');",
						   NULL, NULL, NULL) == SQLITE_OK);
		sqlite3_close(db);
	}

	CHECK(library_open(path));
	char title[256];
	CHECK(library_track_title("/old.m4a", title, sizeof(title)));
	CHECK(strcmp(title, "Old") == 0);
	CHECK(!library_track_explicit("/old.m4a"));
	CHECK(!library_album_explicit("Old Album"));
	library_close();

	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	CHECK(system(cmd) == 0);
}

int main(void) {
	char dir[] = "/tmp/sonix-explicit-test-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);

	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/device_config.ini", dir);
	config_init(config_path);
	test_marker();

	char db_path[512];
	snprintf(db_path, sizeof(db_path), "%s/library.db", dir);
	CHECK(library_open(db_path)); // creates the schema
	library_close();
	write_rows(db_path);
	test_index(db_path);
	test_older_database();

	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	CHECK(system(cmd) == 0);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("explicit: advisory survives the index, marks a record, and stays out of its name");
	return 0;
}
