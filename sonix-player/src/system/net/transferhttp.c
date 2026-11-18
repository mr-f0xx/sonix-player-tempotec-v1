// The transfer page's HTTP API, not a general-purpose web server. One worker
// handles one request at a time, with bounded headers and a fixed I/O buffer.
// No client path is passed to a shell or resolved relative to the process cwd.
#ifndef __EMSCRIPTEN__
#define _GNU_SOURCE
#include "transferhttp.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define HEADER_MAX 8192
#define CARD_PATH_MAX 1024
#define DEPTH_MAX 32
#define BUFFER_SIZE (32 * 1024)
#define HEADER_MS 5000
#define IO_MS 15000
#define IDLE_CLIENTS 8
// The browser sends a folder one file at a time. Don't announce every file
// while the next one is still coming in.
#define UPLOAD_QUIET_MS 1000
#define DIR_FLAGS (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
#define TEMP_PREFIX ".sonix-upload-"
#define COOKIE "sonix_transfer="

struct transfer_http {
	int listener, wake[2], root;
	dev_t device;
	pthread_t thread;
	atomic_bool stopping, running;
	atomic_uint upload_ready_at; // monotonic ms; zero means nothing pending
	const char *page;
	size_t page_size;
	uint16_t port;
	char ip[INET_ADDRSTRLEN], hostname[80], code[TRANSFER_CODE_LEN + 1];
	unsigned serial;
};

typedef struct {
	char buf[HEADER_MAX + 1];
	char *method, *route, *query, *host, *cookie, *pair, *action, *expect;
	uint64_t length;
	bool has_length;
	size_t body_start, used;
} request_t;

static int64_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * INT64_C(1000) + t.tv_nsec / 1000000;
}

// poll() also watches the stop pipe. A stalled browser must never hold up a
// USB handover just because it isn't reading a download or sending an upload.
static bool wait_io(transfer_http_t *s, int fd, short events, int64_t until) {
	struct pollfd p[] = {{s->wake[0], POLLIN, 0}, {fd, events, 0}};
	while (!atomic_load(&s->stopping)) {
		int64_t left = until - now_ms();
		if (left <= 0) break;
		int n = poll(p, 2, (int)left);
		if (n < 0 && errno == EINTR) continue;
		if (n > 0) return !p[0].revents && p[1].revents;
		break;
	}
	return false;
}

static ssize_t receive(transfer_http_t *s, int fd, void *buf, size_t len, int64_t until) {
	while (wait_io(s, fd, POLLIN, until)) {
		ssize_t n = recv(fd, buf, len, 0);
		if (n >= 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return n;
	}
	return -1;
}

static bool send_all(transfer_http_t *s, int fd, const void *buf, size_t len) {
	const char *p = buf;
	int64_t until = now_ms() + IO_MS;
	while (len && !atomic_load(&s->stopping)) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n > 0) {
			p += n;
			len -= (size_t)n;
			until = now_ms() + IO_MS;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_io(s, fd, POLLOUT, until)) {
			continue;
		} else {
			return false;
		}
	}
	return len == 0;
}

static const char *reason(int status) {
	switch (status) {
	case 200: return "OK";
	case 201: return "Created";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 411: return "Length Required";
	case 413: return "Content Too Large";
	case 417: return "Expectation Failed";
	case 431: return "Request Header Fields Too Large";
	case 507: return "Insufficient Storage";
	default: return "Internal Server Error";
	}
}

static bool reply(transfer_http_t *s, int fd, int status, const char *type, uint64_t length, const char *extra) {
	char length_header[48] = "";
	if (status != 204 && length != UINT64_MAX) {
		snprintf(length_header, sizeof(length_header), "Content-Length: %" PRIu64 "\r\n", length);
	}
	char header[1536];
	int n = snprintf(header, sizeof(header),
					 "HTTP/1.1 %d %s\r\nConnection: close\r\nCache-Control: no-store\r\n"
					 "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
					 "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
					 "img-src data:; connect-src 'self'; base-uri 'none'; form-action 'none'\r\n"
					 "Content-Type: %s\r\n%s%s\r\n",
					 status, reason(status), type, length_header, extra);
	return n > 0 && (size_t)n < sizeof(header) && send_all(s, fd, header, (size_t)n);
}

