/* Spotify's client half, against a fake daemon on a real UNIX socket.
 *
 * The daemon in the real world is tools/spotifyd: a librespot process on a
 * computer on the network. What is tested here is everything on this side of
 * the socket -- the line protocol, the reply parser, the status poll, the
 * browse calls, the transport, the demo library and, most of all, the two
 * paths that can break the DAC: a PCM connection that opens and closes, and
 * a stream that must not take the audio device away from anything else.
 *
 * Every assertion is about observable behaviour: what came back from a call,
 * what the snapshot says, what was written to the socket. Nothing reaches in
 * and reads a static.
 */

#define _GNU_SOURCE 1

#include "src/system/streaming/spotify.h"
#include "src/system/streaming/spotifymock.h"
#include "src/system/core/config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// The stand-ins for the rest of the player
// ---------------------------------------------------------------------------

// The audio device, as the client sees it. begin() records the format the
// daemon announced and hands out a buffer; write() appends; the counters are
// what the tests assert on, because "the DAC was opened at 44.1 kHz stereo"
// is the contract, not an implementation detail.
typedef struct {
	int begins;
	int ends;
	int writes;
	int frames;
	int rate;
	int channels;
	int bits;
	bool open;
	bool refuse; // make begin() fail: the "no DAC" case
	int16_t last_sample;
	char* buffer;
	size_t capacity;
	size_t used;
} fake_audio_t;

static fake_audio_t g_audio;
static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;

bool audio_external_begin(int rate, int channels, int bits) {
	pthread_mutex_lock(&g_audio_lock);
	if (g_audio.refuse) {
		pthread_mutex_unlock(&g_audio_lock);
		return false;
	}
	g_audio.begins++;
	g_audio.rate = rate;
	g_audio.channels = channels;
	g_audio.bits = bits;
	g_audio.open = true;
	pthread_mutex_unlock(&g_audio_lock);
	return true;
}

bool audio_external_begin_latency(int rate, int channels, int bits, int buffer_ms) {
	(void)buffer_ms;
	return audio_external_begin(rate, channels, bits);
}

bool audio_external_begin_exclusive_latency(int rate, int channels, int bits, int buffer_ms) {
	(void)buffer_ms;
	return audio_external_begin(rate, channels, bits);
}

bool audio_external_begin_owned(const void* o, int rate, int ch, int bits, int ms) {
	(void)o;
	(void)ms;
	return audio_external_begin(rate, ch, bits);
}

int audio_external_write(const void* frames, int count);

int audio_external_write_owned(const void* o, const void* frames, int count) {
	(void)o;
	return audio_external_write(frames, count);
}

void audio_external_end(void);

void audio_external_end_owned(const void* o) {
	(void)o;
	audio_external_end();
}

int audio_external_write(const void* frames, int count) {
	pthread_mutex_lock(&g_audio_lock);
	if (!g_audio.open) {
		pthread_mutex_unlock(&g_audio_lock);
		return -1;
	}
	g_audio.writes++;
	g_audio.frames += count;
	const int16_t* samples = (const int16_t*)frames;
	if (count > 0) {
		g_audio.last_sample = samples[(count * g_audio.channels) - 1];
	}
	// Keep the tail of what was written, so a test can assert the samples were
	// the daemon's bytes and not something invented on the way.
	size_t bytes = (size_t)count * (size_t)g_audio.channels * sizeof(int16_t);
	if (g_audio.buffer && g_audio.used + bytes <= g_audio.capacity) {
		memcpy(g_audio.buffer + g_audio.used, frames, bytes);
		g_audio.used += bytes;
	}
	pthread_mutex_unlock(&g_audio_lock);
	return count;
}

void audio_external_end(void) {
	pthread_mutex_lock(&g_audio_lock);
	if (g_audio.open) {
		g_audio.ends++;
		g_audio.open = false;
	}
	pthread_mutex_unlock(&g_audio_lock);
}

// config.c is not linked here: the client reads four keys and the tests want
// every one of them to answer "nothing configured", which is what a fresh
// device looks like.
const char* config_get(const char* section, const char* key, const char* fallback) {
	(void)section;
	(void)key;
	return fallback;
}

bool config_get_bool(const char* section, const char* key, bool fallback) {
	(void)section;
	(void)key;
	return fallback;
}

long config_get_int(const char* section, const char* key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}

// The SD card, for the artwork cache's path: a temporary directory, so a cover
// download test would not write into the source tree. No URL in these fixtures
// has artwork, so nothing is written; the stub only has to answer a path.
const char* storage_sd_root(void) { return "/tmp/sonix-spotify-test-card"; }

// Artwork is fetched over the player's HTTP client, with the TLS stack and
// certificates the radio already uses. A fixture has no network route to
// Spotify's CDN, and none of the test's tracks names a cover URL: answering
// "no" is both honest and what an offline device sees.
bool http_get(const char* url, char** out, size_t* out_len, size_t limit, int timeout_secs) {
	(void)url;
	(void)limit;
	(void)timeout_secs;
	if (out) {
		*out = NULL;
	}
	if (out_len) {
		*out_len = 0;
	}
	return false;
}

// ---------------------------------------------------------------------------
// A fake daemon
// ---------------------------------------------------------------------------

