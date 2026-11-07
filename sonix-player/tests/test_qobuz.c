#define _GNU_SOURCE 1

#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/core/config.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// Minimal translation stub matching English.ini for host tests.
const char *tr(const char *key) {
	if (!key) {
		return "";
	}
	if (strcmp(key, "qobuz_unreadable_qobuz_reply") == 0) {
		return "Unreadable Qobuz reply";
	}
	if (strcmp(key, "qobuz_answered_with") == 0) {
		return "Qobuz answered %d";
	}
	if (strcmp(key, "qobuz_no_reply") == 0) {
		return "No reply from Qobuz";
	}
	if (strcmp(key, "qobuz_gave_no_session_token") == 0) {
		return "Qobuz gave no session token";
	}
	if (strcmp(key, "qobuz_you_are_not_signed_in_to_qobuz") == 0) {
		return "You are not signed in to Qobuz";
	}
	if (strcmp(key, "http_server_answered") == 0) {
		return "the server answered %d";
	}
	if (strcmp(key, "http_bad_address") == 0) {
		return "That address makes no sense: %s";
	}
	return key;
}

// Dynamic-Huffman (level 9) compressed 12-album Qobuz JSON payload (2711 bytes).
static const uint8_t FEATURED_GZ[] = {
	0x1f,0x8b,0x08,0x00,0x6b,0x7a,0xee,0x6a,0x02,0xff,0xad,0xd6,0xc1,0x72,0x83,0x20,
	0x14,0x05,0xd0,0x5f,0xc9,0xb0,0x6e,0x22,0x0f,0x15,0x34,0xbb,0x7e,0x47,0xa7,0xe3,
	0x10,0x43,0x0d,0xad,0xc6,0x14,0xb0,0x8b,0x66,0xfc,0xf7,0x22,0xdd,0x74,0xc1,0xbc,
	0xcc,0x14,0x56,0x22,0x4f,0xd1,0x33,0x77,0x73,0xef,0x44,0x8e,0xa7,0x65,0xb2,0xe4,
	0xb8,0xbb,0x13,0xed,0x54,0x58,0xbd,0xf8,0xe5,0xd9,0x5f,0x09,0xa5,0x9c,0xb2,0xba,
	0x12,0x6d,0x43,0x29,0x25,0x4f,0x3b,0xe2,0xb4,0x1b,0xd5,0x36,0x79,0xde,0xde,0xda,
	0x85,0x3d,0x69,0x9c,0xb6,0x2e,0x1c,0x70,0x95,0xd3,0xef,0x34,0x6c,0xf9,0xf1,0xea,
	0xe7,0x7a,0x92,0x83,0x0a,0xe3,0x51,0x9a,0xb0,0x22,0x17,0xe7,0x6e,0xf6,0x58,0x14,
	0xd6,0x49,0xa7,0xfb,0xc3,0xe7,0x7c,0x5a,0xbe,0x0f,0xfd,0x3c,0x15,0xe1,0x59,0x5b,
	0xf4,0xf3,0x97,0x32,0xb6,0xa0,0x1d,0xa7,0xf4,0xf0,0x7e,0x1b,0xc2,0x39,0xce,0xc8,
	0xfe,0xc3,0x76,0xfd,0xbc,0x5c,0xb7,0xaf,0x01,0xf5,0x7b,0x17,0x6d,0xd4,0xf6,0xc7,
	0xce,0x2c,0xca,0xdf,0x1a,0x35,0x2a,0x69,0x55,0x77,0x96,0x4e,0x75,0xb3,0xd1,0x83,
	0xbe,0xca,0x71,0xfb,0x20,0xf3,0x8c,0x3d,0xd0,0x3d,0x88,0xed,0xa8,0x18,0x0f,0x22,
	0x3c,0xc0,0x79,0x90,0xc8,0x03,0x94,0x07,0x7f,0x78,0x6f,0x72,0xb4,0x89,0x3e,0x16,
	0xf1,0x31,0xdc,0xc7,0x12,0x7d,0x0c,0xf5,0xb1,0xac,0xf1,0x95,0x11,0x5e,0x89,0xf3,
	0xca,0x44,0x5e,0x89,0xf2,0xca,0xbc,0xf1,0x55,0x11,0x5f,0x85,0xfb,0xaa,0x44,0x5f,
	0x85,0xfa,0xaa,0xac,0xf1,0xd5,0x11,0x5e,0x8d,0xf3,0xea,0x44,0x5e,0x8d,0xf2,0xea,
	0xbc,0xf1,0xf1,0x88,0x8f,0xe3,0x3e,0x9e,0xe8,0xe3,0xa8,0x8f,0x67,0x8d,0x4f,0x44,
	0x78,0x02,0xe7,0x89,0x44,0x9e,0x40,0x79,0x22,0x6f,0x7c,0x4d,0xc4,0xd7,0xe0,0xbe,
	0x26,0xd1,0xd7,0xa0,0xbe,0x26,0x6b,0x7c,0x6d,0x84,0xd7,0xe2,0xbc,0x36,0x91,0xd7,
	0xa2,0xbc,0x36,0x6b,0x7c,0x10,0xab,0x2e,0xf0,0xa0,0xbb,0x40,0x6a,0x79,0x01,0xb4,
	0xbd,0xb0,0xac,0xed,0x05,0xa2,0xed,0xe5,0x51,0x7d,0x49,0xee,0x2f,0x68,0x81,0x61,
	0xff,0x2e,0x30,0xaf,0xeb,0xfa,0x03,0xcf,0x6f,0x91,0x73,0x97,0x0a,0x00,0x00
};