static bool decimal(const char *p, uint64_t *number) {
	if (!*p) return false;
	*number = 0;
	for (; *p; p++) {
		if (*p < '0' || *p > '9' || *number > (UINT64_MAX - (unsigned)(*p - '0')) / 10) return false;
		*number = *number * 10 + (unsigned)(*p - '0');
	}
	return true;
}

static bool parse_head(request_t *r) {
	char *end = strstr(r->buf, "\r\n");
	if (!end) return false;
	*end = '\0';
	r->method = r->buf;
	char *target = strchr(r->method, ' ');
	if (!target) return false;
	*target++ = '\0';
	char *version = strchr(target, ' ');
	if (!version) return false;
	*version++ = '\0';
	if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) return false;
	if (!*r->method || strspn(r->method, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != strlen(r->method)) return false;
	if (target[0] != '/' || strchr(target, '#')) return false;
	for (const unsigned char *p = (const unsigned char *)target; *p; p++) {
		if (*p < 0x21 || *p > 0x7e) return false;
	}
	r->route = target;
	r->query = strchr(target, '?');
	if (r->query) *r->query++ = '\0';

	for (char *line = end + 2; *line; line = end + 2) {
		end = strstr(line, "\r\n");
		if (!end) return false;
		if (end == line) return true;
		*end = '\0';
		char *value = strchr(line, ':');
		if (!value || value == line) return false;
		*value++ = '\0';
		if (strspn(line, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&'*+-.^_`|~") !=
			strlen(line)) return false;
		value += strspn(value, " \t");
		char *tail = end;
		while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = '\0';
		for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
			if ((*p < 0x20 && *p != '\t') || *p == 0x7f) return false;
		}

		char **slot = NULL;
		if (!strcasecmp(line, "Host")) slot = &r->host;
		else if (!strcasecmp(line, "Cookie")) slot = &r->cookie;
		else if (!strcasecmp(line, "X-Sonix-Pair")) slot = &r->pair;
		else if (!strcasecmp(line, "X-Sonix-Transfer")) slot = &r->action;
		else if (!strcasecmp(line, "Expect")) slot = &r->expect;
		else if (!strcasecmp(line, "Transfer-Encoding")) return false; // fixed-length bodies only
		else if (!strcasecmp(line, "Content-Length")) {
			if (r->has_length || !decimal(value, &r->length)) return false;
			r->has_length = true;
		}
		if (slot) {
			if (*slot) return false;
			*slot = value;
		}
	}
	return false;
}

// Returns 200 for a complete request, zero for a closed/idle socket, or an
// error status. Bytes coalesced with the headers are the start of the upload.
static int read_head(transfer_http_t *s, int fd, request_t *r) {
	int64_t until = now_ms() + HEADER_MS;
	size_t scan = 0;
	while (r->used < HEADER_MAX) {
		ssize_t n = receive(s, fd, r->buf + r->used, HEADER_MAX - r->used, until);
		if (n <= 0) return 0;
		r->used += (size_t)n;
		for (; scan + 4 <= r->used; scan++) {
			if (memcmp(r->buf + scan, "\r\n\r\n", 4)) continue;
			r->body_start = scan + 4;
			if (memchr(r->buf, '\0', r->body_start)) return 400;
			r->buf[r->used] = '\0';
			return parse_head(r) ? 200 : 400;
		}
	}
	return 431;
}

static bool host_matches(const transfer_http_t *s, const char *host) {
	if (!host) return false;
	const char *names[] = {s->ip, s->hostname};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!names[i][0]) continue;
		char with_port[96];
		snprintf(with_port, sizeof(with_port), "%s:%u", names[i], s->port);
		if (!strcasecmp(host, with_port) || (s->port == 80 && !strcasecmp(host, names[i]))) return true;
	}
	return false;
}

