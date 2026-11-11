/* The playlists a Tidal account keeps as favourites, without touching the
 * network.
 *
 * They are somebody else's playlists -- Tidal's editors', an artist's, another
 * user's -- and GET /users/{id}/playlists, which is all the player used to ask,
 * answers only with the ones the account made. A playlist favourited in
 * Tidal's own app was therefore in the account and nowhere on the player.
 *
 * What is pinned here is the client's half of that:
 *
 *   - the request: the favourites collection, paged, newest first, with the
 *     session's token on it -- and not the endpoint of the account's own;
 *   - the reply: entries wrapped as {"created":..,"item":{..}}, read into
 *     tidal_playlist_t, with whoever made an editorial, an artist's and a
 *     user's playlist, and the square picture or the wide one behind it;
 *   - a refusal: Tidal's own words in tidal_last_error(), and the caller's
 *     rows left as they were, which is what keeps the list on screen usable;
 *   - the removal: a DELETE on the favourites collection, never on the
 *     playlist, which is not the account's to delete.
 *
 * The session is a real one: the test signs in through the device-code flow
 * against canned replies, the way a player does, into a directory of its own.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/streaming/tidal.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamkeys.h"

/* ---- the configuration: defaults everywhere, in a directory of its own ---- */

static char config_file[256];

const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return fallback;
}

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}

void config_set(const char *section, const char *key, const char *value) {
	(void)section;
	(void)key;
	(void)value;
}

void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
}

bool config_save(void) { return true; }
const char *config_path(void) { return config_file; }

const char *streamkeys_tidal_client_id(void) { return "test_client_id16"; }
const char *streamkeys_tidal_client_secret(void) { return "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789+/AbCdE="; }

/* The tag itself: enough to tell which message was meant. */
const char *tr(const char *key) { return key; }

char *http_url_encode(const char *in, char *out, size_t out_size) {
	size_t w = 0;
	for (size_t i = 0; in[i] && w + 4 < out_size; i++) {
		unsigned char c = (unsigned char)in[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
			c == '.' || c == '~') {
			out[w++] = (char)c;
		} else {
			w += (size_t)snprintf(out + w, out_size - w, "%%%02X", c);
		}
	}
	if (out_size) {
		out[w] = '\0';
	}
	return out;
}

const char *http_last_error(void) { return NULL; }

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out, size_t *out_len,
				 size_t limit, int timeout_secs, int *status_out) {
	(void)url;
	(void)extra_headers;
	(void)want_error_body;
	(void)out;
	(void)out_len;
	(void)limit;
	(void)timeout_secs;
	(void)status_out;
	assert(!"every request of the Tidal client goes through http_request()");
	return false;
}

/* ---- the network: canned replies, and every request written down ---------- */

typedef struct {
	int status;
	const char *body;
} canned_reply_t;

typedef struct {
	char method[8];
	char url[512];
	char body[512];
	char headers[512];
} seen_request_t;

#define MAX_REQUESTS 8

static canned_reply_t replies[MAX_REQUESTS];
static int reply_count;
static int reply_idx;
static seen_request_t seen[MAX_REQUESTS];
static int seen_count;

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len, size_t limit, int timeout_secs,
				  int *status_out) {
	(void)limit;
	(void)timeout_secs;
	assert(seen_count < MAX_REQUESTS);
	seen_request_t *s = &seen[seen_count++];
	snprintf(s->method, sizeof(s->method), "%s", req && req->method ? req->method : "GET");
	snprintf(s->url, sizeof(s->url), "%s", url);
	snprintf(s->body, sizeof(s->body), "%s", req && req->body ? req->body : "");
	snprintf(s->headers, sizeof(s->headers), "%s", req && req->extra_headers ? req->extra_headers : "");

	if (reply_idx >= reply_count) {
		fprintf(stderr, "a request nobody expected: %s %s\n", s->method, s->url);
		assert(!"more requests than canned replies");
	}
	canned_reply_t r = replies[reply_idx++];
	if (status_out) {
		*status_out = r.status;
	}
	size_t n = strlen(r.body);
	*out = malloc(n + 1);
	assert(*out);
	memcpy(*out, r.body, n + 1);
	if (out_len) {
		*out_len = n;
	}
	return true;
}

static void reset_http(void) {
	reply_count = 0;
	reply_idx = 0;
	seen_count = 0;
}

static void push_reply(int status, const char *body) {
	assert(reply_count < MAX_REQUESTS);
	replies[reply_count++] = (canned_reply_t){status, body};
}

/* ---- the account ----------------------------------------------------------- */

