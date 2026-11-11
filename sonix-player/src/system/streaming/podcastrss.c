#include "podcastrss.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/ebook/epub_xml.h"

// ---------------------------------------------------------------------------
// the two readings with more shapes than their callers expect
// ---------------------------------------------------------------------------

// The month's number from the three letters an RFC 822 date spells it with, or
// 0. Case-insensitive, because feeds are not consistent about it.
static int month_number(const char *name) {
	static const char *const NAMES[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
										"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	if (!name) {
		return 0;
	}
	for (int i = 0; i < 12; i++) {
		if (strncasecmp(name, NAMES[i], 3) == 0) {
			return i + 1;
		}
	}
	return 0;
}

long podcastrss_pubdate(const char *text) {
	if (!text || !text[0]) {
		return 0;
	}

	struct tm when;
	memset(&when, 0, sizeof(when));
	char month[16] = "";
	int day = 0;

	// RFC 822, which is what the specification asks for:
	// "Thu, 05 Nov 2026 15:45:00 GMT". The weekday is optional because feeds
	// disagree about it, and so is the time, because a feed that names only a
	// day is still naming a day. What is left of the line -- a zone, a
	// numeric offset, nothing -- is not read: the date is what the list sorts
	// by, and an hour either way changes nothing there.
	int fields = sscanf(text, "%*3s, %d %15s %d %d:%d:%d", &day, month, &when.tm_year, &when.tm_hour, &when.tm_min,
						&when.tm_sec);
	if (fields < 3) {
		fields = sscanf(text, "%d %15s %d %d:%d:%d", &day, month, &when.tm_year, &when.tm_hour, &when.tm_min,
						&when.tm_sec);
	}

	if (fields >= 3) {
		int mon = month_number(month);
		if (mon == 0) {
			return 0;
		}
		when.tm_mon = mon - 1;
		when.tm_mday = day;
	} else {
		// ISO 8601 instead, which some feeds publish: 2026-11-05T15:45:00Z
		int mon = 0;
		if (sscanf(text, "%d-%d-%dT%d:%d:%d", &when.tm_year, &mon, &day, &when.tm_hour, &when.tm_min,
				   &when.tm_sec) < 3) {
			return 0;
		}
		when.tm_mon = mon - 1;
		when.tm_mday = day;
	}

	if (when.tm_mday < 1 || when.tm_mday > 31 || when.tm_mon < 0 || when.tm_mon > 11) {
		return 0;
	}
	// A two-digit year is a guess about a century, and guessing wrong puts an
	// episode a hundred years from the ones beside it, which sorts worse than
	// having no date at all.
	if (when.tm_year < 70) {
		return 0;
	}
	when.tm_year -= 1900;

	time_t at = timegm(&when);
	return at > 0 ? (long)at : 0;
}

int podcastrss_duration_secs(const char *text) {
	if (!text || !text[0]) {
		return 0;
	}
	// Three shapes and no others: "00:40:36", "23:36" and a bare "1416". Read
	// by hand rather than with one sscanf, because the whole string has to be
	// a duration: a pattern that matches its first number would read "40
	// minutes" as forty seconds and the row would lie about its length.
	const char *p = text;
	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	if (!isdigit((unsigned char)*p)) {
		return 0;
	}

	char *after = NULL;
	long first = strtol(p, &after, 10);
	long parts[3] = {first, 0, 0};
	int count = 1;
	p = after;
	while (count < 3 && *p == ':') {
		p++;
		if (!isdigit((unsigned char)*p)) {
			return 0;
		}
		parts[count++] = strtol(p, &after, 10);
		p = after;
	}

	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	if (*p) {
		return 0; // something that is not a number followed the numbers
	}

	long secs = 0;
	if (count == 3) {
		secs = parts[0] * 3600 + parts[1] * 60 + parts[2];
	} else if (count == 2) {
		secs = parts[0] * 60 + parts[1];
	} else {
		secs = parts[0];
	}
	return secs > 0 ? (int)secs : 0;
}

long long podcastrss_guid_id(const char *guid) {
	if (!guid || !guid[0]) {
		return 0;
	}
	// FNV-1a over the guid. Which hash hardly matters; that it is stable
	// across runs is the whole requirement, because the id names the file on
	// the card and the row in the heard list.
	unsigned long long h = 1469598103934665603ULL;
	for (const unsigned char *p = (const unsigned char *)guid; *p; p++) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	// Positive, and far above the ids the directories hand out: those are
	// small enough to fit a 32-bit long, so a derived id cannot be mistaken
	// for one of theirs.
	return (long long)(h >> 1) | (1LL << 62);
}

// ---------------------------------------------------------------------------
// the walk
// ---------------------------------------------------------------------------

