#include "podcast.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/core/region.h"
#include "src/system/net/http.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/sha1.h"
#include "src/system/streaming/streamkeys.h"

// ---------------------------------------------------------------------------
// The two directories, and which one is in force
// ---------------------------------------------------------------------------
//
// Podcast Index is what this was written against. It is an open catalogue that
// searches, and answers with already normalized JSON, which is what the
// player's parser reads. It asks for a key pair and a signature on every
// request, and its terms do not allow registrations from a free email service
// -- so a published image, which ships without keys because there is no other
// way it could be published, has a Podcasts section that announces missing
// keys and does nothing else. Neither does a player belonging to somebody who
// has no address on a domain of their own to register one with.
//
// Apple's directory needs no key at all: a plain GET answers with the same
// shape of data, and the audio address it hands back is the podcast's own.
// That is what makes it the directory a keyless player can actually use, so it
// is the one a keyless player gets. It costs no registration, no clock and no
// signature, and it is the reason the Podcasts section now works out of the
// box on an image that carries nothing secret.
//
// What it gives up: the search answers with about a hundred results and has no
// offset behind them, the chart comes at three depths and no others, and an
// episode cannot be looked up on its own -- see the note by the table of
// podcasts an episode was seen in, below.
//
// The choice is [podcast] api_base:
//
//     [podcast]
//     api_base = apple                                   Apple's directory
//     api_base = https://itunes.apple.com                the same, spelled out
//     api_base = https://api.podcastindex.org/api/1.0    Podcast Index
//
// and with no api_base at all the keys decide: a pair means Podcast Index, so
// an install that has been working keeps working exactly as it did, and no
// pair means Apple.

#define PODCAST_API_DEFAULT "https://api.podcastindex.org/api/1.0"
#define APPLE_API_DEFAULT "https://itunes.apple.com"

// Apple keeps its chart on a second host. It is its own key rather than
// something derived from api_base because the two really are different hosts,
// and because the reason api_base is configurable at all -- the day a service
// moves, or a build wants to aim at a stand-in -- applies to this one as well.
#define APPLE_CHARTS_DEFAULT "https://rss.marketingtools.apple.com"

// The most a response is expected to carry. A page of sixty episodes with long
// descriptions stays well under half a megabyte, but the lists can now ask for
// up to 240 at once because the directory has no offset parameter. Four
// megabytes covers the worst case and still cuts off anything else. Apple's
// episode lookup adds its own worst case, two hundred episodes each with a
// show note of a few kilobytes, which this also covers.
#define PODCAST_BODY_MAX (4 * 1024 * 1024)
#define PODCAST_TIMEOUT_SECS 15

static __thread char last_error[256];

const char *podcast_last_error(void) { return last_error; }

static void set_error(const char *what) { snprintf(last_error, sizeof(last_error), "%s", what ? what : ""); }

typedef enum { DIR_PODCAST_INDEX, DIR_APPLE } directory_t;

// The configured base, or NULL when nobody configured one. Read without its
// default so that "not set" can be told apart from "set to the default", which
// is what lets the keys make the choice when nobody has. A line reading
// `api_base =` with nothing after it is read as not set too: config_get hands
// back the empty string it was given rather than a fallback, and an empty base
// would turn every request into a relative one.
static const char *configured_api_base(void) {
	const char *base = config_get("podcast", "api_base", NULL);
	return base && base[0] ? base : NULL;
}

static const char *api_base(void) {
	const char *base = configured_api_base();
	return base ? base : PODCAST_API_DEFAULT;
}

// Whether `base` is a URL on `host`. Anything that ends the host -- a port, a
// path, a query, or the end of the string -- ends the comparison, so a host
// that merely starts with the name does not pass for it.
static bool base_is_host(const char *base, const char *host) {
	const char *at = strstr(base, "://");
	at = at ? at + 3 : base;
	size_t len = strlen(host);
	if (strncmp(at, host, len) != 0) {
		return false;
	}
	char next = at[len];
	return next == '\0' || next == '/' || next == ':' || next == '?';
}

static directory_t directory(void) {
	const char *base = configured_api_base();
	if (!base) {
		return streamkeys_podcast_key() && streamkeys_podcast_secret() ? DIR_PODCAST_INDEX : DIR_APPLE;
	}
	if (strcasecmp(base, "apple") == 0 || base_is_host(base, "itunes.apple.com") ||
		base_is_host(base, "rss.marketingtools.apple.com")) {
		return DIR_APPLE;
	}
	return DIR_PODCAST_INDEX;
}

bool podcast_configured(void) {
	// Apple's directory asks for nothing, so there is no key here to be
	// missing: the section that used to say "streaming keys not found" on a
	// published image now says nothing and works.
	if (directory() == DIR_APPLE) {
		return true;
	}
	return streamkeys_podcast_key() && streamkeys_podcast_secret();
}

