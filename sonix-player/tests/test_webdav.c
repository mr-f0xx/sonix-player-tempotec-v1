// End-to-end integration test for WebDAV (`webdav.c`, `webdavstream.c`,
// `webdavcache.c`, `webdavdl.c`, `http.c` 207/PROPFIND, `streambridge_start_ex`,
// `growfile.c`, and `metadata.c`).
//
// Runs a real loopback HTTP/1.1 WebDAV mock server on an ephemeral port and
// exercises account/credential security, RFC 4918 PROPFIND Depth 0/1 + 302
// redirects + XML namespace/multi-propstat edge cases, Mode 1 RAM-ring
// streaming, Mode 2 progressive SD-card growfile caching + LRU eviction +
// magic-byte verification + tag preservation, and permanent SD downloads.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "src/system/core/config.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/growfile.h"
#include "src/system/decode/opusdec.h"
#include "src/system/decode/wavpackdec.h"
#include "src/system/library/metadata.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streambridge.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/webdav.h"
#include "src/system/streaming/webdavcache.h"
#include "src/system/streaming/webdavdl.h"
#include "src/system/streaming/webdavstream.h"

// ---------------------------------------------------------------------------
// Minimal device/audio stubs for standalone host linking
// ---------------------------------------------------------------------------

static char test_sd_root[512];
static bool test_sd_attached = true;
static bool test_sd_writable = true;
static bool test_usb_active = false;
static char test_playing_file[560];
static int test_forget_calls;

const char *storage_sd_root(void) { return test_sd_attached ? test_sd_root : ""; }
bool storage_card_attached(void) { return test_sd_attached; }
bool storage_sd_writable(void) { return test_sd_attached && test_sd_writable; }
bool usb_storage_active(void) { return test_usb_active; }
void audio_get_current_file(char *out, size_t out_size) {
	if (out && out_size) {
		snprintf(out, out_size, "%s", test_playing_file);
	}
}
int library_forget_file(const char *filepath) {
	(void)filepath;
	test_forget_calls++;
	return 1;
}
const char *tr(const char *key) { return key ? key : ""; }

// Unused streaming stubs referenced by metadata.c / decode.c
bool subsonicstream_owns(const char *p) { (void)p; return false; }
bool subsonicstream_metadata(const char *p, song_metadata_t *o) { (void)p; (void)o; return false; }
streambridge_t *subsonicstream_acquire(const char *p, int *d, int *b) { (void)p; (void)d; (void)b; return NULL; }
void subsonicstream_release(streambridge_t *b) { (void)b; }
bool qobuzcache_owns(const char *p) { (void)p; return false; }
bool qobuzcache_read_tags(const char *p, song_metadata_t *o) { (void)p; (void)o; return false; }
bool tidalcache_owns(const char *p) { (void)p; return false; }
bool tidalcache_read_tags(const char *p, song_metadata_t *o) { (void)p; (void)o; return false; }
bool soundcloudcache_owns(const char *p) { (void)p; return false; }
bool soundcloudcache_read_tags(const char *p, song_metadata_t *o) { (void)p; (void)o; return false; }
opusdec_t *opusdec_open(const char *p) { (void)p; return NULL; }
opusdec_t *opusdec_open_stream(void *s, int (*r)(void *, unsigned char *, int)) { (void)s; (void)r; return NULL; }
void opusdec_close(opusdec_t *o) { (void)o; }
int opusdec_channels(const opusdec_t *o) { (void)o; return 0; }
int opusdec_sample_rate(const opusdec_t *o) { (void)o; return 0; }
uint64_t opusdec_total_frames(const opusdec_t *o) { (void)o; return 0; }
uint64_t opusdec_read_s16(opusdec_t *o, uint64_t f, short *out) { (void)o; (void)f; (void)out; return 0; }
bool opusdec_seek(opusdec_t *o, uint64_t f) { (void)o; (void)f; return false; }
wavpackdec_t *wavpackdec_open(const char *p) { (void)p; return NULL; }
void wavpackdec_close(wavpackdec_t *w) { (void)w; }
int wavpackdec_channels(const wavpackdec_t *w) { (void)w; return 0; }
int wavpackdec_sample_rate(const wavpackdec_t *w) { (void)w; return 0; }
int wavpackdec_bits(const wavpackdec_t *w) { (void)w; return 0; }
uint64_t wavpackdec_total_frames(const wavpackdec_t *w) { (void)w; return 0; }
uint64_t wavpackdec_read_s16(wavpackdec_t *w, uint64_t f, short *out) { (void)w; (void)f; (void)out; return 0; }
uint64_t wavpackdec_read_s32(wavpackdec_t *w, uint64_t f, int32_t *out) { (void)w; (void)f; (void)out; return 0; }
bool wavpackdec_seek(wavpackdec_t *w, uint64_t f) { (void)w; (void)f; return false; }

// ---------------------------------------------------------------------------
// Helpers to synthesize valid audio files (MP3 with/without ID3v2, WAV, FLAC)
// ---------------------------------------------------------------------------

// Valid MPEG1 Layer3 128kbps 44.1kHz stereo silence frame (417 bytes):
// Header FF FB 90 64
static void fill_mp3_frames(uint8_t *buf, size_t len) {
	size_t pos = 0;
	while (pos < len) {
		size_t frame = 417;
		if (pos + frame > len) {
			memset(buf + pos, 0, len - pos);
			if (len - pos >= 4) {
				buf[pos] = 0xFF;
				buf[pos + 1] = 0xFB;
				buf[pos + 2] = 0x90;
				buf[pos + 3] = 0x64;
			}
			break;
		}
		memset(buf + pos, 0, frame);
		buf[pos] = 0xFF;
		buf[pos + 1] = 0xFB;
		buf[pos + 2] = 0x90;
		buf[pos + 3] = 0x64;
		pos += frame;
	}
}

