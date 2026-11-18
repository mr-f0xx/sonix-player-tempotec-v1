// Headless checks of the LRCLIB client (lrclib.c): query building, retry
// rules on transient HTTP statuses, free-text fallback, record ranking, and
// atomic .lrc sidecar writes.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LRCLIB_RETRY_PAUSE_SECS 0
#include "../src/system/library/lrclib.c"

// ---------------------------------------------------------------------------
// HTTP stub: URL encoder from real http.c logic + scripted responses
// ---------------------------------------------------------------------------

char *http_url_encode(const char *s, char *out, size_t out_size) {
	static const char hex[] = "0123456789ABCDEF";
	size_t w = 0;
	for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && w + 1 < out_size; p++) {
		unsigned char c = *p;
		bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
						  c == '-' || c == '_' || c == '.' || c == '~';
		if (unreserved) {
			out[w++] = (char)c;
		} else if (w + 3 < out_size) {
			out[w++] = '%';
			out[w++] = hex[c >> 4];
			out[w++] = hex[c & 0x0F];
		} else {
			break;
		}
	}
	if (out_size > 0) {
		out[w] = '\0';
	}
	return out;
}

typedef struct {
	bool ok;
	int status;
	const char *body;
} canned_reply_t;

static canned_reply_t replies[8];
static int reply_count;
static int call_count;
static char seen_urls[8][512];

static void reset_http(void) {
	memset(replies, 0, sizeof(replies));
	memset(seen_urls, 0, sizeof(seen_urls));
	reply_count = 0;
	call_count = 0;
}

static void push_reply(bool ok, int status, const char *body) {
	assert(reply_count < (int)(sizeof(replies) / sizeof(replies[0])));
	replies[reply_count++] = (canned_reply_t){.ok = ok, .status = status, .body = body};
}

const char *http_last_error(void) { return "stub transport"; }