// The base the Apple calls are made against. The configured one when it is a
// URL on Apple's search host, Apple's own otherwise -- which covers both the
// short spelling and a Podcast Index base chosen by somebody with no keys.
static const char *apple_base(void) {
	const char *base = configured_api_base();
	if (base && base_is_host(base, "itunes.apple.com")) {
		return base;
	}
	return APPLE_API_DEFAULT;
}

static const char *apple_charts_base(void) {
	const char *base = config_get("podcast", "charts_base", NULL);
	return base && base[0] ? base : APPLE_CHARTS_DEFAULT;
}

// The ISO code of the interface language, so a user running the player in
// Italian does not get an all-English chart. Podcast Index takes a
// comma-separated list: the regional tag Brazilian feeds declare is listed
// beside the bare code. Apple's chart wants the country instead, which the
// region row already carries, so this is only the Podcast Index side's.
//
// The table lives here rather than in lang.c because this is its only caller:
// lang.c works in language names, which is what a menu needs, and teaching it
// ISO codes for one caller would put weight in the wrong place.
static const char *interface_language_code(void) {
	static const struct {
		const char *name;
		const char *code;
	} MAP[] = {
		{"Italiano", "it"},
		{"English", "en"},
		{"Deutsch", "de"},
		{"Fran\xC3\xA7\x61is", "fr"},
		{"Espa\xC3\xB1ol", "es"},
		{"Japanese", "ja"},
		{"Nederlands", "nl"},
		{"Polski", "pl"},
		{"Russian", "ru"},
		{"Portugu\xC3\xAAs BR", "pt,pt-br"},
		{"Ukrainian", "uk"},
	};
	const char *current = lang_current();
	for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
		if (current && strcmp(current, MAP[i].name) == 0) {
			return MAP[i].code;
		}
	}
	return "en";
}

// ---------------------------------------------------------------------------
// Podcast Index: the signed request
// ---------------------------------------------------------------------------

// The ceiling the directory accepts for `max`: a thousand, per its docs. The
// lists ask for more than one screenful at a time when scrolled to the bottom,
// since the directory has no offset, so the right limit here is the API's and
// not that of a single on-screen page.
#define PODCAST_API_MAX 1000

// Fetches `path` (which starts with /) and returns the body for the caller to
// free. NULL on error, with last_error already written.
static char *api_get(const char *path) {
	const char *key = streamkeys_podcast_key();
	const char *secret = streamkeys_podcast_secret();
	if (!key || !secret) {
		set_error(tr("api_keys_unavailable"));
		return NULL;
	}

	// Wall-clock seconds. Podcast Index tolerates a few minutes of skew and
	// answers 401 beyond that; the 401 handling below recognises that case.
	long now = (long)time(NULL);

	char joined[256];
	snprintf(joined, sizeof(joined), "%s%s%ld", key, secret, now);
	char signature[SHA1_HEX_LEN];
	sha1_hex(joined, strlen(joined), signature);

	// The User-Agent is not politeness: the directory requires it and refuses
	// requests without one.
	char headers[512];
	snprintf(headers, sizeof(headers),
			 "X-Auth-Key: %s\r\n"
			 "X-Auth-Date: %ld\r\n"
			 "Authorization: %s\r\n"
			 "User-Agent: " HTTP_USER_AGENT "\r\n",
			 key, now, signature);

	char url[PODCAST_URL_MAX + 256];
	snprintf(url, sizeof(url), "%s%s", api_base(), path);

	char *body = NULL;
	size_t len = 0;
	int status = 0;

	// want_error_body is true, which matters: with it http_get_ex returns true
	// on a 4xx and hands over the body, where the directory explains itself.
	// It also means the status has to be checked below rather than in the
	// failure branch, which now only sees an unreachable network and 5xx.
	if (!http_get_ex(url, headers, true, &body, &len, PODCAST_BODY_MAX, PODCAST_TIMEOUT_SECS, &status)) {
		if (status >= 500) {
			set_error(tr("podcast_the_podcast_directory_is_not_answering"));
		} else if (status > 0) {
			snprintf(last_error, sizeof(last_error), tr("podcast_directory_status"), status);
		} else {
			const char *why = http_last_error();
			set_error(why && why[0] ? why : tr("podcast_i_cannot_reach_the_podcast_directory"));
		}
		free(body);
		return NULL;
	}

	if (status >= 400) {
		if (status == 401 || status == 403) {
			// A 401 from this directory means two very different things. The
			// first is wrong keys. The second is the clock: the signature
			// contains the time, the directory allows only a few minutes of
			// skew, and this device loses the time at every power-off and gets
			// it back from NTP as soon as there is a network. A player started
			// without Wi-Fi believes it is 1970 and has every request refused.
			//
			// A year before 2020 is the unmistakable sign that NTP has not run
			// yet, so the keys are not at fault and saying so points at the
			// right fix.
			struct tm tm_now;
			time_t t = (time_t)now;
			if (gmtime_r(&t, &tm_now) && tm_now.tm_year + 1900 < 2020) {
				set_error(tr("podcast_clock_behind"));
			} else {
				set_error(tr("podcast_the_podcast_directory_refused_the_keys"));
			}
		} else {
			snprintf(last_error, sizeof(last_error), tr("podcast_directory_status"), status);
		}
		free(body);
		return NULL;
	}

	last_error[0] = '\0';
	return body;
}