static size_t build_tagged_mp3(uint8_t *out, size_t cap, const char *title, const char *artist) {
	size_t tlen = strlen(title) + 1;  // encoding byte + UTF-8
	size_t alen = strlen(artist) + 1;
	size_t frames_len = (10 + tlen) + (10 + alen);
	assert(10 + frames_len + 834 <= cap);

	// ID3v2.3 header with synchsafe tag size
	memcpy(out, "ID3\x03\x00\x00", 6);
	out[6] = (uint8_t)((frames_len >> 21) & 0x7F);
	out[7] = (uint8_t)((frames_len >> 14) & 0x7F);
	out[8] = (uint8_t)((frames_len >> 7) & 0x7F);
	out[9] = (uint8_t)(frames_len & 0x7F);

	size_t p = 10;
	memcpy(out + p, "TIT2", 4);
	out[p + 4] = 0; out[p + 5] = 0; out[p + 6] = 0; out[p + 7] = (uint8_t)tlen;
	out[p + 8] = 0; out[p + 9] = 0;
	out[p + 10] = 0x03; // UTF-8
	memcpy(out + p + 11, title, tlen - 1);
	p += 10 + tlen;

	memcpy(out + p, "TPE1", 4);
	out[p + 4] = 0; out[p + 5] = 0; out[p + 6] = 0; out[p + 7] = (uint8_t)alen;
	out[p + 8] = 0; out[p + 9] = 0;
	out[p + 10] = 0x03; // UTF-8
	memcpy(out + p + 11, artist, alen - 1);
	p += 10 + alen;

	size_t audio_bytes = cap - p;
	fill_mp3_frames(out + p, audio_bytes);
	return p + audio_bytes;
}

static size_t build_pcm_wav(uint8_t *out, size_t pcm_bytes) {
	uint32_t riff_size = (uint32_t)(36 + pcm_bytes);
	uint32_t data_size = (uint32_t)pcm_bytes;
	uint32_t fmt_size = 16;
	uint16_t audio_fmt = 1, channels = 2, block_align = 4, bits = 16;
	uint32_t sample_rate = 44100, byte_rate = 44100 * 4;

	memcpy(out, "RIFF", 4);
	memcpy(out + 4, &riff_size, 4);
	memcpy(out + 8, "WAVEfmt ", 8);
	memcpy(out + 16, &fmt_size, 4);
	memcpy(out + 20, &audio_fmt, 2);
	memcpy(out + 22, &channels, 2);
	memcpy(out + 24, &sample_rate, 4);
	memcpy(out + 28, &byte_rate, 4);
	memcpy(out + 32, &block_align, 2);
	memcpy(out + 34, &bits, 2);
	memcpy(out + 36, "data", 4);
	memcpy(out + 40, &data_size, 4);
	for (size_t i = 0; i < pcm_bytes; i++) {
		out[44 + i] = (uint8_t)(i & 0xFF);
	}
	return 44 + pcm_bytes;
}

// ---------------------------------------------------------------------------
// Loopback HTTP/1.1 WebDAV Mock Server
// ---------------------------------------------------------------------------

static int srv_listen_fd = -1;
static int srv_port;
static pthread_t srv_thread;
static volatile bool srv_stop;

static pthread_mutex_t srv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t srv_cv = PTHREAD_COND_INITIALIZER;
static bool srv_require_auth = true;
// "alice:s3cr3t!" -> Basic YWxpY2U6czNjcjN0IQ==
static const char EXPECTED_AUTH[] = "Basic YWxpY2U6czNjcjN0IQ==";
static int srv_propfind_count;
static int srv_get_count;
static int srv_auth_checked_count;
static bool srv_slow_grow;
static bool srv_grow_in_progress;
static bool srv_release_grow;
static bool srv_truncate_next;
static bool srv_stall_next;
static bool srv_stalled_now;

static uint8_t blob_untagged_mp3[768 * 1024];
static uint8_t blob_tagged_mp3[64 * 1024];
static size_t blob_tagged_mp3_len;
static uint8_t blob_wav[64 * 1024];
static size_t blob_wav_len;

static bool send_all(int fd, const void *buf, size_t len) {
	const uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n <= 0) {
			return false;
		}
		p += (size_t)n;
		len -= (size_t)n;
	}
	return true;
}

static void send_simple_response(int fd, const char *status, const char *ctype,
								 const char *extra_hdr, const void *body, size_t body_len) {
	char hdr[1024];
	int n = snprintf(hdr, sizeof(hdr),
					 "HTTP/1.1 %s\r\n"
					 "Content-Type: %s\r\n"
					 "Content-Length: %zu\r\n"
					 "Connection: close\r\n"
					 "%s"
					 "\r\n",
					 status, ctype, body_len, extra_hdr ? extra_hdr : "");
	if (send_all(fd, hdr, (size_t)n) && body_len > 0) {
		send_all(fd, body, body_len);
	}
}

static const char XML_ROOT[] =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
	"<d:multistatus xmlns:d=\"DAV:\">"
	"  <d:response>"
	"    <d:href>/remote.php/dav/files/alice/Music/</d:href>"
	"    <d:propstat>"
	"      <d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop>"
	"      <d:status>HTTP/1.1 200 OK</d:status>"
	"    </d:propstat>"
	"  </d:response>"
	"  <d:response>"
	"    <d:href>/remote.php/dav/files/alice/Music/Jazz%%20%%26%%20Blues/</d:href>"
	"    <d:propstat>"
	"      <d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop>"
	"      <d:status>HTTP/1.1 200 OK</d:status>"
	"    </d:propstat>"
	"    <d:propstat>"
	"      <d:prop><d:getcontentlength/></d:prop>"
	"      <d:status>HTTP/1.1 404 Not Found</d:status>"
	"    </d:propstat>"
	"  </d:response>"
	"  <d:response>"
	"    <d:href>/remote.php/dav/files/alice/Music/.DS_Store</d:href>"
	"    <d:propstat>"
	"      <d:prop><d:resourcetype/><d:getcontentlength>4096</d:getcontentlength></d:prop>"
	"      <d:status>HTTP/1.1 200 OK</d:status>"
	"    </d:propstat>"
	"  </d:response>"
	"  <d:response>"
	"    <d:href>/remote.php/dav/files/alice/Music/cover.jpg</d:href>"
	"    <d:propstat>"
	"      <d:prop><d:resourcetype/><d:getcontentlength>9999</d:getcontentlength></d:prop>"
	"      <d:status>HTTP/1.1 200 OK</d:status>"
	"    </d:propstat>"
	"  </d:response>"
	"  <d:response>"
	"    <d:href>/remote.php/dav/files/alice/Music/10%%20-%%20Tagged.mp3</d:href>"
	"    <d:propstat>"
	"      <d:prop><d:resourcetype/><d:getcontentlength>%zu</d:getcontentlength></d:prop>"
	"      <d:status>HTTP/1.1 200 OK</d:status>"
	"    </d:propstat>"
	"  </d:response>"
	"  <d:response>"
	"    <d:href>/remote.php/dav/files/alice/Music/2%%20-%%20Untagged%%20%%231.mp3</d:href>"
	"    <d:propstat>"
	"      <d:prop><d:resourcetype/><d:getcontentlength>%zu</d:getcontentlength></d:prop>"
	"      <d:status>HTTP/1.1 200 OK</d:status>"
	"    </d:propstat>"
	"  </d:response>"
	"  <d:response>"
	"    <d:href>/remote.php/dav/files/alice/Music/HiRes.wav</d:href>"
	"    <d:propstat>"
	"      <d:prop><d:resourcetype/><d:getcontentlength>%zu</d:getcontentlength></d:prop>"
	"      <d:status>HTTP/1.1 200 OK</d:status>"
	"    </d:propstat>"
	"  </d:response>"
	"  <d:response>"
	"    <d:href>/remote.php/dav/files/alice/Music/FakeAudio.mp3</d:href>"
	"    <d:propstat>"
	"      <d:prop><d:resourcetype/><d:getcontentlength>64</d:getcontentlength></d:prop>"
	"      <d:status>HTTP/1.1 200 OK</d:status>"
	"    </d:propstat>"
	"  </d:response>"
	"</d:multistatus>";

