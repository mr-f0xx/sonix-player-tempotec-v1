// test_podcastdir.c -- the podcast catalogue, on both of the directories it
// can talk to.
//
// The Podcasts section has to work on an image that carries no keys, which is
// the only kind that can be published, and it has to keep working unchanged
// for an install that does have a Podcast Index pair. Those are two requests
// with different hosts, different authentication and different JSON, answering
// the same five functions, so the thing worth testing is the seam: which
// directory a given configuration picks, the exact request each one sends, and
// that both fill the same structs the pages, the cache and the downloader
// already read.
//
// The network is stubbed, the way the tag manager's art search test stubs it:
// nothing here reaches Apple or Podcast Index, and every assertion about a
// request is made on the URL and the headers the production code built. The
// JSON is the shape those two really answer with, trimmed -- including the
// parts that are easy to get wrong, which is why a chart row's id is a quoted
// string, why a lookup leads with the show itself, and why one episode in the
// fixture has no audio on it.
//
// What is not tested here is the download and the playback of what comes back;
// those have their own suites and see only the structs, which do not know
// which directory filled them.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/region.h"
#include "src/system/core/sha1.h"
#include "src/system/net/http.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastrss.h"
#include "src/system/streaming/streamkeys.h"

static int failures;

#define CHECK(cond)                                                                                     \
	do {                                                                                                \
		if (!(cond)) {                                                                                  \
			failures++;                                                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                             \
		}                                                                                               \
	} while (0)

#define CHECK_STR(got, want)                                                                            \
	do {                                                                                                \
		if (strcmp((got), (want)) != 0) {                                                               \
			failures++;                                                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n  got  \"%s\"\n  want \"%s\"\n", __FILE__, __LINE__, #got, \
					(got), (want));                                                                     \
		}                                                                                               \
	} while (0)

// ---------------------------------------------------------------------------
// the stubbed network
// ---------------------------------------------------------------------------

// What the next request answers with, and how.
static const char *reply_body;
static int reply_status;   // 0 answers a success, anything else that status
static bool reply_offline; // fail the way an unreachable network does
static char http_error[128];

// What the last request looked like, so the tests can read it back.
static char last_url[2048];
static char last_headers[1024];
static int request_count;

// A list of more than one answer, for the calls that make two or three
// requests: the episode list, then the show it belongs to, then the show's own
// feed. A route answers every request whose URL contains its needle, the first
// matching one winning, and a request no route matches gets `reply_body` -- so
// the tests written before routes existed still read the same way.
#define ROUTES_MAX 6
static struct {
	const char *needle;
	const char *body;
	bool offline;
} routes[ROUTES_MAX];
static int route_count;

// Every URL of this run, in order, so a test can say the feed was fetched or
// that it was not. That is the assertion the fallback stands on.
#define SEEN_MAX 8
static char seen_urls[SEEN_MAX][512];
static int seen_count;

static void routes_clear(void) {
	memset(routes, 0, sizeof(routes));
	route_count = 0;
	seen_count = 0;
}

static void route_add(const char *needle, const char *body) {
	if (route_count >= ROUTES_MAX) {
		return;
	}
	routes[route_count].needle = needle;
	routes[route_count].body = body;
	routes[route_count].offline = false;
	route_count++;
}

// A route that fails the way an unreachable host does.
static void route_offline(const char *needle) {
	if (route_count >= ROUTES_MAX) {
		return;
	}
	routes[route_count].needle = needle;
	routes[route_count].body = NULL;
	routes[route_count].offline = true;
	route_count++;
}

static bool was_fetched(const char *needle) {
	for (int i = 0; i < seen_count; i++) {
		if (strstr(seen_urls[i], needle)) {
			return true;
		}
	}
	return false;
}

static void net_reset(const char *body) {
	reply_body = body;
	reply_status = 0;
	reply_offline = false;
	snprintf(http_error, sizeof(http_error), "%s", "");
	last_url[0] = '\0';
	last_headers[0] = '\0';
	request_count = 0;
	routes_clear();
}

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len,
				 size_t limit, int timeout_secs, int *status_out) {
	(void)want_error_body;
	(void)limit;
	(void)timeout_secs;

	request_count++;
	snprintf(last_url, sizeof(last_url), "%s", url ? url : "");
	snprintf(last_headers, sizeof(last_headers), "%s", extra_headers ? extra_headers : "");
	if (seen_count < SEEN_MAX) {
		snprintf(seen_urls[seen_count], sizeof(seen_urls[seen_count]), "%s", url ? url : "");
		seen_count++;
	}

	// The first route that matches this URL answers it.
	for (int i = 0; i < route_count; i++) {
		if (strstr(last_url, routes[i].needle)) {
			if (routes[i].offline) {
				if (status_out) {
					*status_out = 0;
				}
				return false;
			}
			reply_body = routes[i].body;
			break;
		}
	}

	if (status_out) {
		*status_out = reply_offline ? 0 : (reply_status ? reply_status : 200);
	}
	if (reply_offline) {
		return false;
	}
	if (reply_status && !want_error_body) {
		return false;
	}
	if (!reply_body) {
		return false;
	}

	size_t len = strlen(reply_body);
	*out = malloc(len + 1);
	if (!*out) {
		return false;
	}
	memcpy(*out, reply_body, len + 1);
	if (out_len) {
		*out_len = len;
	}
	// A 4xx is a success of the call and a refusal of the request, which is
	// exactly the distinction the production code depends on.
	return reply_status < 400 || want_error_body;
}

const char *http_last_error(void) { return http_error; }

// A real enough encoder that the tests can assert on what was encoded. Only
// the characters a search term actually carries are handled.
char *http_url_encode(const char *in, char *out, size_t out_size) {
	static const char hex[] = "0123456789ABCDEF";
	size_t at = 0;
	for (const char *p = in; *p && at + 4 < out_size; p++) {
		unsigned char c = (unsigned char)*p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
			c == '.' || c == '~') {
			out[at++] = (char)c;
		} else {
			out[at++] = '%';
			out[at++] = hex[c >> 4];
			out[at++] = hex[c & 15];
		}
	}
	out[at] = '\0';
	return out;
}

// ---------------------------------------------------------------------------
// the stubbed keys and strings
// ---------------------------------------------------------------------------