// ---------------------------------------------------------------------------
// from Podcast Index's JSON to structs
// ---------------------------------------------------------------------------

// How many entries the last response carried, before the unusable ones (the
// episodes with no audio, the shows with no title) were dropped. This is the
// count that says whether the response was full up to the requested ceiling,
// and so whether there is probably more to ask for; judging that on the
// cleaned count is wrong, because a single discard mid-page makes it look
// short when it is not. Both directories report it, and it is per thread, like
// last_error.
static __thread int last_raw_count;

int podcast_last_raw_count(void) { return last_raw_count; }

static void read_feed(const json_doc_t *doc, int obj, podcast_feed_t *out) {
	memset(out, 0, sizeof(*out));
	out->id = json_obj_llong(doc, obj, "id", 0);
	json_obj_str(doc, obj, "title", out->title, sizeof(out->title));
	json_obj_str(doc, obj, "author", out->author, sizeof(out->author));

	// "image" is the podcast's own, "artwork" the already-square one the
	// directory derives. Prefer artwork when present: the list thumbnails are
	// square and a rectangular cover looks distorted in them.
	if (!json_obj_str(doc, obj, "artwork", out->image, sizeof(out->image)) || !out->image[0]) {
		json_obj_str(doc, obj, "image", out->image, sizeof(out->image));
	}
	out->episode_count = (int)json_obj_long(doc, obj, "episodeCount", 0);
}

static void read_episode(const json_doc_t *doc, int obj, podcast_episode_t *out) {
	memset(out, 0, sizeof(*out));
	out->id = json_obj_llong(doc, obj, "id", 0);
	out->feed_id = json_obj_llong(doc, obj, "feedId", 0);
	json_obj_str(doc, obj, "title", out->title, sizeof(out->title));
	json_obj_str(doc, obj, "feedTitle", out->feed_title, sizeof(out->feed_title));
	json_obj_str(doc, obj, "enclosureUrl", out->enclosure, sizeof(out->enclosure));
	json_obj_str(doc, obj, "enclosureType", out->mime, sizeof(out->mime));
	out->published = json_obj_long(doc, obj, "datePublished", 0);
	out->duration_secs = (int)json_obj_long(doc, obj, "duration", 0);

	// The episode's own artwork when it has one, the podcast's otherwise. Many
	// episodes have none and the field comes back empty rather than absent.
	if (!json_obj_str(doc, obj, "image", out->image, sizeof(out->image)) || !out->image[0]) {
		json_obj_str(doc, obj, "feedImage", out->image, sizeof(out->image));
	}
}

// The path every Podcast Index list shares: fetch, check that the directory
// said yes, and walk the named array.
static int pi_fetch_list(const char *path, const char *array_key, void *out, int max, bool episodes) {
	last_raw_count = 0;
	char *body = api_get(path);
	if (!body) {
		return -1;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return -1;
	}

	int root = json_root(&doc);

	// The directory answers 200 even when it found nothing: the truth is in
	// "status", which is the string "true" or "false".
	char status[16] = "";
	if (json_obj_str(&doc, root, "status", status, sizeof(status)) && status[0] &&
		(status[0] == 'f' || status[0] == 'F')) {
		char description[200] = "";
		json_obj_str(&doc, root, "description", description, sizeof(description));
		set_error(description[0] ? description : tr("podcast_the_podcast_directory_found_nothing"));
		json_free(&doc);
		free(body);
		return -1;
	}

	int arr = json_get(&doc, root, array_key);
	int count = 0;
	int len = json_len(&doc, arr);
	last_raw_count = len;
	for (int i = 0; i < len && count < max; i++) {
		int item = json_at(&doc, arr, i);
		if (item < 0) {
			continue;
		}
		if (episodes) {
			podcast_episode_t *e = &((podcast_episode_t *)out)[count];
			read_episode(&doc, item, e);
			// An episode with no audio is not an episode: showing it would
			// give a row that does nothing when touched.
			if (e->id != 0 && e->enclosure[0]) {
				count++;
			}
		} else {
			podcast_feed_t *f = &((podcast_feed_t *)out)[count];
			read_feed(&doc, item, f);
			if (f->id != 0 && f->title[0]) {
				count++;
			}
		}
	}

	json_free(&doc);
	free(body);
	return count;
}