static void copy_unescaped(char *out, size_t size, const char *text, size_t len) {
	if (!out || size == 0) {
		return;
	}
	epub_xml_unescape(out, size, text, len);
}

// Drops the spaces and newlines a feed indents with, in place. Text arrives as
// the run between the tags, which on an indented feed is a newline, the text,
// and a newline again -- and a title that begins with a newline is a title the
// list lays out one row lower than the ones beside it.
static void trim_in_place(char *text) {
	char *start = text;
	while (*start && isspace((unsigned char)*start)) {
		start++;
	}
	char *end = start + strlen(start);
	while (end > start && isspace((unsigned char)end[-1])) {
		end--;
	}
	size_t len = (size_t)(end - start);
	if (start != text) {
		memmove(text, start, len);
	}
	text[len] = '\0';
}

static bool only_space(const char *text, size_t len) {
	for (size_t i = 0; i < len; i++) {
		if (!isspace((unsigned char)text[i])) {
			return false;
		}
	}
	return true;
}

// The element whose text is wanted next, so the run of text that follows is
// put somewhere. Nothing else inside an item is read: a feed carries show
// notes, transcripts and whole HTML documents beside the six fields this
// needs, and copying them would fill memory the device does not have.
typedef enum { WANT_NONE, WANT_TITLE, WANT_GUID, WANT_DATE, WANT_DURATION } want_t;

// The six fields of one <item>, gathered while it is walked. A struct rather
// than six locals because both readers want the same set.
typedef struct {
	char title[PODCAST_TITLE_MAX];
	char guid[PODCASTRSS_GUID_MAX];
	char image[PODCAST_URL_MAX];
	char enclosure[PODCAST_URL_MAX];
	char mime[64];
	char date[80];
	char duration[32];
} rss_item_t;

static void item_reset(rss_item_t *it) { memset(it, 0, sizeof(*it)); }

// The episode one <item> describes. False when the item is not an episode --
// no audio, no title, or nothing to key a file on.
static bool item_episode(const rss_item_t *it, long long feed_id, const char *feed_title,
						 const podcastrss_known_t *known, int known_count, podcast_episode_t *out) {
	// An item with no audio is not an episode, and one with no title is not a
	// row anybody can recognise: both would be a line that does nothing when
	// touched.
	if (!it->enclosure[0] || !it->title[0]) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	for (int i = 0; i < known_count; i++) {
		if (known[i].id != 0 && strcmp(known[i].guid, it->guid) == 0) {
			out->id = known[i].id;
			break;
		}
	}
	if (out->id == 0) {
		out->id = podcastrss_guid_id(it->guid);
	}
	if (out->id == 0) {
		return false; // no guid and no directory id: nothing to name a file
	}

	out->feed_id = feed_id;
	snprintf(out->title, sizeof(out->title), "%s", it->title);
	snprintf(out->feed_title, sizeof(out->feed_title), "%s", feed_title ? feed_title : "");
	snprintf(out->enclosure, sizeof(out->enclosure), "%s", it->enclosure);
	snprintf(out->mime, sizeof(out->mime), "%s", it->mime[0] ? it->mime : "audio/mpeg");
	snprintf(out->image, sizeof(out->image), "%s", it->image);
	out->published = podcastrss_pubdate(it->date);
	out->duration_secs = podcastrss_duration_secs(it->duration);
	return true;
}

// One pass over a feed, reading each <item> into `it` and handing it to the
// caller, which decides whether to keep it. Both readers below are this walk
// with a different decision at the end.
typedef bool (*rss_item_fn)(const rss_item_t *it, int index, void *user);