#define USER_ID "4242"
#define API "https://api.tidal.com/v1"

#define EDITORIAL_ID "36ea71a8-445e-41a4-82ab-6628c581535d"
#define ARTIST_ID "7eafb342-141a-4092-91eb-da0012da3a19"
#define FRIEND_ID "944dd087-f65c-4954-a9a3-042a574e86e3"

/* Three favourites, as the favourites collection sends them: each entry is the
 * date it was favourited and the playlist under "item". One of each kind there
 * is. The creator is the part that differs: an editorial playlist has creator
 * number 0 and no name, an artist's carries the artist, another user's only
 * that user's number -- and the last has no square picture, only the wide one. */
static const char FAVOURITES_PAGE[] =
	"{\"limit\":50,\"offset\":0,\"totalNumberOfItems\":3,\"items\":["
	"{\"created\":\"2026-10-30T08:15:02.000+0000\",\"item\":{"
	"\"uuid\":\"" EDITORIAL_ID "\",\"title\":\"Chill Hits\",\"numberOfTracks\":50,\"numberOfVideos\":0,"
	"\"creator\":{\"id\":0},\"description\":\"Kick back.\",\"duration\":10800,"
	"\"lastUpdated\":\"2026-11-01T12:00:00.000+0000\",\"created\":\"2019-03-07T15:10:45.000+0000\","
	"\"type\":\"EDITORIAL\",\"publicPlaylist\":true,\"url\":\"http://www.tidal.com/playlist/" EDITORIAL_ID "\","
	"\"image\":\"7eb5d4f1-6b0a-4d6c-9c53-5a1b7d5f2d0c\",\"popularity\":0,"
	"\"squareImage\":\"0d7a2bd5-6a8e-45c0-9d0e-1f0d6f4d2a11\",\"promotedArtists\":[]}},"
	"{\"created\":\"2025-01-12T19:40:11.000+0000\",\"item\":{"
	"\"uuid\":\"" ARTIST_ID "\",\"title\":\"JAY-Z's Year End Picks 2019\",\"numberOfTracks\":40,"
	"\"numberOfVideos\":0,\"creator\":{\"id\":7804,\"name\":\"JAY Z\",\"artistTypes\":null,\"type\":null,"
	"\"picture\":null},\"description\":\"\",\"duration\":8008,\"type\":\"ARTIST\",\"publicPlaylist\":true,"
	"\"image\":\"11111111-2222-3333-4444-555555555555\",\"popularity\":0,"
	"\"squareImage\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\",\"promotedArtists\":[]}},"
	"{\"created\":\"2024-06-02T07:00:00.000+0000\",\"item\":{"
	"\"uuid\":\"" FRIEND_ID "\",\"title\":\"lofi\",\"numberOfTracks\":1,\"numberOfVideos\":0,"
	"\"creator\":{\"id\":169584258},\"description\":\"\",\"duration\":180,\"type\":\"USER\","
	"\"publicPlaylist\":true,\"image\":\"99999999-8888-7777-6666-555555555555\",\"popularity\":0,"
	"\"squareImage\":null,\"promotedArtists\":null}}"
	"]}";

static void sign_in(void) {
	tidal_init();
	assert(tidal_configured());
	assert(!tidal_logged_in());

	reset_http();
	push_reply(200, "{\"deviceCode\":\"dev-123\",\"userCode\":\"ABCDE\",\"verificationUri\":\"link.tidal.com\","
					"\"verificationUriComplete\":\"link.tidal.com/ABCDE\",\"expiresIn\":300,\"interval\":2}");
	push_reply(200, "{\"access_token\":\"ACCESS-TOKEN\",\"refresh_token\":\"REFRESH-TOKEN\",\"token_type\":\"Bearer\","
					"\"expires_in\":604800,\"user\":{\"userId\":" USER_ID ",\"countryCode\":\"FR\","
					"\"username\":\"listener@example.org\"}}");

	tidal_login_t code;
	assert(tidal_login_begin(&code));
	assert(tidal_login_poll(&code) == TIDAL_LOGIN_OK);
	assert(seen_count == 2);
	assert(tidal_logged_in());
}

static bool has_param(const char *url, const char *param) {
	const char *query = strchr(url, '?');
	if (!query) {
		return false;
	}
	size_t n = strlen(param);
	for (const char *p = query + 1; *p;) {
		const char *end = strchr(p, '&');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (len == n && strncmp(p, param, n) == 0) {
			return true;
		}
		if (!end) {
			break;
		}
		p = end + 1;
	}
	return false;
}

/* ---- the tests -------------------------------------------------------------- */

