#define _GNU_SOURCE 1

#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/growfile.h"
#include "src/system/decode/opusdec.h"
#include "src/system/decode/wavpackdec.h"
#include "src/system/library/metadata.h"
#include "src/system/net/http.h"
#include "src/system/net/tls.h"
#include "src/system/streaming/jellyfin.h"
#include "src/system/streaming/jellyfincache.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/subsonicstream.h"

#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
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

/* ---- stubs for host unit tests (matches test_soundcloud.c) -------------- */

const char *tr(const char *key) { return key ? key : ""; }

bool subsonicstream_owns(const char *path) {
	(void)path;
	return false;
}
bool subsonicstream_metadata(const char *path, song_metadata_t *out) {
	(void)path;
	(void)out;
	return false;
}
streambridge_t *subsonicstream_acquire(const char *path, int *duration, int *bitrate) {
	(void)path;
	(void)duration;
	(void)bitrate;
	return NULL;
}
void subsonicstream_release(streambridge_t *sb) { (void)sb; }

struct opusdec {
	int dummy;
};

opusdec_t *opusdec_open(const char *p) {
	FILE *f = fopen(p, "rb");
	if (!f)
		return NULL;
	unsigned char hdr[36];
	size_t n = fread(hdr, 1, sizeof(hdr), f);
	fclose(f);
	if (n < sizeof(hdr) || memcmp(hdr, "OggS", 4) != 0 || memcmp(hdr + 28, "OpusHead", 8) != 0)
		return NULL;
	return calloc(1, sizeof(opusdec_t));
}
opusdec_t *opusdec_open_stream(void *s, int (*r)(void *, unsigned char *, int)) {
	unsigned char hdr[36];
	int got = 0;
	while (got < (int)sizeof(hdr)) {
		int n = r(s, hdr + got, (int)sizeof(hdr) - got);
		if (n <= 0)
			return NULL;
		got += n;
	}
	if (memcmp(hdr, "OggS", 4) != 0 || memcmp(hdr + 28, "OpusHead", 8) != 0)
		return NULL;
	return calloc(1, sizeof(opusdec_t));
}
void opusdec_close(opusdec_t *o) { free(o); }
int opusdec_channels(const opusdec_t *o) {
	(void)o;
	return 2;
}
int opusdec_sample_rate(const opusdec_t *o) {
	(void)o;
	return 48000;
}
uint64_t opusdec_total_frames(const opusdec_t *o) {
	(void)o;
	return 0; // growing Ogg Opus stream -> decode.c falls back to .tags duration=
}
uint64_t opusdec_read_s16(opusdec_t *o, uint64_t f, short *out) {
	(void)o;
	if (f == 0)
		return 0;
	uint64_t n = f > 256 ? 256 : f;
	memset(out, 0, (size_t)n * 2 * sizeof(short));
	return n;
}
bool opusdec_seek(opusdec_t *o, uint64_t f) {
	(void)o;
	(void)f;
	return false;
}

wavpackdec_t *wavpackdec_open(const char *p) {
	(void)p;
	return NULL;
}
void wavpackdec_close(wavpackdec_t *w) { (void)w; }
int wavpackdec_channels(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
int wavpackdec_sample_rate(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
uint64_t wavpackdec_total_frames(const wavpackdec_t *w) {
	(void)w;
	return 0;
}
uint64_t wavpackdec_read_s16(wavpackdec_t *w, uint64_t f, short *out) {
	(void)w;
	(void)f;
	(void)out;
	return 0;
}
uint64_t wavpackdec_read_s32(wavpackdec_t *w, uint64_t f, int32_t *out) {
	(void)w;
	(void)f;
	(void)out;
	return 0;
}
bool wavpackdec_seek(wavpackdec_t *w, uint64_t f) {
	(void)w;
	(void)f;
	return false;
}

/* ---- temp directory cleanup helper ------------------------------------- */

static int rm_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	(void)sb;
	(void)typeflag;
	(void)ftwbuf;
	return remove(fpath);
}

static void rm_rf(const char *path) {
	if (path && path[0])
		nftw(path, rm_cb, 32, FTW_DEPTH | FTW_PHYS);
}

/* ---- mock Jellyfin HTTP server ----------------------------------------- */

typedef struct {
	int listen_fd;
	int port;
	pthread_t tid;
	pthread_mutex_t mu;
	bool stop;

	bool qc_enabled;
	int qc_poll_step; // 0 = pending, 1 = authorized, -1 = expired (404)
	bool slow_stream;
	int slow_chunk_ms;
	int stream_requests;
	int image_requests;
	int play_start_count;
	int play_progress_count;
	int play_stop_count;
	int fav_add_count;
	int fav_del_count;
	int logout_count;

	char last_auth_header[512];
	char last_path[512];
	char last_playlists_path[512];
	char last_albums_path[512];
	char last_play_body[1024];
	char last_progress_body[1024];
	char last_stop_body[1024];
} mock_server_t;

static void send_all(int fd, const void *buf, size_t len) {
	const uint8_t *p = (const uint8_t *)buf;
	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n <= 0)
			return;
		p += (size_t)n;
		len -= (size_t)n;
	}
}

static void send_json(int fd, int status, const char *status_text, const char *body) {
	char hdr[512];
	size_t blen = body ? strlen(body) : 0;
	int n = snprintf(hdr, sizeof(hdr),
					 "HTTP/1.1 %d %s\r\n"
					 "Content-Type: application/json\r\n"
					 "Content-Length: %zu\r\n"
					 "Connection: close\r\n\r\n",
					 status, status_text, blen);
	send_all(fd, hdr, (size_t)n);
	if (blen > 0)
		send_all(fd, body, blen);
}

