#include "spotify.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/device/system.h"
#include "src/system/net/http.h"
#include "src/system/streaming/spotifymock.h"

// The client half of the daemon protocol described in spotify.h.
//
// Three connections to the daemon, each with its own job:
//
//   status    opened once, polled with STATUS every SPOTIFY_POLL_MS; the daemon
//             answers with a block of key=value lines.
//   command   opened per command that needs an answer the caller waits for
//             (playlists, a search); the transport keys post without waiting.
//   pcm       opened when the stream starts, read by its own thread, and never
//             mixed with either of the other two.
//
// A fourth thread fetches artwork: the daemon hands over a Spotify image URL
// and this asks for it with the player's own HTTP client, so the MIPS build
// keeps one TLS stack rather than two.
//
// Everything is blocking and each loop owns its own thread. What the interface
// reads is behind state_lock, copied out, and never held across a socket.

#define SPOTIFY_POLL_MS 500
#define SPOTIFY_COMMAND_TIMEOUT_MS 4000
#define SPOTIFY_LONG_TIMEOUT_MS 15000
#define SPOTIFY_CONNECT_TIMEOUT_MS 2000

#define SPOTIFY_LINE_MAX 4096
#define SPOTIFY_REPLY_LINES 128
#define SPOTIFY_REPLY_MAX 24576

// The PCM header: magic, then rate, channels, bits as one ASCII line, then a
// newline. Both ends must agree, and the daemon writes it before every chunk,
// so a desync is caught rather than played.
static const unsigned char PCM_MAGIC[32] = "SNXPCM1";

typedef struct {
	bool inited;
	pthread_mutex_t lock;
	pthread_t status_thread;
	pthread_t pcm_thread;
	pthread_t art_thread;
	bool threads_started;
	bool stop_threads;

	int status_fd;
	bool status_open;
	int pcm_fd;
	bool pcm_open;

	spotify_backend_t backend;
	spotify_status_t status;
	unsigned serial;

	// The PCM channel's own state. Written by the playback thread only, read
	// under the lock for the snapshot.
	bool pcm_playing;    // frames are arriving
	bool pcm_failed;     // the channel died; do not reopen until the next track
	int pcm_rate;
	int pcm_channels;
	int pcm_bits;
	bool output_open;    // audio_external_begin() succeeded
	char pcm_error[SPOTIFY_ERROR_MAX];
	int pcm_generation;  // bumped by every stop, so a stale thread exits

	// Artwork.
	char art_url[256];        // the URL the current cover came from, "" none
	char art_path[256];       // the file on the card, "" none
	char art_wanted[256];     // what the status thread last asked for
	pthread_cond_t art_cond;
	bool art_quit;

	char socket_path[SPOTIFY_SOCKET_PATH_MAX];
	char command[512];
	bool spawn_attempted;
	pid_t daemon_pid;
} spotify_ctx_t;

static spotify_ctx_t g;

// ===========================================================================
// Small helpers
// ===========================================================================

// The copy this file makes of a string, truncated at a UTF-8 boundary: half a
// character at the end of a title draws as a box, and these strings are cut
// for width, not for bytes.
static void copy_utf8(char *dst, size_t dst_size, const char *src) {
	if (!dst || dst_size == 0) {
		return;
	}
	if (!src) {
		dst[0] = '\0';
		return;
	}
	size_t n = strlen(src);
	if (n >= dst_size) {
		n = dst_size - 1;
		while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
			n--;
		}
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void set_error(const char *fmt, ...) {
	char text[SPOTIFY_ERROR_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);

	pthread_mutex_lock(&g.lock);
	if (strcmp(g.status.error, text) != 0) {
		copy_utf8(g.status.error, sizeof(g.status.error), text);
		g.serial++;
	}
	pthread_mutex_unlock(&g.lock);
}

static void clear_error_locked(void) {
	if (g.status.error[0]) {
		g.status.error[0] = '\0';
		g.serial++;
	}
}

// Whether the configured address is a network one.
//
// This is the difference that makes the feature work at all: librespot does not
// run on a 64 MB MIPS player, so spotifyd runs on a computer on the same Wi-Fi
// and the player connects to it over TCP. A bare path -- no "host:" before the
// port -- is still accepted, for a daemon that really is running on the player
// itself (a USB-ethernet dhcp run, or a test rig on the same box).
//
// "host:port" with a numeric port is a network address; anything else is a
// filesystem path. The check is a colon followed by digits to the end, because
// a UNIX path may legitimately contain a colon: /tmp/a:b/spotify.sock is a
// path, 192.168.1.20:6600 is an address.
static bool address_is_network(const char *address, char *host, size_t host_size, int *port) {
	if (!address || !address[0] || !host || !port) {
		return false;
	}
	const char *colon = strrchr(address, ':');
	if (!colon || colon == address || !colon[1]) {
		return false;
	}
	for (const char *p = colon + 1; *p; p++) {
		if (*p < '0' || *p > '9') {
			return false;
		}
	}
	int value = atoi(colon + 1);
	if (value < 1 || value > 65535) {
		return false;
	}
	size_t host_len = (size_t)(colon - address);
	if (host_len >= host_size) {
		return false;
	}
	memcpy(host, address, host_len);
	host[host_len] = '\0';
	*port = value;
	return true;
}

// A socket with connect and I/O deadlines. Unix sockets ignore SO_RCVTIMEO for
// connect (there is no handshake), but the daemon is local: a connect that
// hangs is a daemon that is wedged, which the poll reports as unavailable
// rather than waiting for. A network connect does have a handshake, and gets
// the same deadline so an asleep computer costs one poll, not a page.
static int connect_unix(const char *path) {
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		return -1;
	}
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

// getaddrinfo() rather than inet_pton(): the address may be a hostname
// ("studio-pc.local"), which is what somebody types when the DHCP lease moves.
// Both families are tried, v6 first only if it resolved to one: a player on
// IPv4-only Wi-Fi must not spend its deadline on an AAAA record.
static int connect_tcp(const char *host, int port) {
	char port_text[8];
	snprintf(port_text, sizeof(port_text), "%d", port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	struct addrinfo *list = NULL;
	if (getaddrinfo(host, port_text, &hints, &list) != 0 || !list) {
		return -1;
	}

	int fd = -1;
	for (struct addrinfo *ai = list; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0) {
			continue;
		}
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(list);
	return fd;
}

static int connect_socket(const char *address) {
	char host[SPOTIFY_SOCKET_PATH_MAX];
	int port = 0;
	int fd = -1;
	if (address_is_network(address, host, sizeof(host), &port)) {
		fd = connect_tcp(host, port);
	} else {
		fd = connect_unix(address);
	}
	if (fd < 0) {
		return -1;
	}
	struct timeval tv;
	tv.tv_sec = SPOTIFY_CONNECT_TIMEOUT_MS / 1000;
	tv.tv_usec = (SPOTIFY_CONNECT_TIMEOUT_MS % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	// Writes stay small and rare; blocking the writer briefly is cheaper than
	// a partial write that would desync a line.
	return fd;
}

// ---------------------------------------------------------------------------
// finding the daemon without being told where it is
// ---------------------------------------------------------------------------
//
// The setup step exists because nobody can spell "192.168.1.20:6600" on a
// 240-pixel keyboard with a d-pad. So the daemon answers a broadcast: it
// listens on SPOTIFY_DISCOVERY_PORT for one magic packet and replies with its
// own control port. The player sends the packet to the broadcast address, and
// the first answer wins.
//
// This is the same shape listen_together uses on the same network (see
// lt_discover_hosts): a magic u32, a broadcast, a bounded wait, and the source
// address of the reply as the answer. No mDNS: the player's resolver is glibc's
// and ".local" needs avahi, which is not in the firmware.

static int discover_daemon(char *found, size_t found_size) {
	if (!found || found_size == 0) {
		return -1;
	}
	found[0] = '\0';

	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	int bcast = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

	// The probe: the magic, and the protocol's version so a future daemon can
	// refuse an older player without the player having to know why.
	// Network byte order, like every other number that crosses this wire: the
	// player is little-endian MIPS and the daemon is whatever the person's
	// computer is. Both ends convert, so neither has to know.
	uint32_t probe[2] = {htonl(SPOTIFY_DISCOVERY_MAGIC), htonl(SPOTIFY_PROTOCOL_VERSION)};
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(SPOTIFY_DISCOVERY_PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(fd, probe, sizeof(probe), 0, (struct sockaddr *)&to, sizeof(to));

	// Two seconds: a daemon that has to be woken and has to read its cache can
	// take a moment, and this runs once, behind a spinner on the setup row.
	uint32_t answer[2] = {0, 0};
	struct sockaddr_in from;
	socklen_t from_len = sizeof(from);
	struct timeval deadline;
	deadline.tv_sec = 2;
	deadline.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));

	ssize_t got = recvfrom(fd, answer, sizeof(answer), 0, (struct sockaddr *)&from, &from_len);
	close(fd);
	if (got < (ssize_t)sizeof(answer) || ntohl(answer[0]) != SPOTIFY_DISCOVERY_MAGIC) {
		return -1;
	}
	char host[64];
	if (!inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host))) {
		return -1;
	}
	uint32_t answered_port = ntohl(answer[1]);
	int port = (int)(answered_port ? answered_port : SPOTIFY_DEFAULT_PORT);
	snprintf(found, found_size, "%s:%d", host, port);
	return 0;
}