static const char *stub_pi_key;
static const char *stub_pi_secret;

const char *streamkeys_podcast_key(void) { return stub_pi_key; }
const char *streamkeys_podcast_secret(void) { return stub_pi_secret; }

// tr() is the real one on the device and a table in the language files; here
// the tag is enough, because what the tests assert is that an error was set
// and which one, not the words it is shown in.
const char *tr(const char *text) { return text ? text : ""; }
const char *lang_current(void) { return "English"; }

static void keys_clear(void) {
	stub_pi_key = NULL;
	stub_pi_secret = NULL;
}

static void keys_set(void) {
	stub_pi_key = "ABCDEF1234";
	stub_pi_secret = "s3cr3t";
}

// Back to the state a fresh install is in: no api_base, no keys, no region
// chosen, nothing remembered of the last request. The two config lines are
// blanked rather than removed -- there is no way to remove one -- and both
// getters read an empty value as no value at all, which is itself worth
// holding onto: an empty `api_base =` line must not turn every request into a
// relative one.
static void reset(void) {
	config_set("podcast", "api_base", "");
	config_set("podcast", "charts_base", "");
	config_set("region", "value", "");
	keys_clear();
	region_select_language("");
	net_reset(NULL);
}

// ---------------------------------------------------------------------------
// the fixtures: the two directories' own shapes, trimmed
// ---------------------------------------------------------------------------

// Apple's search. Two rows, the second with no collection id, which is what a
// result that is not really a show looks like and what must not reach a list.
static const char *APPLE_SEARCH =
	"{\"resultCount\":2,\"results\":["
	"{\"wrapperType\":\"track\",\"kind\":\"podcast\",\"collectionId\":1200361736,"
	"\"trackId\":1200361736,\"artistName\":\"The New York Times\",\"collectionName\":\"The Daily\","
	"\"feedUrl\":\"https://feeds.simplecast.com/Sl5CSM3S\","
	"\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/image/thumb/a.jpg/100x100bb.jpg\","
	"\"artworkUrl600\":\"https://is1-ssl.mzstatic.com/image/thumb/a.jpg/600x600bb.jpg\","
	"\"trackCount\":2776,\"primaryGenreName\":\"Daily News\"},"
	"{\"wrapperType\":\"track\",\"kind\":\"podcast\",\"collectionId\":0,\"collectionName\":\"No Id\","
	"\"artistName\":\"Nobody\"}"
	"]}";

// Apple's chart. The id is a quoted string here, where search and lookup send
// a number, and the artwork is a 100 px square with nothing larger beside it.
static const char *APPLE_CHART_FR =
	"{\"feed\":{\"title\":\"Classement\",\"country\":\"fr\",\"results\":["
	"{\"artistName\":\"RTL\",\"id\":\"369369012\",\"name\":\"Les Grosses T\\u00eates\",\"kind\":\"podcasts\","
	"\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/image/thumb/b.jpg/100x100bb.png\","
	"\"genres\":[{\"genreId\":\"1489\",\"name\":\"Actualit\\u00e9s\"}],"
	"\"url\":\"https://podcasts.apple.com/fr/podcast/id369369012\"}"
	"]}}";

// An empty chart, which is what a storefront with no chart of its own answers.
static const char *APPLE_CHART_EMPTY = "{\"feed\":{\"title\":\"Charts\",\"country\":\"xx\",\"results\":[]}}";

// Apple's episode lookup: the show itself first, then its episodes. The third
// row is an episode with no audio, which the lists must drop but the raw count
// must not, because it is the count that says whether another page is worth
// asking for.
static const char *APPLE_EPISODES =
	"{\"resultCount\":3,\"results\":["
	"{\"wrapperType\":\"track\",\"kind\":\"podcast\",\"collectionId\":1200361736,\"trackId\":1200361736,"
	"\"artistName\":\"The New York Times\",\"collectionName\":\"The Daily\",\"trackCount\":2776},"
	"{\"wrapperType\":\"podcastEpisode\",\"kind\":\"podcast-episode\",\"trackId\":1000800977857,"
	"\"collectionId\":1200361736,\"collectionName\":\"The Daily\",\"trackName\":\"Ep one\","
	"\"episodeUrl\":\"https://cdn.example/one.mp3\",\"episodeFileExtension\":\"mp3\","
	"\"episodeContentType\":\"audio\",\"releaseDate\":\"2026-11-10T10:55:29Z\",\"trackTimeMillis\":1904000,"
	"\"artworkUrl600\":\"https://is1-ssl.mzstatic.com/image/thumb/c.jpg/600x600bb.jpg\"},"
	"{\"wrapperType\":\"podcastEpisode\",\"kind\":\"podcast-episode\",\"trackId\":1000800746193,"
	"\"collectionId\":1200361736,\"collectionName\":\"The Daily\",\"trackName\":\"Ep two\","
	"\"episodeUrl\":\"\",\"episodeFileExtension\":\"m4a\",\"releaseDate\":\"2026-11-09\","
	"\"trackTimeMillis\":1874000}"
	"]}";

// One episode on its own, the shape the second half of podcast_episode()'s
// lookup finds it in.
static const char *APPLE_ONE_EPISODE =
	"{\"resultCount\":2,\"results\":["
	"{\"wrapperType\":\"track\",\"kind\":\"podcast\",\"collectionId\":1200361736,\"trackId\":1200361736,"
	"\"collectionName\":\"The Daily\"},"
	"{\"wrapperType\":\"podcastEpisode\",\"kind\":\"podcast-episode\",\"trackId\":1000800977857,"
	"\"collectionId\":1200361736,\"collectionName\":\"The Daily\",\"trackName\":\"Ep one\","
	"\"episodeUrl\":\"https://cdn.example/one.mp3\",\"episodeFileExtension\":\"mp3\","
	"\"releaseDate\":\"2026-11-10T10:55:29Z\",\"trackTimeMillis\":1904000}"
	"]}";

