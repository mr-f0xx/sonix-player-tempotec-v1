#include "spotifybrowse.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// The version line, and the only thing about the protocol that is not a field.
// A reply that does not start with it is not one.
#define SPOTIFY_MAGIC "Q2SPOT/1\n"
#define SPOTIFY_MAGIC_LEN 9

static int hex_value(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	char lower = (char)(c | 32);
	if (lower >= 'a' && lower <= 'f') {
		return lower - 'a' + 10;
	}
	return -1;
}

static bool escaped(char c) {
	return c == '%' || c == '\t' || c == '\n' || c == '\r';
}

int spotify_escape(const char *in, char *out, size_t size) {
	static const char digits[] = "0123456789ABCDEF";

	size_t k = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		// The terminator's place is reserved before anything is written, so a
		// field that would not fit leaves `out` alone as far as the caller can
		// tell (it only ever looks when the return is not -1).
		size_t need = escaped((char)*p) ? 3u : 1u;
		if (k + need + 1 > size) {
			return -1;
		}
		if (need == 3) {
			out[k++] = '%';
			out[k++] = digits[*p >> 4];
			out[k++] = digits[*p & 15];
		} else {
			out[k++] = (char)*p;
		}
	}
	if (size == 0) {
		return -1;
	}
	out[k] = '\0';
	return (int)k;
}

int spotify_unescape(const char *in, size_t in_len, char *out, size_t size) {
	size_t k = 0;

	for (size_t i = 0; i < in_len;) {
		unsigned char c = (unsigned char)in[i];
		if (c == '%') {
			// A '%' not followed by two hex digits is malformed, not a literal
			// percent: the protocol says the escape is the only way one is
			// carried, so this is a broken field rather than a strange name.
			if (i + 3 > in_len) {
				return -1;
			}
			int hi = hex_value(in[i + 1]);
			int lo = hex_value(in[i + 2]);
			if (hi < 0 || lo < 0) {
				return -1;
			}
			c = (unsigned char)((hi << 4) | lo);
			i += 3;
			// A decoded NUL would end the field early, and every caller here
			// works on C strings: refuse rather than hand back something that
			// silently stops in the middle.
			if (c == '\0') {
				return -1;
			}
		} else {
			i++;
			if (c == '\0') {
				return -1;
			}
		}

		if (k + 1 >= size) {
			return -1;
		}
		out[k++] = (char)c;
	}

	if (size == 0) {
		return -1;
	}
	out[k] = '\0';
	return (int)k;
}

// The same, for display text: an over-long title is cut rather than refused,
// because it is a label and not an identifier. Cut on a character boundary --
// half a UTF-8 sequence is a row of replacement boxes on the screen.
static void unescape_text(const char *in, size_t in_len, char *out, size_t size) {
	unsigned char *dst = (unsigned char *)out;
	size_t k = 0;

	for (size_t i = 0; i < in_len && k + 1 < size;) {
		unsigned char c = (unsigned char)in[i];
		if (c == '%') {
			if (i + 2 >= in_len) {
				break;
			}
			int hi = hex_value(in[i + 1]);
			int lo = hex_value(in[i + 2]);
			if (hi < 0 || lo < 0) {
				break;
			}
			c = (unsigned char)((hi << 4) | lo);
			i += 3;
			if (c == '\0') {
				break;
			}
		} else {
			i++;
			if (c == '\0') {
				break;
			}
		}
		dst[k++] = c;
	}

	// Back off over any continuation bytes the cut landed in, then over the
	// lead byte that introduced them, so what is left is whole characters.
	while (k > 0 && (dst[k - 1] & 0xC0) == 0x80) {
		k--;
	}
	if (k > 0 && dst[k - 1] >= 0xC0) {
		k--;
	}

	dst[k] = '\0';
}

int spotify_request_build(char *out, size_t size, unsigned id, const char *op, const char *const *args, int nargs) {
	if (nargs < 0) {
		return -1;
	}

	size_t k = 0;
	if (sizeof(SPOTIFY_MAGIC) > size) {
		return -1;
	}
	memcpy(out, SPOTIFY_MAGIC, SPOTIFY_MAGIC_LEN);
	k += SPOTIFY_MAGIC_LEN;

	int written = snprintf(out + k, size - k, "%u\t", id);
	if (written < 0 || (size_t)written >= size - k) {
		return -1;
	}
	k += (size_t)written;

	if (spotify_escape(op, out + k, size - k) < 0) {
		return -1;
	}
	k += strlen(out + k);

	for (int i = 0; i < nargs; i++) {
		if (k + 1 >= size) {
			return -1;
		}
		out[k++] = '\t';
		out[k] = '\0';
		if (spotify_escape(args[i], out + k, size - k) < 0) {
			return -1;
		}
		k += strlen(out + k);
	}

	if (k + 1 >= size) {
		return -1;
	}
	out[k++] = '\n';
	out[k] = '\0';

	if (k > SPOTIFY_REQ_MAX) {
		return -1;
	}
	return (int)k;
}

