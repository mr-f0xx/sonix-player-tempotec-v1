#include "subsonicdl.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/net/http.h"

#define CHUNK 32768
// The card is FAT/exFAT. A file name is capped like podcastdl.c caps one, and
// the whole path stays inside what the browser and the player already accept
// (511 bytes), so a downloaded track opens like any other.
#define NAME_MAX_BYTES 150
#define PATH_MAX_BYTES 511
#define ALBUM_MARKER ".subsonic-album"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static subsonicdl_state_t state = SUBSONICDL_IDLE;
static subsonicdl_progress_t progress;
static volatile bool cancel_asked;
static http_stream_t *active_stream; // guarded by `lock`; NULL outside the body loop

typedef struct {
	subsonic_entry_t *tracks;
	int count;
	int start;
} job_t;

static void reason(char *out, size_t size, const char *text) {
	if (out && size) {
		snprintf(out, size, "%s", text);
	}
}

const char *subsonicdl_root(void) {
	static char root[512];
	const char *sd = storage_sd_root();
	if (!sd || !sd[0]) {
		root[0] = '\0';
	} else if (snprintf(root, sizeof(root), "%s/Music/Subsonic Downloads", sd) >= (int)sizeof(root)) {
		root[0] = '\0'; // a card path with no room left for the folder
	}
	return root;
}

bool subsonicdl_owns(const char *path) {
	const char *base = subsonicdl_root();
	size_t n = strlen(base);
	return n > 0 && path && strncmp(path, base, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

void subsonicdl_safe_name(const char *title, const char *fallback, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	size_t cap = size - 1 < NAME_MAX_BYTES ? size - 1 : NAME_MAX_BYTES;
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)(title ? title : ""); *p && n < cap; p++) {
		unsigned char c = *p;
		if (c < 0x20 || strchr("/\\*?\"<>|", c)) {
			continue;
		}
		// "Part 1: The start" becomes "Part 1 - The start".
		if (c == ':') {
			if (p[1] == ' ' && n + 2 <= cap) {
				out[n++] = ' ';
			}
			c = '-';
		}
		out[n++] = (char)c;
	}
	// A cut inside a multi-byte character leaves its lead bytes behind.
	if (n == cap) {
		while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80) {
			n--;
		}
		if (n > 0 && ((unsigned char)out[n - 1] & 0x80)) {
			n--;
		}
	}
	out[n] = '\0';
	// FAT drops trailing dots and spaces by itself, which would make the name
	// on the card differ from the one asked for.
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) {
		out[--n] = '\0';
	}
	size_t lead = 0;
	while (out[lead] == ' ' || out[lead] == '.') {
		lead++;
	}
	if (lead) {
		memmove(out, out + lead, n - lead + 1);
	}
	if (!out[0]) {
		snprintf(out, size, "%s", fallback ? fallback : "Subsonic");
	}
}

// The extension a saved file carries, which is also what the decoder picks its
// codec by. MP3 and Opus are what the server transcodes to; Raw keeps the
// original file's own suffix when the player can read it at all. NULL means
// this setting cannot save this track; the caller supplies the reason.
static const char *extension_for(const subsonic_entry_t *track, subsonic_quality_t quality) {
	if (quality.format == SUBSONIC_FORMAT_MP3) {
		return "mp3";
	}
	if (quality.format == SUBSONIC_FORMAT_OPUS) {
		return "opus";
	}
	if (strcasecmp(track->suffix, "flac") == 0) {
		return "flac";
	}
	if (strcasecmp(track->suffix, "mp3") == 0) {
		return "mp3";
	}
	if (strcasecmp(track->suffix, "opus") == 0) {
		return "opus";
	}
	return NULL;
}

// The three names a track's file and folder are built from: the artist, the
// album title, and the title. All from the track's own tags, so the same
// track always maps to the same path and a page can ask whether it is saved.
static bool track_names(const subsonic_entry_t *track, char *artist, size_t artist_size, char *album,
						size_t album_size, char *title, size_t title_size) {
	subsonicdl_safe_name(track->artist, "Unknown artist", artist, artist_size);
	subsonicdl_safe_name(track->album, "Unknown album", album, album_size);
	subsonicdl_safe_name(track->title, "Unknown title", title, title_size);
	return true;
}