static bool code_matches(const transfer_http_t *s, const char *p, size_t n) {
	if (n != TRANSFER_CODE_LEN) return false;
	unsigned diff = 0;
	for (size_t i = 0; i < TRANSFER_CODE_LEN; i++) diff |= (unsigned char)p[i] ^ (unsigned char)s->code[i];
	return diff == 0;
}

static bool paired(const transfer_http_t *s, const char *cookie) {
	while (cookie && *cookie) {
		cookie += strspn(cookie, " \t");
		size_t len = strcspn(cookie, ";");
		size_t prefix = sizeof(COOKIE) - 1;
		if (len == prefix + TRANSFER_CODE_LEN && !memcmp(cookie, COOKIE, prefix) &&
			code_matches(s, cookie + prefix, len - prefix)) return true;
		cookie += len;
		if (*cookie) cookie++;
	}
	return false;
}

static int hex_digit(unsigned char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static bool query_field(const char *query, const char *key, const char **value, size_t *length) {
	*value = NULL;
	size_t klen = strlen(key);
	for (const char *p = query; p && *p;) {
		const char *end = p + strcspn(p, "&");
		if ((size_t)(end - p) > klen && p[klen] == '=' && !memcmp(p, key, klen)) {
			if (*value) return false;
			*value = p + klen + 1;
			*length = (size_t)(end - *value);
		}
		p = *end ? end + 1 : NULL;
	}
	return *value != NULL;
}

static bool query_path(const char *query, const char *key, char out[CARD_PATH_MAX]) {
	const char *v;
	size_t length;
	if (!query_field(query, key, &v, &length)) return false;
	const char *end = v + length;
	size_t used = 0;
	for (; v < end; v++) {
		unsigned char c = *v;
		if (c == '%') {
			if (end - v < 3) return false;
			int a = hex_digit(v[1]), b = hex_digit(v[2]);
			if (a < 0 || b < 0) return false;
			c = (unsigned char)(a * 16 + b);
			v += 2;
		} else if (c == '+') {
			c = ' ';
		}
		if (c < 0x20 || c == 0x7f || c == '\\' || used + 1 == CARD_PATH_MAX) return false;
		out[used++] = (char)c;
	}
	if (!used || out[0] != '/') return false;
	if (used > 1 && out[used - 1] == '/' && out[used - 2] != '/') used--;
	out[used] = '\0';
	return true;
}

// Cards are normally FAT/exFAT, but on a Linux-format card, arbitrary bytes
// in a name need not be valid UTF-8. Skip these rather than emitting bad JSON
// or presenting a filename the browser cannot address again.
static bool component_ok(const char *s) {
	size_t len = strlen(s);
	if (!len || len > NAME_MAX || s[len - 1] == '.' || s[len - 1] == ' ' ||
		!strncasecmp(s, TEMP_PREFIX, sizeof(TEMP_PREFIX) - 1)) return false;
	for (const unsigned char *p = (const unsigned char *)s; *p;) {
		unsigned c = *p++;
		if (c < 0x20 || c == 0x7f || c == '\\' || c == '/') return false;
		if (c < 0x80) continue;
		if (c < 0xc2 || c > 0xf4) return false;
		unsigned more = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
		unsigned cp = c & (0x7f >> (more + 1));
		for (unsigned i = 0; i < more; i++) {
			if ((*p & 0xc0) != 0x80) return false;
			cp = (cp << 6) | (*p++ & 0x3f);
		}
		if ((more == 2 && cp < 0x800) || (more == 3 && cp < 0x10000) || cp > 0x10ffff ||
			(cp >= 0xd800 && cp <= 0xdfff)) return false;
	}
	return true;
}

static int same_device(transfer_http_t *s, int fd) {
	struct stat info;
	if (fstat(fd, &info) == 0) {
		if (info.st_dev == s->device) return fd;
		errno = EXDEV;
	}
	int error = errno;
	close(fd);
	errno = error;
	return -1;
}

// Walk a *logical* path starting at the pinned card fd. Each component is
// opened with O_NOFOLLOW, so even a changed link cannot lead off the card.
// When leaf is requested, stop at the parent and return the basename in path.
static int open_path(transfer_http_t *s, char *path, const char **leaf) {
	if (leaf && !path[1]) {
		errno = EPERM;
		return -1;
	}
	int dir = openat(s->root, ".", DIR_FLAGS);
	if (dir < 0 || !path[1]) return dir;
	char *part = path + 1;
	for (unsigned depth = 0;; depth++) {
		char *slash = strchr(part, '/');
		if (slash) *slash = '\0';
		if (depth == DEPTH_MAX || !component_ok(part)) {
			if (slash) *slash = '/';
			close(dir);
			errno = EINVAL;
			return -1;
		}
		if (!slash && leaf) {
			*leaf = part;
			return dir;
		}
		int next = openat(dir, part, DIR_FLAGS);
		int error = errno;
		if (slash) *slash = '/';
		close(dir);
		if (next < 0) {
			errno = error;
			return -1;
		}
		dir = same_device(s, next);
		if (dir < 0) return -1;
		if (!slash) break;
		part = slash + 1;
	}
	return dir;
}

static int stat_entry(transfer_http_t *s, int dir, const char *name, struct stat *info) {
	if (fstatat(dir, name, info, AT_SYMLINK_NOFOLLOW) < 0) return -1;
	if (info->st_dev != s->device || (!S_ISREG(info->st_mode) && !S_ISDIR(info->st_mode))) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

static int file_status(int error) {
	switch (error) {
	case ENOENT: return 404;
	case EINVAL: case ENAMETOOLONG: return 400;
	case ELOOP: case ENOTDIR: case EXDEV: case EPERM: case EACCES: case EROFS: return 403;
	case EEXIST: case EISDIR: case ENOTEMPTY: case EBUSY: return 409;
	case EFBIG: return 413;
	case ENOSPC: case EDQUOT: return 507;
	default: return 500;
	}
}

// Controls are rejected in component_ok(). Only quotes and backslashes need
// escaping; the destination is sized for twice the input plus its NUL.
static void json_escape(const char *s, char *out) {
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') *out++ = '\\';
		*out++ = *s;
	}
	*out = '\0';
}

static int list(transfer_http_t *s, int client, int fd, const char *path, bool hidden) {
	DIR *dir = fdopendir(fd);
	if (!dir) {
		close(fd);
		return 500;
	}
	bool ok = reply(s, client, 200, "application/json; charset=utf-8", UINT64_MAX, "") && send_all(s, client, "[", 1);
	const char *slash = path[1] ? "/" : "";
	bool first = true;
	while (ok) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			if (!errno) send_all(s, client, "]", 1);
			break;
		}
		if (atomic_load(&s->stopping)) break;
		const char *name = entry->d_name;
		if ((!hidden && name[0] == '.') || !component_ok(name)) continue;
		struct stat info;
		if (stat_entry(s, fd, name, &info)) continue;
		bool folder = S_ISDIR(info.st_mode);
		char child[CARD_PATH_MAX], epath[2 * CARD_PATH_MAX], ename[2 * NAME_MAX + 1], size[64] = "";
		int n = snprintf(child, sizeof(child), "%s%s%s%s", path, slash, name, folder ? "/" : "");
		if (n < 0 || (size_t)n >= sizeof(child)) continue;
		json_escape(child, epath);
		json_escape(name, ename);
		if (!folder) snprintf(size, sizeof(size), ",\"size\":%jd", (intmax_t)info.st_size);
		char row[sizeof(epath) + sizeof(ename) + sizeof(size) + 96];
		n = snprintf(row, sizeof(row), "%s{\"path\":\"%s\",\"name\":\"%s\",\"ctime\":%jd%s}",
					 first ? "" : ",", epath, ename, (intmax_t)info.st_mtime, size);
		ok = n > 0 && (size_t)n < sizeof(row) && send_all(s, client, row, (size_t)n);
		first = false;
	}
	closedir(dir);
	return 0;
}