// Default-namespace XML (no `D:` prefix) + nested subfolder for recursive download test
static const char XML_JAZZ[] =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
	"<multistatus xmlns=\"DAV:\">"
	"  <response>"
	"    <href>/remote.php/dav/files/alice/Music/Jazz%%20%%26%%20Blues</href>"
	"    <propstat><prop><resourcetype><collection/></resourcetype></prop><status>HTTP/1.1 200 OK</status></propstat>"
	"  </response>"
	"  <response>"
	"    <href>/remote.php/dav/files/alice/Music/Jazz%%20%%26%%20Blues/Live%%3A%%201959/</href>"
	"    <propstat><prop><resourcetype><collection/></resourcetype></prop><status>HTTP/1.1 200 OK</status></propstat>"
	"  </response>"
	"  <response>"
	"    <href>/remote.php/dav/files/alice/Music/Jazz%%20%%26%%20Blues/01%%20-%%20Blue%%20in%%20Green.mp3</href>"
	"    <propstat><prop><resourcetype/><getcontentlength>%zu</getcontentlength></prop></propstat>"
	"  </response>"
	"</multistatus>";

static const char XML_JAZZ_LIVE[] =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
	"<D:multistatus xmlns:D=\"DAV:\">"
	"  <D:response>"
	"    <D:href>/remote.php/dav/files/alice/Music/Jazz%%20%%26%%20Blues/Live%%3A%%201959/</D:href>"
	"    <D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
	"  </D:response>"
	"  <D:response>"
	"    <D:href>/remote.php/dav/files/alice/Music/Jazz%%20%%26%%20Blues/Live%%3A%%201959/02%%20-%%20So%%20What%%3F.wav</D:href>"
	"    <D:propstat><D:prop><D:resourcetype/><D:getcontentlength>%zu</D:getcontentlength></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
	"  </D:response>"
	"</D:multistatus>";

