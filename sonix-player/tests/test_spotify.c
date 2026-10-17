// Spotify integration tests: the Q2SPOT/1 protocol, the receiver's four
// interfaces, and the sink that carries its audio to the DAC.
//
// Everything here is real: the real spotify.c, spotifysink.c and
// spotifybrowse.c, real Unix sockets, a real FIFO and a real child process.
// What is stubbed is the card's receiver (a shell script that writes the status
// file and feeds the FIFO) and the ALSA layer under it (audio_external_*), so
// the suite needs no librespot, no Spotify account and no network.
//
// Run with: make -C sonix-player/tests spotify

#include "src/system/audio/audio.h"
#include "src/system/streaming/spotify.h"
#include "src/system/streaming/spotifybrowse.h"
#include "src/system/streaming/spotifysink.h"
#include "src/system/core/config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// the stubbed player
//
// Only the surfaces the two Spotify modules touch: the external-audio calls
// into audio.c, the settings store (linked for real), the two thread-priority
// helpers and the device name.
// ---------------------------------------------------------------------------

static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audio_cond = PTHREAD_COND_INITIALIZER;
static long audio_frames;
static int audio_opens;
static int audio_ends;
static int audio_pauses;
static int audio_bits;
static bool audio_device_open;
static atomic_bool local_playing;

static bool (*registered_release_cb)(void);

bool audio_external_begin_latency(int sample_rate, int channels, int bits, int buffer_ms) {
	assert(sample_rate == 44100);
	assert(channels == 2);
	assert(buffer_ms > 0);
	if (bits != 16 && bits != 32) {
		return false;
	}

	// The player's own PCM is open and it is the only writer there is: the
	// external route cannot have the device while it plays, exactly as
	// external_open_locked() refuses on the device. The sink answers that by
	// dropping what arrives until the local player lets go, so the suite waits
	// on the sink's own state rather than on a re-open that cannot happen.
	if (atomic_load(&local_playing)) {
		return false;
	}

	pthread_mutex_lock(&audio_lock);
	if (!audio_device_open) {
		audio_device_open = true;
		audio_opens++;
	}
	audio_bits = bits;
	pthread_cond_broadcast(&audio_cond);
	pthread_mutex_unlock(&audio_lock);
	return true;
}

bool audio_external_begin(int sample_rate, int channels, int bits) {
	return audio_external_begin_latency(sample_rate, channels, bits, 0);
}

int audio_external_write(const void *frames, int count) {
	(void)frames;
	pthread_mutex_lock(&audio_lock);
	audio_frames += count;
	pthread_cond_broadcast(&audio_cond);
	pthread_mutex_unlock(&audio_lock);
	return count;
}

void audio_external_end(void) {
	pthread_mutex_lock(&audio_lock);
	if (audio_device_open) {
		audio_device_open = false;
		audio_ends++;
	}
	pthread_cond_broadcast(&audio_cond);
	pthread_mutex_unlock(&audio_lock);
}

void audio_pause(void) {
	pthread_mutex_lock(&audio_lock);
	audio_pauses++;
	pthread_cond_broadcast(&audio_cond);
	pthread_mutex_unlock(&audio_lock);
}

audio_status_t audio_get_status(void) {
	return atomic_load(&local_playing) ? AUDIO_STATUS_PLAYING : AUDIO_STATUS_STOPPED;
}

void audio_set_external_release_cb(bool (*cb)(void)) { registered_release_cb = cb; }

const char *sysinfo_device_name(void) { return "Test V1"; }

// ---------------------------------------------------------------------------

static char root[512];
static char fixture[600];
static char receiver_script[1024];

static double now_secs(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool wait_until(bool (*done)(void), double seconds) {
	double end = now_secs() + seconds;
	while (now_secs() < end) {
		if (done()) {
			return true;
		}
		usleep(2000);
	}
	return done();
}

// What this process's own threads spent on the CPU over a window of wall
// clock, in milliseconds. The receiver is a child process, so this counts the
// player's side of the Spotify work and nothing else: the sink's two threads
// and the service's control thread, which are all supposed to be asleep when
// there is nothing to do.
//
// It is the measurement the freeze is: a timed wait whose deadline is on the
// wrong clock returns at once, and its thread -- the playback thread, at
// real-time priority -- spins instead of sleeping.
static double cpu_ms_over(double seconds) {
	struct timespec before, after;
	assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &before) == 0);
	double end = now_secs() + seconds;
	while (now_secs() < end) {
		usleep(1000);
	}
	assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &after) == 0);
	return (double)(after.tv_sec - before.tv_sec) * 1000.0 +
	       (double)(after.tv_nsec - before.tv_nsec) / 1000000.0;
}

static bool running_true(void) { return spotify_running(); }
static bool sink_holds_output_true(void) { return spotifysink_holds_output(); }
static bool running_false(void) { return !spotify_running(); }

// The service reads the status file on a half-second poll, so a test that wants
// the receiver's own state waits for it rather than looking once.
// The receivers below stop on their own, so waiting for "not running" would be
// a wait that starts out satisfied -- the state worth waiting for is the report.
static bool idle_stop_reported(void) {
	return !spotify_enabled() && !spotify_running();
}

