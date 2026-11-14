// test_ytmusic.c -- the YouTube Music playback resolver, with a fake network.
//
// The answers to "where are the bytes", tried in this order: the imported
// browser session, the Invidious/Piped instances, the device's own player
// request, and a relay of the user's own -- and the settings can pin any of
// them first. This suite links the real ytmusic.c against a fake http layer
// (real http.c needs the POSIX socket API the tests' Linux host has; the
// fakes keep the suite portable) and checks the resolver end to end:
//
//   - the instances are asked before the device in "auto": an instance that
//     can pass the bytes on plays, and the device's own reply is still used
//     when no instance can;
//   - a source that names a stream which is then refused does not end the
//     attempt: the download walks to the next source and plays from there;
//   - only audio/mp4 streams are picked, never audio/webm (the player reads
//     AAC in MP4; its Opus decoder reads Ogg, not the WebM YouTube wraps Opus
//     in) and never anything carrying video;
//   - the highest bitrate at or under the cap wins;
//   - title, artist and duration come from whichever source answered;
//   - a pinned Invidious instance is played through its /latest_version
//     endpoint, a Piped one only when its own address is what it answers with,
//     and an instance that merely names Google's address is not used;
//   - the settings list holds the built-in answers, the relay only once one is
//     configured, and the instances -- so the page has something to offer on a
//     player that was never set up;
//   - the error left standing says every source refused, not that a relay is
//     missing, because a relay is no longer required.
//
// Guest search against the live endpoint is checked by hand with the same
// two curl invocations the relay documentation carries; this suite cannot
// reach the network, by design.

#include "win_compat_host.h"

#include <errno.h>
#ifndef _WIN32
#include <unistd.h> // rmdir()
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/system/core/config.h"
#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/streaming/ytlikes.h"
#include "src/system/streaming/ytmusic.h"
#include "src/system/streaming/ytpodcastplayed.h"

// The shared region list asks lang.c for a row's label. That file is the whole
// translation tree -- it relabels every widget in the interface -- so it is not
// part of this suite; the identity tr() the interface falls back to when no
// language file is readable is what stands in its place here.
const char *tr(const char *text) { return text; }

static int failures;

#define CHECK(cond)                                                         \
	do {                                                                    \
		if (!(cond)) {                                                        \
			failures++;                                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
		}                                                                     \
	} while (0)

// ---------------------------------------------------------------------------
// the fake http layer
//
// Every URL the resolver asks for is matched against a table of canned
// replies. `relay_calls` counts the requests that reached the relay, so a
// test can see the phone-app player was tried first and refused.
// ---------------------------------------------------------------------------

typedef struct {
	const char *url;	// matched with strstr, so the port-free base works
	int status;			// HTTP status, or 0 = transport failure
	const char *body;
	// Bytes, not a C string: an MP4 body has zero bytes of its own (box sizes
	// and audio frames), and one measured with strlen() would be cut off at
	// the first of them.
	size_t len;
} fake_reply_t;

static fake_reply_t replies[16];
static int reply_count;
static int relay_calls;
// When set, a ranged reply carries at most this many bytes, which is what a
// proxy that hands back less than was asked for looks like from here.
static size_t fake_range_cap;

static const fake_reply_t *find_reply(const char *url) {
	for (int i = 0; i < reply_count; i++) {
		if (replies[i].url[0] && strstr(url, replies[i].url)) return &replies[i];
	}
	return NULL;
}

static void fake_reset(void) {
	reply_count = 0;
	relay_calls = 0;
	fake_range_cap = 0;
	memset(replies, 0, sizeof(replies));
}

static void fake_reply(const char *url, int status, const char *body) {
	replies[reply_count].url = url;
	replies[reply_count].status = status;
	replies[reply_count].body = body;
	replies[reply_count].len = strlen(body);
	reply_count++;
}

// The same, for a body with binary content -- an MP4 that has to be readable
// as a container rather than as text.
static void fake_reply_bytes(const char *url, int status, const char *body, size_t len) {
	replies[reply_count].url = url;
	replies[reply_count].status = status;
	replies[reply_count].body = body;
	replies[reply_count].len = len;
	reply_count++;
}

// A copy the caller owns, NUL-terminated whether or not the body had one.
static char *fake_body_copy(const char *from, size_t len) {
	char *copy = malloc(len + 1);
	if (!copy) return NULL;
	memcpy(copy, from, len);
	copy[len] = '\0';
	return copy;
}

// A page Google really sent: the fixtures carry whole responses the device
// received (and once parsed into nothing). Loaded whole and registered with
// fake_reply() by the test that wants one.
static char *load_fixture(const char *name) {
	FILE *f = fopen(name, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *body = malloc((size_t)size + 1);
	if (!body) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(body, 1, (size_t)size, f);
	fclose(f);
	body[got] = '\0';
	return body;
}

bool http_get(const char *url, char **out, size_t *out_len, size_t limit, int timeout_secs) {
	(void)limit;
	(void)timeout_secs;
	if (strstr(url, "/streams/")) relay_calls++;
	const fake_reply_t *reply = find_reply(url);
	if (!reply || reply->status == 0) return false;
	*out = fake_body_copy(reply->body, reply->len);
	if (out_len) *out_len = reply->len;
	return true;
}

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out,
				 size_t *out_len, size_t limit, int timeout_secs, int *status_out) {
	(void)extra_headers;
	(void)want_error_body;
	(void)limit;
	(void)timeout_secs;
	if (strstr(url, "/streams/")) relay_calls++;
	if (status_out) *status_out = 0;
	const fake_reply_t *reply = find_reply(url);
	if (!reply || reply->status == 0) return false;
	if (status_out) *status_out = reply->status;
	*out = fake_body_copy(reply->body, reply->len);
	if (out_len) *out_len = reply->len;
	return true;
}

// What the last Content-Range said the whole file was. The fake network above
// answers from canned bodies and keeps no such record, so this says "not
// known", which is what a server without Content-Range leaves behind -- and
// exactly the case the downloader has to keep working without.
uint64_t http_last_content_range_total(void) { return 0; }

// A ranged request against a canned body behaves the way a server honouring
// Range does: the bytes asked for, or 416 once the offset runs past the end.
// The downloader relies on that -- a short chunk is not an ending, and only
// "nothing left" is.
bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len, size_t limit,
				  int timeout_secs, int *status_out) {
	(void)limit;
	(void)timeout_secs;
	const fake_reply_t *reply = find_reply(url);
	if (!reply || reply->status == 0) return false;
	unsigned long long start = 0;
	if (req && req->extra_headers &&
		sscanf(req->extra_headers, "Range: bytes=%llu", &start) == 1 &&
		reply->status == 206) {
		size_t total = reply->len;
		if (start >= total) {
			if (status_out) *status_out = 416;
			*out = fake_body_copy("", 0);
			if (out_len) *out_len = 0;
			return true;
		}
		if (status_out) *status_out = 206;
		size_t avail = total - (size_t)start;
		if (fake_range_cap && avail > fake_range_cap) avail = fake_range_cap;
		*out = fake_body_copy(reply->body + start, avail);
		if (out_len) *out_len = avail;
		return true;
	}
	if (status_out) *status_out = reply->status;
	*out = fake_body_copy(reply->body, reply->len);
	if (out_len) *out_len = reply->len;
	return true;
}

bool http_stream_open_ex(http_stream_t *st, const char *url, int timeout_secs,
						 const http_stream_options_t *options) {
	(void)st;
	(void)url;
	(void)timeout_secs;
	(void)options;
	return false; // the download path is not under test here
}

int http_stream_read(http_stream_t *st, void *buf, int len) {
	(void)st;
	(void)buf;
	(void)len;
	return -1;
}

void http_stream_close(http_stream_t *st) { (void)st; }

const char *http_last_error(void) { return "fake network: no route"; }

char *http_url_encode(const char *in, char *out, size_t out_size) {
	// Only the OAuth flow uses this; the tests exercise none of it. A plain
	// copy keeps the symbol defined without pretending to be real.
	snprintf(out, out_size, "%s", in ? in : "");
	return out;
}

// ---------------------------------------------------------------------------
// the canned replies
//
// The relay body is Piped's /streams shape, trimmed to what the resolver
// reads: the audio streams (a webm it must pass over, one over the cap, and
// the AAC pair it must choose from) and the facts the reply carries. The
// player body is what the phone-app client answers today: a refusal.
// ---------------------------------------------------------------------------

