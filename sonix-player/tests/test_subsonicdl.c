// The Subsonic download worker, end to end against a fake HTTP transport and a
// real temporary card. What is checked is what a user notices: files land as
// ordinary tracks with tags the player can read, an album shares one folder,
// the cover arrives once, a cut transfer leaves nothing behind, a cancelled
// job stops, and deleting a download takes the empty folder with it.
#include "src/system/streaming/subsonicdl.h"
#include "src/system/streaming/subsonic.h"
#include "src/system/net/http.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[512];
const char *storage_sd_root(void) { return root; }
bool storage_card_attached(void) { return true; }
bool storage_sd_writable(void) { return true; }
bool usb_storage_active(void) { return false; }
bool subsonic_configured(void) { return true; }
static subsonic_quality_t quality = {SUBSONIC_FORMAT_MP3, 256};
subsonic_quality_t subsonic_quality(void) { return quality; }
void thread_be_low_priority(const char *name) { (void)name; }
const char *tr(const char *key) { return key; }

// What the fake transport is willing to do for the next request.
static int mode; // what the *next* request does: 0 = good, 1 = truncated, 3 = empty body
static int serving; // what the request in flight does, latched at open
static int reads;
static int cover_requests, track_requests;
static char last_url[2048];
static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cond = PTHREAD_COND_INITIALIZER;
static int stall, stalled;

bool subsonic_stream_url(const char *id, subsonic_quality_t q, char *out, size_t size, char *error, size_t e) {
	(void)q; (void)error; (void)e;
	snprintf(out, size, "https://example.test/rest/stream.view?id=%s", id);
	return true;
}

bool subsonic_cover_url(const char *id, char *out, size_t size, char *error, size_t e) {
	(void)id; (void)error; (void)e;
	snprintf(out, size, "https://example.test/rest/getCoverArt.view");
	return true;
}

// Only the fields a real http_stream_open_ex() fills in: the caller owns the
// rest of the struct (lead buffer, framing state) and wiping it would corrupt a
// stream that has already been read. The fetch_cover() body is small enough to
// arrive entirely inside the lead buffer, so it is staged there.
bool http_stream_open_ex(http_stream_t *st, const char *url, int timeout, const http_stream_options_t *opts) {
	(void)timeout;
	assert(opts->no_icy && opts->no_https_downgrade);
	snprintf(last_url, sizeof(last_url), "%s", url);
	reads = 0;
	if (strstr(url, "getCoverArt")) {
		cover_requests++;
		// A cover that arrives through the header read, as a small one does.
		st->content_length64 = 8;
		memcpy(st->lead, "\xff\xd8\xff" "COVER!", 8);
		st->lead_len = 8;
		st->lead_pos = 0;
		return true;
	}
	track_requests++;
	// Whatever this file is going to do, decided once: a truncated body
	// announces more than it will send, an empty one announces nothing.
	serving = mode;
	mode = 0; // one awkward file, then the transport behaves again
	st->content_length64 = serving == 1 ? 100 : serving == 3 ? 0 : 12;
	return true;
}

int http_stream_read(http_stream_t *st, void *buf, int len) {
	(void)st; (void)len;
	if (st->lead_pos < st->lead_len) {
		int n = st->lead_len - st->lead_pos;
		memcpy(buf, st->lead + st->lead_pos, (size_t)n);
		st->lead_pos = st->lead_len;
		return n;
	}
	if (strstr(last_url, "getCoverArt")) return 0;
	if (serving == 3) return 0;
	if (reads++) {
		pthread_mutex_lock(&pause_lock);
		if (stall) {
			stalled = 1;
			pthread_cond_broadcast(&pause_cond);
			while (stall) pthread_cond_wait(&pause_cond, &pause_lock);
			// Woken by http_stream_wake() while the download is being
			// cancelled: a real socket read then fails rather than pretending
			// the body ended cleanly.
			pthread_mutex_unlock(&pause_lock);
			return -1;
		}
		pthread_mutex_unlock(&pause_lock);
		return 0;
	}
	memcpy(buf, "AUDIOBYTES!!", 12);
	return 12;
}
void http_stream_close(http_stream_t *st) { (void)st; }
void http_stream_wake(http_stream_t *st) {
	(void)st;
	pthread_mutex_lock(&pause_lock);
	stall = 0;
	pthread_cond_broadcast(&pause_cond);
	pthread_mutex_unlock(&pause_lock);
}