static void handle_client(int fd) {
	char req[8192];
	size_t used = 0;
	char *hdr_end = NULL;
	while (used + 1 < sizeof(req)) {
		ssize_t n = recv(fd, req + used, sizeof(req) - 1 - used, 0);
		if (n <= 0) {
			return;
		}
		used += (size_t)n;
		req[used] = '\0';
		hdr_end = strstr(req, "\r\n\r\n");
		if (hdr_end) {
			break;
		}
	}
	if (!hdr_end) {
		return;
	}

	// Read declared request body if Content-Length > 0 (e.g. PROPFIND XML body)
	size_t content_len = 0;
	for (const char *line = req; line < hdr_end;) {
		const char *eol = strstr(line, "\r\n");
		if (!eol) break;
		if (strncasecmp(line, "Content-Length:", 15) == 0) {
			content_len = (size_t)strtoul(line + 15, NULL, 10);
		}
		line = eol + 2;
	}
	size_t body_already = (size_t)(req + used - (hdr_end + 4));
	while (body_already < content_len && used + 1 < sizeof(req)) {
		ssize_t n = recv(fd, req + used, sizeof(req) - 1 - used, 0);
		if (n <= 0) break;
		used += (size_t)n;
		body_already += (size_t)n;
		req[used] = '\0';
	}

	char method[32] = {0}, path[1024] = {0};
	if (sscanf(req, "%31s %1023s", method, path) != 2) {
		return;
	}

	// Verify credentials never leaked into URI query or path
	assert(strstr(path, "s3cr3t") == NULL);

	bool has_valid_auth = false;
	bool has_any_auth = false;
	int depth = -1;
	for (const char *line = req; line < hdr_end;) {
		const char *eol = strstr(line, "\r\n");
		if (!eol) break;
		if (strncasecmp(line, "Authorization:", 14) == 0) {
			has_any_auth = true;
			const char *v = line + 14;
			while (*v == ' ' || *v == '\t') v++;
			size_t vlen = (size_t)(eol - v);
			if (vlen == strlen(EXPECTED_AUTH) && strncmp(v, EXPECTED_AUTH, vlen) == 0) {
				has_valid_auth = true;
			}
		} else if (strncasecmp(line, "Depth:", 6) == 0) {
			depth = atoi(line + 6);
		}
		line = eol + 2;
	}

	pthread_mutex_lock(&srv_lock);
	bool need_auth = srv_require_auth;
	if (need_auth) {
		if (!has_valid_auth) {
			pthread_mutex_unlock(&srv_lock);
			send_simple_response(fd, "401 Unauthorized", "text/plain",
								 "WWW-Authenticate: Basic realm=\"WebDAV\"\r\n", "denied", 6);
			return;
		}
		srv_auth_checked_count++;
	} else {
		// In anonymous test mode, client must NOT send Authorization header
		assert(!has_any_auth);
	}
	pthread_mutex_unlock(&srv_lock);

	// Redirect endpoint to verify HTTP 302 preserves PROPFIND + Depth + XML body
	if (strcmp(path, "/dav-redirect") == 0 || strcmp(path, "/dav-redirect/") == 0) {
		char loc[256];
		snprintf(loc, sizeof(loc), "Location: http://127.0.0.1:%d/remote.php/dav/files/alice/Music/\r\n", srv_port);
		send_simple_response(fd, "302 Found", "text/plain", loc, "", 0);
		return;
	}

	// Non-WebDAV endpoint returning 405 Method Not Allowed
	if (strncmp(path, "/not-webdav", 11) == 0) {
		send_simple_response(fd, "405 Method Not Allowed", "text/plain", NULL, "no dav", 6);
		return;
	}

	if (strcmp(method, "PROPFIND") == 0) {
		assert(depth == 0 || depth == 1);
		assert(strstr(hdr_end + 4, "propfind") != NULL);
		pthread_mutex_lock(&srv_lock);
		srv_propfind_count++;
		pthread_mutex_unlock(&srv_lock);

		char xml[8192];
		if (strcmp(path, "/remote.php/dav/files/alice/Music") == 0 ||
			strcmp(path, "/remote.php/dav/files/alice/Music/") == 0) {
			int n = snprintf(xml, sizeof(xml), XML_ROOT,
							 blob_tagged_mp3_len, sizeof(blob_untagged_mp3), blob_wav_len);
			send_simple_response(fd, "207 Multi-Status", "application/xml; charset=utf-8", NULL, xml, (size_t)n);
			return;
		}
		if (strcmp(path, "/remote.php/dav/files/alice/Music/Jazz%20%26%20Blues") == 0 ||
			strcmp(path, "/remote.php/dav/files/alice/Music/Jazz%20%26%20Blues/") == 0) {
			int n = snprintf(xml, sizeof(xml), XML_JAZZ, blob_tagged_mp3_len);
			send_simple_response(fd, "207 Multi-Status", "application/xml; charset=utf-8", NULL, xml, (size_t)n);
			return;
		}
		if (strcmp(path, "/remote.php/dav/files/alice/Music/Jazz%20%26%20Blues/Live%3A%201959") == 0 ||
			strcmp(path, "/remote.php/dav/files/alice/Music/Jazz%20%26%20Blues/Live%3A%201959/") == 0) {
			int n = snprintf(xml, sizeof(xml), XML_JAZZ_LIVE, blob_wav_len);
			send_simple_response(fd, "207 Multi-Status", "application/xml; charset=utf-8", NULL, xml, (size_t)n);
			return;
		}
		send_simple_response(fd, "404 Not Found", "text/plain", NULL, "missing", 7);
		return;
	}

	if (strcmp(method, "GET") == 0) {
		pthread_mutex_lock(&srv_lock);
		srv_get_count++;
		bool slow = srv_slow_grow;
		bool trunc = srv_truncate_next;
		bool stall = srv_stall_next;
		srv_truncate_next = false;
		srv_stall_next = false;
		pthread_mutex_unlock(&srv_lock);

		if (strcmp(path, "/remote.php/dav/files/alice/Music/FakeAudio.mp3") == 0) {
			const char *html = "<html><body>Error login portal, definitely not audio!</body></html>";
			send_simple_response(fd, "200 OK", "text/html", NULL, html, strlen(html));
			return;
		}

		const uint8_t *payload = NULL;
		size_t payload_len = 0;
		const char *ctype = "application/octet-stream";

		if (strcmp(path, "/remote.php/dav/files/alice/Music/2%20-%20Untagged%20%231.mp3") == 0) {
			payload = blob_untagged_mp3;
			payload_len = sizeof(blob_untagged_mp3);
			ctype = "audio/mpeg";
		} else if (strcmp(path, "/remote.php/dav/files/alice/Music/10%20-%20Tagged.mp3") == 0 ||
				   strcmp(path, "/remote.php/dav/files/alice/Music/Jazz%20%26%20Blues/01%20-%20Blue%20in%20Green.mp3") == 0) {
			payload = blob_tagged_mp3;
			payload_len = blob_tagged_mp3_len;
			ctype = "audio/mpeg";
		} else if (strcmp(path, "/remote.php/dav/files/alice/Music/HiRes.wav") == 0 ||
				   strcmp(path, "/remote.php/dav/files/alice/Music/Jazz%20%26%20Blues/Live%3A%201959/02%20-%20So%20What%3F.wav") == 0) {
			payload = blob_wav;
			payload_len = blob_wav_len;
			ctype = "audio/wav";
		} else {
			send_simple_response(fd, "404 Not Found", "text/plain", NULL, "missing", 7);
			return;
		}

		char hdr[512];
		int hn = snprintf(hdr, sizeof(hdr),
						  "HTTP/1.1 200 OK\r\n"
						  "Content-Type: %s\r\n"
						  "Content-Length: %zu\r\n"
						  "Connection: close\r\n\r\n",
						  ctype, payload_len);
		if (!send_all(fd, hdr, (size_t)hn)) {
			return;
		}
		if (trunc) {
			send_all(fd, payload, payload_len / 3);
			return; // close early -> truncated transfer!
		}
		if (stall) {
			send_all(fd, payload, 4096);
			pthread_mutex_lock(&srv_lock);
			srv_stalled_now = true;
			pthread_cond_broadcast(&srv_cv);
			while (!srv_release_grow && !srv_stop) {
				pthread_cond_wait(&srv_cv, &srv_lock);
			}
			pthread_mutex_unlock(&srv_lock);
			return;
		}
		if (slow && payload_len > 576 * 1024) {
			// Send 576 KiB (> 512 KiB STREAM_PREBUFFER) so webdavcache_get returns while
			// the file is actively growing, then wait for main thread to inspect!
			size_t first = 576 * 1024;
			if (!send_all(fd, payload, first)) {
				return;
			}
			pthread_mutex_lock(&srv_lock);
			srv_grow_in_progress = true;
			pthread_cond_broadcast(&srv_cv);
			while (!srv_release_grow && !srv_stop) {
				pthread_cond_wait(&srv_cv, &srv_lock);
			}
			pthread_mutex_unlock(&srv_lock);
			send_all(fd, payload + first, payload_len - first);
			return;
		}
		send_all(fd, payload, payload_len);
	}
}

