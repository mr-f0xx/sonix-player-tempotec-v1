#define _GNU_SOURCE
#include "dlna_cast.h"

#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#include <arpa/inet.h>
#include <ctype.h>
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
#include <time.h>
#include <unistd.h>

#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900
#define DESCRIPTION_MAX (96 * 1024)
#define HTTP_REQUEST_MAX 8192
#define FILE_CHUNK 32768

#define URL_MAX 1024
#define PATH_MAXIMUM 1024
#define CONTROL_MAX 1024
#define TOKEN_MAX 40

typedef struct {
	char name[DLNA_CAST_NAME_MAX];
	char location[URL_MAX];
	char control_url[CONTROL_MAX];
	char service_type[160];
} renderer_t;

static pthread_mutex_t cast_lock = PTHREAD_MUTEX_INITIALIZER;
static renderer_t renderers[DLNA_CAST_MAX_TARGETS];
static int renderer_count;
static int selected_renderer = -1;
static bool scanning;
static bool casting;
static char cast_message[160] = "";

static pthread_mutex_t media_lock = PTHREAD_MUTEX_INITIALIZER;
static char media_path[PATH_MAXIMUM];
static char media_token[TOKEN_MAX];
static int media_listener = -1;
static unsigned token_sequence;
static uint16_t media_port;
static pthread_once_t media_once = PTHREAD_ONCE_INIT;

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
	const char *svc = list;
	while ((svc = strstr(svc, "<service")) && svc < list_end) {
		const char *svc_end = strstr(svc, "</service>");
		if (!svc_end || svc_end > list_end) break;
		size_t len = (size_t)(svc_end - svc) + strlen("</service>");
		char *section = malloc(len + 1);
		if (!section) break;
		memcpy(section, svc, len); section[len] = '\0';
		char st[160] = "", cu[CONTROL_MAX] = "";
		if (xml_value(section, "serviceType", st, sizeof(st)) && strstr(st, "AVTransport") &&
			xml_value(section, "controlURL", cu, sizeof(cu))) {
			snprintf(service_type, sizeof(service_type), "%s", st);
			xml_unescape(cu);
			snprintf(control, sizeof(control), "%s", cu);
			free(section);
			break;
		}
		free(section);
		svc = svc_end + 10;
	}
	free(xml);
	if (!control[0] || !resolve_url(location, control, out->control_url, sizeof(out->control_url))) return false;
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

static const char *mime_for_path(const char *path) {
	const char *ext = strrchr(path ? path : "", '.');
	if (!ext) return "application/octet-stream";
	if (!strcasecmp(ext, ".mp3")) return "audio/mpeg";
	if (!strcasecmp(ext, ".flac")) return "audio/flac";
	if (!strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".aac")) return "audio/mp4";
	if (!strcasecmp(ext, ".wav")) return "audio/wav";
	if (!strcasecmp(ext, ".ogg") || !strcasecmp(ext, ".opus")) return "audio/ogg";
	if (!strcasecmp(ext, ".ape")) return "audio/ape";
	if (!strcasecmp(ext, ".wv")) return "audio/x-wavpack";
	return "application/octet-stream";
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
	char path[PATH_MAXIMUM], token[TOKEN_MAX];
	pthread_mutex_lock(&media_lock);
	snprintf(path, sizeof(path), "%s", media_path);
	snprintf(token, sizeof(token), "%s", media_token);
	pthread_mutex_unlock(&media_lock);
	char expected[96]; snprintf(expected, sizeof(expected), "/sonix/%s/track", token);
	if (!token[0] || strcmp(uri, expected) != 0) {
		const char response[] = "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
		send_all(client, response, sizeof(response) - 1); return;
	}
	int file = open(path, O_RDONLY);
	struct stat st;
	if (file < 0 || fstat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
		if (file >= 0) close(file);
		const char response[] = "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
		send_all(client, response, sizeof(response) - 1); return;
	}
	off_t start = 0, end = st.st_size ? st.st_size - 1 : 0;
	bool partial = false;
	char *range = NULL;
	for (char *line = strstr(request, "\r\n"); line && *line; ) {
		line += 2;
		if (strncasecmp(line, "Range:", 6) == 0) { range = line + 6; break; }
		char *next = strstr(line, "\r\n"); if (!next) break; line = next;
	}
	if (range && st.st_size > 0) {
		while (*range == ' ' || *range == '\t') range++;
		unsigned long long a = 0, b = 0;
		if (sscanf(range, "bytes=%llu-%llu", &a, &b) >= 1 && a < (unsigned long long)st.st_size) {
			start = (off_t)a;
			if (b >= a && b < (unsigned long long)st.st_size) end = (off_t)b;
			partial = true;
		}
	}
	uint64_t length = st.st_size > 0 ? (uint64_t)(end - start + 1) : 0;
	const char *mime = mime_for_path(path);
	char headers[1024];
	int hn = snprintf(headers, sizeof(headers),
		"HTTP/1.1 %s\r\nConnection: close\r\nAccept-Ranges: bytes\r\nContent-Type: %s\r\nContent-Length: %llu\r\n",
		partial ? "206 Partial Content" : "200 OK", mime, (unsigned long long)length);
	if (partial) hn += snprintf(headers + hn, sizeof(headers) - (size_t)hn, "Content-Range: bytes %llu-%llu/%llu\r\n",
		(unsigned long long)start, (unsigned long long)end, (unsigned long long)st.st_size);
	snprintf(headers + hn, sizeof(headers) - (size_t)hn, "\r\n");
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