// Finds the daemon and remembers the address, once. Called by the setup row and
// by spotify_init() when nothing has been configured: the second one is what
// makes a fresh device work with no typing at all.
bool spotify_discover(void) {
	char found[SPOTIFY_SOCKET_PATH_MAX];
	if (discover_daemon(found, sizeof(found)) != 0) {
		return false;
	}
	spotify_set_socket(found);
	config_set("spotify", "socket", found);
	config_save();
	return true;
}

static void settimeout_ms(int fd, int ms) {
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static bool send_all(int fd, const void *data, size_t len) {
	const unsigned char *p = data;
	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

// A read that may come back short, with the deadline checked by the caller.
// Returns >0 bytes, 0 on EOF, -1 on error or timeout.
static ssize_t read_some(int fd, void *buf, size_t len) {
	for (;;) {
		ssize_t n = recv(fd, buf, len, 0);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		return n;
	}
}

// A line reader over a socket, because replies are line-oriented and a recv()
// that returns two lines at once must not lose the second.
typedef struct {
	int fd;
	char buf[SPOTIFY_LINE_MAX];
	size_t len;
	size_t pos;
	bool eof;
} linereader_t;

static void lr_init(linereader_t *lr, int fd) {
	lr->fd = fd;
	lr->len = 0;
	lr->pos = 0;
	lr->eof = false;
}

// The next '\n'-terminated line, without the newline, in `out`. False on EOF,
// error or a line longer than `out_size` (which is then drained and counted as
// a blank line: a daemon with a bug must not wedge the page).
static bool lr_line(linereader_t *lr, char *out, size_t out_size) {
	for (;;) {
		// Anything left in the buffer?
		for (size_t i = lr->pos; i < lr->len; i++) {
			if (lr->buf[i] == '\n') {
				size_t n = i - lr->pos;
				size_t copy = n < out_size - 1 ? n : out_size - 1;
				memcpy(out, lr->buf + lr->pos, copy);
				out[copy] = '\0';
				lr->pos = i + 1;
				if (lr->pos == lr->len) {
					lr->pos = 0;
					lr->len = 0;
				}
				return true;
			}
		}
		// Not yet: compact and read more.
		if (lr->pos > 0) {
			memmove(lr->buf, lr->buf + lr->pos, lr->len - lr->pos);
			lr->len -= lr->pos;
			lr->pos = 0;
		}
		if (lr->len + 1 >= sizeof(lr->buf)) {
			// Forward the overflow as one empty line rather than dropping it
			// silently: the reply's END follows, and the reply parser stops
			// at END.
			lr->len = 0;
			out[0] = '\0';
			return true;
		}
		ssize_t n = read_some(lr->fd, lr->buf + lr->len, sizeof(lr->buf) - lr->len - 1);
		if (n <= 0) {
			lr->eof = true;
			return false;
		}
		lr->len += (size_t)n;
	}
}

// ===========================================================================
// Status parsing
// ===========================================================================

// "PLAYING\t12345\tshuffle=1\trepeat=0\ttrack=abc\t...", one reply, END at the
// end. Unknown keys are ignored on purpose: the daemon may grow fields and an
// old player must not choke on them.
static void parse_status_line(const char *line, spotify_status_t *st, bool *loaded) {
	// Two shapes arrive on this connection, and they disagree about the
	// separator, exactly as PROTOCOL.md says they do:
	//
	//   STATE state=3 pos=4200 dur=... key=value pairs, ONE SPACE AFTER STATE
	//   TRACK <id>\t<title>\t...\t<duration>   tab-separated fields
	//   AUTH ok\t<account>                      or "AUTH ok <account>"
	//
	// So the head is read up to the first space OR tab, and the rest of the
	// line is split by whatever came first after it. The earlier version split
	// the whole line on tabs first, which turned the STATE head into
	// "STATE state=3 pos=4200 ..." -- one token that matched neither name --
	// and left TRACK's id welded to the word TRACK. Both lines were silently
	// dropped, and a real daemon's page would have shown nothing at all.
	const char *rest = NULL;
	char head[16];
	size_t head_len = 0;
	while (line[head_len] && line[head_len] != ' ' && line[head_len] != '\t' && head_len + 1 < sizeof(head)) {
		head[head_len] = line[head_len];
		head_len++;
	}
	head[head_len] = '\0';
	rest = line + head_len;
	char sep = *rest;
	if (sep == ' ' || sep == '\t') {
		rest++;
	}

	if (strcmp(head, "STATE") == 0) {
		// key=value, separated by spaces or tabs alike: the protocol writes
		// spaces, and a daemon that echoed a tab is not worth refusing.
		char work[SPOTIFY_LINE_MAX];
		copy_utf8(work, sizeof(work), rest);
		char *save = NULL;
		for (char *tok = strtok_r(work, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
			char *eq = strchr(tok, '=');
			if (!eq) {
				continue;
			}
			*eq = '\0';
			const char *key = tok;
			const char *val = eq + 1;
			if (strcmp(key, "state") == 0) {
				st->state = (spotify_state_t)atoi(val);
			} else if (strcmp(key, "pos") == 0) {
				st->position_ms = atoi(val);
			} else if (strcmp(key, "dur") == 0) {
				st->track.duration_ms = atoi(val);
			} else if (strcmp(key, "shuffle") == 0) {
				st->shuffle = atoi(val) != 0;
			} else if (strcmp(key, "repeat") == 0) {
				st->repeat = atoi(val);
			} else if (strcmp(key, "volume") == 0) {
				st->volume = atoi(val);
			} else if (strcmp(key, "index") == 0) {
				st->track.index = atoi(val);
			} else if (strcmp(key, "count") == 0) {
				st->track.count = atoi(val);
			} else if (strcmp(key, "account") == 0) {
				copy_utf8(st->account, sizeof(st->account), val);
			} else if (strcmp(key, "error") == 0) {
				copy_utf8(st->error, sizeof(st->error), val);
			}
		}
	} else if (strcmp(head, "TRACK") == 0) {
		// TRACK <id> <title> <artist> <album> <cover> <duration_ms>, tabs.
		// The cover may be empty, which strtok_r would collapse, so the
		// fields are walked positionally instead.
		const char *p2 = rest;
		const char *fields[6] = {0};
		int n = 0;
		while (n < 6 && p2) {
			fields[n++] = p2;
			const char *tab = strchr(p2, '\t');
			if (tab) {
				// Terminate in place: `line` arrives as a mutable copy from
				// the caller's buffer, so this is the caller's own copy.
				((char *)tab)[0] = '\0';
				p2 = tab + 1;
			} else {
				p2 = NULL;
			}
		}
		if (n >= 1) {
			copy_utf8(st->track.id, sizeof(st->track.id), fields[0] ? fields[0] : "");
		}
		if (n >= 2) {
			copy_utf8(st->track.title, sizeof(st->track.title), fields[1] ? fields[1] : "");
		}
		if (n >= 3) {
			copy_utf8(st->track.artist, sizeof(st->track.artist), fields[2] ? fields[2] : "");
		}
		if (n >= 4) {
			copy_utf8(st->track.album, sizeof(st->track.album), fields[3] ? fields[3] : "");
		}
		if (n >= 5) {
			copy_utf8(st->track.cover_url, sizeof(st->track.cover_url), fields[4] ? fields[4] : "");
		}
		if (n >= 6) {
			st->track.duration_ms = atoi(fields[5] ? fields[5] : "0");
		}
		*loaded = true;
	} else if (strcmp(head, "AUTH") == 0) {
		// AUTH ok|none <account>. The verdict ends at the first separator and
		// the account is everything after it, because a display name may have
		// a space in it.
		const char *verdict = rest;
		const char *account = NULL;
		const char *sp = strpbrk(rest, " \t");
		if (sp) {
			account = sp + 1;
			while (*account == ' ' || *account == '\t') {
				account++;
			}
		}
		if (strncmp(verdict, "ok", 2) == 0) {
			st->logged_in = true;
			if (account && *account) {
				copy_utf8(st->account, sizeof(st->account), account);
			}
		}
	}
}

// Whether a value differs enough to bump the serial. The position alone must
// not: it moves five times a second and would repaint every page each tick.
// The page draws its own second hand from the status it already has.
static bool status_worth_serial(const spotify_status_t *a, const spotify_status_t *b) {
	if (a->connected != b->connected || a->logged_in != b->logged_in || a->state != b->state ||
		a->shuffle != b->shuffle || a->repeat != b->repeat || a->track.index != b->track.index ||
		a->track.count != b->track.count || a->track.duration_ms != b->track.duration_ms) {
		return true;
	}
	if (strcmp(a->track.id, b->track.id) != 0 || strcmp(a->track.title, b->track.title) != 0 ||
		strcmp(a->track.artist, b->track.artist) != 0 || strcmp(a->track.album, b->track.album) != 0 ||
		strcmp(a->track.cover_url, b->track.cover_url) != 0 || strcmp(a->error, b->error) != 0 ||
		strcmp(a->account, b->account) != 0) {
		return true;
	}
	// A pause or a resume is the state's, and so is a seek far enough to be
	// the user's rather than the clock's.
	int pa = a->position_ms;
	int pb = b->position_ms;
	if ((a->state == SPOTIFY_STATE_PLAYING) != (b->state == SPOTIFY_STATE_PLAYING)) {
		return true;
	}
	if (pa > pb + 2500 || pb > pa + 2500) {
		return true;
	}
	return false;
}

// ===========================================================================
// Artwork
// ===========================================================================

static void art_path_for(const char *url, char *out, size_t out_size) {
	unsigned long hash = 5381;
	for (const char *p = url; p && *p; p++) {
		hash = ((hash << 5) + hash) + (unsigned char)*p;
	}
	const char *root = storage_sd_root();
	if (!root || !root[0]) {
		root = ".";
	}
	snprintf(out, out_size, "%s/.local/spotify-cache/%08lx.jpg", root, hash & 0xFFFFFFFFUL);
}

static bool art_exists(const char *path) {
	struct stat st;
	return path && path[0] && stat(path, &st) == 0 && st.st_size > 0;
}

// The daemon's image URL is downloaded with the player's HTTP client: one TLS
// stack on the device, and the certificates the other services already trust.
static void art_fetch(const char *url, const char *path) {
	if (!url || !url[0] || !path || !path[0]) {
		return;
	}

	char dir[256];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		// One level at a time: the root this runs under is the card's own.
		char build[256];
		snprintf(build, sizeof(build), "%s", dir);
		for (char *p = build + 1; *p; p++) {
			if (*p == '/') {
				*p = '\0';
				mkdir(build, 0755);
				*p = '/';
			}
		}
		mkdir(build, 0755);
	}

	char *body = NULL;
	size_t len = 0;
	if (!http_get(url, &body, &len, 4 * 1024 * 1024, SPOTIFY_LONG_TIMEOUT_MS / 1000) || !body || len == 0) {
		free(body);
		return;
	}

	char tmp[300];
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		free(body);
		return;
	}
	bool ok = fwrite(body, 1, len, f) == len;
	fclose(f);
	free(body);
	if (!ok) {
		unlink(tmp);
		return;
	}
	rename(tmp, path);
}

static void *art_main(void *arg) {
	(void)arg;
	for (;;) {
		char url[256];
		char path[256];
		pthread_mutex_lock(&g.lock);
		while (!g.art_quit && (g.art_wanted[0] == '\0' || strcmp(g.art_wanted, g.art_url) == 0)) {
			pthread_cond_wait(&g.art_cond, &g.lock);
		}
		if (g.art_quit) {
			pthread_mutex_unlock(&g.lock);
			return NULL;
		}
		copy_utf8(url, sizeof(url), g.art_wanted);
		pthread_mutex_unlock(&g.lock);

		art_path_for(url, path, sizeof(path));
		if (!art_exists(path)) {
			art_fetch(url, path);
		}

		pthread_mutex_lock(&g.lock);
		// The track may have changed under the download; only the URL still
		// being the wanted one becomes current.
		if (strcmp(g.art_wanted, url) == 0) {
			copy_utf8(g.art_url, sizeof(g.art_url), url);
			copy_utf8(g.art_path, sizeof(g.art_path), art_exists(path) ? path : "");
			g.serial++;
		}
		pthread_mutex_unlock(&g.lock);
	}
}

static void art_request(const char *url) {
	// Called with g.lock held (from the status parser's commit). Signalling
	// under the lock is fine and keeps the wanted/quit pair consistent.
	if (strcmp(g.art_wanted, url) == 0) {
		return;
	}
	copy_utf8(g.art_wanted, sizeof(g.art_wanted), url);
	if (g.art_url[0] && strcmp(g.art_url, url) != 0) {
		// Nothing to show for the new track yet; the mark stands in until the
		// file lands. Keeping the old path would draw the previous track's art
		// over this one's name.
		copy_utf8(g.art_path, sizeof(g.art_path), "");
		g.art_url[0] = '\0';
	}
	pthread_cond_signal(&g.art_cond);
}

// ===========================================================================
// The status poll
// ===========================================================================

static bool status_connect(void) {
	int fd = connect_socket(g.socket_path);
	if (fd < 0) {
		return false;
	}
	pthread_mutex_lock(&g.lock);
	if (g.status_fd >= 0) {
		close(g.status_fd);
	}
	g.status_fd = fd;
	g.status_open = true;
	pthread_mutex_unlock(&g.lock);
	return true;
}

static void status_disconnect(void) {
	pthread_mutex_lock(&g.lock);
	if (g.status_fd >= 0) {
		close(g.status_fd);
		g.status_fd = -1;
	}
	g.status_open = false;
	if (g.status.connected) {
		g.status.connected = false;
		g.serial++;
	}
	pthread_mutex_unlock(&g.lock);
}

// One STATUS exchange. False when the connection is gone.
static bool status_poll_once(void) {
	int fd;
	pthread_mutex_lock(&g.lock);
	fd = g.status_fd;
	pthread_mutex_unlock(&g.lock);
	if (fd < 0) {
		return false;
	}

	static const char cmd[] = "STATUS\n";
	if (!send_all(fd, cmd, sizeof(cmd) - 1)) {
		return false;
	}

	linereader_t lr;
	lr_init(&lr, fd);

	spotify_status_t fresh;
	pthread_mutex_lock(&g.lock);
	fresh = g.status;
	pthread_mutex_unlock(&g.lock);

	bool loaded = false;
	bool saw_ok = false;
	char line[SPOTIFY_LINE_MAX];
	for (int i = 0; i < SPOTIFY_REPLY_LINES; i++) {
		if (!lr_line(&lr, line, sizeof(line))) {
			return false;
		}
		if (strncmp(line, "STATE", 5) == 0 || strncmp(line, "TRACK", 5) == 0 || strncmp(line, "AUTH", 4) == 0) {
			parse_status_line(line, &fresh, &loaded);
		} else if (strcmp(line, "END") == 0) {
			saw_ok = true;
			break;
		} else if (strncmp(line, "ERR ", 4) == 0) {
			copy_utf8(fresh.error, sizeof(fresh.error), line + 4);
			saw_ok = true;
			break;
		}
	}
	if (!saw_ok) {
		return false;
	}

	fresh.connected = true;
	if (!loaded) {
		memset(&fresh.track, 0, sizeof(fresh.track));
	}
	if (fresh.state == SPOTIFY_STATE_READY || fresh.state == SPOTIFY_STATE_UNAVAILABLE) {
		memset(&fresh.track, 0, sizeof(fresh.track));
	}
	copy_utf8(fresh.backend, sizeof(fresh.backend), g.backend == SPOTIFY_BACKEND_MOCK ? "mock" : "daemon");

	pthread_mutex_lock(&g.lock);
	if (status_worth_serial(&g.status, &fresh)) {
		g.serial++;
	}
	g.status = fresh;
	// The position is copied on every poll, but it is read from the same
	// struct the page already holds; nothing else needs a serial.
	g.status.position_ms = fresh.position_ms;
	art_request(fresh.track.cover_url);
	pthread_mutex_unlock(&g.lock);
	return true;
}

// The demo's answer to STATUS: it needs no socket, so it runs on the same
// beat as a real poll and feeds the same commit path.
static void mock_poll_once(void) {
	spotifymock_tick();

	spotify_status_t fresh;
	pthread_mutex_lock(&g.lock);
	fresh = g.status;
	pthread_mutex_unlock(&g.lock);

	spotifymock_status(&fresh);
	fresh.connected = true;
	fresh.logged_in = true;
	copy_utf8(fresh.backend, sizeof(fresh.backend), "mock");
	if (fresh.state == SPOTIFY_STATE_READY) {
		memset(&fresh.track, 0, sizeof(fresh.track));
	}

	pthread_mutex_lock(&g.lock);
	if (status_worth_serial(&g.status, &fresh)) {
		g.serial++;
	}
	g.status = fresh;
	pthread_mutex_unlock(&g.lock);
}

static bool start_daemon_if_configured(void) {
	if (!g.command[0] || g.spawn_attempted) {
		return false;
	}
	g.spawn_attempted = true;
#if defined(__EMSCRIPTEN__) || defined(HOST_WEB_EMU)
	return false;
#else
	// Executable, then its arguments, with {socket} replaced. popen-free: the
	// daemon must outlive this call and not hold a pipe open.
	char work[512];
	copy_utf8(work, sizeof(work), g.command);

	char *argv[16];
	int argc = 0;
	char *save = NULL;
	for (char *tok = strtok_r(work, " ", &save); tok && argc < 15; tok = strtok_r(NULL, " ", &save)) {
		argv[argc++] = tok;
	}
	argv[argc] = NULL;
	if (argc == 0) {
		return false;
	}

	// {socket}: a fixed expansion rather than sed, so no shell is involved.
	for (int i = 0; i < argc; i++) {
		char *brace = strstr(argv[i], "{socket}");
		if (!brace) {
			continue;
		}
		static char expanded[256];
		snprintf(expanded, sizeof(expanded), "%.*s%s%s", (int)(brace - argv[i]), argv[i], g.socket_path, brace + 8);
		argv[i] = expanded;
	}

	pid_t pid = fork();
	if (pid == 0) {
		setpgid(0, 0);
		execlp(argv[0], argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], argv[9],
			   argv[10], argv[11], argv[12], argv[13], argv[14], (char *)NULL);
		_exit(127);
	}
	if (pid < 0) {
		return false;
	}
	g.daemon_pid = pid;
	return true;
#endif
}