static int download(transfer_http_t *s, int client, int dir, const char *name, bool head) {
	struct stat info;
	if (stat_entry(s, dir, name, &info)) return file_status(errno);
	if (!S_ISREG(info.st_mode)) return 403;
	int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return file_status(errno);
	if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_dev != s->device) {
		close(fd);
		return 403;
	}
	// Encode every non-alphanumeric byte for RFC 5987 (including quotes and %).
	char encoded[3 * NAME_MAX + 1], *out = encoded;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		unsigned c = *p;
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.') {
			*out++ = (char)c;
		} else {
			static const char hex[] = "0123456789ABCDEF";
			*out++ = '%';
			*out++ = hex[c >> 4];
			*out++ = hex[c & 15];
		}
	}
	*out = '\0';
	char extra[sizeof(encoded) + 60];
	snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename*=UTF-8''%s\r\n", encoded);
	uint64_t left = (uint64_t)info.st_size;
	if (reply(s, client, 200, "application/octet-stream", left, extra) && !head) {
		char buf[BUFFER_SIZE];
		while (left && !atomic_load(&s->stopping)) {
			ssize_t n = read(fd, buf, left < sizeof(buf) ? (size_t)left : sizeof(buf));
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0 || !send_all(s, client, buf, (size_t)n)) break;
			left -= (uint64_t)n;
		}
	}
	close(fd);
	return 0;
}