static bool death_reported(void) {
	return strstr(spotify_status_text(), "stopped (2)") != NULL;
}

// The receiver's own output, where spotify.c puts it: the log the page shows the
// tail of. The stand-in records the two attributes the spawn gave it beside
// itself on the card; the suite reads both back from here.
#define TEST_RECEIVER_LOG "/tmp/sonix-spotify.log"

static bool child_self_recorded(void) {
	char path[600];
	snprintf(path, sizeof(path), "%s/child_self.txt", spotify_dir());
	struct stat st;
	return stat(path, &st) == 0 && st.st_size > 0;
}

static void assert_child_self(void) {
	char path[600];
	snprintf(path, sizeof(path), "%s/child_self.txt", spotify_dir());
	FILE *f = fopen(path, "r");
	assert(f);
	int oom = -1;
	int level = -1;
	assert(fscanf(f, "%d %d", &oom, &level) == 2);
	fclose(f);

	// The player asks to be the last process the kernel kills (oom_score_adj
	// -500, main.c). A receiver that inherited that score is a foreign binary
	// the kernel may not touch when memory runs out, and the device freezes
	// around it instead of the receiver dying: it asks to go first, and it runs
	// below the interface rather than at the same level as the screen.
	assert(oom == 500);
	assert(level == 10);
}

// The receiver's log is on the tmpfs, which is the device's RAM: a receiver
// that decides to be verbose must not be able to fill it. What is kept is the
// tail -- the last thing a receiver said is what explains it stopping -- so the
// marker at the end is there and the first line of the bulk is not.
static bool receiver_log_kept_its_tail(void) {
	struct stat st;
	if (stat(TEST_RECEIVER_LOG, &st) != 0 || st.st_size < 1024 || st.st_size > 100 * 1024) {
		return false;
	}

	static char body[128 * 1024];
	FILE *f = fopen(TEST_RECEIVER_LOG, "r");
	if (!f) {
		return false;
	}
	size_t n = fread(body, 1, sizeof(body) - 1, f);
	fclose(f);
	body[n] = '\0';

	return strstr(body, "LATE-MARKER") != NULL && strstr(body, "EARLY line 0\n") == NULL;
}

static bool state_playing(void) {
	spotify_state_t state;
	spotify_get_state(&state);
	return state.state == SPOTIFY_PLAY_PLAYING;
}

static long frames_now(void) {
	pthread_mutex_lock(&audio_lock);
	long frames = audio_frames;
	pthread_mutex_unlock(&audio_lock);
	return frames;
}

// One second of audio, counted from wherever the caller started looking: the
// two places that write into the FIFO do not share a starting point.
static long frames_baseline;
static bool frames_advanced(void) {
	return frames_now() > frames_baseline + 44100;
}

// ---------------------------------------------------------------------------
// the protocol
// ---------------------------------------------------------------------------

static void test_escape(void) {
	char out[64];

	assert(spotify_escape("plain", out, sizeof(out)) == 5 && strcmp(out, "plain") == 0);
	assert(spotify_escape("a\tb", out, sizeof(out)) == 5 && strcmp(out, "a%09b") == 0);
	assert(spotify_escape("a\nb", out, sizeof(out)) == 5 && strcmp(out, "a%0Ab") == 0);
	assert(spotify_escape("a\rb", out, sizeof(out)) == 5 && strcmp(out, "a%0Db") == 0);
	assert(spotify_escape("50%", out, sizeof(out)) == 5 && strcmp(out, "50%25") == 0);
	// UTF-8 goes through untouched: the escaping is byte-wise.
	assert(spotify_escape("Björk", out, sizeof(out)) == 6 && strcmp(out, "Björk") == 0);
	// Three bytes per escape, plus the terminator: a field that does not fit is
	// refused rather than cut, because the caller sends it as an identifier.
	assert(spotify_escape("aaa", out, 3) == -1);
	assert(spotify_escape("aaaa", out, 3) == -1);
	assert(spotify_escape("%", out, 4) == 3 && strcmp(out, "%25") == 0);
	assert(spotify_escape("%", out, 3) == -1);

	char back[64];
	assert(spotify_unescape("a%09b", 5, back, sizeof(back)) == 3 && strcmp(back, "a\tb") == 0);
	assert(spotify_unescape("50%25", 5, back, sizeof(back)) == 3 && strcmp(back, "50%") == 0);
	assert(spotify_unescape("Björk", 6, back, sizeof(back)) == 6);
	// A '%' that is not an escape, a decoded NUL, and an escape that runs off
	// the end are all malformed rather than best-effort.
	assert(spotify_unescape("%zz", 3, back, sizeof(back)) == -1);
	assert(spotify_unescape("%00", 3, back, sizeof(back)) == -1);
	assert(spotify_unescape("abc%", 4, back, sizeof(back)) == -1);
	assert(spotify_unescape("ab%0", 4, back, sizeof(back)) == -1);
	assert(spotify_unescape("abcd", 4, back, 4) == -1);
	assert(spotify_unescape("abc", 3, back, 4) == 3);
}