// ---------------------------------------------------------------------------
// Apple's directory: the keyless request
// ---------------------------------------------------------------------------

// A plain GET. The failure wording is Podcast Index's, because every string in
// every language file says "the podcast directory" rather than naming a
// service, and the reader is not helped by knowing which one refused.
//
// The case that cannot happen here is the clock. There is no signature, so
// there is no time in the request, so a player that has not reached NTP yet is
// not refused: the whole of the 401 handling above has no counterpart on this
// side, and that is the point of it.
static char *apple_get(const char *url) {
	char headers[96];
	snprintf(headers, sizeof(headers), "User-Agent: " HTTP_USER_AGENT "\r\n");

	char *body = NULL;
	size_t len = 0;
	int status = 0;

	if (!http_get_ex(url, headers, true, &body, &len, PODCAST_BODY_MAX, PODCAST_TIMEOUT_SECS, &status)) {
		if (status >= 500) {
			set_error(tr("podcast_the_podcast_directory_is_not_answering"));
		} else if (status > 0) {
			snprintf(last_error, sizeof(last_error), tr("podcast_directory_status"), status);
		} else {
			const char *why = http_last_error();
			set_error(why && why[0] ? why : tr("podcast_i_cannot_reach_the_podcast_directory"));
		}
		free(body);
		return NULL;
	}

	if (status >= 400) {
		snprintf(last_error, sizeof(last_error), tr("podcast_directory_status"), status);
		free(body);
		return NULL;
	}

	last_error[0] = '\0';
	return body;
}

// Apple sizes its artwork to the list it came from: a search hit carries a
// 600 px square and a chart row a 100 px one, and nothing else. The same path
// serves any size -- the tag manager's art search already relies on that -- so
// ask for the 600 px one the podcast lists want. The two names are the same
// length, so the URL neither moves nor grows.
static void artwork_up(char *url) {
	char *at = strstr(url, "100x100bb");
	if (at) {
		memcpy(at, "600x600bb", 9);
	}
}

static void read_artwork(const json_doc_t *doc, int obj, char *out, size_t size) {
	out[0] = '\0';
	if (!json_obj_str(doc, obj, "artworkUrl600", out, size) || !out[0]) {
		json_obj_str(doc, obj, "artworkUrl100", out, size);
	}
	artwork_up(out);
}

// Apple's timestamps are ISO 8601 in UTC -- "2026-11-10T10:55:29Z" -- where
// Podcast Index sends the unix number itself. A show carries a date with no
// time on it, which is why the time is optional here; a timestamp this cannot
// read is no timestamp at all, and 0 is what "unknown" already is.
static long apple_published(const json_doc_t *doc, int obj) {
	char text[40] = "";
	if (!json_obj_str(doc, obj, "releaseDate", text, sizeof(text))) {
		return 0;
	}
	struct tm when;
	memset(&when, 0, sizeof(when));
	int fields = sscanf(text, "%d-%d-%dT%d:%d:%d", &when.tm_year, &when.tm_mon, &when.tm_mday, &when.tm_hour,
						&when.tm_min, &when.tm_sec);
	if (fields < 3) {
		return 0;
	}
	when.tm_year -= 1900;
	when.tm_mon -= 1;
	time_t at = timegm(&when);
	return at > 0 ? (long)at : 0;
}

// ---------------------------------------------------------------------------
// which podcast an episode came from
// ---------------------------------------------------------------------------
//
// Apple's lookup will not resolve an episode by itself: ask it for one by id
// and it answers with an empty list, where Podcast Index answers with the
// episode. Its episode ids mean nothing outside the show they belong to, so
// finding one again means first knowing that show.
//
// The lists the player fetches say so -- every episode arrives with its
// podcast's id beside it -- so the pairs are kept as they pass through. This
// is a memory of what has been seen and not a directory: an episode the player
// never listed is not in it, and podcast_episode() then says the episode is
// gone rather than guessing a podcast to look it up in.
//
// Bounded, because the player is a long-lived process on a device with little
// memory to spare. The oldest pairs go first, and an episode step -- a hundred
// -- fits inside this twice over.
#define APPLE_FEED_MAP_MAX 256

static struct {
	long long episode;
	long long feed;
} apple_feed_map[APPLE_FEED_MAP_MAX];
static int apple_feed_map_next;
// The lists are fetched on the page's worker thread and the episode is asked
// for from the one that builds the queue, which is not always the same one.
static pthread_mutex_t apple_feed_map_lock = PTHREAD_MUTEX_INITIALIZER;

