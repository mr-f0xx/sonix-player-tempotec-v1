#define _GNU_SOURCE
#include "src/system/streaming/qobuz.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
static int calls, saves, status = 200;
static const char *reply = "{\"id\":42,\"display_name\":\"Alice\"}";
static char stored[256];
const char *tr(const char *s) { return s; }
const char *streamkeys_qobuz_app_id(void) { return "app"; }
const char *streamkeys_qobuz_app_secret(void) { return "secret"; }
const char *config_get(const char *s, const char *k, const char *f) { (void)s; (void)k; return f; }
long config_get_int(const char *s, const char *k, long f) { (void)s; (void)k; return f; }
void config_set(const char *s, const char *k, const char *v) {
    (void)s; if (!strcmp(k, "token")) snprintf(stored, sizeof(stored), "%s", v);
}
void config_set_int(const char *s, const char *k, long v) { (void)s; (void)k; (void)v; }
bool config_save(void) { saves++; return true; }
const char *http_last_error(void) { return "HTTP failure"; }
bool http_get_ex(const char *url, const char *headers, bool errors, char **out,
                 size_t *len, size_t limit, int timeout, int *code) {
    (void)errors; (void)limit; (void)timeout;
    assert(strstr(url, "/user/get?user_id=42"));
    assert(!strstr(url, "token"));
    assert(strstr(headers, "X-User-Auth-Token: valid-token\r\n"));
    calls++; *out = strdup(reply); *len = strlen(reply); *code = status; return true;
}
int main(void) {
    qobuz_init();
    assert(!qobuz_login_token("bad\r\ninjection", "42"));
    assert(!qobuz_login_token("a&b", "42"));
    assert(!qobuz_login_token("valid-token", "42&other=1"));
    assert(!qobuz_login_token("valid-token", "0"));
    assert(!qobuz_login_token("valid-token", "99999999999999999999999"));
    assert(!calls && !saves);
    assert(qobuz_login_token("valid-token", "42"));
    assert(qobuz_logged_in() && !strcmp(stored, "valid-token"));
    assert(!strcmp(qobuz_display_name(), "Alice") && saves == 1);
    reply = "{\"id\":99}";
    assert(!qobuz_login_token("valid-token", "42"));
    assert(saves == 1 && !strcmp(qobuz_display_name(), "Alice"));
    status = 401; reply = "{\"status\":\"error\",\"message\":\"Expired\"}";
    assert(!qobuz_login_token("valid-token", "42"));
    assert(saves == 1 && !strcmp(qobuz_last_error(), "Expired"));
    qobuz_logout(); assert(!qobuz_logged_in() && !stored[0]);
    puts("Qobuz token login tests passed");
}