static const uint8_t FEATURED_ZLIB[] = {
	0x78,0xda,0xad,0xd6,0xc1,0x72,0x83,0x20,0x14,0x05,0xd0,0x5f,0xc9,0xb0,0x6e,0x22,
	0x0f,0x15,0x34,0xbb,0x7e,0x47,0xa7,0xe3,0x10,0x43,0x0d,0xad,0xc6,0x14,0xb0,0x8b,
	0x66,0xfc,0xf7,0x22,0xdd,0x74,0xc1,0xbc,0xcc,0x14,0x56,0x22,0x4f,0xd1,0x33,0x77,
	0x73,0xef,0x44,0x8e,0xa7,0x65,0xb2,0xe4,0xb8,0xbb,0x13,0xed,0x54,0x58,0xbd,0xf8,
	0xe5,0xd9,0x5f,0x09,0xa5,0x9c,0xb2,0xba,0x12,0x6d,0x43,0x29,0x25,0x4f,0x3b,0xe2,
	0xb4,0x1b,0xd5,0x36,0x79,0xde,0xde,0xda,0x85,0x3d,0x69,0x9c,0xb6,0x2e,0x1c,0x70,
	0x95,0xd3,0xef,0x34,0x6c,0xf9,0xf1,0xea,0xe7,0x7a,0x92,0x83,0x0a,0xe3,0x51,0x9a,
	0xb0,0x22,0x17,0xe7,0x6e,0xf6,0x58,0x14,0xd6,0x49,0xa7,0xfb,0xc3,0xe7,0x7c,0x5a,
	0xbe,0x0f,0xfd,0x3c,0x15,0xe1,0x59,0x5b,0xf4,0xf3,0x97,0x32,0xb6,0xa0,0x1d,0xa7,
	0xf4,0xf0,0x7e,0x1b,0xc2,0x39,0xce,0xc8,0xfe,0xc3,0x76,0xfd,0xbc,0x5c,0xb7,0xaf,
	0x01,0xf5,0x7b,0x17,0x6d,0xd4,0xf6,0xc7,0xce,0x2c,0xca,0xdf,0x1a,0x35,0x2a,0x69,
	0x55,0x77,0x96,0x4e,0x75,0xb3,0xd1,0x83,0xbe,0xca,0x71,0xfb,0x20,0xf3,0x8c,0x3d,
	0xd0,0x3d,0x88,0xed,0xa8,0x18,0x0f,0x22,0x3c,0xc0,0x79,0x90,0xc8,0x03,0x94,0x07,
	0x7f,0x78,0x6f,0x72,0xb4,0x89,0x3e,0x16,0xf1,0x31,0xdc,0xc7,0x12,0x7d,0x0c,0xf5,
	0xb1,0xac,0xf1,0x95,0x11,0x5e,0x89,0xf3,0xca,0x44,0x5e,0x89,0xf2,0xca,0xbc,0xf1,
	0x55,0x11,0x5f,0x85,0xfb,0xaa,0x44,0x5f,0x85,0xfa,0xaa,0xac,0xf1,0xd5,0x11,0x5e,
	0x8d,0xf3,0xea,0x44,0x5e,0x8d,0xf2,0xea,0xbc,0xf1,0xf1,0x88,0x8f,0xe3,0x3e,0x9e,
	0xe8,0xe3,0xa8,0x8f,0x67,0x8d,0x4f,0x44,0x78,0x02,0xe7,0x89,0x44,0x9e,0x40,0x79,
	0x22,0x6f,0x7c,0x4d,0xc4,0xd7,0xe0,0xbe,0x26,0xd1,0xd7,0xa0,0xbe,0x26,0x6b,0x7c,
	0x6d,0x84,0xd7,0xe2,0xbc,0x36,0x91,0xd7,0xa2,0xbc,0x36,0x6b,0x7c,0x10,0xab,0x2e,
	0xf0,0xa0,0xbb,0x40,0x6a,0x79,0x01,0xb4,0xbd,0xb0,0xac,0xed,0x05,0xa2,0xed,0xe5,
	0x51,0x7d,0x49,0xee,0x2f,0x68,0x81,0x61,0xff,0x2e,0x30,0xaf,0xeb,0xfa,0x03,0x7a,
	0xf8,0x39,0x86
};