static void *status_main(void *arg) {
	(void)arg;
	int backoff_ms = SPOTIFY_POLL_MS;

	for (;;) {
		pthread_mutex_lock(&g.lock);
		bool quit = g.stop_threads;
		bool open = g.status_open;
		pthread_mutex_unlock(&g.lock);
		if (quit) {
			return NULL;
		}

		if (!open) {
			if (g.backend == SPOTIFY_BACKEND_MOCK) {
				mock_poll_once();
				usleep(SPOTIFY_POLL_MS * 1000);
				continue;
			}
			if (!status_connect()) {
				{
					start_daemon_if_configured();
					pthread_mutex_lock(&g.lock);
					if (g.status.connected) {
						g.status.connected = false;
						g.serial++;
					}
					pthread_mutex_unlock(&g.lock);
					// Retry faster at first: a daemon that init starts one
					// second after the UI should not take half a minute to
					// appear.
					usleep((useconds_t)backoff_ms * 1000);
					if (backoff_ms < 4000) {
						backoff_ms += 500;
					}
				}
				continue;
			}
			backoff_ms = SPOTIFY_POLL_MS;
		}

		if (g.backend == SPOTIFY_BACKEND_MOCK) {
			mock_poll_once();
			usleep(SPOTIFY_POLL_MS * 1000);
			continue;
		}
		if (!status_poll_once()) {
			status_disconnect();
			continue;
		}

		pthread_mutex_lock(&g.lock);
		quit = g.stop_threads;
		pthread_mutex_unlock(&g.lock);
		if (quit) {
			return NULL;
		}
		usleep(SPOTIFY_POLL_MS * 1000);
	}
}

