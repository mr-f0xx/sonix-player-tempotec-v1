// test_listeningstats.c -- the day-by-day listening history, against the real
// library database.
//
// What is checked here is the whole path the Stats page draws from: a second
// of playback counted only when the position moved, a play banked and written
// once, the minute that writes it all down, and then the read side -- the
// summary cards, the chart's per-day rows, and the three top lists, in each of
// the four ranges.
//
// The database, the schema and the queries are the real ones. Only the scan's
// file-type probes are stubbed, as in test_mostplayed.c: nothing here walks a
// card. The day the history is written under is the real today, so the ranges
// are exercised against rows written at today's day number and against rows
// written 40, 200 and 800 days ago.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/db/sqlite3.h"
#include "src/system/library/cue.h"
#include "src/system/library/library.h"
#include "src/system/library/listeningstats.h"
#include "src/system/decode/mp4.h"
#include "src/system/decode/apedec.h"
#include "src/system/playback/playlist.h"

static int failures;
static int fake_today = 20780; // 2026-11-23 UTC
time_t __wrap_time(time_t *out) {
	time_t now = (time_t)fake_today * 86400;
	if (out) *out = now;
	return now;
}

#define CHECK(cond)                                                                             \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			failures++;                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
		}                                                                                       \
	} while (0)

// -----------------------------------------------------------------------
// The scan's file-type probes, stubbed (see test_mostplayed.c).
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
// Writing history the way the player does
// -----------------------------------------------------------------------

// A minute of playback of `path`, banked as `plays` plays of it. Bypasses the
// one-second accumulator: the whole minute lands in one write, which is what
// the accumulator produces anyway.
static bool write_minute(int day, const char *path, long seconds, int plays) {
	library_stats_play_t play;
	memset(&play, 0, sizeof(play));
	snprintf(play.path, sizeof(play.path), "%s", path);
	play.plays = plays;
	return library_stats_add(day, seconds, &play, 1);
}

// The card's index, as the scan would have written it: one album by one
// artist, three tracks.
static void seed_index(void) {
	library_close();
	sqlite3 *db = NULL;
	CHECK(sqlite3_open(getenv("SONIX_TEST_DB"), &db) == SQLITE_OK);
	const char *sql =
		"INSERT INTO MEDIA_TABLE(id,path,name,album,album_artist,artist,genre,disc,dis_id,sortkey)"
		" VALUES"
		"(1,'/card/a/01.flac','First','Red','Artist A','Artist A','Rock',1,1,'first'),"
		"(2,'/card/a/02.flac','Second','Red','Artist A','Artist A','Rock',1,2,'second'),"
		"(3,'/card/b/01.flac','Third','Blue','Artist B','Artist B','Jazz',1,1,'third')";
	CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(db);
	CHECK(library_open(getenv("SONIX_TEST_DB")));
	CHECK(library_is_open());
}

static void test_write_and_summary(void) {
	int today = library_stats_today();

	// Today: ten minutes of the first track, played twice; five of the
	// third, played once.
	CHECK(write_minute(today, "/card/a/01.flac", 600, 2));
	CHECK(write_minute(today, "/card/b/01.flac", 300, 1));
	// Forty days back: one play of the second track, four minutes.
	CHECK(write_minute(today - 40, "/card/a/02.flac", 240, 1));
	// 200 days back, and 800: the year list must see the first and not the
	// second.
	CHECK(write_minute(today - 200, "/card/a/01.flac", 120, 1));
	CHECK(write_minute(today - 800, "/card/b/01.flac", 60, 1));

	// The week sees today's two tracks and nothing else.
	stats_summary_t week;
	stats_summary(STATS_RANGE_WEEK, &week);
	CHECK(week.seconds == 900);
	CHECK(week.plays == 3);
	CHECK(week.tracks == 2);
	CHECK(week.albums == 2);
	CHECK(week.artists == 2);

	// Forty days back is in the preceding calendar month.
	stats_summary_t month;
	stats_summary(STATS_RANGE_MONTH, &month);
	CHECK(month.seconds == 900);
	CHECK(month.tracks == 2);

	// The year adds the row 200 days back (120 s, the first track again) and
	// still not the one at 800.
	stats_summary_t year;
	stats_summary(STATS_RANGE_YEAR, &year);
	CHECK(year.seconds == 1260);
	CHECK(year.tracks == 3); // both of Red's tracks and Blue's one
	CHECK(year.albums == 2);
	CHECK(year.artists == 2);
	CHECK(year.plays == 5);

	// All time: every row, including the one at 800 days that no other range
	// reaches -- which brings the second record and the second artist back,
	// and one more track.
	stats_summary_t all;
	stats_summary(STATS_RANGE_ALL, &all);
	CHECK(all.seconds == 1320);
	CHECK(all.tracks == 3);
	CHECK(all.albums == 2);
	CHECK(all.artists == 2);
	CHECK(all.plays == 6);

	// And the plays were counted for Most played as well, through the same
	// write: the two views cannot disagree about how often something played.
	CHECK(library_play_count("/card/a/01.flac") == 3);
	CHECK(library_play_count("/card/b/01.flac") == 2);
	CHECK(library_play_count("/card/a/02.flac") == 1);
}