static const char *APPLE_ONE_FEED =
	"{\"resultCount\":1,\"results\":["
	"{\"wrapperType\":\"track\",\"kind\":\"podcast\",\"collectionId\":369369012,\"trackId\":369369012,"
	"\"artistName\":\"RTL\",\"collectionName\":\"Les Grosses T\\u00eates\","
	"\"artworkUrl600\":\"https://is1-ssl.mzstatic.com/image/thumb/d.jpg/600x600bb.jpg\",\"trackCount\":124}"
	"]}";

// Podcast Index's search, and its way of saying it found nothing: a 200 with
// the truth in a string field.
static const char *PI_SEARCH =
	"{\"status\":\"true\",\"feeds\":[{\"id\":777,\"title\":\"A show\",\"author\":\"Someone\","
	"\"artwork\":\"https://pi.example/art.jpg\",\"episodeCount\":42}]}";

static const char *PI_NOTHING = "{\"status\":\"false\",\"description\":\"Nothing found\"}";

// ---------------------------------------------------------------------------
// a show too long for one lookup, and the feed that has all of it
// ---------------------------------------------------------------------------
//
// These are the fixtures the whole fallback turns on. Apple's episode lookup
// answers with two hundred rows whatever it is asked for, so a show of four
// hundred is a list whose first two hundred episodes cannot be reached at all
// -- there is no offset and no larger limit, which was verified against a real
// show rather than assumed. The only source with the rest is the feed the show
// publishes, which is the one both directories are built from anyway.
//
// The lookup is generated rather than pasted, because what matters is its
// length: two hundred rows is what the ceiling looks like from the inside, and
// a hundred and ninety-nine is the case just under it, where nothing is
// missing and the feed must not be read.

#define LONG_SHOW_ID 1095138637LL
#define LONG_SHOW_FEED "https://feeds.example/long-show.xml"

static char *make_lookup(int rows, long long track_count, const char *feed_url) {
	size_t size = (size_t)rows * 400 + 512;
	char *out = malloc(size);
	if (!out) {
		return NULL;
	}
	int at = snprintf(out, size, "{\"resultCount\":%d,\"results\":[", rows + 1);
	at += snprintf(out + at, size - (size_t)at,
				   "{\"wrapperType\":\"track\",\"kind\":\"podcast\",\"collectionId\":%lld,\"trackId\":%lld,"
				   "\"artistName\":\"Rusty Quill\",\"collectionName\":\"A Long Show\",\"trackCount\":%lld",
				   LONG_SHOW_ID, LONG_SHOW_ID, track_count);
	if (feed_url && feed_url[0]) {
		at += snprintf(out + at, size - (size_t)at, ",\"feedUrl\":\"%s\"", feed_url);
	}
	at += snprintf(out + at, size - (size_t)at, "}");
	for (int i = 0; i < rows; i++) {
		at += snprintf(out + at, size - (size_t)at,
					   ",{\"wrapperType\":\"podcastEpisode\",\"kind\":\"podcast-episode\","
					   "\"trackId\":%d,\"collectionId\":%lld,\"collectionName\":\"A Long Show\","
					   "\"trackName\":\"Ep %d\",\"episodeUrl\":\"https://cdn.example/%d.mp3\","
					   "\"episodeFileExtension\":\"mp3\",\"episodeGuid\":\"guid-%d\","
					   "\"releaseDate\":\"2026-11-10T10:55:29Z\",\"trackTimeMillis\":1904000}",
					   1000800000 + i, LONG_SHOW_ID, i, i, i);
	}
	snprintf(out + at, size - (size_t)at, "]}");
	return out;
}

// The same show's own feed, built to a size because its size is the point: a
// feed with fewer episodes than the directory handed over is the paywalled
// case below, not the one this fallback is for. The first three items carry
// the shapes real feeds are written in -- indented text, CDATA with an entity
// inside it, a duration in each of its three shapes, a date with and without
// one -- the last real item is the oldest episode of all, and after it an item
// with no title, which is not an episode and must not be shown.
//
// `middle` filler episodes go between the special ones and the oldest.
static char *make_rss(int middle) {
	size_t size = (size_t)(middle + 8) * 320 + 1024;
	char *out = malloc(size);
	if (!out) {
		return NULL;
	}
	int at = snprintf(out, size,
					  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
					  "<rss version=\"2.0\" xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\">\n"
					  "  <channel>\n"
					  "    <title>A Long Show</title>\n"
					  "    <description>Every episode, oldest included.</description>\n"
					  "    <item>\n"
					  "      <title>\n        Ep 0\n      </title>\n"
					  "      <guid isPermaLink=\"false\">guid-0</guid>\n"
					  "      <enclosure url=\"https://cdn.example/0.mp3\" type=\"audio/mpeg\" length=\"1\"/>\n"
					  "      <pubDate>Tue, 10 Nov 2026 10:55:29 GMT</pubDate>\n"
					  "      <itunes:duration>00:31:44</itunes:duration>\n"
					  "      <itunes:image href=\"https://art.example/0.jpg\"/>\n"
					  "    </item>\n"
					  "    <item>\n"
					  "      <title><![CDATA[Ep 1 &amp; friends]]></title>\n"
					  "      <guid>guid-1</guid>\n"
					  "      <enclosure url=\"https://cdn.example/1.mp3\" type=\"audio/mpeg\"/>\n"
					  "      <pubDate>2026-11-09T08:00:00Z</pubDate>\n"
					  "      <itunes:duration>1904</itunes:duration>\n"
					  "    </item>\n"
					  "    <item>\n"
					  "      <title>Ep feed only</title>\n"
					  "      <guid>guid-feed-only</guid>\n"
					  "      <enclosure url=\"https://cdn.example/only.mp3\" type=\"audio/mpeg\"/>\n"
					  "      <itunes:duration>00:20:00</itunes:duration>\n"
					  "    </item>\n");
	for (int i = 0; i < middle; i++) {
		at += snprintf(out + at, size - (size_t)at,
					   "    <item><title>Filler %d</title><guid>guid-r%d</guid>"
					   "<enclosure url=\"https://cdn.example/r%d.mp3\" type=\"audio/mpeg\"/>"
					   "<pubDate>Sun, 09 Nov 2025 09:00:00 GMT</pubDate>"
					   "<itunes:duration>00:30:00</itunes:duration></item>\n",
					   i, i, i);
	}
	// The oldest episode in the archive, and the one a two-hundred-row window
	// never reaches. Its guid is the feed's alone, so its id can only come
	// from it.
	at += snprintf(out + at, size - (size_t)at,
				   "    <item>\n"
				   "      <title>Ep forty</title>\n"
				   "      <guid>guid-old-2016</guid>\n"
				   "      <enclosure url=\"https://cdn.example/40.mp3\" type=\"audio/mpeg\"/>\n"
				   "      <pubDate>Sat, 04 Jun 2016 12:00:00 +0000</pubDate>\n"
				   "      <itunes:duration>25:06</itunes:duration>\n"
				   "    </item>\n"
				   "    <item>\n"
				   "      <title/>\n"
				   "      <guid>guid-no-title</guid>\n"
				   "      <enclosure url=\"https://cdn.example/empty.mp3\" type=\"audio/mpeg\"/>\n"
				   "    </item>\n"
				   "  </channel>\n"
				   "</rss>\n");
	return out;
}

