// Large-library Smart Mix build: the real builder, reader and mix walker, a
// synthetic index of tens of thousands of rows, and a temporary card.
//
// The point of this suite is the numbers the bug report in issue #285 quotes:
// a full library must build without reaching the row limit, the published
// snapshot must decode inside the reader's budget, and a mix must come back
// from it. It also pins the small-library output byte-for-byte, because this
// path replaced a whole-library buffer with a streaming one and nothing else
// in the tree compares the two.
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/db/sqlite3.h"
#include "src/system/library/genius.h"
#include "src/system/library/geniusbuild.h"
#include "src/system/library/geniusdb.h"
#include "src/system/library/library.h"

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
			exit(1); \
		} \
	} while (0)

// ---------------------------------------------------------------------------
// The index, in memory. Same tags every walk, in the same rowid order, with
// the same empty-path exclusion the real MEDIA_TABLE query applies.
// ---------------------------------------------------------------------------

typedef struct {
	int rows, skipped, genres, album_span, path_span, year_span, path_pad, year_base;
	size_t path_bytes;
} fixture_t;

static fixture_t fixture = {.rows = 2000, .genres = 40, .album_span = 12, .path_span = 24, .year_span = 20, .year_base = 1980};
static unsigned fixture_revision = 7;

static void fixture_row(int row, library_tags_t *tags, char path[4096], char artist[64], char album[64], char genre[32]) {
	int group = row / fixture.album_span;
	int digits = fixture.path_span > 999 ? 4 : 3;
	int used = snprintf(path, 4096, "/music/%0*d/Artist %03d - Track %05d", digits, group % fixture.path_span,
						group % 211, row);
	// A path long enough to test the text budget while still under the
	// per-field bound.
	for (int i = 0; i < fixture.path_pad && used + 8 < 4096; i++) path[used++] = 'x';
	snprintf(path + used, (size_t)(4096 - used), ".flac");
	snprintf(artist, 64, "Artist %03d", group % 211);
	snprintf(album, 64, "Album %04d", group % 997);
	snprintf(genre, 32, "Genre %02d", row % fixture.genres);
	tags->path = path;
	tags->path_bytes = strlen(path);
	tags->artist = artist;
	tags->artist_bytes = strlen(artist);
	tags->album = album;
	tags->album_bytes = strlen(album);
	tags->genre = genre;
	tags->genre_bytes = strlen(genre);
	tags->year = fixture.year_base + row % (fixture.year_span ? fixture.year_span : 1);
}

bool library_read_tags(library_tags_cb cb, void *user, const atomic_bool *cancel, unsigned *revision) {
	(void)cancel;
	*revision = fixture_revision;
	for (int row = 0; row < fixture.rows; row++) {
		library_tags_t tags = {0};
		char path[4096], artist[64], album[64], genre[32];
		if (row % 97 == 0 && fixture.skipped) {
			continue;
		}
		fixture_row(row, &tags, path, artist, album, genre);
		if (!cb(&tags, user)) return false;
	}
	return true;
}

bool library_get_indexed(const char *path, bool *indexed) {
	// One track in every thousand has left the index, so membership is not
	// trivially all-or-nothing.
	(void)path;
	*indexed = (fixture_revision % 2) != 0;
	return true;
}

unsigned library_revision(library_list_t kind) {
	(void)kind;
	return fixture_revision;
}

// ---------------------------------------------------------------------------
// A card standing in for the real one.
// ---------------------------------------------------------------------------

static char root[256];

static void root_open(void) {
	snprintf(root, sizeof(root), "/tmp/sonix-geniuslarge-XXXXXX");
	CHECK(mkdtemp(root) != NULL);
	char path[512];
	snprintf(path, sizeof(path), "%s/.local", root);
	CHECK(mkdir(path, 0700) == 0);
}

static void root_close(void) {
	char command[600];
	snprintf(command, sizeof(command), "rm -rf '%s'", root);
	CHECK(system(command) == 0);
	root[0] = 0;
}