static void test_daily(void) {
	int today = library_stats_today();
	stats_day_t days[STATS_CHART_WEEK_DAYS];
	memset(days, 0x7f, sizeof(days));
	stats_daily(STATS_CHART_WEEK_DAYS, days, STATS_CHART_WEEK_DAYS);

	// The last day is today, the first is six days ago, and the gap days in
	// between are zero rather than missing: there is a bar for each of them.
	CHECK(days[STATS_CHART_WEEK_DAYS - 1].day == today);
	CHECK(days[0].day == today - 6);
	for (int i = 0; i < STATS_CHART_WEEK_DAYS - 1; i++) {
		CHECK(days[i].seconds == 0);
		CHECK(days[i].tracks == 0);
	}
	CHECK(days[STATS_CHART_WEEK_DAYS - 1].seconds == 900);
	CHECK(days[STATS_CHART_WEEK_DAYS - 1].tracks == 3); // two plays of one and one of another

	// Thirty days: twenty-nine back from today, and today's row still last.
	stats_day_t chart[STATS_CHART_MONTH_DAYS];
	memset(chart, 0, sizeof(chart));
	stats_daily(30, chart, 30);
	CHECK(chart[0].day == today - 29);
	CHECK(chart[29].day == today);
	CHECK(chart[29].seconds == 900);
}

static void test_top_lists(void) {
	stats_entry_t rows[8];

	// Tracks, this week: the first track leads with its two plays, and the
	// third follows.
	memset(rows, 0, sizeof(rows));
	CHECK(stats_top(STATS_TOP_TRACKS, STATS_RANGE_WEEK, 0, 8, rows) == 2);
	CHECK(strcmp(rows[0].name, "First") == 0);
	CHECK(strcmp(rows[0].artist, "Artist A") == 0);
	CHECK(rows[0].plays == 2);
	CHECK(strcmp(rows[1].name, "Third") == 0);
	CHECK(rows[1].plays == 1);

	// Albums, all time: Red four plays (two today, one two hundred days back,
	// one of its second track at forty) and Blue two.
	memset(rows, 0, sizeof(rows));
	CHECK(stats_top(STATS_TOP_ALBUMS, STATS_RANGE_ALL, 0, 8, rows) == 2);
	CHECK(strcmp(rows[0].name, "Red") == 0);
	CHECK(rows[0].plays == 4);
	CHECK(strcmp(rows[0].artist, "Artist A") == 0);
	CHECK(strcmp(rows[1].name, "Blue") == 0);

	// Artists, this year: A has four plays (two of its first track today, one
	// of it two hundred days back, one of the second at forty), B one.
	memset(rows, 0, sizeof(rows));
	CHECK(stats_top(STATS_TOP_ARTISTS, STATS_RANGE_YEAR, 0, 8, rows) == 2);
	CHECK(strcmp(rows[0].name, "Artist A") == 0);
	CHECK(rows[0].plays == 4);
	CHECK(strcmp(rows[0].genre, "Rock") == 0);
	CHECK(strcmp(rows[1].name, "Artist B") == 0);
	CHECK(rows[1].plays == 1);

	// Paging: one row at a time, in the same order as the list.
	stats_entry_t one[1];
	CHECK(stats_top(STATS_TOP_ARTISTS, STATS_RANGE_YEAR, 0, 1, one) == 1);
	CHECK(strcmp(one[0].name, "Artist A") == 0);
	CHECK(stats_top(STATS_TOP_ARTISTS, STATS_RANGE_YEAR, 1, 1, one) == 1);
	CHECK(strcmp(one[0].name, "Artist B") == 0);
	CHECK(stats_top(STATS_TOP_ARTISTS, STATS_RANGE_YEAR, 2, 1, one) == 0);

	// And the count the "See all" page sizes itself from.
	CHECK(stats_top_count(STATS_TOP_TRACKS, STATS_RANGE_ALL) == 3);
	CHECK(stats_top_count(STATS_TOP_ALBUMS, STATS_RANGE_ALL) == 2);
	CHECK(stats_top_count(STATS_TOP_ARTISTS, STATS_RANGE_ALL) == 2);
	CHECK(stats_top_count(STATS_TOP_TRACKS, STATS_RANGE_WEEK) == 2);
	CHECK(stats_top_count(STATS_TOP_ALBUMS, STATS_RANGE_WEEK) == 2);
}