static bool write_all(int fd, const char *buf, size_t size) {
	while (size) {
		ssize_t n = write(fd, buf, size);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) {
			if (!n) errno = EIO;
			return false;
		}
		buf += n;
		size -= (size_t)n;
	}
	return true;
}

static int vacant(int fd, const char *name) {
	struct stat info;
	if (fstatat(fd, name, &info, AT_SYMLINK_NOFOLLOW) == 0) return 409;
	return errno == ENOENT ? 0 : file_status(errno);
}

static int upload(transfer_http_t *s, int client, const request_t *r, int dir, const char *name) {
	int status = vacant(dir, name);
	if (status) return status;
	struct statvfs fs;
	if (fstatvfs(dir, &fs) == 0 && fs.f_frsize > 0 &&
		(r->length + fs.f_frsize - 1) / fs.f_frsize > fs.f_bavail) return 507;

	// The staging file is on the same filesystem so publication is a rename.
	// Exclusive creation also avoids following a pre-existing link (or a part
	// file left behind by a sudden power loss). The listing never shows parts.
	char temp[60];
	int fd = -1;
	for (unsigned i = 0; i < 32 && fd < 0; i++) {
		snprintf(temp, sizeof(temp), TEMP_PREFIX "%lx-%x.part", (unsigned long)getpid(), ++s->serial);
		fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		if (fd < 0 && errno != EEXIST) return file_status(errno);
	}
	if (fd < 0) return 500;

	const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";
	if (r->expect && !send_all(s, client, go, sizeof(go) - 1)) status = 400;
	uint64_t left = r->length;
	size_t first = r->used - r->body_start;
	if (first > left) first = (size_t)left;
	if (!status && !write_all(fd, r->buf + r->body_start, first)) status = file_status(errno);
	left -= first;

	char buf[BUFFER_SIZE];
	while (!status && left) {
		ssize_t n = receive(s, client, buf, left < sizeof(buf) ? (size_t)left : sizeof(buf), now_ms() + IO_MS);
		if (n <= 0) status = 400;
		else if (!write_all(fd, buf, (size_t)n)) status = file_status(errno);
		else left -= (uint64_t)n;
	}
	if (!status && atomic_load(&s->stopping)) status = 400;
	if (!status && fsync(fd) < 0) status = file_status(errno);
	if (close(fd) < 0 && !status) status = file_status(errno);
	if (!status && atomic_load(&s->stopping)) status = 400;
	if (!status) status = vacant(dir, name);
	if (!status && renameat(dir, temp, dir, name) == 0) return 201;
	if (!status) status = file_status(errno);
	unlinkat(dir, temp, 0);
	return status;
}