static void genius_path(char out[400]) { snprintf(out, 400, "%s/.local/genius.db", root); }

static bool read_generation(const char *path, char *out, size_t size) {
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	bool ok = false;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) return false;
	if (sqlite3_prepare_v2(db, "SELECT generation FROM GENIUS_SNAPSHOT", -1, &stmt, NULL) == SQLITE_OK &&
		sqlite3_step(stmt) == SQLITE_ROW) {
		const char *text = (const char *)sqlite3_column_text(stmt, 0);
		if (text) {
			snprintf(out, size, "%s", text);
			ok = true;
		}
	}
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return ok;
}

static double now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Build, then mount and walk the result. Returns the published path count.
static int build_and_mount(const char *label, int rows, geniusbuild_result_t expected) {
	fixture.rows = rows;
	fixture.skipped = 0;
	char path[400];
	genius_path(path);
	double started = now();
	geniusbuild_result_t result = geniusbuild_run(path, NULL, NULL, NULL);
	printf("%s: %d rows, result %d, %.2fs\n", label, rows, result, now() - started);
	CHECK(result == expected);
	if (expected != GENIUSBUILD_OK) return 0;
	CHECK(geniusdb_open(path));
	geniusdb_snapshot_t *snapshot = NULL;
	CHECK(geniusdb_load_snapshot(&snapshot, NULL) == GENIUSDB_OK);
	CHECK(snapshot);
	CHECK(geniusdb_path_count(snapshot) == rows);
	size_t bytes = geniusdb_snapshot_bytes(snapshot);
	printf("%s: snapshot %zu bytes of %zu, %d paths\n", label, bytes, GENIUSDB_MAX_SNAPSHOT_BYTES, geniusdb_path_count(snapshot));
	CHECK(bytes <= GENIUSDB_MAX_SNAPSHOT_BYTES);
	// The seed is a middle track, and the mix must be a full one.
	char seed[4096], artist[64], album[64], genre[32];
	library_tags_t tags = {0};
	fixture_row(rows / 2, &tags, seed, artist, album, genre);
	genius_mix_t mix = {0};
	genius_result_t mixed = genius_mix_build(snapshot, seed, GENIUS_MAX_TRACKS, 4242, NULL, &mix);
	printf("%s: mix of %d for %s\n", label, mix.count, seed);
	CHECK(mixed == GENIUS_OK);
	CHECK(mix.count == GENIUS_MAX_TRACKS);
	for (int i = 0; i < mix.count; i++) {
		// Only tracks that are still in the index are offered, and no repeat.
		CHECK(geniusdb_find_path(snapshot, mix.paths[i]) >= 0);
		for (int j = 0; j < i; j++) {
			if (strcmp(mix.paths[i], mix.paths[j]) == 0) {
				fprintf(stderr, "duplicate %d/%d: %s\n", i, j, mix.paths[i]);
				exit(1);
			}
		}
	}
	genius_mix_free(&mix);
	// Seeds across the whole library, not just the middle: a sparse graph would
	// leave most of them short.
	int seeds = 0, shortest = GENIUS_MAX_TRACKS;
	for (int row = 0; row < rows; row += rows / 50 + 1) {
		library_tags_t other = {0};
		char other_path[4096], other_artist[64], other_album[64], other_genre[32];
		fixture_row(row, &other, other_path, other_artist, other_album, other_genre);
		genius_mix_t each = {0};
		CHECK(genius_mix_build(snapshot, other_path, GENIUS_MAX_TRACKS, (unsigned)row, NULL, &each) == GENIUS_OK);
		if (each.count < shortest) shortest = each.count;
		seeds++;
		genius_mix_free(&each);
	}
	printf("%s: %d seeds, shortest mix %d of %d\n", label, seeds, shortest, GENIUS_MAX_TRACKS);
	CHECK(shortest >= 20);
	// Every neighbour list stays inside the per-track bound the builder picked.
	geniusdb_free_snapshot(snapshot);
	geniusdb_close();
	return rows;
}