static void send_empty(int fd, int status, const char *status_text) {
	char hdr[256];
	int n = snprintf(hdr, sizeof(hdr),
					 "HTTP/1.1 %d %s\r\n"
					 "Content-Length: 0\r\n"
					 "Connection: close\r\n\r\n",
					 status, status_text);
	send_all(fd, hdr, (size_t)n);
}

static void build_fake_opus_payload(uint8_t *buf, size_t len) {
	memset(buf, 0x5A, len);
	static const uint8_t hdr[36] = {
		'O', 'g', 'g', 'S', 0, 2, 0, 0, 0, 0, 0, 0, 0, 0,
		1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 19,
		'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
	};
	if (len >= sizeof(hdr))
		memcpy(buf, hdr, sizeof(hdr));
}

static void build_fake_jpeg(uint8_t *buf, size_t len) {
	memset(buf, 0x11, len);
	buf[0] = 0xFF;
	buf[1] = 0xD8;
	buf[2] = 0xFF;
	buf[3] = 0xE0;
	buf[len - 2] = 0xFF;
	buf[len - 1] = 0xD9;
}

static void handle_client(mock_server_t *srv, int cfd) {
	char req[8192];
	size_t total = 0;
	size_t header_end = 0;
	size_t content_len = 0;

	struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	while (total + 1 < sizeof(req)) {
		ssize_t n = recv(cfd, req + total, sizeof(req) - 1 - total, 0);
		if (n <= 0)
			break;
		total += (size_t)n;
		req[total] = '\0';
		if (header_end == 0) {
			char *sep = strstr(req, "\r\n\r\n");
			if (sep) {
				header_end = (size_t)(sep - req) + 4;
				char *cl = strcasestr(req, "\r\nContent-Length:");
				if (cl && cl < sep) {
					content_len = (size_t)strtoul(cl + 17, NULL, 10);
				}
			}
		}
		if (header_end > 0 && total >= header_end + content_len)
			break;
	}
	if (header_end == 0) {
		close(cfd);
		return;
	}

	char method[16] = {0};
	char path[512] = {0};
	sscanf(req, "%15s %511s", method, path);

	char auth[512] = {0};
	char *ah = strcasestr(req, "\r\nAuthorization:");
	if (ah && ah < req + header_end) {
		ah += 16;
		while (*ah == ' ' || *ah == '\t')
			ah++;
		size_t i = 0;
		while (ah[i] && ah[i] != '\r' && ah[i] != '\n' && i + 1 < sizeof(auth)) {
			auth[i] = ah[i];
			i++;
		}
		auth[i] = '\0';
	}

	const char *body = req + header_end;

	pthread_mutex_lock(&srv->mu);
	snprintf(srv->last_auth_header, sizeof(srv->last_auth_header), "%s", auth);
	snprintf(srv->last_path, sizeof(srv->last_path), "%s", path);
	bool qc_enabled = srv->qc_enabled;
	int qc_poll_step = srv->qc_poll_step;
	bool slow_stream = srv->slow_stream;
	int slow_chunk_ms = srv->slow_chunk_ms;
	pthread_mutex_unlock(&srv->mu);

	/* Ensure token is NEVER leaked in URL query string */
	assert(strstr(path, "api_key=") == NULL);
	assert(strstr(path, "tok-secret-") == NULL);

	if (strcmp(method, "POST") == 0 && strcmp(path, "/Users/AuthenticateByName") == 0) {
		if (strstr(body, "\"Username\":\"alice\"") && strstr(body, "\"Pw\":\"hunter2\"")) {
			send_json(cfd, 200, "OK",
					  "{\"AccessToken\":\"tok-secret-123\",\"ServerId\":\"srv-01\","
					  "\"User\":{\"Id\":\"user-42\",\"Name\":\"alice\"}}");
		} else if (strstr(body, "\"Username\":\"boom\"")) {
			send_json(cfd, 500, "Internal Server Error", "{\"error\":\"boom\"}");
		} else {
			send_json(cfd, 401, "Unauthorized", "{\"error\":\"bad credentials\"}");
		}
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/Sessions/Logout") == 0) {
		pthread_mutex_lock(&srv->mu);
		srv->logout_count++;
		pthread_mutex_unlock(&srv->mu);
		send_empty(cfd, 204, "No Content");
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/QuickConnect/Enabled") == 0) {
		send_json(cfd, 200, "OK", qc_enabled ? "true" : "false");
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/QuickConnect/Initiate") == 0) {
		if (!qc_enabled) {
			send_empty(cfd, 401, "Unauthorized");
		} else {
			send_json(cfd, 200, "OK",
					  "{\"Secret\":\"qc-sec-999\",\"Code\":\"482913\",\"Authenticated\":false}");
		}
	} else if (strcmp(method, "GET") == 0 && strncmp(path, "/QuickConnect/Connect?secret=qc-sec-999", 39) == 0) {
		if (qc_poll_step < 0) {
			send_empty(cfd, 404, "Not Found");
		} else if (qc_poll_step == 0) {
			send_json(cfd, 200, "OK", "{\"Secret\":\"qc-sec-999\",\"Code\":\"482913\",\"Authenticated\":false}");
		} else {
			send_json(cfd, 200, "OK", "{\"Secret\":\"qc-sec-999\",\"Code\":\"482913\",\"Authenticated\":true}");
		}
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/Users/AuthenticateWithQuickConnect") == 0) {
		if (strstr(body, "\"Secret\":\"qc-sec-999\"")) {
			send_json(cfd, 200, "OK",
					  "{\"AccessToken\":\"tok-secret-qc\",\"User\":{\"Id\":\"user-qc\",\"Name\":\"bob\"}}");
		} else {
			send_empty(cfd, 401, "Unauthorized");
		}
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/Users/Me") == 0) {
		if (strstr(auth, "Token=\"tok-secret-")) {
			send_json(cfd, 200, "OK", "{\"Id\":\"user-42\",\"Name\":\"alice\"}");
		} else {
			send_empty(cfd, 401, "Unauthorized");
		}
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/System/Info/Public") == 0) {
		send_json(cfd, 200, "OK", "{\"ServerName\":\"HomeFin\",\"Version\":\"10.10.3\",\"Id\":\"srv-01\"}");
	} else if (strcmp(method, "GET") == 0 && strcmp(path, "/Users/user-42/Views") == 0) {
		send_json(cfd, 200, "OK",
				  "{\"Items\":["
				  "{\"Id\":\"lib-movies\",\"Name\":\"Movies\",\"CollectionType\":\"movies\"},"
				  "{\"Id\":\"lib-music-1\",\"Name\":\"Lossless Music\",\"CollectionType\":\"music\"},"
				  "{\"Id\":\"lib-music-2\",\"Name\":\"Vinyl Rips\",\"CollectionType\":\"music\"}"
				  "],\"TotalRecordCount\":3}");
	} else if (strcmp(method, "GET") == 0 && strncmp(path, "/Artists/AlbumArtists", 21) == 0) {
		send_json(cfd, 200, "OK",
				  "{\"Items\":["
				  "{\"Id\":\"art-1\",\"Name\":\"Boards of Canada\",\"Type\":\"MusicArtist\",\"ChildCount\":6},"
				  "{\"Id\":\"art-2\",\"Name\":\"Aphex Twin\",\"Type\":\"MusicArtist\",\"ChildCount\":9}"
				  "],\"TotalRecordCount\":2}");
	} else if (strcmp(method, "GET") == 0 && strncmp(path, "/MusicGenres", 12) == 0) {
		send_json(cfd, 200, "OK",
				  "{\"Items\":["
				  "{\"Id\":\"gen-1\",\"Name\":\"Ambient\",\"Type\":\"MusicGenre\",\"ChildCount\":40}"
				  "],\"TotalRecordCount\":1}");
	} else if (strcmp(method, "GET") == 0 && strncmp(path, "/Playlists/pl-1/Items", 21) == 0) {
		send_json(cfd, 200, "OK",
				  "{\"Items\":["
				  "{\"Id\":\"trk-long\",\"Name\":\"Long Mix\",\"Type\":\"Audio\",\"Container\":\"flac\","
				  "\"Album\":\"Deep Cuts\",\"AlbumId\":\"alb-1\",\"AlbumArtist\":\"Boards of Canada\","
				  "\"RunTimeTicks\":37230000000,\"IndexNumber\":1,\"ParentIndexNumber\":1,"
				  "\"UserData\":{\"IsFavorite\":true}}"
				  "],\"TotalRecordCount\":1}");
	} else if (strcmp(method, "GET") == 0 && strncmp(path, "/Users/user-42/Items", 20) == 0) {
		if (strstr(path, "IncludeItemTypes=Playlist")) {
			pthread_mutex_lock(&srv->mu);
			snprintf(srv->last_playlists_path, sizeof(srv->last_playlists_path), "%s", path);
			pthread_mutex_unlock(&srv->mu);
			send_json(cfd, 200, "OK",
					  "{\"Items\":["
					  "{\"Id\":\"pl-1\",\"Name\":\"Late Night\",\"Type\":\"Playlist\",\"ChildCount\":12}"
					  "],\"TotalRecordCount\":1}");
		} else if (strstr(path, "IncludeItemTypes=MusicAlbum")) {
			pthread_mutex_lock(&srv->mu);
			snprintf(srv->last_albums_path, sizeof(srv->last_albums_path), "%s", path);
			pthread_mutex_unlock(&srv->mu);
			send_json(cfd, 200, "OK",
					  "{\"Items\":["
					  "{\"Id\":\"alb-1\",\"Name\":\"Music Has the Right to Children\",\"Type\":\"MusicAlbum\","
					  "\"AlbumArtist\":\"Boards of Canada\",\"ProductionYear\":1998,\"ChildCount\":18}"
					  "],\"TotalRecordCount\":3}");
		} else if (strstr(path, "SearchTerm=")) {
			send_json(cfd, 200, "OK",
					  "{\"Items\":["
					  "{\"Id\":\"trk-search\",\"Name\":\"Roygbiv\",\"Type\":\"Audio\",\"Container\":\"opus\","
					  "\"Album\":\"MHTRTC\",\"AlbumArtist\":\"Boards of Canada\","
					  "\"RunTimeTicks\":1510000000,\"IndexNumber\":6,\"UserData\":{\"IsFavorite\":false}}"
					  "],\"TotalRecordCount\":1}");
		} else if (strstr(path, "Filters=IsFavorite")) {
			send_json(cfd, 200, "OK",
					  "{\"Items\":["
					  "{\"Id\":\"trk-long\",\"Name\":\"Long Mix\",\"Type\":\"Audio\",\"Container\":\"flac\","
					  "\"Album\":\"Deep Cuts\",\"AlbumArtist\":\"Boards of Canada\","
					  "\"RunTimeTicks\":37230000000,\"UserData\":{\"IsFavorite\":true}}"
					  "],\"TotalRecordCount\":1}");
		} else if (strstr(path, "ParentId=alb-1") || strstr(path, "GenreIds=gen-1")) {
			send_json(cfd, 200, "OK",
					  "{\"Items\":["
					  "{\"Id\":\"trk-opus\",\"Name\":\"Roygbiv\",\"Type\":\"Audio\",\"Container\":\"opus\","
					  "\"Album\":\"MHTRTC\",\"AlbumId\":\"alb-1\",\"AlbumArtist\":\"Boards of Canada\","
					  "\"RunTimeTicks\":1510000000,\"IndexNumber\":6,\"ParentIndexNumber\":1,"
					  "\"UserData\":{\"IsFavorite\":false}},"
					  "{\"Id\":\"trk-m4a\",\"Name\":\"Aquarius\",\"Type\":\"Audio\",\"Container\":\"m4a\","
					  "\"Album\":\"MHTRTC\",\"AlbumId\":\"alb-1\",\"AlbumArtist\":\"Boards of Canada\","
					  "\"RunTimeTicks\":3580000000,\"IndexNumber\":7,\"ParentIndexNumber\":1,"
					  "\"UserData\":{\"IsFavorite\":true}}"
					  "],\"TotalRecordCount\":2}");
		} else {
			send_json(cfd, 200, "OK",
					  "{\"Items\":["
					  "{\"Id\":\"fld-1\",\"Name\":\"Electronic\",\"Type\":\"Folder\",\"ChildCount\":4}"
					  "],\"TotalRecordCount\":1}");
		}
	} else if (strcmp(method, "POST") == 0 && strncmp(path, "/Users/user-42/FavoriteItems/", 29) == 0) {
		pthread_mutex_lock(&srv->mu);
		srv->fav_add_count++;
		pthread_mutex_unlock(&srv->mu);
		send_json(cfd, 200, "OK", "{\"IsFavorite\":true}");
	} else if (strcmp(method, "DELETE") == 0 && strncmp(path, "/Users/user-42/FavoriteItems/", 29) == 0) {
		pthread_mutex_lock(&srv->mu);
		srv->fav_del_count++;
		pthread_mutex_unlock(&srv->mu);
		send_json(cfd, 200, "OK", "{\"IsFavorite\":false}");
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/Sessions/Playing") == 0) {
		pthread_mutex_lock(&srv->mu);
		srv->play_start_count++;
		snprintf(srv->last_play_body, sizeof(srv->last_play_body), "%s", body);
		pthread_mutex_unlock(&srv->mu);
		send_empty(cfd, 204, "No Content");
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/Sessions/Playing/Progress") == 0) {
		pthread_mutex_lock(&srv->mu);
		srv->play_progress_count++;
		snprintf(srv->last_progress_body, sizeof(srv->last_progress_body), "%s", body);
		pthread_mutex_unlock(&srv->mu);
		send_empty(cfd, 204, "No Content");
	} else if (strcmp(method, "POST") == 0 && strcmp(path, "/Sessions/Playing/Stopped") == 0) {
		pthread_mutex_lock(&srv->mu);
		srv->play_stop_count++;
		snprintf(srv->last_stop_body, sizeof(srv->last_stop_body), "%s", body);
		pthread_mutex_unlock(&srv->mu);
		send_empty(cfd, 204, "No Content");
	} else if (strcmp(method, "GET") == 0 && strstr(path, "/Images/Primary")) {
		pthread_mutex_lock(&srv->mu);
		srv->image_requests++;
		pthread_mutex_unlock(&srv->mu);
		uint8_t jpg[512];
		build_fake_jpeg(jpg, sizeof(jpg));
		char hdr[256];
		int n = snprintf(hdr, sizeof(hdr),
						 "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n"
						 "Connection: close\r\n\r\n",
						 sizeof(jpg));
		send_all(cfd, hdr, (size_t)n);
		send_all(cfd, jpg, sizeof(jpg));
	} else if (strcmp(method, "GET") == 0 && (strstr(path, "/Download") || strncmp(path, "/Audio/", 7) == 0)) {
		assert(strstr(auth, "MediaBrowser ") != NULL);
		assert(strstr(auth, "Token=\"tok-secret-") != NULL);
		pthread_mutex_lock(&srv->mu);
		srv->stream_requests++;
		pthread_mutex_unlock(&srv->mu);

		size_t total_bytes = 96 * 1024;
		uint8_t *payload = malloc(total_bytes);
		assert(payload != NULL);
		build_fake_opus_payload(payload, total_bytes);

		char hdr[256];
		int n = snprintf(hdr, sizeof(hdr),
						 "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
						 "Content-Length: %zu\r\nConnection: close\r\n\r\n",
						 total_bytes);
		send_all(cfd, hdr, (size_t)n);

		if (slow_stream) {
			size_t off = 0;
			size_t chunk = 8 * 1024;
			while (off < total_bytes) {
				size_t w = total_bytes - off > chunk ? chunk : total_bytes - off;
				ssize_t sent = send(cfd, payload + off, w, MSG_NOSIGNAL);
				if (sent <= 0)
					break;
				off += (size_t)sent;
				if (slow_chunk_ms > 0)
					usleep((useconds_t)slow_chunk_ms * 1000u);
			}
		} else {
			send_all(cfd, payload, total_bytes);
		}
		free(payload);
	} else {
		send_empty(cfd, 404, "Not Found");
	}

	close(cfd);
}