static void *server_main(void *arg) {
	(void)arg;
	while (!srv_stop) {
		int cfd = accept(srv_listen_fd, NULL, NULL);
		if (cfd < 0) {
			if (errno == EINTR) continue;
			break;
		}
		handle_client(cfd);
		close(cfd);
	}
	return NULL;
}

static void start_mock_server(void) {
	srv_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(srv_listen_fd >= 0);
	int yes = 1;
	setsockopt(srv_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	assert(bind(srv_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(srv_listen_fd, 16) == 0);

	socklen_t alen = sizeof(addr);
	assert(getsockname(srv_listen_fd, (struct sockaddr *)&addr, &alen) == 0);
	srv_port = ntohs(addr.sin_port);
	assert(pthread_create(&srv_thread, NULL, server_main, NULL) == 0);
}

static void stop_mock_server(void) {
	pthread_mutex_lock(&srv_lock);
	srv_stop = true;
	srv_release_grow = true;
	pthread_cond_broadcast(&srv_cv);
	pthread_mutex_unlock(&srv_lock);
	if (srv_listen_fd >= 0) {
		shutdown(srv_listen_fd, SHUT_RDWR);
		close(srv_listen_fd);
		srv_listen_fd = -1;
	}
	pthread_join(srv_thread, NULL);
}

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	(void)sb; (void)typeflag; (void)ftwbuf;
	return remove(fpath);
}

static void rm_rf(const char *path) {
	nftw(path, unlink_cb, 32, FTW_DEPTH | FTW_PHYS);
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

static void test_account_and_credentials(const char *cfg_dir) {
	char err[128] = {0};
	webdav_credentials_t cred = {0};

	// Reject malformed URLs and userinfo-in-URL
	snprintf(cred.server, sizeof(cred.server), "ftp://example.com/dav");
	assert(!webdav_configure(&cred, false, err, sizeof(err)));
	assert(strcmp(err, "webdav_invalid_url") == 0);

	snprintf(cred.server, sizeof(cred.server), "http://alice:secret@127.0.0.1:%d/dav", srv_port);
	assert(!webdav_configure(&cred, false, err, sizeof(err)));
	assert(strcmp(err, "webdav_invalid_url") == 0);

	// Session-only configuration (remember = false): no credential file written
	memset(&cred, 0, sizeof(cred));
	snprintf(cred.server, sizeof(cred.server),
			 "http://127.0.0.1:%d/remote.php/dav/files/alice/Music///", srv_port);
	snprintf(cred.username, sizeof(cred.username), "alice");
	snprintf(cred.password, sizeof(cred.password), "s3cr3t!");
	assert(webdav_configure(&cred, false, err, sizeof(err)));
	assert(webdav_configured());

	char cred_file[600];
	snprintf(cred_file, sizeof(cred_file), "%s/webdav-credentials.json", cfg_dir);
	struct stat st;
	assert(stat(cred_file, &st) != 0);

	// Persisted configuration (remember = true): 0600 JSON, never in device_config.ini
	assert(webdav_configure(&cred, true, err, sizeof(err)));
	assert(stat(cred_file, &st) == 0);
	assert((st.st_mode & 0777) == 0600);

	char ini_path[600];
	snprintf(ini_path, sizeof(ini_path), "%s/device_config.ini", cfg_dir);
	FILE *ini = fopen(ini_path, "r");
	if (ini) {
		char buf[4096];
		size_t n = fread(buf, 1, sizeof(buf) - 1, ini);
		buf[n] = '\0';
		fclose(ini);
		assert(strstr(buf, "s3cr3t") == NULL);
		assert(strstr(buf, "alice") == NULL);
	}

	// Reload from disk via webdav_init() and verify normalized URL (trailing slashes stripped)
	webdav_init();
	assert(webdav_configured());
	webdav_credentials_t loaded = {0};
	bool rem = false;
	webdav_credentials(&loaded, &rem);
	assert(rem);
	char expected_srv[256];
	snprintf(expected_srv, sizeof(expected_srv),
			 "http://127.0.0.1:%d/remote.php/dav/files/alice/Music", srv_port);
	assert(strcmp(loaded.server, expected_srv) == 0);
	assert(strcmp(loaded.username, "alice") == 0);
	assert(strcmp(loaded.password, "s3cr3t!") == 0);

	// Forget removes file and clears memory
	assert(webdav_forget());
	assert(!webdav_configured());
	assert(stat(cred_file, &st) != 0);

	// Anonymous server mode: omits Authorization header completely
	pthread_mutex_lock(&srv_lock);
	srv_require_auth = false;
	pthread_mutex_unlock(&srv_lock);
	memset(&cred, 0, sizeof(cred));
	snprintf(cred.server, sizeof(cred.server), "%s", expected_srv);
	assert(webdav_configure(&cred, false, err, sizeof(err)));
	assert(webdav_ping(err, sizeof(err)));

	// Restore authenticated mode
	pthread_mutex_lock(&srv_lock);
	srv_require_auth = true;
	pthread_mutex_unlock(&srv_lock);
	assert(!webdav_ping(err, sizeof(err)));
	assert(strcmp(err, "webdav_auth_failed") == 0);

	snprintf(cred.username, sizeof(cred.username), "alice");
	snprintf(cred.password, sizeof(cred.password), "s3cr3t!");
	assert(webdav_configure(&cred, true, err, sizeof(err)));
	printf("PASS account & credential security (0600 JSON, URL validation, anonymous & Basic auth)\n");
}

static void test_propfind_and_browse(void) {
	char err[128] = {0};

	// Verify 302 redirect preserves PROPFIND method, Depth header, XML body, and same-origin Auth
	webdav_credentials_t redir = {0};
	snprintf(redir.server, sizeof(redir.server), "http://127.0.0.1:%d/dav-redirect", srv_port);
	snprintf(redir.username, sizeof(redir.username), "alice");
	snprintf(redir.password, sizeof(redir.password), "s3cr3t!");
	assert(webdav_configure(&redir, false, err, sizeof(err)));
	assert(webdav_ping(err, sizeof(err)));

	// Non-WebDAV endpoint returns webdav_not_supported on 405
	snprintf(redir.server, sizeof(redir.server), "http://127.0.0.1:%d/not-webdav", srv_port);
	assert(webdav_configure(&redir, false, err, sizeof(err)));
	assert(!webdav_ping(err, sizeof(err)));
	assert(strcmp(err, "webdav_not_supported") == 0);

	// Configure primary Nextcloud-style subpath server
	snprintf(redir.server, sizeof(redir.server),
			 "http://127.0.0.1:%d/remote.php/dav/files/alice/Music", srv_port);
	assert(webdav_configure(&redir, true, err, sizeof(err)));
	assert(webdav_ping(err, sizeof(err)));

	// Browse root:
	// - Self-entry `/remote.php/dav/files/alice/Music/` skipped
	// - `.DS_Store` hidden dotfile skipped
	// - `cover.jpg` non-audio skipped
	// - `Jazz & Blues` folder kept despite second 404 propstat block, sorted first!
	// - Natural sort orders `2 - Untagged #1.mp3` BEFORE `10 - Tagged.mp3`!
	webdav_entry_t items[16];
	bool more = true;
	int n = webdav_browse("", 0, items, 16, &more, err, sizeof(err));
	assert(n == 5);
	assert(!more);

	assert(items[0].kind == WEBDAV_DIR);
	assert(strcmp(items[0].title, "Jazz & Blues") == 0);
	assert(strcmp(items[0].path, "Jazz & Blues") == 0);

	assert(items[1].kind == WEBDAV_TRACK);
	assert(strcmp(items[1].title, "2 - Untagged #1.mp3") == 0);
	assert(strcmp(items[1].suffix, "mp3") == 0);
	assert(items[1].size == sizeof(blob_untagged_mp3));

	assert(items[2].kind == WEBDAV_TRACK);
	assert(strcmp(items[2].title, "10 - Tagged.mp3") == 0);

	assert(items[3].kind == WEBDAV_TRACK);
	assert(strcmp(items[3].title, "FakeAudio.mp3") == 0);

	assert(items[4].kind == WEBDAV_TRACK);
	assert(strcmp(items[4].title, "HiRes.wav") == 0);

	// Pagination check (offset=1, limit=2 -> 2 items, more=true)
	n = webdav_browse("", 1, items, 2, &more, err, sizeof(err));
	assert(n == 2 && more);
	assert(strcmp(items[0].title, "2 - Untagged #1.mp3") == 0);
	assert(strcmp(items[1].title, "10 - Tagged.mp3") == 0);

	// Subfolder with `&` in name and default `xmlns="DAV:"` (no namespace prefix)
	n = webdav_browse("Jazz & Blues", 0, items, 16, &more, err, sizeof(err));
	assert(n == 2 && !more);
	assert(items[0].kind == WEBDAV_DIR && strcmp(items[0].title, "Live: 1959") == 0);
	assert(items[1].kind == WEBDAV_TRACK && strcmp(items[1].title, "01 - Blue in Green.mp3") == 0);
	assert(strcmp(items[1].album, "Jazz & Blues") == 0);

	// Path traversal rejected locally before touching network
	assert(webdav_browse("../etc", 0, items, 16, &more, err, sizeof(err)) < 0);
	assert(strcmp(err, "webdav_invalid_path") == 0);

	printf("PASS PROPFIND 207, 302 redirect, XML namespaces, multi-propstat, percent-decoding & natural sort\n");
}

static void test_ram_streaming(void) {
	webdav_entry_t entries[8];
	bool more = false;
	char err[128] = {0};
	int n = webdav_browse("", 0, entries, 8, &more, err, sizeof(err));
	assert(n == 5);

	char paths[8][96];
	const char *ptrs[8];
	int queued = 0, selected = 0;

	// Directly tapping WAV in RAM-stream mode asks user to enable SD cache
	assert(!webdavstream_queue(entries, n, 4 /* HiRes.wav */, paths, ptrs, &queued, &selected, err, sizeof(err)));
	assert(strcmp(err, "webdav_enable_sd_cache_for_format") == 0);

	// Tapping MP3 queues the MP3 tracks and skips the folder + WAV cleanly
	assert(webdavstream_queue(entries, n, 2 /* 10 - Tagged.mp3 */, paths, ptrs, &queued, &selected, err, sizeof(err)));
	assert(queued == 3);
	assert(selected == 1);
	assert(webdavstream_owns(ptrs[selected]));
	assert(strstr(ptrs[selected], "alice") == NULL && strstr(ptrs[selected], "s3cr3t") == NULL);

	// Metadata fallback strips extension for RAM stream
	song_metadata_t meta = {0};
	metadata_read(ptrs[selected], &meta);
	assert(strcmp(meta.title, "10 - Tagged") == 0);

	// Prepare + prefetch + acquire + read full stream bytes from RAM ring
	char play_path[96];
	snprintf(play_path, sizeof(play_path), "%s", ptrs[selected]);
	assert(webdavstream_prepare(play_path, sizeof(play_path)));
	webdavstream_prefetch(ptrs[0]);

	int dur = -1, br = -1;
	streambridge_t *bridge = webdavstream_acquire(play_path, &dur, &br);
	assert(bridge != NULL);

	size_t total_read = 0;
	uint8_t buf[8192];
	for (;;) {
		int got = streambridge_read(bridge, buf, sizeof(buf));
		assert(got >= 0);
		if (got == 0) break;
		assert(memcmp(buf, blob_tagged_mp3 + total_read, (size_t)got) == 0);
		total_read += (size_t)got;
	}
	assert(total_read == blob_tagged_mp3_len);
	webdavstream_release(bridge);
	webdavstream_reset();
	assert(!webdavstream_network_wanted());
	printf("PASS Mode 1 RAM-ring streaming (credential-free pseudo-URIs, format guard, prefetch & byte-exact read)\n");
}

static void test_sd_cache_and_growfile(void) {
	webdav_set_sd_cache(true);
	assert(webdav_sd_cache());
	webdavcache_set_max_mb(128);
	assert(webdavcache_max_mb() == 128);

	webdavcache_set_root(test_sd_root);

	webdav_entry_t items[8];
	bool more = false;
	char err[128] = {0};
	int n = webdav_browse("", 0, items, 8, &more, err, sizeof(err));
	assert(n == 5);
	const webdav_entry_t *untagged = &items[1]; // 768 KiB MP3
	const webdav_entry_t *tagged = &items[2];   // 64 KiB ID3v2 MP3
	const webdav_entry_t *fake = &items[3];     // HTML disguised as .mp3
	const webdav_entry_t *wav = &items[4];      // 32 KiB WAV

	// Placeholder check
	char ph[560];
	assert(webdavcache_placeholder(untagged, ph, sizeof(ph)));
	assert(webdavcache_is_placeholder(ph));
	assert(strstr(ph, "s3cr3t") == NULL);

	// 1. Progressive growfile: hold server after 160 KiB so webdavcache_get()
	// returns while background worker is still writing, then verify growfile_is_growing()!
	pthread_mutex_lock(&srv_lock);
	srv_slow_grow = true;
	srv_grow_in_progress = false;
	srv_release_grow = false;
	int gets_before = srv_get_count;
	pthread_mutex_unlock(&srv_lock);

	char cached_untagged[560] = {0};
	assert(webdavcache_get(untagged, cached_untagged, sizeof(cached_untagged), err, sizeof(err)));
	assert(webdavcache_owns(cached_untagged));
	assert(growfile_is_growing(cached_untagged));

	pthread_mutex_lock(&srv_lock);
	while (!srv_grow_in_progress) {
		pthread_cond_wait(&srv_cv, &srv_lock);
	}
	srv_release_grow = true;
	srv_slow_grow = false;
	pthread_cond_broadcast(&srv_cv);
	pthread_mutex_unlock(&srv_lock);

	// Wait for background worker to finish finalizing + sidecar `.tags`
	for (int i = 0; i < 100 && growfile_is_growing(cached_untagged); i++) {
		usleep(20000);
	}
	assert(!growfile_is_growing(cached_untagged));
	usleep(30000); // let maybe_write_fallback_tags complete

	struct stat st;
	assert(stat(cached_untagged, &st) == 0);
	assert((size_t)st.st_size == sizeof(blob_untagged_mp3));

	// Second call is an instant SD cache hit (zero new HTTP GET requests!)
	pthread_mutex_lock(&srv_lock);
	int gets_after_first = srv_get_count;
	pthread_mutex_unlock(&srv_lock);
	assert(gets_after_first == gets_before + 1);

	char hit_path[560] = {0};
	assert(webdavcache_get(untagged, hit_path, sizeof(hit_path), err, sizeof(err)));
	assert(strcmp(hit_path, cached_untagged) == 0);
	pthread_mutex_lock(&srv_lock);
	assert(srv_get_count == gets_after_first);
	pthread_mutex_unlock(&srv_lock);

	// Untagged MP3 gets fallback `.tags` sidecar read by metadata_read()
	song_metadata_t meta = {0};
	metadata_read(cached_untagged, &meta);
	assert(strcmp(meta.title, "2 - Untagged #1") == 0);

	// 2. Tagged MP3 preserves embedded ID3v2 tags (no `.tags` sidecar overwrite!)
	char cached_tagged[560] = {0};
	assert(webdavcache_get(tagged, cached_tagged, sizeof(cached_tagged), err, sizeof(err)));
	for (int i = 0; i < 100 && growfile_is_growing(cached_tagged); i++) usleep(10000);
	usleep(20000);
	memset(&meta, 0, sizeof(meta));
	metadata_read(cached_tagged, &meta);
	assert(strcmp(meta.title, "Embedded Kind of Blue") == 0);
	assert(strcmp(meta.artist, "Miles Davis") == 0);
	char sidecar[580];
	snprintf(sidecar, sizeof(sidecar), "%s.tags", cached_tagged);
	assert(stat(sidecar, &st) != 0); // no fallback sidecar written when real tags exist!

	// 3. Container-seekable WAV + MP3 decode cleanly via SD cache + decoder_open()
	char cached_wav[560] = {0};
	assert(webdavcache_get(wav, cached_wav, sizeof(cached_wav), err, sizeof(err)));
	assert(!growfile_is_growing(cached_wav)); // seek-on-open format waits for full file!
	assert(stat(cached_wav, &st) == 0 && (size_t)st.st_size == blob_wav_len);

	decoder_t *dec = decoder_open(cached_tagged, DECODE_FORMAT_MP3);
	assert(dec != NULL);
	assert(decoder_sample_rate(dec) == 44100 && decoder_channels(dec) == 2);
	decoder_close(dec);

	// 4. Magic-byte mismatch (HTML served for FakeAudio.mp3) rejected cleanly!
	char bad_path[560] = {0};
	assert(!webdavcache_get(fake, bad_path, sizeof(bad_path), err, sizeof(err)));
	assert(strcmp(err, "webdav_codec_mismatch") == 0);

	// 5. Reboot `.incompleto` pruning leaves completed tracks untouched
	char stale_inc[600];
	snprintf(stale_inc, sizeof(stale_inc), "%s/.local/webdav-cache/deadbeef.mp3.incompleto", test_sd_root);
	FILE *fp = fopen(stale_inc, "wb");
	assert(fp != NULL);
	fwrite("part", 1, 4, fp);
	fclose(fp);
	webdavcache_prune_incomplete();
	assert(stat(stale_inc, &st) != 0);
	assert(stat(cached_untagged, &st) == 0);

	// 6. webdavcache_clear() preserves the currently playing file and deletes others
	snprintf(test_playing_file, sizeof(test_playing_file), "%s", cached_tagged);
	int removed = 0;
	assert(webdavcache_clear(&removed));
	assert(removed == 2); // untagged + wav removed, tagged kept!
	assert(stat(cached_tagged, &st) == 0);
	assert(stat(cached_untagged, &st) != 0);
	test_playing_file[0] = '\0';

	// 7. Unmount guard (`webdavcache_set_root(NULL)`) refuses writes onto unmounted root
	webdavcache_set_root(NULL);
	assert(!webdavcache_get(untagged, cached_untagged, sizeof(cached_untagged), err, sizeof(err)));
	assert(strcmp(err, "webdav_no_card") == 0);
	webdavcache_set_root(test_sd_root);

	printf("PASS Mode 2 SD cache (growfile progressive handoff, cache hit, ID3v2 vs .tags, WAV decode, magic-byte guard & unmount safety)\n");
}

static void test_permanent_downloads(void) {
	char err[128] = {0};
	webdav_entry_t root_items[8];
	bool more = false;
	int n = webdav_browse("", 0, root_items, 8, &more, err, sizeof(err));
	assert(n == 5);

	// 1. Single track permanent save -> <sd>/Music/WebDAV Downloads/10 - Tagged.mp3
	assert(webdavdl_start_tracks(root_items, n, 2, err, sizeof(err)));
	webdavdl_progress_t st;
	for (int i = 0; i < 200; i++) {
		webdavdl_progress(&st);
		if (st.state != WEBDAVDL_RUNNING) break;
		usleep(10000);
	}
	assert(st.state == WEBDAVDL_DONE && st.completed == 1 && st.failed == 0);
	webdavdl_acknowledge();

	// Saving the exact same track again succeeds without re-downloading
	pthread_mutex_lock(&srv_lock);
	int gets_before = srv_get_count;
	pthread_mutex_unlock(&srv_lock);
	assert(webdavdl_start_tracks(root_items, n, 2, err, sizeof(err)));
	for (int i = 0; i < 200; i++) {
		webdavdl_progress(&st);
		if (st.state != WEBDAVDL_RUNNING) break;
		usleep(10000);
	}
	assert(st.state == WEBDAVDL_DONE && st.completed == 1 && st.failed == 0);
	webdavdl_acknowledge();
	pthread_mutex_lock(&srv_lock);
	assert(srv_get_count == gets_before);
	pthread_mutex_unlock(&srv_lock);

	// 2. Truncated HTTP body leaves NO partial file and NO `.part` behind
	pthread_mutex_lock(&srv_lock);
	srv_truncate_next = true;
	pthread_mutex_unlock(&srv_lock);
	assert(webdavdl_start_tracks(root_items, n, 4 /* HiRes.wav */, err, sizeof(err)));
	for (int i = 0; i < 200; i++) {
		webdavdl_progress(&st);
		if (st.state != WEBDAVDL_RUNNING) break;
		usleep(10000);
	}
	assert(st.state == WEBDAVDL_FAILED && st.completed == 0 && st.failed == 1);
	webdavdl_acknowledge();
	char wav_dl_path[600];
	snprintf(wav_dl_path, sizeof(wav_dl_path), "%s/Music/WebDAV Downloads/HiRes.wav", test_sd_root);
	struct stat fst;
	assert(stat(wav_dl_path, &fst) != 0);

	// 3. Mid-transfer cancel wakes blocked socket and cleans up `.part`
	pthread_mutex_lock(&srv_lock);
	srv_stall_next = true;
	srv_stalled_now = false;
	srv_release_grow = false;
	pthread_mutex_unlock(&srv_lock);

	assert(webdavdl_start_tracks(root_items, n, 1 /* 2 - Untagged #1.mp3 */, err, sizeof(err)));
	pthread_mutex_lock(&srv_lock);
	while (!srv_stalled_now) {
		pthread_cond_wait(&srv_cv, &srv_lock);
	}
	pthread_mutex_unlock(&srv_lock);

	webdavdl_cancel();
	pthread_mutex_lock(&srv_lock);
	srv_release_grow = true;
	pthread_cond_broadcast(&srv_cv);
	pthread_mutex_unlock(&srv_lock);

	for (int i = 0; i < 200; i++) {
		webdavdl_progress(&st);
		if (st.state != WEBDAVDL_RUNNING) break;
		usleep(10000);
	}
	assert(st.state == WEBDAVDL_CANCELLED);
	webdavdl_acknowledge();

	// 4. Recursive folder save (`Jazz & Blues` + nested `Live: 1959` sanitized to `Live_ 1959`)
	assert(webdavdl_start_folder("Jazz & Blues", "Jazz & Blues", err, sizeof(err)));
	for (int i = 0; i < 300; i++) {
		webdavdl_progress(&st);
		if (st.state != WEBDAVDL_RUNNING) break;
		usleep(10000);
	}
	assert(st.state == WEBDAVDL_DONE && st.total == 2 && st.completed == 2 && st.failed == 0);
	webdavdl_acknowledge();

	webdavdl_entry_t saved[16];
	int saved_count = webdavdl_history(saved, 16);
	assert(saved_count == 3); // 10 - Tagged.mp3 + 2 recursive tracks in Jazz & Blues

	// Delete nested tracks and verify empty sanitized subfolder is pruned cleanly
	for (int i = 0; i < saved_count; i++) {
		assert(webdavdl_delete(saved[i].path));
	}
	assert(webdavdl_history(saved, 16) == 0);
	printf("PASS permanent SD downloads (atomic .part, dedup, truncated cleanup, cancel wake, recursive folder & delete pruning)\n");
}

int main(void) {
	fill_mp3_frames(blob_untagged_mp3, sizeof(blob_untagged_mp3));
	blob_tagged_mp3_len = build_tagged_mp3(blob_tagged_mp3, sizeof(blob_tagged_mp3),
										   "Embedded Kind of Blue", "Miles Davis");
	blob_wav_len = build_pcm_wav(blob_wav, 32768);

	char tmp_dir[] = "/tmp/sonix-webdav-test-XXXXXX";
	assert(mkdtemp(tmp_dir) != NULL);
	snprintf(test_sd_root, sizeof(test_sd_root), "%s/sdcard", tmp_dir);
	assert(mkdir(test_sd_root, 0755) == 0);

	char cfg_file[600];
	snprintf(cfg_file, sizeof(cfg_file), "%s/device_config.ini", tmp_dir);
	config_init(cfg_file);
	webdav_init();

	start_mock_server();

	test_account_and_credentials(tmp_dir);
	test_propfind_and_browse();
	test_ram_streaming();
	test_sd_cache_and_growfile();
	test_permanent_downloads();

	webdavcache_clear_on_exit();
	webdavdl_cancel();
	stop_mock_server();
	rm_rf(tmp_dir);

	printf("All WebDAV backend & protocol integration tests passed.\n");
	return 0;
}