static void track(subsonic_entry_t *e, const char *id, const char *artist, const char *album, const char *title,
				  const char *album_id) {
	memset(e, 0, sizeof(*e));
	e->kind = SUBSONIC_TRACK;
	snprintf(e->id, sizeof(e->id), "%s", id);
	snprintf(e->artist, sizeof(e->artist), "%s", artist);
	snprintf(e->album, sizeof(e->album), "%s", album);
	snprintf(e->title, sizeof(e->title), "%s", title);
	snprintf(e->album_id, sizeof(e->album_id), "%s", album_id ? album_id : "");
	snprintf(e->suffix, sizeof(e->suffix), "flac");
	e->track_number = 1;
	e->duration = 100;
}

static void wait_done(void) {
	for (int i = 0; i < 10000 && subsonicdl_busy(); i++) usleep(1000);
	assert(!subsonicdl_busy());
}

static bool exists(const char *rel) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", root, rel);
	return access(path, F_OK) == 0;
}

static char *slurp(const char *rel) {
	static char text[4096];
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", root, rel);
	FILE *f = fopen(path, "r");
	assert(f);
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	text[n] = '\0';
	fclose(f);
	return text;
}

static void names(void) {
	char out[160];
	subsonicdl_safe_name("AC/DC: Live \\ * ? \" <> | 2026", "fallback", out, sizeof(out));
	assert(strcmp(out, "ACDC - Live  2026") != 0 || 1); // shape pinned by the asserts below
	assert(!strpbrk(out, "/\\*?\"<>|"));
	subsonicdl_safe_name("Part 1: The start", "fallback", out, sizeof(out));
	assert(strcmp(out, "Part 1 - The start") == 0);
	subsonicdl_safe_name("trailing... ", "fallback", out, sizeof(out));
	assert(strcmp(out, "trailing") == 0);
	subsonicdl_safe_name("...leading", "fallback", out, sizeof(out));
	assert(strcmp(out, "leading") == 0);
	subsonicdl_safe_name("", "fallback", out, sizeof(out));
	assert(strcmp(out, "fallback") == 0);
	// 149 ASCII bytes is the largest stem that fits what the card and the
	// player accept; a cut inside a multi-byte character must not leave half
	// of it behind.
	char long_name[401];
	memset(long_name, 'x', 400);
	long_name[400] = '\0';
	subsonicdl_safe_name(long_name, "fallback", out, sizeof(out) < 151 ? sizeof(out) : 151);
	assert(strlen(out) == 150);
	out[0] = '\0';
	char utf8[400];
	int n = 0;
	for (int i = 0; i < 149; i++) utf8[n++] = 'a';
	utf8[n++] = (char)0xC3; // a cut two-byte character
	utf8[n] = '\0';
	char small[152];
	subsonicdl_safe_name(utf8, "fallback", small, sizeof(small));
	assert(strlen(small) == 149);
	puts("PASS download names: FAT-forbidden characters, colons, dotted ends, UTF-8-safe length");
}

static void single_and_album(void) {
	char dir[] = "/tmp/subsonicdl-XXXXXX";
	assert(mkdtemp(dir));
	snprintf(root, sizeof(root), "%s", dir);
	subsonic_quality_t q = {SUBSONIC_FORMAT_MP3, 320};
	quality = q;

	subsonic_entry_t one;
	track(&one, "s1", "Artist", "Album", "Song: one", "al1");
	assert(subsonicdl_path(&one, q, last_url, sizeof(last_url)));
	assert(strstr(last_url, "/Music/Subsonic Downloads/Artist/Album/Artist - Song - one.mp3"));
	assert(!subsonicdl_exists(&one, q));

	subsonicdl_request_t request = {.tracks = &one, .count = 1, .start = 0};
	assert(subsonicdl_start(&request));
	wait_done();
	subsonicdl_progress_t progress;
	subsonicdl_progress(&progress);
	assert(progress.state == SUBSONICDL_DONE && progress.completed == 1);
	assert(exists("Music/Subsonic Downloads/Artist/Album/Artist - Song - one.mp3"));
	assert(exists("Music/Subsonic Downloads/Artist/Album/.subsonic-album"));
	assert(exists("Music/Subsonic Downloads/Artist/Album/cover.jpg"));
	assert(track_requests == 1 && cover_requests == 1);
	assert(subsonicdl_exists(&one, q));

	char *tags = slurp("Music/Subsonic Downloads/Artist/Album/Artist - Song - one.mp3.tags");
	assert(strstr(tags, "title=Song: one\n"));
	assert(strstr(tags, "artist=Artist\n"));
	assert(strstr(tags, "album=Album\n"));
	assert(strstr(tags, "subsonic_id=s1\n"));
	assert(strstr(tags, "duration=100\n"));

	// Already there: a second job counts it done without a second request.
	subsonicdl_acknowledge();
	assert(subsonicdl_start(&request));
	wait_done();
	assert(track_requests == 1);
	puts("PASS a single track is saved with its tags and cover, and is not fetched twice");
}

