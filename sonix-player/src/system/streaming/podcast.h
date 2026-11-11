#ifndef PODCAST_H
#define PODCAST_H

#include <stdbool.h>
#include <stddef.h>

// The catalogue behind the Podcasts section, of which there are two to choose
// from: Podcast Index and Apple's. podcast.c says which, and why; the choice
// is [podcast] api_base, and with none configured the streaming keys decide --
// a pair means Podcast Index, no pair means Apple, which needs none. Nothing
// past this line differs between them, which is the whole point: the pages,
// the cache and the downloader see one interface either way.
//
// Why a catalogue at all, and not the RSS feeds directly: a podcast is an RSS
// feed, and in theory downloading it would be enough. In practice it is not,
// for two reasons:
//
//   * search. Without a catalogue, the only way to find a podcast is to know
//     its URL by heart and type it on an on-screen keyboard. Nobody does that.
//   * the format. Podcast RSS is XML with four namespaces on it (itunes:,
//     podcast:, media:, content:), and the wild contains everything -- feeds
//     with broken XML, dates in five formats, duplicated tags. The one XML
//     parser this tree has is src/system/ebook/epub_xml.c, and it reads the
//     XHTML and OPF of an ebook that was validated on the way in: it has no
//     notion of namespaces, no entities beyond the five XML ones and no
//     recovery from a malformed document, so pointing it at podcast feeds
//     would mean teaching it all three before it could read one. Both
//     catalogues here do that job already and answer with normalized JSON,
//     for which a parser is here.
//
// The signature, on the Podcast Index side only: every request there carries
// three headers -- the key, the time, and the SHA-1 of key+secret+time in hex.
// The time must be the real one to within a few minutes, and this device loses
// the clock at every power-off and gets it back from NTP at boot, so without
// network the time is wrong and the catalogue answers 401. See
// podcast_last_error(), which recognizes that case and says so plainly instead
// of leaving "401". Apple's requests carry nothing but a User-Agent, so the
// clock cannot make them fail.
//
// Everything in here blocks. Call only from the page's worker thread, never
// from the UI thread.

#define PODCAST_TITLE_MAX 160
#define PODCAST_AUTHOR_MAX 120
#define PODCAST_URL_MAX 512
#define PODCAST_PAGE_LIMIT 60

// The ids are 64-bit, and not out of generosity: on this device `long` is
// 32-bit, and Podcast Index numbers episodes with eleven digits (16795090626
// and the like) and Apple with thirteen (1000800977857). With a 32-bit `long`
// strtol does not get such a number wrong, it saturates -- every episode
// collapses onto 2147483647, sharing one cache file and one sidecar row. Feed
// ids are small and would fit, but they travel with episode ids in too many
// places to be worth distinguishing.

// A podcast: the feed, not an episode.
typedef struct {
	long long id; // the catalogue's own id, which is how it is found again
	char title[PODCAST_TITLE_MAX];
	char author[PODCAST_AUTHOR_MAX];
	char image[PODCAST_URL_MAX];
	int episode_count; // 0 when the catalogue does not say
} podcast_feed_t;

typedef struct {
	long long id;
	long long feed_id;
	char title[PODCAST_TITLE_MAX];
	char feed_title[PODCAST_TITLE_MAX];
	char image[PODCAST_URL_MAX];
	char enclosure[PODCAST_URL_MAX]; // the audio itself
	char mime[64];					 // the declared type, for the extension
	long published;					 // unix time, 0 when unknown
	int duration_secs;				 // 0 when unknown
} podcast_episode_t;

// Whether a key pair is present. Without one the section disables itself and
// says so.
bool podcast_configured(void);

// Why the last call failed, already translated for display.
const char *podcast_last_error(void);

// How many entries the catalogue sent in the last response, before the
// unusable ones were discarded: this is the number that says whether the
// response filled the requested limit, and hence whether asking for more is
// worthwhile.
int podcast_last_raw_count(void);

// Searches podcasts by name. Returns how many it put in `out`, -1 on error.
int podcast_search(const char *query, podcast_feed_t *out, int max);

// The current chart.
int podcast_trending(podcast_feed_t *out, int max);

// The language the chart is asked for: an ISO 639 code, or "" for the
// interface's own. This is what "trending where I am" means here: the chart of
// the language somebody chose to read the player in, or another one
// deliberately, for a listener who wants the podcasts of a place they are not.
//
// The choice is one row of the shared region list, which carries a language
// and a country and never one without the other, and each catalogue asks for
// the half it understands: Podcast Index filters its chart by language and has
// no country filter at all, while Apple's chart is a storefront's and takes
// the country. Same row, same picker, so the answer is the same place either
// way -- which is why this stays a language even when the request carries the
// country beside it.
const char *podcast_trending_lang(void);
void podcast_set_trending_lang(const char *code);

// The language the chart is really asked for: the chosen one, or the
// interface's own when none was chosen.
const char *podcast_trending_lang_effective(void);

// The name the chart is kept under on the card -- one per language.
const char *podcast_trending_cache_key(void);

// A podcast's episodes, most recent first.
int podcast_episodes(long long feed_id, podcast_episode_t *out, int max);

// A single podcast, by id. Needed by "My podcasts", which stores ids rather
// than titles, so a podcast that is renamed stays the right one.
bool podcast_feed(long long feed_id, podcast_feed_t *out);

// A single episode. Needed by the player, which has the id from the sidecar and
// wants to know which podcast it belongs to.
bool podcast_episode(long long episode_id, podcast_episode_t *out);

// ---------------------------------------------------------------------------
// The two skip buttons, which on podcasts are their own and not the audiobook
// ones.
//
// The two cases look alike -- back because a sentence was missed, forward to
// jump an advert -- but are not listened to the same way: in an audiobook the
// back skip picks up the thread of a story, in a podcast the forward skip jumps
// a half-minute sponsor. Wanting -10/+30 on podcasts and -30/+10 on books
// should not be a choice: they are two settings, in two different sections of
// the configuration file.
#define PODCAST_SKIP_SHORT 10
#define PODCAST_SKIP_LONG 30
#define PODCAST_SKIP_HUGE 60

int podcast_skip_back(void);
int podcast_skip_forward(void);
void podcast_set_skip_back(int seconds);
void podcast_set_skip_forward(int seconds);

// Playback speed for episodes, in thousandths (1000 is normal), apart from the
// audiobook one in audiobook.h. Same range, [podcast] speed in the config.
int podcast_speed_permille(void);
void podcast_set_speed_permille(int permille);
double podcast_speed(void);

// Stop at the end of the episode: when this one finishes, the queue stays where
// it is instead of starting the next. The audiobook's chapter-end switch under
// another name, and for the same listener -- somebody following one thing at a
// time, who does not want the next hour starting on its own.
bool podcast_stop_at_episode_end(void);
void podcast_set_stop_at_episode_end(bool on);

#endif /* PODCAST_H */