bool subsonicdl_path(const subsonic_entry_t *track, subsonic_quality_t quality, char *out, size_t size) {
	if (!track || !out || size == 0 || track->kind != SUBSONIC_TRACK) {
		return false;
	}
	const char *suffix = extension_for(track, quality);
	const char *base = subsonicdl_root();
	if (!suffix || !base[0]) {
		return false;
	}
	char artist[128], album[200], title[200], stem[340];
	track_names(track, artist, sizeof(artist), album, sizeof(album), title, sizeof(title));
	if (snprintf(stem, sizeof(stem), "%s - %s", artist, title) >= (int)sizeof(stem)) {
		return false;
	}
	int n = snprintf(out, size, "%s/%s/%s/%s.%s", base, artist, album, stem, suffix);
	return n > 0 && n <= PATH_MAX_BYTES && n < (int)size;
}

bool subsonicdl_exists(const subsonic_entry_t *track, subsonic_quality_t quality) {
	char path[768];
	struct stat st;
	return subsonicdl_path(track, quality, path, sizeof(path)) && stat(path, &st) == 0 &&
		   S_ISREG(st.st_mode) && st.st_size > 0;
}

// Whether the card is still the one the job started on, mounted and writable.
// Checked between reads so an ejected card or a USB export stops the download
// instead of writing into a path that no longer means anything.
// `card` is the card root the job was started on, not the folder inside it:
// the point is to notice a card that was swapped or unmounted, not to test the
// path a file is about to be written to.
static bool card_ok(const char *card) {
	return storage_card_attached() && storage_sd_writable() && !usb_storage_active() && storage_sd_root() &&
		   card[0] && strcmp(storage_sd_root(), card) == 0;
}

static bool folder_ready(const char *folder) {
	struct stat st;
	if (mkdir(folder, 0755) == 0) {
		return true;
	}
	return stat(folder, &st) == 0 && S_ISDIR(st.st_mode);
}

// The album's identity, kept in a hidden marker inside its folder: the
// server's album id when it sends one, the album title when it does not. It is
// what keeps two different albums that share an artist and a title apart, and
// what makes a second download of the same album find the folder the first one
// made. An opaque server id never appears in a visible name.
static void album_identity(const subsonic_entry_t *track, char *out, size_t size) {
	if (track->album_id[0]) {
		snprintf(out, size, "%s", track->album_id);
	} else {
		subsonicdl_safe_name(track->album, "Unknown album", out, size);
	}
}

static bool marker_matches(const char *folder, const char *identity) {
	char marker[800], saved[SUBSONIC_ID_MAX + 2];
	snprintf(marker, sizeof(marker), "%s/%s", folder, ALBUM_MARKER);
	FILE *f = fopen(marker, "r");
	if (!f) {
		return false; // not a folder this module made for this album
	}
	bool read = fgets(saved, sizeof(saved), f) != NULL;
	fclose(f);
	if (!read) {
		return false;
	}
	saved[strcspn(saved, "\r\n")] = '\0';
	return strcmp(saved, identity) == 0;
}