static void test_request(void) {
	char request[SPOTIFY_REQ_MAX];

	int n = spotify_request_build(request, sizeof(request), 7, "liked", NULL, 0);
	assert(n == (int)strlen("Q2SPOT/1\n7\tliked\n"));
	assert(strcmp(request, "Q2SPOT/1\n7\tliked\n") == 0);

	const char *play_args[] = {"spotify:album:abc", "12", "spotify:track:zzz"};
	n = spotify_request_build(request, sizeof(request), 1024, "play", play_args, 3);
	assert(n > 0 && strcmp(request, "Q2SPOT/1\n1024\tplay\tspotify:album:abc\t12\tspotify:track:zzz\n") == 0);

	// A search term with the separator in it survives the round trip.
	const char *search_args[] = {"tracks", "a\tb%c", NULL};
	n = spotify_request_build(request, sizeof(request), 3, "search", search_args, 2);
	assert(n > 0 && strcmp(request, "Q2SPOT/1\n3\tsearch\ttracks\ta%09b%25c\n") == 0);

	// Too long for the protocol's 8 KiB: refused, not truncated -- a cut
	// argument is a different request, and for a URI that means another track.
	char big[SPOTIFY_REQ_MAX + 64];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	const char *big_args[] = {big};
	assert(spotify_request_build(request, sizeof(request), 1, "search", big_args, 1) == -1);
}

static void test_parse(void) {
	// A whole page, including a row whose fields carry the escaped bytes.
	const char *reply = "Q2SPOT/1\n"
						"7\tok\t\tCg4IARIK\n"
						"track\tspotify:track:4uLU6hMCjMI75M1A2tKUQC\tNever Gonna Give You Up\tRick Astley\t"
						"spotify:user:alice:collection\t0\n"
						"playlist\tspotify:playlist:37i9dQZF1DXcBWIGoYBM5M\tToday's Top Hits\tSpotify\t\t\n"
						"artist\tspotify:artist:0OdUWJ0sBjDrqHygGUXeCF\tBand of Horses\t\t\t\n";
	spotify_reply_t parsed;
	assert(spotify_reply_parse(reply, strlen(reply), 7, &parsed));
	assert(parsed.status == SPOTIFY_REPLY_OK);
	assert(strcmp(parsed.cursor, "Cg4IARIK") == 0);
	assert(parsed.count == 3);
	assert(parsed.rows[0].kind == SPOTIFY_ROW_TRACK);
	assert(strcmp(parsed.rows[0].uri, "spotify:track:4uLU6hMCjMI75M1A2tKUQC") == 0);
	assert(strcmp(parsed.rows[0].title, "Never Gonna Give You Up") == 0);
	assert(strcmp(parsed.rows[0].subtitle, "Rick Astley") == 0);
	assert(strcmp(parsed.rows[0].context, "spotify:user:alice:collection") == 0);
	assert(strcmp(parsed.rows[0].position, "0") == 0);
	assert(parsed.rows[1].kind == SPOTIFY_ROW_PLAYLIST && parsed.rows[1].context[0] == '\0');

	// Escapes are decoded on the way in, titles and all.
	const char *escaped = "Q2SPOT/1\n1\tok\tA%09B\t\n"
						  "setting\tshuffle\ton\t\t\t\n";
	assert(spotify_reply_parse(escaped, strlen(escaped), 1, &parsed));
	assert(strcmp(parsed.message, "A\tB") == 0);
	assert(parsed.count == 1 && parsed.rows[0].kind == SPOTIFY_ROW_SETTING);
	assert(strcmp(parsed.rows[0].uri, "shuffle") == 0 && strcmp(parsed.rows[0].title, "on") == 0);

	// A status the page shows rather than a list.
	const char *login = "Q2SPOT/1\n9\tlogin\tPair from the Spotify app\t\n";
	assert(spotify_reply_parse(login, strlen(login), 9, &parsed));
	assert(parsed.status == SPOTIFY_REPLY_LOGIN && parsed.count == 0);
	assert(strcmp(parsed.message, "Pair from the Spotify app") == 0);

	const char *throttled = "Q2SPOT/1\n2\tthrottled\tTry again in 30 s\t\n";
	assert(spotify_reply_parse(throttled, strlen(throttled), 2, &parsed));
	assert(parsed.status == SPOTIFY_REPLY_THROTTLED);

	// A late answer: another request's number means it is not ours to read.
	assert(!spotify_reply_parse(login, strlen(login), 8, &parsed));
	assert(parsed.status == SPOTIFY_REPLY_MALFORMED);

	// Malformed in every way the protocol document calls out.
	const char *bad_magic = "Q2SPOT/2\n9\tlogin\t\t\n";
	assert(!spotify_reply_parse(bad_magic, strlen(bad_magic), 9, &parsed));
	const char *short_line = "Q2SPOT/1\n9\tlogin\t\n";
	assert(!spotify_reply_parse(short_line, strlen(short_line), 9, &parsed));
	const char *seven = "Q2SPOT/1\n1\tok\t\t\ntrack\tu\tt\ts\tc\tp\textra\n";
	assert(!spotify_reply_parse(seven, strlen(seven), 1, &parsed));
	const char *bad_escape = "Q2SPOT/1\n1\tok\t\t\ntrack\tspotify:track:%zz\tt\ts\tc\tp\n";
	assert(!spotify_reply_parse(bad_escape, strlen(bad_escape), 1, &parsed));
	const char *unknown_status = "Q2SPOT/1\n1\tbanana\t\t\n";
	assert(!spotify_reply_parse(unknown_status, strlen(unknown_status), 1, &parsed));
	// A last row that lost a field is a reply that was cut off, newline or no
	// newline; the same row without its final newline but whole is accepted, so
	// that how a writer flushed the last byte is not what decides.
	const char *incomplete = "Q2SPOT/1\n1\tok\t\t\ntrack\tspotify:track:x\tt\ts\tc";
	assert(!spotify_reply_parse(incomplete, strlen(incomplete), 1, &parsed));
	const char *no_newline = "Q2SPOT/1\n1\tok\t\t\ntrack\tspotify:track:x\tt\ts\tc\tp";
	assert(spotify_reply_parse(no_newline, strlen(no_newline), 1, &parsed));
	assert(parsed.count == 1 && strcmp(parsed.rows[0].position, "p") == 0);

	// An opaque field that does not fit is refused: the URI is passed back to
	// the receiver, so a cut one would play something else.
	char long_uri[SPOTIFY_URI_MAX + 64];
	memset(long_uri, 'u', sizeof(long_uri));
	long_uri[sizeof(long_uri) - 1] = '\0';
	char oversized[1200];
	int written = snprintf(oversized, sizeof(oversized), "Q2SPOT/1\n1\tok\t\t\ntrack\t%s\tt\ts\tc\tp\n", long_uri);
	assert(written > 0 && written < (int)sizeof(oversized));
	assert(!spotify_reply_parse(oversized, (size_t)written, 1, &parsed));

	// ...but a long *title* is display text and is cut to fit instead, on a
	// character boundary.
	char long_title[SPOTIFY_TEXT_MAX * 2 + 2];
	char *at = long_title;
	for (int i = 0; i < SPOTIFY_TEXT_MAX; i++) {
		*at++ = (char)0xC3; // "é", two bytes: 400 of them, twice the field
		*at++ = (char)0xA9;
	}
	*at = '\0';
	written = snprintf(oversized, sizeof(oversized), "Q2SPOT/1\n1\tok\t\t\ntrack\tspotify:track:x\t%s\ts\tc\tp\n", long_title);
	if (written > 0 && written < (int)sizeof(oversized)) {
		assert(spotify_reply_parse(oversized, (size_t)written, 1, &parsed));
		size_t length = strlen(parsed.rows[0].title);
		assert(length > 0 && length < SPOTIFY_TEXT_MAX);
		// Whole characters only: the last byte is the second half of a pair,
		// never the lead byte that would have introduced a half one.
		assert((unsigned char)parsed.rows[0].title[length - 1] == 0xA9);
	}

	// A NUL inside the reply would silently shorten every comparison below it.
	const char embedded[] = "Q2SPOT/1\n1\tok\t\t\ntrack\tspot:ify\tT\0S\tc\tp\n";
	assert(!spotify_reply_parse(embedded, sizeof(embedded) - 1, 1, &parsed));

	// More rows than the protocol allows.
	char many[SPOTIFY_REPLY_MAX];
	int used = snprintf(many, sizeof(many), "Q2SPOT/1\n1\tok\t\t\n");
	for (int i = 0; i < SPOTIFY_ROWS_MAX + 1 && used < (int)sizeof(many) - 64; i++) {
		used += snprintf(many + used, sizeof(many) - (size_t)used, "track\tspotify:track:%d\t\t\t\t\n", i);
	}
	assert(!spotify_reply_parse(many, (size_t)used, 1, &parsed));
}

