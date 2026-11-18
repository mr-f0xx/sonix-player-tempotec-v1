#define _GNU_SOURCE 1
#include "networkshare.h"

#include "src/system/ebook/epub_xml.h"
#include "src/system/playback/playlist.h"

#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define LIST_BYTES (1024 * 1024)
#define URL_BYTES (sizeof(((share_server_t *)0)->origin) + 3 * (SHARE_FOLDER_MAX + SHARE_NAME_MAX))

// Stable libcurl ABI, as shipped in the V1 1.3 rootfs (libcurl.so.4, 7.68.0).
// The MIPS toolchain does not ship curl headers or libraries. Like tls.c, load
// only the required public symbols at runtime, also allowing host builds.
// Numbers below are from curl/curl.h and curl/multi.h in 7.68.0.
typedef void curl_handle_t;
typedef void curl_multi_t;
struct curl_header;
typedef long long curl_offset_t;
typedef struct {
	int message;
	curl_handle_t *handle;
	union { void *pointer; int result; } data;
} curl_message_t;

static struct {
	int (*global_init)(long);
	curl_handle_t *(*easy_init)(void);
	int (*easy_setopt)(curl_handle_t *, int, ...);
	int (*easy_getinfo)(curl_handle_t *, int, ...);
	void (*easy_cleanup)(curl_handle_t *);
	struct curl_header *(*slist_append)(struct curl_header *, const char *);
	void (*slist_free_all)(struct curl_header *);
	curl_multi_t *(*multi_init)(void);
	int (*multi_add_handle)(curl_multi_t *, curl_handle_t *);
	int (*multi_perform)(curl_multi_t *, int *);
	int (*multi_poll)(curl_multi_t *, void *, unsigned, int, int *);
	curl_message_t *(*multi_info_read)(curl_multi_t *, int *);
	int (*multi_remove_handle)(curl_multi_t *, curl_handle_t *);
	int (*multi_cleanup)(curl_multi_t *);
} curl;

static pthread_once_t load_once = PTHREAD_ONCE_INIT;
static bool curl_loaded;

static void load_curl(void) {
#ifndef __EMSCRIPTEN__
	void *lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
	if (!lib) return;
#define LOAD(name) do { if (!(curl.name = dlsym(lib, "curl_" #name))) goto unavailable; } while (0)
	LOAD(global_init);
	LOAD(easy_init);
	LOAD(easy_setopt);
	LOAD(easy_getinfo);
	LOAD(easy_cleanup);
	LOAD(slist_append);
	LOAD(slist_free_all);
	LOAD(multi_init);
	LOAD(multi_add_handle);
	LOAD(multi_perform);
	LOAD(multi_poll);
	LOAD(multi_info_read);
	LOAD(multi_remove_handle);
	LOAD(multi_cleanup);
#undef LOAD
	if (curl.global_init(3L) == 0) { // CURL_GLOBAL_DEFAULT
		curl_loaded = true;
		return; // Keep the library and global state for the lifetime of the player.
	}
unavailable:
	dlclose(lib);
#endif
}

enum {
	OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_PROXY = 10004, OPT_WRITEFUNCTION = 20011,
	OPT_TIMEOUT = 13, OPT_POSTFIELDS = 10015, OPT_USERAGENT = 10018, OPT_LOW_SPEED_LIMIT = 19,
	OPT_LOW_SPEED_TIME = 20, OPT_HTTPHEADER = 10023, OPT_SSLVERSION = 32, OPT_CUSTOMREQUEST = 10036,
	OPT_NOPROGRESS = 43, OPT_FAILONERROR = 45, OPT_XFERINFODATA = 10057, OPT_SSL_VERIFYPEER = 64, OPT_CAINFO = 10065,
	OPT_CONNECTTIMEOUT = 78, OPT_SSL_VERIFYHOST = 81, OPT_NOSIGNAL = 99, OPT_HTTPAUTH = 107,
	OPT_FTP_RESPONSE_TIMEOUT = 112, OPT_MAXFILESIZE_LARGE = 30117, OPT_FTP_SKIP_PASV_IP = 137,
	OPT_USERNAME = 10173, OPT_PASSWORD = 10174, OPT_PROTOCOLS = 181, OPT_XFERINFOFUNCTION = 20219,
};
enum {
	INFO_RESPONSE_CODE = 0x200002, INFO_CONTENT_TYPE = 0x100012,
	PROTO_HTTP = 1, PROTO_HTTPS = 2, PROTO_FTP = 4, PROTO_FTPS = 8,
};

