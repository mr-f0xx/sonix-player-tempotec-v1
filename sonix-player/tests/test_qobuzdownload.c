#define _GNU_SOURCE
#include "src/system/streaming/qobuzdownload.h"
#include "src/system/net/http.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>

static atomic_int completed;
static int short_body, reads;
static char result[256];
int qobuz_get_format(void) { return 6; }
bool qobuz_track_url_format(long id, int fmt, char *url, size_t size, char *mime, size_t msize, int *out) {
    (void)id; (void)out; assert(fmt == 6);
    snprintf(url, size, "https://example.test/file");
    snprintf(mime, msize, "audio/flac"); return true;
}
const char *qobuz_last_error(void) { return "API refused"; }
const char *http_last_error(void) { return "HTTP refused"; }
void streamturn_acquire(void) {}
void streamturn_release(void) {}
bool http_stream_open_ex(http_stream_t *s, const char *url, int timeout, const http_stream_options_t *o) {
    (void)url; (void)timeout; assert(o->no_icy && o->no_https_downgrade);
    memset(s, 0, sizeof(*s)); s->content_length64 = short_body ? 100 : 4; reads = 0; return true;
}
int http_stream_read(http_stream_t *s, void *buf, int size) {
    (void)s; (void)size;
    if (reads++) return 0;
    memcpy(buf, "fLaC", 4); return 4;
}
void http_stream_close(http_stream_t *s) { (void)s; }
static void done(const char *error) {
    snprintf(result, sizeof(result), "%s", error ? error : "");
    atomic_store(&completed, 1);
}
static void wait_done(void) {
    for (int i = 0; i < 5000 && (!atomic_load(&completed) || qobuzdownload_busy()); i++) usleep(1000);
    assert(atomic_load(&completed) && !qobuzdownload_busy());
}
int main(void) {
    char root[] = "/tmp/sonix-qobuz-XXXXXX", path[512];
    assert(mkdtemp(root));
    qobuz_track_t t = {.id = 42, .streamable = true};
    strcpy(t.artist, "../Artist"); strcpy(t.title, "Title/unsafe");
    assert(qobuzdownload_start(root, &t, done)); wait_done(); assert(!result[0]);
    snprintf(path, sizeof(path), "%s/Music/Qobuz/.._Artist - Title_unsafe [42-q6].flac", root);
    struct stat st; assert(!stat(path, &st) && st.st_size == 4);
    atomic_store(&completed, 0);
    assert(qobuzdownload_start(root, &t, done)); wait_done(); assert(strstr(result, "exists"));
    unlink(path);
    short_body = 1; atomic_store(&completed, 0);
    assert(qobuzdownload_start(root, &t, done)); wait_done(); assert(strstr(result, "Incomplete"));
    assert(stat(path, &st)); strcat(path, ".part"); assert(stat(path, &st));
    snprintf(path, sizeof(path), "%s/Music/Qobuz", root); rmdir(path);
    snprintf(path, sizeof(path), "%s/Music", root); rmdir(path); rmdir(root);
    puts("qobuzdownload tests passed");
}
