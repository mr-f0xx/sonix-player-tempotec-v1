#include "podcastcatcher.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/shell/gui.h"
#include "src/system/addons/addons.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/podcastcatcher_internal.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/podcastrss.h"

// The watcher: Wi-Fi's state is polled, because that is how this player's
// radio works -- see wifi.h, which has no callbacks on purpose. A transition
// into CONNECTED is the trigger, and a card arriving while the radio is
// already up is the other one (podcastcatcher_card_changed).
//
// The thread is started only when the module is on, so an install that never
// switches it on pays nothing: no thread, no poll, no feed read.

// A feed is small, but a show that publishes whole transcripts in its items
// is not. Eight megabytes is far past the largest feed this should ever meet
// and still cuts off anything that is not one.
#define CATCHER_BODY_MAX (8 * 1024 * 1024)
#define CATCHER_TIMEOUT_SECS 30

// Two checks closer together than this are one check: a radio that flaps
// between two access points must not read every feed on every flap.
#define CATCHER_COOLDOWN_SECS 60

static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static bool started;
static pthread_t worker;
static volatile bool stop_asked;
static volatile bool check_requested;

static pthread_mutex_t message_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_message[256] = "idle";

static void set_message(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	pthread_mutex_lock(&message_lock);
	vsnprintf(last_message, sizeof(last_message), fmt, args);
	pthread_mutex_unlock(&message_lock);
	va_end(args);
}

const char *podcastcatcher_last_message(void) {
	// One static answer rather than a copy the caller has to free: this is
	// read for the console and by tests, never held.
	static char copy[sizeof(last_message)];
	pthread_mutex_lock(&message_lock);
	memcpy(copy, last_message, sizeof(copy));
	pthread_mutex_unlock(&message_lock);
	return copy;
}

bool podcastcatcher_enabled(void) {
	return addons_enabled("podcastcatcher") && addons_enabled("podcasts");
}

void podcastcatcher_card_changed(void) {
	if (started) {
		check_requested = true;
	}
}

// Sleeps in short pieces so stop_asked is noticed at once. The card being
// released must not wait out a whole poll period for the catcher to let go.
static void sleep_ms(int ms) {
	for (int slept = 0; slept < ms && !stop_asked; slept += 200) {
		usleep(200 * 1000);
	}
}