// ---------------------------------------------------------------------------
// the conversation, against a socket that answers the way the receiver does
// ---------------------------------------------------------------------------

typedef struct {
	const char *socket_path;
	const char *reply;
	bool stay_silent; // read the request, answer nothing, keep the socket open
	int listen_fd;
	char seen[SPOTIFY_REQ_MAX];
	atomic_bool done;
	pthread_t thread;
} fake_server_t;

static void *fake_server_main(void *arg) {
	fake_server_t *server = arg;

	int fd = accept(server->listen_fd, NULL, NULL);
	assert(fd >= 0);

	// The client half-closes once the request is written; the reply follows.
	size_t got = 0;
	ssize_t n;
	while ((n = read(fd, server->seen + got, sizeof(server->seen) - got - 1)) > 0) {
		got += (size_t)n;
	}
	server->seen[got] = '\0';

	if (server->reply) {
		assert(write(fd, server->reply, strlen(server->reply)) == (ssize_t)strlen(server->reply));
	} else if (server->stay_silent) {
		// Longer than the client's deadline: this is the receiver that accepted
		// the request and has not answered yet.
		usleep(900 * 1000);
	}
	close(fd);
	close(server->listen_fd);
	unlink(server->socket_path);
	atomic_store(&server->done, true);
	return NULL;
}

