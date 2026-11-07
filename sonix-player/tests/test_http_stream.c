// Regression test for the radio transport freeze: a server that accepts a
// connection but never finishes its header block (or trickles it) must not be
// able to hold http_stream_open() -- and therefore transport_stop()'s join on
// the UI thread -- for ever. read_headers() is now bounded by an overall
// deadline, and the socket is published on the stream before the header read,
// so a stop can wake it.
//
// Plain C: no SDL, no ALSA, no network beyond loopback.
// Build: cc -I.. -std=gnu11 -Wall -Wextra -Werror test_http_stream.c
//                          ../src/system/net/http.c ../src/system/net/tls.c -lpthread -ldl -lm

#include "../src/system/net/http.h"

#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

const char *tr(const char *key) {
	return key;
}

static int failures;

#define CHECK(cond)                                                              \
	do {                                                                        \
		if (!(cond)) {                                                          \
			failures++;                                                         \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
		}                                                                       \
	} while (0)

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ------------------------------------------------------------------
// A test server bound to loopback port 0 (port discovered via getsockname).
// One accepted connection; then one of three behaviours selected by mode:
//
// HANG     -- never write anything (the header block never arrives)
// SLOW_OK  -- write complete headers after a one-second pause (legitimate)
// TRICKLE  -- write header fragments every three seconds, never "\r\n\r\n"
typedef enum { MODE_HANG, MODE_SLOW_OK, MODE_TRICKLE } server_mode_t;

typedef struct {
	server_mode_t mode;
	int port;              // learned from getsockname
	bool listening;        // true once listen() is active
	bool stop;
	int client_fd;         // the one accepted connection
	pthread_t thread;
} server_t;

static void *server_run(void *arg) {
	server_t *s = arg;

	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		return NULL;
	}
	int one = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(listen_fd, 1) != 0) {
		close(listen_fd);
		return NULL;
	}
	socklen_t len = sizeof(addr);
	if (getsockname(listen_fd, (struct sockaddr *)&addr, &len) != 0) {
		close(listen_fd);
		return NULL;
	}
	s->port = ntohs(addr.sin_port);
	s->listening = true;

	while (!s->stop) {
		struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
		if (poll(&pfd, 1, 200) <= 0) {
			continue;
		}
		s->client_fd = accept(listen_fd, NULL, NULL);
		if (s->client_fd < 0) {
			continue;
		}

		if (s->mode == MODE_SLOW_OK) {
			usleep(1000000); // a legitimate server that needs a moment
			const char *head =
				"HTTP/1.0 200 OK\r\n"
				"Server: test\r\n"
				"Content-Type: audio/mpeg\r\n"
				"\r\n";
			write(s->client_fd, head, strlen(head));
		} else if (s->mode == MODE_TRICKLE) {
			for (int i = 0; i < 8 && !s->stop; i++) {
				write(s->client_fd, "HTTP/1.1 ", 9);
				usleep(3000000);
			}
		}
		// MODE_HANG and the post-header phases: keep the connection open
		// without ever sending "\r\n\r\n", until the test stops us.
		while (!s->stop) {
			usleep(500000);
		}
		if (s->client_fd >= 0) {
			close(s->client_fd);
			s->client_fd = -1;
		}
		break;
	}

	close(listen_fd);
	return NULL;
}

static void server_start(server_t *s, server_mode_t mode) {
	memset(s, 0, sizeof(*s));
	s->mode = mode;
	if (pthread_create(&s->thread, NULL, server_run, s) != 0) {
		return;
	}
	while (!s->listening && !s->stop) {
		usleep(10000);
	}
}

static void server_stop(server_t *s) {
	s->stop = true;
	pthread_join(s->thread, NULL);
}

int main(void) {
	signal(SIGPIPE, SIG_IGN);

	// --------------------------------------------------------------
	// A server that accepts and never finishes its header block must
	// fail http_stream_open() at the deadline instead of hanging.
	{
		server_t s;
		server_start(&s, MODE_HANG);
		CHECK(s.port > 0);

		char url[128];
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
		http_stream_t st;
		memset(&st, 0, sizeof(st));
		long start = now_ms();
		bool ok = http_stream_open(&st, url, 10);
		long elapsed = now_ms() - start;
		CHECK(!ok);
		CHECK(st.fd < 0); // nothing leaks: no socket left belonging to the stream
		CHECK(elapsed < 15000); // bounded by the header deadline
		fprintf(stderr, "http-stream: hung headers failed after %ld ms\n", elapsed);

		server_stop(&s);
	}

	// --------------------------------------------------------------
	// A legitimately slow server that does finish its headers connects fine:
	// the deadline only gives up on headers that never arrive.
	{
		server_t s;
		server_start(&s, MODE_SLOW_OK);
		CHECK(s.port > 0);

		char url[128];
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
		http_stream_t st;
		memset(&st, 0, sizeof(st));
		long start = now_ms();
		bool ok = http_stream_open(&st, url, 10);
		long elapsed = now_ms() - start;
		CHECK(ok);
		CHECK(st.fd >= 0);
		CHECK(elapsed < 15000);
		fprintf(stderr, "http-stream: slow-but-complete headers connected after %ld ms\n",
		        elapsed);
		http_stream_close(&st);

		server_stop(&s);
	}

	// --------------------------------------------------------------
	// A server that trickles header bytes slower than the deadline also
	// fails at the deadline.
	{
		server_t s;
		server_start(&s, MODE_TRICKLE);
		CHECK(s.port > 0);

		char url[128];
		snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
		http_stream_t st;
		memset(&st, 0, sizeof(st));
		long start = now_ms();
		bool ok = http_stream_open(&st, url, 10);
		long elapsed = now_ms() - start;
		CHECK(!ok);
		CHECK(st.fd < 0);
		CHECK(elapsed < 15000);
		fprintf(stderr, "http-stream: trickle headers failed after %ld ms\n", elapsed);

		server_stop(&s);
	}

	if (failures != 0) {
		fprintf(stderr, "http-stream: %d failures\n", failures);
		return 1;
	}
	printf("ok http-stream: header-read deadline bounds a hung stream\n");
	return 0;
}