#define VID "dQw4w9WgXcQ"

// Where the playback cache is written. On the device it is \tmp; a Windows host
// can refuse every create under that folder (ACLs, or security software
// answering EACCES), so the suite points SONIX_YTMUSIC_CACHE at a folder it owns
// and checks the downloads there instead of skipping them.
#define YTM_CACHE ".build/ytmusic-cache"
#define YTM_CACHE_FRESH ".build/ytmusic-cache-fresh"
#define YTM_CACHE_PROBE YTM_CACHE "/probe.txt"

static char cache_env[] = "SONIX_YTMUSIC_CACHE=" YTM_CACHE;
static char fresh_cache_env[] = "SONIX_YTMUSIC_CACHE=" YTM_CACHE_FRESH;

static const char RELAY_GOOD[] =
	"{\"title\":\"Never Gonna Give You Up\",\"uploader\":\"Rick Astley\",\"duration\":213,"
	"\"audioStreams\":["
	"{\"url\":\"https://relay.example/webm-160\",\"format\":\"WEBM_OPUS\",\"mimeType\":\"audio/webm; codecs=opus\",\"bitrate\":160000,\"itag\":251},"
	"{\"url\":\"https://relay.example/mp4-400\",\"format\":\"M4A\",\"mimeType\":\"audio/mp4; codecs=mp4a.40.2\",\"bitrate\":400000,\"itag\":999},"
	"{\"url\":\"https://relay.example/mp4-128\",\"format\":\"M4A\",\"mimeType\":\"audio/mp4; codecs=mp4a.40.2\",\"bitrate\":128000,\"itag\":140},"
	"{\"url\":\"https://relay.example/mp4-256\",\"format\":\"M4A\",\"mimeType\":\"audio/mp4; codecs=mp4a.40.2\",\"bitrate\":256000,\"itag\":141}"
	"],\"videoStreams\":[{\"url\":\"https://relay.example/video\",\"mimeType\":\"video/mp4\",\"bitrate\":1000000}]}";

static const char RELAY_WEBM_ONLY[] =
	"{\"title\":\"x\",\"uploader\":\"y\",\"duration\":1,"
	"\"audioStreams\":[{\"url\":\"https://relay.example/webm\",\"mimeType\":\"audio/webm; codecs=opus\",\"bitrate\":160000}]}";

static const char RELAY_GARBAGE[] = "this is not json at all";

static const char ANDROID_REFUSED[] =
	"{\"error\":{\"code\":400,\"message\":\"Precondition check failed.\",\"reason\":\"failedPrecondition\"}}";

// An Invidious instance's formats reply, trimmed to what the resolver reads:
// the facts, and three adaptive formats -- the Opus track it must pass over,
// a low-bitrate AAC one, and the 128k AAC one a track is played from.
static const char INVIDIOUS_FORMATS[] =
	"{\"title\":\"Never Gonna Give You Up\",\"author\":\"Rick Astley\",\"lengthSeconds\":213,"
	"\"adaptiveFormats\":["
	"{\"itag\":251,\"type\":\"audio/webm; codecs=\\\"opus\\\"\",\"bitrate\":160000,\"url\":\"https://inv.test/x\"},"
	"{\"itag\":139,\"type\":\"audio/mp4; codecs=\\\"mp4a.40.5\\\"\",\"bitrate\":48000,\"url\":\"https://inv.test/y\"},"
	"{\"itag\":140,\"type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\",\"bitrate\":128000,"
	"\"clen\":\"4095\",\"url\":\"https://inv.test/z\"}"
	"]}";

// A Piped instance that fronts the bytes itself, which is the only kind of
// Piped answer this player can fetch. The webm is passed over, as always.
static const char PIPED_OWN_HOST[] =
	"{\"title\":\"Never Gonna Give You Up\",\"uploader\":\"Rick Astley\",\"duration\":213,"
	"\"audioStreams\":["
	"{\"url\":\"https://piped.test/webm\",\"mimeType\":\"audio/webm; codecs=opus\",\"bitrate\":160000},"
	"{\"url\":\"https://piped.test/audio/140\",\"mimeType\":\"audio/mp4; codecs=mp4a.40.2\",\"bitrate\":128000}"
	"]}";

// The same instance handing out Google's address, which belongs to it and not
// to this device: nothing here can be fetched, so it is not the answer.
static const char PIPED_GOOGLE_ADDRESS[] =
	"{\"title\":\"x\",\"uploader\":\"y\",\"duration\":1,"
	"\"audioStreams\":[{\"url\":\"https://rr3---sn-abc.googlevideo.com/videoplayback?itag=140\","
	"\"mimeType\":\"audio/mp4; codecs=mp4a.40.2\",\"bitrate\":128000}]}";

// ---------------------------------------------------------------------------
// the tests
// ---------------------------------------------------------------------------

static void set_relay(const char *base) { config_set("ytmusic", "relay_base", base); }

static void test_nothing_configured_says_what_to_do(void) {
	set_relay("");
	CHECK(ytmusic_source_select("auto"));
	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
	CHECK(!ok);
	// The device is asked first and refused, no instance answers, and there is
	// no relay either: the error says so without naming a relay as the only
	// way out, because it is not one any more.
	CHECK(strcmp(error, "ytmusic_no_source") == 0);
}

static void test_relay_picks_the_aac_stream(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED); // the phone-app player, as today
	fake_reply("relay.test/streams/", 200, RELAY_GOOD);
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	int duration = 0;
	char title[YTMUSIC_TITLE_MAX] = "", artist[YTMUSIC_NAME_MAX] = "";
	bool proxied = false;
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), &duration, title, sizeof(title), artist,
								sizeof(artist), &proxied, error, sizeof(error));
	CHECK(ok);
	// The reply names another machine, so the bytes are asked of the relay, and
	// the address carries the format this end chose: 256k beats 128k, the webm
	// is skipped and the 400k stream is over the cap.
	CHECK(strcmp(url, "http://relay.test/audio/" VID "?f=141") == 0);
	CHECK(proxied);
	CHECK(duration == 213);
	CHECK(strcmp(title, "Never Gonna Give You Up") == 0);
	CHECK(strcmp(artist, "Rick Astley") == 0);
}

// The instance comes before the device in "auto". The device's own player
// request answers with an address Google then refuses to serve -- 403 on every
// media fetch, which is what the field log showed -- so an instance that can
// pass the bytes on is the answer that actually plays. The device is still in
// the walk, and the relay is still last.
static void test_instances_are_asked_before_the_device(void) {
	char *player = load_fixture("fixtures/player-android-dQw4w9WgXcQ.json");
	if (!player) {
		CHECK(0 && "the phone-app player fixture is missing");
		return;
	}
	fake_reset();
	fake_reply("inv.test/api/v1/videos/", 200, INVIDIOUS_FORMATS);
	fake_reply("youtubei/v1/player", 200, player); // the device would answer as well
	fake_reply("relay.test/streams/", 200, RELAY_GOOD);
	set_relay("http://relay.test");
	// A list of one instance, so the built-in hosts -- which this fake network
	// does not answer -- are not part of the question.
	config_set("ytmusic", "instances", "https://inv.test");
	CHECK(ytmusic_source_select("auto"));

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error,
								sizeof(error));
	CHECK(ok);
	if (!ok) fprintf(stderr, "  nothing played: %s\n", error);
	CHECK(strcmp(url, "https://inv.test/latest_version?id=" VID "&itag=140&local=true") == 0);

	// With the instance answering nothing, the device's own reply is what plays.
	fake_reset();
	fake_reply("youtubei/v1/player", 200, player);
	ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error,
							sizeof(error));
	CHECK(ok);
	CHECK(strstr(url, "googlevideo.com") != NULL);

	config_set("ytmusic", "instances", "");
	set_relay("");
	free(player);
}

// The downloader walks the same list, which is the part that made a play fail
// even when a source had resolved: a source can name a stream that is then
// refused. Here the first instance refuses its bytes and the second one serves
// them -- in pieces, the way a proxy that hands back less than was asked for
// does. The file must still hold every byte: a short piece is not an ending,
// and taking it for one is what ended a track after a minute.

static char AUDIO_BYTES[4096];
static char COVER_BYTES[4096];