static void apple_remember(long long episode_id, long long feed_id) {
	if (episode_id <= 0 || feed_id <= 0) {
		return;
	}
	pthread_mutex_lock(&apple_feed_map_lock);
	for (int i = 0; i < APPLE_FEED_MAP_MAX; i++) {
		if (apple_feed_map[i].episode == episode_id) {
			apple_feed_map[i].feed = feed_id;
			pthread_mutex_unlock(&apple_feed_map_lock);
			return;
		}
	}
	apple_feed_map[apple_feed_map_next].episode = episode_id;
	apple_feed_map[apple_feed_map_next].feed = feed_id;
	apple_feed_map_next = (apple_feed_map_next + 1) % APPLE_FEED_MAP_MAX;
	pthread_mutex_unlock(&apple_feed_map_lock);
}

static long long apple_feed_of(long long episode_id) {
	long long feed = 0;
	pthread_mutex_lock(&apple_feed_map_lock);
	for (int i = 0; i < APPLE_FEED_MAP_MAX; i++) {
		if (apple_feed_map[i].episode == episode_id) {
			feed = apple_feed_map[i].feed;
			break;
		}
	}
	pthread_mutex_unlock(&apple_feed_map_lock);
	return feed;
}

// ---------------------------------------------------------------------------
// from Apple's JSON to structs
// ---------------------------------------------------------------------------

static void read_apple_feed(const json_doc_t *doc, int obj, podcast_feed_t *out) {
	memset(out, 0, sizeof(*out));
	out->id = json_obj_llong(doc, obj, "collectionId", 0);
	json_obj_str(doc, obj, "collectionName", out->title, sizeof(out->title));
	json_obj_str(doc, obj, "artistName", out->author, sizeof(out->author));
	read_artwork(doc, obj, out->image, sizeof(out->image));
	out->episode_count = (int)json_obj_long(doc, obj, "trackCount", 0);
}

static void read_apple_episode(const json_doc_t *doc, int obj, podcast_episode_t *out) {
	memset(out, 0, sizeof(*out));
	out->id = json_obj_llong(doc, obj, "trackId", 0);
	out->feed_id = json_obj_llong(doc, obj, "collectionId", 0);
	json_obj_str(doc, obj, "trackName", out->title, sizeof(out->title));
	json_obj_str(doc, obj, "collectionName", out->feed_title, sizeof(out->feed_title));
	json_obj_str(doc, obj, "episodeUrl", out->enclosure, sizeof(out->enclosure));

	// The declared type, in the form the downloader's extension mapping reads:
	// it looks for the codec's own name inside the string, so "audio/" in
	// front of Apple's file extension is enough, and a feed that declares no
	// extension lands on the same MP3 default that mapping already has.
	char ext[32] = "";
	json_obj_str(doc, obj, "episodeFileExtension", ext, sizeof(ext));
	snprintf(out->mime, sizeof(out->mime), "audio/%s", ext[0] ? ext : "mpeg");

	out->published = apple_published(doc, obj);
	out->duration_secs = (int)(json_obj_llong(doc, obj, "trackTimeMillis", 0) / 1000);
	read_artwork(doc, obj, out->image, sizeof(out->image));
}

// A chart row, which is thinner than a search hit: a ranking and not a
// catalogue entry, so no feed address and no episode count. Both have
// somewhere to go -- the count is documented as 0 when the catalogue does not
// say, and the struct keeps no feed address at all -- so the row is enough to
// draw and to open.
static void read_apple_chart_row(const json_doc_t *doc, int obj, podcast_feed_t *out) {
	memset(out, 0, sizeof(*out));
	// A string here, "369369012", where search and lookup send a number. The
	// reader takes both, which is the only reason this needs no special case.
	out->id = json_obj_llong(doc, obj, "id", 0);
	json_obj_str(doc, obj, "name", out->title, sizeof(out->title));
	json_obj_str(doc, obj, "artistName", out->author, sizeof(out->author));
	read_artwork(doc, obj, out->image, sizeof(out->image));
}

typedef enum { APPLE_FEEDS, APPLE_EPISODES, APPLE_CHART } apple_shape_t;