static void *server_thread(void *arg) {
	mock_server_t *srv = (mock_server_t *)arg;
	for (;;) {
		struct sockaddr_in ca;
		socklen_t clen = sizeof(ca);
		int cfd = accept(srv->listen_fd, (struct sockaddr *)&ca, &clen);
		if (cfd < 0) {
			pthread_mutex_lock(&srv->mu);
			bool stop = srv->stop;
			pthread_mutex_unlock(&srv->mu);
			if (stop)
				break;
			continue;
		}
		handle_client(srv, cfd);
	}
	return NULL;
}

static void mock_server_start(mock_server_t *srv) {
	memset(srv, 0, sizeof(*srv));
	pthread_mutex_init(&srv->mu, NULL);
	srv->qc_enabled = true;

	srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(srv->listen_fd >= 0);
	int one = 1;
	setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	assert(bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(srv->listen_fd, 16) == 0);

	socklen_t alen = sizeof(addr);
	assert(getsockname(srv->listen_fd, (struct sockaddr *)&addr, &alen) == 0);
	srv->port = ntohs(addr.sin_port);

	assert(pthread_create(&srv->tid, NULL, server_thread, srv) == 0);
}

static void mock_server_stop(mock_server_t *srv) {
	pthread_mutex_lock(&srv->mu);
	srv->stop = true;
	pthread_mutex_unlock(&srv->mu);
	shutdown(srv->listen_fd, SHUT_RDWR);
	close(srv->listen_fd);
	pthread_join(srv->tid, NULL);
	pthread_mutex_destroy(&srv->mu);
}