// ===========================================================================
// Command connection
// ===========================================================================

// Sends one command and reads its reply. The reply's first data line is
// returned in `out` (the whole reply when `whole_reply`). True when the daemon
// ended with END; an ERR comes back false with g.status.error set.
static bool command_once(const char *command, char *out, size_t out_size, int timeout_ms, bool whole_reply) {
	if (g.backend == SPOTIFY_BACKEND_MOCK) {
		return spotifymock_command(command, out, out_size, whole_reply);
	}

	int fd = connect_socket(g.socket_path);
	if (fd < 0) {
		set_error("no daemon at %s", g.socket_path);
		return false;
	}
	settimeout_ms(fd, timeout_ms);

	char line[SPOTIFY_LINE_MAX];
	snprintf(line, sizeof(line), "%s\n", command);
	if (!send_all(fd, line, strlen(line))) {
		close(fd);
		set_error("the daemon stopped answering");
		return false;
	}

	linereader_t lr;
	lr_init(&lr, fd);
	bool ok = false;
	bool refused = false; // the daemon answered; its answer was no
	size_t used = 0;
	if (out && out_size) {
		out[0] = '\0';
	}
	for (int i = 0; i < SPOTIFY_REPLY_LINES; i++) {
		if (!lr_line(&lr, line, sizeof(line))) {
			break;
		}
		if (strcmp(line, "END") == 0) {
			ok = true;
			break;
		}
		if (strncmp(line, "ERR ", 4) == 0) {
			set_error("%s", line + 4);
			refused = true;
			break;
		}
		if (out && out_size && used + strlen(line) + 2 < out_size) {
			size_t n = strlen(line);
			memcpy(out + used, line, n);
			used += n;
			out[used++] = '\n';
			out[used] = '\0';
			if (!whole_reply) {
				// One line is what the caller wanted; drain to END so the
				// socket closes in a clean state.
				while (i < SPOTIFY_REPLY_LINES && lr_line(&lr, line, sizeof(line))) {
					if (strcmp(line, "END") == 0) {
						ok = true;
						break;
					}
				}
				break;
			}
		} else if (!whole_reply) {
			ok = true; // no room to keep it; the command still went through
			break;
		}
	}
	close(fd);
	// A refusal is an outcome, not a failure to ask: the caller gets true with
	// the error set, and an empty result, which is the shape spotify.h
	// documents for exactly this case. A transport failure is still false.
	if (ok && !refused) {
		pthread_mutex_lock(&g.lock);
		clear_error_locked();
		pthread_mutex_unlock(&g.lock);
	}
	return ok || refused;
}