// The path every Apple list shares: fetch, parse, and walk the array at
// `path`, which is "results" at the root for a search or a lookup and
// "feed.results" for a chart.
static int apple_fetch(const char *url, const char *path, void *out, int max, apple_shape_t shape) {
	last_raw_count = 0;
	char *body = apple_get(url);
	if (!body) {
		return -1;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return -1;
	}

	int arr = json_path(&doc, json_root(&doc), path);
	int len = json_len(&doc, arr);
	int count = 0;
	int seen = 0;

	// The whole array is walked even past `max`, so that the raw count is the
	// number the catalogue sent and not the number this page could hold -- and,
	// for episodes, so that every podcast the response names is remembered and
	// not only the ones that made the page.
	for (int i = 0; i < len; i++) {
		int item = json_at(&doc, arr, i);
		if (item < 0) {
			continue;
		}

		if (shape == APPLE_EPISODES) {
			// A lookup by show id answers with the show itself first and its
			// episodes after it. The show is not an episode, and counting it
			// as one would make a full page look like a page and a half, which
			// is what tells the list to offer another.
			char kind[32] = "";
			json_obj_str(&doc, item, "kind", kind, sizeof(kind));
			if (strcmp(kind, "podcast-episode") != 0) {
				continue;
			}
			seen++;
			apple_remember(json_obj_llong(&doc, item, "trackId", 0), json_obj_llong(&doc, item, "collectionId", 0));
			if (count >= max) {
				continue;
			}
			podcast_episode_t *e = &((podcast_episode_t *)out)[count];
			read_apple_episode(&doc, item, e);
			// An episode with no audio is not an episode: showing it would
			// give a row that does nothing when touched.
			if (e->id != 0 && e->enclosure[0]) {
				count++;
			}
			continue;
		}

		seen++;
		if (count >= max) {
			continue;
		}
		podcast_feed_t *f = &((podcast_feed_t *)out)[count];
		if (shape == APPLE_CHART) {
			read_apple_chart_row(&doc, item, f);
		} else {
			read_apple_feed(&doc, item, f);
		}
		if (f->id != 0 && f->title[0]) {
			count++;
		}
	}

	last_raw_count = seen;
	json_free(&doc);
	free(body);
	return count;
}

// The most a search answers with. Apple documents 200 and sends about a
// hundred, with no offset to reach the rest, so asking for more than it gives
// would only leave the list offering a page that was never there.
#define APPLE_SEARCH_MAX 100

// The most episodes one lookup returns, whatever it is asked for. An older
// episode than these needs the show's own feed, which this does not read.
#define APPLE_EPISODES_MAX 200

static int apple_search(const char *query, podcast_feed_t *out, int max) {
	char encoded[512];
	http_url_encode(query, encoded, sizeof(encoded));

	char url[PODCAST_URL_MAX + 512];
	// No country: the search is of the whole catalogue, as it always was, and
	// narrowing it to a storefront would quietly drop the shows that are not
	// sold in the listener's. The chart is the one place a region belongs.
	snprintf(url, sizeof(url), "%s/search?term=%s&media=podcast&limit=%d", apple_base(), encoded,
			 max > APPLE_SEARCH_MAX ? APPLE_SEARCH_MAX : max);
	return apple_fetch(url, "results", out, max, APPLE_FEEDS);
}

// Apple serves the chart at exactly three depths and no others; a fourth
// answers 404. The smallest that covers the request is the one to ask for, and
// the deepest is the ceiling: a list wanting more than a hundred rows gets a
// hundred, sees that the catalogue sent fewer than it asked for, and stops
// offering another.
static int apple_chart_depth(int want) {
	if (want <= 25) {
		return 25;
	}
	if (want <= 50) {
		return 50;
	}
	return 100;
}

static int apple_trending(podcast_feed_t *out, int max) {
	// The chart is a storefront's, so it takes the country of the region the
	// reader chose -- the same choice Podcast Index's language filter came out
	// of, and the row carries both. Worldwide is the American chart, which is
	// what region_market() already answers for it.
	char country[8] = "";
	region_market(country, sizeof(country), NULL, 0);
	for (char *p = country; *p; p++) {
		*p = (char)tolower((unsigned char)*p);
	}

	int depth = apple_chart_depth(max);
	char url[PODCAST_URL_MAX + 256];
	snprintf(url, sizeof(url), "%s/api/v2/%s/podcasts/top/%d/podcasts.json", apple_charts_base(), country, depth);
	int n = apple_fetch(url, "feed.results", out, max, APPLE_CHART);

	// A storefront with no chart of its own leaves the page saying there is
	// nothing to show, which is a worse answer than somebody else's chart. The
	// American one is the fallback, for the same reason English was.
	if (n <= 0 && strcmp(country, "us") != 0) {
		snprintf(url, sizeof(url), "%s/api/v2/us/podcasts/top/%d/podcasts.json", apple_charts_base(), depth);
		n = apple_fetch(url, "feed.results", out, max, APPLE_CHART);
	}
	return n;
}

static int apple_episodes(long long feed_id, podcast_episode_t *out, int max) {
	char url[PODCAST_URL_MAX + 256];
	snprintf(url, sizeof(url), "%s/lookup?id=%lld&entity=podcastEpisode&limit=%d", apple_base(), feed_id,
			 max > APPLE_EPISODES_MAX ? APPLE_EPISODES_MAX : max);
	return apple_fetch(url, "results", out, max, APPLE_EPISODES);
}

static bool apple_feed(long long feed_id, podcast_feed_t *out) {
	char url[PODCAST_URL_MAX + 256];
	snprintf(url, sizeof(url), "%s/lookup?id=%lld&entity=podcast", apple_base(), feed_id);

	char *body = apple_get(url);
	if (!body) {
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return false;
	}
	// Always a list, even for one: Apple's lookup has no single-object shape
	// the way Podcast Index's "feed" and "episode" fields do.
	int item = json_path(&doc, json_root(&doc), "results.0");
	bool ok = false;
	if (item >= 0) {
		read_apple_feed(&doc, item, out);
		ok = out->id != 0;
	}
	json_free(&doc);
	free(body);
	if (!ok) {
		set_error(tr("podcast_gone"));
	}
	return ok;
}