static bool write_marker(const char *folder, const char *identity) {
	char marker[800], tmp[832];
	snprintf(marker, sizeof(marker), "%s/%s", folder, ALBUM_MARKER);
	snprintf(tmp, sizeof(tmp), "%s.part", marker);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return false;
	}
	bool ok = fprintf(f, "%s\n", identity) > 0;
	if (fclose(f) != 0) {
		ok = false;
	}
	if (!ok || rename(tmp, marker) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

// Makes (or finds) the album folder for `track` and returns it in `out`. False
// when the card went away or nothing could be made: there is nowhere to save
// the download then.
//
// `<root>/<artist>/<album>`. A folder that already exists is used only when
// its marker names this same album; otherwise the next numbered name is tried,
// so two albums never mix and a folder somebody made by hand is not written
// into.
static bool album_folder(const subsonic_entry_t *track, const char *card, char *out, size_t size) {
	const char *base = subsonicdl_root();
	if (!base[0] || !card_ok(card)) {
		return false;
	}
	// `<card>/Music` is where every service's downloads live. It may already
	// be there for the library's own reasons; making it is only about the
	// first download on a fresh card.
	char music[560];
	if (snprintf(music, sizeof(music), "%.500s/Music", storage_sd_root()) >= (int)sizeof(music) ||
		!folder_ready(music) || !folder_ready(base)) {
		return false;
	}
	char artist[128], album[200], title[200], identity[SUBSONIC_ID_MAX];
	track_names(track, artist, sizeof(artist), album, sizeof(album), title, sizeof(title));
	album_identity(track, identity, sizeof(identity));
	if (snprintf(out, size, "%s/%s", base, artist) >= (int)size || !folder_ready(out)) {
		return false;
	}
	size_t artist_len = strlen(out);
	for (int n = 1; n <= 20; n++) {
		int written = n == 1 ? snprintf(out + artist_len, size - artist_len, "/%s", album)
							 : snprintf(out + artist_len, size - artist_len, "/%s (%d)", album, n);
		if (written < 0 || written >= (int)(size - artist_len)) {
			return false;
		}
		struct stat st;
		if (mkdir(out, 0755) == 0) {
			return write_marker(out, identity);
		}
		if (stat(out, &st) != 0 || !S_ISDIR(st.st_mode)) {
			return false; // a file is in the way: not ours to move
		}
		if (marker_matches(out, identity)) {
			return true;
		}
	}
	return false;
}

// The album's artwork, once per folder: cover.jpg or cover.png by what the
// bytes are, which is what the player and the thumbnails look for. Bounded and
// written atomically; an HTML error page must never be left as a sleeve. A
// missing cover is not a failed download.
static void fetch_cover(const char *folder, const subsonic_entry_t *track, const char *root) {
	char jpg[800], png[800], part[816];
	snprintf(jpg, sizeof(jpg), "%s/cover.jpg", folder);
	snprintf(png, sizeof(png), "%s/cover.png", folder);
	struct stat st;
	if ((stat(jpg, &st) == 0 && st.st_size > 0) || (stat(png, &st) == 0 && st.st_size > 0)) {
		return;
	}
	char url[SUBSONIC_URL_MAX], error[192] = "";
	if (!subsonic_cover_url(track->id, url, sizeof(url), error, sizeof(error))) {
		return;
	}
	snprintf(part, sizeof(part), "%s/.cover.part", folder);
	http_stream_t *stream = calloc(1, sizeof(*stream));
	if (!stream) {
		memset(url, 0, sizeof(url));
		return;
	}
	http_stream_options_t opts = {.no_icy = true, .no_https_downgrade = true};
	bool opened = http_stream_open_ex(stream, url, 30, &opts);
	memset(url, 0, sizeof(url)); // the URL carries a token; it is not kept
	FILE *f = opened ? fopen(part, "wb") : NULL;
	unsigned char buf[8192], head[8] = {0};
	size_t head_len = 0;
	uint64_t done = 0;
	bool ok = f != NULL;
	while (ok) {
		if (!card_ok(root)) {
			ok = false;
			break;
		}
		int n = http_stream_read(stream, buf, sizeof(buf));
		if (n < 0) {
			ok = false;
			break;
		}
		if (!n) {
			break;
		}
		if (done + (uint64_t)n > 4u * 1024 * 1024) {
			ok = false; // a cover is a picture, not an album of them
			break;
		}
		if (head_len < sizeof(head)) {
			size_t take = (size_t)n < sizeof(head) - head_len ? (size_t)n : sizeof(head) - head_len;
			memcpy(head + head_len, buf, take);
			head_len += take;
		}
		if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
			ok = false;
			break;
		}
		done += (unsigned)n;
	}
	if (f && fclose(f) != 0) {
		ok = false;
	}
	if (!done || (stream->content_length64 && done != stream->content_length64)) {
		ok = false;
	}
	if (opened) {
		http_stream_close(stream);
	}
	free(stream);
	bool is_jpg = head_len >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff;
	bool is_png = head_len >= 8 && !memcmp(head, "\x89PNG\r\n\x1a\n", 8);
	if (!is_jpg && !is_png) {
		ok = false;
	}
	if (ok && rename(part, is_jpg ? jpg : png) != 0) {
		ok = false;
	}
	if (!ok) {
		remove(part);
	}
}

static void tag_value(FILE *f, const char *key, const char *value) {
	fprintf(f, "%s=", key);
	for (const char *p = value; *p; ++p) {
		fputc(*p == '\n' || *p == '\r' ? ' ' : *p, f);
	}
	fputc('\n', f);
}