/* Before anyone has signed in there is no account to ask about: an error with
 * its reason, and nothing sent. */
static void test_signed_out_asks_nothing(void) {
	tidal_init();
	reset_http();

	tidal_playlist_t got[2];
	assert(tidal_favorite_playlists(0, got, 2) == -1);
	assert(strcmp(tidal_last_error(), "tidal_not_signed_in") == 0);
	assert(!tidal_favorite_remove_playlist(EDITORIAL_ID));
	assert(strcmp(tidal_last_error(), "tidal_not_signed_in") == 0);
	assert(seen_count == 0);
}

static void test_the_list_is_read_from_the_favourites(void) {
	reset_http();
	push_reply(200, FAVOURITES_PAGE);

	tidal_playlist_t got[50];
	memset(got, 0x55, sizeof(got));
	assert(tidal_favorite_playlists(0, got, 50) == 3);

	/* One request, a GET, to the favourites collection of this account: the
	 * same family the favourite tracks, albums and artists are read from, and
	 * not /users/{id}/playlists, which is the account's own. */
	assert(seen_count == 1);
	assert(strcmp(seen[0].method, "GET") == 0);
	assert(strncmp(seen[0].url, API "/users/" USER_ID "/favorites/playlists?",
				   strlen(API "/users/" USER_ID "/favorites/playlists?")) == 0);
	assert(has_param(seen[0].url, "countryCode=FR"));
	assert(has_param(seen[0].url, "limit=50"));
	assert(has_param(seen[0].url, "offset=0"));
	/* Newest first: what was favourited yesterday is what is looked for today. */
	assert(has_param(seen[0].url, "order=DATE"));
	assert(has_param(seen[0].url, "orderDirection=DESC"));
	assert(strstr(seen[0].headers, "Authorization: Bearer ACCESS-TOKEN\r\n") != NULL);
	assert(seen[0].body[0] == '\0');

	/* The playlist is under "item": read as a flat entry, all three would come
	 * out as blank rows with no error to say why. */
	assert(strcmp(got[0].id, EDITORIAL_ID) == 0);
	assert(strcmp(got[0].name, "Chill Hits") == 0);
	assert(got[0].track_count == 50);
	/* Tidal's editors made it, and nobody is named: the row says TIDAL
	 * rather than nothing. */
	assert(strcmp(got[0].owner, "TIDAL") == 0);
	/* The square picture, where there is one. */
	assert(strcmp(got[0].image, "https://resources.tidal.com/images/0d7a2bd5/6a8e/45c0/9d0e/1f0d6f4d2a11/320x320.jpg") ==
		   0);

	/* An artist's playlist names the artist. */
	assert(strcmp(got[1].id, ARTIST_ID) == 0);
	assert(strcmp(got[1].name, "JAY-Z's Year End Picks 2019") == 0);
	assert(got[1].track_count == 40);
	assert(strcmp(got[1].owner, "JAY Z") == 0);

	/* Another user's carries a number and no name, and no name is made up for
	 * it; with no square picture, the wide one is used. */
	assert(strcmp(got[2].id, FRIEND_ID) == 0);
	assert(strcmp(got[2].name, "lofi") == 0);
	assert(got[2].track_count == 1);
	assert(got[2].owner[0] == '\0');
	assert(strcmp(got[2].image, "https://resources.tidal.com/images/99999999/8888/7777/6666/555555555555/320x320.jpg") ==
		   0);
}

/* The list is paged like every other: the next fifty are asked for by offset,
 * and no more entries are written than the caller has room for. */
static void test_the_next_page_and_the_room_there_is(void) {
	reset_http();
	push_reply(200, FAVOURITES_PAGE);
	push_reply(200, "{\"limit\":50,\"offset\":100,\"totalNumberOfItems\":53,\"items\":[]}");

	tidal_playlist_t got[3];
	memset(got, 0, sizeof(got));
	assert(tidal_favorite_playlists(50, got, 2) == 2);
	assert(has_param(seen[0].url, "offset=50"));
	assert(has_param(seen[0].url, "limit=2"));
	assert(strcmp(got[1].id, ARTIST_ID) == 0);
	assert(got[2].id[0] == '\0'); /* the third stayed out: there was room for two */

	/* Past the end is an empty page, which is an answer and not an error. */
	assert(tidal_favorite_playlists(100, got, 3) == 0);
	assert(has_param(seen[1].url, "offset=100"));

	/* And no room at all asks for nothing. */
	assert(tidal_favorite_playlists(0, got, 0) == 0);
	assert(seen_count == 2);
}

/* A name that is there is never replaced, whatever the type says; and the
 * account's own playlists are still asked for where they always were. */