/* ---- tests -------------------------------------------------------------- */

static void test_url_normalization(void) {
	char out[256], err[128];
	assert(jellyfin_normalize_server("192.168.1.50:8096", out, sizeof(out), err, sizeof(err)));
	assert(strcmp(out, "http://192.168.1.50:8096") == 0);

	assert(jellyfin_normalize_server("  https://music.example.com/jellyfin///  ", out, sizeof(out), err, sizeof(err)));
	assert(strcmp(out, "https://music.example.com/jellyfin") == 0);

	assert(!jellyfin_normalize_server("", out, sizeof(out), err, sizeof(err)));
	assert(!jellyfin_normalize_server("http://host/path?a=1", out, sizeof(out), err, sizeof(err)));
	assert(!jellyfin_normalize_server("http://host/path#frag", out, sizeof(out), err, sizeof(err)));
	assert(!jellyfin_normalize_server("ftp://host:8096", out, sizeof(out), err, sizeof(err)));
	assert(!jellyfin_normalize_server("http://bad host:8096", out, sizeof(out), err, sizeof(err)));
	printf("PASS url_normalization\n");
}

static void test_auth_and_credentials(mock_server_t *srv, const char *tmp_dir) {
	char server_url[128], err[128];
	snprintf(server_url, sizeof(server_url), "http://127.0.0.1:%d/", srv->port);

	char cred_path[512];
	snprintf(cred_path, sizeof(cred_path), "%s/jellyfin-credentials.json", tmp_dir);

	/* Bad password -> auth error */
	jellyfin_credentials_t creds;
	memset(&creds, 0, sizeof(creds));
	snprintf(creds.server, sizeof(creds.server), "%s", server_url);
	snprintf(creds.username, sizeof(creds.username), "alice");
	snprintf(creds.password, sizeof(creds.password), "wrong");
	assert(!jellyfin_authenticate(&creds, true, err, sizeof(err)));
	assert(strcmp(err, "jellyfin_auth_failed") == 0);
	assert(!jellyfin_logged_in());

	/* Server 500 -> server error */
	snprintf(creds.username, sizeof(creds.username), "boom");
	assert(!jellyfin_authenticate(&creds, true, err, sizeof(err)));
	assert(strcmp(err, "jellyfin_server_error") == 0);

	/* Ephemeral login (remember=false) must NOT write credentials file */
	snprintf(creds.username, sizeof(creds.username), "alice");
	snprintf(creds.password, sizeof(creds.password), "hunter2");
	assert(jellyfin_authenticate(&creds, false, err, sizeof(err)));
	assert(jellyfin_logged_in());
	assert(access(cred_path, F_OK) != 0);

	/* Remembered login writes 0600 file and NEVER writes password */
	assert(jellyfin_authenticate(&creds, true, err, sizeof(err)));
	assert(jellyfin_logged_in());

	struct stat st;
	assert(stat(cred_path, &st) == 0);
	assert((st.st_mode & 0777) == 0600);

	FILE *f = fopen(cred_path, "rb");
	assert(f != NULL);
	char buf[2048];
	size_t nr = fread(buf, 1, sizeof(buf) - 1, f);
	buf[nr] = '\0';
	fclose(f);
	assert(strstr(buf, "tok-secret-123") != NULL);
	assert(strstr(buf, "hunter2") == NULL);

	/* Host-isolated auth headers: sent for configured server, EMPTY for foreign host! */
	char hdr[512];
	char own_url[256];
	snprintf(own_url, sizeof(own_url), "http://127.0.0.1:%d/Users/Me", srv->port);
	jellyfin_headers_for_url(own_url, hdr, sizeof(hdr));
	assert(strstr(hdr, "Authorization: MediaBrowser ") != NULL);
	assert(strstr(hdr, "Client=\"SonixPlayer\"") != NULL);
	assert(strstr(hdr, "Token=\"tok-secret-123\"") != NULL);

	jellyfin_headers_for_url("https://evil.example.com/steal", hdr, sizeof(hdr));
	assert(hdr[0] == '\0');

	/* Reload from disk preserves session */
	jellyfin_init();
	assert(jellyfin_logged_in());
	jellyfin_account_info_t info;
	jellyfin_account_info(&info);
	assert(strcmp(info.username, "alice") == 0);
	assert(strcmp(info.user_id, "user-42") == 0);
	assert(info.logged_in && info.remembered);

	/* Ping refreshes server_name */
	assert(jellyfin_ping(err, sizeof(err)));
	jellyfin_account_info(&info);
	assert(strcmp(info.server_name, "HomeFin") == 0);

	printf("PASS auth_and_credentials\n");
}