// One file: headers, body, rename, sidecar. Runs on the download thread only.
static bool download(const job_t *job, const subsonic_entry_t *track, const char *root, char *current_folder,
					 size_t current_size) {
	(void)job;
	if (!track || track->kind != SUBSONIC_TRACK || !track->id[0]) {
		reason(progress.error, sizeof(progress.error), "subsonic_invalid_id");
		return false;
	}
	subsonic_quality_t quality = subsonic_quality();
	const char *suffix = extension_for(track, quality);
	if (!suffix) {
		reason(progress.error, sizeof(progress.error), "subsonic_raw_unsupported");
		return false;
	}
	if (!card_ok(root)) {
		reason(progress.error, sizeof(progress.error), "subsonic_no_card");
		return false;
	}
	char folder[768];
	if (!album_folder(track, root, folder, sizeof(folder))) {
		reason(progress.error, sizeof(progress.error), "subsonic_no_card");
		return false;
	}
	if (strcmp(folder, current_folder) != 0) {
		snprintf(current_folder, current_size, "%s", folder);
		fetch_cover(folder, track, root); // once per folder, and never fatal
	}

	char artist[128], album[200], title[200], stem[340];
	track_names(track, artist, sizeof(artist), album, sizeof(album), title, sizeof(title));
	if (snprintf(stem, sizeof(stem), "%s - %s", artist, title) >= (int)sizeof(stem)) {
		reason(progress.error, sizeof(progress.error), "subsonic_invalid_response");
		return false;
	}
	char path[768], part[800], tags[792], tagpart[816];
	int written = snprintf(path, sizeof(path), "%s/%s.%s", folder, stem, suffix);
	if (written < 0 || written >= (int)sizeof(path)) {
		reason(progress.error, sizeof(progress.error), "subsonic_invalid_response");
		return false;
	}
	struct stat existing;
	if (stat(path, &existing) == 0 && existing.st_size > 0) {
		// Already there. A re-download must not replace a finished file with
		// one that fails halfway, and the album loop counts it as done.
		return true;
	}
	snprintf(part, sizeof(part), "%s.part", path);
	snprintf(tags, sizeof(tags), "%s.tags", path);
	snprintf(tagpart, sizeof(tagpart), "%s.part", tags);
	remove(part);   // a leftover from a run that was cut short
	remove(tagpart);

	char url[SUBSONIC_URL_MAX], error[192] = "";
	if (!subsonic_stream_url(track->id, quality, url, sizeof(url), error, sizeof(error))) {
		reason(progress.error, sizeof(progress.error), error[0] ? error : "subsonic_stream_failed");
		return false;
	}
	http_stream_t *stream = calloc(1, sizeof(*stream));
	if (!stream) {
		reason(progress.error, sizeof(progress.error), "subsonic_out_of_memory");
		memset(url, 0, sizeof(url));
		return false;
	}
	http_stream_options_t opts = {.no_icy = true, .no_https_downgrade = true};
	bool opened = http_stream_open_ex(stream, url, 30, &opts);
	memset(url, 0, sizeof(url)); // never keep the token-bearing URL around
	bool ok = opened;
	if (!ok) {
		reason(progress.error, sizeof(progress.error), "subsonic_stream_refused");
	}
	pthread_mutex_lock(&lock);
	progress.bytes_total = opened ? stream->content_length64 : 0;
	progress.bytes_done = 0;
	active_stream = stream;
	pthread_mutex_unlock(&lock);
	FILE *f = ok ? fopen(part, "wb") : NULL;
	uint64_t done = 0;
	char *buf = f ? malloc(CHUNK) : NULL;
	if (ok && (!f || !buf)) {
		ok = false;
		reason(progress.error, sizeof(progress.error), "subsonic_download_write_failed");
	}
	while (ok) {
		if (cancel_asked) {
			ok = false;
			reason(progress.error, sizeof(progress.error), "subsonic_download_cancelled");
			break;
		}
		if (!card_ok(root)) {
			ok = false;
			reason(progress.error, sizeof(progress.error), "subsonic_no_card");
			break;
		}
		int n = http_stream_read(stream, buf, CHUNK);
		if (n < 0) {
			ok = false;
			reason(progress.error, sizeof(progress.error), "subsonic_stream_failed");
			break;
		}
		if (!n) {
			break;
		}
		if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
			ok = false;
			reason(progress.error, sizeof(progress.error), "subsonic_download_write_failed");
			break;
		}
		done += (uint64_t)n;
		pthread_mutex_lock(&lock);
		progress.bytes_done = done;
		pthread_mutex_unlock(&lock);
	}
	free(buf);
	if (f && fclose(f) != 0) {
		ok = false;
	}
	if (!done) {
		ok = false;
	} else if (stream->content_length64 && done != stream->content_length64) {
		ok = false;
		reason(progress.error, sizeof(progress.error), "subsonic_stream_failed");
	}
	pthread_mutex_lock(&lock);
	active_stream = NULL;
	pthread_mutex_unlock(&lock);
	if (opened) {
		http_stream_close(stream);
	}
	free(stream);
	if (!ok) {
		remove(part);
		remove(tagpart);
		return false;
	}
	// The sidecar wins over the file's own tags (metadata.c reads it after
	// them), which is what makes a downloaded FLAC with no tags at all read
	// correctly in the player, the browser and the details page.
	f = fopen(tagpart, "w");
	if (!f) {
		remove(part);
		reason(progress.error, sizeof(progress.error), "subsonic_download_write_failed");
		return false;
	}
	tag_value(f, "title", track->title);
	tag_value(f, "artist", track->artist);
	tag_value(f, "album", track->album);
	fprintf(f, "track=%d\n", track->track_number);
	fprintf(f, "duration=%d\n", track->duration);
	fprintf(f, "subsonic_id=%s\n", track->id);
	bool tags_ok = fclose(f) == 0;
	if (!tags_ok || rename(tagpart, tags) != 0 || rename(part, path) != 0) {
		remove(part);
		remove(tagpart);
		remove(tags);
		reason(progress.error, sizeof(progress.error), "subsonic_download_write_failed");
		return false;
	}
	return true;
}