static int feed_walk(const char *body, rss_item_fn each, void *user, int *items_seen) {
	epub_xml_t x;
	epub_xml_init(&x, body);

	int seen = 0;
	bool in_item = false;
	want_t want = WANT_NONE;
	rss_item_t it;
	item_reset(&it);

	while (epub_xml_next(&x)) {
		if (x.event == EPUB_XML_TEXT) {
			if (!in_item || want == WANT_NONE) {
				continue;
			}
			// An element written across several lines reports its layout
			// first; that run is not the text and must not take its place.
			if (only_space(x.text, x.text_len)) {
				continue;
			}
			char *dst = NULL;
			size_t size = 0;
			if (want == WANT_TITLE) {
				dst = it.title;
				size = sizeof(it.title);
			} else if (want == WANT_GUID) {
				dst = it.guid;
				size = sizeof(it.guid);
			} else if (want == WANT_DATE) {
				dst = it.date;
				size = sizeof(it.date);
			} else if (want == WANT_DURATION) {
				dst = it.duration;
				size = sizeof(it.duration);
			}
			want = WANT_NONE;
			if (dst) {
				copy_unescaped(dst, size, x.text, x.text_len);
				trim_in_place(dst);
			}
			continue;
		}

		if (x.event == EPUB_XML_CLOSE) {
			if (!epub_xml_is(&x, "item")) {
				continue;
			}
			in_item = false;
			want = WANT_NONE;
			if (!each(&it, seen, user)) {
				break; // the caller has what it came for
			}
			continue;
		}

		// An element opening.
		if (epub_xml_is(&x, "item")) {
			in_item = true;
			want = WANT_NONE;
			item_reset(&it);
			seen++;
			continue;
		}
		if (!in_item) {
			continue; // the channel's own title, description and artwork
		}

		if (epub_xml_is(&x, "title")) {
			want = WANT_TITLE;
		} else if (epub_xml_is(&x, "guid")) {
			want = WANT_GUID;
		} else if (epub_xml_is(&x, "pubDate")) {
			want = WANT_DATE;
		} else if (epub_xml_is(&x, "duration")) {
			want = WANT_DURATION; // itunes:duration, with the prefix stripped
		} else if (epub_xml_is(&x, "enclosure")) {
			const char *value = NULL;
			size_t len = 0;
			if (epub_xml_attr(&x, "url", &value, &len)) {
				copy_unescaped(it.enclosure, sizeof(it.enclosure), value, len);
			}
			if (epub_xml_attr(&x, "type", &value, &len)) {
				copy_unescaped(it.mime, sizeof(it.mime), value, len);
			}
		} else if (epub_xml_is(&x, "image")) {
			const char *value = NULL;
			size_t len = 0;
			// itunes:image carries the address in an attribute, and there is
			// no plain <image> of any use inside an item.
			if (epub_xml_attr(&x, "href", &value, &len)) {
				copy_unescaped(it.image, sizeof(it.image), value, len);
			}
		}

		// <title/> with nothing in it is its own close as well, and leaving
		// the want standing would put the next run of text in it.
		if (x.self_closing) {
			want = WANT_NONE;
		}
	}

	// A feed with no items in it is not a shorter list of episodes, it is a
	// document this could not read: an HTML error page, a redirect, a feed
	// behind a login. The caller keeps the directory's list on that answer.
	if (items_seen) {
		*items_seen = seen;
	}
	return seen;
}

typedef struct {
	long long feed_id;
	const char *feed_title;
	const podcastrss_known_t *known;
	int known_count;
	podcast_episode_t *out;
	int max;
	int count;
} rss_list_t;

static bool list_keep(const rss_item_t *it, int index, void *user) {
	(void)index;
	rss_list_t *list = user;
	if (list->count >= list->max) {
		return false; // the list is full; stop reading the feed
	}
	if (!item_episode(it, list->feed_id, list->feed_title, list->known, list->known_count, &list->out[list->count])) {
		return true; // not an episode; keep reading
	}
	list->count++;
	return true;
}

int podcastrss_episodes(const char *body, long long feed_id, const char *feed_title,
						const podcastrss_known_t *known, int known_count, podcast_episode_t *out, int max,
						int *items_seen) {
	if (items_seen) {
		*items_seen = 0;
	}
	if (!body || !out || max <= 0) {
		return -1;
	}

	rss_list_t list = {.feed_id = feed_id,
					   .feed_title = feed_title,
					   .known = known,
					   .known_count = known ? known_count : 0,
					   .out = out,
					   .max = max,
					   .count = 0};
	int seen = feed_walk(body, list_keep, &list, items_seen);
	if (seen == 0) {
		return -1;
	}
	return list.count;
}

typedef struct {
	long long feed_id;
	const char *feed_title;
	long long want;
	podcast_episode_t *out;
	bool found;
} rss_find_t;

static bool find_match(const rss_item_t *it, int index, void *user) {
	(void)index;
	rss_find_t *find = user;
	// Decided on the guid alone, before the item is turned into an episode:
	// an item with no audio is still the item that was asked for, and saying
	// so is more useful than reading on past it.
	if (podcastrss_guid_id(it->guid) != find->want) {
		return true;
	}
	find->found = item_episode(it, find->feed_id, find->feed_title, NULL, 0, find->out);
	return false;
}

bool podcastrss_find(const char *body, long long feed_id, const char *feed_title, long long episode_id,
					 podcast_episode_t *out) {
	if (!body || !out || episode_id <= 0) {
		return false;
	}
	rss_find_t find = {.feed_id = feed_id, .feed_title = feed_title, .want = episode_id, .out = out, .found = false};
	feed_walk(body, find_match, &find, NULL);
	return find.found;
}