static void test_survives_reopen(void) {
	// The history lives in the card's own database: closing and reopening it
	// -- a restart, a card taken out and put back -- changes nothing.
	library_close();
	CHECK(library_open(getenv("SONIX_TEST_DB")));
	stats_summary_t all;
	stats_summary(STATS_RANGE_ALL, &all);
	CHECK(all.seconds == 1320);
	CHECK(all.tracks == 3);
}

// The accumulator in front of all of that: one call is one second at most,
// and only a position that actually moved is a second.
static void test_accumulator(void) {
	int today = library_stats_today();
	stats_reset_pending();

	// The player's own cadence: the progress poll runs twice a second while
	// something plays, and the position moves half a second a poll. Twenty
	// polls are ten seconds of listening, not twenty -- a counter that
	// trusted the call count instead of the clock would double it.
	stats_note_playback("/card/a/01.flac", 30.0, 200.0, 0);
	double position = 30.0;
	for (int i = 1; i <= 20; i++) {
		position += 0.5;
		stats_note_playback("/card/a/01.flac", position, 200.0, (uint32_t)i * 500u);
	}
	// A pause: the position stands still, however many polls go by.
	stats_note_playback("/card/a/01.flac", position, 200.0, 11000);
	stats_note_playback("/card/a/01.flac", position, 200.0, 12000);
	// A seek forwards, and one back: neither is listening.
	stats_note_playback("/card/a/01.flac", 150.0, 200.0, 13000);
	stats_note_playback("/card/a/01.flac", 20.0, 200.0, 14000);

	// One play banked, then the minute is up: ten seconds and one play reach
	// the database under today's number.
	stats_note_play("/card/a/01.flac");
	stats_flush();

	stats_day_t days[1];
	stats_daily(1, days, 1);
	CHECK(days[0].day == today);
	// The ten seconds counted just now, on top of the nine hundred written
	// above.
	CHECK(days[0].seconds == 910);

	stats_entry_t rows[8];
	CHECK(stats_top(STATS_TOP_TRACKS, STATS_RANGE_WEEK, 0, 8, rows) == 2);
	CHECK(strcmp(rows[0].name, "First") == 0);
	CHECK(rows[0].plays == 3); // the two above, and the one just banked
	CHECK(library_play_count("/card/a/01.flac") == 4); // the three above, and it

	// Nothing pending, nothing written: a paused player costs no writes.
	stats_flush();
	stats_daily(1, days, 1);
	CHECK(days[0].seconds == 910);

	// A second track is a new clock, not a jump: it starts counting at once,
	// in the same half-second steps the poll delivers, and two of them are one
	// second of listening.
	stats_note_playback("/card/b/01.flac", 0.0, 180.0, 20000);
	stats_note_playback("/card/b/01.flac", 0.5, 180.0, 20500);
	stats_note_playback("/card/b/01.flac", 1.0, 180.0, 21000);
	stats_flush();
	stats_daily(1, days, 1);
	CHECK(days[0].seconds == 911);
}
static void test_format(void) {
	char text[32];
	stats_format_duration(0, text, sizeof(text));
	CHECK(strcmp(text, "0h 00m") == 0);
	stats_format_duration(20, text, sizeof(text));
	CHECK(strcmp(text, "0h 01m") == 0);
	stats_format_duration(3 * 60, text, sizeof(text));
	CHECK(strcmp(text, "0h 03m") == 0);
	stats_format_duration(59 * 60 + 40, text, sizeof(text));
	CHECK(strcmp(text, "1h 00m") == 0);
	stats_format_duration(12 * 3600 + 34 * 60, text, sizeof(text));
	CHECK(strcmp(text, "12h 34m") == 0);
	stats_format_duration(100 * 3600, text, sizeof(text));
	CHECK(strcmp(text, "100h 00m") == 0);
}