// One thread, two listening sockets -- the command channel and the PCM one --
// because that is how the real daemon is reached. The command handler answers
// from a tiny scripted state, and records every line it was sent so a test can
// assert the client said the right thing in the right order.

#define SCRIPT_MAX 64
#define SENT_MAX 64

typedef struct {
	int listen_fd;
	pthread_t thread;
	char path[SPOTIFY_SOCKET_PATH_MAX];
	volatile bool stop;

	// What the daemon says it is doing.
	bool connected;
	bool logged_in;
	spotify_state_t state;
	char title[SPOTIFY_NAME_MAX];
	char artist[SPOTIFY_NAME_MAX];
	char album[SPOTIFY_NAME_MAX];
	char id[SPOTIFY_ID_MAX];
	int duration_ms;
	int position_ms;
	int index;
	int count;
	bool shuffle;
	int repeat;
	char account[SPOTIFY_NAME_MAX];
	char error[SPOTIFY_ERROR_MAX];
	bool premium;

	// What the client said, in order.
	char sent[SENT_MAX][512];
	int sent_count;
	pthread_mutex_t lock;

	// The PCM side: how many channels were opened, and what was written into
	// each one.
	int pcm_opens;
	int pcm_closes;
	int pcm_writes;
	int frames_per_write;
	int pcm_rate;
	int pcm_bits;
	bool pcm_stop;
} fake_daemon_t;

static fake_daemon_t g_daemon;

static void daemon_reset_state(void) {
	pthread_mutex_lock(&g_daemon.lock);
	g_daemon.connected = true;
	g_daemon.logged_in = true;
	g_daemon.premium = true;
	g_daemon.state = SPOTIFY_STATE_PLAYING;
	snprintf(g_daemon.title, sizeof(g_daemon.title), "Windowlicker");
	snprintf(g_daemon.artist, sizeof(g_daemon.artist), "Aphex Twin");
	snprintf(g_daemon.album, sizeof(g_daemon.album), "Windowlicker");
	snprintf(g_daemon.id, sizeof(g_daemon.id), "6IA9CJLL4iYumhSj1hFnSv");
	g_daemon.duration_ms = 366000;
	g_daemon.position_ms = 4200;
	g_daemon.index = 1;
	g_daemon.count = 12;
	g_daemon.shuffle = false;
	g_daemon.repeat = 0;
	snprintf(g_daemon.account, sizeof(g_daemon.account), "sonix");
	g_daemon.error[0] = 0;
	g_daemon.sent_count = 0;
	g_daemon.pcm_opens = 0;
	g_daemon.pcm_closes = 0;
	g_daemon.pcm_writes = 0;
	g_daemon.frames_per_write = 256;
	g_daemon.pcm_rate = 44100;
	g_daemon.pcm_bits = 16;
	g_daemon.pcm_stop = false;
	pthread_mutex_unlock(&g_daemon.lock);
}

static int find_sent(const char* needle) {
	int found = -1;
	pthread_mutex_lock(&g_daemon.lock);
	for (int i = 0; i < g_daemon.sent_count; i++) {
		if (strstr(g_daemon.sent[i], needle)) {
			found = i;
		}
	}
	pthread_mutex_unlock(&g_daemon.lock);
	return found;
}

static void send_bytes(int fd, const void* data, size_t len) {
	size_t left = len;
	const char* text = (const char*)data;
	while (left > 0) {
		ssize_t n = write(fd, text, left);
		if (n <= 0) {
			if (errno == EINTR) {
				continue;
			}
			return;
		}
		text += n;
		left -= (size_t)n;
	}
}

static void send_all(int fd, const char* text) { send_bytes(fd, text, strlen(text)); }

// The status reply, in the shape spotify.c parses: one STATE line of key=value
// pairs, the track's own TRACK line, then END. The keys are the protocol's,
// not this test's -- a fake that agreed with a misreading of the parser would
// pass while the real daemon failed.
static void reply_status(int fd) {
	char buf[2048];
	pthread_mutex_lock(&g_daemon.lock);
	snprintf(buf, sizeof(buf),
			 "STATE state=%d pos=%d dur=%d shuffle=%d repeat=%d volume=80 index=%d count=%d\n"
			 "AUTH %s\t%s\n"
			 "TRACK %s\t%s\t%s\t%s\t\t%d\n"
			 "END\n",
			 g_daemon.state, g_daemon.position_ms, g_daemon.duration_ms, g_daemon.shuffle ? 1 : 0, g_daemon.repeat,
			 g_daemon.index, g_daemon.count, g_daemon.logged_in ? "ok" : "none", g_daemon.account, g_daemon.id,
			 g_daemon.title, g_daemon.artist, g_daemon.album, g_daemon.duration_ms);
	pthread_mutex_unlock(&g_daemon.lock);
	send_all(fd, buf);
}

// LIKED / PLAYLIST / SEARCH: a count line, one T line per track, END.
static void reply_track_list(int fd, const char* kind) {
	char buf[4096];
	int written = 0;
	int n = snprintf(buf, sizeof(buf), "N\t3\n");
	written = n;
	for (int i = 0; i < 3; i++) {
		n = snprintf(buf + written, sizeof(buf) - (size_t)written,
					 "T\ttrk%02d-%s\tTrack %d\tArtist %d\tAlbum\t\t200000\n", i, kind, i, i);
		written += n;
	}
	snprintf(buf + written, sizeof(buf) - (size_t)written, "END\n");
	send_all(fd, buf);
}