static bool digits(char c) { return c >= '0' && c <= '9'; }
static bool ascii_letter(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static bool clean_text(const char *text, size_t cap) {
	if (!text || strnlen(text, cap) >= cap) return false;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (*p < 32 || *p == 127) return false;
	}
	return true;
}

static bool name_ok(const char *name) {
	return clean_text(name, SHARE_NAME_MAX) && name[0] && strcmp(name, ".") && strcmp(name, "..") &&
		   !strchr(name, '/') && !strchr(name, '\\');
}

static int hex_value(char c) {
	if (digits(c)) return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Decode each segment separately. Encoded slashes, controls and dot-segments
// cannot change the share root or turn a server's name into a local path.
static bool decode_path(const char *path, char *out, size_t cap) {
	if (path[0] != '/' || cap < 2 || strpbrk(path, "?#")) return false;
	size_t used = 0;
	out[used++] = '/';
	for (const char *p = path + 1; *p;) {
		const char *end = p + strcspn(p, "/");
		char part[SHARE_NAME_MAX];
		size_t n = 0;
		while (p < end) {
			unsigned char c = (unsigned char)*p++;
			if (c == '%') {
				if (end - p < 2 || hex_value(p[0]) < 0 || hex_value(p[1]) < 0) return false;
				c = (unsigned char)(hex_value(p[0]) * 16 + hex_value(p[1]));
				p += 2;
			}
			if (!c || n == sizeof(part) - 1) return false;
			part[n++] = (char)c;
		}
		part[n] = '\0';
		if (!name_ok(part) || used + n + (*end == '/') >= cap) return false;
		memcpy(out + used, part, n);
		used += n;
		if (*end == '/') out[used++] = *p++;
	}
	out[used] = '\0';
	return true;
}

static long protocol(const char *url) {
	if (!strncasecmp(url, "http://", 7)) return PROTO_HTTP;
	if (!strncasecmp(url, "https://", 8)) return PROTO_HTTPS;
	if (!strncasecmp(url, "ftp://", 6)) return PROTO_FTP;
	if (!strncasecmp(url, "ftps://", 7)) return PROTO_FTPS;
	return 0;
}

// Accept DNS names/IPv4 or a bracketed IPv6 literal and an optional port.
// Canonicalise the default port so DAV hrefs need not spell it as we typed it.
static const char *split_url(const char *url, long proto, char *origin, size_t size) {
	if (!proto || strpbrk(url, "?#")) return NULL;
	const char *host = strstr(url, "://") + 3;
	const char *end = host + strcspn(host, "/");
	const char *host_end = host;
	if (*host == '[') {
		host_end = memchr(host + 1, ']', (size_t)(end - host - 1));
		if (!host_end || host_end == host + 1) return NULL;
		for (const char *p = host + 1; p < host_end; p++) {
			if (hex_value(*p) < 0 && *p != ':' && *p != '.') return NULL;
		}
		host_end++;
	} else {
		while (host_end < end && (ascii_letter(*host_end) || digits(*host_end) || *host_end == '.' || *host_end == '-')) {
			host_end++;
		}
		if (host_end == host) return NULL;
	}
	if (host_end < end && *host_end != ':') return NULL;
	unsigned port = 0;
	if (host_end < end) {
		if (host_end + 1 == end) return NULL;
		for (const char *p = host_end + 1; p < end; p++) {
			if (!digits(*p)) return NULL;
			port = port * 10 + (unsigned)(*p - '0');
			if (port > 65535) return NULL;
		}
		if (!port) return NULL;
	}
	unsigned standard = proto == PROTO_HTTP ? 80 : proto == PROTO_HTTPS ? 443 : proto == PROTO_FTP ? 21 : 990;
	size_t n = (size_t)(host_end - url);
	if (n >= size) return NULL;
	for (size_t i = 0; i < n; i++) origin[i] = (char)tolower((unsigned char)url[i]);
	origin[n] = '\0';
	if (port && port != standard && snprintf(origin + n, size - n, ":%u", port) >= (int)(size - n)) return NULL;
	return end;
}

bool networkshare_server_init(share_server_t *out, share_kind_t kind, const char *address,
							  const char *username, const char *password, const char *ca_file) {
	if (!out) return false;
	*out = (share_server_t){0};
	if (!clean_text(address, SHARE_ADDRESS_MAX) || !clean_text(username, SHARE_LOGIN_MAX) ||
		!clean_text(password, SHARE_LOGIN_MAX) || (ca_file && !clean_text(ca_file, sizeof(out->ca_file))) ||
		(!username[0] && password[0])) return false;
	long proto = protocol(address);
	if (kind == SHARE_WEBDAV ? !(proto & (PROTO_HTTP | PROTO_HTTPS)) :
		kind != SHARE_FTP || !(proto & (PROTO_FTP | PROTO_FTPS))) return false;
	const char *path = split_url(address, proto, out->origin, sizeof(out->origin));
	if (!path || !decode_path(*path ? path : "/", out->root, sizeof(out->root))) return false;
	size_t len = strlen(out->root);
	if (out->root[len - 1] != '/') {
		if (len + 1 >= sizeof(out->root)) return false;
		strcat(out->root, "/");
	}
	out->kind = kind;
	strcpy(out->username, username);
	strcpy(out->password, password);
	if (ca_file) strcpy(out->ca_file, ca_file);
	return true;
}

bool networkshare_valid_folder(const share_server_t *server, const char *folder) {
	if (!server || !server->root[0] || !folder || folder[0] != '/') return false;
	size_t len = strnlen(folder, SHARE_FOLDER_MAX);
	if (len >= SHARE_FOLDER_MAX || folder[len - 1] != '/' ||
		strncmp(folder, server->root, strlen(server->root))) return false;
	for (const char *p = folder + 1; *p;) {
		size_t n = strcspn(p, "/");
		char part[SHARE_NAME_MAX];
		if (n >= sizeof(part)) return false;
		memcpy(part, p, n);
		part[n] = '\0';
		if (!name_ok(part)) return false;
		p += n + 1;
	}
	return true;
}

bool networkshare_child_folder(const share_server_t *server, const char *folder, const char *name,
								  char *out, size_t size) {
	return out && size && networkshare_valid_folder(server, folder) && name_ok(name) && strlen(folder) + strlen(name) + 1 < SHARE_FOLDER_MAX &&
		   snprintf(out, size, "%s%s/", folder, name) < (int)size;
}

static void make_url(char out[URL_BYTES], const share_server_t *server, const char *folder, const char *name) {
	static const char hex[] = "0123456789ABCDEF";
	strcpy(out, server->origin);
	char *dest = out + strlen(out);
	const char *parts[] = {folder, name ? name : ""};
	for (int i = 0; i < 2; i++) {
		for (const unsigned char *p = (const unsigned char *)parts[i]; *p; p++) {
			unsigned char c = *p;
			if (ascii_letter(c) || digits((char)c) || strchr("-._~/", c)) *dest++ = (char)c;
			else { *dest++ = '%'; *dest++ = hex[c >> 4]; *dest++ = hex[c & 15]; }
		}
	}
	*dest = '\0';
}

typedef struct {
	share_cancel_fn cancel;
	share_write_fn write;
	share_size_fn size;
	void *user;
	bool listing;
	share_status_t error;
	long response;
	uint64_t received;
} request_t;

static int report_size(void *user, curl_offset_t total, curl_offset_t now, curl_offset_t ultotal, curl_offset_t ulnow) {
	(void)now; (void)ultotal; (void)ulnow;
	request_t *req = user;
	if (req->size && total > 0) req->size(req->user, (uint64_t)total);
	return 0;
}

static size_t write_body(char *data, size_t size, size_t count, void *user) {
	request_t *req = user;
	size_t n = size * count; // libcurl passes size=1 and a bounded count
	if (req->cancel && req->cancel(req->user)) {
		req->error = SHARE_CANCELLED;
		return 0;
	}
	if (!req->listing && req->received + n > UINT32_MAX) {
		req->error = SHARE_FILE_TOO_LARGE;
		return 0;
	}
	req->error = req->write(req->user, data, n);
	if (req->error != SHARE_OK) return 0;
	req->received += n;
	return n;
}

static share_status_t request(const share_server_t *server, const char *url, const char *method, request_t *req) {
	pthread_once(&load_once, load_curl);
	if (!curl_loaded) return SHARE_UNAVAILABLE;
	if (req->cancel && req->cancel(req->user)) return SHARE_CANCELLED;
	curl_handle_t *easy = curl.easy_init();
	if (!easy) return SHARE_NO_MEMORY;
	curl_multi_t *multi = NULL;
	struct curl_header *headers = NULL;
	bool attached = false;
	share_status_t status = SHARE_UNAVAILABLE;
	long proto = protocol(url);
#define SET(opt, value) do { if (curl.easy_setopt(easy, OPT_##opt, value) != 0) goto done; } while (0)
	SET(URL, url);
	SET(PROTOCOLS, proto);
	SET(PROXY, ""); // ignore process-wide proxy environment; shares are direct
	SET(NOSIGNAL, 1L);
	SET(CONNECTTIMEOUT, 10L);
	SET(LOW_SPEED_LIMIT, 1L);
	SET(LOW_SPEED_TIME, 15L);
	SET(USERAGENT, "Sonix Network Folders");
	SET(WRITEDATA, req);
	SET(WRITEFUNCTION, write_body);
	SET(FAILONERROR, 1L);
	if (req->size) {
		SET(XFERINFODATA, req);
		SET(XFERINFOFUNCTION, report_size);
		SET(NOPROGRESS, 0L);
	}
	if (proto == PROTO_HTTPS || proto == PROTO_FTPS) {
		SET(SSL_VERIFYPEER, 1L);
		SET(SSL_VERIFYHOST, 2L);
		SET(SSLVERSION, 6L); // TLS 1.2 or newer
		if (server->ca_file[0]) SET(CAINFO, server->ca_file);
		else if (access("/etc/ssl/cert.pem", R_OK) == 0) SET(CAINFO, "/etc/ssl/cert.pem");
	}
	if (server->username[0]) {
		SET(USERNAME, server->username);
		SET(PASSWORD, server->password);
		if (server->kind == SHARE_WEBDAV) SET(HTTPAUTH, 3L); // CURLAUTH_BASIC | CURLAUTH_DIGEST
	}
	if (server->kind == SHARE_FTP) {
		SET(FTP_SKIP_PASV_IP, 1L); // data goes to the control connection's peer
		SET(FTP_RESPONSE_TIMEOUT, 15L);
	}
	if (req->listing) SET(TIMEOUT, 30L);
	else SET(MAXFILESIZE_LARGE, (curl_offset_t)UINT32_MAX);
	if (method) SET(CUSTOMREQUEST, method);
	if (req->listing && server->kind == SHARE_WEBDAV) {
		static const char xml[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
			"<D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/></D:prop></D:propfind>";
		headers = curl.slist_append(NULL, "Depth: 1");
		if (!headers) { status = SHARE_NO_MEMORY; goto done; }
		struct curl_header *list = curl.slist_append(headers, "Content-Type: application/xml; charset=utf-8");
		if (!list) { status = SHARE_NO_MEMORY; goto done; }
		headers = list;
		SET(HTTPHEADER, headers);
		SET(POSTFIELDS, xml);
	}
#undef SET
	status = SHARE_NETWORK_ERROR;
	multi = curl.multi_init();
	if (!multi) { status = SHARE_NO_MEMORY; goto done; }
	if (curl.multi_add_handle(multi, easy) != 0) goto done;
	attached = true;
	int running;
	do {
		if (req->cancel && req->cancel(req->user)) { status = SHARE_CANCELLED; goto done; }
		if (curl.multi_perform(multi, &running) != 0) goto done;
		if (running && curl.multi_poll(multi, NULL, 0, 200, NULL) != 0) goto done;
	} while (running);

	curl.easy_getinfo(easy, INFO_RESPONSE_CODE, &req->response);
	int remaining = 0;
	curl_message_t *msg = curl.multi_info_read(multi, &remaining);
	if (req->error != SHARE_OK) status = req->error;
	else if (req->cancel && req->cancel(req->user)) status = SHARE_CANCELLED;
	else if (!msg || msg->message != 1) status = SHARE_NETWORK_ERROR; // CURLMSG_DONE
	else if (msg->data.result == 67 || req->response == 401 || req->response == 403 || req->response == 530) status = SHARE_AUTH_ERROR;
	else if (msg->data.result == 28) status = SHARE_TIMEOUT;
	else if (msg->data.result == 60 || msg->data.result == 77 || msg->data.result == 35) status = SHARE_CERT_ERROR;
	else if (msg->data.result == 1 || msg->data.result == 4) status = SHARE_UNAVAILABLE;
	else if (msg->data.result == 63) status = SHARE_FILE_TOO_LARGE;
	else if (req->response >= 400 || msg->data.result == 19) status = SHARE_SERVER_ERROR;
	else if (msg->data.result != 0) status = SHARE_NETWORK_ERROR;
	else if (server->kind == SHARE_WEBDAV && req->response != (req->listing ? 207 : 200)) status = SHARE_SERVER_ERROR;
	else if (!req->listing && !req->received) status = SHARE_SERVER_ERROR;
	else {
		char *type = NULL;
		if (!req->listing && server->kind == SHARE_WEBDAV) curl.easy_getinfo(easy, INFO_CONTENT_TYPE, &type);
		// Don't save a login/HTML error page as if it were a music file.
		status = type && (!strncasecmp(type, "text/", 5) || !strncasecmp(type, "application/json", 16) ||
						  !strncasecmp(type, "application/xml", 15)) ? SHARE_SERVER_ERROR : SHARE_OK;
	}
done:
	if (attached) curl.multi_remove_handle(multi, easy);
	if (multi) curl.multi_cleanup(multi);
	curl.easy_cleanup(easy);
	curl.slist_free_all(headers);
	return status;
}

bool networkshare_valid_track(const char *name) {
	return name_ok(name) && name[0] != '.' && playlist_is_playable_file(name);
}

static void add_entry(share_listing_t *out, const char *name, bool directory) {
	if (directory ? !name_ok(name) || name[0] == '.' : !networkshare_valid_track(name)) return;
	for (size_t i = 0; i < out->count; i++) {
		if (!strcmp(name, out->entries[i].name)) return;
	}
	if (out->count == SHARE_LIST_MAX) {
		out->truncated = true;
		return;
	}
	share_entry_t *entry = &out->entries[out->count++];
	strcpy(entry->name, name);
	entry->directory = directory;
}

static int entry_cmp(const void *a, const void *b) {
	const share_entry_t *left = a, *right = b;
	if (left->directory != right->directory) return left->directory ? -1 : 1;
	int folded = strcasecmp(left->name, right->name);
	return folded ? folded : strcmp(left->name, right->name);
}

static void dav_entry(const share_server_t *server, const char *folder, char *href, bool directory, share_listing_t *out) {
	href += strspn(href, " \t\r\n");
	size_t length = strlen(href);
	while (length && strchr(" \t\r\n", href[length - 1])) href[--length] = '\0';
	if (href[0] != '/') {
		char origin[sizeof(server->origin)];
		const char *path = split_url(href, protocol(href), origin, sizeof(origin));
		if (!path || strcmp(origin, server->origin)) return;
		href = (char *)path;
	}
	char path[SHARE_FOLDER_MAX + SHARE_NAME_MAX];
	if (!decode_path(href, path, sizeof(path))) return;
	size_t n = strlen(folder);
	if (strncmp(path, folder, n) || !path[n]) return; // self or outside the requested folder
	char *name = path + n;
	char *slash = strchr(name, '/');
	if (slash) {
		if (!directory || slash[1]) return; // only direct children, never descendants
		*slash = '\0';
	}
	add_entry(out, name, directory);
}

// Use the bounded XML tokenizer already used by Podcasts. Only DAV response
// hrefs and successful resourcetype properties matter. No external entities
// or remote schemas are loaded by this tokenizer.
static bool parse_dav(const share_server_t *server, const char *folder, const char *text, share_listing_t *out) {
	enum { OTHER, MULTI, RESPONSE, HREF, PROPSTAT, PROP, TYPE, COLLECTION, STATUS } tags[32] = {OTHER};
	const char *const names[] = {NULL, "multistatus", "response", "href", "propstat", "prop", "resourcetype", "collection", "status"};
	int depth = 0;
	bool root_seen = false, root_closed = false;
	bool have_type = false, directory = false, prop_type = false, prop_dir = false, prop_ok = false;
	bool response_ok = false, overflow = false;
	char href[URL_BYTES] = "", status[96] = "";
	epub_xml_t xml;
	epub_xml_init(&xml, text);
	while (epub_xml_next(&xml)) {
		if (xml.event == EPUB_XML_TEXT) {
			char *target = tags[depth] == HREF ? href : tags[depth] == STATUS ? status : NULL;
			if (target) {
				size_t cap = target == href ? sizeof(href) : sizeof(status);
				size_t n = strlen(target);
				if (xml.text_len >= cap - n) overflow = true;
				else epub_xml_unescape(target + n, cap - n, xml.text, xml.text_len);
			}
			continue;
		}
		if (xml.event == EPUB_XML_OPEN) {
			int parent = tags[depth];
			int tag = OTHER;
			if (depth == 0) {
				if (root_seen || !epub_xml_is(&xml, names[MULTI])) return false;
				root_seen = true;
				root_closed = xml.self_closing;
				tag = MULTI;
			} else if (parent == MULTI && epub_xml_is(&xml, names[RESPONSE])) {
				tag = RESPONSE;
				href[0] = '\0';
				response_ok = true;
				have_type = directory = overflow = false;
			} else if (parent == RESPONSE && epub_xml_is(&xml, names[HREF])) tag = HREF;
			else if (parent == RESPONSE && epub_xml_is(&xml, names[PROPSTAT])) {
				tag = PROPSTAT;
				prop_type = prop_dir = prop_ok = false;
			} else if (parent == PROPSTAT && epub_xml_is(&xml, names[PROP])) tag = PROP;
			else if (parent == PROP && epub_xml_is(&xml, names[TYPE])) { tag = TYPE; prop_type = true; }
			else if (parent == TYPE && epub_xml_is(&xml, names[COLLECTION])) { tag = COLLECTION; prop_dir = true; }
			else if ((parent == PROPSTAT || parent == RESPONSE) && epub_xml_is(&xml, names[STATUS])) {
				tag = STATUS;
				status[0] = '\0';
			}
			if (!xml.self_closing) {
				if (depth + 1 == (int)(sizeof(tags) / sizeof(tags[0]))) return false;
				tags[++depth] = tag;
			}
			continue;
		}
		if (depth == 0 || (tags[depth] != OTHER && !epub_xml_is(&xml, names[tags[depth]]))) return false;
		switch (tags[depth--]) {
		case STATUS: {
			const char *value = status + strspn(status, " \t\r\n");
			const char *code = strchr(value, ' ');
			if (code) code += strspn(code, " ");
			bool ok = !strncmp(value, "HTTP/", 5) && code && code[0] == '2' && digits(code[1]) &&
					  digits(code[2]) && (!code[3] || isspace((unsigned char)code[3]));
			if (tags[depth] == PROPSTAT) prop_ok = ok;
			else response_ok = ok;
			break;
		}
		case PROPSTAT: if (prop_ok && prop_type) { have_type = true; directory = prop_dir; } break;
		case RESPONSE: if (!overflow && response_ok && have_type) dav_entry(server, folder, href, directory, out); break;
		case MULTI: root_closed = true; break;
		default: break;
		}
	}
	return root_closed && depth == 0;
}

static void parse_ftp(char *body, bool mlsd, share_listing_t *out) {
	char *next = NULL;
	for (char *line = strtok_r(body, "\r\n", &next); line; line = strtok_r(NULL, "\r\n", &next)) {
		if (mlsd) {
			char *space = strchr(line, ' ');
			if (!space) continue;
			*space = '\0';
			char *save = NULL;
			for (char *fact = strtok_r(line, ";", &save); fact; fact = strtok_r(NULL, ";", &save)) {
				if (!strcasecmp(fact, "type=dir")) add_entry(out, space + 1, true);
				if (!strcasecmp(fact, "type=file")) add_entry(out, space + 1, false);
			}
		} else {
			// Common legacy LIST dialects: Unix ls -l and Windows/DOS. Ignore
			// links and special files, rather than trying to follow their targets.
			bool unix_style = (*line == 'd' || *line == '-') && strlen(line) >= 11 && isspace((unsigned char)line[10]);
			if (!unix_style && !(digits(line[0]) && digits(line[1]) && (line[2] == '-' || line[2] == '/'))) continue;
			char *p = line;
			bool directory = line[0] == 'd';
			for (int i = 0; i < (unix_style ? 8 : 3) && *p; i++) {
				if (!unix_style && i == 2) {
					directory = strncasecmp(p, "<DIR>", 5) == 0 && isspace((unsigned char)p[5]);
					if (!directory && !digits(*p)) { p += strlen(p); break; }
				}
				p += strcspn(p, " \t");
				p += strspn(p, " \t");
			}
			add_entry(out, p, directory);
		}
	}
}

typedef struct {
	char *data;
	size_t len, allocated;
	share_cancel_fn cancel;
	void *user;
} list_body_t;

static bool list_cancelled(void *arg) {
	list_body_t *body = arg;
	return body->cancel && body->cancel(body->user);
}

static share_status_t collect_listing(void *arg, const void *data, size_t n) {
	list_body_t *body = arg;
	if (n > LIST_BYTES - body->len) return SHARE_LIST_TOO_LARGE;
	if (memchr(data, '\0', n)) return SHARE_BAD_LIST;
	if (body->len + n + 1 > body->allocated) {
		size_t want = body->allocated ? body->allocated * 2 : 8192;
		if (want < body->len + n + 1) want = body->len + n + 1;
		if (want > LIST_BYTES + 1) want = LIST_BYTES + 1;
		char *grown = realloc(body->data, want);
		if (!grown) return SHARE_NO_MEMORY;
		body->data = grown;
		body->allocated = want;
	}
	memcpy(body->data + body->len, data, n);
	body->len += n;
	body->data[body->len] = '\0';
	return SHARE_OK;
}

share_status_t networkshare_list(const share_server_t *server, const char *folder, share_listing_t *out,
								 share_cancel_fn cancel, void *user) {
	if (!networkshare_valid_folder(server, folder) || !out) return SHARE_BAD_ADDRESS;
	out->count = 0;
	out->truncated = false;
	char url[URL_BYTES];
	make_url(url, server, folder, NULL);
	list_body_t body = {.cancel = cancel, .user = user};
	request_t req = {.cancel = list_cancelled, .write = collect_listing, .user = &body, .listing = true};
	bool mlsd = server->kind == SHARE_FTP;
	share_status_t status = request(server, url, mlsd ? "MLSD" : "PROPFIND", &req);
	if (mlsd && status == SHARE_SERVER_ERROR && ((req.response >= 500 && req.response <= 504) || req.response == 550)) {
		body.len = 0;
		if (body.data) body.data[0] = '\0';
		req = (request_t){.cancel = list_cancelled, .write = collect_listing, .user = &body, .listing = true};
		status = request(server, url, NULL, &req);
		mlsd = false;
	}
	if (status == SHARE_OK) {
		if (server->kind == SHARE_WEBDAV) {
			if (!body.data || !parse_dav(server, folder, body.data, out)) status = SHARE_BAD_LIST;
		} else if (body.data) parse_ftp(body.data, mlsd, out);
		if (status == SHARE_OK) qsort(out->entries, out->count, sizeof(out->entries[0]), entry_cmp);
	}
	free(body.data);
	return status;
}

share_status_t networkshare_fetch(const share_server_t *server, const char *folder, const char *name,
								  share_cancel_fn cancel, share_write_fn write, share_size_fn content_size, void *user) {
	if (!networkshare_valid_folder(server, folder) || !networkshare_valid_track(name) || !write) return SHARE_BAD_ADDRESS;
	char url[URL_BYTES];
	make_url(url, server, folder, name);
	request_t req = {.cancel = cancel, .write = write, .size = content_size, .user = user};
	return request(server, url, NULL, &req);
}