// One episode, which on this side is two steps: which show the id was seen in,
// and then that show's list, searched. The list is walked in place rather than
// kept, because two hundred of these structs is a third of a megabyte and no
// thread here has that to spare on a lookup.
//
// The cost is a full episode list where Podcast Index sends one object. It is
// paid only on the fallback path -- an episode the queue reached that the list
// on screen does not hold -- and the alternative on this directory is no
// answer at all.
static bool apple_episode(long long episode_id, podcast_episode_t *out) {
	long long feed_id = apple_feed_of(episode_id);
	if (feed_id <= 0) {
		set_error(tr("podcast_episode_gone"));
		return false;
	}

	char url[PODCAST_URL_MAX + 256];
	snprintf(url, sizeof(url), "%s/lookup?id=%lld&entity=podcastEpisode&limit=%d", apple_base(), feed_id,
			 APPLE_EPISODES_MAX);

	char *body = apple_get(url);
	if (!body) {
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return false;
	}

	int arr = json_get(&doc, json_root(&doc), "results");
	int len = json_len(&doc, arr);
	bool ok = false;
	for (int i = 0; i < len && !ok; i++) {
		int item = json_at(&doc, arr, i);
		if (item < 0) {
			continue;
		}
		char kind[32] = "";
		json_obj_str(&doc, item, "kind", kind, sizeof(kind));
		if (strcmp(kind, "podcast-episode") != 0) {
			continue;
		}
		if (json_obj_llong(&doc, item, "trackId", 0) != episode_id) {
			continue;
		}
		read_apple_episode(&doc, item, out);
		ok = out->id != 0 && out->enclosure[0] != '\0';
	}

	json_free(&doc);
	free(body);
	if (!ok) {
		// Known show, episode not in its two hundred most recent: the same
		// answer as a show that does not know it, and the same honest one.
		set_error(tr("podcast_episode_gone"));
	}
	return ok;
}

// ---------------------------------------------------------------------------
// the calls
// ---------------------------------------------------------------------------

int podcast_search(const char *query, podcast_feed_t *out, int max) {
	if (!query || !query[0] || !out || max <= 0) {
		return -1;
	}
	if (directory() == DIR_APPLE) {
		return apple_search(query, out, max);
	}

	char encoded[512];
	http_url_encode(query, encoded, sizeof(encoded));

	char path[640];
	// No `clean`: it would drop podcasts marked explicit, which is not the
	// player's choice to make for the listener.
	snprintf(path, sizeof(path), "/search/byterm?q=%s&max=%d", encoded, max > PODCAST_API_MAX ? PODCAST_API_MAX : max);
	return pi_fetch_list(path, "feeds", out, max, false);
}

// Which chart the Trending card shows. The choice itself lives in the one
// shared list in core/region.h -- the same row Radio and YouTube Music write --
// so this returns that choice's language. Nobody has chosen reads as empty,
// which the caller above has always turned into the interface's own language,
// because the catalogue has no worldwide chart to ask for.
const char *podcast_trending_lang(void) {
	return region_language();
}

// Turn a stored code back into the choice: the entry whose language it is, or
// Worldwide when there is none.
void podcast_set_trending_lang(const char *code) {
	region_select_language(code);
}

const char *podcast_trending_lang_effective(void) {
	const char *want = podcast_trending_lang();
	return want[0] ? want : interface_language_code();
}

// The name the chart is kept under on the card. One per language, and not one
// for every chart: an English list and an Italian one are different lists, and
// a file written for the region chosen a moment ago must not be read back as
// this one's. Apple's chart is a storefront's rather than a language's, but
// the row the reader chose carries one of each and never one without the
// other, so the name still picks out exactly one chart.
const char *podcast_trending_cache_key(void) {
	static char key[64];
	snprintf(key, sizeof(key), "pod_trending_%s", podcast_trending_lang_effective());
	return key;
}