static long config_clamped(const char *key, long fallback, long low, long high) {
	long value = config_get_int("podcast_catcher", key, fallback);
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

// True when the card still has `min_free_mb` to spare. A statvfs that fails
// says nothing, and a catcher that refuses to download because it could not
// read the card would be worse than one that tries.
static bool card_has_room(const char *root, long min_free_mb) {
	struct statvfs vfs;
	if (statvfs(root, &vfs) != 0) {
		return true;
	}
	unsigned long long free = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_bsize;
	return free >= (unsigned long long)min_free_mb * 1024ULL * 1024ULL;
}

// The path of feeds.txt. A value with a leading slash is used as it is, so a
// card mounted somewhere unusual can be pointed at directly; anything else is
// relative to the card's root, which is what the feature promises.
static void feeds_path(const char *root, char *out, size_t size) {
	const char *name = config_get("podcast_catcher", "feeds_file", PODCASTCATCHER_FEEDS_FILE);
	if (name && name[0] == '/') {
		snprintf(out, size, "%s", name);
	} else {
		snprintf(out, size, "%.480s/%s", root, name && name[0] ? name : PODCASTCATCHER_FEEDS_FILE);
	}
}

static void notify_new_episodes(int count) {
	// gui_notify_popup() is safe from here: it posts to the interface thread
	// rather than touching LVGL on this one (see gui.h).
	if (count == 1) {
		gui_notify_popup("New podcast episode downloaded");
	} else if (count > 1) {
		char text[64];
		snprintf(text, sizeof(text), "%d new podcast episodes downloaded", count);
		gui_notify_popup(text);
	}
}

static int check_feed(podcastcatcher_state_t *st, const podcastcatcher_feed_t *feed, int max_new, long min_free_mb,
					  const char *root) {
	char *body = NULL;
	size_t len = 0;
	if (!http_get(feed->url, &body, &len, CATCHER_BODY_MAX, CATCHER_TIMEOUT_SECS)) {
		set_message("feed failed: %s", feed->url);
		fprintf(stderr, "podcastcatcher: %s does not download\n", feed->url);
		return 0;
	}

	char title[PODCAST_TITLE_MAX] = "";
	char author[PODCAST_AUTHOR_MAX] = "";
	char image[PODCAST_URL_MAX] = "";
	if (!podcastrss_channel(body, title, sizeof(title), author, sizeof(author), image, sizeof(image))) {
		set_message("not a feed: %s", feed->url);
		fprintf(stderr, "podcastcatcher: %s has no channel title\n", feed->url);
		free(body);
		return 0;
	}

	bool first = false;
	podcastcatcher_state_feed(st, feed->url, title, author, image, &first);

	podcast_episode_t episodes[PODCASTCATCHER_MAX_EPISODES];
	int count = podcastrss_episodes(body, 0, title, NULL, 0, episodes, PODCASTCATCHER_MAX_EPISODES, NULL);
	free(body);
	if (count <= 0) {
		set_message("no episodes: %s", title);
		return 0;
	}

	int indexes[PODCASTCATCHER_MAX_EPISODES];
	int taken = podcastcatcher_state_take_new(st, feed->url, first, episodes, count, max_new, indexes);

	int downloaded = 0;
	for (int i = 0; i < taken && !stop_asked; i++) {
		podcast_episode_t *episode = &episodes[indexes[i]];

		// The card can leave between one episode and the next. A missing card
		// is not a failed download -- it would spend the episode's retries on
		// a card that is simply not there.
		if (!podcastdl_root()[0]) {
			set_message("no card");
			break;
		}

		// Already on the card, from a download made before the feed was in
		// feeds.txt or by hand from the Podcasts page: nothing to do but
		// remember that it is there.
		if (podcastdl_exists(episode, title)) {
			podcastcatcher_state_downloaded(st, feed->url, episode->id);
			continue;
		}
		if (!card_has_room(root, min_free_mb)) {
			set_message("card full: %s", title);
			fprintf(stderr, "podcastcatcher: card is nearly full; stopping\n");
			break;
		}

		set_message("downloading: %s", episode->title);
		if (podcastdl_fetch(episode, title, author, image)) {
			podcastcatcher_state_downloaded(st, feed->url, episode->id);
			downloaded++;
		} else {
			podcastcatcher_state_failed(st, feed->url, episode->id);
			set_message("download failed: %s", episode->title);
		}
	}
	return downloaded;
}

// One pass over feeds.txt. Called on the way into a Wi-Fi connection, once at
// start-up when the radio is already up, and when a card arrives.
static void run_check(void) {
	const char *root = storage_sd_root();
	if (!root || !root[0]) {
		set_message("no card");
		return;
	}
	if (!podcastcatcher_enabled()) {
		set_message("module off");
		return;
	}

	char path[600];
	feeds_path(root, path, sizeof(path));
	char *text = read_file_content(path);
	if (!text) {
		set_message("no %s", path);
		fprintf(stderr, "podcastcatcher: no %s on the card\n", path);
		return;
	}

	podcastcatcher_feed_t feeds[PODCASTCATCHER_MAX_FEEDS];
	int feed_count = podcastcatcher_parse_feeds(text, feeds, PODCASTCATCHER_MAX_FEEDS);
	free(text);
	if (feed_count == 0) {
		set_message("no feeds in %s", path);
		return;
	}

	podcastcatcher_state_t *st = podcastcatcher_state_open(root);
	if (!st) {
		set_message("cannot open state");
		return;
	}
	podcastcatcher_state_prune(st, feeds, feed_count);

	int max_new = (int)config_clamped("max_episodes", PODCASTCATCHER_DEFAULT_MAX_NEW, 1, PODCASTCATCHER_MAX_EPISODES);
	long min_free_mb = config_clamped("min_free_mb", 200, 0, 100000);

	int downloaded = 0;
	for (int i = 0; i < feed_count && !stop_asked; i++) {
		set_message("checking: %s", feeds[i].url);
		downloaded += check_feed(st, &feeds[i], max_new, min_free_mb, root);
	}
	podcastcatcher_state_close(st);

	set_message("%d downloaded", downloaded);
	if (downloaded > 0) {
		notify_new_episodes(downloaded);
	}
}

static void *catcher_main(void *arg) {
	(void)arg;
	thread_be_low_priority("podcast catcher");

	bool was_connected = false;
	// Monotonic, not the wall clock: this device's clock starts at 1970 and
	// jumps when NTP answers, and a check scheduled against a clock that jumps
	// backwards waits for an hour that already happened.
	double last_check = -(double)CATCHER_COOLDOWN_SECS;

	for (;;) {
		if (stop_asked) {
			break;
		}

		wifi_status_t status;
		wifi_get_status(&status);
		bool connected = status.state == WIFI_STATE_CONNECTED;

		// The first poll answers "not connected before, connected now" for a
		// radio that was already up when the player started, which is the
		// boot-time check for free.
		bool joined = connected && !was_connected;
		bool asked = check_requested;
		check_requested = false;

		if (joined || asked) {
			// DHCP and DNS are not finished the instant the link is. A feed
			// fetched in that window fails for a reason that has nothing to do
			// with the feed.
			sleep_ms(3000);
			struct timespec now_ts;
			clock_gettime(CLOCK_MONOTONIC, &now_ts);
			double now = (double)now_ts.tv_sec + (double)now_ts.tv_nsec / 1e9;
			if (!stop_asked && (now - last_check >= (double)CATCHER_COOLDOWN_SECS || asked)) {
				run_check();
				last_check = now;
			}
		}

		was_connected = connected;
		sleep_ms(2000);
	}

	set_message("stopped");
	return NULL;
}

void podcastcatcher_start(void) {
	pthread_mutex_lock(&start_lock);
	if (started) {
		pthread_mutex_unlock(&start_lock);
		return;
	}
	if (!podcastcatcher_enabled()) {
		pthread_mutex_unlock(&start_lock);
		return;
	}
	stop_asked = false;
	check_requested = false;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	// The episode array on the checker's stack is the largest thing here;
	// half a megabyte leaves room for the XML scanner and the HTTP buffers.
	pthread_attr_setstacksize(&attr, 512 * 1024);
	bool ok = pthread_create(&worker, &attr, catcher_main, NULL) == 0;
	pthread_attr_destroy(&attr);
	if (ok) {
		started = true;
		set_message("watching");
	}
	pthread_mutex_unlock(&start_lock);
}

void podcastcatcher_stop(void) {
	stop_asked = true;
}