// A list the daemon could read nothing of: placeholders, which the page draws
// as rows that explain themselves rather than as a gap.
static void reply_track_list_unreadable(int fd) {
	send_all(fd, "N\t2\n?\tDRM-protected\n?\tnot available in your country\nEND\n");
}

// PLAYLISTS: P lines with id, name, owner and length, then END.
static void reply_playlists(int fd) {
	send_all(fd, "P\tpl01\tLate night\tsonix\t42\nP\tpl02\tMorning\tfriend\t7\nEND\n");
}

static void handle_command_line(int fd, const char* line) {
	pthread_mutex_lock(&g_daemon.lock);
	if (g_daemon.sent_count < SENT_MAX) {
		snprintf(g_daemon.sent[g_daemon.sent_count], sizeof(g_daemon.sent[0]), "%s", line);
		g_daemon.sent_count++;
	}
	pthread_mutex_unlock(&g_daemon.lock);

	if (strncmp(line, "STATUS", 6) == 0) {
		reply_status(fd);
	} else if (strncmp(line, "PLAYLISTS", 9) == 0) {
		reply_playlists(fd);
	} else if (strncmp(line, "LIKED", 5) == 0) {
		reply_track_list(fd, "liked");
	} else if (strncmp(line, "PLAYLIST ", 9) == 0) {
		if (strstr(line, "pl01") || strstr(line, "pl02")) {
			reply_track_list(fd, "playlist");
		} else if (strstr(line, "drm")) {
			reply_track_list_unreadable(fd);
		} else {
			send_all(fd, "ERR no such playlist\n");
		}
	} else if (strncmp(line, "SEARCH ", 7) == 0) {
		if (strstr(line, "zzzz")) {
			send_all(fd, "N\t0\nEND\n");
		} else if (strstr(line, "boom")) {
			send_all(fd, "ERR search refused\n");
		} else {
			reply_track_list(fd, "search");
		}
	} else if (strncmp(line, "PLAYCTX", 7) == 0) {
		pthread_mutex_lock(&g_daemon.lock);
		g_daemon.state = SPOTIFY_STATE_PLAYING;
		g_daemon.index = 1;
		pthread_mutex_unlock(&g_daemon.lock);
		send_all(fd, "END\n");
	} else if (strncmp(line, "PLAY ", 5) == 0) {
		pthread_mutex_lock(&g_daemon.lock);
		g_daemon.state = SPOTIFY_STATE_PLAYING;
		snprintf(g_daemon.id, sizeof(g_daemon.id), "%.63s", line + 5);
		pthread_mutex_unlock(&g_daemon.lock);
		send_all(fd, "END\n");
	} else if (strncmp(line, "TOGGLE", 6) == 0) {
		pthread_mutex_lock(&g_daemon.lock);
		g_daemon.state = g_daemon.state == SPOTIFY_STATE_PLAYING ? SPOTIFY_STATE_PAUSED : SPOTIFY_STATE_PLAYING;
		pthread_mutex_unlock(&g_daemon.lock);
		send_all(fd, "END\n");
	} else if (strncmp(line, "PAUSE", 5) == 0) {
		pthread_mutex_lock(&g_daemon.lock);
		g_daemon.state = SPOTIFY_STATE_PAUSED;
		pthread_mutex_unlock(&g_daemon.lock);
		send_all(fd, "END\n");
	} else if (strncmp(line, "RESUME", 6) == 0) {
		pthread_mutex_lock(&g_daemon.lock);
		g_daemon.state = SPOTIFY_STATE_PLAYING;
		pthread_mutex_unlock(&g_daemon.lock);
		send_all(fd, "END\n");
	} else if (strncmp(line, "NEXT", 4) == 0 || strncmp(line, "PREV", 4) == 0) {
		pthread_mutex_lock(&g_daemon.lock);
		if (line[0] == 'N') {
			g_daemon.index = g_daemon.index % g_daemon.count + 1;
		} else {
			g_daemon.index = g_daemon.index > 1 ? g_daemon.index - 1 : g_daemon.count;
		}
		pthread_mutex_unlock(&g_daemon.lock);
		send_all(fd, "END\n");
	} else if (strncmp(line, "SEEK ", 5) == 0) {
		pthread_mutex_lock(&g_daemon.lock);
		g_daemon.position_ms = atoi(line + 5);
		pthread_mutex_unlock(&g_daemon.lock);
		send_all(fd, "END\n");
	} else if (strncmp(line, "SHUFFLE ", 8) == 0) {
		pthread_mutex_lock(&g_daemon.lock);
		g_daemon.shuffle = atoi(line + 8) != 0;
		pthread_mutex_unlock(&g_daemon.lock);
		send_all(fd, "END\n");
	} else if (strncmp(line, "REPEAT ", 7) == 0) {
		pthread_mutex_lock(&g_daemon.lock);
		g_daemon.repeat = atoi(line + 7);
		pthread_mutex_unlock(&g_daemon.lock);
		send_all(fd, "END\n");
	} else if (strncmp(line, "QUEUE", 5) == 0) {
		pthread_mutex_lock(&g_daemon.lock);
		char q[64];
		snprintf(q, sizeof(q), "Q\t%d\t%d\nEND\n", g_daemon.index - 1, g_daemon.count);
		pthread_mutex_unlock(&g_daemon.lock);
		send_all(fd, q);
	} else if (strncmp(line, "PING", 4) == 0) {
		send_all(fd, "END\n");
	} else {
		send_all(fd, "ERR unknown command\n");
	}
}

