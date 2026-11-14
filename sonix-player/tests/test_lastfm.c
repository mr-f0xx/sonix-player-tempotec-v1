// Headless checks of the Last.fm scrobbler: queueing plays while API keys
// are unavailable (LASTFM_STATE_UNAVAILABLE), persisting them to
// .local/lastfm.queue, dynamically attaching keys from the card (including when
// a .bin sits beside streaming-keys.ini), and flushing/scrobbling once keys and
// a connection become available.

#define _GNU_SOURCE 1

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Advance CLOCK_BOOTTIME deterministically for fast playback qualification
// while leaving CLOCK_MONOTONIC real for pthread_cond_timedwait().
static pthread_mutex_t boot_clock_lock = PTHREAD_MUTEX_INITIALIZER;
static long long fake_boot_ns = 100000000000LL;

static int test_clock_gettime(clockid_t clk_id, struct timespec *tp) {
#ifdef CLOCK_BOOTTIME
	if (clk_id == CLOCK_BOOTTIME) {
		pthread_mutex_lock(&boot_clock_lock);
		long long ns = fake_boot_ns;
		pthread_mutex_unlock(&boot_clock_lock);
		tp->tv_sec = (time_t)(ns / 1000000000LL);
		tp->tv_nsec = (long)(ns % 1000000000LL);
		return 0;
	}
#endif
	return clock_gettime(clk_id, tp);
}

static void advance_boot_secs(double secs) {
	pthread_mutex_lock(&boot_clock_lock);
	fake_boot_ns += (long long)(secs * 1000000000.0);
	pthread_mutex_unlock(&boot_clock_lock);
}

#define clock_gettime test_clock_gettime
#include "../src/system/lastfm/lastfm.c"
#undef clock_gettime

// ---------------------------------------------------------------------------
// stubbed storage, Wi-Fi and HTTP transport
// ---------------------------------------------------------------------------

static char stub_card_root[256];
static bool stub_card_ok = true;
static bool stub_wifi_up = true;

const char *storage_sd_root(void) { return stub_card_ok ? stub_card_root : NULL; }
const char *storage_sd_device(void) { return stub_card_ok ? "sd0" : NULL; }
bool storage_card_attached(void) { return stub_card_ok; }
bool storage_sd_writable(void) { return stub_card_ok; }
bool usb_storage_active(void) { return false; }

void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	if (stub_wifi_up) {
		out->state = WIFI_STATE_CONNECTED;
		snprintf(out->ip, sizeof(out->ip), "192.168.1.50");
	} else {
		out->state = WIFI_STATE_OFF;
	}
}

static pthread_mutex_t http_lock = PTHREAD_MUTEX_INITIALIZER;
static bool next_sent = true;
static int next_status = 200;
static char next_body[2048] = "<lfm status=\"ok\"/>";
static char next_error[160] = "";
static int scrobble_requests = 0;
static char last_scrobble_body[8192];

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len, size_t limit, int timeout_secs,
				  int *status_out) {
	(void)url;
	(void)limit;
	(void)timeout_secs;
	pthread_mutex_lock(&http_lock);
	if (req && req->body && strstr(req->body, "method=track.scrobble")) {
		scrobble_requests++;
		snprintf(last_scrobble_body, sizeof(last_scrobble_body), "%s", req->body);
	}
	bool sent = next_sent;
	int st = next_status;
	char reply[2048];
	snprintf(reply, sizeof(reply), "%s", next_body);
	pthread_mutex_unlock(&http_lock);

	if (!sent) {
		*status_out = 0;
		return false;
	}
	*status_out = st;
	*out_len = strlen(reply);
	*out = malloc(*out_len + 1);
	assert(*out);
	memcpy(*out, reply, *out_len + 1);
	return true;
}

const char *http_last_error(void) { return next_error; }

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void play_track(const char *file, const char *artist, const char *title, const char *album, double duration) {
	lastfm_playback_t pb = {
		.playing = true,
		.file = file,
		.position = 0.0,
		.duration = duration,
		.artist = artist,
		.title = title,
		.album = album,
		.skip = false,
	};
	lastfm_note_playback(&pb);
	int ticks = (int)(duration / 2.0) + 2;
	for (int i = 1; i <= ticks; i++) {
		advance_boot_secs(1.0);
		pb.position = (double)i;
		lastfm_note_playback(&pb);
	}
}