// Bound and listening before the client is let loose: a test that raced the
// thread above would fail as a missing socket, which is a case of its own.
static void fake_server_start(fake_server_t *server) {
	memset(server->seen, 0, sizeof(server->seen));
	atomic_store(&server->done, false);

	unlink(server->socket_path);
	int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(listen_fd >= 0);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", server->socket_path);
	assert(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(listen_fd, 4) == 0);
	server->listen_fd = listen_fd;

	assert(pthread_create(&server->thread, NULL, fake_server_main, server) == 0);
}

static void test_socket_call(void) {
	char path[sizeof(root) + 32];
	snprintf(path, sizeof(path), "%s/browse.sock", root);

	const char *reply = "Q2SPOT/1\n"
						"7\tok\t\tcursor1\n"
						"track\tspotify:track:1\tTitle\tArtist\tspotify:user:x:collection\t3\n";

	fake_server_t server = {.socket_path = path, .reply = reply};

	fake_server_start(&server);
	spotify_reply_t parsed;
	const char *args[] = {"spotify:playlist:1", NULL};
	// The fake answers with request number 7, so this asks as 7: a reply
	// carrying another number is dropped, which test_parse() covers.
	assert(spotify_browse_call(path, 7, "playlist", args, 1, &parsed, 2000));
	assert(parsed.count == 1);
	assert(strcmp(parsed.cursor, "cursor1") == 0);
	assert(strcmp(parsed.rows[0].position, "3") == 0);
	pthread_join(server.thread, NULL);
	assert(strcmp(server.seen, "Q2SPOT/1\n7\tplaylist\tspotify:playlist:1\n") == 0);

	// A request number that is not this request's is dropped: the headers of
	// two socket rounds above differ only in that field, and test_parse()
	// covers the mismatched case without a socket.
	assert(!spotify_browse_call(path, 6, "playlist", args, 1, &parsed, 100));
	assert(parsed.status == SPOTIFY_REPLY_NOSOCKET);

	// A socket with nobody listening is not an error to shout about: it is a
	// receiver that is not up, and the page has a message for it.
	assert(!spotify_browse_call(path, 1, "liked", NULL, 0, &parsed, 500));
	assert(parsed.status == SPOTIFY_REPLY_NOSOCKET);

	// An answer that never comes runs into the deadline rather than hanging the
	// page's worker forever.
	fake_server_t silent = {.socket_path = path, .reply = NULL, .stay_silent = true};
	fake_server_start(&silent);
	assert(!spotify_browse_call(path, 1, "liked", NULL, 0, &parsed, 300));
	assert(parsed.status == SPOTIFY_REPLY_TIMEOUT);
	pthread_join(silent.thread, NULL);
	assert(silent.seen[0] == 'Q');

	// And a peer that closes without answering at all is a malformed reply
	// rather than a wait that ran out of time.
	fake_server_t closed = {.socket_path = path, .reply = NULL, .stay_silent = false};
	fake_server_start(&closed);
	assert(!spotify_browse_call(path, 1, "liked", NULL, 0, &parsed, 2000));
	assert(parsed.status == SPOTIFY_REPLY_MALFORMED);
	pthread_join(closed.thread, NULL);
}

// ---------------------------------------------------------------------------
// the control socket
// ---------------------------------------------------------------------------

static void test_control_datagrams(void) {
	// The service sends to a fixed path; this is the other end of it.
	unlink("/tmp/sonix-spotify.sock");
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	assert(fd >= 0);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", "/tmp/sonix-spotify.sock");
	assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

	spotify_control(SPOTIFY_CMD_PLAY_PAUSE);
	spotify_control(SPOTIFY_CMD_NEXT);
	spotify_control(SPOTIFY_CMD_PREV);
	spotify_control(SPOTIFY_CMD_PLAY);
	spotify_control(SPOTIFY_CMD_PAUSE);
	spotify_seek_ms(4200);

	char buffer[32];
	const char *expected[] = {"p", "n", "b", "P", "s", "S4200"};
	for (int i = 0; i < 6; i++) {
		ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
		assert(n == (ssize_t)strlen(expected[i]));
		buffer[n] = '\0';
		assert(strcmp(buffer, expected[i]) == 0);
	}

	close(fd);
	unlink("/tmp/sonix-spotify.sock");

	// With nobody listening the send goes nowhere and nothing blocks.
	spotify_control(SPOTIFY_CMD_NEXT);
}

// ---------------------------------------------------------------------------
// the service, with a stand-in receiver on the card
// ---------------------------------------------------------------------------