// A feed that is not a list of episodes: an error page, a redirect, a feed
// behind a login. Every one of those is a 200 with HTML in it, and none of
// them is a shorter list of the show's episodes.
static const char *NOT_A_FEED = "<html><head><title>403 Forbidden</title></head><body>Nope</body></html>";

// The show behind its own paywall: a directory that indexes every episode and
// a feed of trailers. This is why the feed is read only when the directory has
// said it came up short -- read it always and this show gets worse.
static const char *TRAILERS_ONLY_RSS =
	"<rss><channel><title>A Show</title>"
	"<item><title>Trailer</title><guid>t1</guid>"
	"<enclosure url=\"https://cdn.example/trailer.mp3\" type=\"audio/mpeg\"/></item>"
	"</channel></rss>";

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static bool headers_carry(const char *what) { return strstr(last_headers, what) != NULL; }

// The unix time of a date the fixtures spell out, computed the same way so the
// assertion is about the parser and not about the arithmetic in this file.
static long unix_of(int year, int mon, int day, int hour, int min, int sec) {
	struct tm when;
	memset(&when, 0, sizeof(when));
	when.tm_year = year - 1900;
	when.tm_mon = mon - 1;
	when.tm_mday = day;
	when.tm_hour = hour;
	when.tm_min = min;
	when.tm_sec = sec;
	return (long)timegm(&when);
}

// ---------------------------------------------------------------------------
// which directory a configuration picks
// ---------------------------------------------------------------------------

static void test_directory_choice(void) {
	// A published image: no keys, nothing configured. This is the case the
	// second directory exists for.
	reset();
	CHECK(podcast_configured());
	net_reset(APPLE_SEARCH);
	podcast_feed_t feeds[4];
	CHECK(podcast_search("anything", feeds, 4) == 1);
	CHECK(strstr(last_url, "itunes.apple.com/search") != NULL);

	// An install with a pair keeps doing what it always did.
	reset();
	keys_set();
	CHECK(podcast_configured());
	net_reset(PI_SEARCH);
	CHECK(podcast_search("anything", feeds, 4) == 1);
	CHECK(strstr(last_url, "api.podcastindex.org/api/1.0/search/byterm") != NULL);

	// Spelled out, either way.
	reset();
	keys_set(); // and the keys do not win over an explicit choice
	config_set("podcast", "api_base", "apple");
	net_reset(APPLE_SEARCH);
	CHECK(podcast_search("anything", feeds, 4) == 1);
	CHECK(strstr(last_url, "itunes.apple.com/search") != NULL);

	reset();
	config_set("podcast", "api_base", "https://itunes.apple.com");
	net_reset(APPLE_SEARCH);
	CHECK(podcast_search("anything", feeds, 4) == 1);
	CHECK(strstr(last_url, "itunes.apple.com/search") != NULL);

	reset();
	keys_set(); // chosen explicitly, and this one does need the pair
	config_set("podcast", "api_base", "https://api.podcastindex.org/api/1.0");
	net_reset(PI_SEARCH);
	CHECK(podcast_search("anything", feeds, 4) == 1);
	CHECK(strstr(last_url, "api.podcastindex.org/api/1.0/search/byterm") != NULL);

	// A host that merely starts with the name is not the name. Without this a
	// typo, or a directory that answers for somebody else, would be trusted
	// with requests shaped for Apple.
	reset();
	keys_set(); // it falls to Podcast Index, which is the point
	config_set("podcast", "api_base", "https://itunes.apple.com.evil.example");
	net_reset(PI_SEARCH);
	CHECK(podcast_search("anything", feeds, 4) == 1);
	CHECK(strstr(last_url, "evil.example/search/byterm") != NULL);
	CHECK(strstr(last_url, "media=podcast") == NULL);

	// And a keyless Podcast Index still says why it cannot work, rather than
	// quietly changing directory under somebody who chose one.
	reset();
	config_set("podcast", "api_base", "https://api.podcastindex.org/api/1.0");
	CHECK(!podcast_configured());
	CHECK(podcast_search("anything", feeds, 4) == -1);
	CHECK_STR(podcast_last_error(), "api_keys_unavailable");
	CHECK(request_count == 0); // refused before spending a request

	printf("  directory choice\n");
}

// ---------------------------------------------------------------------------
// Apple: the search
// ---------------------------------------------------------------------------