static spotify_row_kind_t kind_of(const char *name, size_t len) {
	if (len == 5 && memcmp(name, "track", 5) == 0) {
		return SPOTIFY_ROW_TRACK;
	}
	if (len == 5 && memcmp(name, "album", 5) == 0) {
		return SPOTIFY_ROW_ALBUM;
	}
	if (len == 6 && memcmp(name, "artist", 6) == 0) {
		return SPOTIFY_ROW_ARTIST;
	}
	if (len == 8 && memcmp(name, "playlist", 8) == 0) {
		return SPOTIFY_ROW_PLAYLIST;
	}
	if (len == 7 && memcmp(name, "account", 7) == 0) {
		return SPOTIFY_ROW_ACCOUNT;
	}
	if (len == 7 && memcmp(name, "setting", 7) == 0) {
		return SPOTIFY_ROW_SETTING;
	}
	if (len == 2 && memcmp(name, "op", 2) == 0) {
		return SPOTIFY_ROW_OP;
	}
	return SPOTIFY_ROW_UNKNOWN;
}

static bool status_of(const char *name, size_t len, spotify_reply_status_t *out) {
	static const struct {
		const char *name;
		spotify_reply_status_t status;
	} table[] = {
		{"ok", SPOTIFY_REPLY_OK},
		{"login", SPOTIFY_REPLY_LOGIN},
		{"offline", SPOTIFY_REPLY_OFFLINE},
		{"premium", SPOTIFY_REPLY_PREMIUM},
		{"refused", SPOTIFY_REPLY_REFUSED},
		{"unavailable", SPOTIFY_REPLY_UNAVAILABLE},
		{"stale", SPOTIFY_REPLY_STALE},
		{"throttled", SPOTIFY_REPLY_THROTTLED},
		{"unsupported", SPOTIFY_REPLY_UNSUPPORTED},
		{"bad", SPOTIFY_REPLY_BAD},
		{"error", SPOTIFY_REPLY_ERROR},
	};

	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (strlen(table[i].name) == len && memcmp(table[i].name, name, len) == 0) {
			*out = table[i].status;
			return true;
		}
	}
	return false;
}

const char *spotify_reply_status_name(spotify_reply_status_t status) {
	switch (status) {
	case SPOTIFY_REPLY_OK:
		return "ok";
	case SPOTIFY_REPLY_LOGIN:
		return "login";
	case SPOTIFY_REPLY_OFFLINE:
		return "offline";
	case SPOTIFY_REPLY_PREMIUM:
		return "premium";
	case SPOTIFY_REPLY_REFUSED:
		return "refused";
	case SPOTIFY_REPLY_UNAVAILABLE:
		return "unavailable";
	case SPOTIFY_REPLY_STALE:
		return "stale";
	case SPOTIFY_REPLY_THROTTLED:
		return "throttled";
	case SPOTIFY_REPLY_UNSUPPORTED:
		return "unsupported";
	case SPOTIFY_REPLY_BAD:
		return "bad";
	case SPOTIFY_REPLY_ERROR:
		return "error";
	case SPOTIFY_REPLY_MALFORMED:
		return "malformed";
	case SPOTIFY_REPLY_NOSOCKET:
		return "no socket";
	case SPOTIFY_REPLY_TIMEOUT:
		return "timeout";
	}
	return "?";
}

// A field reader over one line: `fld`/`len` come back pointing at the run
// between two tabs (or between a tab and the line's end).
typedef struct {
	const char *at;
	const char *end;
} line_t;

static bool next_field(line_t *line, const char **fld, size_t *len) {
	if (line->at > line->end) {
		return false;
	}

	const char *start = line->at;
	const char *stop = start;
	while (stop < line->end && *stop != '\t') {
		stop++;
	}

	*fld = start;
	*len = (size_t)(stop - start);
	line->at = stop < line->end ? stop + 1 : line->end + 1;
	return true;
}