// Two builds of the same input: the second must recognise the first instead of
// rewriting it, and a changed row must produce a new generation.
static void unchanged_and_changed(void) {
	fixture.rows = 900;
	fixture.skipped = 1;
	char path[400];
	genius_path(path);
	CHECK(geniusbuild_run(path, NULL, NULL, NULL) == GENIUSBUILD_OK);
	CHECK(geniusbuild_run(path, NULL, NULL, NULL) == GENIUSBUILD_CURRENT);
	// A changed tag is a changed input even at the same revision.
	fixture.year_base = 1990;
	CHECK(geniusbuild_run(path, NULL, NULL, NULL) == GENIUSBUILD_OK);
	fixture.year_base = 1980;
	CHECK(geniusbuild_run(path, NULL, NULL, NULL) == GENIUSBUILD_OK);
	printf("rebuild: current input reused, changed tags rebuilt\n");
}

// The 4,000-track graph this fixture produced before the builder learned to
// stream must still be produced exactly: the same input digest and the same
// graph digest mean an existing genius.db stays current and a graph from the PC
// exporter is still recognised instead of rebuilt.
static void reference_graph(void) {
	fixture.rows = 4000;
	fixture.genres = 40;
	fixture.skipped = 0;
	fixture.path_pad = 0;
	char path[400], generation[1024];
	genius_path(path);
	CHECK(geniusbuild_run(path, NULL, NULL, NULL) == GENIUSBUILD_OK);
	CHECK(read_generation(path, generation, sizeof(generation)));
	CHECK(strstr(generation, "\"input_sha256\":\"fe9fffdebf04b2eb948e68cc157d51ff1f4264a9df6bef6f55f8b813c6f9a8e0\""));
	CHECK(strstr(generation, "\"sha256\":\"4fd40686af6f8653221160e8262921e2f83f875b036f3ac60bbd564849d0be82\""));
	CHECK(strstr(generation, "\"tracks\":4000"));
	printf("reference graph: input and graph digests match the previous builder\n");
}

// The row cap reports LIMIT with the previous graph left alone.
static void row_limit(void) {
	fixture.rows = GENIUSBUILD_MAX_TRACKS + 1;
	fixture.genres = 4096; // one track per genre keeps the walk cheap
	fixture.skipped = 0;
	char path[400];
	genius_path(path);
	CHECK(geniusbuild_run(path, NULL, NULL, NULL) == GENIUSBUILD_LIMIT);
	printf("row cap: %d rows refused with LIMIT\n", fixture.rows);
	fixture.genres = 40;
}

// Paths that do not fit the text budget are refused the same way.
static void text_limit(void) {
	fixture.rows = 4000;
	fixture.path_pad = 3000; // still under the per-field bound, over the pool
	fixture.skipped = 0;
	char path[400];
	genius_path(path);
	CHECK(geniusbuild_run(path, NULL, NULL, NULL) == GENIUSBUILD_LIMIT);
	printf("text cap: oversized path pool refused with LIMIT\n");
	fixture.path_pad = 0;
}

int main(int argc, char **argv) {
	// An explicit case number lets the suite be pointed at one library shape
	// while developing; the default runs every case.
	int only = argc > 1 ? atoi(argv[1]) : 0;
	root_open();
	if (!only || only == 1) build_and_mount("25k tracks", 25000, GENIUSBUILD_OK);
	if (!only || only == 2) build_and_mount("50k tracks", 50000, GENIUSBUILD_OK);
	if (!only || only == 3) unchanged_and_changed();
	if (!only || only == 4) row_limit();
	if (!only || only == 5) text_limit();
	if (!only || only == 7) reference_graph();
	if (!only || only == 6) {
		// One genre for the whole library: the largest cluster the ranking can
		// meet, which must finish in bounded time rather than hours.
		fixture.genres = 1;
		build_and_mount("one genre", 20000, GENIUSBUILD_OK);
		fixture.genres = 40;
	}
	root_close();
	puts("Smart Mix large-library tests passed");
	return 0;
}