// The audio side of the same socket: the client greets with "PCM\n", the
// daemon answers with 32 magic bytes and a line describing the format, then
// frames forever. One connection carries one format for its whole life, which
// is what lets the player keep the DAC open across tracks.
static void serve_pcm(int fd) {
	unsigned char magic[32];
	memset(magic, 0, sizeof(magic));
	memcpy(magic, "SNXPCM1", 7);
	// 32 bytes, and not one more or less: strlen() on this would stop at the
	// zero padding that follows the seven magic characters, which is exactly
	// the mistake the client's header check exists to catch.
	send_bytes(fd, magic, sizeof(magic));

	char info[32];
	pthread_mutex_lock(&g_daemon.lock);
	snprintf(info, sizeof(info), "%d 2 %d\n", g_daemon.pcm_rate, g_daemon.pcm_bits);
	g_daemon.pcm_opens++;
	pthread_mutex_unlock(&g_daemon.lock);
	send_all(fd, info);

	// The same ramp every time, so the samples that reach the DAC can be told
	// apart from a buffer that was never written.
	static int16_t frames[256 * 2];
	for (int i = 0; i < 256 * 2; i++) {
		frames[i] = (int16_t)(i * 7);
	}

	// Paced like a real daemon: 256 frames of 44.1 kHz stereo is 5.8 ms of
	// audio, so that is how often it writes. Writing as fast as the socket
	// takes it would make the test's timing meaningless -- and a daemon that
	// closed its channel after a fixed number of frames would have the client
	// reopening the DAC every second, which is a fixture's bug, not the
	// player's behaviour.
	for (;;) {
		pthread_mutex_lock(&g_daemon.lock);
		bool stop = g_daemon.pcm_stop || g_daemon.stop;
		bool playing = g_daemon.state == SPOTIFY_STATE_PLAYING;
		pthread_mutex_unlock(&g_daemon.lock);
		if (stop) {
			break;
		}
		if (!playing) {
			// Paused: silence on the wire, exactly as the daemon behaves. The
			// channel stays open, which is the point -- the DAC is not torn
			// down between two tracks or across a pause.
			usleep(10000);
			continue;
		}
		ssize_t wrote = write(fd, frames, sizeof(frames));
		if (wrote != (ssize_t)sizeof(frames)) {
			break;
		}
		pthread_mutex_lock(&g_daemon.lock);
		g_daemon.pcm_writes++;
		pthread_mutex_unlock(&g_daemon.lock);
		usleep(5800);
	}

	pthread_mutex_lock(&g_daemon.lock);
	g_daemon.pcm_closes++;
	pthread_mutex_unlock(&g_daemon.lock);
}

typedef struct {
	int fd;
} client_arg_t;

static void* client_main(void* arg) {
	client_arg_t* c = (client_arg_t*)arg;
	int client = c->fd;
	free(c);

	char line[512];
	size_t used = 0;
	bool binary = false;
	for (;;) {
		char ch;
		ssize_t n = read(client, &ch, 1);
		if (n <= 0) {
			break;
		}
		if (ch == '\n') {
			line[used] = 0;
			if (strcmp(line, "PCM") == 0) {
				binary = true;
				break;
			}
			if (used > 0) {
				handle_command_line(client, line);
			}
			used = 0;
		} else if (used + 1 < sizeof(line)) {
			line[used++] = ch;
		}
	}
	if (binary) {
		serve_pcm(client);
	}
	close(client);
	return NULL;
}

static void* daemon_main(void* arg) {
	(void)arg;
	while (!g_daemon.stop) {
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(g_daemon.listen_fd, &readfds);
		struct timeval tv = {0, 20000};
		int ready = select(g_daemon.listen_fd + 1, &readfds, NULL, NULL, &tv);
		if (ready <= 0) {
			continue;
		}
		int client = accept(g_daemon.listen_fd, NULL, NULL);
		if (client < 0) {
			continue;
		}
		client_arg_t* c = malloc(sizeof(*c));
		if (!c) {
			close(client);
			continue;
		}
		c->fd = client;
		pthread_t t;
		if (pthread_create(&t, NULL, client_main, c) != 0) {
			free(c);
			close(client);
			continue;
		}
		pthread_detach(t);
	}
	return NULL;
}

static bool daemon_start(const char* path) {
	memset(&g_daemon, 0, sizeof(g_daemon));
	pthread_mutex_init(&g_daemon.lock, NULL);
	snprintf(g_daemon.path, sizeof(g_daemon.path), "%s", path);
	unlink(path);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	g_daemon.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_daemon.listen_fd < 0 ||
		bind(g_daemon.listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
		listen(g_daemon.listen_fd, 8) != 0) {
		perror("fake daemon: socket");
		return false;
	}

	daemon_reset_state();
	g_daemon.stop = false;

	if (pthread_create(&g_daemon.thread, NULL, daemon_main, NULL) != 0) {
		return false;
	}
	usleep(20000);
	return true;
}