bool spotify_reply_parse(const char *buf, size_t len, unsigned want_id, spotify_reply_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->status = SPOTIFY_REPLY_MALFORMED;

	if (len < SPOTIFY_MAGIC_LEN + 2 || memcmp(buf, SPOTIFY_MAGIC, SPOTIFY_MAGIC_LEN) != 0) {
		snprintf(out->message, sizeof(out->message), "not a Q2SPOT/1 reply");
		return false;
	}
	// The whole reply is handled as text, so an embedded NUL would silently
	// shorten it -- a truncated row list read as a whole one. Refuse instead.
	if (memchr(buf, '\0', len) != NULL) {
		snprintf(out->message, sizeof(out->message), "the reply carries a NUL byte");
		return false;
	}

	const char *p = buf + SPOTIFY_MAGIC_LEN;
	const char *end = buf + len;
	bool have_header = false;

	while (p < end) {
		// Every line ends with a newline, but the last one may not: a reader
		// that stops at the byte before it would drop the final row of a page
		// for no better reason than how the writer flushed. It still has to be
		// a whole row -- the field count below is what catches a reply that was
		// really cut off.
		const char *newline = memchr(p, '\n', (size_t)(end - p));
		const char *line_end = newline ? newline : end;

		line_t line = {.at = p, .end = line_end};
		const char *fld;
		size_t flen;
		int fields = 0;

		if (!have_header) {
			char id_buf[16];
			unsigned long id = 0;
			bool bad = false;

			// Four fields: the request's number, the status, a message and a
			// cursor. A late answer -- someone else's number -- is dropped,
			// which is what the number is for.
			while (next_field(&line, &fld, &flen)) {
				if (fields == 0) {
					if (flen == 0 || flen >= sizeof(id_buf)) {
						bad = true;
						break;
					}
					memcpy(id_buf, fld, flen);
					id_buf[flen] = '\0';
					char *stop = NULL;
					id = strtoul(id_buf, &stop, 10);
					if (!stop || *stop != '\0') {
						bad = true;
						break;
					}
				} else if (fields == 1) {
					bad = !status_of(fld, flen, &out->status);
					if (bad) {
						break;
					}
				} else if (fields == 2) {
					unescape_text(fld, flen, out->message, sizeof(out->message));
				} else if (fields == 3) {
					if (spotify_unescape(fld, flen, out->cursor, sizeof(out->cursor)) < 0) {
						snprintf(out->message, sizeof(out->message), "the reply's cursor is malformed");
						out->status = SPOTIFY_REPLY_MALFORMED;
						return false;
					}
				}
				fields++;
			}

			if (bad || fields != 4) {
				snprintf(out->message, sizeof(out->message), "the reply's header is malformed");
				out->status = SPOTIFY_REPLY_MALFORMED;
				return false;
			}
			if (id != (unsigned long)want_id) {
				snprintf(out->message, sizeof(out->message), "a late answer to request %lu", id);
				out->status = SPOTIFY_REPLY_MALFORMED;
				return false;
			}
			have_header = true;
			p = newline ? line_end + 1 : end;
			continue;
		}

		if (out->count >= SPOTIFY_ROWS_MAX) {
			snprintf(out->message, sizeof(out->message), "the reply carries more than %d rows", SPOTIFY_ROWS_MAX);
			out->status = SPOTIFY_REPLY_MALFORMED;
			return false;
		}

		// Six fields: what the row is, its URI, the two labels, and where it
		// sits in the list it came from (context and position), which is what a
		// play request needs to start it in its own list.
		spotify_row_t *row = &out->rows[out->count];
		fields = 0;
		bool bad = false;
		while (next_field(&line, &fld, &flen)) {
			switch (fields) {
			case 0:
				row->kind = kind_of(fld, flen);
				break;
			case 1:
				if (spotify_unescape(fld, flen, row->uri, sizeof(row->uri)) < 0) {
					bad = true;
				}
				break;
			case 2:
				unescape_text(fld, flen, row->title, sizeof(row->title));
				break;
			case 3:
				unescape_text(fld, flen, row->subtitle, sizeof(row->subtitle));
				break;
			case 4:
				if (spotify_unescape(fld, flen, row->context, sizeof(row->context)) < 0) {
					bad = true;
				}
				break;
			case 5:
				if (spotify_unescape(fld, flen, row->position, sizeof(row->position)) < 0) {
					bad = true;
				}
				break;
			default:
				bad = true; // a seventh field: not a row
				break;
			}
			if (bad) {
				break;
			}
			fields++;
		}

		if (bad || fields != 6) {
			snprintf(out->message, sizeof(out->message), "row %d is malformed", out->count);
			out->status = SPOTIFY_REPLY_MALFORMED;
			return false;
		}
		out->count++;
		p = newline ? line_end + 1 : end;
	}

	if (!have_header) {
		snprintf(out->message, sizeof(out->message), "the reply has no header");
		out->status = SPOTIFY_REPLY_MALFORMED;
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// the conversation
// ---------------------------------------------------------------------------

// Deadline in milliseconds from now.
static void deadline_in(struct timespec *out, int ms) {
	clock_gettime(CLOCK_MONOTONIC, out);
	out->tv_sec += ms / 1000;
	out->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (out->tv_nsec >= 1000000000L) {
		out->tv_nsec -= 1000000000L;
		out->tv_sec++;
	}
}

static int remaining_ms(const struct timespec *until) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (long)(until->tv_sec - now.tv_sec) * 1000 + (until->tv_nsec - now.tv_nsec) / 1000000;
	return ms > 0 ? (int)ms : 0;
}

// What to wait for, or -1 when the deadline has passed or poll() failed.
static short poll_for(int fd, short events, const struct timespec *until) {
	struct pollfd p = {.fd = fd, .events = events};
	int r = poll(&p, 1, remaining_ms(until));
	if (r <= 0) {
		return -1;
	}
	return p.revents;
}

// Writes everything, or nothing (the request is small: the socket takes it or
// the peer is gone).
static bool write_all(int fd, const char *data, size_t len, const struct timespec *until) {
	size_t done = 0;
	while (done < len) {
		short revents = poll_for(fd, POLLOUT, until);
		if (revents < 0 || (revents & (POLLERR | POLLHUP | POLLNVAL))) {
			return false;
		}
		ssize_t n = send(fd, data + done, len - done, MSG_NOSIGNAL);
		if (n > 0) {
			done += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			continue;
		}
		return false;
	}
	return true;
}

static int read_reply(int fd, char *buf, size_t size, const struct timespec *until) {
	size_t done = 0;
	for (;;) {
		short revents = poll_for(fd, POLLIN, until);
		if (revents < 0) {
			return -1; // the deadline passed
		}
		if (revents & (POLLERR | POLLNVAL)) {
			return -1;
		}

		if (done + 1 >= size) {
			return -1; // larger than any legal reply: refuse rather than cut
		}
		ssize_t n = recv(fd, buf + done, size - done - 1, 0);
		if (n > 0) {
			done += (size_t)n;
			continue;
		}
		if (n == 0) {
			break; // the backend answered and closed, which is the protocol
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			continue;
		}
		return -1;
	}
	buf[done] = '\0';
	return (int)done;
}

bool spotify_browse_call(const char *socket_path, unsigned id, const char *op, const char *const *args, int nargs,
						 spotify_reply_t *out, int timeout_ms) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->status = SPOTIFY_REPLY_NOSOCKET;
	snprintf(out->message, sizeof(out->message), "the receiver is not running");

	if (!socket_path || !socket_path[0] || timeout_ms <= 0) {
		return false;
	}

	char request[SPOTIFY_REQ_MAX];
	int request_len = spotify_request_build(request, sizeof(request), id, op, args, nargs);
	if (request_len < 0) {
		out->status = SPOTIFY_REPLY_BAD;
		snprintf(out->message, sizeof(out->message), "the request does not fit");
		return false;
	}

	struct timespec until;
	deadline_in(&until, timeout_ms);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		out->status = SPOTIFY_REPLY_ERROR;
		snprintf(out->message, sizeof(out->message), "socket(): %s", strerror(errno));
		return false;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		// The socket is missing or nobody is listening: the receiver is not up
		// (yet). Not an error worth a message of its own -- the page has one.
		close(fd);
		return false;
	}

	// The read side must not block when the deadline runs out mid-reply.
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}

	bool ok = write_all(fd, request, (size_t)request_len, &until);
	if (ok) {
		// Half-close: the backend reads the request to its end before it
		// answers, and this is what says the request has one.
		shutdown(fd, SHUT_WR);

		char *reply = malloc(SPOTIFY_REPLY_MAX + 1);
		if (!reply) {
			out->status = SPOTIFY_REPLY_ERROR;
			snprintf(out->message, sizeof(out->message), "out of memory");
			close(fd);
			return false;
		}
		int len = read_reply(fd, reply, SPOTIFY_REPLY_MAX + 1, &until);
		if (len < 0) {
			out->status = remaining_ms(&until) == 0 ? SPOTIFY_REPLY_TIMEOUT : SPOTIFY_REPLY_ERROR;
			snprintf(out->message, sizeof(out->message), "no answer from the receiver");
		} else {
			ok = spotify_reply_parse(reply, (size_t)len, id, out);
		}
		free(reply);
	}
	close(fd);

	return ok && out->status == SPOTIFY_REPLY_OK;
}
