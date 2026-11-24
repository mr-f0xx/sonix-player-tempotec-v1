#ifndef PODCASTCATCHER_H
#define PODCASTCATCHER_H

#include <stdbool.h>

// The offline podcast catcher: the player's own RSS reader, for somebody who
// knows which feeds they follow and does not need a catalogue to find them.
//
// The list is `feeds.txt` on the card, one feed address a line:
//
//     # One feed per line. Blank lines and lines starting with # are ignored.
//     https://feeds.example.com/show.rss
//     https://example.org/other.xml
//
// Whenever Wi-Fi joins a network -- and once at start-up if it is already on --
// every feed is read, the episodes that are new are downloaded into the same
// Podcast folder the Podcasts section downloads into, and each episode is
// bookmarked as it is listened to, so opening it again carries on where it was
// left. Nothing is streamed: once an episode is on the card it plays with the
// radio off.
//
// It is an opt-in module under Settings -> Plugins (`podcastcatcher`), and it
// needs the Podcasts module too, because that is the page the downloaded
// episodes appear on. With the module off nothing here runs: no thread is
// started and no feed is read.
//
// The configuration is in [podcast_catcher] of device_config.ini:
//
//     [podcast_catcher]
//     feeds_file = feeds.txt      ; on the card, relative to its root
//     max_episodes = 3            ; newest episodes taken per feed, per check
//     auto_bookmark = true        ; remember where each episode was left
//     min_free_mb = 200           ; stop rather than fill the card
//
// The first check of a feed takes its newest `max_episodes` and records the
// rest as seen, so adding a feed with a thousand episodes does not fill the
// card. A later check takes only what is genuinely new, and retries a failed
// episode on the next connection up to three times.

// Starts the background watcher. Safe to call more than once; the second call
// does nothing. It checks the module switch itself, so the caller does not
// have to -- a caller that starts it unconditionally is not a bug.
void podcastcatcher_start(void);

// Asks the watcher to stop, for the card being released or the player going
// off. Returns at once; the thread notices on its next poll.
void podcastcatcher_stop(void);

// True when the module is switched on and the Podcasts module it depends on is
// on too. The watcher, the bookmark store and the tests all ask this one
// question rather than reading two config keys each.
bool podcastcatcher_enabled(void);

// Asks for a check on the next poll, whether or not Wi-Fi changed state. For a
// card that arrived while the radio was already up: the new card's feeds.txt
// would otherwise wait for the next disconnect and reconnect.
void podcastcatcher_card_changed(void);

// What the catcher last did, for the log and for anyone reading the device's
// console. A single line, never NULL.
const char *podcastcatcher_last_message(void);

#endif /* PODCASTCATCHER_H */
