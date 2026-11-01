#include "artsearch.h"
#include "src/system/core/json.h"
#include "src/system/library/jpeginfo.h"
#include "src/system/net/http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef ARTSEARCH_API
#define ARTSEARCH_API "https://itunes.apple.com/search"
#endif
#define JSON_LIMIT (256u * 1024u)
#define TIMEOUT_SECS 15

// Only URLs on Apple's image CDN, not arbitrary URLs returned by an API.
static bool artwork_url(const char *url) {
	const char prefix[] = "https://";
	if (strncmp(url, prefix, sizeof(prefix) - 1))
		return false;
	const char *host = url + sizeof(prefix) - 1, *end = strchr(host, '/');
	if (!end || end == host || memchr(host, ':', (size_t)(end - host)) || memchr(host, '@', (size_t)(end - host)))
		return false;
	const char suffix[] = ".mzstatic.com";
	size_t n = (size_t)(end - host), m = sizeof(suffix) - 1;
	if (n <= m || strncasecmp(end - m, suffix, m))
		return false;
	for (const char *p = host; p < end; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.'))
			return false;
	return true;
}
static bool fetch(const char *url, size_t max, char **data, size_t *len) {
	http_req_t req = {.no_https_downgrade = true};
	size_t got = 0;
	char *body = NULL;
	if (!http_request(url, &req, &body, &got, max + 1, TIMEOUT_SECS, NULL))
		return false;
	if (got > max) {
		free(body);
		return false;
	}
	*data = body;
	*len = got;
	return true;
}
bool artsearch_find(const char *query, artsearch_result_t *results, int *count) {
	if (!query || !*query || strlen(query) > 255 || !results || !count)
		return false;
	*count = 0;
	char encoded[768], url[1200];
	http_url_encode(query, encoded, sizeof(encoded));
	int n = snprintf(url, sizeof(url), ARTSEARCH_API "?term=%s&media=music&entity=album&limit=8", encoded);
	if (n < 0 || (size_t)n >= sizeof(url))
		return false;
	char *body;
	size_t size;
	if (!fetch(url, JSON_LIMIT, &body, &size))
		return false;
	json_doc_t doc;
	bool ok = false;
	if (json_parse_limited(body, &doc, 4096)) {
		int arr = json_get(&doc, json_root(&doc), "results");
		if (arr >= 0 && doc.toks[arr].type == JSON_ARRAY) {
			ok = true;
			for (int i = 0; i < json_len(&doc, arr) && *count < ARTSEARCH_MAX_RESULTS; i++) {
				int item = json_at(&doc, arr, i);
				artsearch_result_t r = {0};
				if (!json_str_exact(&doc, json_get(&doc, item, "artworkUrl100"), r.url, sizeof(r.url)) || !artwork_url(r.url))
					continue;
				json_obj_str(&doc, item, "collectionName", r.album, sizeof(r.album));
				json_obj_str(&doc, item, "artistName", r.artist, sizeof(r.artist));
				if (!r.album[0])
					continue;
				// The catalog's small thumbnail URL also supports a modest,
				// device-friendly 600px image at the same path.
				char *p = strstr(r.url, "100x100bb");
				if (p)
					memcpy(p, "600x600bb", 9);
				results[(*count)++] = r;
			}
		}
		json_free(&doc);
	}
	free(body);
	return ok;
}
bool artsearch_download(const char *url, uint8_t **data, size_t *size, int *width, int *height) {
	if (!url || !artwork_url(url) || !data || !size || !width || !height)
		return false;
	char *body;
	size_t n;
	if (!fetch(url, ARTSEARCH_MAX_IMAGE, &body, &n))
		return false;
	jpeg_info_t info;
	if (n < 4 || (unsigned char)body[n - 2] != 0xff || (unsigned char)body[n - 1] != 0xd9 || !jpeg_info_parse((const unsigned char *)body, n, &info) || info.width <= 0 || info.height <= 0 || info.width > 4096 || info.height > 4096) {
		free(body);
		return false;
	}
	*data = (uint8_t *)body;
	*size = n;
	*width = info.width;
	*height = info.height;
	return true;
}
