#define _GNU_SOURCE
#include "src/system/streaming/qobuzdl.h"
#include "src/system/net/http.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[512];
static int pages;
static int reads;
static int fail_body;
static int calls;
const char *storage_sd_root(void) { return root; }
bool storage_card_attached(void) { return true; }
bool storage_sd_writable(void) { return true; }
bool usb_storage_active(void) { return false; }
bool qobuz_logged_in(void) { return true; }
void gui_notify_popup(const char *s) { (void)s; }
bool qobuz_track_url_for_format(long id, int format, char *url, size_t size, char *mime, size_t msize, int *actual) {
    (void)id; calls++;
    snprintf(url, size, "https://example.test/audio");
    snprintf(mime, msize, "%s", format == QOBUZ_FORMAT_MP3 ? "audio/mpeg" : "audio/flac");
    *actual = format;
    return true;
}
int qobuz_album_tracks(const char *id, int offset, qobuz_track_t *out, int max) {
    assert(!strcmp(id, "album") && max == 50);
    pages++;
    int n = offset == 0 ? 50 : offset == 50 ? 2 : 0;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].id = offset + i + 1;
        out[i].streamable = true;
        snprintf(out[i].title, sizeof(out[i].title), "Track %d", offset + i + 1);
        snprintf(out[i].artist, sizeof(out[i].artist), "Artist");
        snprintf(out[i].album, sizeof(out[i].album), "Album");
    }
    return n;
}
bool http_stream_open_ex(http_stream_t *st, const char *url, int timeout, const http_stream_options_t *opts) {
    (void)url; (void)timeout; assert(opts->no_icy && opts->no_https_downgrade);
    st->content_length64 = fail_body ? 10 : 4;
    reads = 0;
    return true;
}
int http_stream_read(http_stream_t *st, void *buf, int len) {
    (void)st; (void)len;
    if (reads++) return 0;
    memcpy(buf, "data", 4);
    return 4;
}
void http_stream_close(http_stream_t *st) { (void)st; }
static void wait_done(void) {
    for (int i = 0; i < 10000 && qobuzdl_busy(); i++) usleep(1000);
    assert(!qobuzdl_busy());
}
int main(void) {
    char tmp[] = "/tmp/qobuzdl-test-XXXXXX";
    assert(mkdtemp(tmp));
    snprintf(root, sizeof(root), "%s", tmp);
    assert(qobuzdl_album("album", QOBUZ_FORMAT_CD));
    wait_done();
    assert(pages == 2 && calls == 52);
    qobuzdl_entry_t *entries = calloc(100, sizeof(*entries));
    assert(entries && qobuzdl_history(entries, 100) == 52);
    qobuz_track_t track = {.id = 100, .streamable = true};
    snprintf(track.title, sizeof(track.title), "Song");
    fail_body = 1;
    assert(qobuzdl_track(&track, QOBUZ_FORMAT_MP3));
    wait_done();
    assert(qobuzdl_history(entries, 100) == 52);
    char path[768];
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/100.mp3.part", root);
    assert(access(path, F_OK));
    fail_body = 0;
    assert(qobuzdl_track(&track, QOBUZ_FORMAT_MP3));
    wait_done();
    assert(qobuzdl_history(entries, 100) == 53);
    int before = calls;
    assert(qobuzdl_track(&track, QOBUZ_FORMAT_MP3));
    wait_done();
    // A duplicate is not overwritten, even though its signed URL is refreshed.
    assert(calls == before + 1 && qobuzdl_history(entries, 100) == 53);
    qobuz_track_t unavailable = {.id = 200, .streamable = false};
    assert(qobuzdl_track(&unavailable, QOBUZ_FORMAT_CD));
    wait_done();
    assert(qobuzdl_history(entries, 100) == 53);
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/100.mp3", root);
    assert(!access(path, F_OK));
    assert(!qobuzdl_track(&track, -1));
    free(entries);
    puts("test_qobuzdl: all assertions passed");
    return 0;
}