static const uint8_t FEATURED_RAW_DEFLATE[] = {
	0xad,0xd6,0xc1,0x72,0x83,0x20,0x14,0x05,0xd0,0x5f,0xc9,0xb0,0x6e,0x22,0x0f,0x15,
	0x34,0xbb,0x7e,0x47,0xa7,0xe3,0x10,0x43,0x0d,0xad,0xc6,0x14,0xb0,0x8b,0x66,0xfc,
	0xf7,0x22,0xdd,0x74,0xc1,0xbc,0xcc,0x14,0x56,0x22,0x4f,0xd1,0x33,0x77,0x73,0xef,
	0x44,0x8e,0xa7,0x65,0xb2,0xe4,0xb8,0xbb,0x13,0xed,0x54,0x58,0xbd,0xf8,0xe5,0xd9,
	0x5f,0x09,0xa5,0x9c,0xb2,0xba,0x12,0x6d,0x43,0x29,0x25,0x4f,0x3b,0xe2,0xb4,0x1b,
	0xd5,0x36,0x79,0xde,0xde,0xda,0x85,0x3d,0x69,0x9c,0xb6,0x2e,0x1c,0x70,0x95,0xd3,
	0xef,0x34,0x6c,0xf9,0xf1,0xea,0xe7,0x7a,0x92,0x83,0x0a,0xe3,0x51,0x9a,0xb0,0x22,
	0x17,0xe7,0x6e,0xf6,0x58,0x14,0xd6,0x49,0xa7,0xfb,0xc3,0xe7,0x7c,0x5a,0xbe,0x0f,
	0xfd,0x3c,0x15,0xe1,0x59,0x5b,0xf4,0xf3,0x97,0x32,0xb6,0xa0,0x1d,0xa7,0xf4,0xf0,
	0x7e,0x1b,0xc2,0x39,0xce,0xc8,0xfe,0xc3,0x76,0xfd,0xbc,0x5c,0xb7,0xaf,0x01,0xf5,
	0x7b,0x17,0x6d,0xd4,0xf6,0xc7,0xce,0x2c,0xca,0xdf,0x1a,0x35,0x2a,0x69,0x55,0x77,
	0x96,0x4e,0x75,0xb3,0xd1,0x83,0xbe,0xca,0x71,0xfb,0x20,0xf3,0x8c,0x3d,0xd0,0x3d,
	0x88,0xed,0xa8,0x18,0x0f,0x22,0x3c,0xc0,0x79,0x90,0xc8,0x03,0x94,0x07,0x7f,0x78,
	0x6f,0x72,0xb4,0x89,0x3e,0x16,0xf1,0x31,0xdc,0xc7,0x12,0x7d,0x0c,0xf5,0xb1,0xac,
	0xf1,0x95,0x11,0x5e,0x89,0xf3,0xca,0x44,0x5e,0x89,0xf2,0xca,0xbc,0xf1,0x55,0x11,
	0x5f,0x85,0xfb,0xaa,0x44,0x5f,0x85,0xfa,0xaa,0xac,0xf1,0xd5,0x11,0x5e,0x8d,0xf3,
	0xea,0x44,0x5e,0x8d,0xf2,0xea,0xbc,0xf1,0xf1,0x88,0x8f,0xe3,0x3e,0x9e,0xe8,0xe3,
	0xa8,0x8f,0x67,0x8d,0x4f,0x44,0x78,0x02,0xe7,0x89,0x44,0x9e,0x40,0x79,0x22,0x6f,
	0x7c,0x4d,0xc4,0xd7,0xe0,0xbe,0x26,0xd1,0xd7,0xa0,0xbe,0x26,0x6b,0x7c,0x6d,0x84,
	0xd7,0xe2,0xbc,0x36,0x91,0xd7,0xa2,0xbc,0x36,0x6b,0x7c,0x10,0xab,0x2e,0xf0,0xa0,
	0xbb,0x40,0x6a,0x79,0x01,0xb4,0xbd,0xb0,0xac,0xed,0x05,0xa2,0xed,0xe5,0x51,0x7d,
	0x49,0xee,0x2f,0x68,0x81,0x61,0xff,0x2e,0x30,0xaf,0xeb,0xfa,0x03
};

