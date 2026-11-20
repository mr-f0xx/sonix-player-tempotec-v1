#define _GNU_SOURCE
#include "dlna_cast.h"

#include "src/system/remote/dlna.h"
#include "src/system/decode/decode.h"

#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef SSDP_ADDR
#define SSDP_ADDR "239.255.255.250"
#endif
#ifndef SSDP_PORT
#define SSDP_PORT 1900
#endif
#define DESCRIPTION_MAX (96 * 1024)
#define HTTP_REQUEST_MAX 8192
#define FILE_CHUNK 32768
// A renderer opens two or three connections to one stream: a probe, the
// stream itself, and another one every time it seeks. They are served at the
// same time, so a slow reader cannot hold up the next request.
#define MEDIA_CLIENTS_MAX 4
#define MEDIA_CLIENT_STACK (128 * 1024)

#define URL_MAX 1024
#define PATH_MAXIMUM 1024
#define CONTROL_MAX 1024
#define TOKEN_MAX 40
#define SUFFIX_MAX 16
#define MIME_MAX 64

typedef struct {
	char name[DLNA_CAST_NAME_MAX];
	char location[URL_MAX];
	char control_url[CONTROL_MAX];
	char service_type[160];
	// ConnectionManager is optional, and it is the only place a renderer says
	// which mime types it accepts -- see choose_mime().
	char manager_url[CONTROL_MAX];
	char manager_type[160];
} renderer_t;

static pthread_mutex_t cast_lock = PTHREAD_MUTEX_INITIALIZER;
static renderer_t renderers[DLNA_CAST_MAX_TARGETS];
static int renderer_count;
static int selected_renderer = -1;
static bool scanning;
static bool casting;
static char cast_message[160] = "";

static pthread_mutex_t media_lock = PTHREAD_MUTEX_INITIALIZER;
// What the media server is handing out right now. Published as a whole, under
// the lock, so a request can never see one cast's path beside another's token.
typedef struct {
	char path[PATH_MAXIMUM];
	char token[TOKEN_MAX];
	char suffix[SUFFIX_MAX]; // ".mp3": the tail of the URL, because some
							 // televisions decide what a stream is from its
							 // name and look at nothing else
	char mime[MIME_MAX];
	char features[192]; // the fourth field of protocolInfo, and the
						// contentFeatures.dlna.org response header
} media_service_t;
static media_service_t media;
static int media_listener = -1;
static unsigned token_sequence;
static uint16_t media_port;
static pthread_once_t media_once = PTHREAD_ONCE_INIT;

static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
static int client_count;

static void set_message(const char *message) {
	pthread_mutex_lock(&cast_lock);
	snprintf(cast_message, sizeof(cast_message), "%s", message ? message : "");
	pthread_mutex_unlock(&cast_lock);
}

void dlna_cast_status(char *out, size_t size) {
	if (!out || size == 0) return;
	pthread_mutex_lock(&cast_lock);
	snprintf(out, size, "%s", scanning ? "Searching for renderers…" : (casting ? "Sending to renderer…" : cast_message));
	pthread_mutex_unlock(&cast_lock);
}

bool dlna_cast_scan_running(void) {
	pthread_mutex_lock(&cast_lock);
	bool running = scanning;
	pthread_mutex_unlock(&cast_lock);
	return running;
}

int dlna_cast_targets(dlna_cast_target_t *out, int max) {
	if (!out || max <= 0) return 0;
	pthread_mutex_lock(&cast_lock);
	int n = renderer_count < max ? renderer_count : max;
	for (int i = 0; i < n; i++) {
		memcpy(out[i].name, renderers[i].name, sizeof(out[i].name));
		out[i].name[sizeof(out[i].name) - 1] = '\0';
	}
	pthread_mutex_unlock(&cast_lock);
	return n;
}

int dlna_cast_selected(void) {
	pthread_mutex_lock(&cast_lock);
	int selected = selected_renderer;
	pthread_mutex_unlock(&cast_lock);
	return selected;
}

void dlna_cast_select(int index) {
	pthread_mutex_lock(&cast_lock);
	selected_renderer = index >= 0 && index < renderer_count ? index : -1;
	if (selected_renderer >= 0) snprintf(cast_message, sizeof(cast_message), "Selected: %s", renderers[selected_renderer].name);
	pthread_mutex_unlock(&cast_lock);
}

// Small XML helpers: device descriptions are short, trusted LAN responses;
// still bound every copy and accept only complete element values.
static bool xml_value(const char *xml, const char *tag, char *out, size_t size) {
	char open[96], close[96];
	if (!xml || !tag || !out || size == 0) return false;
	snprintf(open, sizeof(open), "<%s", tag);
	const char *p = xml;
	while ((p = strstr(p, open)) != NULL) {
		const char *after_name = p + strlen(open);
		// A prefix match such as <friendlyNameExtra> is not the requested tag.
		if (*after_name != '>' && *after_name != ' ' && *after_name != '\t' && *after_name != '\r' && *after_name != '\n') {
			p = after_name;
			continue;
		}
		const char *gt = strchr(after_name, '>');
		if (!gt) return false;
		snprintf(close, sizeof(close), "</%s>", tag);
		const char *end = strstr(gt + 1, close);
		if (!end) return false;
		size_t n = (size_t)(end - (gt + 1));
		if (n >= size) n = size - 1;
		memcpy(out, gt + 1, n);
		out[n] = '\0';
		return true;
	}
	return false;
}

static void xml_unescape(char *text) {
	if (!text) return;
	char *r = text, *w = text;
	while (*r) {
		if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; }
		else if (strncmp(r, "&lt;", 4) == 0) { *w++ = '<'; r += 4; }
		else if (strncmp(r, "&gt;", 4) == 0) { *w++ = '>'; r += 4; }
		else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"'; r += 6; }
		else if (strncmp(r, "&apos;", 6) == 0) { *w++ = '\''; r += 6; }
		else *w++ = *r++;
	}
	*w = '\0';
}