static void test_apple_search(void) {
	reset();
	config_set("podcast", "api_base", "apple");
	net_reset(APPLE_SEARCH);

	podcast_feed_t feeds[4];
	int n = podcast_search("the daily", feeds, 4);

	CHECK(n == 1); // the row with no id never reaches the list
	CHECK(feeds[0].id == 1200361736);
	CHECK_STR(feeds[0].title, "The Daily");
	CHECK_STR(feeds[0].author, "The New York Times");
	CHECK_STR(feeds[0].image, "https://is1-ssl.mzstatic.com/image/thumb/a.jpg/600x600bb.jpg");
	CHECK(feeds[0].episode_count == 2776);

	// The request: the term encoded, the media fixed, and no storefront -- the
	// search is of the whole catalogue, as it has always been.
	CHECK(strstr(last_url, "/search?term=the%20daily&media=podcast&limit=4") != NULL);
	CHECK(strstr(last_url, "country=") == NULL);

	// No key, no signature, no time: the whole of what a keyless request is.
	CHECK(!headers_carry("X-Auth"));
	CHECK(!headers_carry("Authorization"));
	CHECK(headers_carry("User-Agent: "));

	// The raw count is what the list asks "is there another page?", and it
	// counts what was sent rather than what was kept.
	CHECK(podcast_last_raw_count() == 2);

	// Apple answers with about a hundred whatever it is asked for, so asking
	// for a thousand would only promise a page that is not there.
	net_reset(APPLE_SEARCH);
	CHECK(podcast_search("the daily", feeds, 4) == 1);
	net_reset(APPLE_SEARCH);
	podcast_feed_t many[2];
	CHECK(podcast_search("the daily", many, 2) == 1);
	CHECK(strstr(last_url, "limit=2") != NULL);
	net_reset(APPLE_SEARCH);
	podcast_feed_t one[1];
	CHECK(podcast_search("the daily", one, 1) == 1);
	CHECK(strstr(last_url, "limit=1") != NULL);

	// An empty term is refused without a request, as it always was.
	CHECK(podcast_search("", feeds, 4) == -1);
	CHECK(podcast_search(NULL, feeds, 4) == -1);
	CHECK(podcast_search("x", feeds, 0) == -1);

	printf("  apple search\n");
}

// ---------------------------------------------------------------------------
// Apple: the chart
// ---------------------------------------------------------------------------

static void test_apple_chart(void) {
	reset();
	config_set("podcast", "api_base", "apple");

	// France, asked for more than fifty: the deepest chart Apple serves.
	region_select_language("fr");
	net_reset(APPLE_CHART_FR);

	podcast_feed_t feeds[4];
	int n = podcast_trending(feeds, 4);
	CHECK(n == 1);
	CHECK(feeds[0].id == 369369012); // a quoted string in the JSON, a number here
	CHECK_STR(feeds[0].title, "Les Grosses T\xC3\xAAtes");
	CHECK_STR(feeds[0].author, "RTL");
	// The chart carries a 100 px square and nothing larger; the same path
	// serves the size the lists want.
	CHECK_STR(feeds[0].image, "https://is1-ssl.mzstatic.com/image/thumb/b.jpg/600x600bb.png");
	CHECK(feeds[0].episode_count == 0); // a ranking says nothing about this
	CHECK(strstr(last_url, "/api/v2/fr/podcasts/top/25/podcasts.json") != NULL);
	CHECK(strstr(last_url, "rss.marketingtools.apple.com") != NULL);

	// The three depths it serves, and no fourth.
	net_reset(APPLE_CHART_FR);
	podcast_feed_t page[60];
	CHECK(podcast_trending(page, 60) == 1);
	CHECK(strstr(last_url, "/top/100/") != NULL);

	net_reset(APPLE_CHART_FR);
	CHECK(podcast_trending(feeds, 4) == 1);
	CHECK(strstr(last_url, "/top/25/") != NULL);

	// A storefront with no chart of its own falls back to the American one,
	// because a foreign chart beats an empty page.
	net_reset(APPLE_CHART_EMPTY);
	CHECK(podcast_trending(feeds, 4) <= 0);
	// Two requests: the chosen storefront, then the fallback.
	CHECK(request_count == 2);
	CHECK(strstr(last_url, "/api/v2/us/podcasts/top/") != NULL);

	// Worldwide is the American chart to begin with, so there is nothing to
	// fall back from and no second request.
	reset();
	config_set("podcast", "api_base", "apple");
	region_select_language("");
	net_reset(APPLE_CHART_FR);
	CHECK(podcast_trending(feeds, 4) == 1);
	CHECK(request_count == 1);
	CHECK(strstr(last_url, "/api/v2/us/podcasts/top/") != NULL);

	// The chart's host is its own key, for the same reason api_base is one.
	reset();
	config_set("podcast", "api_base", "apple");
	config_set("podcast", "charts_base", "https://charts.example");
	net_reset(APPLE_CHART_FR);
	CHECK(podcast_trending(feeds, 4) == 1);
	CHECK(strstr(last_url, "https://charts.example/api/v2/us/podcasts/top/") != NULL);

	printf("  apple chart\n");
}

// ---------------------------------------------------------------------------
// Apple: episodes, and finding one again by id
// ---------------------------------------------------------------------------

static void test_apple_episodes(void) {
	reset();
	config_set("podcast", "api_base", "apple");
	net_reset(APPLE_EPISODES);

	podcast_episode_t episodes[4];
	int n = podcast_episodes(1200361736LL, episodes, 4);

	// Two episode rows and one show row came back; the show is not an episode,
	// the second episode has no audio, and one usable episode is what is left.
	CHECK(n == 1);
	CHECK(podcast_last_raw_count() == 2); // the show row is not counted as an episode
	CHECK_STR(last_url, "https://itunes.apple.com/lookup?id=1200361736&entity=podcastEpisode&limit=4");

	CHECK(episodes[0].id == 1000800977857LL); // eleven digits past a 32-bit long
	CHECK(episodes[0].feed_id == 1200361736LL);
	CHECK_STR(episodes[0].title, "Ep one");
	CHECK_STR(episodes[0].feed_title, "The Daily");
	CHECK_STR(episodes[0].enclosure, "https://cdn.example/one.mp3");
	CHECK_STR(episodes[0].mime, "audio/mp3");
	CHECK(episodes[0].duration_secs == 1904); // milliseconds in, seconds out
	CHECK(episodes[0].published == unix_of(2026, 11, 10, 10, 55, 29));
	CHECK_STR(episodes[0].image, "https://is1-ssl.mzstatic.com/image/thumb/c.jpg/600x600bb.jpg");

	// A show with no date on it is still a date.
	CHECK(unix_of(2026, 11, 9, 0, 0, 0) > 0);

	// Asking for more than Apple serves asks for what it serves.
	net_reset(APPLE_EPISODES);
	podcast_episode_t many[4];
	CHECK(podcast_episodes(1200361736LL, many, 4) >= 0);
	net_reset(APPLE_EPISODES);
	CHECK(podcast_episodes(1200361736LL, many, 1) == 1);
	CHECK(strstr(last_url, "limit=1") != NULL);

	// The guards that were there before either directory was chosen.
	CHECK(podcast_episodes(0, episodes, 4) == -1);
	CHECK(podcast_episodes(-1, episodes, 4) == -1);
	CHECK(podcast_episodes(1200361736LL, NULL, 4) == -1);
	CHECK(podcast_episodes(1200361736LL, episodes, 0) == -1);

	printf("  apple episodes\n");
}