static void test_quick_connect(mock_server_t *srv) {
	char server_url[128], err[128];
	snprintf(server_url, sizeof(server_url), "http://127.0.0.1:%d", srv->port);

	/* Disabled on server */
	pthread_mutex_lock(&srv->mu);
	srv->qc_enabled = false;
	pthread_mutex_unlock(&srv->mu);
	jellyfin_qc_t qc;
	assert(jellyfin_qc_initiate(server_url, &qc, err, sizeof(err)) == JELLYFIN_QC_DISABLED);

	/* Enabled -> initiate -> pending -> expired -> authorized */
	pthread_mutex_lock(&srv->mu);
	srv->qc_enabled = true;
	srv->qc_poll_step = 0;
	pthread_mutex_unlock(&srv->mu);

	assert(jellyfin_qc_initiate(server_url, &qc, err, sizeof(err)) == JELLYFIN_QC_PENDING);
	assert(strcmp(qc.code, "482913") == 0);
	assert(strcmp(qc.secret, "qc-sec-999") == 0);
	assert(strstr(qc.web_url, "/web/#/quickconnect") != NULL);

	assert(jellyfin_qc_poll(&qc, false, err, sizeof(err)) == JELLYFIN_QC_PENDING);

	pthread_mutex_lock(&srv->mu);
	srv->qc_poll_step = -1;
	pthread_mutex_unlock(&srv->mu);
	assert(jellyfin_qc_poll(&qc, false, err, sizeof(err)) == JELLYFIN_QC_EXPIRED);

	pthread_mutex_lock(&srv->mu);
	srv->qc_poll_step = 1;
	pthread_mutex_unlock(&srv->mu);
	assert(jellyfin_qc_poll(&qc, false, err, sizeof(err)) == JELLYFIN_QC_OK);
	jellyfin_account_info_t info;
	jellyfin_account_info(&info);
	assert(strcmp(info.username, "bob") == 0);
	assert(strcmp(info.user_id, "user-qc") == 0);

	/* Re-authenticate alice (remembered) for browse tests */
	jellyfin_credentials_t creds;
	memset(&creds, 0, sizeof(creds));
	snprintf(creds.server, sizeof(creds.server), "%s", server_url);
	snprintf(creds.username, sizeof(creds.username), "alice");
	snprintf(creds.password, sizeof(creds.password), "hunter2");
	assert(jellyfin_authenticate(&creds, true, err, sizeof(err)));
	jellyfin_account_info(&info);
	assert(strcmp(info.user_id, "user-42") == 0);
	printf("PASS quick_connect\n");
}