static bool parse_url_origin(const char *url, char *origin, size_t osz, char *base_path, size_t bsz) {
	if (!url || strncasecmp(url, "http://", 7) != 0) return false;
	const char *host = url + 7;
	const char *slash = strchr(host, '/');
	const char *query = strchr(host, '?');
	const char *end = slash;
	if (!end || (query && query < end)) end = query;
	if (!end) end = url + strlen(url);
	if (end == host || (size_t)(end - url) >= osz) return false;
	memcpy(origin, url, (size_t)(end - url));
	origin[end - url] = '\0';
	if (end == url + strlen(url) || *end == '?') snprintf(base_path, bsz, "/");
	else snprintf(base_path, bsz, "%s", end);
	return true;
}

static bool resolve_url(const char *base, const char *ref, char *out, size_t size) {
	if (!ref || !ref[0] || !out || size == 0) return false;
	if (strncasecmp(ref, "http://", 7) == 0) return snprintf(out, size, "%s", ref) < (int)size;
	char origin[URL_MAX], path[URL_MAX];
	if (!parse_url_origin(base, origin, sizeof(origin), path, sizeof(path))) return false;
	if (ref[0] == '/') return snprintf(out, size, "%s%s", origin, ref) < (int)size;
	char *last = strrchr(path, '/');
	if (!last) return false;
	last[1] = '\0';
	return snprintf(out, size, "%s%s%s", origin, path, ref) < (int)size;
}

static bool get_renderer(const char *location, renderer_t *out) {
	char *xml = NULL;
	size_t length = 0;
	if (!http_get(location, &xml, &length, DESCRIPTION_MAX, 3) || !xml) return false;
	(void)length;
	char name[DLNA_CAST_NAME_MAX] = "DLNA renderer";
	char device_type[192] = "";
	if (xml_value(xml, "deviceType", device_type, sizeof(device_type)) &&
		!strstr(device_type, "MediaRenderer")) {
		free(xml);
		return false;
	}
	if (xml_value(xml, "friendlyName", name, sizeof(name))) xml_unescape(name);
	const char *list = strstr(xml, "<serviceList");
	if (!list) { free(xml); return false; }
	const char *list_end = strstr(list, "</serviceList>");
	if (!list_end) { free(xml); return false; }
	char service_type[160] = "", control[CONTROL_MAX] = "";
	char manager_type[160] = "", manager[CONTROL_MAX] = "";
	const char *svc = list;
	while ((svc = strstr(svc, "<service")) && svc < list_end) {
		const char *svc_end = strstr(svc, "</service>");
		if (!svc_end || svc_end > list_end) break;
		size_t len = (size_t)(svc_end - svc) + strlen("</service>");
		char *section = malloc(len + 1);
		if (!section) break;
		memcpy(section, svc, len); section[len] = '\0';
		char st[160] = "", cu[CONTROL_MAX] = "";
		if (xml_value(section, "serviceType", st, sizeof(st)) &&
			xml_value(section, "controlURL", cu, sizeof(cu))) {
			xml_unescape(cu);
			if (strstr(st, "AVTransport") && !control[0]) {
				snprintf(service_type, sizeof(service_type), "%s", st);
				snprintf(control, sizeof(control), "%s", cu);
			} else if (strstr(st, "ConnectionManager") && !manager[0]) {
				// Kept for choose_mime(): a renderer that names its formats
				// in a spelling this player would not guess can only be
				// asked, not deduced.
				snprintf(manager_type, sizeof(manager_type), "%s", st);
				snprintf(manager, sizeof(manager), "%s", cu);
			}
		}
		free(section);
		if (control[0] && manager[0]) break;
		svc = svc_end + 10;
	}
	free(xml);
	if (!control[0] || !resolve_url(location, control, out->control_url, sizeof(out->control_url))) return false;
	// ConnectionManager is optional: a renderer without one is cast to all the
	// same, on the format's preferred spelling.
	out->manager_url[0] = '\0';
	out->manager_type[0] = '\0';
	if (manager[0] && resolve_url(location, manager, out->manager_url, sizeof(out->manager_url))) {
		snprintf(out->manager_type, sizeof(out->manager_type), "%s", manager_type);
	}
	snprintf(out->name, sizeof(out->name), "%s", name[0] ? name : "DLNA renderer");
	snprintf(out->location, sizeof(out->location), "%s", location);
	snprintf(out->service_type, sizeof(out->service_type), "%s", service_type);
	return true;
}

static bool already_seen(const char *location) {
	for (int i = 0; i < renderer_count; i++) if (strcmp(renderers[i].location, location) == 0) return true;
	return false;
}

static void *scan_worker(void *unused) {
	(void)unused;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) goto failed;
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	struct sockaddr_in dest = {0};
	dest.sin_family = AF_INET;
	dest.sin_port = htons(SSDP_PORT);
	inet_aton(SSDP_ADDR, &dest.sin_addr);
	const char request[] =
		"M-SEARCH * HTTP/1.1\r\n"
		"HOST: 239.255.255.250:1900\r\n"
		"MAN: \"ssdp:discover\"\r\n"
		"MX: 2\r\n"
		"ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";
	if (sendto(fd, request, sizeof(request) - 1, 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
		close(fd); goto failed;
	}
	struct pollfd pfd = {.fd = fd, .events = POLLIN};
	int elapsed = 0;
	while (elapsed < 3500 && renderer_count < DLNA_CAST_MAX_TARGETS) {
		int ready = poll(&pfd, 1, 250);
		elapsed += 250;
		if (ready <= 0) continue;
		char packet[4096];
		ssize_t n = recv(fd, packet, sizeof(packet) - 1, 0);
		if (n <= 0) continue;
		packet[n] = '\0';
		char *line = packet;
		char location[URL_MAX] = "";
		while (*line) {
			char *next = strstr(line, "\r\n");
			if (!next) break;
			*next = '\0';
			if (strncasecmp(line, "LOCATION:", 9) == 0) {
				line += 9;
				while (*line == ' ' || *line == '\t') line++;
				snprintf(location, sizeof(location), "%s", line);
				break;
			}
			line = next + 2;
		}
		if (!location[0]) continue;
		pthread_mutex_lock(&cast_lock);
		bool duplicate = already_seen(location) || renderer_count >= DLNA_CAST_MAX_TARGETS;
		pthread_mutex_unlock(&cast_lock);
		if (duplicate) continue;
		renderer_t item = {0};
		if (!get_renderer(location, &item)) continue;
		pthread_mutex_lock(&cast_lock);
		if (!already_seen(location) && renderer_count < DLNA_CAST_MAX_TARGETS) renderers[renderer_count++] = item;
		pthread_mutex_unlock(&cast_lock);
	}
	close(fd);
	pthread_mutex_lock(&cast_lock);
	scanning = false;
	if (renderer_count == 0) snprintf(cast_message, sizeof(cast_message), "No DLNA renderers found");
	else snprintf(cast_message, sizeof(cast_message), "Found %d renderer%s", renderer_count, renderer_count == 1 ? "" : "s");
	pthread_mutex_unlock(&cast_lock);
	return NULL;
failed:
	pthread_mutex_lock(&cast_lock);
	scanning = false;
	snprintf(cast_message, sizeof(cast_message), "Renderer search failed");
	pthread_mutex_unlock(&cast_lock);
	return NULL;
}

