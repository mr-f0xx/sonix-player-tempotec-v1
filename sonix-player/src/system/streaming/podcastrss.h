#ifndef PODCASTRSS_H
#define PODCASTRSS_H

#include "src/system/streaming/podcast.h"

// A podcast's own feed, read for the episodes a directory does not list.
//
// Why this exists beside the two directories: both of them can hand back fewer
// episodes than a show has. Apple's episode lookup stops at two hundred
// whatever it is asked for, which on a show of four hundred is the most recent
// two hundred and no way to the rest -- there is no offset and no larger
// limit. The feed the show publishes is the one source that has all of them,
// and it is the source both directories are built from in the first place.
//
// So it is a fallback and not a replacement, and the caller decides when: a
// feed is worth reading only when the directory's own count says it left
// something out. Reading it always would be a mistake, because a feed is not
// guaranteed to be the fuller source -- a show whose back catalogue moved
// behind its own platform publishes a feed of trailers while the directory
// still indexes the episodes, and swapping one for the other would lose them.
//
// The parsing is src/system/ebook/epub_xml.c, which is the XML this tree has.
// It is a scanner with no tree and no validation, and that suits a feed: the
// wild contains broken XML, dates in five shapes and duplicated tags, and a
// reader that refuses a show over a stray ampersand shows the listener nothing
// at all. Namespace prefixes come off, so `itunes:duration` is `duration`
// here. See podcast.h for what that parser does not do.

// The longest <guid> remembered. Podcast guids run from a bare hex string to
// an art19 locator of some sixty characters; one this long is never truncated,
// and a longer one simply does not match and gets a derived id instead.
#define PODCASTRSS_GUID_MAX 128

// An episode the directory already numbered. Matching a feed item to one of
// these keeps the id the downloads, the sidecars and the heard marks are keyed
// on, so switching the source under a listener does not orphan what they have.
typedef struct {
	char guid[PODCASTRSS_GUID_MAX];
	long long id;
} podcastrss_known_t;

// Reads `body` as a feed and fills `out` with up to `max` episodes, in the
// order the feed lists them -- newest first, which is the convention and what
// the directories answer with too.
//
// `feed_id` and `feed_title` are stamped onto every episode: a feed says which
// show it belongs to once, in the channel, and the player wants it per row.
//
// An episode with no audio is not counted, and one with no title is not
// either: both would be a row that does nothing when touched.
//
// `items_seen`, when not NULL, receives how many <item> elements were walked:
// every one of them if the feed ran out first, or the ones up to a full `out`
// if it did not. Either way it is the caller's measure of how much feed there
// was, and a 0 means the document was not a feed at all.
//
// Returns how many it put in `out`, or -1 when `body` has no <item> in it -- an
// error page, a redirect, a feed behind a login -- which is the caller's cue to
// keep the directory's list.
int podcastrss_episodes(const char *body, long long feed_id, const char *feed_title,
						const podcastrss_known_t *known, int known_count, podcast_episode_t *out, int max,
						int *items_seen);

// The three readings worth testing on their own, because each has more shapes
// in the wild than the code that calls them expects.

// "Thu, 05 Nov 2026 15:45:00 GMT" -> unix time. Also a date with no weekday,
// no time, or a numeric zone, and ISO 8601, which some feeds use. 0 when it
// cannot be read, which is what "unknown" already is.
long podcastrss_pubdate(const char *text);

// "00:40:36", "23:36" or "1416" -> seconds. 0 when it cannot be read.
int podcastrss_duration_secs(const char *text);

// A stable non-zero id for an episode the directory does not number, derived
// from its guid: the same guid always gives the same id, so a downloaded
// episode is found again on the next list and is not downloaded twice.
long long podcastrss_guid_id(const char *guid);

// Finds one episode in a feed by the derived id podcastrss_guid_id() gave it,
// and fills `out`. Walking rather than listing, so it costs no array: a show
// with four hundred episodes is not worth six hundred kilobytes to find one
// row of it.
//
// Only derived ids are matched, and that is enough: an episode a directory did
// number keeps that number in a list, and is found there before a feed is ever
// read. What reaches this is an episode no directory listed, which is exactly
// the kind whose id came from its guid.
bool podcastrss_find(const char *body, long long feed_id, const char *feed_title, long long episode_id,
					 podcast_episode_t *out);

#endif /* PODCASTRSS_H */
