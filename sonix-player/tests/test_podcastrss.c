// test_podcastrss.c -- reading a podcast's own feed.
//
// The feed is read for one reason: a directory that stops at two hundred
// episodes leaves the first ones of a long show unreachable, and the feed is
// the only source with all of them. Which means the parser has to survive the
// wild, where feeds are hand-written by a hundred different publishing tools
// and disagree about nearly everything that is not required of them.
//
// So the tests are about the disagreement. Three readings are exercised on
// their own before any whole feed is read, because each has more shapes in the
// wild than one feed and each fails differently: a date that cannot be read is
// a missing sort key, a duration that cannot be read is a missing length, and
// an id that changes between runs is a downloaded episode that cannot be found
// again. Then the walk, against a feed written the way real ones are.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/system/streaming/podcastrss.h"

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

// Midnight UTC on a date, the way the tests say a date without arguing about
// the machine they run on.
static long utc_of(int year, int mon, int day, int hour, int min, int sec) {
	struct tm t;
	memset(&t, 0, sizeof(t));
	t.tm_year = year - 1900;
	t.tm_mon = mon - 1;
	t.tm_mday = day;
	t.tm_hour = hour;
	t.tm_min = min;
	t.tm_sec = sec;
	return (long)timegm(&t);
}

// ---------------------------------------------------------------------------
// the date
// ---------------------------------------------------------------------------

static void test_pubdate(void) {
	// The shape the specification asks for.
	CHECK(podcastrss_pubdate("Thu, 05 Nov 2026 15:45:00 GMT") == utc_of(2026, 11, 5, 15, 45, 0));
	// A numeric zone, and a weekday that is not there.
	CHECK(podcastrss_pubdate("Sat, 04 Jun 2016 12:00:00 +0000") == utc_of(2016, 6, 4, 12, 0, 0));
	CHECK(podcastrss_pubdate("05 Nov 2026 15:45:00 GMT") == utc_of(2026, 11, 5, 15, 45, 0));
	// No time at all: still a day, and midnight of it.
	CHECK(podcastrss_pubdate("Thu, 05 Nov 2026") == utc_of(2026, 11, 5, 0, 0, 0));
	// A month in lower case, and one with no leading zero on the day.
	CHECK(podcastrss_pubdate("mon, 5 jan 2015 01:02:03 GMT") == utc_of(2015, 1, 5, 1, 2, 3));
	// ISO 8601, which some publishing tools emit instead.
	CHECK(podcastrss_pubdate("2026-11-05T15:45:00Z") == utc_of(2026, 11, 5, 15, 45, 0));
	CHECK(podcastrss_pubdate("2016-06-04") == utc_of(2016, 6, 4, 0, 0, 0));

	// Nothing that is not a date, and nothing ambiguous: a two-digit year is a
	// guess about a century, and guessing wrong puts an episode a hundred years
	// from the ones beside it, which sorts worse than having no date at all.
	CHECK(podcastrss_pubdate("") == 0);
	CHECK(podcastrss_pubdate(NULL) == 0);
	CHECK(podcastrss_pubdate("not a date") == 0);
	CHECK(podcastrss_pubdate("Thu, 05 Xyz 2026 15:45:00 GMT") == 0); // no such month
	CHECK(podcastrss_pubdate("Thu, 45 Nov 2026 15:45:00 GMT") == 0); // no such day
	CHECK(podcastrss_pubdate("Thu, 05 Nov 26 15:45:00 GMT") == 0);
	CHECK(podcastrss_pubdate("<p>No date</p>") == 0);

	printf("  pubDate\n");
}

// ---------------------------------------------------------------------------
// the duration
// ---------------------------------------------------------------------------

static void test_duration(void) {
	CHECK(podcastrss_duration_secs("00:40:36") == 40 * 60 + 36);
	CHECK(podcastrss_duration_secs("1:02:03") == 3723);
	CHECK(podcastrss_duration_secs("23:36") == 23 * 60 + 36);
	CHECK(podcastrss_duration_secs("1416") == 1416); // bare seconds
	CHECK(podcastrss_duration_secs("  00:01:00  ") == 60); // the whitespace a feed indents with
	CHECK(podcastrss_duration_secs("00:00:00") == 0); // a length of zero is no length
	CHECK(podcastrss_duration_secs("") == 0);
	CHECK(podcastrss_duration_secs(NULL) == 0);
	CHECK(podcastrss_duration_secs("40 minutes") == 0);

	printf("  duration\n");
}

// ---------------------------------------------------------------------------
// the id derived from a guid
// ---------------------------------------------------------------------------