static void test_download_walks_past_a_refused_stream(void) {
	memset(AUDIO_BYTES, 'A', sizeof(AUDIO_BYTES) - 1);
	AUDIO_BYTES[sizeof(AUDIO_BYTES) - 1] = '\0';
	fake_reset();
	fake_reply("inv.test/api/v1/videos/", 200, INVIDIOUS_FORMATS);
	fake_reply("inv.test/latest_version", 403, "<html>403: the download is not available</html>");
	fake_reply("inv2.test/api/v1/videos/", 200, INVIDIOUS_FORMATS);
	fake_reply("inv2.test/latest_version", 206, AUDIO_BYTES);
	// Every ranged reply carries 1000 bytes of the 4095 asked for.
	fake_range_cap = 1000;
	// The walk cannot be checked where a file cannot be written at all.
	FILE *writable = fopen(YTM_CACHE_PROBE, "wb");
	if (!writable) {
		fprintf(stderr, "  skipped: this host cannot write %s (%s)\n", YTM_CACHE,
				strerror(errno));
		return;
	}
	fclose(writable);
	remove(YTM_CACHE_PROBE);
	char stale[600];
	snprintf(stale, sizeof(stale), "%s/%s.m4a", YTM_CACHE, VID);
	remove(stale);
	snprintf(stale, sizeof(stale), "%s/%s.done", YTM_CACHE, VID);
	remove(stale);
	snprintf(stale, sizeof(stale), "%s/%s.jpg", YTM_CACHE, VID);
	remove(stale);
	snprintf(stale, sizeof(stale), "%s/%s.tags", YTM_CACHE, VID);
	remove(stale);
	config_set("ytmusic", "instances", "https://inv.test,https://inv2.test");
	set_relay("");
	CHECK(ytmusic_source_select("auto"));

	ytmusic_track_t track;
	memset(&track, 0, sizeof(track));
	snprintf(track.id, sizeof(track.id), "%s", VID);
	snprintf(track.title, sizeof(track.title), "%s", "Never Gonna Give You Up");
	// The row's sleeve, which the download writes beside the audio.
	snprintf(track.thumbnail, sizeof(track.thumbnail), "%s",
			 "https://lh3.test/art=w120-h120-l90-rj");
	snprintf(COVER_BYTES, sizeof(COVER_BYTES), "%s", AUDIO_BYTES);
	fake_reply("lh3.test/art", 200, COVER_BYTES);
	char path[512] = "", error[192] = "";
	bool ok = ytmusic_download_for_playback(&track, path, sizeof(path), error, sizeof(error));
	CHECK(ok);
	if (!ok) fprintf(stderr, "  the download gave up: %s\n", error);
	CHECK(strstr(path, VID) != NULL);
	FILE *f = ok ? fopen(path, "rb") : NULL;
	CHECK(f != NULL);
	if (f) {
		fseek(f, 0, SEEK_END);
		CHECK(ftell(f) == (long)sizeof(AUDIO_BYTES) - 1);
		fclose(f);
	}
	// And the sleeve beside it, which is how the player finds a file's artwork.
	char cover[600];
	snprintf(cover, sizeof(cover), "%s", path);
	char *dot = strrchr(cover, '.');
	if (dot) *dot = '\0';
	snprintf(cover + strlen(cover), sizeof(cover) - strlen(cover), ".jpg");
	FILE *art = fopen(cover, "rb");
	CHECK(art != NULL);
	if (art) {
		fseek(art, 0, SEEK_END);
		CHECK(ftell(art) == (long)sizeof(COVER_BYTES) - 1);
		fclose(art);
	}

	char tags[600];
	snprintf(tags, sizeof(tags), "%s.tags", path);
	remove(path);
	remove(tags);
	remove(cover);
	config_set("ytmusic", "instances", "");
}

// The phone-app player's reply, byte for byte as YouTube sent it to this
// device's own request shape, replayed through the real parser. This is the
// standalone path: the player resolves the address itself and the bytes come
// straight from Google, so a track plays with no relay and no other machine.
static void test_phone_player_reply_plays_on_its_own(void) {
	char *player = load_fixture("fixtures/player-android-dQw4w9WgXcQ.json");
	if (!player) {
		CHECK(0 && "the phone-app player fixture is missing");
		return;
	}
	ytmusic_logout(); // no account, no relay: nothing but the reply
	fake_reset();
	fake_reply("youtubei/v1/player", 200, player);
	set_relay("");

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	bool relayed = true;
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, &relayed, error,
								sizeof(error));
	CHECK(ok);
	if (!ok) fprintf(stderr, "  the phone-app reply gave no stream: %s\n", error);
	CHECK(strncmp(url, "https://", 8) == 0);
	CHECK(url[0] && strstr(url, "googlevideo.com") != NULL);
	CHECK(!relayed); // the address is Google's, resolved here, and fetched as it stands
	free(player);
}

// An instance named in the settings is what plays the track. Invidious answers
// the formats reply, and its own /latest_version endpoint -- local=true -- is
// asked for the bytes: the instance fetches the audio and passes it on, which
// is the one instance shape a different machine can use.
// A pin to the relay asks the relay and nothing else -- the device would answer
// this one, and the point of the pin is that the relay is what is wanted.
static void test_relay_pin_asks_only_the_relay(void) {
	char *player = load_fixture("fixtures/player-android-dQw4w9WgXcQ.json");
	if (!player) {
		CHECK(0 && "the phone-app player fixture is missing");
		return;
	}
	fake_reset();
	fake_reply("youtubei/v1/player", 200, player); // a playable answer, left unused
	fake_reply("relay.test/streams/", 200, RELAY_GOOD);
	set_relay("http://relay.test");
	CHECK(ytmusic_source_select("relay"));

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error,
								sizeof(error));
	CHECK(ok);
	CHECK(strncmp(url, "http://relay.test/audio/", 24) == 0);
	CHECK(ytmusic_source_select("auto"));
	set_relay("");
	free(player);
}

static void test_pinned_invidious_instance_plays(void) {
	fake_reset();
	fake_reply("inv.test/api/v1/videos/", 200, INVIDIOUS_FORMATS);
	set_relay("");
	CHECK(ytmusic_source_select("https://inv.test"));

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	char title[YTMUSIC_TITLE_MAX] = "", artist[YTMUSIC_NAME_MAX] = "";
	int duration = 0;
	bool proxied = false;
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), &duration, title, sizeof(title), artist,
								sizeof(artist), &proxied, error, sizeof(error));
	CHECK(ok);
	if (!ok) fprintf(stderr, "  the instance gave no stream: %s\n", error);
	CHECK(strcmp(url, "https://inv.test/latest_version?id=" VID "&itag=140&local=true") == 0);
	CHECK(proxied);
	CHECK(duration == 213);
	CHECK(strcmp(title, "Never Gonna Give You Up") == 0);
	CHECK(strcmp(artist, "Rick Astley") == 0);
	CHECK(ytmusic_source_select("auto"));
}

// A Piped instance fronting the bytes itself: its own address is fetched as it
// stands, so nothing is proxied and the address is kept whole.
static void test_pinned_piped_instance_plays(void) {
	fake_reset();
	fake_reply("piped.test/streams/", 200, PIPED_OWN_HOST);
	set_relay("");
	CHECK(ytmusic_source_select("https://piped.test"));

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	bool proxied = true;
	CHECK(ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, &proxied, error,
								sizeof(error)));
	CHECK(strcmp(url, "https://piped.test/audio/140") == 0);
	CHECK(!proxied);
	CHECK(ytmusic_source_select("auto"));
}

// An instance that only names Google's address cannot play the track from
// here, however healthy it is; the pin is not honoured with an address that
// will be refused, and the failure is named rather than silent.
static void test_instance_naming_google_is_not_the_answer(void) {
	fake_reset();
	fake_reply("piped.test/streams/", 200, PIPED_GOOGLE_ADDRESS);
	set_relay("");
	CHECK(ytmusic_source_select("https://piped.test"));

	char url[YTMUSIC_URL_MAX] = "", error[192] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error,
								sizeof(error));
	CHECK(!ok);
	CHECK(url[0] == '\0');
	CHECK(strcmp(error, "ytmusic_no_source") == 0);
	CHECK(ytmusic_source_select("auto"));
}