static void test_browse_search_favorites_and_reporting(mock_server_t *srv) {
	jellyfin_entry_t entries[16];
	bool more = false;
	char err[128];

	/* Music libraries filter ignores Movies */
	int n = jellyfin_browse(JELLYFIN_BROWSE_LIBRARIES, NULL, 0, entries, 16, &more, err, sizeof(err));
	assert(n == 2);
	assert(strcmp(entries[0].id, "lib-music-1") == 0);
	assert(strcmp(entries[1].id, "lib-music-2") == 0);

	/* Select a music library */
	assert(jellyfin_set_library(entries[0].id, entries[0].title));

	/* Playlists MUST omit ParentId even when a music library is selected */
	n = jellyfin_browse(JELLYFIN_BROWSE_PLAYLISTS, NULL, 0, entries, 16, &more, err, sizeof(err));
	assert(n == 1 && entries[0].kind == JELLYFIN_ITEM_PLAYLIST);
	assert(strcmp(entries[0].id, "pl-1") == 0);
	pthread_mutex_lock(&srv->mu);
	assert(strstr(srv->last_playlists_path, "ParentId=") == NULL);
	pthread_mutex_unlock(&srv->mu);

	/* Albums A-Z vs Recently added + pagination (`more == true`) */
	n = jellyfin_browse(JELLYFIN_BROWSE_ALBUMS, NULL, 0, entries, 1, &more, err, sizeof(err));
	assert(n == 1 && more);
	pthread_mutex_lock(&srv->mu);
	assert(strstr(srv->last_albums_path, "SortBy=SortName") != NULL);
	assert(strstr(srv->last_albums_path, "ParentId=lib-music-1") != NULL);
	pthread_mutex_unlock(&srv->mu);

	n = jellyfin_browse(JELLYFIN_BROWSE_RECENT, NULL, 0, entries, 16, &more, err, sizeof(err));
	assert(n == 1);
	pthread_mutex_lock(&srv->mu);
	assert(strstr(srv->last_albums_path, "SortBy=DateCreated") != NULL);
	assert(strstr(srv->last_albums_path, "SortOrder=Descending") != NULL);
	pthread_mutex_unlock(&srv->mu);

	/* Artists, Genres, Folders, Search, Favorites */
	n = jellyfin_browse(JELLYFIN_BROWSE_ARTISTS, NULL, 0, entries, 16, &more, err, sizeof(err));
	assert(n == 2 && entries[0].kind == JELLYFIN_ITEM_ARTIST);

	n = jellyfin_browse(JELLYFIN_BROWSE_GENRES, NULL, 0, entries, 16, &more, err, sizeof(err));
	assert(n == 1 && entries[0].kind == JELLYFIN_ITEM_GENRE);

	n = jellyfin_browse(JELLYFIN_BROWSE_FOLDERS, NULL, 0, entries, 16, &more, err, sizeof(err));
	assert(n == 1 && entries[0].kind == JELLYFIN_ITEM_FOLDER);

	n = jellyfin_browse(JELLYFIN_BROWSE_SEARCH, "Roygbiv & friends", 0, entries, 16, &more, err, sizeof(err));
	assert(n == 1 && entries[0].kind == JELLYFIN_ITEM_TRACK && entries[0].duration == 151);

	/* Playlist children + 64-bit RunTimeTicks (> 2^31) */
	n = jellyfin_browse(JELLYFIN_BROWSE_PLAYLIST_ITEMS, "pl-1", 0, entries, 16, &more, err, sizeof(err));
	assert(n == 1);
	assert(entries[0].duration == 3723);
	assert(entries[0].is_favorite);
	assert(jellyfin_is_favorite("trk-long"));

	/* Favorite toggle */
	assert(jellyfin_set_favorite("trk-opus", true, err, sizeof(err)));
	assert(jellyfin_is_favorite("trk-opus"));
	assert(jellyfin_set_favorite("trk-opus", false, err, sizeof(err)));
	assert(!jellyfin_is_favorite("trk-opus"));
	pthread_mutex_lock(&srv->mu);
	assert(srv->fav_add_count == 1 && srv->fav_del_count == 1);
	pthread_mutex_unlock(&srv->mu);

	/* Playback reporting sync + async */
	jellyfin_set_report_playback(true);
	assert(jellyfin_report_playback_start("trk-long", "sess-1", 0, false));
	assert(jellyfin_report_playback_progress("trk-long", "sess-1", 37000000000LL, true, false));
	assert(jellyfin_report_playback_stopped("trk-long", "sess-1", 37230000000LL));

	jellyfin_report_start_async("trk-opus", "sess-2", 0, true);
	jellyfin_report_progress_async("trk-opus", "sess-2", 420000000LL, false, true);
	jellyfin_report_stopped_async("trk-opus", "sess-2", 1510000000LL);
	for (int i = 0; i < 100; i++) {
		pthread_mutex_lock(&srv->mu);
		bool done = (srv->play_stop_count >= 2);
		pthread_mutex_unlock(&srv->mu);
		if (done)
			break;
		usleep(20000);
	}

	pthread_mutex_lock(&srv->mu);
	assert(srv->play_start_count == 2);
	assert(srv->play_progress_count == 2);
	assert(srv->play_stop_count == 2);
	assert(strstr(srv->last_play_body, "\"PlayMethod\":\"Transcode\"") != NULL);
	assert(strstr(srv->last_progress_body, "420000000") != NULL);
	assert(strstr(srv->last_stop_body, "1510000000") != NULL);
	pthread_mutex_unlock(&srv->mu);

	/* Reporting disabled -> zero new HTTP calls */
	jellyfin_set_report_playback(false);
	assert(jellyfin_report_playback_start("trk-long", "sess-3", 0, false));
	pthread_mutex_lock(&srv->mu);
	assert(srv->play_start_count == 2);
	pthread_mutex_unlock(&srv->mu);
	jellyfin_set_report_playback(true);

	printf("PASS browse_search_favorites_and_reporting\n");
}