// Wraps arbitrary bytes as a valid RFC 1952 gzip member (with optional FEXTRA,
// FNAME, FCOMMENT, FHCRC header fields) using stored DEFLATE blocks so multi-MB
// payloads can be generated without linking zlib.
static uint8_t *gzip_wrap_stored(const uint8_t *src, size_t len, bool with_extra_hdrs, size_t *out_len) {
	size_t blocks = (len / 65535) + 1;
	size_t cap = 64 + len + blocks * 5 + 8;
	uint8_t *out = malloc(cap);
	assert(out);

	size_t p = 0;
	out[p++] = 0x1f;
	out[p++] = 0x8b;
	out[p++] = 0x08;
	out[p++] = with_extra_hdrs ? (0x04 | 0x08 | 0x10 | 0x02) : 0x00;
	memset(out + p, 0, 6);
	p += 6;

	if (with_extra_hdrs) {
		out[p++] = 4;
		out[p++] = 0;
		memcpy(out + p, "QOBZ", 4);
		p += 4;
		memcpy(out + p, "api.json", 9);
		p += 9;
		memcpy(out + p, "comment", 8);
		p += 8;
		out[p++] = 0x12;
		out[p++] = 0x34;
	}

	size_t pos = 0;
	do {
		size_t chunk = len - pos;
		if (chunk > 65535) {
			chunk = 65535;
		}
		bool final = (pos + chunk == len);
		out[p++] = final ? 0x01 : 0x00; // BFINAL + BTYPE=00 (stored)
		uint16_t clen = (uint16_t)chunk;
		uint16_t nlen = (uint16_t)~clen;
		out[p++] = (uint8_t)(clen & 0xff);
		out[p++] = (uint8_t)(clen >> 8);
		out[p++] = (uint8_t)(nlen & 0xff);
		out[p++] = (uint8_t)(nlen >> 8);
		if (chunk > 0) {
			memcpy(out + p, src + pos, chunk);
			p += chunk;
		}
		pos += chunk;
	} while (pos < len);

	memset(out + p, 0, 4); // CRC32 (not checked by HTTP transport layer)
	p += 4;
	uint32_t isize = (uint32_t)len;
	out[p++] = (uint8_t)(isize & 0xff);
	out[p++] = (uint8_t)((isize >> 8) & 0xff);
	out[p++] = (uint8_t)((isize >> 16) & 0xff);
	out[p++] = (uint8_t)((isize >> 24) & 0xff);

	*out_len = p;
	return out;
}

typedef struct {
	int listen_fd;
	int port;
	volatile bool stop;
	char expected_app_id[64];
	char expected_secret[128];
	char issued_token[1200];
	char encoded_token[3600];
	int mode;
	int fav_id_pages;
} fake_qobuz_t;

enum {
	MODE_NORMAL = 0,
	MODE_GZIP_DYNAMIC,
	MODE_ZLIB,
	MODE_RAW_DEFLATE,
	MODE_GZIP_MULTIMEMBER_AND_FLAGS,
	MODE_GZIP_LARGE_3MB,
	MODE_OVERSIZED_HEADERS,
	MODE_HTML_403,
	MODE_EMPTY_401,
	MODE_UTF8_BOM,
};

static bool send_all(int fd, const void *buf, size_t len) {
	const uint8_t *p = (const uint8_t *)buf;
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
		if (n <= 0) {
			return false;
		}
		sent += (size_t)n;
	}
	return true;
}

static bool extract_header(const char *req, const char *name, char *out, size_t out_size) {
	size_t nlen = strlen(name);
	const char *line = strchr(req, '\n');
	while (line && *line) {
		line++;
		if (*line == '\r' || *line == '\n') {
			break;
		}
		if (strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
			const char *v = line + nlen + 1;
			while (*v == ' ' || *v == '\t') {
				v++;
			}
			size_t len = strcspn(v, "\r\n");
			assert(len < out_size);
			memcpy(out, v, len);
			out[len] = '\0';
			return true;
		}
		line = strchr(line, '\n');
	}
	return false;
}

static void send_http(int fd, int status, const char *extra_hdrs, const void *body, size_t body_len) {
	char head[2048];
	int n = snprintf(head, sizeof(head),
					 "HTTP/1.1 %d %s\r\n"
					 "Content-Type: application/json; charset=utf-8\r\n"
					 "Content-Length: %zu\r\n"
					 "Connection: close\r\n"
					 "%s"
					 "\r\n",
					 status, status == 200 ? "OK" : "Error", body_len, extra_hdrs ? extra_hdrs : "");
	assert(n > 0 && (size_t)n < sizeof(head));
	send_all(fd, head, (size_t)n);
	if (body_len > 0) {
		send_all(fd, body, body_len);
	}
}

