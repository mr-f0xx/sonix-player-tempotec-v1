#ifndef PODCASTCATCHER_INTERNAL_H
#define PODCASTCATCHER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/podcast.h"

// The two halves of the catcher that are worth testing on their own: reading
// feeds.txt, and deciding which of a feed's episodes are new. The worker that
// talks to the network sits on top of them (podcastcatcherworker.c); a test
// links these and nothing else.

#define PODCASTCATCHER_FEEDS_FILE "feeds.txt"
#define PODCASTCATCHER_MAX_FEEDS 64
#define PODCASTCATCHER_MAX_EPISODES 64
#define PODCASTCATCHER_DEFAULT_MAX_NEW 3
#define PODCASTCATCHER_MAX_ATTEMPTS 3

typedef struct {
	char url[PODCAST_URL_MAX];
} podcastcatcher_feed_t;

// The addresses in feeds.txt, in the order the file lists them. Blank lines,
// lines starting with # or ;, and lines that are not an http(s) address are
// skipped. Returns how many were read, which is `max` if the file had more --
// the rest are ignored rather than read past the array.
int podcastcatcher_parse_feeds(const char *text, podcastcatcher_feed_t *out, int max);

// What the catcher remembers between checks, on the card:
// <card>/.local/podcast-catcher.db. One row per feed it has seen and one per
// episode, so a feed read a second time can tell a new episode from an old one
// and a failed download from one still to do.
typedef struct podcastcatcher_state podcastcatcher_state_t;

podcastcatcher_state_t *podcastcatcher_state_open(const char *sd_root);
void podcastcatcher_state_close(podcastcatcher_state_t *st);

// Records the feed, and says whether this is the first time it was seen: the
// first check is the one that has to decide how much of a long feed to take.
bool podcastcatcher_state_feed(podcastcatcher_state_t *st, const char *url, const char *title,
							   const char *author, const char *image, bool *first_out);

// Which episodes to download now. `episodes` is in feed order, newest first;
// the first `limit` of them that are new -- or that failed and have attempts
// left -- are marked queued and their indexes written to `indexes`, and the
// count is returned. Every other episode is left exactly as it was.
//
// On the feed's first check the episodes past the limit are recorded as seen
// but not wanted, so an old episode a later check sees again is not mistaken
// for a new one. On every later check they are left alone, so a feed that
// publishes more than `limit` at once still has the rest taken next time.
int podcastcatcher_state_take_new(podcastcatcher_state_t *st, const char *feed_url, bool first,
								  const podcast_episode_t *episodes, int count, int limit, int *indexes);

void podcastcatcher_state_downloaded(podcastcatcher_state_t *st, const char *feed_url, long long episode_id);
void podcastcatcher_state_failed(podcastcatcher_state_t *st, const char *feed_url, long long episode_id);

// Forgets every feed that is no longer in feeds.txt, and its episodes with it.
void podcastcatcher_state_prune(podcastcatcher_state_t *st, const podcastcatcher_feed_t *feeds, int count);

#endif /* PODCASTCATCHER_INTERNAL_H */