static void *worker(void *arg) {
	job_t *job = arg;
	thread_be_low_priority("subsonic save");
	char root[512], current_folder[768] = "";
	snprintf(root, sizeof(root), "%s", storage_sd_root() ? storage_sd_root() : "");
	int completed = 0, failed = 0;
	for (int i = job->start; i < job->count && !cancel_asked; i++) {
		pthread_mutex_lock(&lock);
		snprintf(progress.title, sizeof(progress.title), "%s", job->tracks[i].title);
		progress.bytes_done = progress.bytes_total = 0;
		progress.completed = completed;
		progress.failed = failed;
		pthread_mutex_unlock(&lock);
		if (download(job, &job->tracks[i], root, current_folder, sizeof(current_folder))) {
			completed++;
		} else {
			failed++;
		}
		pthread_mutex_lock(&lock);
		progress.completed = completed;
		progress.failed = failed;
		pthread_mutex_unlock(&lock);
	}
	// The last file's reason is what the toast says. A job that ended well
	// still counts a file that failed earlier, so the reason may come from a
	// file other than the one now in progress and is never cleared between
	// them.
	subsonicdl_state_t final = cancel_asked ? SUBSONICDL_CANCELLED : failed ? SUBSONICDL_FAILED : SUBSONICDL_DONE;
	pthread_mutex_lock(&lock);
	state = final;
	progress.state = final;
	progress.running = false;
	progress.bytes_done = progress.bytes_total = 0;
	pthread_mutex_unlock(&lock);
	fprintf(stderr, "subsonicdl: %d saved, %d failed%s\n", completed, failed, cancel_asked ? ", cancelled" : "");
	free(job->tracks);
	free(job);
	return NULL;
}