// The settings page lists exactly what the resolver can use: the default, the
// device on its own, a relay once one is configured, and the instances -- the
// built-in public list when nothing was ever set, so the page has something to
// choose from out of the box.
static void test_sources_list_the_choices(void) {
	set_relay("");
	CHECK(ytmusic_source_select("auto"));
	ytmusic_source_t sources[YTMUSIC_SOURCES_MAX];
	int count = ytmusic_sources(sources, YTMUSIC_SOURCES_MAX);
	CHECK(count >= 3);
	CHECK(strcmp(sources[0].value, "auto") == 0);
	CHECK(strcmp(sources[0].label, "ytmusic_source_auto") == 0);
	CHECK(strcmp(sources[1].value, "direct") == 0);
	bool has_relay = false, has_instance = false;
	for (int i = 0; i < count; i++) {
		if (strcmp(sources[i].value, "relay") == 0) has_relay = true;
		if (strstr(sources[i].value, "://")) has_instance = true;
	}
	CHECK(!has_relay); // none configured: no row that could not play
	CHECK(has_instance);

	set_relay("http://relay.test");
	count = ytmusic_sources(sources, YTMUSIC_SOURCES_MAX);
	has_relay = false;
	for (int i = 0; i < count; i++) {
		if (strcmp(sources[i].value, "relay") == 0) has_relay = true;
	}
	CHECK(has_relay);
	set_relay("");

	// A host typed on its own is an address: https is filled in, because that
	// is what people type and what every instance serves. Something that is
	// not a host at all is refused rather than stored.
	CHECK(!ytmusic_source_select("not a host"));
	CHECK(ytmusic_source_select("inv.example"));
	char typed[YTMUSIC_SOURCE_MAX] = "";
	ytmusic_source_active(typed, sizeof(typed));
	CHECK(strcmp(typed, "https://inv.example") == 0);
	CHECK(ytmusic_source_select("direct"));
	char active[YTMUSIC_SOURCE_MAX] = "";
	ytmusic_source_active(active, sizeof(active));
	CHECK(strcmp(active, "direct") == 0);
	CHECK(ytmusic_source_select("auto"));
}

static void test_webm_is_never_the_answer(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED);
	fake_reply("relay.test/streams/", 200, RELAY_WEBM_ONLY);
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
	CHECK(!ok);
	CHECK(strcmp(error, "ytmusic_aac_stream_unavailable") == 0);
	CHECK(url[0] == '\0');
}

static void test_unreadable_relay_names_itself(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED);
	fake_reply("relay.test/streams/", 200, RELAY_GARBAGE);
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
	CHECK(!ok);
	CHECK(strcmp(error, "ytmusic_relay_unreadable") == 0);
}

static void test_unreachable_relay_names_itself(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED);
	// No relay reply at all: the transport fails before a status is read.
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
	CHECK(!ok);
	CHECK(strcmp(error, "ytmusic_relay_unreachable") == 0);
}

// A relay's answer is fetched from the relay, or not at all. The media
// addresses a Piped or Invidious instance answers with belong to Google and to
// that instance's own address: fetched from here they are refused (403) and
// they are longer than the player's URL buffer besides, which is what "could
// not reach the stream" used to mean. See ytmusic_relay_stream_url().
static void test_stream_url_stays_on_the_relay(void) {
	char out[YTMUSIC_URL_MAX];
	bool proxied = false;

	// The relay's own address is used as it stands: tools/ytmusic-relay.py
	// answers with exactly this.
	CHECK(ytmusic_relay_stream_url("http://relay.test", VID, 140,
								   "http://relay.test/audio/" VID "?f=140", out, sizeof(out), &proxied));
	CHECK(!proxied);
	CHECK(strcmp(out, "http://relay.test/audio/" VID "?f=140") == 0);

	// Somebody else's machine: the player asks the relay for the audio instead
	// of fetching an address that will refuse it -- and names the format it
	// picked, so the relay does not choose again.
	CHECK(ytmusic_relay_stream_url("http://relay.test", VID, 140,
								   "https://rr3---sn-abc.googlevideo.com/videoplayback?itag=140", out,
								   sizeof(out), &proxied));
	CHECK(proxied);
	CHECK(strcmp(out, "http://relay.test/audio/" VID "?f=140") == 0);

	// A host that only begins with the relay's name is still somebody else's.
	CHECK(ytmusic_relay_stream_url("http://relay.test", VID, 0, "http://relay.tests.example/audio/x",
								   out, sizeof(out), &proxied));
	CHECK(proxied);
	CHECK(strcmp(out, "http://relay.test/audio/" VID) == 0);

	// Nothing to build a request from.
	CHECK(!ytmusic_relay_stream_url("", VID, 140, "https://relay.example/x", out, sizeof(out), &proxied));
	CHECK(!ytmusic_relay_stream_url("http://relay.test", "short", 140, "https://relay.example/x", out,
									sizeof(out), &proxied));
}

// And a relay with no /streams/{id} -- Invidious serves its own API shape there
// -- is named as a relay that cannot serve this player, not as a broken stream.
static void test_relay_without_the_endpoint_names_itself(void) {
	fake_reset();
	fake_reply("youtubei/v1/player", 400, ANDROID_REFUSED);
	fake_reply("relay.test/streams/", 404, "{\"error\":\"not found\"}");
	set_relay("http://relay.test");

	char url[YTMUSIC_URL_MAX] = "", error[128] = "";
	bool ok = ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error));
	CHECK(!ok);
	CHECK(strcmp(error, "ytmusic_relay_not_serving") == 0);
}

// The pages Google really sent this device, replayed through the real parser.
// These arrived whole (the bodies end like JSON and carry full shelves of
// items) and once parsed into zero rows; whatever the fault was, these bytes
// are the truth the fix has to satisfy.
static void test_real_device_pages(void) {
	char *search = load_fixture("fixtures/empty-search.json");
	if (search) {
		fake_reset();
		fake_reply("youtubei/v1/search", 200, search);
		set_relay("http://relay.test"); // harmless: search never reaches the relay
		ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
		char error[128] = "";
		int count = ytmusic_search_items("kanye", YTMUSIC_FILTER_SONGS, items, YTMUSIC_ITEMS_MAX, error,
										 sizeof(error));
		// The fixture holds a full "Songs" shelf: twenty responsive items.
		CHECK(count == 20);
		if (count > 0) {
			CHECK(items[0].kind == YTMUSIC_ITEM_TRACK);
			CHECK(items[0].id[0] != '\0');
			CHECK(items[0].title[0] != '\0');
			// The artwork the row draws comes from the page's own thumbnail
			// list -- the largest size it offers.
			CHECK(strncmp(items[0].thumbnail, "https://", 8) == 0);
		} else {
			fprintf(stderr, "  search fixture parsed %d rows, error %s\n", count, error);
		}
		free(search);
	}

	char *charts = load_fixture("fixtures/empty-FEmusic_charts.json");
	if (charts) {
		fake_reset();
		fake_reply("youtubei/v1/browse", 200, charts);
		ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
		char error[128] = "";
		int count = ytmusic_browse_items("FEmusic_charts", items, YTMUSIC_ITEMS_MAX, error, sizeof(error));
		CHECK(count > 0);
		if (count <= 0) fprintf(stderr, "  charts fixture parsed %d rows, error %s\n", count, error);
		free(charts);
	}

	char *explore = load_fixture("fixtures/empty-explore.json");
	if (explore) {
		fake_reset();
		fake_reply("youtubei/v1/browse", 200, explore);
		ytmusic_section_t sections[YTMUSIC_SECTIONS_MAX];
		char error[128] = "";
		int count = ytmusic_explore_sections(sections, YTMUSIC_SECTIONS_MAX, error, sizeof(error));
		// The fixture carries four carousels; "Moods & genres" is a grid of
		// filter buttons and is dropped on purpose, so three sections show.
		CHECK(count == 3);
		if (count > 0) {
			CHECK(sections[0].count > 0);
			CHECK(sections[0].items[0].title[0] != '\0');
		} else {
			fprintf(stderr, "  explore fixture parsed %d sections, error %s\n", count, error);
		}
		// Each carousel is named after what it holds. The headings are read from
		// the header object at the front of each shelf; when that read failed the
		// page drew three rows all labelled "Explore", so the names are the check
		// and not only the count.
		CHECK(strcmp(sections[0].title, "New albums & singles") == 0);
		if (count > 1) CHECK(strcmp(sections[1].title, "Trending") == 0);
		for (int i = 0; i < count; i++) {
			CHECK(sections[i].title[0] != '\0');
			// And every card carries the cover its row draws.
			CHECK(sections[i].items[0].thumbnail[0] != '\0');
			if (!sections[i].items[0].thumbnail[0]) {
				fprintf(stderr, "  section %d (%s) has no thumbnail on its first card\n", i,
						sections[i].title);
			}
		}
		free(explore);
	}

	// And the one page that is REALLY empty signed out: liked songs, which is
	// Google's own "sign in" card. It comes back with no rows, and the player
	// says which credential the page wants rather than showing an empty list
	// that can never fill -- Favourites used to sit on "no results" forever.
	char *vllm = load_fixture("fixtures/empty-VLLM.json");
	if (vllm) {
		fake_reset();
		fake_reply("youtubei/v1/browse", 200, vllm);
		ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
		char error[128] = "";
		int count = ytmusic_browse_items("VLLM", items, YTMUSIC_ITEMS_MAX, error, sizeof(error));
		CHECK(count < 0);
		CHECK(strcmp(error, "ytmusic_library_needs_session") == 0);
		if (count >= 0 || strcmp(error, "ytmusic_library_needs_session") != 0) {
			fprintf(stderr, "  VLLM signed out gave %d rows, error %s\n", count, error);
		}
		free(vllm);
	}

	fake_reset();
}

