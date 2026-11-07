// Boot-time playback restore against the real library database and CUE paths.
//
// Two regressions are pinned here:
//   - album.cue?track=3 is a virtual track, not a readable filesystem name;
//     restore must validate album.cue instead of rejecting the saved track;
//   - Detect changes off means a returning card starts no automatic worker,
//     including a graph-only Smart Mix run on an already-indexed library.
// The saved position, queue and album name are checked across a real close and
// reopen, so the restore inputs are covered as well as their validation.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/db/sqlite3.h"
#include "src/system/decode/apedec.h"
#include "src/system/decode/mp4.h"
#include "src/system/library/cue.h"
#include "src/system/library/geniusbuild.h"
#include "src/system/library/geniusdb.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/playlist.h"

static int failures;

#define CHECK(cond)                                                                                                 \
	do {                                                                                                            \
		if (!(cond)) {                                                                                              \
			failures++;                                                                                             \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                           \
	} while (0)

// The scan worker is linked because library_card_returned() is under test, but
// no file is actually probed in the disabled-setting case. These stubs keep the
// unrelated codecs and Smart Mix builder out of this database-focused fixture.
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

mp4_file_t *mp4_open(const char *path) {
	(void)path;
	return NULL;
}

void mp4_close(mp4_file_t *m) { (void)m; }

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

void metadata_read(const char *filepath, song_metadata_t *out) {
	(void)filepath;
	if (out) {
		memset(out, 0, sizeof(*out));
	}
}

geniusbuild_result_t geniusbuild_run(const char *path, const atomic_bool *cancel, geniusbuild_progress_cb progress,
									 void *user) {
	(void)path;
	(void)cancel;
	(void)progress;
	(void)user;
	return GENIUSBUILD_CURRENT;
}

bool geniusdb_open(const char *path) {
	(void)path;
	return true;
}

void geniusdb_close(void) {}
bool geniusdb_available(void) { return true; }

static int update_events;

static void update_listener(library_update_event_t event, int added, int removed, int updated) {
	(void)event;
	(void)added;
	(void)removed;
	(void)updated;
	update_events++;
}

static void make_file(const char *path) {
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if (f) {
		fclose(f);
	}
}

static void check_saved_state(const char *virtual_track) {
	char loaded[512] = "";
	double position = -1;
	CHECK(library_playback_state_load(loaded, sizeof(loaded), &position));
	CHECK(strcmp(loaded, virtual_track) == 0);
	CHECK(position == 87.5);

	char **queue = NULL;
	int index = -1;
	bool custom = true;
	bool ordered = false;
	int count = library_queue_load(&queue, &index, &custom, &ordered);
	CHECK(count == 2);
	CHECK(index == 1);
	CHECK(!custom);
	CHECK(ordered);
	if (count == 2) {
		CHECK(strstr(queue[0], "song.flac") != NULL);
		CHECK(strcmp(queue[1], virtual_track) == 0);
	}
	library_queue_free(queue, count);

	char album[128];
	CHECK(library_queue_load_album(album, sizeof(album)));
	CHECK(strcmp(album, "CUE Album") == 0);
}

int main(void) {
	char dir[] = "/tmp/sonix-playback-state-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}

	char config_path[512], db_path[512], song[512], cue[512], missing[512], virtual_track[600];
	snprintf(config_path, sizeof(config_path), "%s/device_config.ini", dir);
	snprintf(db_path, sizeof(db_path), "%s/library.db", dir);
	snprintf(song, sizeof(song), "%s/song.flac", dir);
	snprintf(cue, sizeof(cue), "%s/album.cue", dir);
	snprintf(missing, sizeof(missing), "%s/missing.cue?track=1", dir);
	snprintf(virtual_track, sizeof(virtual_track), "%s?track=3", cue);
	make_file(song);
	make_file(cue);
	config_init(config_path);
	CHECK(library_open(db_path));

	// Both an ordinary file and a virtual CUE track are valid restore inputs.
	CHECK(library_playback_track_readable(song));
	CHECK(library_playback_track_readable(virtual_track));
	CHECK(!library_playback_track_readable(missing));
	CHECK(!library_playback_track_readable(NULL));
	CHECK(!library_playback_track_readable(""));
	char sheet[512];
	CHECK(cue_split_path(virtual_track, sheet, sizeof(sheet)) == 3);
	CHECK(strcmp(sheet, cue) == 0);

	// Save exactly what a CUE album session needs after a restart.
	library_playback_state_save(virtual_track, 87.5);
	const char *queue[] = {song, virtual_track};
	library_queue_save(queue, 2, 1, false, true);
	library_queue_save_album("CUE Album");
	check_saved_state(virtual_track);
	library_close();

	// Give the library an indexed row. With automatic detection explicitly
	// disabled, the old code nevertheless started SCAN_GENIUS here.
	sqlite3 *raw = NULL;
	CHECK(sqlite3_open(db_path, &raw) == SQLITE_OK);
	if (raw) {
		CHECK(sqlite3_exec(raw, "INSERT INTO MEDIA_TABLE(id,path) VALUES(1,'/tmp/song.flac')", NULL, NULL,
						   NULL) == SQLITE_OK);
		sqlite3_close(raw);
	}
	CHECK(library_open(db_path));
	check_saved_state(virtual_track);

	library_set_detect_changes(false);
	library_set_update_listener(update_listener);
	update_events = 0;
	CHECK(library_detect_changes_chosen());
	CHECK(!library_detect_changes());
	CHECK(library_track_count() == 1);
	CHECK(!library_card_returned(dir));
	CHECK(!library_scan_running());
	CHECK(update_events == 0);
	check_saved_state(virtual_track); // rejecting automatic work loses nothing
	library_close();

	unlink(song);
	unlink(cue);
	unlink(db_path);
	unlink(config_path);
	rmdir(dir);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("PASS playback state: CUE restore inputs, saved album queue and Detect changes gating");
	return 0;
}