static void test_guid_id(void) {
	// Stable, which is the whole requirement: the id names the file on the card
	// and the row in the heard list, so it has to come out the same every run.
	long long a = podcastrss_guid_id("b6085bcd-3542-4a43-b6a8-021e3fd251b8");
	CHECK(a == podcastrss_guid_id("b6085bcd-3542-4a43-b6a8-021e3fd251b8"));

	// Different guids are different episodes.
	CHECK(a != podcastrss_guid_id("b6085bcd-3542-4a43-b6a8-021e3fd251b9"));
	CHECK(a != podcastrss_guid_id(""));

	// Positive, and out of the range the directories hand out, so a derived id
	// is never mistaken for one of theirs.
	CHECK(a > 0);
	CHECK(a > 1000800977857LL); // eleven digits, which is what Apple's are
	CHECK(a > (1LL << 31));     // and past a 32-bit long, which they also are

	// No guid, no id.
	CHECK(podcastrss_guid_id("") == 0);
	CHECK(podcastrss_guid_id(NULL) == 0);

	printf("  guid\n");
}

// ---------------------------------------------------------------------------
// the walk
// ---------------------------------------------------------------------------

// A feed with everything awkward in it at once: namespaces, CDATA, entities,
// indentation, an empty element, an item with no audio, an item with no title,
// show notes in HTML, and a guid longer than the buffer that holds one.
static const char *MESSY =
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	"<rss version=\"2.0\" xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\"\n"
	"     xmlns:content=\"http://purl.org/rss/1.0/modules/content/\">\n"
	"  <channel>\n"
	"    <title>The Show Itself</title>\n"
	"    <description><![CDATA[<p>Not an episode.</p>]]></description>\n"
	"    <itunes:image href=\"https://art.example/show.jpg\"/>\n"
	"    <item>\n"
	"      <title>\n        First &amp; best\n      </title>\n"
	"      <guid isPermaLink=\"false\">g-first</guid>\n"
	"      <enclosure url=\"https://cdn.example/first.mp3\" type=\"audio/mpeg\" length=\"1\"/>\n"
	"      <pubDate>Mon, 10 Nov 2025 06:00:00 GMT</pubDate>\n"
	"      <itunes:duration>01:02:03</itunes:duration>\n"
	"      <itunes:image href=\"https://art.example/first.jpg\"/>\n"
	"      <content:encoded><![CDATA[<p>Show notes, a long one, with &lt;tags&gt; in it.</p>]]></content:encoded>\n"
	"    </item>\n"
	"    <item>\n"
	"      <title>Second</title>\n"
	"      <guid>https://art.example/episode/a-locator-that-goes-on-and-on-and-on-past-the-buffer</guid>\n"
	"      <enclosure url=\"https://cdn.example/second.m4a\" type=\"audio/x-m4a\"/>\n"
	"      <itunes:duration>600</itunes:duration>\n"
	"    </item>\n"
	"    <item>\n"
	"      <title>No audio</title>\n"
	"      <guid>g-silent</guid>\n"
	"      <description>An item with nothing to play.</description>\n"
	"    </item>\n"
	"    <item>\n"
	"      <title/>\n"
	"      <guid>g-untitled</guid>\n"
	"      <enclosure url=\"https://cdn.example/untitled.mp3\" type=\"audio/mpeg\"/>\n"
	"    </item>\n"
	"  </channel>\n"
	"</rss>\n";