// The library pages need the session imported from the card, and the player has
// to say so. A Google sign-in token is refused by the music endpoints, and
// refused quietly: the page comes back empty rather than with an error, so
// Favourites answered "no results" for an account that had in fact signed in.
//
// The sidecar is seeded by hand here, in the two shapes the player can hold: an
// No session at all (so the library cannot answer), and an imported session
// (which can).
static void write_session_file(const char *body) {
	FILE *f = fopen(".build/ytmusic_session.ini", "w");
	if (!f) {
		CHECK(0 && "the sidecar could not be written");
		return;
	}
	fputs(body, f);
	fclose(f);
	ytmusic_init(); // the sidecar is read at init, so this is a reboot
}

static void test_library_says_what_is_missing(void) {
	char *vllm = load_fixture("fixtures/empty-VLLM.json");
	if (!vllm) return;

	// No account attached: the library says what is missing instead of
	// showing a list that can never fill.
	write_session_file("");
	CHECK(!ytmusic_session_cookie_attached());
	fake_reset();
	fake_reply("youtubei/v1/browse", 200, vllm);
	ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
	char error[192] = "";
	int count = ytmusic_browse_items("VLLM", items, YTMUSIC_ITEMS_MAX, error, sizeof(error));
	CHECK(count == -1);
	CHECK(strcmp(error, "ytmusic_library_needs_session") == 0);

	// With an imported session, the account list also includes local hearts.
	write_session_file("cookie=SID=x; SAPISID=abcdefghijklmnop; HSID=y\n");
	ytlike_t song_like = {0};
	snprintf(song_like.id, sizeof(song_like.id), "%s", "abcdefghijk");
	snprintf(song_like.title, sizeof(song_like.title), "%s", "Locally hearted song");
	snprintf(song_like.artist, sizeof(song_like.artist), "%s", "Test artist");
	ytlikes_remove(song_like.id);
	CHECK(ytlikes_add(&song_like));
	ytlike_t episode_like = {0};
	snprintf(episode_like.id, sizeof(episode_like.id), "%s", "podcast1234");
	snprintf(episode_like.title, sizeof(episode_like.title), "%s", "Locally hearted episode");
	snprintf(episode_like.artist, sizeof(episode_like.artist), "%s", "Test podcast");
	ytlikes_remove(episode_like.id);
	CHECK(ytlikes_add(&episode_like));
	fake_reset();
	fake_reply("youtubei/v1/browse", 200, vllm);
	error[0] = '\0';
	count = ytmusic_browse_items("VLLM", items, YTMUSIC_ITEMS_MAX, error, sizeof(error));
	CHECK(count == 2);
	CHECK(error[0] == '\0');
	if (count == 2) {
		CHECK(strcmp(items[0].id, episode_like.id) == 0);
		CHECK(strcmp(items[0].title, episode_like.title) == 0);
		CHECK(strcmp(items[1].id, song_like.id) == 0);
		CHECK(items[0].kind == YTMUSIC_ITEM_TRACK && items[1].kind == YTMUSIC_ITEM_TRACK);
	}
	CHECK(ytlikes_remove(song_like.id));
	CHECK(ytlikes_remove(episode_like.id));

	ytmusic_logout();
	CHECK(!ytmusic_session_cookie_attached());
	free(vllm);
}

// ---------------------------------------------------------------------------
// the choices the settings page stores
// ---------------------------------------------------------------------------

// The 256 kbit/s AAC beside the 128 one: what the quality pill is for, and what
// "Standard" is the way out of when an instance is too slow to carry it.
static const char INVIDIOUS_HI_FORMATS[] =
	"{\"title\":\"Never Gonna Give You Up\",\"author\":\"Rick Astley\",\"lengthSeconds\":213,"
	"\"adaptiveFormats\":["
	"{\"itag\":140,\"type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\",\"bitrate\":128000,"
	"\"clen\":\"4095\",\"url\":\"https://inv.test/z\"},"
	"{\"itag\":141,\"type\":\"audio/mp4; codecs=\\\"mp4a.40.2\\\"\",\"bitrate\":256000,"
	"\"clen\":\"8191\",\"url\":\"https://inv.test/w\"}"
	"]}";

static void test_quality_choice_picks_the_stream(void) {
	fake_reset();
	fake_reply("inv.test/api/v1/videos/", 200, INVIDIOUS_HI_FORMATS);
	config_set("ytmusic", "instances", "https://inv.test");
	set_relay("");
	CHECK(ytmusic_source_select("auto"));
	char url[YTMUSIC_URL_MAX] = "", error[192] = "";

	ytmusic_quality_set(true);
	CHECK(ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error)));
	CHECK(strstr(url, "itag=141") != NULL);

	memset(url, 0, sizeof(url));
	error[0] = '\0';
	ytmusic_quality_set(false);
	CHECK(ytmusic_audio_url(VID, url, sizeof(url), NULL, NULL, 0, NULL, 0, NULL, error, sizeof(error)));
	CHECK(strstr(url, "itag=140") != NULL);

	ytmusic_quality_set(true);
	CHECK(ytmusic_quality_high());
}

static void test_region_choice(void) {
	char region[YTMUSIC_REGION_MAX], language[YTMUSIC_REGION_MAX];
	ytmusic_region_active(region, sizeof(region), language, sizeof(language));
	CHECK(strcmp(region, "US") == 0);
	CHECK(strcmp(language, "en") == 0);

	// Poland, the pair the shared region list offers for Polish
	// (core/region.h): one entry per language the player speaks, so the
	// charts and the row names move together.
	CHECK(ytmusic_region_select("PL", "pl"));
	ytmusic_region_active(region, sizeof(region), language, sizeof(language));
	CHECK(strcmp(region, "PL") == 0);
	CHECK(strcmp(language, "pl") == 0);

	// A pair the shared list does not offer, and two codes that are not
	// codes: both are refused rather than stored.
	CHECK(!ytmusic_region_select("PL", "en"));
	CHECK(!ytmusic_region_select("P", "en"));
	CHECK(!ytmusic_region_select("PL", "english"));
	CHECK(ytmusic_region_select("US", "en"));
	ytmusic_region_active(region, sizeof(region), language, sizeof(language));
	CHECK(strcmp(region, "US") == 0);
	CHECK(strcmp(language, "en") == 0);
}

static void test_cache_owns_its_own_downloads(void) {
	// The folder is the one the cache is actually using, which the suite moved
	// to its own directory at start-up.
	char own[512];
	snprintf(own, sizeof(own), "%s/dQw4w9WgXcQ.m4a", YTM_CACHE);
	CHECK(ytmusic_cache_owns(own));
	snprintf(own, sizeof(own), "%s/dQw4w9WgXcQ.jpg", YTM_CACHE);
	CHECK(!ytmusic_cache_owns(own));
	snprintf(own, sizeof(own), "%s/sub/dQw4w9WgXcQ.m4a", YTM_CACHE);
	CHECK(!ytmusic_cache_owns(own));
	CHECK(!ytmusic_cache_owns("/mnt/sd_0/Music/Artist/song.m4a"));
	CHECK(!ytmusic_cache_owns(NULL));
}

