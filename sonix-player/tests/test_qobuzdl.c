#define _GNU_SOURCE
#include "src/system/streaming/qobuzdl.h"
#include "src/system/net/http.h"
#include <assert.h>
#include <pthread.h>
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
static int forced_format;
static int last_requested_format;
static int cover_reads;
static int cover_mode;
static int cover_requests;
static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cond = PTHREAD_COND_INITIALIZER;
static int block_body, body_waiting;
const char *storage_sd_root(void) { return root; }
bool storage_card_attached(void) { return true; }
bool storage_sd_writable(void) { return true; }
bool usb_storage_active(void) { return false; }
bool qobuz_logged_in(void) { return true; }
void gui_notify_popup(const char *s) { (void)s; }
bool qobuz_track_url_for_format(long id, int format, char *url, size_t size, char *mime, size_t msize, int *actual) {
    (void)id; calls++;
    last_requested_format = format;
    int returned = forced_format ? forced_format : format;
    snprintf(url, size, "https://example.test/audio");
    snprintf(mime, msize, "%s", returned == QOBUZ_FORMAT_MP3 ? "audio/mpeg" : "audio/flac");
    *actual = returned;
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
        snprintf(out[i].album_artist, sizeof(out[i].album_artist), "Album Artist");
        snprintf(out[i].released, sizeof(out[i].released), "2020");
        snprintf(out[i].cover, sizeof(out[i].cover), "https://example.test/cover");
        out[i].album_track_count = 52;
        out[i].track_number = offset + i + 1;
        out[i].disc_number = 1;
    }
    return n;
}
bool http_stream_open_ex(http_stream_t *st, const char *url, int timeout, const http_stream_options_t *opts) {
    (void)timeout; assert(opts->no_icy && opts->no_https_downgrade);
    cover_mode = strstr(url, "/cover") != NULL;
    if (cover_mode) { cover_requests++; cover_reads = 0; }
    st->content_length64 = cover_mode ? 8 : fail_body ? 10 : 4;
    reads = 0;
    return true;
}
int http_stream_read(http_stream_t *st, void *buf, int len) {
    (void)st; (void)len;
    if (cover_mode) {
        if (cover_reads++) return 0;
        memcpy(buf, "\xff\xd8\xffTEST!", 8);
        return 8;
    }
    if (reads++) {
        pthread_mutex_lock(&pause_lock);
        if (block_body) {
            body_waiting = 1;
            pthread_cond_broadcast(&pause_cond);
            while (block_body) pthread_cond_wait(&pause_cond, &pause_lock);
        }
        pthread_mutex_unlock(&pause_lock);
        return 0;
    }
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
    pthread_mutex_lock(&pause_lock); block_body = 1; pthread_mutex_unlock(&pause_lock);
    assert(qobuzdl_album("album", QOBUZ_FORMAT_CD));
    pthread_mutex_lock(&pause_lock);
    while (!body_waiting) pthread_cond_wait(&pause_cond, &pause_lock);
    pthread_mutex_unlock(&pause_lock);
    qobuzdl_progress_t album_progress;
    qobuzdl_progress(&album_progress);
    assert(album_progress.running && album_progress.album && album_progress.total_tracks == 52);
    assert(album_progress.completed_tracks == 0 && album_progress.bytes_done == 4 && album_progress.bytes_total == 4);
    pthread_mutex_lock(&pause_lock); block_body = 0; body_waiting = 0;
    pthread_cond_broadcast(&pause_cond); pthread_mutex_unlock(&pause_lock);
    wait_done();
    assert(pages == 2 && calls == 52 && cover_requests == 1);
    char path[768];
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/Album Artist - Album (2020) [album]/cover.jpg", root);
    assert(!access(path, F_OK));
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/Album Artist - Album (2020) [album]/1.flac.tags", root);
    FILE *tags = fopen(path, "r"); assert(tags);
    char contents[512] = {0}; assert(fread(contents, 1, sizeof(contents)-1, tags) > 0); fclose(tags);
    assert(strstr(contents, "title=Track 1\n") && strstr(contents, "album_artist=Album Artist\n") &&
           strstr(contents, "year=2020\n") && strstr(contents, "disc=1\n"));
    qobuzdl_entry_t *entries = calloc(100, sizeof(*entries));
    assert(entries && qobuzdl_history(entries, 100) == 52);
    qobuz_track_t track = {.id = 100, .streamable = true};
    snprintf(track.title, sizeof(track.title), "Song");
    fail_body = 1;
    pthread_mutex_lock(&pause_lock); block_body = 1; pthread_mutex_unlock(&pause_lock);
    assert(qobuzdl_track(&track, QOBUZ_FORMAT_MP3));
    pthread_mutex_lock(&pause_lock);
    while (!body_waiting) pthread_cond_wait(&pause_cond, &pause_lock);
    pthread_mutex_unlock(&pause_lock);
    qobuzdl_progress_t progress;
    qobuzdl_progress(&progress);
    assert(progress.running && !progress.album && progress.bytes_done == 4 && progress.bytes_total == 10);
    assert(!strcmp(progress.title, "Song"));
    pthread_mutex_lock(&pause_lock); block_body = 0; pthread_cond_broadcast(&pause_cond); pthread_mutex_unlock(&pause_lock);
    wait_done();
    assert(qobuzdl_history(entries, 100) == 52);
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/Unknown artist - Unknown album (Unknown year) [100]/100.mp3.part", root);
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
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/Unknown artist - Unknown album (Unknown year) [100]/100.mp3", root);
    assert(!access(path, F_OK));
    // Both FLAC qualities can coexist and appear in history.
    assert(qobuzdl_track(&track, QOBUZ_FORMAT_CD));
    wait_done();
    assert(last_requested_format == QOBUZ_FORMAT_CD);
    assert(qobuzdl_track(&track, QOBUZ_FORMAT_HIRES192));
    wait_done();
    assert(last_requested_format == QOBUZ_FORMAT_HIRES192);
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/Unknown artist - Unknown album (Unknown year) [100]/100.flac", root);
    assert(!access(path, F_OK));
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/Unknown artist - Unknown album (Unknown year) [100]/100-hires.flac", root);
    assert(!access(path, F_OK));
    assert(qobuzdl_history(entries, 100) == 55);
    // If Qobuz falls back to CD quality, do not mislabel it as hi-res.
    qobuz_track_t fallback = {.id = 101, .streamable = true};
    forced_format = QOBUZ_FORMAT_CD;
    assert(qobuzdl_track(&fallback, QOBUZ_FORMAT_HIRES192));
    wait_done();
    forced_format = 0;
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/Unknown artist - Unknown album (Unknown year) [101]/101.flac", root);
    assert(!access(path, F_OK));
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/Unknown artist - Unknown album (Unknown year) [101]/101-hires.flac", root);
    assert(access(path, F_OK));
    assert(qobuzdl_history(entries, 100) == 56);
    // Files downloaded by the previous flat layout remain visible.
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/987.mp3", root);
    FILE *legacy = fopen(path, "wb"); assert(legacy); fputs("data", legacy); fclose(legacy);
    assert(qobuzdl_history(entries, 100) == 57);
    assert(qobuzdl_album("album", QOBUZ_FORMAT_HIRES192));
    wait_done();
    assert(last_requested_format == QOBUZ_FORMAT_HIRES192);
    snprintf(path, sizeof(path), "%s/Music/Qobuz Downloads/Album Artist - Album (2020) [album]/1-hires.flac", root);
    assert(!access(path, F_OK));
    assert(!qobuzdl_track(&track, -1));
    free(entries);
    puts("test_qobuzdl: all assertions passed");
    return 0;
}