static void *server_main(void *arg) {
	fake_qobuz_t *srv = (fake_qobuz_t *)arg;
	char *req = malloc(32768);
	assert(req);

	while (!srv->stop) {
		int cfd = accept(srv->listen_fd, NULL, NULL);
		if (cfd < 0) {
			if (srv->stop) {
				break;
			}
			continue;
		}

		size_t used = 0;
		while (used + 1 < 32768) {
			ssize_t r = recv(cfd, req + used, 32768 - used - 1, 0);
			if (r <= 0) {
				break;
			}
			used += (size_t)r;
			req[used] = '\0';
			if (strstr(req, "\r\n\r\n")) {
				break;
			}
		}
		if (used == 0) {
			close(cfd);
			continue;
		}

		char app_hdr[128] = "";
		assert(extract_header(req, "X-App-Id", app_hdr, sizeof(app_hdr)));
		assert(strcmp(app_hdr, srv->expected_app_id) == 0);

		char enc_hdr[128] = "";
		assert(extract_header(req, "Accept-Encoding", enc_hdr, sizeof(enc_hdr)));
		assert(strstr(enc_hdr, "gzip") != NULL);

		if (strncmp(req, "GET /api.json/0.2/user/login?", 29) == 0) {
			char tok_hdr[64] = "";
			assert(!extract_header(req, "X-User-Auth-Token", tok_hdr, sizeof(tok_hdr)));
			char dev_hdr[128] = "";
			assert(extract_header(req, "device_manufacturer_id", dev_hdr, sizeof(dev_hdr)));
			assert(strcmp(dev_hdr, srv->expected_secret) == 0);

			char reply[2048];
			int rn = snprintf(reply, sizeof(reply),
							  "{\"user_auth_token\":\"%s\",\"user\":{\"display_name\":\"HiFi Listener\"}}",
							  srv->issued_token);
			assert(rn > 0 && (size_t)rn < sizeof(reply));
			send_http(cfd, 200, NULL, reply, (size_t)rn);
			close(cfd);
			continue;
		}

		// Every post-login request must carry the FULL un-truncated JWT in
		// X-User-Auth-Token -- if truncated at 255 or 127 bytes, mimic an HTML
		// 400 gateway failure so the test catches it immediately.
		char auth_hdr[2048] = "";
		if (!extract_header(req, "X-User-Auth-Token", auth_hdr, sizeof(auth_hdr)) ||
			strcmp(auth_hdr, srv->issued_token) != 0) {
			const char *bad = "<html><body><h1>400 Bad Request: truncated auth token</h1></body></html>";
			send_http(cfd, 400, NULL, bad, strlen(bad));
			close(cfd);
			continue;
		}

		if (srv->mode == MODE_HTML_403) {
			const char *html = "<!DOCTYPE html><html><title>403 Forbidden</title></html>";
			send_http(cfd, 403, NULL, html, strlen(html));
			close(cfd);
			continue;
		}
		if (srv->mode == MODE_EMPTY_401) {
			send_http(cfd, 401, NULL, "", 0);
			close(cfd);
			continue;
		}
		if (srv->mode == MODE_GZIP_DYNAMIC) {
			send_http(cfd, 200, "Content-Encoding: gzip\r\n", FEATURED_GZ, sizeof(FEATURED_GZ));
			close(cfd);
			continue;
		}
		if (srv->mode == MODE_ZLIB) {
			send_http(cfd, 200, "Content-Encoding: deflate\r\n", FEATURED_ZLIB, sizeof(FEATURED_ZLIB));
			close(cfd);
			continue;
		}
		if (srv->mode == MODE_RAW_DEFLATE) {
			send_http(cfd, 200, "Content-Encoding: deflate\r\n", FEATURED_RAW_DEFLATE, sizeof(FEATURED_RAW_DEFLATE));
			close(cfd);
			continue;
		}
		if (srv->mode == MODE_GZIP_MULTIMEMBER_AND_FLAGS) {
			const char *part1 = "{\"albums\":{\"items\":[{\"id\":\"001\",\"title\":\"Part One\",";
			const char *part2 = "\"artist\":{\"name\":\"Tester\"},\"tracks_count\":8,\"hires\":true}]}}";
			size_t g1_len = 0, g2_len = 0;
			uint8_t *g1 = gzip_wrap_stored((const uint8_t *)part1, strlen(part1), true, &g1_len);
			uint8_t *g2 = gzip_wrap_stored((const uint8_t *)part2, strlen(part2), false, &g2_len);
			uint8_t *both = malloc(g1_len + g2_len);
			assert(both);
			memcpy(both, g1, g1_len);
			memcpy(both + g1_len, g2, g2_len);
			send_http(cfd, 200, "Content-Encoding: gzip\r\n", both, g1_len + g2_len);
			free(both);
			free(g1);
			free(g2);
			close(cfd);
			continue;
		}
		if (srv->mode == MODE_GZIP_LARGE_3MB) {
			// Build a ~2.8 MB 50-track JSON payload (exceeding the old 2 MB cap)
			// where every track carries a ~55 KB editorial album description.
			size_t big_cap = 3200000;
			char *big = malloc(big_cap);
			char *filler = malloc(55000);
			assert(big && filler);
			memset(filler, 'A', 54999);
			filler[54999] = '\0';

			size_t pos = (size_t)snprintf(big, big_cap, "{\"tracks\":{\"items\":[");
			for (int i = 0; i < 50; i++) {
				pos += (size_t)snprintf(
					big + pos, big_cap - pos,
					"%s{\"id\":%d,\"title\":\"Track %d\",\"duration\":245,\"track_number\":%d,"
					"\"hires\":true,\"streamable\":true,\"maximum_bit_depth\":24,\"maximum_sampling_rate\":96.0,"
					"\"performer\":{\"name\":\"Performer %d\"},"
					"\"album\":{\"id\":\"alb%d\",\"title\":\"Big Album %d\",\"description\":\"%s\","
					"\"image\":{\"large\":\"https://static.qobuz.com/%d.jpg\"}}}",
					i ? "," : "", 1000 + i, i, i + 1, i, i, i, filler, i);
			}
			pos += (size_t)snprintf(big + pos, big_cap - pos, "]}}");
			assert(pos > 2700000 && pos < big_cap);

			size_t gz_len = 0;
			uint8_t *gz = gzip_wrap_stored((const uint8_t *)big, pos, false, &gz_len);
			send_http(cfd, 200, "Content-Encoding: gzip\r\n", gz, gz_len);
			free(gz);
			free(filler);
			free(big);
			close(cfd);
			continue;
		}
		if (srv->mode == MODE_OVERSIZED_HEADERS) {
			// Send > 12 KB of Set-Cookie / X-Pad headers past HTTP_HEADER_BUFFER (8192).
			const char *json = "{\"albums\":{\"items\":[{\"id\":\"hdr1\",\"title\":\"Past 8KB Headers\","
							   "\"artist\":{\"name\":\"Edge CDN\"},\"tracks_count\":5}]}}";
			char *hdr = malloc(16384);
			assert(hdr);
			int hn = snprintf(hdr, 16384,
							  "HTTP/1.1 200 OK\r\n"
							  "Content-Type: application/json\r\n"
							  "Content-Length: %zu\r\n"
							  "Connection: close\r\n",
							  strlen(json));
			for (int i = 0; i < 120; i++) {
				hn += snprintf(hdr + hn, 16384 - (size_t)hn,
							   "X-Cloud-Cookie-%03d: 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
							   "0123456789abcdef0123456789abcdef\r\n",
							   i);
			}
			hn += snprintf(hdr + hn, 16384 - (size_t)hn, "\r\n");
			assert(hn > 12000 && hn < 16384);
			send_all(cfd, hdr, (size_t)hn);
			send_all(cfd, json, strlen(json));
			free(hdr);
			close(cfd);
			continue;
		}
		if (srv->mode == MODE_UTF8_BOM) {
			const char *raw_bom = "\xEF\xBB\xBF{\"albums\":{\"items\":[{\"id\":\"bom1\",\"title\":\"BOM Reply\"}]}}";
			send_http(cfd, 200, NULL, raw_bom, strlen(raw_bom));
			close(cfd);
			continue;
		}

		// MODE_NORMAL endpoint routing:
		if (strncmp(req, "GET /api.json/0.2/favorite/getUserFavorites?", 44) == 0) {
			// Verify paged qobuz_favorite_track_ids never asks for > 50 at once.
			const char *lim_p = strstr(req, "limit=");
			const char *off_p = strstr(req, "offset=");
			int limit = lim_p ? atoi(lim_p + 6) : 50;
			int offset = off_p ? atoi(off_p + 7) : 0;
			assert(limit <= 50);
			srv->fav_id_pages++;

			char *buf = malloc(16384);
			assert(buf);
			size_t pos = (size_t)snprintf(buf, 16384, "{\"tracks\":{\"items\":[");
			int total = 115;
			int count = total - offset;
			if (count < 0) count = 0;
			if (count > limit) count = limit;
			for (int i = 0; i < count; i++) {
				pos += (size_t)snprintf(buf + pos, 16384 - pos, "%s{\"id\":%d,\"title\":\"Fav %d\"}", i ? "," : "",
										5000 + offset + i, offset + i);
			}
			pos += (size_t)snprintf(buf + pos, 16384 - pos, "]}}");
			send_http(cfd, 200, NULL, buf, pos);
			free(buf);
		} else if (strncmp(req, "GET /api.json/0.2/favorite/create?", 34) == 0 ||
				   strncmp(req, "GET /api.json/0.2/playlist/create?", 34) == 0 ||
				   strncmp(req, "GET /api.json/0.2/playlist/addTracks?", 37) == 0 ||
				   strncmp(req, "GET /api.json/0.2/track/getFileUrl?", 35) == 0) {
			// Query-string endpoints MUST carry the full percent-encoded token
			// (not chopped at %.255s and with '+'/'/'/'=' encoded).
			char needle[3700];
			snprintf(needle, sizeof(needle), "user_auth_token=%s", srv->encoded_token);
			assert(strstr(req, needle) != NULL);

			if (strncmp(req, "GET /api.json/0.2/track/getFileUrl?", 35) == 0) {
				assert(strstr(req, "&format_id=27") != NULL);
				assert(strstr(req, "&intent=stream") != NULL);
				assert(strstr(req, "&track_id=777") != NULL);
				const char *reply =
					"{\"url\":\"https://streaming-qobuz-std.akamaized.net/file.flac\","
					"\"mime_type\":\"audio/flac\",\"format_id\":27}";
				send_http(cfd, 200, NULL, reply, strlen(reply));
			} else {
				const char *reply = "{\"id\":4242,\"status\":\"success\"}";
				send_http(cfd, 200, NULL, reply, strlen(reply));
			}
		} else {
			const char *reply = "{\"playlists\":{\"items\":[{\"id\":99,\"name\":\"My Mix\",\"tracks_count\":3}]}}";
			send_http(cfd, 200, NULL, reply, strlen(reply));
		}

		close(cfd);
	}

	free(req);
	return NULL;
}