// Writes the card's receiver: a shell script that reports itself as playing,
// feeds the FIFO, and stays up until it is stopped -- the four interfaces, with
// no librespot behind them.
static void write_receiver_script(const char *status_state, bool feed_audio) {
	FILE *f = fopen(receiver_script, "w");
	assert(f);

	fprintf(f,
			"#!/bin/sh\n"
			"echo \"receiver: stand-in started\" >&2\n"
			// What the player's spawn asked of this process, recorded for the
			// suite to read back: see child_exec() in spotify.c.
			// (Written aside and renamed: the suite must never read it half written.)
			"d=$(dirname \"$0\")\n"
			"echo \"$(cat /proc/$$/oom_score_adj) $(sed 's/.*) //' /proc/$$/stat | awk '{print $17}')\" > \"$d/child_self.tmp\"\n"
			"mv \"$d/child_self.tmp\" \"$d/child_self.txt\"\n"
			"uptime_ms() { awk '{printf \"%%d\\n\", $1 * 1000}' /proc/uptime; }\n"
			"write_status() {\n"
			"  tmp=/tmp/sonix-spotify.state.tmp\n"
			"  printf 'state=%%s\\ntrack=%%s\\ntitle=%%s\\nartist=%%s\\nalbum=%%s\\nduration=%%s\\nposition=%%s\\nat=%%s\\ncover=%%s\\n' \\\n"
			"    '%s' 'spotify:track:stand-in' 'Stand-in Title' 'Stand-in Artist' 'Stand-in Album' 240000 60000 \"$(uptime_ms)\" 0 > $tmp\n"
			"  mv $tmp /tmp/sonix-spotify.state\n"
			"}\n",
			status_state);

	if (feed_audio) {
		// cat blocks in write() once the ring is full, which is exactly the
		// back-pressure librespot's pipe backend relies on.
		fprintf(f, "i=0\nwhile [ $i -lt 100 ]; do write_status; cat '%s' > /tmp/sonix-spotify.pcm; i=$((i+1)); done\n", fixture);
	} else {
		fprintf(f, "i=0\nwhile [ $i -lt 100 ]; do write_status; sleep 1; i=$((i+1)); done\n");
	}

	fclose(f);
	assert(chmod(receiver_script, 0755) == 0);
}

// A receiver that does its best to fill the tmpfs: half a megabyte of log, a
// marker under it, then a status loop that keeps it up. See log_trim().
static void write_verbose_receiver_script(void) {
	FILE *f = fopen(receiver_script, "w");
	assert(f);

	fprintf(f,
			"#!/bin/sh\n"
			"awk 'BEGIN { for (i = 0; i < 8000; i++) print \"EARLY line \" i \" 01234567890123456789012345678901234567890123456789\" }'\n"
			"echo LATE-MARKER\n"
			"i=0\nwhile [ $i -lt 100 ]; do printf 'state=playing\\n' > /tmp/sonix-spotify.state; sleep 1; i=$((i+1)); done\n");

	fclose(f);
	assert(chmod(receiver_script, 0755) == 0);
}

// 1 s of a quiet 44100 stereo ramp, so a wrong frame count or a wrong sample
// size shows up in the arithmetic rather than only in the timing.
static void write_pcm_fixture(void) {
	FILE *f = fopen(fixture, "wb");
	assert(f);
	for (int i = 0; i < 44100; i++) {
		int16_t sample = (int16_t)(i % 512);
		int16_t left = sample;
		int16_t right = (int16_t)-sample;
		assert(fwrite(&left, sizeof(left), 1, f) == 1);
		assert(fwrite(&right, sizeof(right), 1, f) == 1);
	}
	fclose(f);
}