static void test_the_accounts_own_list_is_left_alone(void) {
	reset_http();
	push_reply(200, "{\"limit\":50,\"offset\":0,\"totalNumberOfItems\":2,\"items\":["
					"{\"uuid\":\"11111111-aaaa-bbbb-cccc-000000000001\",\"title\":\"Evening\",\"numberOfTracks\":12,"
					"\"creator\":{\"id\":" USER_ID "},\"type\":\"USER\",\"image\":\"\",\"squareImage\":\"\"},"
					"{\"uuid\":\"11111111-aaaa-bbbb-cccc-000000000002\",\"title\":\"Staff picks\",\"numberOfTracks\":3,"
					"\"creator\":{\"id\":0,\"name\":\"The editors\"},\"type\":\"EDITORIAL\"}"
					"]}");

	tidal_playlist_t got[4];
	assert(tidal_user_playlists(0, got, 4) == 2);
	assert(strncmp(seen[0].url, API "/users/" USER_ID "/playlists?", strlen(API "/users/" USER_ID "/playlists?")) == 0);
	assert(strstr(seen[0].url, "favorites") == NULL);
	assert(strcmp(got[0].name, "Evening") == 0);
	assert(got[0].owner[0] == '\0');
	assert(strcmp(got[1].owner, "The editors") == 0);
}

/* Tidal says no in its own words, and they are what the page shows. The
 * caller's rows are not written to: the page keeps the previous list on
 * screen when a request fails, and those rows have to stay true. */
static void test_a_refusal_is_reported_and_writes_nothing(void) {
	reset_http();
	push_reply(404, "{\"status\":404,\"subStatus\":2001,\"userMessage\":\"The requested resource could not be found\"}");

	tidal_playlist_t got[2];
	memset(got, 0x55, sizeof(got));
	tidal_playlist_t before[2];
	memcpy(before, got, sizeof(got));

	assert(tidal_favorite_playlists(0, got, 2) == -1);
	assert(strcmp(tidal_last_error(), "The requested resource could not be found") == 0);
	assert(memcmp(got, before, sizeof(got)) == 0);
}

static void test_the_playlist_leaves_the_favourites(void) {
	/* A successful DELETE answers 204 with nothing in it. */
	reset_http();
	push_reply(204, "");
	assert(tidal_favorite_remove_playlist(EDITORIAL_ID));
	assert(seen_count == 1);
	assert(strcmp(seen[0].method, "DELETE") == 0);
	/* On the favourites collection. DELETE /playlists/{uuid} is how one of the
	 * account's own is deleted, and this playlist is somebody else's. */
	assert(strcmp(seen[0].url, API "/users/" USER_ID "/favorites/playlists/" EDITORIAL_ID "?countryCode=FR") == 0);
	assert(seen[0].body[0] == '\0');
	assert(strstr(seen[0].headers, "Authorization: Bearer ACCESS-TOKEN\r\n") != NULL);
	assert(tidal_last_error()[0] == '\0');

	/* Refused -- a sign-in that may read and not write, say -- with the reason. */
	reset_http();
	push_reply(403, "{\"status\":403,\"subStatus\":6005,\"userMessage\":\"Missing required scope(s): w_usr\"}");
	assert(!tidal_favorite_remove_playlist(ARTIST_ID));
	assert(strcmp(tidal_last_error(), "Missing required scope(s): w_usr") == 0);

	/* A row without an id asks for nothing rather than for the whole
	 * collection. */
	reset_http();
	assert(!tidal_favorite_remove_playlist(""));
	assert(!tidal_favorite_remove_playlist(NULL));
	assert(seen_count == 0);
}

int main(void) {
	char dir[] = "/tmp/sonix-tidal-favplaylists-XXXXXX";
	assert(mkdtemp(dir));
	snprintf(config_file, sizeof(config_file), "%s/device_config.ini", dir);

	test_signed_out_asks_nothing();
	sign_in();
	test_the_list_is_read_from_the_favourites();
	test_the_next_page_and_the_room_there_is();
	test_the_accounts_own_list_is_left_alone();
	test_a_refusal_is_reported_and_writes_nothing();
	test_the_playlist_leaves_the_favourites();

	/* The session the sign-in wrote, and the directory it was written to. */
	char session[300];
	snprintf(session, sizeof(session), "%s/tidal_session.ini", dir);
	assert(remove(session) == 0);
	assert(rmdir(dir) == 0);

	printf("PASS tidal favourite playlists (request, wrapped entries and creators, paging, refusal, removal)\n");
	return 0;
}
