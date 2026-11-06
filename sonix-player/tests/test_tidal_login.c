/* Exercise Tidal's v1 OAuth2 device-login error mapping and read-only scope
 * fallback without touching the network. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/streaming/tidal.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamkeys.h"

static const char *fake_cfg_scope = "";

const char *config_get(const char *section, const char *key, const char *fallback) {
	if (strcmp(section, "tidal") == 0 && strcmp(key, "scope") == 0) {
		return fake_cfg_scope[0] ? fake_cfg_scope : fallback;
	}
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
const char *config_path(void) { return "/tmp/nonexistent-sonix-tidal-cfg.ini"; }

const char *streamkeys_tidal_client_id(void) { return "test_client_id16"; }
const char *streamkeys_tidal_client_secret(void) {
	return "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789+/AbCdE=";
}

const char *tr(const char *key) {
	if (strcmp(key, "tidal_not_device_client") == 0) {
		return "NOT_LIMITED_INPUT_DEVICE_MSG";
	}
	return key;
}

char *http_url_encode(const char *in, char *out, size_t out_size) {
	size_t w = 0;
	for (size_t i = 0; in[i] && w + 4 < out_size; i++) {
		unsigned char c = (unsigned char)in[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
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

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body,
				 char **out, size_t *out_len, size_t limit, int timeout_secs, int *status_out) {
	(void)url;
	(void)extra_headers;
	(void)want_error_body;
	(void)out;
	(void)out_len;
	(void)limit;
	(void)timeout_secs;
	if (status_out) {
		*status_out = 404;
	}
	return false;
}

typedef struct {
	int status;
	const char *body;
} canned_reply_t;

static canned_reply_t replies[8];
static int reply_count;
static int reply_idx;
static char seen_urls[8][256];
static char seen_bodies[8][512];
static int seen_count;

bool http_request(const char *url, const http_req_t *req, char **out, size_t *out_len,
				  size_t limit, int timeout_secs, int *status_out) {
	(void)limit;
	(void)timeout_secs;
	assert(seen_count < 8);
	snprintf(seen_urls[seen_count], sizeof(seen_urls[0]), "%s", url);
	snprintf(seen_bodies[seen_count], sizeof(seen_bodies[0]), "%s", req && req->body ? req->body : "");
	seen_count++;

	assert(reply_idx < reply_count);
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
	assert(reply_count < 8);
	replies[reply_count++] = (canned_reply_t){status, body};
}

static void test_developer_portal_key_gives_actionable_error(void) {
	fake_cfg_scope = "";
	tidal_init();

	reset_http();
	push_reply(400, "{\"status\":400,\"error\":\"invalid_client\","
					"\"sub_status\":1005,"
					"\"error_description\":\"Client is not a Limited Input Device client.\"}");

	tidal_login_t code;
	assert(!tidal_login_begin(&code));
	assert(seen_count == 1);
	assert(strcmp(tidal_last_error(), "NOT_LIMITED_INPUT_DEVICE_MSG") == 0);
}

static void test_readonly_scope_fallback_synchronizes_begin_and_poll(void) {
	fake_cfg_scope = "";
	tidal_init();

	reset_http();
	/* Stock V1 device key rejects WRITE_USR (w_usr), accepts READ_USR_DATA (r_usr). */
	push_reply(400, "{\"status\":400,\"error\":\"invalid_scope\","
					"\"error_description\":\"One or more of the requested scopes are not allowed. "
					"Requested scopes: [WRITE_USR, READ_USR_DATA], allowed scopes: [READ_USR_DATA]\"}");
	push_reply(200, "{\"deviceCode\":\"dev-123\",\"userCode\":\"ABCDE\","
					"\"verificationUri\":\"link.tidal.com\","
					"\"verificationUriComplete\":\"link.tidal.com/ABCDE\","
					"\"expiresIn\":300,\"interval\":2}");
	push_reply(400, "{\"status\":400,\"error\":\"authorization_pending\","
					"\"sub_status\":1002,\"error_description\":\"pending\"}");

	tidal_login_t code;
	assert(tidal_login_begin(&code));
	assert(seen_count == 2);
	assert(strstr(seen_bodies[0], "scope=r_usr%20w_usr") != NULL);
	assert(strstr(seen_bodies[1], "scope=r_usr") != NULL);
	assert(strstr(seen_bodies[1], "w_usr") == NULL);
	assert(strcmp(code.user_code, "ABCDE") == 0);

	/* Subsequent token poll MUST send the downgraded scope=r_usr, not r_usr%20w_usr. */
	assert(tidal_login_poll(&code) == TIDAL_LOGIN_PENDING);
	assert(seen_count == 3);
	assert(strstr(seen_bodies[2], "scope=r_usr&") != NULL ||
		   strcmp(seen_bodies[2] + strlen(seen_bodies[2]) - strlen("scope=r_usr"), "scope=r_usr") == 0);
	assert(strstr(seen_bodies[2], "w_usr") == NULL);
}

static void test_explicit_custom_scope_is_not_overridden(void) {
	fake_cfg_scope = "r_usr w_sub";
	tidal_init();

	reset_http();
	push_reply(400, "{\"status\":400,\"error\":\"invalid_scope\","
					"\"error_description\":\"One or more of the requested scopes are not allowed.\"}");

	tidal_login_t code;
	assert(!tidal_login_begin(&code));
	/* Custom scope in device_config.ini must fail directly without silent rewrite. */
	assert(seen_count == 1);
	fake_cfg_scope = "";
}

int main(void) {
	test_developer_portal_key_gives_actionable_error();
	test_readonly_scope_fallback_synchronizes_begin_and_poll();
	test_explicit_custom_scope_is_not_overridden();
	printf("PASS tidal login (Limited Input Device message, r_usr fallback and poll scope sync)\n");
	return 0;
}
