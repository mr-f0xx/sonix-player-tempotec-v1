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

static void net_reset(const char *body) {
	reply_body = body;
	reply_status = 0;
	reply_offline = false;
	snprintf(http_error, sizeof(http_error), "%s", "");
	last_url[0] = '\0';
	last_headers[0] = '\0';
	request_count = 0;
}

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len,
				 size_t limit, int timeout_secs, int *status_out) {
	(void)want_error_body;
	(void)limit;
	(void)timeout_secs;

	request_count++;
	snprintf(last_url, sizeof(last_url), "%s", url ? url : "");
	snprintf(last_headers, sizeof(last_headers), "%s", extra_headers ? extra_headers : "");

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
	test_podcast_index_unchanged();
	test_settings_untouched();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("podcast directory tests passed");
	return 0;
}