bool subsonicdl_start(const subsonicdl_request_t *request) {
	if (!request || !request->tracks || request->count <= 0 || request->count > SUBSONIC_PAGE_SIZE ||
		request->start < 0 || request->start >= request->count || !subsonic_configured()) {
		return false;
	}
	if (!storage_card_attached() || !storage_sd_writable() || usb_storage_active() || !subsonicdl_root()[0]) {
		return false;
	}
	subsonic_quality_t quality = subsonic_quality();
	// A Raw download of a codec the player cannot read is refused here rather
	// than as a row of failed files in a progress card: nothing starts.
	int savable = 0;
	for (int i = request->start; i < request->count; i++) {
		if (request->tracks[i].kind == SUBSONIC_TRACK && extension_for(&request->tracks[i], quality)) {
			savable++;
		}
	}
	if (!savable) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool busy = state == SUBSONICDL_RUNNING;
	pthread_mutex_unlock(&lock);
	if (busy) {
		return false;
	}
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return false;
	}
	job->tracks = calloc(SUBSONIC_PAGE_SIZE, sizeof(*job->tracks));
	if (!job->tracks) {
		free(job);
		return false;
	}
	// Only the tracks are kept, in the order they were listed. Folders on a
	// mixed page are skipped rather than failed: the page may be a directory
	// holding sub-folders and loose songs.
	int n = 0, start = 0;
	for (int i = 0; i < request->count; i++) {
		if (request->tracks[i].kind != SUBSONIC_TRACK || !extension_for(&request->tracks[i], quality)) {
			continue;
		}
		if (i == request->start) {
			start = n;
		}
		job->tracks[n++] = request->tracks[i];
	}
	job->count = n;
	job->start = start;
	pthread_mutex_lock(&lock);
	if (state == SUBSONICDL_RUNNING) {
		pthread_mutex_unlock(&lock);
		free(job->tracks);
		free(job);
		return false;
	}
	state = SUBSONICDL_RUNNING;
	memset(&progress, 0, sizeof(progress));
	progress.state = SUBSONICDL_RUNNING;
	progress.running = true;
	progress.total_tracks = job->count;
	snprintf(progress.title, sizeof(progress.title), "%s", job->tracks[job->start].title);
	cancel_asked = false;
	pthread_mutex_unlock(&lock);
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	int rc = pthread_create(&thread, &attr, worker, job);
	pthread_attr_destroy(&attr);
	if (rc) {
		free(job->tracks);
		free(job);
		pthread_mutex_lock(&lock);
		state = SUBSONICDL_IDLE;
		progress.state = SUBSONICDL_IDLE;
		progress.running = false;
		pthread_mutex_unlock(&lock);
		return false;
	}
	return true;
}

void subsonicdl_cancel(void) {
	cancel_asked = true;
	// A read blocked on a stalled socket returns only at the receive timeout
	// unless the socket is shut under it, as podcastdl_cancel() does.
	pthread_mutex_lock(&lock);
	if (active_stream) {
		http_stream_wake(active_stream);
	}
	pthread_mutex_unlock(&lock);
}

void subsonicdl_progress(subsonicdl_progress_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&lock);
	*out = progress;
	pthread_mutex_unlock(&lock);
}

bool subsonicdl_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = state == SUBSONICDL_RUNNING;
	pthread_mutex_unlock(&lock);
	return busy;
}

void subsonicdl_acknowledge(void) {
	pthread_mutex_lock(&lock);
	if (state != SUBSONICDL_RUNNING) {
		state = SUBSONICDL_IDLE;
		progress.state = SUBSONICDL_IDLE;
	}
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// what is on the card
// ---------------------------------------------------------------------------

static int compare_entries(const void *a, const void *b) {
	const subsonicdl_entry_t *x = a, *y = b;
	return x->modified < y->modified ? 1 : x->modified > y->modified ? -1 : strcmp(x->path, y->path);
}

static void read_sidecar(const char *path, subsonicdl_entry_t *entry) {
	char tags[800], line[512];
	if (snprintf(tags, sizeof(tags), "%.760s.tags", path) >= (int)sizeof(tags)) {
		return;
	}
	FILE *f = fopen(tags, "r");
	if (!f) {
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		char *equals = strchr(line, '=');
		if (!equals) {
			continue;
		}
		*equals = '\0';
		const char *key = line, *value = equals + 1;
		if (!value[0]) {
			continue;
		}
		if (strcmp(key, "title") == 0) {
			snprintf(entry->title, sizeof(entry->title), "%s", value);
		} else if (strcmp(key, "artist") == 0) {
			snprintf(entry->artist, sizeof(entry->artist), "%s", value);
		} else if (strcmp(key, "album") == 0) {
			snprintf(entry->album, sizeof(entry->album), "%s", value);
		}
	}
	fclose(f);
}

// A file's own name without the folder and extension: its title when nothing
// better is on the card.
static void name_from_file(const char *path, char *out, size_t size) {
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	size_t n = strnlen(name, size - 1);
	memcpy(out, name, n);
	out[n] = '\0';
	char *dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = '\0';
	}
}