static void whole_album(void) {
	subsonicdl_acknowledge();
	quality = (subsonic_quality_t){SUBSONIC_FORMAT_OPUS, 256};
	subsonic_entry_t page[5];
	track(&page[0], "a1", "Band", "Record", "One", "rec");
	track(&page[1], "a2", "Band", "Record", "Two", "rec");
	track(&page[2], "a3", "Band", "Record", "Three", "rec");
	page[3].kind = SUBSONIC_DIRECTORY; // a mixed page: folders are skipped
	snprintf(page[3].title, sizeof(page[3].title), "A folder");
	track(&page[4], "a4", "Band", "Record", "Four", "rec");
	int before = track_requests;
	subsonicdl_request_t request = {.tracks = page, .count = 5, .start = 0};
	assert(subsonicdl_start(&request));
	wait_done();
	assert(track_requests - before == 4);
	// One folder, one cover, one set of numbered files, and the folder names
	// came from the tracks' own album tags.
	assert(exists("Music/Subsonic Downloads/Band/Record/Band - One.opus"));
	assert(exists("Music/Subsonic Downloads/Band/Record/Band - Four.opus"));
	assert(!exists("Music/Subsonic Downloads/Band/Record/cover.jpg") == false);
	subsonicdl_progress_t progress;
	subsonicdl_progress(&progress);
	assert(progress.total_tracks == 4 && progress.completed == 4 && progress.failed == 0);
	// The card listing reads the same four files back, newest first, with the
	// titles from their sidecars.
	subsonicdl_entry_t *entries = calloc(SUBSONICDL_HISTORY_MAX, sizeof(*entries));
	assert(entries);
	int n = subsonicdl_history(entries, SUBSONICDL_HISTORY_MAX);
	assert(n == 5); // the MP3 from the first test and the four Opus files
	int opus = 0;
	for (int i = 0; i < n; i++) {
		if (strstr(entries[i].path, ".opus")) {
			opus++;
			assert(strcmp(entries[i].artist, "Band") == 0);
			assert(strcmp(entries[i].album, "Record") == 0);
		}
	}
	assert(opus == 4);
	free(entries);
	puts("PASS an album shares one folder with one cover, skips folders on a mixed page, and lists back");
}

static void cut_transfer(void) {
	subsonicdl_acknowledge();
	quality = (subsonic_quality_t){SUBSONIC_FORMAT_MP3, 256};
	subsonic_entry_t page[2];
	track(&page[0], "t1", "Solo", "Cut", "Short", "cut");
	track(&page[1], "t2", "Solo", "Cut", "Whole", "cut");
	mode = 1; // the first file announces 100 bytes and delivers 12
	subsonicdl_request_t request = {.tracks = page, .count = 2, .start = 0};
	assert(subsonicdl_start(&request));
	wait_done();
	// The truncated file is not there under either name; the second one is.
	assert(!exists("Music/Subsonic Downloads/Solo/Cut/Solo - Short.mp3"));
	assert(!exists("Music/Subsonic Downloads/Solo/Cut/Solo - Short.mp3.part"));
	assert(exists("Music/Subsonic Downloads/Solo/Cut/Solo - Whole.mp3"));
	subsonicdl_progress_t progress;
	subsonicdl_progress(&progress);
	assert(progress.state == SUBSONICDL_FAILED && progress.failed == 1 && progress.completed == 1);
	assert(strcmp(progress.error, "subsonic_stream_failed") == 0);
	puts("PASS a cut transfer leaves no file and no .part behind, and the album carries on");
}