void dlna_cast_scan(void) {
	pthread_mutex_lock(&cast_lock);
	if (scanning || casting) { pthread_mutex_unlock(&cast_lock); return; }
	scanning = true;
	renderer_count = 0;
	selected_renderer = -1;
	cast_message[0] = '\0';
	pthread_mutex_unlock(&cast_lock);
	pthread_t thread;
	if (pthread_create(&thread, NULL, scan_worker, NULL) != 0) {
		pthread_mutex_lock(&cast_lock); scanning = false;
		snprintf(cast_message, sizeof(cast_message), "Could not start renderer search");
		pthread_mutex_unlock(&cast_lock);
	} else pthread_detach(thread);
}

static bool send_all(int fd, const void *data, size_t length) {
	const char *p = data;
	while (length) {
		ssize_t n = send(fd, p, length, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return false;
		p += n; length -= (size_t)n;
	}
	return true;
}

// ---------------------------------------------------------------------------
// what a renderer is told the track is
// ---------------------------------------------------------------------------
//
// DLNA does not let a renderer judge a stream by Content-Type alone. The
// fourth field of the DIDL's protocolInfo and the contentFeatures.dlna.org
// response header carry a feature string, a television that follows DLNA
// (Samsung, LG and Sony in particular) will not play a track without it, and
// what it says when it refuses is that the format is not valid -- [#363].
// "http-get:*:audio/mpeg:*", what this player used to send, says only "some
// MPEG audio"; the feature string says what can be done with the stream.
//
//   DLNA.ORG_OP=01   byte-range seeks are served, time seeks are not (see
//                    parse_range())
//   DLNA.ORG_CI=0    the bytes are the file on the card, not converted
//   DLNA.ORG_FLAGS   01700000...: DLNA 1.5, streaming and interactive
//                    transfer modes, background transfer allowed -- the set
//                    minidlna and every other media server sends.
#define DLNA_ORG_OP_CI "DLNA.ORG_OP=01;DLNA.ORG_CI=0"
#define DLNA_ORG_FLAGS "DLNA.ORG_FLAGS=01700000000000000000000000000000"

typedef struct {
	// Also the tail of the URL: a renderer given "/sonix/<token>/track" with no
	// name has nothing to go on, and several say "not a valid video" rather
	// than guess.
	const char *extension;
	// DLNA.ORG_PN=..., or NULL where DLNA has no profile that fits every file
	// with this extension. MP3 and FLAC are named because the profile covers
	// the format exactly; a bare audio/mp4 may hold AAC or ALAC, and a wav may
	// hold samples the LPCM profile does not allow, so neither is named --
	// claiming a profile the bytes do not follow is a refusal, not a courtesy.
	const char *profile;
	// What this format may be called, preferred first. The renderer's own sink
	// list picks between them; the first is used when it expresses no opinion.
	const char *mimes[4];
} media_format_t;

static const media_format_t MEDIA_FORMATS[] = {
	{".mp3", "DLNA.ORG_PN=MP3", {"audio/mpeg", "audio/mp3", "audio/x-mp3", NULL}},
	{".flac", "DLNA.ORG_PN=FLAC", {"audio/flac", "audio/x-flac", NULL}},
	{".m4a", NULL, {"audio/mp4", "audio/x-m4a", "audio/m4a", NULL}},
	{".m4b", NULL, {"audio/mp4", "audio/x-m4a", "audio/m4a", NULL}},
	{".mp4", NULL, {"audio/mp4", "audio/x-m4a", "audio/m4a", NULL}},
	{".aac", NULL, {"audio/aac", "audio/x-aac", "audio/mp4", NULL}},
	{".wav", NULL, {"audio/wav", "audio/x-wav", "audio/wave", NULL}},
	{".ogg", NULL, {"audio/ogg", "application/ogg", "audio/x-vorbis+ogg", NULL}},
	{".oga", NULL, {"audio/ogg", "application/ogg", "audio/x-vorbis+ogg", NULL}},
	{".opus", NULL, {"audio/ogg", "audio/opus", "audio/x-opus+ogg", NULL}},
	{".aiff", NULL, {"audio/aiff", "audio/x-aiff", NULL}},
	{".aif", NULL, {"audio/aiff", "audio/x-aiff", NULL}},
	{".ape", NULL, {"audio/x-ape", "audio/ape", NULL}},
	{".wv", NULL, {"audio/x-wavpack", "audio/wavpack", NULL}},
	{".dsf", NULL, {"audio/x-dsf", "audio/dsd", NULL}},
	{".dff", NULL, {"audio/x-dff", "audio/dsd", NULL}},
};

#define MEDIA_FORMAT_COUNT (sizeof(MEDIA_FORMATS) / sizeof(MEDIA_FORMATS[0]))

static const media_format_t *format_for_path(const char *path) {
	const char *ext = strrchr(path ? path : "", '.');
	if (!ext) return NULL;
	for (size_t i = 0; i < MEDIA_FORMAT_COUNT; i++) {
		if (!strcasecmp(ext, MEDIA_FORMATS[i].extension)) return &MEDIA_FORMATS[i];
	}
	return NULL;
}

// The feature string for a format; also the fourth field of protocolInfo.
static void dlna_features(const media_format_t *format, char *out, size_t size) {
	if (format && format->profile)
		snprintf(out, size, "%s;%s;%s", format->profile, DLNA_ORG_OP_CI, DLNA_ORG_FLAGS);
	else
		snprintf(out, size, "%s;%s", DLNA_ORG_OP_CI, DLNA_ORG_FLAGS);
}

// A comma-separated sink list entry is "http-get:*:<mime>:<features>", so the
// mime is what sits between a ':' on each side. Matching on those boundaries
// keeps "audio/mpeg" from matching inside "audio/mpeg4-generic".
static bool sink_names_mime(const char *sink, const char *mime) {
	size_t len = strlen(mime);
	for (const char *p = sink; (p = strcasestr(p, mime)) != NULL; p += len) {
		if (p > sink && p[-1] == ':' && p[len] == ':') return true;
	}
	return false;
}

// "http-get:*:*:*" and friends: the renderer is not being specific, so any
// spelling will do. (Testing for ":*:" anywhere would match every entry --
// the second field of protocolInfo is always a star.)
static bool sink_accepts_anything(const char *sink) {
	return strstr(sink, "http-get:*:*:") != NULL || sink_names_mime(sink, "audio/*");
}

static bool soap_call(const char *url, const char *service_type, const char *action, const char *arguments,
					  char **response, size_t limit);

// The one thing a control point cannot know on its own is how a renderer
// spells a format: FLAC is "audio/flac" to some televisions and "audio/x-flac"
// to others, and a renderer that is sent the spelling it does not list refuses
// the track. ConnectionManager's GetProtocolInfo is where it says.
//
// Everything here is best effort. A renderer that does not answer, has no
// ConnectionManager, or lists nothing lands on the format's preferred
// spelling, and a cast that would have worked is not withheld because a
// preference could not be confirmed.
// True when the renderer's own sink list named a spelling of the format, so
// what gets sent is what the renderer says it plays rather than a guess: a
// renderer that lists nothing for FLAC gets a WAV instead (see cast_worker).
static bool choose_mime(const renderer_t *renderer, const media_format_t *format, char *out, size_t size) {
	snprintf(out, size, "%s", format->mimes[0]);
	if (!renderer->manager_url[0] || !renderer->manager_type[0]) return false;
	char *response = NULL;
	if (!soap_call(renderer->manager_url, renderer->manager_type, "GetProtocolInfo", NULL, &response, 32 * 1024) || !response) {
		free(response);
		return false;
	}
	char *sink = malloc(16 * 1024);
	if (!sink) { free(response); return false; }
	bool matched = false;
	if (xml_value(response, "Sink", sink, 16 * 1024)) {
		xml_unescape(sink);
		if (!sink_accepts_anything(sink)) {
			for (size_t i = 0; i < sizeof(format->mimes) / sizeof(format->mimes[0]) && format->mimes[i]; i++) {
				if (sink_names_mime(sink, format->mimes[i])) {
					snprintf(out, size, "%s", format->mimes[i]);
					matched = true;
					break;
				}
			}
		} else {
			// http-get:*:*:* or audio/*: anything goes, the preferred spelling is fine
			matched = true;
		}
	}
	free(sink);
	free(response);
	return matched;
}

// ---------------------------------------------------------------------------
// the media server: the track, on a socket the renderer can reach
// ---------------------------------------------------------------------------

// One header out of a request. Names are compared without case, as the spec
// says, and the value is copied bounded: a renderer is free to send a Range
// that makes no sense.
static bool header_value(const char *request, const char *name, char *out, size_t size) {
	size_t len = strlen(name);
	const char *line = strstr(request, "\r\n");
	while (line) {
		line += 2;
		if (line[0] == '\r' && line[1] == '\n') break; // end of the headers
		if (strncasecmp(line, name, len) == 0 && line[len] == ':') {
			const char *value = line + len + 1;
			while (*value == ' ' || *value == '\t') value++;
			const char *end = strstr(value, "\r\n");
			size_t n = end ? (size_t)(end - value) : strlen(value);
			if (n >= size) n = size - 1;
			memcpy(out, value, n);
			out[n] = '\0';
			return true;
		}
		line = strstr(line, "\r\n");
	}
	return false;
}

typedef enum {
	RANGE_NONE,			 // no range asked for, or one that cannot be honoured
	RANGE_PARTIAL,		 // start and end are set
	RANGE_UNSATISFIABLE, // entirely past the end of the file: 416
} range_result_t;

// "bytes=" ranges, the only unit a renderer on a LAN sends.
//
// The open-ended form is the one that matters: "Range: bytes=0-" asks for the
// whole file, because the second number means "to the end" when it is missing.
// Reading that missing number as a zero -- which is what this player did --
// answered with a one-byte body, and a television handed one byte of an MP3
// reports the file as not a valid format. That was [#363], and it explains a
// failure that had nothing to do with the format of the file.
//
// A list of ranges is not honoured: the answer is the whole file, which DLNA
// allows, and no renderer asks for one while playing a single track.
static range_result_t parse_range(const char *value, uint64_t size, off_t *start, off_t *end) {
	if (size == 0 || !value || strncasecmp(value, "bytes=", 6) != 0) return RANGE_NONE;
	const char *first_digit = value + 6;
	if (strchr(first_digit, ',')) return RANGE_NONE;
	const char *dash = strchr(first_digit, '-');
	if (!dash) return RANGE_NONE;
	const char *tail = dash + 1;
	size_t first_len = (size_t)(dash - first_digit), second_len = strcspn(tail, " \t\r\n");
	char first[32], second[32];
	if (first_len >= sizeof(first) || second_len >= sizeof(second)) return RANGE_NONE;
	memcpy(first, first_digit, first_len);
	first[first_len] = '\0';
	memcpy(second, tail, second_len);
	second[second_len] = '\0';
	char *after = NULL;
	if (first_len == 0) {
		// "-N": the last N bytes.
		if (second_len == 0) return RANGE_NONE;
		unsigned long long n = strtoull(second, &after, 10);
		if (!after || *after) return RANGE_NONE;
		if (n == 0) return RANGE_UNSATISFIABLE;
		*start = (off_t)(n >= size ? 0 : size - n);
		*end = (off_t)(size - 1);
		return RANGE_PARTIAL;
	}
	unsigned long long a = strtoull(first, &after, 10);
	if (!after || *after) return RANGE_NONE;
	if (a >= size) return RANGE_UNSATISFIABLE;
	*start = (off_t)a;
	*end = (off_t)(size - 1);
	if (second_len) {
		unsigned long long b = strtoull(second, &after, 10);
		if (!after || *after) return RANGE_NONE;
		if (b < a) return RANGE_NONE;
		if (b < size) *end = (off_t)b; // a last byte past the end means "to the end"
	}
	return RANGE_PARTIAL;
}

static void http_date(char *out, size_t size) {
	// Spelled out rather than through strftime(): %a and %b follow the locale,
	// and this header is read by machines.
	static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
										 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	time_t now = time(NULL);
	struct tm tm;
	if (!gmtime_r(&now, &tm)) {
		snprintf(out, size, "Thu, 01 Jan 1970 00:00:00 GMT");
		return;
	}
	snprintf(out, size, "%s, %02d %s %04d %02d:%02d:%02d GMT", DAYS[tm.tm_wday % 7], tm.tm_mday,
			 MONTHS[tm.tm_mon % 12], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void serve_media_client(int client) {
	struct timeval timeout = {.tv_sec = 4, .tv_usec = 0};
	struct timeval send_timeout = {.tv_sec = 12, .tv_usec = 0};
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
	char request[HTTP_REQUEST_MAX + 1];
	size_t used = 0;
	while (used < HTTP_REQUEST_MAX) {
		ssize_t n = recv(client, request + used, HTTP_REQUEST_MAX - used, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return;
		used += (size_t)n;
		request[used] = '\0';
		if (strstr(request, "\r\n\r\n")) break;
	}
	if (!strstr(request, "\r\n\r\n")) return;
	char method[8] = "", uri[512] = "";
	if (sscanf(request, "%7s %511s", method, uri) != 2) return;
	bool head = strcmp(method, "HEAD") == 0;
	if (!head && strcmp(method, "GET") != 0) {
		const char response[] = "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
		send_all(client, response, sizeof(response) - 1); return;
	}
	media_service_t service;
	pthread_mutex_lock(&media_lock);
	service = media;
	pthread_mutex_unlock(&media_lock);
	// The name carries the token and the format's extension, both of which a
	// renderer may look at: the token so that one cast's URL is not another's,
	// the extension because several decide what a stream is from its name.
	char expected[TOKEN_MAX + SUFFIX_MAX + 32];
	snprintf(expected, sizeof(expected), "/sonix/%s/track%s", service.token, service.suffix);
	if (!service.token[0] || strcmp(uri, expected) != 0) {
		const char response[] = "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
		send_all(client, response, sizeof(response) - 1); return;
	}
	int file = open(service.path, O_RDONLY);
	struct stat st;
	if (file < 0 || fstat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
		if (file >= 0) close(file);
		const char response[] = "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
		send_all(client, response, sizeof(response) - 1); return;
	}
	char range[128] = "";
	off_t start = 0, end = st.st_size > 0 ? st.st_size - 1 : 0;
	range_result_t ranged = RANGE_NONE;
	if (header_value(request, "Range", range, sizeof(range))) {
		ranged = parse_range(range, (uint64_t)st.st_size, &start, &end);
	}
	if (ranged == RANGE_NONE) start = 0;
	uint64_t length = ranged == RANGE_PARTIAL ? (uint64_t)(end - start + 1) : (uint64_t)st.st_size;
	if (ranged == RANGE_UNSATISFIABLE) {
		char headers[256];
		snprintf(headers, sizeof(headers),
				 "HTTP/1.1 416 Range Not Satisfiable\r\n"
				 "Connection: close\r\n"
				 "Content-Range: bytes */%llu\r\n"
				 "Content-Length: 0\r\n\r\n",
				 (unsigned long long)st.st_size);
		send_all(client, headers, strlen(headers));
		close(file);
		return;
	}
	// DLNA lets a renderer ask for the transfer to be paced differently, and
	// Background is the one worth honouring: the renderer wants the file ahead
	// of playing it.
	char transfer_mode[16] = "Streaming", requested[16] = "";
	if (header_value(request, "transferMode.dlna.org", requested, sizeof(requested)) &&
		!strcasecmp(requested, "Background")) {
		snprintf(transfer_mode, sizeof(transfer_mode), "Background");
	}
	char date[64];
	http_date(date, sizeof(date));
	// contentFeatures.dlna.org is what a DLNA television reads instead of
	// trusting Content-Type, and it is sent on every response rather than only
	// when asked for with getcontentFeatures.dlna.org: a renderer that needs it
	// and forgets to ask is still served correctly.
	char headers[900];
	int n = snprintf(headers, sizeof(headers),
					 "HTTP/1.1 %s\r\n"
					 "Server: Linux/3.0 UPnP/1.0 SonixPlayer/1.0\r\n"
					 "Date: %s\r\n"
					 "Connection: close\r\n"
					 "Accept-Ranges: bytes\r\n"
					 "Content-Type: %s\r\n"
					 "Content-Length: %llu\r\n"
					 "contentFeatures.dlna.org: %s\r\n"
					 "transferMode.dlna.org: %s\r\n",
					 ranged == RANGE_PARTIAL ? "206 Partial Content" : "200 OK", date,
					 service.mime[0] ? service.mime : "application/octet-stream",
					 (unsigned long long)length, service.features, transfer_mode);
	if (n < 0 || (size_t)n >= sizeof(headers)) { close(file); return; }
	if (ranged == RANGE_PARTIAL) {
		int added = snprintf(headers + n, sizeof(headers) - (size_t)n, "Content-Range: bytes %llu-%llu/%llu\r\n",
							 (unsigned long long)start, (unsigned long long)end,
							 (unsigned long long)st.st_size);
		if (added < 0 || (size_t)added >= sizeof(headers) - (size_t)n) { close(file); return; }
		n += added;
	}
	int added = snprintf(headers + n, sizeof(headers) - (size_t)n, "\r\n");
	if (added < 0 || (size_t)added >= sizeof(headers) - (size_t)n) { close(file); return; }
	if (!send_all(client, headers, strlen(headers)) || head || length == 0) { close(file); return; }
	if (lseek(file, start, SEEK_SET) < 0) { close(file); return; }
	char buffer[FILE_CHUNK];
	uint64_t left = length;
	while (left) {
		size_t want = left > sizeof(buffer) ? sizeof(buffer) : (size_t)left;
		ssize_t n = read(file, buffer, want);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0 || !send_all(client, buffer, (size_t)n)) break;
		left -= (uint64_t)n;
	}
	close(file);
}

static void *media_client_thread(void *arg) {
	int client = (int)(intptr_t)arg;
	serve_media_client(client);
	close(client);
	pthread_mutex_lock(&client_lock);
	client_count--;
	pthread_mutex_unlock(&client_lock);
	return NULL;
}

static void *media_server(void *unused) {
	(void)unused;
	for (;;) {
		struct pollfd pfd = {.fd = media_listener, .events = POLLIN};
		if (poll(&pfd, 1, 1000) <= 0) continue;
		int client = accept(media_listener, NULL, NULL);
		if (client < 0) continue;
		pthread_mutex_lock(&client_lock);
		bool room = client_count < MEDIA_CLIENTS_MAX;
		if (room) client_count++;
		pthread_mutex_unlock(&client_lock);
		if (!room) {
			const char response[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
			send_all(client, response, sizeof(response) - 1);
			close(client);
			continue;
		}
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, MEDIA_CLIENT_STACK);
		pthread_t thread;
		if (pthread_create(&thread, &attr, media_client_thread, (void *)(intptr_t)client) == 0) {
			pthread_detach(thread);
		} else {
			// No thread to spare: serve it here rather than drop a renderer
			// that is already playing.
			media_client_thread((void *)(intptr_t)client);
		}
		pthread_attr_destroy(&attr);
	}
	return NULL;
}

static void media_server_start(void) {
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(0); // Ask the kernel for an unused port.
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return;
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) { close(fd); return; }
	socklen_t addr_len = sizeof(addr);
	if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) { close(fd); return; }
	media_listener = fd;
	media_port = ntohs(addr.sin_port);
	pthread_t thread;
	if (pthread_create(&thread, NULL, media_server, NULL) != 0) { close(fd); media_listener = -1; media_port = 0; return; }
	pthread_detach(thread);
}

static bool ensure_media_server(void) {
	pthread_once(&media_once, media_server_start);
	return media_listener >= 0 && media_port != 0;
}

// What the server answers with, published in one piece under the media lock.
static void media_publish(const char *path, const char *token, const char *suffix, const char *mime,
						  const char *features) {
	pthread_mutex_lock(&media_lock);
	snprintf(media.path, sizeof(media.path), "%s", path ? path : "");
	snprintf(media.token, sizeof(media.token), "%s", token ? token : "");
	snprintf(media.suffix, sizeof(media.suffix), "%s", suffix ? suffix : "");
	snprintf(media.mime, sizeof(media.mime), "%s", mime ? mime : "");
	snprintf(media.features, sizeof(media.features), "%s", features ? features : "");
	pthread_mutex_unlock(&media_lock);
}

static void media_withdraw(void) {
	pthread_mutex_lock(&media_lock);
	media.token[0] = '\0';
	media.path[0] = '\0';
	pthread_mutex_unlock(&media_lock);
}

static void xml_escape(const char *src, char *out, size_t size) {
	size_t used = 0;
	if (!src) src = "";
	for (; *src && used + 7 < size; src++) {
		const char *rep = NULL;
		switch (*src) { case '&': rep = "&amp;"; break; case '<': rep = "&lt;"; break; case '>': rep = "&gt;"; break; case '\"': rep = "&quot;"; break; case '\'': rep = "&apos;"; break; }
		if (rep) { size_t n = strlen(rep); memcpy(out + used, rep, n); used += n; }
		else out[used++] = *src;
	}
	out[used] = '\0';
}

// One SOAP action against one service. The response is handed back malloc'd
// for callers that read it -- GetProtocolInfo's sink list is the only one that
// does -- and dropped for the rest.
static bool soap_call(const char *url, const char *service_type, const char *action, const char *arguments,
					  char **response, size_t limit) {
	char body[8192], extra[512];
	int n = snprintf(body, sizeof(body),
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
		"<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
		action, service_type, arguments ? arguments : "", action);
	if (n < 0 || (size_t)n >= sizeof(body)) return false;
	snprintf(extra, sizeof(extra), "SOAPAction: \"%s#%s\"\r\n", service_type, action);
	http_req_t req = {.method = "POST", .extra_headers = extra, .body = body,
		.body_len = strlen(body), .content_type = "text/xml; charset=\"utf-8\"", .allow_empty_body = true};
	char *received = NULL;
	int status = 0;
	bool ok = http_request(url, &req, &received, NULL, limit, 8, &status);
	if (!ok || status < 200 || status >= 300) {
		free(received);
		return false;
	}
	if (response) *response = received;
	else free(received);
	return true;
}

static bool soap(const renderer_t *renderer, const char *action, const char *arguments) {
	return soap_call(renderer->control_url, renderer->service_type, action, arguments, NULL, 16 * 1024);
}

// A renderer that does not list FLAC among the formats it plays gets WAV:
// the FLAC is decoded to 16-bit PCM and wrapped in a plain RIFF header, which
// every DLNA television accepts where an "audio/flac" stream is refused as an
// illegal MIME type (issue #365). The file lands in the DLNA cache next to the
// tracks phones send over, so both kinds of transient track live in one place
// and are cleared together. The decoder is the unified one in system/decode:
// dr_flac behind it, opened once and read to the end.
static bool flac_to_wav(const char *flac_path, char *wav_path, size_t size) {
	char dir[DLNA_PATH_MAX];
	if (!dlna_cache_dir(dir, sizeof(dir))) return false;

	// Casts are serialised: at most one converted file should be lying around,
	// so drop the WAV a previous cast left behind. A renderer still playing it
	// got Stop first -- a new cast always follows one.
	{
		DIR *d = opendir(dir);
		if (d) {
			struct dirent *e;
			while ((e = readdir(d)) != NULL) {
				if (strncmp(e->d_name, "cast-", 5) != 0) continue;
				size_t len = strlen(e->d_name);
				if (len > 4 && strcmp(e->d_name + len - 4, ".wav") == 0) {
					char gone[DLNA_PATH_MAX + 32];
					if (snprintf(gone, sizeof(gone), "%s/%s", dir, e->d_name) < (int)sizeof(gone)) {
						remove(gone);
					}
				}
			}
			closedir(d);
		}
	}

	// Overwriting a file a renderer may still be fetching is off the table:
	// each cast gets its own name.
	snprintf(wav_path, size, "%s/cast-%lu.wav", dir, (unsigned long)time(NULL));

	decoder_t *dec = decoder_open(flac_path, DECODE_FORMAT_FLAC);
	if (!dec) return false;
	int channels = decoder_channels(dec);
	int rate = decoder_sample_rate(dec);
	if (channels <= 0 || rate <= 0) {
		decoder_close(dec);
		return false;
	}

	FILE *out = fopen(wav_path, "wb");
	if (!out) {
		decoder_close(dec);
		return false;
	}

	// RIFF header, both sizes left blank until the decode is done.
	uint32_t byte_rate = (uint32_t)rate * (uint32_t)channels * 2;
	uint16_t block_align = (uint16_t)(channels * 2);
	const char riff[4] = {'R', 'I', 'F', 'F'};
	const char wave[4] = {'W', 'A', 'V', 'E'};
	const char fmt[4] = {'f', 'm', 't', ' '};
	const char data[4] = {'d', 'a', 't', 'a'};
	uint32_t riff_size = 0, data_size = 0;
	uint16_t format_tag = 1, bits = 16;
	uint32_t fmt_size = 16;
	fwrite(riff, 1, 4, out);
	fwrite(&riff_size, 4, 1, out);
	fwrite(wave, 1, 4, out);
	fwrite(fmt, 1, 4, out);
	fwrite(&fmt_size, 4, 1, out);
	fwrite(&format_tag, 2, 1, out);
	fwrite(&channels, 2, 1, out);
	fwrite(&rate, 4, 1, out);
	fwrite(&byte_rate, 4, 1, out);
	fwrite(&block_align, 2, 1, out);
	fwrite(&bits, 2, 1, out);
	fwrite(data, 1, 4, out);
	fwrite(&data_size, 4, 1, out);

	short buffer[4096];
	uint64_t frames;
	while ((frames = decoder_read_pcm_frames_s16(dec,
			(uint64_t)(sizeof(buffer) / sizeof(short) / (size_t)channels), buffer)) > 0) {
		if (fwrite(buffer, (size_t)frames * (size_t)channels * sizeof(short), 1, out) != 1) break;
		data_size += (uint32_t)frames * (uint32_t)channels * 2u;
	}
	bool ok = ferror(out) == 0 && data_size > 0;
	fclose(out);
	decoder_close(dec);

	if (!ok) {
		remove(wav_path);
		return false;
	}

	// Patch the two sizes the decoder could not know up front.
	out = fopen(wav_path, "r+b");
	if (out) {
		fseek(out, 0, SEEK_END);
		long file_size = ftell(out);
		if (file_size >= 0) {
			uint32_t riff_size_v = (uint32_t)file_size - 8;
			fseek(out, 4, SEEK_SET);
			fwrite(&riff_size_v, 4, 1, out);
			// RIFF(4) size(4) WAVE(4) fmt(4) fmtSize(4) = 20, tag(2) channels(2)
			// rate(4) byteRate(4) blockAlign(2) bits(2) = 16 more, data(4) at 36
			fseek(out, 40, SEEK_SET);
			fwrite(&data_size, 4, 1, out);
		}
		fclose(out);
	}
	return ok;
}

static void *cast_worker(void *arg) {
	char path[PATH_MAXIMUM];
	snprintf(path, sizeof(path), "%s", (char *)arg);
	free(arg);
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || access(path, R_OK) != 0) {
		set_message("This track cannot be shared over DLNA"); goto done;
	}
	if (!ensure_media_server()) { set_message("Could not start the DLNA media server"); goto done; }
	char ip[40] = "";
	if (!wifi_interface_address(ip, sizeof(ip)) || !ip[0]) { set_message("Wi-Fi is not connected"); goto done; }
	renderer_t target;
	pthread_mutex_lock(&cast_lock);
	int choice = selected_renderer;
	if (choice < 0 || choice >= renderer_count) { pthread_mutex_unlock(&cast_lock); set_message("Choose a DLNA renderer first"); goto done; }
	target = renderers[choice];
	pthread_mutex_unlock(&cast_lock);

	// How this renderer wants the format spelled, asked of its own sink list
	// (see choose_mime()). A renderer that does not say it plays FLAC gets a
	// converted WAV/LPCM instead: plain uncompressed audio every DLNA
	// television accepts, where "audio/flac" is refused as an illegal
	// MIME type (issue #365).
	const media_format_t *format = format_for_path(path);
	char mime[MIME_MAX] = "application/octet-stream";
	bool sink_speaks_flac = format && choose_mime(&target, format, mime, sizeof(mime));
	char serve_path[PATH_MAXIMUM] = "";
	const char *serve = path;
	const media_format_t *serve_format = format;
	if (format && !strcasecmp(format->extension, ".flac") && !sink_speaks_flac) {
		if (!flac_to_wav(path, serve_path, sizeof(serve_path))) {
			set_message("This FLAC track cannot be sent");
			goto done;
		}
		serve = serve_path;
		serve_format = format_for_path(serve);
		(void)choose_mime(&target, serve_format, mime, sizeof(mime));
	}
	char suffix[SUFFIX_MAX];
	snprintf(suffix, sizeof(suffix), "%s", serve_format ? serve_format->extension : "");
	char features[192];
	dlna_features(serve_format, features, sizeof(features));

	pthread_mutex_lock(&media_lock);
	unsigned serial = ++token_sequence;
	char token[TOKEN_MAX];
	snprintf(token, sizeof(token), "%08x%08x", (unsigned)getpid(), serial ^ (unsigned)time(NULL));
	pthread_mutex_unlock(&media_lock);
	media_publish(serve, token, suffix, mime, features);

	char uri[URL_MAX];
	if (snprintf(uri, sizeof(uri), "http://%s:%u/sonix/%s/track%s", ip, media_port, token, suffix) >= (int)sizeof(uri)) {
		set_message("Could not build the track URL"); goto done;
	}
	const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
	char title_source[160], safe_uri[256], safe_title[960];
	size_t title_len = strnlen(base, sizeof(title_source) - 1);
	memcpy(title_source, base, title_len);
	title_source[title_len] = '\0';
	xml_escape(title_source, safe_title, sizeof(safe_title));
	xml_escape(uri, safe_uri, sizeof(safe_uri));
	char metadata[2048], args[7600];
	// The fourth field of protocolInfo is the feature string, not a star: a
	// DLNA renderer reads it (and contentFeatures.dlna.org) to decide whether
	// it can play the track at all. The size lets a renderer that wants the
	// whole file ahead of time know how much that is.
	snprintf(metadata, sizeof(metadata),
		"<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
		"<item id=\"0\" parentID=\"0\" restricted=\"1\"><dc:title>%s</dc:title><upnp:class>object.item.audioItem.musicTrack</upnp:class>"
		"<res protocolInfo=\"http-get:*:%s:%s\" size=\"%llu\">%s</res></item></DIDL-Lite>",
		safe_title, mime, features, (unsigned long long)st.st_size, safe_uri);
	char safe_metadata[7000]; xml_escape(metadata, safe_metadata, sizeof(safe_metadata));
	snprintf(args, sizeof(args), "<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI><CurrentURIMetaData>%s</CurrentURIMetaData>", safe_uri, safe_metadata);
	set_message("Connecting to renderer…");
	if (!soap(&target, "SetAVTransportURI", args)) { set_message("Renderer rejected the track"); goto done; }
	if (!soap(&target, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>")) { set_message("Track queued, but renderer could not start playback"); goto done; }
	{
		char msg[160]; snprintf(msg, sizeof(msg), "Playing on %s", target.name); set_message(msg);
	}
done:
	pthread_mutex_lock(&cast_lock); casting = false; pthread_mutex_unlock(&cast_lock);
	return NULL;
}

bool dlna_cast_play(const char *path) {
	if (!path || !path[0]) { set_message("Nothing is playing"); return false; }
	pthread_mutex_lock(&cast_lock);
	if (scanning || casting || selected_renderer < 0 || selected_renderer >= renderer_count) {
		pthread_mutex_unlock(&cast_lock);
		set_message("Find and select a DLNA renderer first");
		return false;
	}
	casting = true;
	pthread_mutex_unlock(&cast_lock);
	char *copy = strdup(path);
	if (!copy) { pthread_mutex_lock(&cast_lock); casting = false; pthread_mutex_unlock(&cast_lock); set_message("Not enough memory"); return false; }
	pthread_t thread;
	if (pthread_create(&thread, NULL, cast_worker, copy) != 0) {
		free(copy); pthread_mutex_lock(&cast_lock); casting = false; pthread_mutex_unlock(&cast_lock); set_message("Could not start DLNA playback"); return false;
	}
	pthread_detach(thread);
	return true;
}

static void *stop_worker(void *arg) {
	renderer_t *renderer = arg;
	bool stopped = soap(renderer, "Stop", "<InstanceID>0</InstanceID>");
	free(renderer);
	if (stopped) media_withdraw();
	set_message(stopped ? "Stopped DLNA playback" : "Could not stop the renderer");
	pthread_mutex_lock(&cast_lock); casting = false; pthread_mutex_unlock(&cast_lock);
	return NULL;
}

void dlna_cast_stop(void) {
	pthread_mutex_lock(&cast_lock);
	if (casting || scanning || selected_renderer < 0 || selected_renderer >= renderer_count) {
		pthread_mutex_unlock(&cast_lock); return;
	}
	casting = true;
	renderer_t target = renderers[selected_renderer];
	pthread_mutex_unlock(&cast_lock);
	// Stop is best effort and must not block the caller (normally the UI).
	renderer_t *copy = malloc(sizeof(*copy));
	if (!copy) {
		pthread_mutex_lock(&cast_lock); casting = false; pthread_mutex_unlock(&cast_lock);
		set_message("Not enough memory");
		return;
	}
	*copy = target;
	pthread_t thread;
	if (pthread_create(&thread, NULL, stop_worker, copy) == 0) pthread_detach(thread);
	else {
		free(copy);
		pthread_mutex_lock(&cast_lock); casting = false; pthread_mutex_unlock(&cast_lock);
		set_message("Could not stop the renderer");
	}
}