// An episode id the player has never seen. This has to run before any list has
// been fetched, because the table of shows an episode was seen in is process
// state and a later fetch would make the id a known one.
static void test_apple_episode_unknown(void) {
	reset();
	config_set("podcast", "api_base", "apple");

	// Not guessed at: there is no way to know which show to look it up in, and
	// Apple will not resolve an episode id on its own.
	podcast_episode_t episode;
	net_reset(APPLE_ONE_EPISODE);
	CHECK(!podcast_episode(1000800977857LL, &episode));
	CHECK_STR(podcast_last_error(), "podcast_episode_gone");
	CHECK(request_count == 0); // answered from what is known, without spending a call

	CHECK(!podcast_episode(0, &episode));
	CHECK(!podcast_episode(-5, &episode));
	CHECK(!podcast_episode(1, NULL));

	printf("  apple episode never seen\n");
}

static void test_apple_episode_by_id(void) {
	reset();
	config_set("podcast", "api_base", "apple");

	// Once a list has been fetched, the show it came from is known and the
	// episode is found again inside it.
	net_reset(APPLE_EPISODES);
	podcast_episode_t episodes[4];
	CHECK(podcast_episodes(1200361736LL, episodes, 4) == 1);

	podcast_episode_t episode;
	memset(&episode, 0, sizeof(episode));
	net_reset(APPLE_ONE_EPISODE);
	CHECK(podcast_episode(1000800977857LL, &episode));
	CHECK(episode.id == 1000800977857LL);
	CHECK(episode.feed_id == 1200361736LL);
	CHECK_STR(episode.enclosure, "https://cdn.example/one.mp3");
	CHECK(episode.duration_secs == 1904);
	// The lookup went to the show it was remembered against, and asked for the
	// deepest list Apple serves, because the episode need not be a recent one.
	CHECK(strstr(last_url, "lookup?id=1200361736&entity=podcastEpisode&limit=200") != NULL);

	// An id that is known but not in the list it is looked up in is gone, not
	// silently replaced by whichever episode came first.
	memset(&episode, 0, sizeof(episode));
	net_reset(APPLE_ONE_EPISODE);
	CHECK(!podcast_episode(1000800746193LL, &episode));
	CHECK_STR(podcast_last_error(), "podcast_episode_gone");

	printf("  apple episode by id\n");
}

// ---------------------------------------------------------------------------
// Apple: one podcast, and the failures both directories share
// ---------------------------------------------------------------------------

static void test_apple_feed_and_errors(void) {
	reset();
	config_set("podcast", "api_base", "apple");

	net_reset(APPLE_ONE_FEED);
	podcast_feed_t feed;
	CHECK(podcast_feed(369369012LL, &feed));
	CHECK(feed.id == 369369012LL);
	CHECK_STR(feed.title, "Les Grosses T\xC3\xAAtes");
	CHECK_STR(feed.author, "RTL");
	CHECK(feed.episode_count == 124);
	CHECK_STR(last_url, "https://itunes.apple.com/lookup?id=369369012&entity=podcast");

	// A show that is not there, which is an empty list rather than an error.
	net_reset("{\"resultCount\":0,\"results\":[]}");
	CHECK(!podcast_feed(369369012LL, &feed));
	CHECK_STR(podcast_last_error(), "podcast_gone");

	CHECK(!podcast_feed(0, &feed));
	CHECK(!podcast_feed(1, NULL));

	// The failures, which read the same whichever directory refused.
	podcast_feed_t feeds[4];

	net_reset(NULL);
	reply_status = 503;
	CHECK(podcast_search("x", feeds, 4) == -1);
	CHECK_STR(podcast_last_error(), "podcast_the_podcast_directory_is_not_answering");

	net_reset(NULL);
	reply_status = 404;
	CHECK(podcast_search("x", feeds, 4) == -1);
	CHECK(strcmp(podcast_last_error(), "podcast_directory_status") == 0);

	net_reset(NULL);
	reply_offline = true;
	snprintf(http_error, sizeof(http_error), "%s", "the Wi-Fi is off");
	CHECK(podcast_search("x", feeds, 4) == -1);
	CHECK_STR(podcast_last_error(), "the Wi-Fi is off");

	net_reset(NULL);
	reply_offline = true;
	http_error[0] = '\0';
	CHECK(podcast_search("x", feeds, 4) == -1);
	CHECK_STR(podcast_last_error(), "podcast_i_cannot_reach_the_podcast_directory");

	// A body that is not JSON at all.
	net_reset("<html>not json</html>");
	CHECK(podcast_search("x", feeds, 4) == -1);
	CHECK_STR(podcast_last_error(), "podcast_directory_unparsable");

	printf("  apple one podcast, and failures\n");
}

// ---------------------------------------------------------------------------
// Podcast Index: unchanged for whoever has a pair
// ---------------------------------------------------------------------------

static void test_podcast_index_unchanged(void) {
	reset();
	keys_set();

	net_reset(PI_SEARCH);
	podcast_feed_t feeds[4];
	CHECK(podcast_search("a show", feeds, 4) == 1);
	CHECK(feeds[0].id == 777);
	CHECK_STR(feeds[0].title, "A show");
	CHECK(feeds[0].episode_count == 42);
	CHECK_STR(last_url, "https://api.podcastindex.org/api/1.0/search/byterm?q=a%20show&max=4");

	// The signature: three headers, and the third is the SHA-1 of key, secret
	// and the time in the second. Read the time back out of the request and
	// sign it again, which checks the construction rather than the clock.
	CHECK(headers_carry("X-Auth-Key: ABCDEF1234\r\n"));
	CHECK(headers_carry("X-Auth-Date: "));

	const char *at = strstr(last_headers, "X-Auth-Date: ");
	CHECK(at != NULL);
	if (at) {
		long when = strtol(at + strlen("X-Auth-Date: "), NULL, 10);
		CHECK(when > 1700000000L); // a real time, not 1970 and not garbage

		char joined[256];
		snprintf(joined, sizeof(joined), "%s%s%ld", "ABCDEF1234", "s3cr3t", when);
		char want[SHA1_HEX_LEN];
		sha1_hex(joined, strlen(joined), want);

		const char *sig = strstr(last_headers, "Authorization: ");
		CHECK(sig != NULL);
		if (sig) {
			char got[SHA1_HEX_LEN] = "";
			snprintf(got, sizeof(got), "%.*s", SHA1_HEX_LEN - 1, sig + strlen("Authorization: "));
			CHECK_STR(got, want);
		}
	}

	// A 200 that says it found nothing is a refusal, not an empty list.
	net_reset(PI_NOTHING);
	CHECK(podcast_search("a show", feeds, 4) == -1);
	CHECK_STR(podcast_last_error(), "Nothing found");

	printf("  podcast index unchanged\n");
}