bool http_get_ex(const char *url, const char *extra_headers, bool insecure_tls, char **out, size_t *out_len,
				 size_t limit, int timeout_secs, int *status_out) {
	(void)extra_headers;
	(void)insecure_tls;
	(void)limit;
	assert(timeout_secs > 0);
	int idx = call_count++;
	if (idx < (int)(sizeof(seen_urls) / sizeof(seen_urls[0]))) {
		snprintf(seen_urls[idx], sizeof(seen_urls[idx]), "%s", url);
	}
	assert(idx < reply_count);
	*status_out = replies[idx].status;
	if (!replies[idx].ok) {
		*out = NULL;
		if (out_len) {
			*out_len = 0;
		}
		return false;
	}
	const char *body = replies[idx].body ? replies[idx].body : "";
	size_t n = strlen(body);
	*out = malloc(n + 1);
	assert(*out != NULL);
	memcpy(*out, body, n + 1);
	if (out_len) {
		*out_len = n;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_missing_tags_rejected(void) {
	reset_http();
	lrclib_record_t *recs = NULL;
	int count = -1;
	assert(lrclib_search("", "Artist", "Album", 180, &recs, &count) == LRCLIB_NO_TAGS);
	assert(recs == NULL && count == 0 && call_count == 0);
	assert(lrclib_search("Title", NULL, "Album", 180, &recs, &count) == LRCLIB_NO_TAGS);
	assert(recs == NULL && count == 0 && call_count == 0);
}

static void test_retry_on_transient_errors_only(void) {
	// 503 then 429 then 200 -> succeeds on 3rd attempt.
	reset_http();
	push_reply(false, 503, NULL);
	push_reply(false, 429, NULL);
	push_reply(true, 200,
			   "[{\"trackName\":\"Teardrop\",\"artistName\":\"Massive Attack\","
			   "\"albumName\":\"Mezzanine\",\"duration\":330,"
			   "\"syncedLyrics\":\"[00:10.00] Love, love is a verb\"}]");

	lrclib_record_t *recs = NULL;
	int count = 0;
	assert(lrclib_search("Teardrop", "Massive Attack", "Mezzanine", 330, &recs, &count) == LRCLIB_OK);
	assert(call_count == 3);
	assert(count == 1 && recs != NULL);
	assert(recs[0].synced);
	assert(strcmp(recs[0].track, "Teardrop") == 0);
	lrclib_free(recs, count);

	// 400 is non-transient -> 1 attempt, LRCLIB_NETWORK, no free-text fallback.
	reset_http();
	push_reply(false, 400, NULL);
	assert(lrclib_search("Teardrop", "Massive Attack", NULL, 330, &recs, &count) == LRCLIB_NETWORK);
	assert(call_count == 1);

	// 404 is non-transient -> 1 tag attempt + 1 free-text fallback attempt -> LRCLIB_NOT_FOUND.
	reset_http();
	push_reply(false, 404, NULL);
	push_reply(true, 200, "[]");
	assert(lrclib_search("Unknown & Rare", "A/B", NULL, 200, &recs, &count) == LRCLIB_NOT_FOUND);
	assert(call_count == 2);
	assert(strstr(seen_urls[0], "/search?track_name=Unknown%20%26%20Rare&artist_name=A%2FB") != NULL);
	assert(strstr(seen_urls[1], "/search?q=Unknown%20%26%20Rare%20A%2FB") != NULL);
}

static void test_free_text_fallback_and_instrumental(void) {
	// Tag search empty -> free-text finds a match.
	reset_http();
	push_reply(true, 200, "[]");
	push_reply(true, 200,
			   "[{\"trackName\":\"Song (Remastered)\",\"artistName\":\"Band\","
			   "\"albumName\":\"LP\",\"duration\":210,"
			   "\"plainLyrics\":\"First line\\nSecond line\"}]");
	lrclib_record_t *recs = NULL;
	int count = 0;
	assert(lrclib_search("Song", "Band", "LP", 210, &recs, &count) == LRCLIB_OK);
	assert(call_count == 2 && count == 1);
	assert(!recs[0].synced);
	assert(strcmp(recs[0].text, "First line\nSecond line") == 0);
	lrclib_free(recs, count);

	// Tag search empty and free-text network error -> preserves LRCLIB_NOT_FOUND.
	reset_http();
	push_reply(true, 200, "[]");
	push_reply(false, 0, NULL);
	assert(lrclib_search("Song", "Band", "LP", 210, &recs, &count) == LRCLIB_NOT_FOUND);

	// Instrumental-only records -> LRCLIB_INSTRUMENTAL.
	reset_http();
	push_reply(true, 200,
			   "[{\"trackName\":\"Intro\",\"artistName\":\"Band\",\"instrumental\":true,\"duration\":62}]");
	push_reply(true, 200, "[]");
	assert(lrclib_search("Intro", "Band", NULL, 62, &recs, &count) == LRCLIB_INSTRUMENTAL);
	assert(recs == NULL && count == 0);
}

static void test_title_only_search(void) {
	lrclib_record_t *recs = NULL;
	int count = -1;
	reset_http();
	assert(lrclib_search_title("", NULL, 0, &recs, &count) == LRCLIB_NO_TAGS);
	assert(lrclib_search_title(NULL, NULL, 0, &recs, &count) == LRCLIB_NO_TAGS);
	assert(recs == NULL && count == 0 && call_count == 0);

	// The cover's album and duration can rank the results, but never filter
	// the request. Both artists are selectable; the closer recording is first.
	const char *title = "L'été & moi";
	push_reply(true, 200,
			   "[{\"artistName\":\"Live band\",\"duration\":300,\"plainLyrics\":\"live\"},"
			   "{\"artistName\":\"Original artist\",\"duration\":201,\"syncedLyrics\":\"[00:01] original\"}]");
	assert(lrclib_search_title(title, "Cover album", 200, &recs, &count) == LRCLIB_OK);
	assert(call_count == 1 && count == 2);
	assert(strcmp(seen_urls[0], LRCLIB_API "/search?track_name=L%27%C3%A9t%C3%A9%20%26%20moi") == 0);
	assert(recs[0].synced && strcmp(recs[0].artist, "Original artist") == 0);
	assert(!recs[1].synced && strcmp(recs[1].artist, "Live band") == 0);
	lrclib_free(recs, count);

	// No second, free-text query when the explicit title search comes up empty.
	reset_http();
	push_reply(true, 200, "[]");
	assert(lrclib_search_title(title, NULL, 0, &recs, &count) == LRCLIB_NOT_FOUND);
	assert(call_count == 1 && recs == NULL && count == 0);
}

static void test_ranking_priority_and_stability(void) {
	reset_http();
	push_reply(
		true, 200,
		"["
		// [0] off duration (240 vs 200), synced, matching album -> 59 + 200 + 100 = 359
		"{\"trackName\":\"Cut\",\"artistName\":\"Art\",\"albumName\":\"Target Album\","
		"\"duration\":240,\"syncedLyrics\":\"[00:01.00] extended\"},"
		// [1] within 2s (201 vs 200), plain, matching album -> 1000 + 98 + 100 = 1198
		"{\"trackName\":\"Cut\",\"artistName\":\"Art\",\"albumName\":\"Target Album\","
		"\"duration\":201,\"plainLyrics\":\"plain on album\"},"
		// [2] within 2s (200 vs 200), synced, other album -> 1000 + 99 + 200 = 1299
		"{\"trackName\":\"Cut\",\"artistName\":\"Art\",\"albumName\":\"Single\","
		"\"duration\":200,\"syncedLyrics\":\"[00:01.00] single synced\"},"
		// [3] within 2s (199 vs 200), synced, case-insensitive album -> 1000 + 98 + 200 + 100 = 1398
		"{\"trackName\":\"Cut\",\"artistName\":\"Art\",\"albumName\":\"target album\","
		"\"duration\":199,\"syncedLyrics\":\"[00:01.00] best match\"},"
		// [4] identical score to [3], appears later -> stays after [3]
		"{\"trackName\":\"Cut\",\"artistName\":\"Art\",\"albumName\":\"TARGET ALBUM\","
		"\"duration\":201,\"syncedLyrics\":\"[00:01.00] tie second\"}"
		"]");

	lrclib_record_t *recs = NULL;
	int count = 0;
	assert(lrclib_search("Cut", "Art", "Target Album", 200, &recs, &count) == LRCLIB_OK);
	assert(count == 5);
	assert(strstr(recs[0].text, "best match") != NULL);
	assert(strstr(recs[1].text, "tie second") != NULL);
	assert(strstr(recs[2].text, "single synced") != NULL);
	assert(strstr(recs[3].text, "plain on album") != NULL);
	assert(strstr(recs[4].text, "extended") != NULL);
	lrclib_free(recs, count);
}

static void test_atomic_save_and_sidecar_paths(void) {
	char dir[] = "/tmp/sonix-lrclib-XXXXXX";
	assert(mkdtemp(dir) != NULL);

	char track_path[512], lrc_path[512], part_path[512];
	snprintf(track_path, sizeof(track_path), "%s/01. Artist - My.Song.flac", dir);
	snprintf(lrc_path, sizeof(lrc_path), "%s/01. Artist - My.Song.lrc", dir);
	snprintf(part_path, sizeof(part_path), "%s/01. Artist - My.Song.lrc.part", dir);

	assert(lrclib_save(track_path, "[00:01.20] Hello world"));
	struct stat st;
	assert(stat(part_path, &st) != 0);
	FILE *f = fopen(lrc_path, "rb");
	assert(f != NULL);
	char buf[128] = {0};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	// Trailing newline appended once.
	assert(n == strlen("[00:01.20] Hello world\n"));
	assert(strcmp(buf, "[00:01.20] Hello world\n") == 0);

	// Existing trailing newline is not doubled.
	assert(lrclib_save(track_path, "[00:02.00] Already newline\n"));
	f = fopen(lrc_path, "rb");
	assert(f != NULL);
	memset(buf, 0, sizeof(buf));
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	assert(n == strlen("[00:02.00] Already newline\n"));
	assert(strcmp(buf, "[00:02.00] Already newline\n") == 0);
	unlink(lrc_path);

	// Dotfile without extension appends .lrc rather than stripping leading dot.
	char dot_track[512], dot_lrc[512];
	snprintf(dot_track, sizeof(dot_track), "%s/.hidden", dir);
	snprintf(dot_lrc, sizeof(dot_lrc), "%s/.hidden.lrc", dir);
	assert(lrclib_save(dot_track, "Plain words"));
	assert(stat(dot_lrc, &st) == 0);
	unlink(dot_lrc);

	// Nonexistent directory fails cleanly.
	assert(!lrclib_save("/tmp/sonix-lrclib-does-not-exist/song.flac", "words"));

	rmdir(dir);
}

int main(void) {
	test_missing_tags_rejected();
	test_retry_on_transient_errors_only();
	test_free_text_fallback_and_instrumental();
	test_title_only_search();
	test_ranking_priority_and_stability();
	test_atomic_save_and_sidecar_paths();
	printf("lrclib: all checks passed\n");
	return 0;
}