// Fire-and-forget: the transport keys must not wait on the daemon to repaint a
// button, and a failed press is corrected by the next poll anyway.
static void post(const char *command) {
	if (g.backend == SPOTIFY_BACKEND_MOCK) {
		char dummy[8];
		spotifymock_command(command, dummy, sizeof(dummy), false);
		return;
	}
	int fd = connect_socket(g.socket_path);
	if (fd < 0) {
		return;
	}
	char line[SPOTIFY_LINE_MAX];
	snprintf(line, sizeof(line), "%s\n", command);
	send_all(fd, line, strlen(line));
	close(fd);
}

// ===========================================================================
// The PCM channel
// ===========================================================================

// Every byte the playback thread has read that is not a frame: the header
// after a track change, and anything else the daemon may send before audio.
// Frames are read up to this many at a time; 1024 frames is 23 ms at 44.1 kHz,
// which is what the radio's decode loop writes per iteration too.
#define PCM_CHUNK_FRAMES 1024

static void pcm_close_output_locked(void) {
	if (g.output_open) {
		audio_external_end();
		g.output_open = false;
	}
	g.pcm_playing = false;
}

static void pcm_thread_cleanup(void) {
	pthread_mutex_lock(&g.lock);
	pcm_close_output_locked();
	if (g.pcm_fd >= 0) {
		close(g.pcm_fd);
		g.pcm_fd = -1;
	}
	g.pcm_open = false;
	g.serial++;
	pthread_mutex_unlock(&g.lock);
}

static void *pcm_main(void *arg) {
	(void)arg;

	for (;;) {
		pthread_mutex_lock(&g.lock);
		int generation = g.pcm_generation;
		bool quit = g.stop_threads;
		pthread_mutex_unlock(&g.lock);
		if (quit) {
			return NULL;
		}

		if (g.backend == SPOTIFY_BACKEND_MOCK) {
			// The demo has no audio: the channel opens while it "plays" and
			// closes when it does not, so the player's Now Playing behaves --
			// and the DAC is never opened for silence.
			bool demo_playing = spotifymock_playing();
			pthread_mutex_lock(&g.lock);
			if (demo_playing != g.pcm_open) {
				g.pcm_open = demo_playing;
				g.pcm_playing = demo_playing;
				g.serial++;
			}
			pthread_mutex_unlock(&g.lock);
			usleep(SPOTIFY_POLL_MS * 1000);
			continue;
		}

		// Nothing playing means no channel, and no channel means the DAC is
		// still the local player's. Without this gate the channel opened the
		// moment the program started and held the output device for a Spotify
		// session that might never play a note -- local playback would find
		// the DAC taken by a service that was only sitting on its home page.
		// Between two tracks the state stays PLAYING or LOADING, so the
		// channel survives a track change and the DAC is not reopened; it is
		// only closed when playback actually stops or pauses for good.
		pthread_mutex_lock(&g.lock);
		spotify_state_t want = g.status.state;
		bool open_now = g.pcm_open;
		pthread_mutex_unlock(&g.lock);
		bool should_play = want == SPOTIFY_STATE_PLAYING || want == SPOTIFY_STATE_LOADING;
		if (!should_play && !open_now) {
			usleep(SPOTIFY_POLL_MS * 1000);
			continue;
		}

		int fd = connect_socket(g.socket_path);
		if (fd < 0) {
			usleep(SPOTIFY_POLL_MS * 1000);
			continue;
		}
		// No receive deadline on the audio socket: a pause is silence on the
		// wire, not a failure, and a timeout here tore down a healthy channel
		// every time a track sat paused for more than the connect deadline
		// connect_socket() had already left on the fd. Clearing it is the
		// point of this block, not a formality.
		struct timeval no_recv;
		no_recv.tv_sec = 0;
		no_recv.tv_usec = 0;
		struct timeval send_deadline;
		send_deadline.tv_sec = 30;
		send_deadline.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_recv, sizeof(no_recv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_deadline, sizeof(send_deadline));

		pthread_mutex_lock(&g.lock);
		if (g.pcm_generation != generation) {
			pthread_mutex_unlock(&g.lock);
			close(fd);
			continue;
		}
		if (g.pcm_fd >= 0) {
			close(g.pcm_fd);
		}
		g.pcm_fd = fd;
		g.pcm_open = true;
		g.pcm_failed = false;
		g.pcm_error[0] = '\0';
		g.serial++;
		pthread_mutex_unlock(&g.lock);

		static const char hello[] = "PCM\n";
		if (!send_all(fd, hello, sizeof(hello) - 1)) {
			pcm_thread_cleanup();
			usleep(SPOTIFY_POLL_MS * 1000);
			continue;
		}

		// The header: 32 magic bytes, then a line "rate channels bits\n".
		unsigned char magic[32];
		size_t got = 0;
		bool header_ok = false;
		char info[64];
		size_t info_len = 0;
		while (got < sizeof(magic)) {
			ssize_t n = read_some(fd, magic + got, sizeof(magic) - got);
			if (n <= 0) {
				break;
			}
			got += (size_t)n;
		}
		if (got == sizeof(magic) && memcmp(magic, PCM_MAGIC, sizeof(PCM_MAGIC)) == 0) {
			while (info_len + 1 < sizeof(info)) {
				char c;
				if (read_some(fd, &c, 1) != 1) {
					break;
				}
				if (c == '\n') {
					header_ok = true;
					break;
				}
				info[info_len++] = c;
			}
			info[info_len] = '\0';
		}

		if (!header_ok) {
			pthread_mutex_lock(&g.lock);
			copy_utf8(g.pcm_error, sizeof(g.pcm_error), "the Spotify stream sent a header this player cannot read");
			g.pcm_failed = true;
			g.serial++;
			pthread_mutex_unlock(&g.lock);
			pcm_thread_cleanup();
			usleep(1000 * 1000);
			continue;
		}

		int rate = 44100;
		int channels = 2;
		int bits = 16;
		sscanf(info, "%d %d %d", &rate, &channels, &bits);
		if (channels != 2) {
			channels = 2; // the daemon always sends two; a mono one is widened below
		}

		pthread_mutex_lock(&g.lock);
		g.pcm_rate = rate;
		g.pcm_channels = channels;
		g.pcm_bits = bits;
		if (g.pcm_generation != generation) {
			// A stop landed while the header was being read.
			pthread_mutex_unlock(&g.lock);
			pcm_thread_cleanup();
			continue;
		}
		// The same door the radio comes through. Local playback is stopped
		// before this is reached (spotify_page does it, the same way a station
		// does); one PCM takes one writer.
		g.output_open = audio_external_begin(rate, 2, bits);
		if (!g.output_open) {
			copy_utf8(g.pcm_error, sizeof(g.pcm_error), "the Spotify stream could not open the output");
			g.pcm_failed = true;
		}
		g.serial++;
		pthread_mutex_unlock(&g.lock);

		if (!g.output_open) {
			pcm_thread_cleanup();
			usleep(2000 * 1000);
			continue;
		}

		// Frames, until the channel ends or a stop bumps the generation.
		const int frame_bytes = bits == 24 ? 6 : (bits == 32 ? 8 : 4);
		static short pcm[PCM_CHUNK_FRAMES * 2];
		static short wide[PCM_CHUNK_FRAMES * 4];
		// One buffer, with a carry: a socket may hand back half a frame, and
		// the leftover bytes have to start the next read. Without it every
		// partial read shifted the stream by two bytes and swapped the
		// channels for the rest of the track.
		unsigned char raw[PCM_CHUNK_FRAMES * 8 + 8];
		size_t carry = 0;

		for (;;) {
			pthread_mutex_lock(&g.lock);
			bool stale = g.pcm_generation != generation;
			bool quitting = g.stop_threads;
			pthread_mutex_unlock(&g.lock);
			if (stale || quitting) {
				break;
			}

			size_t want = PCM_CHUNK_FRAMES * (size_t)frame_bytes;
			size_t have = carry;
			while (have < want) {
				ssize_t n = read_some(fd, raw + have, want - have);
				if (n < 0 && (errno == EINTR)) {
					continue;
				}
				if (n <= 0) {
					break;
				}
				have += (size_t)n;
			}
			if (have == 0) {
				break;
			}

			int frames = (int)(have / (size_t)frame_bytes);
			size_t leftover = have - (size_t)frames * (size_t)frame_bytes;
			if (leftover > 0) {
				// Hand the tail to the next read rather than dropping it.
				memmove(raw, raw + (size_t)frames * (size_t)frame_bytes, leftover);
			}
			carry = leftover;
			if (frames <= 0) {
				continue;
			}

			// First frame after a gap: say so, so the interface stops drawing
			// the loading spinner. A daemon that has gone quiet is not an
			// error: the user paused it.
			pthread_mutex_lock(&g.lock);
			bool was_playing = g.pcm_playing;
			g.pcm_playing = true;
			if (!was_playing) {
				g.serial++;
			}
			pthread_mutex_unlock(&g.lock);

			if (bits == 16) {
				memcpy(pcm, raw, (size_t)frames * 4);
				audio_external_write(pcm, frames);
			} else if (bits == 24) {
				// S24_3LE: three bytes, low first. The DAC path takes 24-bit
				// packed, so the frame is copied as it arrived.
				audio_external_write(raw, frames);
			} else {
				// Not written by this daemon today; converted rather than
				// played at the wrong scale, which is noise at full volume.
				const int32_t *src = (const int32_t *)raw;
				for (int i = 0; i < frames * 2; i++) {
					pcm[i] = (short)(src[i] >> 16);
				}
				audio_external_write(pcm, frames);
			}
			(void)wide;
		}

		pcm_thread_cleanup();
		usleep(500 * 1000);
	}
}