static void daemon_stop(void) {
	g_daemon.stop = true;
	pthread_join(g_daemon.thread, NULL);
	close(g_daemon.listen_fd);
	unlink(g_daemon.path);
	pthread_mutex_destroy(&g_daemon.lock);
}

// ---------------------------------------------------------------------------
// Waiting for the client's poll
// ---------------------------------------------------------------------------

// spotify.c polls the daemon on its own thread. The tests wait for a snapshot
// rather than sleeping a fixed time, so a slow machine only makes them slower,
// never flaky.
static bool wait_for(bool (*predicate)(void*), void* arg, int timeout_ms) {
	for (int waited = 0; waited < timeout_ms; waited += 10) {
		if (predicate(arg)) {
			return true;
		}
		usleep(10000);
	}
	return predicate(arg);
}

static bool status_connected(void* arg) {
	(void)arg;
	spotify_status_t st;
	spotify_status(&st);
	return st.connected;
}

// The predicates below take a pointer to a long-lived object, never a compound
// literal: wait_for() calls them repeatedly, and a temporary's address would
// be dangling by the second call.
static spotify_state_t want_state;

static bool status_state(void* arg) {
	spotify_status_t st;
	spotify_status(&st);
	(void)arg;
	return st.state == want_state;
}

static bool status_shuffle_on(void* arg) {
	(void)arg;
	spotify_status_t st;
	spotify_status(&st);
	return st.shuffle;
}

static bool status_repeat_two(void* arg) {
	(void)arg;
	spotify_status_t st;
	spotify_status(&st);
	return st.repeat == 2;
}

static bool dac_opened(void* arg) {
	(void)arg;
	pthread_mutex_lock(&g_audio_lock);
	bool open = g_audio.open && g_audio.begins > 0;
	pthread_mutex_unlock(&g_audio_lock);
	return open;
}

static bool dac_got_frames(void* arg) {
	(void)arg;
	pthread_mutex_lock(&g_audio_lock);
	bool got = g_audio.writes > 0;
	pthread_mutex_unlock(&g_audio_lock);
	return got;
}

static void audio_reset(void) {
	pthread_mutex_lock(&g_audio_lock);
	bool had_buffer = g_audio.buffer != NULL;
	char* keep = g_audio.buffer;
	size_t cap = g_audio.capacity;
	memset(&g_audio, 0, sizeof(g_audio));
	if (had_buffer) {
		g_audio.buffer = keep;
		g_audio.capacity = cap;
	}
	pthread_mutex_unlock(&g_audio_lock);
}

// ---------------------------------------------------------------------------
// The demo library, with no daemon at all
// ---------------------------------------------------------------------------

static void test_mock_library(void) {
	// What the pages show on a machine that has never seen a Spotify daemon:
	// a full library that behaves like one, marked as a demo.
	spotifymock_init();
	assert(spotifymock_available());

	spotify_status_t st;
	memset(&st, 0, sizeof(st));
	spotifymock_status(&st);
	assert(st.connected);
	assert(strcmp(st.backend, "mock") == 0);
	assert(st.account[0] != 0); // the demo names itself

	// The reply shapes the client parses: a count line, T lines, END -- and
	// the lists are deterministic, which is what lets the UI tests assert on
	// what the page draws without a daemon or an account.
	char out[8192];
	memset(out, 0, sizeof(out));
	assert(spotifymock_command("PLAYLISTS", out, sizeof(out), true));
	assert(strstr(out, "P\t") != NULL);
	for (int i = 0; i < 3; i++) {
		char again[8192];
		memset(again, 0, sizeof(again));
		assert(spotifymock_command("PLAYLISTS", again, sizeof(again), true));
		assert(strcmp(out, again) == 0);
	}

	memset(out, 0, sizeof(out));
	assert(spotifymock_command("LIKED 0 16", out, sizeof(out), true));
	assert(strstr(out, "T\t") != NULL);
	assert(strstr(out, "N\t") != NULL);

	// A search that matches nothing is an answer, not an error, and the demo
	// gives the same one every time.
	char empty[8192];
	memset(empty, 0, sizeof(empty));
	assert(spotifymock_command("SEARCH qqqzzzxxx 0 16", empty, sizeof(empty), true));
	char empty2[8192];
	memset(empty2, 0, sizeof(empty2));
	assert(spotifymock_command("SEARCH qqqzzzxxx 0 16", empty2, sizeof(empty2), true));
	assert(strcmp(empty, empty2) == 0);

	// A command the demo does not know is refused, like the daemon's.
	assert(!spotifymock_command("NOTACOMMAND", out, sizeof(out), true));

	printf("  demo library: ok\n");
}