int main(void) {
	char tmpdir[] = "/tmp/test_qobuz_XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	char keys_path[512];
	char cfg_path[512];
	char sess_path[512];
	snprintf(keys_path, sizeof(keys_path), "%s/streaming-keys.ini", tmpdir);
	snprintf(cfg_path, sizeof(cfg_path), "%s/device_config.ini", tmpdir);
	snprintf(sess_path, sizeof(sess_path), "%s/qobuz_session.ini", tmpdir);

	fake_qobuz_t srv;
	memset(&srv, 0, sizeof(srv));
	snprintf(srv.expected_app_id, sizeof(srv.expected_app_id), "%s", "123456789");
	snprintf(srv.expected_secret, sizeof(srv.expected_secret), "%s", "0123456789abcdef0123456789abcdef");

	// Build a 960-character JWT-shaped token containing '+', '/', and '=' so
	// both >255-byte truncation and query-string base64 corruption are tested.
	strcpy(srv.issued_token, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.");
	while (strlen(srv.issued_token) < 920) {
		strcat(srv.issued_token, "AbCdEfGhIjKlMnOpQrStUvWxYz012345+/");
	}
	strcat(srv.issued_token, "==.SigTailEnd99");
	assert(strlen(srv.issued_token) > 900);
	http_url_encode(srv.issued_token, srv.encoded_token, sizeof(srv.encoded_token));

	FILE *kf = fopen(keys_path, "w");
	assert(kf);
	fprintf(kf, "[qobuz]\napp_id = %s\napp_secret = %s\n", srv.expected_app_id, srv.expected_secret);
	fclose(kf);
	assert(streamkeys_load_card(tmpdir));

	srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(srv.listen_fd >= 0);
	int one = 1;
	setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		.sin_port = 0,
	};
	assert(bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	socklen_t alen = sizeof(addr);
	assert(getsockname(srv.listen_fd, (struct sockaddr *)&addr, &alen) == 0);
	srv.port = ntohs(addr.sin_port);
	assert(listen(srv.listen_fd, 16) == 0);

	pthread_t thr;
	assert(pthread_create(&thr, NULL, server_main, &srv) == 0);

	char base[128];
	snprintf(base, sizeof(base), "http://127.0.0.1:%d/api.json/0.2", srv.port);

	// 1. Legacy 127-char truncated token in device_config.ini must be dropped on init.
	{
		char truncated127[128];
		memset(truncated127, 'x', 127);
		truncated127[127] = '\0';
		FILE *cf = fopen(cfg_path, "w");
		assert(cf);
		fprintf(cf, "[qobuz]\nbase_url = %s\ntoken = %s\nuser = OldUser\n", base, truncated127);
		fclose(cf);

		config_init(cfg_path);
		qobuz_init();
		assert(qobuz_configured());
		assert(!qobuz_logged_in());
	}

	// 2. Login with a >900-char JWT token succeeds, persists to qobuz_session.ini,
	//    survives a simulated reboot, and sends the full un-truncated token on
	//    every header and query-string endpoint.
	assert(qobuz_login("listener@example.com", "hunter2"));
	assert(qobuz_logged_in());
	assert(strcmp(qobuz_display_name(), "HiFi Listener") == 0);

	// Simulate reboot: reload config_init + qobuz_init from disk.
	config_init(cfg_path);
	qobuz_init();
	assert(qobuz_logged_in());
	assert(strcmp(qobuz_display_name(), "HiFi Listener") == 0);

	qobuz_playlist_t pls[8];
	assert(qobuz_user_playlists(0, pls, 8) == 1);
	assert(pls[0].id == 99 && strcmp(pls[0].name, "My Mix") == 0);

	assert(qobuz_favorite_add_track(12345));
	long created_pl = 0;
	assert(qobuz_playlist_create("Road Trip", &created_pl) && created_pl == 4242);
	assert(qobuz_playlist_add_track(created_pl, 12345));

	char stream_url[256] = "";
	char mime[64] = "";
	int fmt = 0;
	assert(qobuz_track_url(777, stream_url, sizeof(stream_url), mime, sizeof(mime), &fmt));
	assert(strcmp(stream_url, "https://streaming-qobuz-std.akamaized.net/file.flac") == 0);
	assert(strcmp(mime, "audio/flac") == 0 && fmt == 27);

	// 3. Paged qobuz_favorite_track_ids(ids, 120) fetches 3 pages of <=50 items.
	long fav_ids[120];
	srv.fav_id_pages = 0;
	int fav_n = qobuz_favorite_track_ids(fav_ids, 120);
	assert(fav_n == 115);
	assert(srv.fav_id_pages == 3);
	assert(fav_ids[0] == 5000 && fav_ids[114] == 5114);

	// 4. Content-Encoding: gzip (dynamic Huffman), deflate (zlib & raw), multi-member
	//    gzip with optional header fields, and a >2.7 MB 50-track gzipped payload.
	qobuz_album_t albums[20];
	srv.mode = MODE_GZIP_DYNAMIC;
	assert(qobuz_featured_albums("new-releases", 0, albums, 20) == 12);
	assert(strcmp(albums[0].id_text, "0060254798000") == 0);
	assert(strcmp(albums[11].title, "Album 11") == 0);

	srv.mode = MODE_ZLIB;
	assert(qobuz_featured_albums("new-releases", 0, albums, 20) == 12);
	assert(strcmp(albums[5].artist, "Artist 5") == 0);

	srv.mode = MODE_RAW_DEFLATE;
	assert(qobuz_featured_albums("new-releases", 0, albums, 20) == 12);

	srv.mode = MODE_GZIP_MULTIMEMBER_AND_FLAGS;
	assert(qobuz_featured_albums("new-releases", 0, albums, 20) == 1);
	assert(strcmp(albums[0].title, "Part One") == 0 && albums[0].track_count == 8);

	qobuz_track_t tracks[50];
	srv.mode = MODE_GZIP_LARGE_3MB;
	assert(qobuz_favorite_tracks(0, tracks, 50) == 50);
	assert(tracks[0].id == 1000 && tracks[49].id == 1049);
	assert(strcmp(tracks[49].album, "Big Album 49") == 0);

	// 5. >12 KB HTTP response headers must not spill into JSON body.
	srv.mode = MODE_OVERSIZED_HEADERS;
	assert(qobuz_featured_albums("new-releases", 0, albums, 20) == 1);
	assert(strcmp(albums[0].title, "Past 8KB Headers") == 0);

	// 6. UTF-8 BOM before JSON root object is accepted cleanly.
	srv.mode = MODE_UTF8_BOM;
	assert(qobuz_featured_albums("new-releases", 0, albums, 20) == 1);
	assert(strcmp(albums[0].title, "BOM Reply") == 0);

	// 7. HTML 403 surfaces HTTP status ("Qobuz answered 403") instead of
	//    "Unreadable Qobuz reply", while keeping session intact.
	srv.mode = MODE_HTML_403;
	assert(qobuz_featured_albums("new-releases", 0, albums, 20) == -1);
	assert(strcmp(qobuz_last_error(), "Qobuz answered 403") == 0);
	assert(qobuz_logged_in());

	// 8. Empty-body 401 closes the rejected session and reports 401 cleanly.
	srv.mode = MODE_EMPTY_401;
	assert(qobuz_favorite_tracks(0, tracks, 10) == -1);
	assert(strcmp(qobuz_last_error(), "Qobuz answered 401") == 0);
	assert(!qobuz_logged_in());

	srv.stop = true;
	shutdown(srv.listen_fd, SHUT_RDWR);
	close(srv.listen_fd);
	pthread_join(thr, NULL);

	unlink(keys_path);
	unlink(cfg_path);
	unlink(sess_path);
	rmdir(tmpdir);

	printf("test_qobuz: all assertions passed\n");
	return 0;
}