static void test_podcast_episode_marker_is_cached(void) {
	char path[512];
	CHECK(ytmusic_cache_path_for(VID, path, sizeof(path)));
	ytmusic_track_t track = {0};
	snprintf(track.id, sizeof(track.id), "%s", VID);
	snprintf(track.title, sizeof(track.title), "Episode");
	track.podcast_episode = true;
	CHECK(ytmusic_cache_write_sidecars(&track, path));
	CHECK(ytmusic_cache_is_podcast_episode(path));

	track.podcast_episode = false;
	CHECK(ytmusic_cache_write_sidecars(&track, path));
	CHECK(!ytmusic_cache_is_podcast_episode(path));
	char tags[544];
	snprintf(tags, sizeof(tags), "%s.tags", path);
	remove(tags);
}

static void test_podcast_played_state_persists(void) {
	static const char EPISODE_ID[] = "abcdefghijk";
	CHECK(ytpodcastplayed_set(EPISODE_ID, false));
	CHECK(!ytpodcastplayed_get(EPISODE_ID));
	CHECK(ytpodcastplayed_set(EPISODE_ID, true));
	CHECK(ytpodcastplayed_get(EPISODE_ID));
	ytpodcastplayed_init();
	CHECK(ytpodcastplayed_get(EPISODE_ID));
	CHECK(ytpodcastplayed_set(EPISODE_ID, false));
	CHECK(!ytpodcastplayed_get(EPISODE_ID));
	CHECK(!ytpodcastplayed_set("not-an-id", true));
}

static void test_sidecars_create_a_fresh_cache_directory(void) {
	char path[512], tags[520];
	snprintf(path, sizeof(path), "%s/%s.m4a", YTM_CACHE_FRESH, VID);
	snprintf(tags, sizeof(tags), "%s.tags", path);
	remove(tags);
	rmdir(YTM_CACHE_FRESH);
	CHECK(putenv(fresh_cache_env) == 0);

	ytmusic_track_t track = {0};
	snprintf(track.id, sizeof(track.id), "%s", VID);
	snprintf(track.title, sizeof(track.title), "Fresh cache title");
	snprintf(track.artist, sizeof(track.artist), "Fresh cache artist");
	CHECK(ytmusic_cache_write_sidecars(&track, path));

	FILE *f = fopen(tags, "r");
	CHECK(f != NULL);
	if (f) {
		char contents[256] = "";
		size_t n = fread(contents, 1, sizeof(contents) - 1, f);
		contents[n] = '\0';
		CHECK(strstr(contents, "title=Fresh cache title\n") != NULL);
		CHECK(strstr(contents, "artist=Fresh cache artist\n") != NULL);
		fclose(f);
	}

	CHECK(putenv(cache_env) == 0);
	remove(tags);
	rmdir(YTM_CACHE_FRESH);
}

// A song row as the store sends it: the second column is "artist • album •
// duration", one run per piece, and the artist itself is three runs when a track
// lists more than one. The album is the only thing here that is not the artist
// and not the length, and it is what the download folder is named after.
static const char SEARCH_SONG_ROW[] =
	"{\"contents\":{\"singleColumnBrowseResultsRenderer\":{\"tabs\":[{\"tabRenderer\":{\"content\":"
	"{\"sectionListRenderer\":{\"contents\":[{\"musicShelfRenderer\":{\"contents\":["
	"{\"musicResponsiveListItemRenderer\":{"
	"\"playlistItemData\":{\"videoId\":\"5NV6Rdv1a3I\"},"
	"\"flexColumns\":["
	"{\"musicResponsiveListItemFlexColumnRenderer\":{\"text\":{\"runs\":[{\"text\":\"Get Lucky\"}]}}},"
	"{\"musicResponsiveListItemFlexColumnRenderer\":{\"text\":{\"runs\":["
	"{\"text\":\"Daft Punk\"},{\"text\":\", \"},{\"text\":\"Pharrell Williams\"},"
	"{\"text\":\" \xE2\x80\xA2 \"},{\"text\":\"Random Access Memories\"},"
	"{\"text\":\" \xE2\x80\xA2 \"},{\"text\":\"6:10\"}]}}},"
	"{\"musicResponsiveListItemFlexColumnRenderer\":{\"text\":{\"runs\":[{\"text\":\"1.9B plays\"}]}}}"
	"],"
	"\"fixedColumns\":[{\"musicResponsiveListItemFixedColumnRenderer\":{\"text\":{\"runs\":[{\"text\":\"6:10\"}]}}}],"
	"\"thumbnail\":{\"musicThumbnailRenderer\":{\"thumbnail\":{\"thumbnails\":["
	"{\"url\":\"https://lh3.test/x=w60\",\"width\":60}]}}}"
	"}}]}}]}}]}}]}}";

static const char SEARCH_PODCAST_EPISODE[] =
	"{\"contents\":{\"musicMultiRowListItemRenderer\":{"
	"\"playlistItemData\":{\"videoId\":\"abcdefghijk\"},"
	"\"flexColumns\":["
	"{\"musicResponsiveListItemFlexColumnRenderer\":{\"text\":{\"runs\":[{\"text\":\"Episode title\"}]}}}"
	"],"
	"\"longBylineText\":{\"runs\":[{\"text\":\"Podcast creator\"}]},"
	"\"thumbnail\":{\"musicThumbnailRenderer\":{\"thumbnail\":{\"thumbnails\":["
	"{\"url\":\"https://lh3.test/episode=w60\",\"width\":60}]}}}"
	"}}}";

static void test_song_row_names_its_album(void) {
	fake_reset();
	fake_reply("youtubei/v1/browse", 200, SEARCH_SONG_ROW);
	ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
	char error[192] = "";
	int count = ytmusic_browse_items("VLPLtest123", items, 4, error, sizeof(error));
	CHECK(count == 1);
	if (count != 1) {
		fprintf(stderr, "  the row was not read: %s\n", error);
		return;
	}
	CHECK(items[0].kind == YTMUSIC_ITEM_TRACK);
	CHECK(strcmp(items[0].subtitle, "Daft Punk") == 0);
	CHECK(strcmp(items[0].album, "Random Access Memories") == 0);
}


static void test_mpsp_podcast_show_marks_episodes(void) {
	fake_reset();
	fake_reply("youtubei/v1/browse", 200, SEARCH_SONG_ROW);
	ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
	char error[192] = "";
	int count = ytmusic_browse_items("MPSPtest123", items, 4, error, sizeof(error));
	CHECK(count == 1);
	CHECK(items[0].kind == YTMUSIC_ITEM_TRACK);
	CHECK(items[0].podcast_episode);
}

static void test_direct_tags_path_podcast_episode(void) {
	char path[] = ".build/test_direct_ep.m4a";
	char tag_path[] = ".build/test_direct_ep.m4a.tags";
	FILE *f = fopen(tag_path, "w");
	CHECK(f != NULL);
	fprintf(f, "title=Direct Episode\nartist=Show\npodcast_episode=1\n");
	fclose(f);
	CHECK(ytmusic_cache_is_podcast_episode(path));
	remove(tag_path);
	CHECK(!ytmusic_cache_is_podcast_episode(path));
}

static void test_podcast_episode_row_keeps_creator_and_type(void) {
	fake_reset();
	fake_reply("youtubei/v1/search", 200, SEARCH_PODCAST_EPISODE);
	ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
	char error[192] = "";
	int count = ytmusic_search_items("episode", YTMUSIC_FILTER_PODCASTS, items, 4,
									 error, sizeof(error));
	CHECK(count == 1);
	if (count != 1) {
		fprintf(stderr, "  podcast episode row was not read: %s\n", error);
		return;
	}
	CHECK(items[0].kind == YTMUSIC_ITEM_TRACK);
	CHECK(items[0].podcast_episode);
	CHECK(strcmp(items[0].title, "Episode title") == 0);
	CHECK(strcmp(items[0].subtitle, "Podcast creator") == 0);
}