int podcast_trending(podcast_feed_t *out, int max) {
	if (!out || max <= 0) {
		return -1;
	}
	if (directory() == DIR_APPLE) {
		return apple_trending(out, max);
	}

	char path[128];
	// One language, not a list: with ",en" appended the chart filled up with
	// American podcasts, which always trend, and the requested language
	// disappeared among them. The language is the region the reader chose -- the
	// catalogue filters its chart by language and offers no country filter -- and
	// the interface's own when nothing was chosen. When the chosen language
	// yields nothing at all, fall back to English: a foreign chart beats an empty
	// page.
	//
	// A language the directory has no chart for comes back empty, and one it does
	// not recognise comes back as an error; both leave the page saying there is
	// nothing to show, so both fall back. That is the difference between choosing
	// a region and losing the tab.
	const char *want = podcast_trending_lang_effective();
	snprintf(path, sizeof(path), "/podcasts/trending?max=%d&lang=%s",
			 max > PODCAST_API_MAX ? PODCAST_API_MAX : max, want);
	int n = pi_fetch_list(path, "feeds", out, max, false);
	if (n <= 0 && strcmp(want, "en") != 0) {
		snprintf(path, sizeof(path), "/podcasts/trending?max=%d&lang=en",
				 max > PODCAST_API_MAX ? PODCAST_API_MAX : max);
		n = pi_fetch_list(path, "feeds", out, max, false);
	}
	return n;
}

int podcast_episodes(long long feed_id, podcast_episode_t *out, int max) {
	if (feed_id <= 0 || !out || max <= 0) {
		return -1;
	}
	if (directory() == DIR_APPLE) {
		return apple_episodes(feed_id, out, max);
	}

	char path[128];
	snprintf(path, sizeof(path), "/episodes/byfeedid?id=%lld&max=%d", feed_id,
			 max > PODCAST_API_MAX ? PODCAST_API_MAX : max);
	return pi_fetch_list(path, "items", out, max, true);
}

bool podcast_feed(long long feed_id, podcast_feed_t *out) {
	if (feed_id <= 0 || !out) {
		return false;
	}
	if (directory() == DIR_APPLE) {
		return apple_feed(feed_id, out);
	}

	char path[128];
	snprintf(path, sizeof(path), "/podcasts/byfeedid?id=%lld", feed_id);

	char *body = api_get(path);
	if (!body) {
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return false;
	}
	// Here the field is "feed", not "feeds": it is a single object.
	int feed = json_get(&doc, json_root(&doc), "feed");
	bool ok = false;
	if (feed >= 0) {
		read_feed(&doc, feed, out);
		ok = out->id != 0;
	}
	json_free(&doc);
	free(body);
	if (!ok) {
		set_error(tr("podcast_gone"));
	}
	return ok;
}

bool podcast_episode(long long episode_id, podcast_episode_t *out) {
	if (episode_id <= 0 || !out) {
		return false;
	}
	if (directory() == DIR_APPLE) {
		return apple_episode(episode_id, out);
	}

	char path[128];
	snprintf(path, sizeof(path), "/episodes/byid?id=%lld", episode_id);

	char *body = api_get(path);
	if (!body) {
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return false;
	}
	int episode = json_get(&doc, json_root(&doc), "episode");
	bool ok = false;
	if (episode >= 0) {
		read_episode(&doc, episode, out);
		ok = out->id != 0;
	}
	json_free(&doc);
	free(body);
	if (!ok) {
		set_error(tr("podcast_episode_gone"));
	}
	return ok;
}

// ---------------------------------------------------------------------------
// the two skip intervals
// ---------------------------------------------------------------------------

// Only the three values the buttons can draw: a hand-edited config file must
// not be able to give the player a seven-hundred-second skip behind an icon
// that says ten.
static int clamp_skip(int value) {
	if (value == PODCAST_SKIP_LONG) {
		return PODCAST_SKIP_LONG;
	}
	if (value == PODCAST_SKIP_HUGE) {
		return PODCAST_SKIP_HUGE;
	}
	return PODCAST_SKIP_SHORT;
}

// The defaults are not the audiobook ones (-10/+10): on a podcast the forward
// skip is almost always used to jump a sponsor read, and thirty seconds is that
// length.
int podcast_skip_back(void) { return clamp_skip(config_get_int("podcast", "skip_back", PODCAST_SKIP_SHORT)); }

int podcast_skip_forward(void) { return clamp_skip(config_get_int("podcast", "skip_forward", PODCAST_SKIP_LONG)); }

void podcast_set_skip_back(int seconds) { config_set_int("podcast", "skip_back", clamp_skip(seconds)); }

void podcast_set_skip_forward(int seconds) { config_set_int("podcast", "skip_forward", clamp_skip(seconds)); }

int podcast_speed_permille(void) {
	long value = config_get_int("podcast", "speed", 1000);
	if (value < 250 || value > 4000) {
		value = 1000;
	}
	return (int)value;
}

void podcast_set_speed_permille(int permille) {
	if (permille < 250 || permille > 4000) {
		permille = 1000;
	}
	config_set_int("podcast", "speed", permille);
}

double podcast_speed(void) { return (double)podcast_speed_permille() / 1000.0; }

bool podcast_stop_at_episode_end(void) { return config_get_int("podcast", "stop_episode_end", 0) != 0; }

void podcast_set_stop_at_episode_end(bool on) { config_set_int("podcast", "stop_episode_end", on ? 1 : 0); }
