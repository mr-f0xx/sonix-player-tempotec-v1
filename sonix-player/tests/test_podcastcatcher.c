// test_podcastcatcher.c -- the offline catcher's decisions, with no network.
//
// Two halves are worth testing on their own. The reader of feeds.txt has to
// survive comments, blank lines and lines that are not addresses -- the file
// is written by hand on a computer and copied to the card, so it will contain
// all three. The new-episode decision is the one that can fill a card: a feed
// added with years of back catalogue must contribute its newest few episodes
// and never offer the rest again, while an episode that is genuinely new must
// still be taken, and one whose download failed must be tried again without
// being tried for ever.
//
// The state is the real SQLite, on a temporary card. Nothing else is linked:
// the worker that fetches the feed sits on top of these two and is not what is
// under test here.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/db/sqlite3.h"
#include "src/system/streaming/podcastcatcher_internal.h"

static int failures;

#define CHECK(cond)                                                                             \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			failures++;                                                                         \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
		}                                                                                       \
	} while (0)

static char card[512];

static podcast_episode_t episode(long long id, const char *title) {
	podcast_episode_t e;
	memset(&e, 0, sizeof(e));
	e.id = id;
	e.feed_id = 0;
	snprintf(e.title, sizeof(e.title), "%s", title);
	e.published = 1700000000L + (long)id;
	return e;
}

// A database that exists, with the two tables the catcher needs: the schema is
// made on open, which is the only way the player ever gets one.
static bool db_exists(void) {
	char path[600];
	snprintf(path, sizeof(path), "%s/.local/podcast-catcher.db", card);
	struct stat st;
	return stat(path, &st) == 0;
}

static void test_parse_feeds(void) {
	const char *text = "# a comment\n"
					   "\n"
					   "  https://example.test/a.rss  \r\n"
					   "; a semicolon comment\n"
					   "not an address\n"
					   "http://example.test/b.xml\n";
	podcastcatcher_feed_t feeds[4];
	int n = podcastcatcher_parse_feeds(text, feeds, 4);
	CHECK(n == 2);
	CHECK(strcmp(feeds[0].url, "https://example.test/a.rss") == 0);
	CHECK(strcmp(feeds[1].url, "http://example.test/b.xml") == 0);

	// More lines than the array has room for: the array is filled and the
	// rest ignored, not read past the end.
	char many[64 * 40];
	size_t used = 0;
	for (int i = 0; i < 70; i++) {
		used += (size_t)snprintf(many + used, sizeof(many) - used, "https://example.test/%d.rss\n", i);
	}
	n = podcastcatcher_parse_feeds(many, feeds, 4);
	CHECK(n == 4);
	CHECK(strcmp(feeds[0].url, "https://example.test/0.rss") == 0);
	CHECK(strcmp(feeds[3].url, "https://example.test/3.rss") == 0);

	CHECK(podcastcatcher_parse_feeds("", feeds, 4) == 0);
	CHECK(podcastcatcher_parse_feeds("# only a comment\n", feeds, 4) == 0);

	// A file written in Notepad carries a byte-order mark, and the first
	// address is still the first address.
	n = podcastcatcher_parse_feeds("\xEF\xBB\xBFhttps://example.test/first.rss\n", feeds, 4);
	CHECK(n == 1);
	CHECK(strcmp(feeds[0].url, "https://example.test/first.rss") == 0);
}

static void test_state(void) {
	const char *A = "https://example.test/a.rss";
	const char *B = "https://example.test/b.rss";

	podcastcatcher_state_t *st = podcastcatcher_state_open(card);
	CHECK(st != NULL);
	CHECK(db_exists());

	bool first = false;
	CHECK(podcastcatcher_state_feed(st, A, "Show A", "Author", "https://example.test/a.jpg", &first));
	CHECK(first);
	CHECK(podcastcatcher_state_feed(st, A, "Show A", "Author", "https://example.test/a.jpg", &first));
	CHECK(!first);

	podcast_episode_t eps[5];
	for (int i = 0; i < 5; i++) {
		eps[i] = episode(i + 1, "Episode");
	}
	int indexes[5];

	// The first check of a five-episode feed with a limit of two: the newest
	// two are taken, and the other three are remembered as seen.
	int taken = podcastcatcher_state_take_new(st, A, true, eps, 5, 2, indexes);
	CHECK(taken == 2);
	CHECK(indexes[0] == 0 && indexes[1] == 1);

	// An episode taken but not downloaded -- the player was switched off
	// before the file landed -- is taken again on the next check. That is the
	// crash-recovery half of the same rule.
	CHECK(podcastcatcher_state_take_new(st, A, false, eps, 5, 2, indexes) == 2);

	// What was taken is on the card once it downloads, and never offered again.
	podcastcatcher_state_downloaded(st, A, 1);
	podcastcatcher_state_downloaded(st, A, 2);
	CHECK(podcastcatcher_state_take_new(st, A, false, eps, 5, 2, indexes) == 0);

	// A genuinely new episode at the front of the feed is new.
	podcast_episode_t eps6[6];
	eps6[0] = episode(6, "New one");
	for (int i = 0; i < 5; i++) {
		eps6[i + 1] = eps[i];
	}
	taken = podcastcatcher_state_take_new(st, A, false, eps6, 6, 2, indexes);
	CHECK(taken == 1);
	CHECK(indexes[0] == 0);
	podcastcatcher_state_downloaded(st, A, 6);

	// A failed download is tried again on the next check, and stops being
	// tried after three failures.
	podcast_episode_t eps7[7];
	eps7[0] = episode(7, "Keeps failing");
	for (int i = 0; i < 6; i++) {
		eps7[i + 1] = eps6[i];
	}
	CHECK(podcastcatcher_state_take_new(st, A, false, eps7, 7, 1, indexes) == 1);
	CHECK(indexes[0] == 0);
	podcastcatcher_state_failed(st, A, 7);
	CHECK(podcastcatcher_state_take_new(st, A, false, eps7, 7, 1, indexes) == 1);
	podcastcatcher_state_failed(st, A, 7);
	CHECK(podcastcatcher_state_take_new(st, A, false, eps7, 7, 1, indexes) == 1);
	podcastcatcher_state_failed(st, A, 7);
	CHECK(podcastcatcher_state_take_new(st, A, false, eps7, 7, 1, indexes) == 0);

	// A feed that leaves feeds.txt leaves the database with it, and its
	// episodes are not remembered if it comes back.
	CHECK(podcastcatcher_state_feed(st, B, "Show B", "", "", &first));
	CHECK(first);
	podcastcatcher_feed_t kept[1];
	snprintf(kept[0].url, sizeof(kept[0].url), "%s", A);
	podcastcatcher_state_prune(st, kept, 1);
	CHECK(podcastcatcher_state_feed(st, B, "Show B", "", "", &first));
	CHECK(first);

	// The rows of a feed that was pruned are gone with it: the episode the
	// first check recorded for B is new again if B ever comes back, because
	// there is nothing left to remember it by.
	podcast_episode_t one = episode(70, "B's episode");
	CHECK(podcastcatcher_state_take_new(st, B, true, &one, 1, 1, indexes) == 1);
	CHECK(indexes[0] == 0);

	podcastcatcher_state_close(st);
}

int main(void) {
	char template[] = "/tmp/sonix-catcher-XXXXXX";
	char *made = mkdtemp(template);
	if (!made) {
		fprintf(stderr, "FAIL: cannot make a temporary card\n");
		return 1;
	}
	snprintf(card, sizeof(card), "%s", made);

	test_parse_feeds();
	test_state();

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("podcastcatcher: ok\n");
	return 0;
}