// ---------------------------------------------------------------------------
// the settings, which neither directory has anything to do with
// ---------------------------------------------------------------------------

static void test_settings_untouched(void) {
	reset();

	CHECK(podcast_skip_back() == PODCAST_SKIP_SHORT);
	CHECK(podcast_skip_forward() == PODCAST_SKIP_LONG);

	podcast_set_skip_back(PODCAST_SKIP_HUGE);
	CHECK(podcast_skip_back() == PODCAST_SKIP_HUGE);
	podcast_set_skip_back(700); // not a value the buttons can draw
	CHECK(podcast_skip_back() == PODCAST_SKIP_SHORT);

	CHECK(podcast_speed_permille() == 1000);
	podcast_set_speed_permille(1500);
	CHECK(podcast_speed_permille() == 1500);
	podcast_set_speed_permille(99999);
	CHECK(podcast_speed_permille() == 1000);

	CHECK(!podcast_stop_at_episode_end());
	podcast_set_stop_at_episode_end(true);
	CHECK(podcast_stop_at_episode_end());

	// The chart's cache name still picks out exactly one chart, and changes
	// with the choice, so a file written for one region is never read back as
	// another's.
	region_select_language("fr");
	CHECK_STR(podcast_trending_cache_key(), "pod_trending_fr");
	region_select_language("it");
	CHECK_STR(podcast_trending_cache_key(), "pod_trending_it");
	region_select_language("");
	CHECK_STR(podcast_trending_lang(), "");
	CHECK_STR(podcast_trending_lang_effective(), "en");
	CHECK_STR(podcast_trending_cache_key(), "pod_trending_en");

	printf("  settings untouched\n");
}

// ---------------------------------------------------------------------------
// the episodes a directory does not list
// ---------------------------------------------------------------------------

// A lookup that hit Apple's ceiling on a show that says it has more, answered
// from the show's own feed instead. Four hundred episodes where the directory
// has two hundred.
static void test_long_show_from_feed(void) {
	reset();
	config_set("podcast", "api_base", "apple");

	char *lookup = make_lookup(200, 405, LONG_SHOW_FEED);
	char *rss = make_rss(400);
	CHECK(lookup != NULL && rss != NULL);
	route_add("entity=podcastEpisode", lookup);
	route_add(LONG_SHOW_FEED, rss);

	static podcast_episode_t episodes[405];
	int n = podcast_episodes(LONG_SHOW_ID, episodes, 405);

	// Every episode the feed has, and not the one with no title in it.
	CHECK(n == 404);
	CHECK(podcast_last_raw_count() == 405); // items in the feed, the no-title one among them
	CHECK(was_fetched("entity=podcastEpisode"));
	CHECK(was_fetched(LONG_SHOW_FEED));

	// In the order the feed lists them, newest first.
	CHECK_STR(episodes[0].title, "Ep 0");
	CHECK(episodes[0].title[0] == 'E'); // the indentation around it is not part of it
	CHECK_STR(episodes[1].title, "Ep 1 & friends"); // CDATA, with an entity inside
	CHECK_STR(episodes[2].title, "Ep feed only");

	// An episode both sources know keeps the id the directory gave it, because
	// that is the id its downloaded file and its heard mark are keyed on. A
	// switch of source under the reader must not orphan either.
	CHECK(episodes[0].id == 1000800000);
	CHECK(episodes[1].id == 1000800001);
	CHECK_STR(episodes[0].mime, "audio/mpeg");
	CHECK(episodes[0].duration_secs == 31 * 60 + 44);
	CHECK(episodes[0].published == unix_of(2026, 11, 10, 10, 55, 29));
	CHECK_STR(episodes[0].image, "https://art.example/0.jpg"); // itunes:image, href attribute
	CHECK(episodes[1].duration_secs == 1904); // a bare number of seconds
	CHECK(episodes[1].published == unix_of(2026, 11, 9, 8, 0, 0)); // ISO 8601 rather than RFC 822

	// One only the feed knows gets an id derived from its guid: not zero, not
	// something a directory would hand out, and the same every time.
	CHECK(episodes[2].id == podcastrss_guid_id("guid-feed-only"));
	CHECK(episodes[2].id > 0);
	CHECK(episodes[2].duration_secs == 20 * 60);
	CHECK(episodes[2].published == 0); // a feed with no date on it is still an episode

	// The oldest one, the reason this exists: a 2016 episode that no two
	// hundred rows of a directory will ever reach.
	int last = n - 1;
	CHECK_STR(episodes[last].title, "Ep forty");
	CHECK(episodes[last].id == podcastrss_guid_id("guid-old-2016"));
	CHECK(episodes[last].id != episodes[2].id);
	CHECK(episodes[last].feed_id == LONG_SHOW_ID);
	CHECK_STR(episodes[last].feed_title, "A Long Show");
	CHECK_STR(episodes[last].enclosure, "https://cdn.example/40.mp3");
	CHECK(episodes[last].duration_secs == 25 * 60 + 6);
	CHECK(episodes[last].published == unix_of(2016, 6, 4, 12, 0, 0));

	// And it plays: asked for by the id it was listed under, the episode is
	// found in the feed it came from rather than reported gone.
	podcast_episode_t one;
	CHECK(podcast_episode(episodes[last].id, &one));
	CHECK(one.id == episodes[last].id);
	CHECK_STR(one.enclosure, "https://cdn.example/40.mp3");
	CHECK_STR(one.title, "Ep forty");

	// One listed four hundred rows ago, and the memory of which show an
	// episode belongs to has to reach that far back or the row on screen
	// answers "gone" when it is touched. This is what the size of that memory
	// is for: a show's whole list, not the last screenful of it.
	CHECK(podcast_episode(episodes[2].id, &one));
	CHECK_STR(one.title, "Ep feed only");

	// An episode no source has is still gone, and says so rather than guessing.
	CHECK(!podcast_episode(podcastrss_guid_id("guid-never-published"), &one));
	CHECK_STR(podcast_last_error(), "podcast_episode_gone");

	free(lookup);
	free(rss);
	printf("  long show, from its own feed\n");
}