static void wait_for_queued(int want_queued) {
	for (int i = 0; i < 200; i++) {
		pthread_mutex_lock(&lock);
		int fc = file_count;
		int pc = pending_count;
		pthread_mutex_unlock(&lock);
		if (fc == want_queued && pc == 0) {
			return;
		}
		struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000};
		nanosleep(&ts, NULL);
	}
	pthread_mutex_lock(&lock);
	int fc = file_count;
	int pc = pending_count;
	pthread_mutex_unlock(&lock);
	fprintf(stderr, "timed out waiting for file_count=%d pending_count=0 (got file=%d pending=%d)\n",
			want_queued, fc, pc);
	assert(fc == want_queued && pc == 0);
}

static int count_occurrences(const char *path, const char *needle) {
	FILE *f = fopen(path, "r");
	assert(f);
	char line[512];
	int count = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, needle)) {
			count++;
		}
	}
	fclose(f);
	return count;
}

int main(void) {
	char tmpdir[] = "/tmp/lastfm-test-XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	char data_ini[512];
	snprintf(data_ini, sizeof(data_ini), "%s/data-streaming-keys.ini", tmpdir);
	setenv("SONIX_STREAMKEYS_DATA", data_ini, 1);

	char cfg_path[512];
	snprintf(cfg_path, sizeof(cfg_path), "%s/device_config.ini", tmpdir);
	snprintf(stub_card_root, sizeof(stub_card_root), "%s/card", tmpdir);
	assert(mkdir(stub_card_root, 0755) == 0);

	// Seed config with Last.fm enabled and a saved session, but NO API keys.
	config_init(cfg_path);
	config_set_bool("lastfm", "enabled", true);
	config_save();

	char sess[512];
	snprintf(sess, sizeof(sess), "%s/lastfm.session", tmpdir);
	FILE *sf = fopen(sess, "w");
	assert(sf);
	fprintf(sf, "sk-test-123\nlistener\n");
	fclose(sf);

	streamkeys_init();
	lastfm_init();

	lastfm_status_t st;
	lastfm_get_status(&st);
	assert(st.state == LASTFM_STATE_UNAVAILABLE);
	assert(!lastfm_available());
	assert(lastfm_active());
	assert(st.queued == 0);

	// Play two tracks while Last.fm reports LASTFM_STATE_UNAVAILABLE.
	play_track("/sd/01.flac", "Artist One", "Track One", "Album A", 40.0);
	play_track("/sd/02.flac", "Artist Two", "Track Two", "Album B", 50.0);

	// Both plays must be retained in the on-card queue file immediately.
	wait_for_queued(2);
	char qpath[512];
	snprintf(qpath, sizeof(qpath), "%s/.local/lastfm.queue", stub_card_root);
	assert(count_occurrences(qpath, "#lastfm\tlistener") == 1);
	assert(count_occurrences(qpath, "Artist One\tTrack One\tAlbum A") == 1);
	assert(count_occurrences(qpath, "Artist Two\tTrack Two\tAlbum B") == 1);

	// Even if a sealed streaming-keys.bin for another key sits beside
	// streaming-keys.ini on the card, streamkeys_load_card() must still read
	// streaming-keys.ini and pick up [lastfm].
	char bin_path[512];
	snprintf(bin_path, sizeof(bin_path), "%s/streaming-keys.bin", stub_card_root);
	FILE *bf = fopen(bin_path, "wb");
	assert(bf);
	fwrite("SXK1invalid-sealed-blob", 1, 23, bf);
	fclose(bf);

	char card_ini[512];
	snprintf(card_ini, sizeof(card_ini), "%s/streaming-keys.ini", stub_card_root);
	FILE *kf = fopen(card_ini, "w");
	assert(kf);
	fprintf(kf, "[lastfm]\napi_key = testkey123\napi_secret = testsecret456\n");
	fclose(kf);

	// Simulate transport failure first: keys load, state becomes CONNECTED,
	// and queued tracks remain safe on disk without being dropped.
	pthread_mutex_lock(&http_lock);
	next_sent = false;
	snprintf(next_error, sizeof(next_error), "network unreachable");
	pthread_mutex_unlock(&http_lock);

	assert(streamkeys_load_card(stub_card_root));
	lastfm_reload_keys();

	lastfm_get_status(&st);
	assert(st.state == LASTFM_STATE_CONNECTED);
	assert(lastfm_available());
	assert(st.queued == 2);

	// Also verify queue_append() does not duplicate the header when appending
	// another play to a non-empty queue file.
	play_track("/sd/03.flac", "Artist Three", "Track Three", "", 40.0);
	wait_for_queued(3);
	assert(count_occurrences(qpath, "#lastfm\tlistener") == 1);
	assert(count_occurrences(qpath, "Artist Three\tTrack Three\t") == 1);

	// Now let HTTP succeed and trigger immediate retry via key refresh / wake.
	pthread_mutex_lock(&http_lock);
	next_sent = true;
	next_status = 200;
	snprintf(next_body, sizeof(next_body), "<lfm status=\"ok\"><scrobbles accepted=\"3\" ignored=\"0\"/></lfm>");
	pthread_mutex_unlock(&http_lock);

	// Simulate Wi-Fi reconnect / wake so backoff resets immediately.
	stub_wifi_up = false;
	pthread_mutex_lock(&lock);
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
	struct timespec ts = {.tv_sec = 0, .tv_nsec = 30000000};
	nanosleep(&ts, NULL);
	stub_wifi_up = true;
	lastfm_init(); // idempotent re-init / wake

	wait_for_queued(0);
	pthread_mutex_lock(&http_lock);
	assert(scrobble_requests >= 1);
	assert(strstr(last_scrobble_body, "Track%20One") != NULL || strstr(last_scrobble_body, "Track+One") != NULL);
	assert(strstr(last_scrobble_body, "Track%20Two") != NULL || strstr(last_scrobble_body, "Track+Two") != NULL);
	assert(strstr(last_scrobble_body, "Track%20Three") != NULL || strstr(last_scrobble_body, "Track+Three") != NULL);
	pthread_mutex_unlock(&http_lock);
	assert(access(qpath, F_OK) != 0);

	// Play a track while the card is not attached yet, then attach the card
	// and verify the buffered play flushes and scrobbles.
	stub_card_ok = false;
	play_track("/sd/04.flac", "Artist Four", "Track Four", "Album D", 40.0);
	lastfm_get_status(&st);
	assert(st.queued == 1);
	stub_card_ok = true;
	lastfm_reload_keys();
	wait_for_queued(0);

	// If Last.fm refuses with an invalid API key (LASTFM_STATE_REFUSED), the
	// play stays queued, and reloading updated keys clears the refusal and
	// scrobbles immediately.
	pthread_mutex_lock(&http_lock);
	next_sent = true;
	next_status = 200;
	snprintf(next_body, sizeof(next_body),
			 "<lfm status=\"failed\"><error code=\"10\">Invalid API key</error></lfm>");
	pthread_mutex_unlock(&http_lock);

	play_track("/sd/05.flac", "Artist Five", "Track Five", "Album E", 40.0);
	for (int i = 0; i < 200; i++) {
		lastfm_get_status(&st);
		if (st.state == LASTFM_STATE_REFUSED) {
			break;
		}
		struct timespec wait_ts = {.tv_sec = 0, .tv_nsec = 10000000};
		nanosleep(&wait_ts, NULL);
	}
	lastfm_get_status(&st);
	assert(st.state == LASTFM_STATE_REFUSED);
	assert(st.queued == 1);

	kf = fopen(card_ini, "w");
	assert(kf);
	fprintf(kf, "[lastfm]\napi_key = updatedkey789\napi_secret = updatedsecret012\n");
	fclose(kf);
	pthread_mutex_lock(&http_lock);
	snprintf(next_body, sizeof(next_body), "<lfm status=\"ok\"><scrobbles accepted=\"1\" ignored=\"0\"/></lfm>");
	pthread_mutex_unlock(&http_lock);
	assert(streamkeys_load_card(stub_card_root));
	lastfm_reload_keys();
	wait_for_queued(0);
	lastfm_get_status(&st);
	assert(st.state == LASTFM_STATE_CONNECTED);

	// Clean up temp files.
	unlink(card_ini);
	unlink(bin_path);
	unlink(sess);
	unlink(cfg_path);
	char local_dir[512];
	snprintf(local_dir, sizeof(local_dir), "%s/.local", stub_card_root);
	rmdir(local_dir);
	rmdir(stub_card_root);
	rmdir(tmpdir);

	puts("test_lastfm: ok");
	return 0;
}