// ===========================================================================
// Lifecycle
// ===========================================================================

// The path spotify_set_socket() was given, kept outside `g` because init()
// zeroes that whole struct: the header documents the call as made *before*
// init, which is exactly when the zeroing would otherwise eat it. A test, or a
// settings page that has already resolved the path, sets it once and init
// honours it over the configuration.
static char g_socket_override[SPOTIFY_SOCKET_PATH_MAX];

static void read_settings(void) {
	const char *socket = config_get("spotify", "socket", NULL);
	const char *command = config_get("spotify", "daemon_command", NULL);
	if (!g.socket_path[0]) {
		copy_utf8(g.socket_path, sizeof(g.socket_path),
				  g_socket_override[0] ? g_socket_override
									   : (socket && socket[0] ? socket : SPOTIFY_DEFAULT_SOCKET));
	}
	if (command && command[0]) {
		copy_utf8(g.command, sizeof(g.command), command);
	}
}

void spotify_set_socket(const char *path) {
	if (!path) {
		return;
	}
	copy_utf8(g_socket_override, sizeof(g_socket_override), path);
	pthread_mutex_lock(&g.lock);
	copy_utf8(g.socket_path, sizeof(g.socket_path), path);
	pthread_mutex_unlock(&g.lock);
}

const char *spotify_get_socket(void) {
	if (!g.inited) {
		read_settings();
	}
	return g.inited ? g.socket_path : g_socket_override;
}

bool spotify_address_set(void) {
	const char *address = spotify_get_socket();
	// ":6600" is the default: the port is known and the host is not. Anything
	// that starts with a real host -- or a path -- counts as an address.
	if (!address || !address[0] || address[0] == ':') {
		return false;
	}
	return true;
}

void spotify_set_command(const char *command) {
	pthread_mutex_lock(&g.lock);
	copy_utf8(g.command, sizeof(g.command), command && command[0] ? command : "");
	g.spawn_attempted = false;
	pthread_mutex_unlock(&g.lock);
}

const char *spotify_get_command(void) {
	if (!g.inited) {
		read_settings();
	}
	return g.command;
}

bool spotify_init(void) {
	if (g.inited) {
		return true;
	}
	memset(&g, 0, sizeof(g));
	pthread_mutex_init(&g.lock, NULL);
	pthread_cond_init(&g.art_cond, NULL);
	g.status_fd = -1;
	g.pcm_fd = -1;
	g.status.volume = -1;
	g.backend = SPOTIFY_BACKEND_AUTO;
	g.daemon_pid = -1;

	read_settings();

	// The backend decision: an address already answering means the daemon is up
	// (init started it, or the last session left it); a configured command
	// means it can be started; otherwise the demo library answers, so the
	// pages work on a device with no Spotify at all.
	//
	// The access() short-cut is only for a filesystem path. A network address
	// has nothing to stat -- the whole point is that it is another machine --
	// and asking access() about "192.168.1.20:6600" would either fail or, worse,
	// find a file of that name in the working directory.
	char probe_host[SPOTIFY_SOCKET_PATH_MAX];
	int probe_port = 0;
	int probe = -1;
	if (address_is_network(g.socket_path, probe_host, sizeof(probe_host), &probe_port)) {
		probe = connect_socket(g.socket_path);
	} else if (access(g.socket_path, F_OK) == 0) {
		probe = connect_socket(g.socket_path);
	}

	// Nothing configured at all: look for the daemon on the LAN before giving
	// up on it. This is the step that makes a new device work without anybody
	// typing an address -- the pages then have a real library on first open,
	// and the demo only appears when the network really has no daemon on it.
	//
	// An address that was set on purpose is honoured even when nothing answers
	// it: a test that points at a dead socket is asking about the mock, and a
	// person who typed their computer's name does not want it silently
	// replaced by whatever else is on the Wi-Fi.
	if (probe < 0 && !g_socket_override[0] && !g.command[0]) {
		char found[SPOTIFY_SOCKET_PATH_MAX];
		if (discover_daemon(found, sizeof(found)) == 0) {
			// Only remember an address that is different: the discovery runs on
			// every boot and writing the same line to the config file each time
			// is a flash write for nothing.
			if (strcmp(found, g.socket_path) != 0) {
				copy_utf8(g.socket_path, sizeof(g.socket_path), found);
				copy_utf8(g_socket_override, sizeof(g_socket_override), found);
				config_set("spotify", "socket", found);
				config_save();
			}
			probe = connect_socket(g.socket_path);
		}
	}
	if (probe >= 0 || g.command[0]) {
		g.backend = SPOTIFY_BACKEND_DAEMON;
	} else {
		// AUTO has to resolve here, not stay behind as "undecided". Every
		// poll, every command and the audio thread all branch on
		// BACKEND_MOCK, and an AUTO that never became one of the two left the
		// status thread retrying a socket that was not there, forever, with
		// the pages showing an empty library instead of the demo that exists
		// for exactly this case.
		g.backend = SPOTIFY_BACKEND_MOCK;
	}
	if (probe >= 0) {
		close(probe);
	}
	if (g.backend == SPOTIFY_BACKEND_MOCK) {
		spotifymock_init();
	}

	g.inited = true;

	if (pthread_create(&g.status_thread, NULL, status_main, NULL) != 0) {
		g.inited = false;
		return false;
	}
	if (pthread_create(&g.pcm_thread, NULL, pcm_main, NULL) != 0) {
		g.stop_threads = true;
		pthread_join(g.status_thread, NULL);
		g.inited = false;
		return false;
	}
	if (pthread_create(&g.art_thread, NULL, art_main, NULL) != 0) {
		g.stop_threads = true;
		pthread_join(g.status_thread, NULL);
		pthread_join(g.pcm_thread, NULL);
		g.inited = false;
		return false;
	}
	g.threads_started = true;
	return true;
}