// The ways the feed can fail to be the better source, each of which has to
// leave the directory's list exactly as it was. A fallback that can make
// things worse is worse than no fallback.
static void test_feed_not_taken_when_it_does_not_help(void) {
	static podcast_episode_t episodes[200];

	// 1. The directory has all of them. A show of two hundred episodes that
	//    answers with two hundred is not a truncated list.
	reset();
	config_set("podcast", "api_base", "apple");
	char *complete = make_lookup(200, 200, LONG_SHOW_FEED);
	char *rss = make_rss(400);
	route_add("entity=podcastEpisode", complete);
	route_add(LONG_SHOW_FEED, rss);
	CHECK(podcast_episodes(LONG_SHOW_ID, episodes, 200) == 200);
	CHECK(podcast_last_raw_count() == 200);
	CHECK(!was_fetched(LONG_SHOW_FEED));
	CHECK_STR(episodes[0].title, "Ep 0");
	free(complete);
	free(rss);

	// 2. A page smaller than the ceiling. Apple answers a request for sixty
	//    with sixty and says nothing about what else there is, so that is not
	//    evidence of a truncated list -- and reading the feed here would send
	//    every long show off to it on the very first page.
	reset();
	config_set("podcast", "api_base", "apple");
	char *first_page = make_lookup(200, 405, LONG_SHOW_FEED);
	rss = make_rss(400);
	route_add("entity=podcastEpisode", first_page);
	route_add(LONG_SHOW_FEED, rss);
	CHECK(podcast_episodes(LONG_SHOW_ID, episodes, 60) == 60);
	CHECK(!was_fetched(LONG_SHOW_FEED));
	free(first_page);
	free(rss);

	// 3. A directory that indexes more than the feed publishes -- the show
	//    behind its own paywall, whose feed is trailers. The lookup came up
	//    short on the numbers and the feed was read, and the directory's
	//    longer list is what the reader gets anyway. This is the case that
	//    decides the fallback has to be conditional.
	reset();
	config_set("podcast", "api_base", "apple");
	char *paywalled = make_lookup(200, 405, "https://feeds.example/trailers.xml");
	route_add("entity=podcastEpisode", paywalled);
	route_add("trailers.xml", TRAILERS_ONLY_RSS);
	CHECK(podcast_episodes(LONG_SHOW_ID, episodes, 200) == 200);
	CHECK(podcast_last_raw_count() == 200);
	CHECK(was_fetched("trailers.xml")); // it was asked, and then not believed
	CHECK_STR(episodes[0].title, "Ep 0");
	free(paywalled);

	printf("  feed left alone when it does not help\n");
}

// A feed that cannot be read is not an error, because the directory's list is
// still there. Nothing here should leave a message up on the page.
static void test_feed_that_cannot_be_read(void) {
	static podcast_episode_t episodes[200];
	char *lookup = make_lookup(200, 405, LONG_SHOW_FEED);

	// An error page where the feed was: a 200, and no items in it.
	reset();
	config_set("podcast", "api_base", "apple");
	route_add("entity=podcastEpisode", lookup);
	route_add(LONG_SHOW_FEED, NOT_A_FEED);
	CHECK(podcast_episodes(LONG_SHOW_ID, episodes, 200) == 200);
	CHECK(podcast_last_raw_count() == 200);
	CHECK_STR(episodes[0].title, "Ep 0");
	CHECK_STR(podcast_last_error(), "");

	// A feed host that does not answer at all.
	reset();
	config_set("podcast", "api_base", "apple");
	route_add("entity=podcastEpisode", lookup);
	route_offline(LONG_SHOW_FEED);
	CHECK(podcast_episodes(LONG_SHOW_ID, episodes, 200) == 200);
	CHECK_STR(podcast_last_error(), "");

	// A show whose row carries no feed address: nothing to ask, and the list
	// is the directory's.
	reset();
	config_set("podcast", "api_base", "apple");
	char *no_feed = make_lookup(200, 405, "");
	route_add("entity=podcastEpisode", no_feed);
	CHECK(podcast_episodes(LONG_SHOW_ID, episodes, 200) == 200);
	CHECK(!was_fetched("feeds.example"));

	// A list just under the ceiling is not a truncated one, even on a show
	// that has far more.
	reset();
	config_set("podcast", "api_base", "apple");
	char *rss = make_rss(400);
	char *short_list = make_lookup(199, 405, LONG_SHOW_FEED);
	route_add("entity=podcastEpisode", short_list);
	route_add(LONG_SHOW_FEED, rss);
	CHECK(podcast_episodes(LONG_SHOW_ID, episodes, 200) == 199);
	CHECK(!was_fetched(LONG_SHOW_FEED));

	free(lookup);
	free(no_feed);
	free(short_list);
	free(rss);
	printf("  feed that cannot be read\n");
}

int main(void) {
	printf("podcast directory tests\n");

	// First, and before anything fetches a list: it is the one case whose
	// answer depends on nothing having been remembered yet.
	test_apple_episode_unknown();

	test_directory_choice();
	test_apple_search();
	test_apple_chart();
	test_apple_episodes();
	test_apple_episode_by_id();
	test_apple_feed_and_errors();
	test_long_show_from_feed();
	test_feed_not_taken_when_it_does_not_help();
	test_feed_that_cannot_be_read();

	test_podcast_index_unchanged();
	test_settings_untouched();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("podcast directory tests passed");
	return 0;
}