static int remove_tree(transfer_http_t *s, int parent, const char *name, unsigned depth) {
	if (atomic_load(&s->stopping)) return 409;
	struct stat info;
	if (fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) < 0) return file_status(errno);
	if (info.st_dev != s->device) return 403;
	if (!S_ISDIR(info.st_mode)) return unlinkat(parent, name, 0) == 0 ? 204 : file_status(errno);
	if (depth == DEPTH_MAX) return 409;
	int fd = openat(parent, name, DIR_FLAGS);
	if (fd < 0) return file_status(errno);
	if (same_device(s, fd) < 0) return file_status(errno);
	DIR *dir = fdopendir(fd);
	if (!dir) {
		close(fd);
		return 500;
	}
	int status = 204;
	while (status == 204) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			if (errno) status = file_status(errno);
			break;
		}
		const char *child = entry->d_name;
		if (!strcmp(child, ".") || !strcmp(child, "..")) continue;
		status = remove_tree(s, fd, child, depth + 1);
	}
	closedir(dir);
	if (status == 204 && unlinkat(parent, name, AT_REMOVEDIR) < 0) status = file_status(errno);
	return status;
}

static int move(transfer_http_t *s, int dir, const char *name, const char *query) {
	struct stat info;
	if (stat_entry(s, dir, name, &info)) return file_status(errno);
	char dest[CARD_PATH_MAX];
	if (!query_path(query, "newPath", dest)) return 400;
	const char *dest_name;
	int to = open_path(s, dest, &dest_name);
	if (to < 0) return file_status(errno);
	int status = vacant(to, dest_name);
	if (!status) status = renameat(dir, name, to, dest_name) == 0 ? 204 : file_status(errno);
	close(to);
	return status;
}