void spotify_shutdown(void) {
	if (!g.inited) {
		return;
	}
	pthread_mutex_lock(&g.lock);
	g.stop_threads = true;
	g.pcm_generation++;
	g.art_quit = true;
	pthread_cond_signal(&g.art_cond);
	pthread_mutex_unlock(&g.lock);

	if (g.threads_started) {
		pthread_join(g.status_thread, NULL);
		pthread_join(g.pcm_thread, NULL);
		pthread_join(g.art_thread, NULL);
	}
	status_disconnect();
	pthread_mutex_lock(&g.lock);
	pcm_close_output_locked();
	if (g.pcm_fd >= 0) {
		close(g.pcm_fd);
		g.pcm_fd = -1;
	}
	g.inited = false;
	pthread_mutex_unlock(&g.lock);
}

spotify_backend_t spotify_backend(void) { return g.backend; }

unsigned spotify_serial(void) {
	unsigned serial;
	pthread_mutex_lock(&g.lock);
	serial = g.serial;
	pthread_mutex_unlock(&g.lock);
	return serial;
}

void spotify_status(spotify_status_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&g.lock);
	*out = g.status;
	out->serial = g.serial;
	copy_utf8(out->backend, sizeof(out->backend), g.backend == SPOTIFY_BACKEND_MOCK ? "mock" : "daemon");
	// A channel that is open but has sent nothing yet is connecting, whatever
	// the daemon's last poll said: between the press and the first frame the
	// page draws a spinner, not a track that has lost its file.
	if (g.pcm_open && !g.pcm_playing && g.pcm_error[0] == '\0' && g.status.state == SPOTIFY_STATE_PLAYING) {
		if (out->state == SPOTIFY_STATE_PLAYING) {
			out->state = SPOTIFY_STATE_LOADING;
		}
	}
	pthread_mutex_unlock(&g.lock);
}

// ===========================================================================
// Transport
// ===========================================================================

void spotify_play_track(const char *track_id) {
	if (!track_id || !track_id[0]) {
		return;
	}
	char line[256];
	snprintf(line, sizeof(line), "PLAY %s", track_id);
	post(line);
}

void spotify_play_pause(void) {
	// The daemon decides which way the key goes from the state it holds, and
	// the next poll tells the interface. That is deliberate: the daemon is the
	// one place that knows whether a track is playing, paused, or between two
	// of them, and guessing here would flip the button twice.
	post("TOGGLE");
}

void spotify_next(void) { post("NEXT"); }
void spotify_prev(void) { post("PREV"); }

void spotify_seek_ms(int position_ms) {
	if (position_ms < 0) {
		position_ms = 0;
	}
	char line[64];
	snprintf(line, sizeof(line), "SEEK %d", position_ms);
	post(line);
}

void spotify_set_shuffle(bool on) { post(on ? "SHUFFLE 1" : "SHUFFLE 0"); }

void spotify_set_repeat(int mode) {
	if (mode < 0) {
		mode = 0;
	}
	if (mode > 2) {
		mode = 2;
	}
	char line[32];
	snprintf(line, sizeof(line), "REPEAT %d", mode);
	post(line);
}

void spotify_set_volume(int percent) {
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 100) {
		percent = 100;
	}
	char line[32];
	snprintf(line, sizeof(line), "VOLUME %d", percent);
	post(line);
}

void spotify_play_context(const char *kind, const char *context_id, const char *track_id) {
	if (!kind || !track_id || !track_id[0]) {
		return;
	}
	char line[512];
	if (strcmp(kind, "playlist") == 0 && context_id && context_id[0]) {
		snprintf(line, sizeof(line), "PLAYCTX playlist %s %s", context_id, track_id);
	} else if (strcmp(kind, "search") == 0) {
		snprintf(line, sizeof(line), "PLAYCTX search - %s", track_id);
	} else {
		snprintf(line, sizeof(line), "PLAYCTX liked - %s", track_id);
	}
	post(line);
}

bool spotify_command(const char *command, char *out, size_t out_size, int timeout_ms) {
	if (!g.inited) {
		return false;
	}
	if (timeout_ms <= 0) {
		timeout_ms = SPOTIFY_COMMAND_TIMEOUT_MS;
	}
	return command_once(command, out, out_size, timeout_ms, true);
}

// ===========================================================================
// Browse
// ===========================================================================

// A track line: "T <id> <title> <artist> <album> <cover> <duration_ms>", or
// "? <reason>" for one the daemon could not read (it sends the placeholder so
// the numbering in the list matches the numbering Spotify shows).
// One track line: "T\t<id>\t<title>\t<artist>\t<album>\t<cover_url>\t<duration>",
// or "?\t<reason>" for a track the daemon could not read.
//
// The fields are walked positionally rather than with strtok_r, because the
// cover URL is allowed to be empty and strtok_r collapses an empty field: a
// track with no artwork would shift the duration into the cover's place and
// play with a size of zero. The same reason the status parser does it this way.
static bool parse_track_line(const char *line, spotify_track_t *out) {
	if (!line || !line[0]) {
		return false;
	}
	if (line[0] == '?' && (line[1] == '\t' || line[1] == '\0')) {
		// A track the daemon could not read keeps its place in the list. The
		// reason goes in the title and the id stays empty, which is how the
		// page knows to draw a row that explains itself instead of one that
		// silently does nothing when tapped.
		memset(out, 0, sizeof(*out));
		const char *why = line + 1;
		while (*why == '\t') {
			why++;
		}
		copy_utf8(out->title, sizeof(out->title), why[0] ? why : "(unreadable)");
		return true;
	}
	if (line[0] != 'T' || line[1] != '\t') {
		return false;
	}

	// `line` is the caller's mutable copy; the separators are overwritten with
	// NULs so each field is a string in place.
	char work[SPOTIFY_LINE_MAX];
	copy_utf8(work, sizeof(work), line + 2);

	const char *fields[6] = {0};
	int n = 0;
	char *p = work;
	while (n < 6) {
		fields[n++] = p;
		char *tab = strchr(p, '\t');
		if (!tab) {
			break;
		}
		*tab = '\0';
		p = tab + 1;
	}

	memset(out, 0, sizeof(*out));
	copy_utf8(out->id, sizeof(out->id), fields[0] ? fields[0] : "");
	copy_utf8(out->title, sizeof(out->title), fields[1] ? fields[1] : "");
	copy_utf8(out->artist, sizeof(out->artist), fields[2] ? fields[2] : "");
	copy_utf8(out->album, sizeof(out->album), fields[3] ? fields[3] : "");
	copy_utf8(out->cover_url, sizeof(out->cover_url), fields[4] ? fields[4] : "");
	out->duration_ms = fields[5] ? atoi(fields[5]) : 0;
	return out->id[0] || out->title[0];
}