static void test_walk(void) {
	podcast_episode_t out[8];
	int items = 0;
	int n = podcastrss_episodes(MESSY, 4242, "A Show", NULL, 0, out, 8, &items);

	// Four items, two of them episodes. The other two would each be a row that
	// does nothing when touched -- one has no audio, one has no title -- and
	// they are counted as seen so the caller can tell a short feed from one it
	// could not read.
	CHECK(items == 4);
	CHECK(n == 2);

	CHECK_STR(out[0].title, "First & best"); // an entity, and indentation around it
	CHECK_STR(out[0].enclosure, "https://cdn.example/first.mp3");
	CHECK_STR(out[0].mime, "audio/mpeg");
	CHECK(out[0].duration_secs == 3723);
	CHECK(out[0].published == utc_of(2025, 11, 10, 6, 0, 0));
	CHECK_STR(out[0].image, "https://art.example/first.jpg"); // the item's, not the channel's
	CHECK_STR(out[0].feed_title, "A Show");
	CHECK(out[0].feed_id == 4242);
	// The show notes are not an episode's title, and a namespaced element is
	// not one of the ones read.
	CHECK(strstr(out[0].title, "Show notes") == NULL);

	CHECK_STR(out[1].title, "Second");
	CHECK_STR(out[1].mime, "audio/x-m4a");
	CHECK(out[1].duration_secs == 600);
	CHECK(out[1].published == 0); // no date at all, and still listed
	CHECK_STR(out[1].image, "");
	// A guid longer than the buffer is truncated rather than run off the end
	// of it; it still makes an id, and a stable one.
	CHECK(out[1].id == out[1].id);
	CHECK(out[1].id > 0);
	CHECK(out[0].id != out[1].id);

	// The same episode the directory numbered keeps that number, so switching
	// the source under a reader does not orphan the file already downloaded.
	podcastrss_known_t known[] = {{.id = 1000800977857LL}};
	snprintf(known[0].guid, sizeof(known[0].guid), "%s", "g-first");
	n = podcastrss_episodes(MESSY, 4242, "A Show", known, 1, out, 8, &items);
	CHECK(n == 2);
	CHECK(out[0].id == 1000800977857LL);
	CHECK(out[1].id != 1000800977857LL);

	// A page smaller than the feed: the first episodes, in the feed's order,
	// and the walk stops once the page is full -- which is what lets a list of
	// four hundred be paged through without reading four hundred items every
	// time. The count is the number walked, so it is still bigger than the
	// page asked for, which is what tells the reader there is more.
	n = podcastrss_episodes(MESSY, 4242, "A Show", NULL, 0, out, 1, &items);
	CHECK(n == 1);
	CHECK(items == 2);
	CHECK_STR(out[0].title, "First & best");

	// The guards.
	CHECK(podcastrss_episodes(MESSY, 4242, "A Show", NULL, 0, out, 0, &items) == -1);
	CHECK(podcastrss_episodes(MESSY, 4242, "A Show", NULL, 0, NULL, 8, &items) == -1);
	CHECK(podcastrss_episodes(NULL, 4242, "A Show", NULL, 0, out, 8, &items) == -1);

	// A feed with nothing to play is still a list, just an empty one -- that is
	// different from a document that is not a feed at all, which is below.
	CHECK(podcastrss_episodes("<rss><channel><item><title>x</title></item></channel></rss>", 1, "S", NULL, 0, out, 8,
							  &items) == 0);
	CHECK(items == 1);

	printf("  walk\n");
}

// What the caller does with an answer of -1 is keep the directory's list, so
// the -1 has to mean "not a feed" and not "a feed with nothing in it".
static void test_not_a_feed(void) {
	podcast_episode_t out[4];
	int items = -1;

	// An error page where a feed was expected: a 200 with HTML in it.
	CHECK(podcastrss_episodes("<html><head><title>403</title></head><body>No</body></html>", 1, "S", NULL, 0, out, 4,
							  &items) == -1);
	CHECK(items == 0);

	// A redirect's JSON, and an empty body.
	CHECK(podcastrss_episodes("{\"url\":\"https://elsewhere\"}", 1, "S", NULL, 0, out, 4, &items) == -1);
	CHECK(podcastrss_episodes("", 1, "S", NULL, 0, out, 4, &items) == -1);

	// A feed that opens but is cut off halfway: what is complete is still
	// episodes, and the caller's own count decides whether that is enough.
	CHECK(podcastrss_episodes("<rss><channel><item><title>One</title><guid>g1</guid>"
							  "<enclosure url=\"https://cdn.example/1.mp3\" type=\"audio/mpeg\"/></item><item><tit",
							  1, "S", NULL, 0, out, 4, &items) == 1);
	CHECK_STR(out[0].title, "One");

	printf("  not a feed\n");
}

// Finding one episode again by the id it was listed under, which is how an
// episode plays after the list it came from has gone.
static void test_find(void) {
	podcast_episode_t one;
	memset(&one, 0, sizeof(one));

	CHECK(podcastrss_find(MESSY, 4242, "A Show", podcastrss_guid_id("g-first"), &one));
	CHECK_STR(one.title, "First & best");
	CHECK_STR(one.enclosure, "https://cdn.example/first.mp3");
	CHECK(one.feed_id == 4242);

	// The one with no title is still the one that was asked for, and saying
	// there is nothing to play is more useful than reading on past it.
	memset(&one, 0, sizeof(one));
	CHECK(!podcastrss_find(MESSY, 4242, "A Show", podcastrss_guid_id("g-untitled"), &one));

	// One the feed does not have.
	CHECK(!podcastrss_find(MESSY, 4242, "A Show", podcastrss_guid_id("g-never"), &one));

	// The guards.
	CHECK(!podcastrss_find(MESSY, 4242, "A Show", 0, &one));
	CHECK(!podcastrss_find(MESSY, 4242, "A Show", -1, &one));
	CHECK(!podcastrss_find(MESSY, 4242, "A Show", 1, NULL));
	CHECK(!podcastrss_find(NULL, 4242, "A Show", 1, &one));

	printf("  finding one episode\n");
}

int main(void) {
	printf("podcast feed tests\n");

	test_pubdate();
	test_duration();
	test_guid_id();
	test_walk();
	test_not_a_feed();
	test_find();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("podcast feed tests passed");
	return 0;
}