static int dispatch(transfer_http_t *s, int client, request_t *r) {
	if (!host_matches(s, r->host)) return 403; // also reject a DNS-rebinding origin
	bool get = !strcmp(r->method, "GET"), head = !strcmp(r->method, "HEAD");
	bool post = !strcmp(r->method, "POST"), put = !strcmp(r->method, "PUT");
	if (r->expect && (!put || strcasecmp(r->expect, "100-continue"))) return 417;
	if (!put && r->length) return 400;
	if (!strcmp(r->route, "/")) {
		if (!get && !head) return 405;
		if (reply(s, client, 200, "text/html; charset=utf-8", s->page_size, "") && !head) {
			send_all(s, client, s->page, s->page_size);
		}
		return 0;
	}
	if (!strcmp(r->route, "/pair")) {
		if (!post) return 405;
		if (!r->pair || !code_matches(s, r->pair, strlen(r->pair))) return 401;
		char cookie[128];
		snprintf(cookie, sizeof(cookie), "Set-Cookie: " COOKIE "%s; Path=/; HttpOnly; SameSite=Strict\r\n", s->code);
		reply(s, client, 204, "text/plain", 0, cookie);
		return 0;
	}
	if (!paired(s, r->cookie)) return 401;
	// An HTML form can't supply this header. Cross-origin scripts would need
	// a successful CORS preflight; this server never grants one.
	if ((post || put) && (!r->action || strcmp(r->action, "1"))) return 403;

	bool listing = !strcmp(r->route, "/list"), downloading = !strcmp(r->route, "/download");
	bool uploading = !strcmp(r->route, "/upload"), creating = !strcmp(r->route, "/create");
	bool deleting = !strcmp(r->route, "/delete"), moving = !strcmp(r->route, "/move");
	if (!(listing || downloading || uploading || creating || deleting || moving)) return 404;
	if ((listing && !get) || (downloading && !get && !head) || (uploading && !put) ||
		((creating || deleting || moving) && !post)) return 405;
	if (uploading) atomic_store(&s->upload_ready_at, 0);
	if (uploading && !r->has_length) return 411;
	if (uploading && r->length > UINT32_MAX) return 413; // works on FAT32 as well as exFAT

	char path[CARD_PATH_MAX];
	if (!query_path(r->query, moving ? "oldPath" : "path", path)) return 400;
	const char *leaf = NULL;
	int dir = open_path(s, path, listing ? NULL : &leaf);
	if (dir < 0) return file_status(errno);
	if (listing) {
		const char *value;
		size_t length;
		bool hidden = query_field(r->query, "hidden", &value, &length) && length == 1 && *value == '1';
		return list(s, client, dir, path, hidden);
	}
	int status;
	if (downloading) status = download(s, client, dir, leaf, head);
	else if (uploading) {
		status = upload(s, client, r, dir, leaf);
		if (status == 201) {
			// Publication, not a browser progress reading, is what counts.
			unsigned due = (uint32_t)now_ms() + UPLOAD_QUIET_MS;
			atomic_store(&s->upload_ready_at, due ? due : 1);
		}
	} else if (moving) status = move(s, dir, leaf, r->query);
	else if (deleting) status = remove_tree(s, dir, leaf, 0);
	else if (mkdirat(dir, leaf, 0755) == 0) status = 201;
	else {
		status = file_status(errno);
		// Folder uploads may share existing album/artist directories.
		struct stat info;
		if (status == 409 && stat_entry(s, dir, leaf, &info) == 0 && S_ISDIR(info.st_mode)) status = 204;
	}
	close(dir);
	return status;
}

static void serve(transfer_http_t *s, int client) {
	request_t r = {0};
	int status = read_head(s, client, &r);
	if (status == 200) status = dispatch(s, client, &r);
	if (status && reply(s, client, status, "text/plain", 0, "") && status >= 400) {
		// Give the client a chance to read an early rejection (e.g. a file
		// exists) before closing a socket that may still have upload bytes
		// coming in. Closing immediately with unread data can reset it.
		shutdown(client, SHUT_WR);
		char discard[2048];
		int64_t until = now_ms() + 150;
		while (receive(s, client, discard, sizeof(discard), until) > 0) {}
	}
}

static void *worker(void *arg) {
	transfer_http_t *s = arg;
	// Browsers preconnect idle sockets. Wait for readable data before choosing
	// a request, otherwise one unused connection stalls all the others.
	struct pollfd polls[2 + IDLE_CLIENTS] = {{s->wake[0], POLLIN, 0}, {s->listener, POLLIN, 0}};
	int64_t expires[IDLE_CLIENTS] = {0};
	for (unsigned i = 0; i < IDLE_CLIENTS; i++) polls[i + 2] = (struct pollfd){-1, POLLIN, 0};
	while (!atomic_load(&s->stopping)) {
		int ready = poll(polls, 2 + IDLE_CLIENTS, 1000);
		if (ready < 0 && errno == EINTR) continue;
		if (ready < 0 || polls[0].revents || (polls[1].revents & (POLLERR | POLLHUP | POLLNVAL))) break;
		for (unsigned i = 0; i < IDLE_CLIENTS && !atomic_load(&s->stopping); i++) {
			struct pollfd *p = &polls[i + 2];
			if (p->fd < 0) continue;
			if (p->revents & POLLIN) serve(s, p->fd);
			else if (!p->revents && now_ms() < expires[i]) continue;
			close(p->fd);
			p->fd = -1;
		}
		if (polls[1].revents & POLLIN) {
			int fd = accept4(s->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
			if (fd < 0) continue;
			unsigned i = 0;
			while (i < IDLE_CLIENTS && polls[i + 2].fd >= 0) i++;
			if (i == IDLE_CLIENTS) close(fd);
			else {
				polls[i + 2].fd = fd;
				expires[i] = now_ms() + HEADER_MS;
			}
		}
	}
	for (unsigned i = 0; i < IDLE_CLIENTS; i++) {
		if (polls[i + 2].fd >= 0) close(polls[i + 2].fd);
	}
	atomic_store(&s->running, false);
	return NULL;
}

static bool make_code(char code[TRANSFER_CODE_LEN + 1]) {
	static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
	unsigned char random[TRANSFER_CODE_LEN];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) return false;
	size_t got = 0;
	while (got < sizeof(random)) {
		ssize_t n = read(fd, random + got, sizeof(random) - got);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		got += (size_t)n;
	}
	close(fd);
	if (got != sizeof(random)) return false;
	for (size_t i = 0; i < got; i++) code[i] = alphabet[random[i] & 31];
	code[got] = '\0';
	return true;
}