static void test_calendar(void) {
	int first, count, month, year;
	CHECK(library_stats_calendar(20485, &first, &count, &month, &year)); // Feb 2026
	CHECK(first == 20485 && count == 28 && month == 2 && year == 2026);
	CHECK(library_stats_calendar(21243, &first, &count, &month, &year)); // leap day
	CHECK(first == 21215 && count == 29 && month == 2 && year == 2028);
	CHECK(library_stats_calendar(20454, &first, &count, &month, &year));
	CHECK(count == 31 && month == 1);
	CHECK(library_stats_calendar(20758, &first, &count, &month, &year));
	CHECK(count == 30 && month == 11);

	seed_index();
	CHECK(write_minute(20453, "/card/a/01.flac", 10, 1)); // previous year
	CHECK(write_minute(20454, "/card/a/01.flac", 60, 1));
	CHECK(write_minute(20512, "/card/a/02.flac", 120, 2));
	CHECK(write_minute(20758, "/card/b/01.flac", 180, 3));
	CHECK(write_minute(20780, "/card/b/01.flac", 240, 4));
	CHECK(write_minute(20787, "/card/a/01.flac", 999, 5)); // future in month
	CHECK(write_minute(20788, "/card/a/01.flac", 888, 6)); // future month
	fake_today = 20780;
	stats_day_t days[STATS_CHART_MONTH_DAYS];
	CHECK(stats_calendar_month(days, &month, &year) == 30);
	CHECK(month == 11 && year == 2026 && days[0].day == 20758);
	CHECK(days[0].seconds == 180 && days[22].seconds == 240 && days[29].seconds == 0);
	int64_t months[12];
	stats_calendar_year(months, &year);
	CHECK(year == 2026 && months[0] == 60 && months[1] == 120 && months[10] == 420 && months[11] == 0);
	stats_summary_t summary;
	stats_summary(STATS_RANGE_MONTH, &summary);
	CHECK(summary.seconds == 420 && summary.plays == 7);
	CHECK(summary.tracks == 1);
	stats_entry_t entries[4];
	CHECK(stats_top(STATS_TOP_TRACKS, STATS_RANGE_MONTH, 0, 4, entries) == 1);
	CHECK(strcmp(entries[0].name, "Third") == 0 && entries[0].plays == 7);
	CHECK(stats_top_count(STATS_TOP_TRACKS, STATS_RANGE_MONTH) == 1);
	stats_summary(STATS_RANGE_YEAR, &summary);
	CHECK(summary.seconds == 600 && summary.plays == 10 && summary.tracks == 3);
	CHECK(stats_top_count(STATS_TOP_TRACKS, STATS_RANGE_YEAR) == 3);
	fake_today = 21243;
	CHECK(library_stats_add(21243, 55, NULL, 0));
	CHECK(stats_calendar_month(days, &month, &year) == 29);
	CHECK(days[28].seconds == 55);
	stats_calendar_year(months, &year);
	CHECK(year == 2028 && months[1] == 55);
	fake_today = 20512;
	CHECK(stats_calendar_month(days, &month, &year) == 28);
	fake_today = 20454;
	CHECK(stats_calendar_month(days, &month, &year) == 31);
	stats_summary(STATS_RANGE_YEAR, &summary);
	CHECK(summary.seconds == 60 && summary.plays == 1);
}

int main(void) {
	char dir[] = "/tmp/sonix-stats-test-XXXXXX";
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

	listeningstats_init();
	seed_index();

	// The library has to know the files before a play can be filed under
	// their record and artist, which is what seed_index() writes.
	library_close();
	CHECK(library_open(db));

	test_write_and_summary();
	test_daily();
	test_top_lists();
	test_survives_reopen();
	test_accumulator();
	test_format();

	library_close();
	unlink(db);
	// Isolate the fixed-date year tests from the real-today fixture above.
	CHECK(library_open(db));
	test_calendar();
	library_close();
	unlink(db);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("listeningstats: seconds, plays, days and top lists all add up\n");
	return 0;
}