// The shared body of the four list requests: send, parse every "T" line until
// END, keep at most `capacity`. END's count is the list's own length, which is
// what the page shows as "12 of 200" without a second request.
static bool fetch_tracks(const char *command, spotify_track_t *out, int capacity, int *count) {
	if (count) {
		*count = 0;
	}
	char *reply = malloc(SPOTIFY_REPLY_MAX);
	if (!reply) {
		return false;
	}
	bool ok = command_once(command, reply, SPOTIFY_REPLY_MAX, SPOTIFY_LONG_TIMEOUT_MS, true);
	if (ok) {
		int n = 0;
		char *save = NULL;
		for (char *line = strtok_r(reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if (line[0] == 'T' && line[1] == '\t') {
				if (n < capacity) {
					if (parse_track_line(line, &out[n])) {
						n++;
					}
				}
			} else if (line[0] == '?' && line[1] == '\t') {
				if (n < capacity) {
					if (parse_track_line(line, &out[n])) {
						n++;
					}
				}
			} else if (strncmp(line, "N\t", 2) == 0) {
				if (count) {
					*count = atoi(line + 2);
				}
			}
		}
		if (count && *count == 0) {
			*count = n;
		}
	}
	free(reply);
	return ok;
}

bool spotify_fetch_liked(int offset, int limit, spotify_track_t *out, int capacity, int *count) {
	char line[128];
	snprintf(line, sizeof(line), "LIKED %d %d", offset, limit > 0 ? limit : SPOTIFY_PAGE_MAX);
	return fetch_tracks(line, out, capacity, count);
}

bool spotify_fetch_playlist(const char *playlist_id, int offset, int limit, spotify_track_t *out, int capacity,
							int *count) {
	if (!playlist_id || !playlist_id[0]) {
		return false;
	}
	char line[256];
	snprintf(line, sizeof(line), "PLAYLIST %s %d %d", playlist_id, offset, limit > 0 ? limit : SPOTIFY_PAGE_MAX);
	return fetch_tracks(line, out, capacity, count);
}

bool spotify_search(const char *query, int offset, int limit, spotify_track_t *out, int capacity, int *count) {
	if (!query || !query[0]) {
		return false;
	}
	// The query is one argument: tabs and newlines would split the line, and
	// neither belongs in a search a keyboard typed.
	char clean[256];
	size_t j = 0;
	for (size_t i = 0; query[i] && j + 1 < sizeof(clean); i++) {
		char c = query[i];
		if (c == '\t' || c == '\n' || c == '\r') {
			c = ' ';
		}
		clean[j++] = c;
	}
	clean[j] = '\0';

	char line[400];
	snprintf(line, sizeof(line), "SEARCH %s %d %d", clean, offset, limit > 0 ? limit : 50);
	return fetch_tracks(line, out, capacity, count);
}

bool spotify_fetch_queue(int *index, int *count) {
	char line[128];
	bool ok = command_once("QUEUE", line, sizeof(line), SPOTIFY_COMMAND_TIMEOUT_MS, true);
	if (!ok) {
		return false;
	}
	if (index) {
		*index = 0;
	}
	if (count) {
		*count = 0;
	}
	int a = 0, b = 0;
	if (sscanf(line, "Q %d %d", &a, &b) == 2) {
		if (index) {
			*index = a;
		}
		if (count) {
			*count = b;
		}
	}
	return true;
}

bool spotify_fetch_playlists(spotify_list_t *out, int capacity, int *count) {
	if (count) {
		*count = 0;
	}
	char *reply = malloc(SPOTIFY_REPLY_MAX);
	if (!reply) {
		return false;
	}
	bool ok = command_once("PLAYLISTS", reply, SPOTIFY_REPLY_MAX, SPOTIFY_LONG_TIMEOUT_MS, true);
	if (ok) {
		int n = 0;
		char *save = NULL;
		for (char *line = strtok_r(reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if (strncmp(line, "P\t", 2) != 0) {
				continue;
			}
			if (n >= capacity) {
				continue;
			}
			// P <id> <name> <owner> <count>, positionally: an owner may be
			// empty for a playlist built by an account that is gone, and a
			// collapsed empty field would put a count in the owner's place.
			char *fields[4] = {0};
			int nf = 0;
			char *p = line + 2;
			while (nf < 4) {
				fields[nf++] = p;
				char *tab = strchr(p, '\t');
				if (!tab) {
					break;
				}
				*tab = '\0';
				p = tab + 1;
			}
			memset(&out[n], 0, sizeof(out[n]));
			copy_utf8(out[n].id, sizeof(out[n].id), fields[0] ? fields[0] : "");
			copy_utf8(out[n].name, sizeof(out[n].name), fields[1] ? fields[1] : "");
			copy_utf8(out[n].owner, sizeof(out[n].owner), fields[2] ? fields[2] : "");
			out[n].total = fields[3] ? atoi(fields[3]) : 0;
			n++;
		}
		if (count) {
			*count = n;
		}
	}
	free(reply);
	return ok;
}

// ===========================================================================
// Playback state for the rest of the player
// ===========================================================================

bool spotify_stream_active(void) {
	bool active;
	pthread_mutex_lock(&g.lock);
	active = g.pcm_open;
	pthread_mutex_unlock(&g.lock);
	return active;
}

bool spotify_is_active(void) {
	bool active;
	pthread_mutex_lock(&g.lock);
	active = g.pcm_open && g.status.state != SPOTIFY_STATE_STOPPED && g.status.state != SPOTIFY_STATE_UNAVAILABLE;
	pthread_mutex_unlock(&g.lock);
	return active;
}

void spotify_stop(void) {
	// Bumping the generation is the stop: the playback thread sees it, closes
	// the DAC through audio_external_end() and its socket, and waits for the
	// next one. The daemon is told too, so the SESSION does not go on playing
	// into a channel nobody reads.
	//
	// The bump alone is not enough. Between two polls -- or, here, between two
	// frames -- the thread is parked in a blocking recv() that can sit for as
	// long as the daemon is silent, so it would not see the new generation
	// until the next frame arrived. shutdown() wakes it with a zero-length
	// read (recv returns 0, the loop breaks, the cleanup path runs), which is
	// what makes the stop close the DAC now rather than at the next track.
	pthread_mutex_lock(&g.lock);
	g.pcm_generation++;
	int fd = g.pcm_fd;
	pthread_mutex_unlock(&g.lock);
	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR);
	}
	post("PAUSE");
	pthread_mutex_lock(&g.lock);
	if (g.status.state == SPOTIFY_STATE_PLAYING || g.status.state == SPOTIFY_STATE_LOADING) {
		g.status.state = SPOTIFY_STATE_PAUSED;
		g.serial++;
	}
	pthread_mutex_unlock(&g.lock);
}

void spotify_resume(void) {
	pthread_mutex_lock(&g.lock);
	g.pcm_generation++;
	int fd = g.pcm_fd;
	pthread_mutex_unlock(&g.lock);
	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR); // the old channel is done; the thread reopens it
	}
	post("RESUME");
	pthread_mutex_lock(&g.lock);
	if (g.status.state == SPOTIFY_STATE_PAUSED || g.status.state == SPOTIFY_STATE_STOPPED) {
		g.status.state = SPOTIFY_STATE_LOADING;
		g.serial++;
	}
	pthread_mutex_unlock(&g.lock);
}

void spotify_get_now(spotify_now_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&g.lock);
	out->active = g.pcm_open;
	out->connecting = g.pcm_open && !g.pcm_playing && g.pcm_error[0] == '\0';
	out->state = g.status.state;
	copy_utf8(out->title, sizeof(out->title), g.status.track.title);
	copy_utf8(out->artist, sizeof(out->artist), g.status.track.artist);
	copy_utf8(out->album, sizeof(out->album), g.status.track.album);
	copy_utf8(out->cover_path, sizeof(out->cover_path), art_exists(g.art_path) ? g.art_path : "");
	out->position_ms = g.status.position_ms;
	out->duration_ms = g.status.track.duration_ms;
	out->index = g.status.track.index;
	out->count = g.status.track.count;
	pthread_mutex_unlock(&g.lock);
}

bool spotify_cover_path(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return false;
	}
	out[0] = '\0';
	pthread_mutex_lock(&g.lock);
	bool there = art_exists(g.art_path);
	if (there) {
		copy_utf8(out, out_size, g.art_path);
	}
	pthread_mutex_unlock(&g.lock);
	return there;
}