// The loading card's numbers come from the downloader, and must not be left
// saying something is running once it has finished -- a bar that never goes away
// is worse than no bar at all.
static const char EXPLORE_CARD_ITEM[] =
	"{\"contents\":{\"musicTwoRowItemRenderer\":{"
	"\"title\":{\"runs\":[{\"text\":\"Patrzcie i mowcie\",\"navigationEndpoint\":{\"browseEndpoint\":{\"browseId\":\"MPREb_pETx2j8239i\"}}}]},"
	"\"subtitle\":{\"runs\":[{\"text\":\"Singiel\"},{\"text\":\" \xE2\x80\xA2 \"},{\"text\":\"Nina Lakomy\"}]},"
	"\"thumbnailRenderer\":{\"musicThumbnailRenderer\":{\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://lh3.test/nina=w226\",\"width\":226}]}}}"
	"}}}";

static void test_two_row_card_extracts_artist_from_subtitle(void) {
	fake_reset();
	fake_reply("youtubei/v1/search", 200, EXPLORE_CARD_ITEM);
	ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
	char error[192] = "";
	int count = ytmusic_search_items("nina", YTMUSIC_FILTER_ALBUMS, items, 4, error, sizeof(error));
	CHECK(count == 1);
	if (count != 1) return;
	CHECK(items[0].kind == YTMUSIC_ITEM_CONTAINER);
	CHECK(strcmp(items[0].title, "Patrzcie i mowcie") == 0);
	CHECK(strcmp(items[0].subtitle, "Nina Lakomy") == 0);
	CHECK(strcmp(items[0].thumbnail, "https://lh3.test/nina=w226") == 0);
}

static const char ALBUM_PAGE_WITH_HEADER[] =
	"{\"contents\":{\"twoColumnBrowseResultsRenderer\":{\"secondaryContents\":{\"sectionListRenderer\":{\"contents\":["
	"{\"musicResponsiveHeaderRenderer\":{"
	"\"title\":{\"runs\":[{\"text\":\"The Rhythm Takes Control\"}]},"
	"\"straplineTextOne\":{\"runs\":[{\"text\":\"Dub Style MCs\"}]},"
	"\"thumbnail\":{\"musicThumbnailRenderer\":{\"thumbnail\":{\"thumbnails\":[{\"url\":\"https://lh3.test/album=w544\",\"width\":544}]}}}"
	"}},"
	"{\"musicShelfRenderer\":{\"contents\":["
	"{\"musicResponsiveListItemRenderer\":{"
	"\"playlistItemData\":{\"videoId\":\"ZwVBZeHIYTM\"},"
	"\"flexColumns\":["
	"{\"musicResponsiveListItemFlexColumnRenderer\":{\"text\":{\"runs\":[{\"text\":\"The Rhythm Takes Control Raw ((Scratch Mix Version))\"}]}}},"
	"{\"musicResponsiveListItemFlexColumnRenderer\":{\"text\":{}}}"
	"],"
	"\"fixedColumns\":[{\"musicResponsiveListItemFixedColumnRenderer\":{\"text\":{\"runs\":[{\"text\":\"5:09\"}]}}}]"
	"}}"
	"]}}"
	"]}}}}}}";

static void test_album_browse_populates_tracks_with_header_metadata(void) {
	fake_reset();
	fake_reply("youtubei/v1/browse", 200, ALBUM_PAGE_WITH_HEADER);
	ytmusic_item_t items[YTMUSIC_ITEMS_MAX];
	char error[192] = "";
	int count = ytmusic_browse_items("MPREb_r7BwZyiJV39", items, 4, error, sizeof(error));
	CHECK(count == 1);
	if (count != 1) return;
	CHECK(items[0].kind == YTMUSIC_ITEM_TRACK);
	CHECK(strcmp(items[0].title, "The Rhythm Takes Control Raw ((Scratch Mix Version))") == 0);
	CHECK(strcmp(items[0].subtitle, "Dub Style MCs") == 0);
	CHECK(strcmp(items[0].album, "The Rhythm Takes Control") == 0);
	CHECK(strcmp(items[0].thumbnail, "https://lh3.test/album=w544") == 0);
}

static void test_progress_is_cleared_when_the_download_ends(void) {
	// The progress itself is in memory rather than a file on the card, so this
	// check needs no folder and skips on no host.
	fake_reset();
	fake_reply("inv.test/api/v1/videos/", 200, INVIDIOUS_FORMATS);
	fake_reply("inv.test/latest_version", 206, AUDIO_BYTES);
	config_set("ytmusic", "instances", "https://inv.test");
	set_relay("");
	CHECK(ytmusic_source_select("auto"));
	ytmusic_track_t track;
	memset(&track, 0, sizeof(track));
	snprintf(track.id, sizeof(track.id), "%s", VID);
	snprintf(track.title, sizeof(track.title), "%s", "Never Gonna Give You Up");
	char path[512] = "", error[192] = "";
	bool ok = ytmusic_download_for_playback(&track, path, sizeof(path), error, sizeof(error));
	CHECK(ok);
	int percent = 42;
	bool running = true;
	ytmusic_download_progress(&percent, &running);
	CHECK(!running);
	CHECK(percent == -1);
}

// A browser's export, which is what "put the session on the card" actually
// means: the usual Netscape cookies.txt used to be refused as invalid, and a
// refused import looks exactly like a session Google declined.
static void test_netscape_cookie_file_is_imported(void) {
	const char *path = ".build/ytmusic_session.txt";
	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "  skipped: this host cannot write %s\n", path);
		return;
	}
	fprintf(f, "# Netscape HTTP Cookie File\n");
	fprintf(f, ".youtube.com\tTRUE\t/\tTRUE\t0\tSAPISID\tabcdefghijklmnop\n");
	fprintf(f, ".youtube.com\tTRUE\t/\tTRUE\t0\t__Secure-3PAPISID\tqrstuvwxyz\n");
	fprintf(f, ".example.com\tTRUE\t/\tFALSE\t0\tTRACKER\tnope\n");
	fclose(f);

	char error[192] = "";
	bool ok = ytmusic_import_session_file(".build", error, sizeof(error));
	CHECK(ok);
	if (!ok) fprintf(stderr, "  the import refused the export: %s\n", error);
	char cookie[4096];
	ytmusic_session_cookie(cookie, sizeof(cookie));
	CHECK(strstr(cookie, "SAPISID=abcdefghijklmnop") != NULL);
	CHECK(strstr(cookie, "__Secure-3PAPISID=qrstuvwxyz") != NULL);
	CHECK(strstr(cookie, "TRACKER") == NULL);
	CHECK(ytmusic_session_cookie_attached());
	memset(cookie, 0, sizeof(cookie));
	FILE *left = fopen(path, "r");
	CHECK(left == NULL); // the card copy is not left lying there
	if (left) fclose(left);
	ytmusic_logout();
	CHECK(!ytmusic_session_cookie_attached());
}

// A build with no InnerTube key cannot ask YouTube anything, and must say that
// rather than send the request: the endpoint answers a keyless request with the
// same empty stub an unknown User-Agent gets, which reaches the page as "the
// search failed" with no hint that the build is what is wrong. This test runs
// in its own binary, compiled with an empty key (see the Makefile); the rest of
// the suite needs a non-empty one for the requests it checks.
#ifdef YTMUSIC_TEST_KEYLESS
static void test_missing_key_is_named(void) {
	char error[192] = "";
	ytmusic_item_t items[4];
	memset(items, 0, sizeof(items));
	int found = ytmusic_search_items("never gonna give you up", YTMUSIC_FILTER_SONGS, items, 4,
									 error, sizeof(error));
	CHECK(found <= 0);
	CHECK(strcmp(error, "ytmusic_api_key_missing") == 0);
	if (strcmp(error, "ytmusic_api_key_missing") != 0) {
		fprintf(stderr, "  the search said: %s\n", error[0] ? error : "(nothing)");
	}
}
#endif

// A song must not wait for its whole file. The instance serves it in pieces;
// the player is handed the path as soon as enough of it is on the disk, and the
// rest keeps arriving while growfile keeps the decoder waiting for it.
// The size the canned stream info advertises, so the completeness check the
// downloader makes is satisfied by the body the fake serves.
static char STREAM_BYTES[4096];

typedef struct {
	int calls;
	char path[512];
	long size_when_ready;
	bool growing_when_ready;
} ready_probe_t;