// The artist and album the file sits in: `.../Artist/Album/Artist - Title.ext`
// is the layout the worker writes, and it still names the album when the
// sidecar is gone.
static void folders_of(const char *dir, char *artist, size_t artist_size, char *album, size_t album_size) {
	char copy[768];
	snprintf(copy, sizeof(copy), "%s", dir);
	char *album_name = strrchr(copy, '/');
	if (!album_name) {
		return;
	}
	*album_name++ = '\0';
	char *artist_name = strrchr(copy, '/');
	if (!artist_name) {
		return;
	}
	snprintf(album, album_size, "%s", album_name);
	snprintf(artist, artist_size, "%s", artist_name + 1);
}

// Walks <root>/<artist>/<album>/<file>. Only two levels are read, which is the
// layout this module writes; a deeper tree is somebody else's.
static void scan(const char *dir, int depth, subsonicdl_entry_t *entries, int max, int *count) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char full[768];
		if (snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= (int)sizeof(full)) {
			continue;
		}
		struct stat st;
		if (lstat(full, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (depth < 2) {
				scan(full, depth + 1, entries, max, count);
			}
			continue;
		}
		if (!S_ISREG(st.st_mode) || !st.st_size) {
			continue;
		}
		const char *ext = strrchr(de->d_name, '.');
		if (!ext || (strcasecmp(ext, ".mp3") && strcasecmp(ext, ".flac") && strcasecmp(ext, ".opus"))) {
			continue;
		}
		if (*count >= max) {
			continue; // the page reads the newest first; a longer list is cut
		}
		subsonicdl_entry_t *entry = &entries[*count];
		memset(entry, 0, sizeof(*entry));
		snprintf(entry->path, sizeof(entry->path), "%s", full);
		entry->modified = (long long)st.st_mtime;
		read_sidecar(entry->path, entry);
		if (!entry->title[0]) {
			name_from_file(entry->path, entry->title, sizeof(entry->title));
		}
		if (!entry->artist[0] || !entry->album[0]) {
			folders_of(dir, entry->artist, sizeof(entry->artist), entry->album, sizeof(entry->album));
		}
		(*count)++;
	}
	closedir(d);
}

int subsonicdl_history(subsonicdl_entry_t *entries, int max) {
	if (!entries || max <= 0) {
		return 0;
	}
	const char *root = subsonicdl_root();
	if (!root[0] || !storage_card_attached()) {
		return 0;
	}
	int count = 0;
	scan(root, 0, entries, max, &count);
	qsort(entries, (size_t)count, sizeof(*entries), compare_entries);
	return count;
}

bool subsonicdl_delete(const char *path) {
	const char *root = subsonicdl_root();
	if (!path || !root[0] || !subsonicdl_owns(path) || !strcmp(path, root) || !storage_card_attached() ||
		!storage_sd_writable() || usb_storage_active()) {
		return false;
	}
	// Only the two-level album layout this module writes; a file dropped into
	// the folder by hand is left where it is.
	char folder[768];
	snprintf(folder, sizeof(folder), "%s", path);
	char *slash = strrchr(folder, '/');
	if (!slash) {
		return false;
	}
	*slash = '\0';
	struct stat st;
	if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return false;
	}
	if (unlink(path) != 0) {
		return false;
	}
	char tags[800];
	if (snprintf(tags, sizeof(tags), "%.760s.tags", path) < (int)sizeof(tags)) {
		unlink(tags);
	}
	// The album folder goes when nothing but its marker and cover is left in
	// it, then the artist folder when it held only that album: the user asked
	// for this track, not for empty folders left behind.
	for (int level = 0; level < 2; level++) {
		DIR *d = opendir(folder);
		if (!d) {
			break;
		}
		bool only_art = true;
		struct dirent *de;
		while ((de = readdir(d)) && only_art) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !strcmp(de->d_name, ALBUM_MARKER) ||
				(!level && (!strcmp(de->d_name, "cover.jpg") || !strcmp(de->d_name, "cover.png")))) {
				continue;
			}
			only_art = false;
		}
		closedir(d);
		if (!only_art) {
			break;
		}
		char file[800];
		snprintf(file, sizeof(file), "%s/%s", folder, ALBUM_MARKER);
		unlink(file);
		if (!level) {
			snprintf(file, sizeof(file), "%s/cover.jpg", folder);
			unlink(file);
			snprintf(file, sizeof(file), "%s/cover.png", folder);
			unlink(file);
		}
		if (rmdir(folder) != 0 || level) {
			break;
		}
		slash = strrchr(folder, '/');
		if (!slash) {
			break;
		}
		*slash = '\0';
	}
	return true;
}