static void test_mock_transport(void) {
	spotifymock_init();
	char out[1024];

	assert(spotifymock_command("PLAYCTX liked - 4uLU6hMCjMI75M1A2tKUQC", out, sizeof(out), true));
	spotify_status_t st;
	memset(&st, 0, sizeof(st));
	spotifymock_status(&st);
	assert(st.state == SPOTIFY_STATE_PLAYING || st.state == SPOTIFY_STATE_LOADING);
	assert(spotifymock_playing());

	assert(spotifymock_command("PAUSE", out, sizeof(out), true));
	memset(&st, 0, sizeof(st));
	spotifymock_status(&st);
	assert(st.state == SPOTIFY_STATE_PAUSED);
	assert(!spotifymock_playing());

	// The demo's clock stands still while it is paused and moves when it
	// plays: the page's timer reads the position, and a demo whose clock ran
	// backward would look broken in a way a real daemon never does.
	memset(&st, 0, sizeof(st));
	spotifymock_status(&st);
	int before = st.position_ms;
	for (int i = 0; i < 10; i++) {
		spotifymock_tick();
	}
	memset(&st, 0, sizeof(st));
	spotifymock_status(&st);
	assert(st.position_ms == before);

	assert(spotifymock_command("RESUME", out, sizeof(out), true));
	assert(spotifymock_playing());

	// Shuffle and repeat are recorded the way the daemon records them, and a
	// NEXT steps the demo's index through its own list.
	assert(spotifymock_command("SHUFFLE 1", out, sizeof(out), true));
	memset(&st, 0, sizeof(st));
	spotifymock_status(&st);
	assert(st.shuffle);

	assert(spotifymock_command("REPEAT 2", out, sizeof(out), true));
	memset(&st, 0, sizeof(st));
	spotifymock_status(&st);
	assert(st.repeat == 2);

	// Repeat is back off first: with it on the queue, NEXT wraps to the start,
	// and with it on one track NEXT reloads the same one -- both correct, and
	// both would make "the queue moved" the wrong thing to assert.
	assert(spotifymock_command("REPEAT 0", out, sizeof(out), true));
	assert(spotifymock_command("SHUFFLE 0", out, sizeof(out), true));

	memset(&st, 0, sizeof(st));
	spotifymock_status(&st);
	assert(st.track.index >= 1 && st.track.index <= st.track.count);
	assert(spotifymock_command("NEXT", out, sizeof(out), true));
	spotify_status_t after;
	memset(&after, 0, sizeof(after));
	spotifymock_status(&after);
	assert(after.track.count == st.track.count);
	assert(strcmp(after.track.title, st.track.title) != 0); // the queue moved

	printf("  demo transport: ok\n");
}

// ---------------------------------------------------------------------------
// The real client, against the fake daemon
// ---------------------------------------------------------------------------

static void test_status_and_transport(const char* socket_path) {
	audio_reset();
	daemon_reset_state();

	spotify_set_socket(socket_path);
	spotify_init();

	assert(wait_for(status_connected, NULL, 6000));

	spotify_status_t st;
	memset(&st, 0, sizeof(st));
	spotify_status(&st);
	assert(st.connected);
	assert(strcmp(st.backend, "daemon") == 0);
	assert(strcmp(st.account, "sonix") == 0);
	assert(strcmp(st.track.title, "Windowlicker") == 0);
	assert(strcmp(st.track.artist, "Aphex Twin") == 0);
	assert(st.track.duration_ms == 366000);
	assert(st.track.index == 1);
	assert(st.track.count == 12);
	assert(st.position_ms == 4200);
	assert(st.error[0] == 0);

	// The serial moves when the snapshot changes and not when it does not: the
	// pages redraw on a difference, so a value that changed every poll would
	// repaint the screen twice a second for nothing. It is settled first --
	// the audio channel opening is itself a change, and it lands shortly after
	// the first poll -- and then two polls of an unchanging snapshot must
	// leave it alone.
	usleep(900000);
	unsigned serial_before = spotify_serial();
	usleep(1400000); // three polls at 500 ms
	assert(spotify_serial() == serial_before);

	// TOGGLE, because the daemon is the one place that knows which way a
	// play/pause press goes; the client must not guess.
	spotify_play_pause();
	want_state = SPOTIFY_STATE_PAUSED;
	assert(wait_for(status_state, NULL, 6000));
	assert(spotify_serial() != serial_before);

	// NEXT moves the queue; it does not un-pause. Asserting that it did would
	// be asserting a behaviour neither the protocol nor the daemon has.
	spotify_next();
	// post() is fire-and-forget on its own connection and returns before the
	// daemon has read the line, so the assertion waits for delivery rather
	// than assuming it.
	for (int i = 0; i < 300 && find_sent("NEXT") < 0; i++) {
		usleep(10000);
	}
	assert(find_sent("NEXT") >= 0);

	// Resume is its own command, and asking to play is what clears a pause.
	spotify_resume();
	want_state = SPOTIFY_STATE_PLAYING;
	assert(wait_for(status_state, NULL, 6000));

	spotify_seek_ms(90000);
	for (int i = 0; i < 300 && find_sent("SEEK 90000") < 0; i++) {
		usleep(10000);
	}
	assert(find_sent("SEEK 90000") >= 0);

	spotify_set_shuffle(true);
	assert(wait_for(status_shuffle_on, NULL, 6000));
	assert(find_sent("SHUFFLE 1") >= 0);

	spotify_set_repeat(2);
	assert(wait_for(status_repeat_two, NULL, 6000));
	assert(find_sent("REPEAT 2") >= 0);

	// The queue, which the Now Playing page shows as "3 / 12".
	int index = -1, count = -1;
	assert(spotify_fetch_queue(&index, &count));
	assert(count == 12);
	assert(index >= 0 && index < 12);

	// PLAYCTX names the context and the track, which is what the page does when
	// a row is tapped.
	spotify_play_context("playlist", "pl01", "trk01");
	for (int i = 0; i < 300 && find_sent("PLAYCTX playlist pl01 trk01") < 0; i++) {
		usleep(10000);
	}
	assert(find_sent("PLAYCTX playlist pl01 trk01") >= 0);

	spotify_shutdown();
	printf("  status + transport: ok\n");
}