static void dispose(transfer_http_t *s) {
	if (s->listener >= 0) close(s->listener);
	if (s->wake[0] >= 0) close(s->wake[0]);
	if (s->wake[1] >= 0) close(s->wake[1]);
	if (s->root >= 0) close(s->root);
	free(s);
}

transfer_http_t *transfer_http_start(int card_fd, const char *ip, uint16_t port, const char *hostname,
									 const char *page, size_t page_size) {
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 || addr.sin_addr.s_addr == INADDR_ANY) return NULL;
	struct stat info;
	if (fstat(card_fd, &info) < 0 || !S_ISDIR(info.st_mode)) return NULL;
	transfer_http_t *s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->listener = s->wake[0] = s->wake[1] = -1;
	s->root = fcntl(card_fd, F_DUPFD_CLOEXEC, 0);
	s->device = info.st_dev;
	s->page = page;
	s->page_size = page_size;
	snprintf(s->ip, sizeof(s->ip), "%s", ip);
	if (hostname) snprintf(s->hostname, sizeof(s->hostname), "%s", hostname);
	if (s->root < 0 || !make_code(s->code) || pipe2(s->wake, O_CLOEXEC | O_NONBLOCK) < 0) goto fail;
	s->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (s->listener < 0) goto fail;
	const int one = 1;
	setsockopt(s->listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	socklen_t len = sizeof(addr);
	if (bind(s->listener, (struct sockaddr *)&addr, len) < 0 || listen(s->listener, IDLE_CLIENTS) < 0 ||
		getsockname(s->listener, (struct sockaddr *)&addr, &len) < 0) goto fail;
	s->port = ntohs(addr.sin_port);
	atomic_init(&s->stopping, false);
	atomic_init(&s->running, true);
	atomic_init(&s->upload_ready_at, 0);
	if (pthread_create(&s->thread, NULL, worker, s) != 0) goto fail;
	return s;

fail:
	dispose(s);
	return NULL;
}

uint16_t transfer_http_port(const transfer_http_t *s) { return s->port; }
const char *transfer_http_code(const transfer_http_t *s) { return s->code; }
bool transfer_http_running(const transfer_http_t *s) { return s && atomic_load(&s->running); }

bool transfer_http_take_upload_complete(transfer_http_t *s) {
	if (!transfer_http_running(s)) return false;
	unsigned due = atomic_load(&s->upload_ready_at);
	return due && (int32_t)((uint32_t)now_ms() - due) >= 0 &&
		   atomic_compare_exchange_strong(&s->upload_ready_at, &due, 0);
}

void transfer_http_stop(transfer_http_t *s) {
	if (!s) return;
	atomic_store(&s->stopping, true);
	ssize_t n;
	do { n = write(s->wake[1], "!", 1); } while (n < 0 && errno == EINTR);
	pthread_join(s->thread, NULL);
	dispose(s);
}
#endif /* !__EMSCRIPTEN__ */