static void test_service(void) {
	char config_file[600];
	snprintf(config_file, sizeof(config_file), "%s/device_config.ini", root);
	// A missing file is not an error: the defaults are what a first boot has.
	(void)config_init(config_file);
	(void)config_save();

	// No receiver on the card yet: the switch refuses to move.
	unlink(receiver_script);
	spotify_init(root);
	assert(!spotify_available());

	write_receiver_script("playing", true);
	assert(spotify_available());
	char expected_dir[600];
	snprintf(expected_dir, sizeof(expected_dir), "%s/.spotify", root);
	assert(strcmp(spotify_dir(), expected_dir) == 0);

	// Nothing is running before the switch, and the player is untouched.
	assert(!spotify_running() && !spotify_owns_playback());
	spotify_control(SPOTIFY_CMD_NEXT); // no socket yet: sent to nowhere

	frames_baseline = frames_now();
	spotify_set_enabled(true);
	assert(wait_until(running_true, 5.0));

	// The receiver's process carries the two answers the player's spawn gave it
	// on the way in: below the interface, and first in line to be killed.
	assert(wait_until(child_self_recorded, 5.0));
	assert_child_self();

	// The status file gets read, and the page's view of it arrives.
	assert(wait_until(frames_advanced, 8.0) == true);
	assert(audio_opens >= 1);
	assert(audio_bits == 16); // the receiver's native depth, no widening

	// The state is the receiver's own account of itself.
	assert(wait_until(state_playing, 3.0));
	spotify_state_t state;
	spotify_get_state(&state);
	assert(state.state == SPOTIFY_PLAY_PLAYING);
	assert(state.session);
	assert(strcmp(state.title, "Stand-in Title") == 0);
	assert(strcmp(state.artist, "Stand-in Artist") == 0);
	assert(strcmp(state.album, "Stand-in Album") == 0);
	assert(state.duration_ms == 240000);
	// position + the elapsed time since `at`, which the stand-in rewrites on
	// every track: 60 s and a little more.
	assert(state.position_ms >= 60000 && state.position_ms < 90000);
	assert(spotify_owns_playback());

	// The keys go to the receiver while it owns playback.
	unsigned serial = spotify_serial();
	assert(serial > 0);

	// Local playback asks for the DAC: the receiver is paused over the control
	// socket, the keys go back, and the PCM is closed within a poll.
	// The sink has to be holding the device when it is asked for it: the
	// stand-in's cat writes a second of audio and then comes back for more, and
	// a gap longer than the sink's silence rule hands the output back on its
	// own -- a release asked for then is answered "it is already free", and
	// there is no hand-back left to wait for.
	assert(wait_until(sink_holds_output_true, 3.0));
	assert(registered_release_cb != NULL);
	int pauses_before = audio_pauses;
	int ends_before = audio_ends;
	atomic_store(&local_playing, true);
	assert(registered_release_cb() == true);
	// audio_external_end comes from the playback thread, so it takes a poll.
	double end = now_secs() + 3.0;
	while (now_secs() < end) {
		pthread_mutex_lock(&audio_lock);
		bool closed = audio_ends > ends_before;
		pthread_mutex_unlock(&audio_lock);
		if (closed) {
			break;
		}
		usleep(10000);
	}
	pthread_mutex_lock(&audio_lock);
	assert(audio_ends > ends_before);
	assert(audio_pauses >= pauses_before);
	pthread_mutex_unlock(&audio_lock);
	assert(!spotify_owns_playback());
	atomic_store(&local_playing, false);

	// The browse socket is not there -- the stand-in does not answer it -- and
	// that reads as "the receiver is not running", not as a hang.
	spotify_reply_t reply;
	assert(!spotify_browse("caps", NULL, 0, &reply));
	assert(reply.status == SPOTIFY_REPLY_NOSOCKET);
	assert(!spotify_backend_version(NULL, 0));

	// The log the receiver wrote is what the page shows when it will not start.
	char tail[256];
	spotify_log_tail(tail, sizeof(tail));
	assert(strstr(tail, "stand-in started") != NULL);

	// Switching it off stops the process and hands everything back.
	spotify_set_enabled(false);
	assert(wait_until(running_false, 6.0));
	assert(!spotify_enabled());

	spotify_get_state(&state);
	assert(state.state == SPOTIFY_PLAY_NONE && !state.session);

	// A receiver that is up and sending nothing must cost the device nothing
	// either: the sink's threads wait for audio that is not coming, and the
	// control thread waits out its tick. Both waits are timed waits on statically
	// initialized condvars, on the clock those condvars are read against, and
	// this is what says so -- the playback thread asking to be real time is the
	// reason a wrong clock here is not merely a warm device but a frozen one.
	write_receiver_script("playing", false);
	spotify_set_enabled(true);
	assert(wait_until(running_true, 5.0));
	usleep(500 * 1000); // the spawn, the log and the first status poll
	double quiet_cpu_ms = cpu_ms_over(0.8);
	assert(quiet_cpu_ms < 250.0);
	spotify_set_enabled(false);
	assert(wait_until(running_false, 6.0));

	// An idling receiver stops itself when the setting says so. The stand-in
	// writes state=playing but sends no audio, which is the case the rule is
	// for: it has played, it is not playing now, and nobody is looking.
	spotify_set_idle_stop_secs(1);
	assert(spotify_idle_stop_secs() == 1);
	write_receiver_script("playing", false);
	spotify_set_enabled(true);
	assert(wait_until(running_true, 5.0));
	assert(wait_until(idle_stop_reported, 8.0));
	assert(!spotify_running());
	assert(strstr(spotify_status_text(), "idle") != NULL);
	spotify_set_idle_stop_secs(0);

	// A receiver that dies is reported rather than left as a switch that does
	// nothing: the stand-in exits at once when its loop is empty.
	FILE *f = fopen(receiver_script, "w");
	assert(f);
	fprintf(f, "#!/bin/sh\necho 'unrecognized flag --browse-socket' >&2\nexit 2\n");
	fclose(f);
	assert(chmod(receiver_script, 0755) == 0);

	spotify_set_enabled(true);
	assert(wait_until(death_reported, 8.0));
	assert(!spotify_enabled() && !spotify_running());
	assert(strstr(spotify_status_text(), "unrecognized flag") != NULL);

	// The receiver's log is on the tmpfs, which is the device's RAM. A receiver
	// that fills it is trimmed back to its own tail, once every check, and the
	// file that is left is the one that explains it.
	write_verbose_receiver_script();
	spotify_set_enabled(true);
	assert(wait_until(running_true, 5.0));
	assert(wait_until(receiver_log_kept_its_tail, 12.0)); // one 5 s check, and slack
	spotify_set_enabled(false);
	assert(wait_until(running_false, 6.0));

	// The settings round-trip through the real configuration store.
	spotify_set_bitrate(320);
	spotify_set_cache_mb(64);
	spotify_set_receiver_name("Den");
	assert(spotify_bitrate() == 320);
	assert(spotify_cache_mb() == 64);
	assert(strcmp(spotify_receiver_name(), "Den") == 0);
	spotify_set_bitrate(97); // not one of the three: the default is kept
	assert(spotify_bitrate() == 160);
	spotify_set_receiver_name("");
	assert(strcmp(spotify_receiver_name(), "Test V1") == 0);

	spotify_shutdown();
	assert(!spotify_running());
}