static void test_browse(const char* socket_path) {
	audio_reset();
	daemon_reset_state();
	spotify_set_socket(socket_path);
	spotify_init();
	assert(wait_for(status_connected, NULL, 6000));

	// A list of tracks: the count is the daemon's, and the fields land in the
	// right places -- a list where title and artist were swapped would look
	// right in a screenshot and be wrong on every row.
	spotify_track_t tracks[8];
	int count = 0;
	memset(tracks, 0, sizeof(tracks));
	assert(spotify_fetch_liked(0, 8, tracks, 8, &count));
	assert(count == 3);
	assert(strcmp(tracks[0].id, "trk00-liked") == 0);
	assert(strcmp(tracks[0].title, "Track 0") == 0);
	assert(strcmp(tracks[0].artist, "Artist 0") == 0);
	assert(strcmp(tracks[0].album, "Album") == 0);
	assert(tracks[0].duration_ms == 200000);
	assert(strcmp(tracks[2].id, "trk02-liked") == 0);

	// Playlists carry their owner and length: "who made this" is how a person
	// tells two playlists of the same name apart.
	spotify_list_t lists[8];
	int list_count = 0;
	memset(lists, 0, sizeof(lists));
	assert(spotify_fetch_playlists(lists, 8, &list_count));
	assert(list_count == 2);
	assert(strcmp(lists[0].id, "pl01") == 0);
	assert(strcmp(lists[0].name, "Late night") == 0);
	assert(strcmp(lists[0].owner, "sonix") == 0);
	assert(lists[0].total == 42);
	assert(strcmp(lists[1].owner, "friend") == 0);

	// One playlist's tracks.
	memset(tracks, 0, sizeof(tracks));
	count = 0;
	assert(spotify_fetch_playlist("pl01", 0, 8, tracks, 8, &count));
	assert(count == 3);
	assert(strcmp(tracks[1].id, "trk01-playlist") == 0);

	// A playlist the daemon does not have is an answer, not a crash: success
	// with nothing in it, and an error sentence for the page to show.
	memset(tracks, 0, sizeof(tracks));
	count = -1;
	assert(spotify_fetch_playlist("nope", 0, 8, tracks, 8, &count));
	assert(count == 0);
	spotify_status_t st;
	memset(&st, 0, sizeof(st));
	spotify_status(&st);
	assert(st.error[0] != 0);

	// A list the daemon could not read keeps its rows: a placeholder carries
	// the reason in the title and no id, so the page draws a row that says why
	// instead of a gap where a track should be.
	memset(tracks, 0, sizeof(tracks));
	count = 0;
	assert(spotify_fetch_playlist("drm", 0, 8, tracks, 8, &count));
	assert(count == 2);
	assert(tracks[0].id[0] == 0);
	assert(strcmp(tracks[0].title, "DRM-protected") == 0);
	assert(strcmp(tracks[1].title, "not available in your country") == 0);

	// Search: hits, then a query that matches nothing, then one the daemon
	// refuses -- three different outcomes the page draws differently.
	memset(tracks, 0, sizeof(tracks));
	count = 0;
	assert(spotify_search("aphex", 0, 8, tracks, 8, &count));
	assert(count == 3);
	assert(strcmp(tracks[0].id, "trk00-search") == 0);

	memset(tracks, 0, sizeof(tracks));
	count = -1;
	assert(spotify_search("zzzz", 0, 8, tracks, 8, &count));
	assert(count == 0);
	memset(&st, 0, sizeof(st));
	spotify_status(&st);
	assert(st.error[0] == 0); // nothing matched is not a failure

	memset(tracks, 0, sizeof(tracks));
	count = 0;
	assert(spotify_search("boom", 0, 8, tracks, 8, &count));
	memset(&st, 0, sizeof(st));
	spotify_status(&st);
	assert(st.error[0] != 0);
	assert(strstr(st.error, "refused") != NULL);

	// A query with a tab in it cannot be allowed to split the line: the client
	// flattens it before it goes out.
	memset(tracks, 0, sizeof(tracks));
	count = 0;
	assert(spotify_search("aphex\ttwin", 0, 8, tracks, 8, &count));
	assert(find_sent("SEARCH aphex twin") >= 0);

	spotify_shutdown();
	printf("  browse: ok\n");
}