static void test_stream_urls_and_cache(mock_server_t *srv, const char *tmp_dir) {
	char sd_dir[512];
	snprintf(sd_dir, sizeof(sd_dir), "%s/sdcard", tmp_dir);
	assert(mkdir(sd_dir, 0755) == 0);
	jellyfincache_set_root(sd_dir);

	/* Stream URL building across quality modes + auto-transcode unsupported WMA */
	char url[512], ext[16], err[128];
	bool transcoded = false;
	assert(jellyfin_stream_url("trk-opus", JELLYFIN_QUALITY_ORIGINAL, "opus", url, sizeof(url), ext, sizeof(ext),
							   &transcoded, err, sizeof(err)));
	assert(strstr(url, "/Audio/trk-opus/stream?static=true") != NULL || strstr(url, "/Items/trk-opus/Download") != NULL);
	assert(strcmp(ext, "opus") == 0 && !transcoded);

	assert(jellyfin_stream_url("trk-opus", JELLYFIN_QUALITY_MP3_320, "flac", url, sizeof(url), ext, sizeof(ext),
							   &transcoded, err, sizeof(err)));
	assert(strstr(url, "/Audio/trk-opus/stream") != NULL || strstr(url, "/Audio/trk-opus/universal") != NULL);
	assert(strcmp(ext, "mp3") == 0 && transcoded);

	assert(jellyfin_stream_url("trk-opus", JELLYFIN_QUALITY_OPUS_128, "flac", url, sizeof(url), ext, sizeof(ext),
							   &transcoded, err, sizeof(err)));
	assert(strcmp(ext, "opus") == 0 && transcoded);

	/* Unsupported WMA in ORIGINAL mode auto-transcodes to MP3 */
	assert(!jellyfin_container_playable("wma"));
	assert(jellyfin_container_playable("flac"));
	assert(jellyfin_stream_url("trk-wma", JELLYFIN_QUALITY_ORIGINAL, "wma", url, sizeof(url), ext, sizeof(ext),
							   &transcoded, err, sizeof(err)));
	assert(strcmp(ext, "mp3") == 0 && transcoded);

	/* Path and ID extraction round-trip */
	jellyfin_set_quality(JELLYFIN_QUALITY_OPUS_128);
	char cpath[512], extracted[JELLYFIN_ID_MAX];
	assert(jellyfincache_placeholder_path("trk-opus", "flac", cpath, sizeof(cpath)));
	assert(jellyfincache_owns(cpath));
	assert(jellyfincache_item_id_of(cpath, extracted, sizeof(extracted)));
	assert(strcmp(extracted, "trk-opus") == 0);

	/* Slow stream -> progressive growfile ready BEFORE download finishes + decoder_open */
	jellyfin_entry_t item;
	memset(&item, 0, sizeof(item));
	item.kind = JELLYFIN_ITEM_TRACK;
	snprintf(item.id, sizeof(item.id), "trk-opus");
	snprintf(item.title, sizeof(item.title), "Roygbiv");
	snprintf(item.artist, sizeof(item.artist), "Boards of Canada");
	snprintf(item.album, sizeof(item.album), "MHTRTC");
	snprintf(item.cover_id, sizeof(item.cover_id), "alb-1");
	snprintf(item.container, sizeof(item.container), "opus");
	item.duration = 151;

	pthread_mutex_lock(&srv->mu);
	srv->slow_stream = true;
	srv->slow_chunk_ms = 20;
	srv->stream_requests = 0;
	srv->image_requests = 0;
	pthread_mutex_unlock(&srv->mu);

	jellyfin_set_quality(JELLYFIN_QUALITY_ORIGINAL);
	char ready_path[512] = {0};
	assert(jellyfincache_get(&item, false, ready_path, sizeof(ready_path), err, sizeof(err)));
	assert(jellyfincache_owns(ready_path));

	/* Verify decoder_open() handles the growing Opus file + reads .tags duration=151 */
	decoder_t *dec = decoder_open(ready_path, DECODE_FORMAT_OPUS);
	assert(dec != NULL);
	assert(decoder_total_pcm_frames(dec) == 151ULL * 48000ULL);
	int32_t pcm32[256];
	assert(decoder_read_pcm_frames_s32(dec, 128, pcm32) > 0);
	decoder_close(dec);

	/* Wait for background download completion */
	for (int i = 0; i < 300 && jellyfincache_downloading(); i++)
		usleep(20000);
	assert(!jellyfincache_downloading());

	/* Verify sidecars exist (.tags + .jpg) and .incompleto is cleaned */
	char tags_path[544], sentinel_path[544];
	snprintf(tags_path, sizeof(tags_path), "%s.tags", ready_path);
	snprintf(sentinel_path, sizeof(sentinel_path), "%s.incompleto", ready_path);
	assert(access(tags_path, F_OK) == 0);
	assert(access(sentinel_path, F_OK) != 0);

	song_metadata_t meta;
	memset(&meta, 0, sizeof(meta));
	metadata_read(ready_path, &meta);
	assert(strcmp(meta.title, "Roygbiv") == 0);
	assert(strcmp(meta.artist, "Boards of Canada") == 0);
	assert(strcmp(meta.album, "MHTRTC") == 0);

	char *tags_content = read_file_content(tags_path);
	assert(tags_content != NULL && strstr(tags_content, "duration=151") != NULL);
	free(tags_content);

	/* Second get hits local cache without hitting HTTP again */
	int prev_reqs = 0;
	pthread_mutex_lock(&srv->mu);
	prev_reqs = srv->stream_requests;
	srv->slow_stream = false;
	pthread_mutex_unlock(&srv->mu);

	char hit_path[512] = {0};
	assert(jellyfincache_get(&item, false, hit_path, sizeof(hit_path), err, sizeof(err)));
	assert(strcmp(hit_path, ready_path) == 0);
	pthread_mutex_lock(&srv->mu);
	assert(srv->stream_requests == prev_reqs);
	pthread_mutex_unlock(&srv->mu);

	/* Non-growfile codec (m4a) waits for full download before returning */
	jellyfin_entry_t m4a = item;
	snprintf(m4a.id, sizeof(m4a.id), "trk-m4a");
	snprintf(m4a.container, sizeof(m4a.container), "m4a");
	char m4a_path[512] = {0};
	assert(jellyfincache_get(&m4a, false, m4a_path, sizeof(m4a_path), err, sizeof(err)));
	struct stat st;
	assert(stat(m4a_path, &st) == 0 && st.st_size == 96 * 1024);

	/* Cache stats + active track protection on clear + clear_on_exit full wipe */
	uint64_t bytes = 0;
	int tracks = 0;
	jellyfincache_stats(&tracks, &bytes);
	assert(tracks >= 2 && bytes >= 2u * 96u * 1024u);

	jellyfincache_protect(ready_path);
	assert(jellyfincache_clear());
	assert(access(ready_path, R_OK) == 0);

	jellyfincache_clear_on_exit();
	jellyfincache_stats(&tracks, &bytes);
	assert(tracks == 0 && bytes == 0);

	/* Logout & forget server removes credentials and calls /Sessions/Logout */
	assert(jellyfin_logout_and_forget());
	assert(!jellyfin_logged_in());
	pthread_mutex_lock(&srv->mu);
	assert(srv->logout_count >= 1);
	pthread_mutex_unlock(&srv->mu);

	printf("PASS stream_urls_and_cache\n");
}

int main(void) {
	char tmp_dir[] = "/tmp/test_jellyfin_XXXXXX";
	assert(mkdtemp(tmp_dir) != NULL);

	char cfg_path[512];
	snprintf(cfg_path, sizeof(cfg_path), "%s/sonix.cfg", tmp_dir);
	config_init(cfg_path);
	jellyfin_init();

	mock_server_t srv;
	mock_server_start(&srv);

	test_url_normalization();
	test_auth_and_credentials(&srv, tmp_dir);
	test_quick_connect(&srv);
	test_browse_search_favorites_and_reporting(&srv);
	test_stream_urls_and_cache(&srv, tmp_dir);

	mock_server_stop(&srv);
	rm_rf(tmp_dir);
	printf("PASS jellyfin (all tests)\n");
	return 0;
}