static void *media_server(void *unused) {
	(void)unused;
	for (;;) {
		struct pollfd pfd = {.fd = media_listener, .events = POLLIN};
		int r = poll(&pfd, 1, 1000);
		if (r <= 0) continue;
		int client = accept(media_listener, NULL, NULL);
		if (client < 0) { if (errno == EINTR) continue; continue; }
		serve_media_client(client);
		close(client);
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

static bool soap(const renderer_t *renderer, const char *action, const char *arguments) {
	char body[8192], extra[512];
	int n = snprintf(body, sizeof(body),
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
		"<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
		action, renderer->service_type, arguments ? arguments : "", action);
	if (n < 0 || (size_t)n >= sizeof(body)) return false;
	snprintf(extra, sizeof(extra), "SOAPAction: \"%s#%s\"\r\n", renderer->service_type, action);
	http_req_t req = {.method = "POST", .extra_headers = extra, .body = body,
		.body_len = strlen(body), .content_type = "text/xml; charset=\"utf-8\"", .allow_empty_body = true};
	char *response = NULL;
	int status = 0;
	bool ok = http_request(renderer->control_url, &req, &response, NULL, 16 * 1024, 8, &status);
	free(response);
	return ok && status >= 200 && status < 300;
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

	pthread_mutex_lock(&media_lock);
	snprintf(media_path, sizeof(media_path), "%s", path);
	unsigned serial = ++token_sequence;
	snprintf(media_token, sizeof(media_token), "%08x%08x", (unsigned)getpid(), serial ^ (unsigned)time(NULL));
	char token[TOKEN_MAX]; snprintf(token, sizeof(token), "%s", media_token);
	pthread_mutex_unlock(&media_lock);

	char uri[URL_MAX];
	if (snprintf(uri, sizeof(uri), "http://%s:%u/sonix/%s/track", ip, media_port, token) >= (int)sizeof(uri)) {
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
	const char *mime = mime_for_path(path);
	snprintf(metadata, sizeof(metadata),
		"<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
		"<item id=\"0\" parentID=\"0\" restricted=\"1\"><dc:title>%s</dc:title><upnp:class>object.item.audioItem.musicTrack</upnp:class>"
		"<res protocolInfo=\"http-get:*:%s:*\">%s</res></item></DIDL-Lite>", safe_title, mime, safe_uri);
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
	if (stopped) {
		pthread_mutex_lock(&media_lock);
		media_token[0] = '\0';
		media_path[0] = '\0';
		pthread_mutex_unlock(&media_lock);
	}
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