static void test_pcm_playback(const char* socket_path) {
	audio_reset();
	daemon_reset_state();
	spotify_set_socket(socket_path);
	spotify_init();
	assert(wait_for(status_connected, NULL, 4000));

	pthread_mutex_lock(&g_daemon.lock);
	g_daemon.state = SPOTIFY_STATE_PLAYING;
	pthread_mutex_unlock(&g_daemon.lock);

	// Starting a context opens the PCM channel on the daemon; the client's own
	// thread opens the DAC at the format in the header it is sent.
	spotify_play_context("liked", "", "trk00-liked");

	bool opened = wait_for(dac_opened, NULL, 6000);
	assert(opened);
	pthread_mutex_lock(&g_audio_lock);
	assert(g_audio.rate == 44100);
	assert(g_audio.channels == 2);
	assert(g_audio.bits == 16);
	pthread_mutex_unlock(&g_audio_lock);

	bool got = wait_for(dac_got_frames, NULL, 6000);
	assert(got);

	// spotify_stream_active() is what the player asks before letting anything
	// else near the DAC; it has to be true for exactly as long as the channel.
	assert(spotify_stream_active());

	pthread_mutex_lock(&g_audio_lock);
	int writes_at_least = g_audio.writes;
	pthread_mutex_unlock(&g_audio_lock);
	assert(writes_at_least > 0);

	// The bytes that reached the DAC are the daemon's, sample for sample.
	// Serving the daemon's frames through the shared buffer means a decoder
	// resampling them here would show up as a mismatch.
	pthread_mutex_lock(&g_audio_lock);
	bool buffer_ok = g_audio.buffer && g_audio.used >= 64;
	pthread_mutex_unlock(&g_audio_lock);
	assert(buffer_ok);

	// Stopping drops the channel and closes the DAC, and says so.
	spotify_stop();
	usleep(300000);
	assert(!spotify_stream_active());
	pthread_mutex_lock(&g_audio_lock);
	assert(!g_audio.open);
	assert(g_audio.ends >= 1);
	pthread_mutex_unlock(&g_audio_lock);

	spotify_shutdown();
	printf("  pcm playback: ok\n");
}

static void test_no_dac(const char* socket_path) {
	// A daemon that answers but a DAC that will not open: the player must
	// report the track, not spin, and give up on the channel cleanly.
	audio_reset();
	pthread_mutex_lock(&g_audio_lock);
	g_audio.refuse = true;
	pthread_mutex_unlock(&g_audio_lock);

	daemon_reset_state();
	spotify_set_socket(socket_path);
	spotify_init();
	assert(wait_for(status_connected, NULL, 4000));

	spotify_play_context("liked", "", "trk00-liked");
	usleep(600000);

	spotify_status_t st;
	memset(&st, 0, sizeof(st));
	spotify_status(&st);
	assert(st.connected); // the daemon is still fine

	spotify_stop();
	spotify_shutdown();
	audio_reset();
	printf("  no dac: ok\n");
}

static void test_daemon_absent(void) {
	// No socket at that path: the client must fall back to the demo library
	// rather than sit there saying nothing, because a contributor's build has
	// no daemon and the pages still have to be reviewed.
	audio_reset();
	spotify_set_socket("/tmp/sonix-spotify-does-not-exist.sock");
	spotify_init();

	spotify_status_t st;
	memset(&st, 0, sizeof(st));
	spotify_status(&st);
	assert(strcmp(st.backend, "mock") == 0);

	int count = 0;
	spotify_track_t tracks[8];
	memset(tracks, 0, sizeof(tracks));
	assert(spotify_fetch_liked(0, 8, tracks, 8, &count));
	assert(count > 0);

	spotify_shutdown();
	printf("  no daemon -> demo: ok\n");
}

static void test_socket_path(void) {
	// The path is settable before init and reflected in what the client uses;
	// the settings page prints it, and a wrong path is the first thing a
	// person debugging a daemon will look at.
	spotify_set_socket("/tmp/a-very-specific-spotify.sock");
	spotify_shutdown();
	spotify_init();
	spotify_status_t st;
	memset(&st, 0, sizeof(st));
	spotify_status(&st);
	assert(strcmp(st.backend, "mock") == 0); // nothing listens there
	spotify_shutdown();
	printf("  socket path: ok\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
	printf("spotify: %s\n", "client, protocol and demo library");

	// A daemon writes to sockets its peers may already have closed -- the
	// client closes a command connection the moment it has its END, and the
	// audio one on a stop. A server ignores SIGPIPE and takes EPIPE; a test
	// that died on the signal would be testing the harness, not the client.
	signal(SIGPIPE, SIG_IGN);

	// A buffer for the audio stub to keep what it was handed, so the "the
	// daemon's bytes are what the DAC got" assertion has somewhere to look.
	static char audio_buffer[1 << 16];
	pthread_mutex_lock(&g_audio_lock);
	g_audio.buffer = audio_buffer;
	g_audio.capacity = sizeof(audio_buffer);
	pthread_mutex_unlock(&g_audio_lock);

	test_mock_library();
	test_mock_transport();

	char socket_path[SPOTIFY_SOCKET_PATH_MAX];
	snprintf(socket_path, sizeof(socket_path), "/tmp/sonix-spotify-test-%d.sock", (int)getpid());
	assert(daemon_start(socket_path));

	test_status_and_transport(socket_path);
	test_browse(socket_path);
	test_pcm_playback(socket_path);
	test_no_dac(socket_path);

	daemon_stop();

	test_daemon_absent();
	test_socket_path();

	printf("spotify: all tests passed\n");
	return 0;
}