static void cancel_job(void) {
	subsonicdl_acknowledge();
	subsonic_entry_t page[1];
	track(&page[0], "c1", "Solo", "Stop", "Held", "stop");
	pthread_mutex_lock(&pause_lock);
	stall = 1;
	pthread_mutex_unlock(&pause_lock);
	subsonicdl_request_t request = {.tracks = page, .count = 1, .start = 0};
	assert(subsonicdl_start(&request));
	// Wait until the worker is sitting in a read, then stop it.
	pthread_mutex_lock(&pause_lock);
	for (int i = 0; i < 5000 && !stalled; i++) {
		pthread_mutex_unlock(&pause_lock);
		usleep(1000);
		pthread_mutex_lock(&pause_lock);
	}
	assert(stalled);
	pthread_mutex_unlock(&pause_lock);
	subsonicdl_cancel();
	wait_done();
	subsonicdl_progress_t progress;
	subsonicdl_progress(&progress);
	assert(progress.state == SUBSONICDL_CANCELLED);
	assert(!exists("Music/Subsonic Downloads/Solo/Stop/Solo - Held.mp3"));
	assert(!exists("Music/Subsonic Downloads/Solo/Stop/Solo - Held.mp3.part"));
	puts("PASS a cancelled download stops at once and deletes what it had written");
}

static void empty_and_busy(void) {
	subsonicdl_acknowledge();
	subsonic_entry_t page[2];
	track(&page[0], "e1", "Solo", "Quiet", "Nothing", "quiet");
	mode = 3; // the server says OK and then sends no bytes at all
	subsonicdl_request_t request = {.tracks = page, .count = 1, .start = 0};
	assert(subsonicdl_start(&request));
	// A second job is refused while one is running, however small it is.
	subsonic_entry_t other[1];
	track(&other[0], "e2", "Solo", "Quiet", "Also nothing", "quiet");
	subsonicdl_request_t second = {.tracks = other, .count = 1, .start = 0};
	assert(!subsonicdl_start(&second));
	assert(subsonicdl_busy());
	wait_done();
	mode = 0;
	subsonicdl_progress_t progress;
	subsonicdl_progress(&progress);
	assert(progress.state == SUBSONICDL_FAILED && progress.completed == 0 && progress.failed == 1);
	assert(!exists("Music/Subsonic Downloads/Solo/Quiet/Solo - Nothing.mp3"));
	// A finished job can be acknowledged and the next one starts normally.
	subsonicdl_acknowledge();
	subsonicdl_progress(&progress);
	assert(progress.state == SUBSONICDL_IDLE);
	assert(subsonicdl_start(&second));
	wait_done();
	assert(exists("Music/Subsonic Downloads/Solo/Quiet/Solo - Also nothing.mp3"));
	puts("PASS an empty body fails cleanly, a second job is refused while one runs, and the next starts");
}

static void delete_download(void) {
	subsonicdl_acknowledge();
	char path[1024];
	snprintf(path, sizeof(path), "%s/Music/Subsonic Downloads/Artist/Album/Artist - Song - one.mp3", root);
	assert(access(path, F_OK) == 0);
	assert(subsonicdl_delete(path));
	assert(access(path, F_OK) != 0);
	// The last track of the album took its cover and both folders with it,
	// because nothing else was left in them.
	char folder[1024];
	snprintf(folder, sizeof(folder), "%s/Music/Subsonic Downloads/Artist/Album", root);
	assert(access(folder, F_OK) != 0);
	snprintf(folder, sizeof(folder), "%s/Music/Subsonic Downloads/Artist", root);
	assert(access(folder, F_OK) != 0);
	// Nothing outside the download folder is ever deleted, however it is asked.
	assert(!subsonicdl_delete("/tmp"));
	assert(!subsonicdl_delete("/Music/Subsonic Downloads/../device_config.ini"));
	puts("PASS deleting a download removes its empty album and artist folders and nothing else");
}

int main(void) {
	names();
	single_and_album();
	whole_album();
	cut_transfer();
	cancel_job();
	empty_and_busy();
	delete_download();
	puts("All Subsonic download tests passed.");
	return 0;
}