static int sink_pause_calls;
static void sink_pause_receiver(void) { sink_pause_calls++; }

// The sink on its own, without the service: what a FIFO writer sees, and what
// the release callback does to it.
static void test_sink(void) {
	char fifo[sizeof(root) + 32];
	snprintf(fifo, sizeof(fifo), "%s/sink.pcm", root);

	assert(spotifysink_start(fifo, sink_pause_receiver));
	assert(!spotifysink_playing());
	assert(spotifysink_silent_ms() == -1);

	// Nothing on the other end of the FIFO yet: the sink has nothing to play and
	// nothing to do, and the only thing that could make it cost anything is its
	// own wait not waiting. Measured before the writer below is started, so the
	// process's own CPU over this window is the playback thread's idea of idling.
	double idle_cpu_ms = cpu_ms_over(0.6);
	assert(idle_cpu_ms < 150.0);

	// A writer on the other end of the FIFO, in this process.
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		int fd = open(fifo, O_WRONLY);
		if (fd < 0) {
			_exit(1);
		}
		// The sink's own read end of this FIFO came across in the fork above.
		// A writer that holds a read end of the pipe it is writing into never
		// sees its write() fail: when the sink stops, this process would still
		// hold the only reader, its write() would block with the pipe full, and
		// waitpid() below would wait for it forever. The receiver's own spawn
		// sweeps its descriptors for the same reason -- see child_exec().
		for (int i = 3; i < 1024; i++) {
			if (i != fd) {
				close(i);
			}
		}
		char buffer[4096];
		memset(buffer, 0, sizeof(buffer));
		// More than the ring holds, written as fast as the FIFO takes it: the
		// ring is full, or filling, at the moment of the release below -- which
		// is the case the hand-back has to survive (see play_worker).
		for (int i = 0; i < 512; i++) { // 2 MiB
			if (write(fd, buffer, sizeof(buffer)) != (ssize_t)sizeof(buffer)) {
				_exit(1);
			}
		}
		_exit(0);
	}

	frames_baseline = frames_now();
	assert(wait_until(frames_advanced, 5.0));
	assert(spotifysink_playing());
	assert(spotifysink_silent_ms() >= 0 && spotifysink_silent_ms() < 1000);

	// Local playback wants the device: the receiver is told, and the PCM goes
	// away. (The service's own pause callback is what sends the datagram; here
	// it is counted.)
	pthread_mutex_lock(&audio_lock);
	bool open_before = audio_device_open;
	int ends_before = audio_ends;
	pthread_mutex_unlock(&audio_lock);
	assert(open_before);

	// And the player's own PCM is open from here to the end of the function:
	// the device has a local track on it, so the sink cannot take it back, and
	// "not holding the output" is a state the suite can look at rather than a
	// moment it has to catch.
	atomic_store(&local_playing, true);
	assert(spotifysink_release_output() == true);
	assert(sink_pause_calls == 1);
	double end = now_secs() + 3.0;
	while (now_secs() < end) {
		pthread_mutex_lock(&audio_lock);
		bool closed = audio_ends > ends_before;
		pthread_mutex_unlock(&audio_lock);
		if (closed) {
			break;
		}
		usleep(10000);
	}
	pthread_mutex_lock(&audio_lock);
	assert(audio_ends > ends_before);
	pthread_mutex_unlock(&audio_lock);
	assert(!spotifysink_holds_output());

	// Released while nothing is held, the answer is "the device is free":
	// local playback must never be refused for a receiver that is idle -- and
	// the receiver is not told to pause for a device it is not holding.
	assert(spotifysink_release_output() == true);
	assert(sink_pause_calls == 1);
	atomic_store(&local_playing, false);

	spotifysink_stop();
	int status = 0;
	waitpid(pid, &status, 0);
	assert(!spotifysink_playing());
	assert(access(fifo, F_OK) != 0); // the FIFO went with it
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGPIPE, SIG_IGN);

	assert(snprintf(root, sizeof(root), "/tmp/sonix-spotify-test-%d", (int)getpid()) > 0);
	char card[600];
	snprintf(card, sizeof(card), "%s/%s", root, ".spotify");
	char command[1200];
	assert(snprintf(command, sizeof(command), "rm -rf %s && mkdir -p %s", root, card) > 0);
	assert(system(command) == 0);
	snprintf(receiver_script, sizeof(receiver_script), "%s/librespot", card);
	snprintf(fixture, sizeof(fixture), "%s/pcm.raw", root);
	write_pcm_fixture();

	test_escape();
	test_request();
	test_parse();
	test_socket_call();
	test_control_datagrams();
	test_service();
	test_sink();

	// Nothing is left behind for the next run.
	assert(snprintf(command, sizeof(command), "rm -rf %s", root) > 0);
	assert(system(command) == 0);
	unlink("/tmp/sonix-spotify.state");
	unlink("/tmp/sonix-spotify.state.jpg");
	unlink("/tmp/sonix-spotify.sock");
	unlink("/tmp/sonix-spotify.browse.sock");
	unlink("/tmp/sonix-spotify.pcm");
	unlink("/tmp/sonix-spotify.log");

	puts("All Spotify integration tests passed.");
	return 0;
}