static void stream_ready(const char *path, void *user) {
	ready_probe_t *probe = user;
	probe->calls++;
	snprintf(probe->path, sizeof(probe->path), "%s", path);
	FILE *f = fopen(path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		probe->size_when_ready = ftell(f);
		fclose(f);
	}
	probe->growing_when_ready = growfile_is_growing(path);
}

static void test_playback_starts_before_the_download_ends(void) {
	// A container shaped like the ones the instances serve: an ftyp box, then a
	// whole moov index within the first few dozen bytes, then the payload. The
	// index has to be in the part that arrives first, because the start waits
	// for it -- and waits for it to be whole, which is a question about the
	// box headers. Their sizes are binary, so this body is registered by
	// length rather than as a C string.
	static const char MP4_HEAD[] = {
		0x00, 0x00, 0x00, 0x10, 'f', 't', 'y', 'p', 'M', '4', 'A', ' ', 'm', 'p', '4', '2',
		0x00, 0x00, 0x00, 0x18, 'm', 'o', 'o', 'v',
	};
	memset(STREAM_BYTES, 'B', sizeof(STREAM_BYTES) - 1);
	memcpy(STREAM_BYTES, MP4_HEAD, sizeof(MP4_HEAD));
	fake_reset();
	fake_reply("inv.test/api/v1/videos/", 200, INVIDIOUS_FORMATS);
	fake_reply_bytes("inv.test/latest_version", 206, STREAM_BYTES, sizeof(STREAM_BYTES) - 1);
	// Pieces, the way an instance that proxies its own upstream replies.
	fake_range_cap = 1000;
	FILE *writable = fopen(YTM_CACHE_PROBE, "wb");
	if (!writable) {
		fprintf(stderr, "  skipped: this host cannot write %s (%s)\n", YTM_CACHE,
				strerror(errno));
		return;
	}
	fclose(writable);
	remove(YTM_CACHE_PROBE);
	char stale[600];
	snprintf(stale, sizeof(stale), "%s/%s.m4a", YTM_CACHE, VID);
	remove(stale);
	snprintf(stale, sizeof(stale), "%s/%s.done", YTM_CACHE, VID);
	remove(stale);
	snprintf(stale, sizeof(stale), "%s/%s.jpg", YTM_CACHE, VID);
	remove(stale);
	snprintf(stale, sizeof(stale), "%s/%s.tags", YTM_CACHE, VID);
	remove(stale);
	config_set("ytmusic", "instances", "https://inv.test");
	set_relay("");
	CHECK(ytmusic_source_select("auto"));

	ytmusic_track_t track;
	memset(&track, 0, sizeof(track));
	snprintf(track.id, sizeof(track.id), "%s", VID);
	snprintf(track.title, sizeof(track.title), "%s", "Never Gonna Give You Up");

	ready_probe_t probe;
	memset(&probe, 0, sizeof(probe));
	char path[512] = "", error[192] = "";
	bool ok = ytmusic_stream_for_playback(&track, path, sizeof(path), stream_ready, &probe,
									  error, sizeof(error));
	CHECK(ok);
	if (!ok) fprintf(stderr, "  the stream gave up: %s\n", error);
	// Told once, and about the file the caller was given.
	CHECK(probe.calls == 1);
	CHECK(probe.path[0] != '\0');
	CHECK(strcmp(probe.path, path) == 0);
	// Started on a part of it, while it was still being written.
	CHECK(probe.size_when_ready > 0);
	CHECK((size_t)probe.size_when_ready < sizeof(STREAM_BYTES) - 1);
	CHECK(probe.growing_when_ready);
	// And once the transfer is over the decoder sees an ordinary, whole file.
	CHECK(!growfile_is_growing(path));
	FILE *f = ok ? fopen(path, "rb") : NULL;
	CHECK(f != NULL);
	if (f) {
		fseek(f, 0, SEEK_END);
		CHECK(ftell(f) == (long)sizeof(STREAM_BYTES) - 1);
		fclose(f);
	}
}

// A song saved to the card is still YouTube Music's. The folder it is kept in is
// one the music library scans, so the heart on it has to reach the player's own
// likes and never the card's favourites -- and for that it needs the id, which
// the file's name (the song's name) does not carry. The sidecar does.
static void test_kept_download_keeps_its_id(void) {
	const char *dir = ".build/card/Music/YouTube Music/Artist/Album";
	char path[512], tags[560];
	mkdir(".build/card", 0700);
	mkdir(".build/card/Music", 0700);
	mkdir(".build/card/Music/YouTube Music", 0700);
	mkdir(".build/card/Music/YouTube Music/Artist", 0700);
	mkdir(dir, 0700);
	snprintf(path, sizeof(path), "%s/01 Song.m4a", dir);
	snprintf(tags, sizeof(tags), "%s.tags", path);

	FILE *f = fopen(tags, "w");
	CHECK(f != NULL);
	if (f) {
		fprintf(f, "title=Kept song\nartist=Kept artist\nalbum=Kept album\npodcast_episode=0\nid=dQw4w9WgXcQ\n");
		fclose(f);
	}
	char id[16] = "unset";
	CHECK(ytmusic_cache_track_id(path, id, sizeof(id)));
	CHECK(strcmp(id, "dQw4w9WgXcQ") == 0);

	// Kept before ids were written: it still answers "mine", with no id. The
	// heart then has nothing to like it by and leaves it alone -- it must not
	// fall back into the card's favourites.
	f = fopen(tags, "w");
	if (f) {
		fprintf(f, "title=Older kept song\n");
		fclose(f);
	}
	id[0] = '\0';
	CHECK(ytmusic_cache_track_id(path, id, sizeof(id)));
	CHECK(id[0] == '\0');
	remove(tags);

	// A file of the user's own is not YouTube Music's at all.
	CHECK(!ytmusic_cache_track_id(".build/card/somewhere/else.m4a", id, sizeof(id)));
}

int main(void) {
	FILE *seed = fopen(".build/test_ytmusic_config.ini", "w");
	if (seed) {
		fclose(seed);
	}
	config_init(".build/test_ytmusic_config.ini");
	// The cache the download checks write into: this suite's own folder, so a
	// host that refuses 	mp still runs them.
	putenv(cache_env);
	// .build is the build tree's own name and normally already there; the
	// cache folder inside it is this suite's, and a fresh checkout has neither.
	mkdir(".build", 0700);
	mkdir(YTM_CACHE, 0700);
	streamkeys_init();
	ytmusic_init();

#ifdef YTMUSIC_TEST_KEYLESS
	// Nothing else can run without a key: this build is the one check.
	test_missing_key_is_named();
	if (failures) {
		fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("test_ytmusic: ok (no key)\n");
	return 0;
#endif

	test_nothing_configured_says_what_to_do();
	test_relay_picks_the_aac_stream();
	test_instances_are_asked_before_the_device();
	test_phone_player_reply_plays_on_its_own();
	test_download_walks_past_a_refused_stream();
	test_playback_starts_before_the_download_ends();
	test_relay_pin_asks_only_the_relay();
	test_pinned_invidious_instance_plays();
	test_pinned_piped_instance_plays();
	test_instance_naming_google_is_not_the_answer();
	test_sources_list_the_choices();
	test_webm_is_never_the_answer();
	test_unreadable_relay_names_itself();
	test_unreachable_relay_names_itself();
	test_stream_url_stays_on_the_relay();
	test_relay_without_the_endpoint_names_itself();
	test_real_device_pages();
	test_library_says_what_is_missing();
	test_quality_choice_picks_the_stream();
	test_region_choice();
	test_cache_owns_its_own_downloads();
	test_podcast_episode_marker_is_cached();
	test_podcast_played_state_persists();
	test_sidecars_create_a_fresh_cache_directory();
	test_song_row_names_its_album();
	test_mpsp_podcast_show_marks_episodes();
	test_direct_tags_path_podcast_episode();
	test_podcast_episode_row_keeps_creator_and_type();
	test_netscape_cookie_file_is_imported();
	test_two_row_card_extracts_artist_from_subtitle();
	test_album_browse_populates_tracks_with_header_metadata();
	test_progress_is_cleared_when_the_download_ends();
	test_kept_download_keeps_its_id();

	if (failures) {
		fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("test_ytmusic: ok\n");
	return 0;
}
