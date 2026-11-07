#include "qobuzdl.h"

#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/shell/gui.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/net/http.h"

#define CHUNK 32768
#define PAGE 50
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool running;

typedef struct {
    char root[512];
    char album_id[40];
    qobuz_track_t track;
    int format;
} request_t;

bool qobuzdl_busy(void) {
    pthread_mutex_lock(&lock);
    bool result = running;
    pthread_mutex_unlock(&lock);
    return result;
}

static bool folder(const char *root, char *out, size_t size) {
    char music[640];
    if (snprintf(music, sizeof(music), "%s/Music", root) >= (int)sizeof(music) ||
        snprintf(out, size, "%s/Qobuz Downloads", music) >= (int)size) return false;
    if (mkdir(music, 0755) && access(music, F_OK)) return false;
    if (mkdir(out, 0755) && access(out, F_OK)) return false;
    return true;
}

static void tag_value(FILE *f, const char *key, const char *value) {
    fprintf(f, "%s=", key);
    for (const char *p = value; *p; ++p) fputc(*p == '\n' || *p == '\r' ? ' ' : *p, f);
    fputc('\n', f);
}

static bool download(const request_t *job, const qobuz_track_t *track) {
    if (!track->streamable || track->id <= 0 || !storage_card_attached() || !storage_sd_writable() ||
        usb_storage_active() || !storage_sd_root() || strcmp(storage_sd_root(), job->root)) return false;
    char url[QOBUZ_URL_MAX], mime[64];
    int actual = 0;
    if (!qobuz_track_url_for_format(track->id, job->format, url, sizeof(url), mime, sizeof(mime), &actual)) return false;
    bool mp3 = actual == QOBUZ_FORMAT_MP3 && strstr(mime, "mpeg");
    bool flac = actual != QOBUZ_FORMAT_MP3 && strstr(mime, "flac");
    if ((job->format == QOBUZ_FORMAT_MP3 && !mp3) || (job->format != QOBUZ_FORMAT_MP3 && !flac)) return false;
    // Playback and the library accept paths of at most 511 bytes.
    char dir[768], path[512], part[528], tags[528], tagpart[544];
    if (!folder(job->root, dir, sizeof(dir))) return false;
    if (snprintf(path, sizeof(path), "%s/%ld.%s", dir, track->id, mp3 ? "mp3" : "flac") >= (int)sizeof(path)) return false;
    snprintf(part, sizeof(part), "%s.part", path);
    snprintf(tags, sizeof(tags), "%s.tags", path);
    snprintf(tagpart, sizeof(tagpart), "%s.part", tags);
    // Do not replace a completed file when an attempted re-download fails.
    struct stat existing;
    if (stat(path, &existing) == 0 && existing.st_size > 0) return true;
    http_stream_t *stream = calloc(1, sizeof(*stream));
    if (!stream) return false;
    http_stream_options_t opts = {.no_icy = true, .no_https_downgrade = true};
    bool opened = http_stream_open_ex(stream, url, 30, &opts);
    bool ok = opened;
    FILE *f = ok ? fopen(part, "wb") : NULL;
    uint64_t done = 0;
    char *buf = f ? malloc(CHUNK) : NULL;
    ok = f && buf;
    while (ok) {
        if (!storage_card_attached() || !storage_sd_writable() || usb_storage_active() ||
            !storage_sd_root() || strcmp(storage_sd_root(), job->root)) { ok = false; break; }
        int n = http_stream_read(stream, buf, CHUNK);
        if (n < 0) { ok = false; break; }
        if (!n) break;
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
        done += (unsigned)n;
    }
    free(buf);
    if (f && fclose(f)) ok = false;
    if (!done || (stream->content_length64 && done != stream->content_length64)) ok = false;
    if (opened) http_stream_close(stream);
    free(stream);
    if (ok) {
        f = fopen(tagpart, "w");
        if (!f) ok = false;
        else {
            tag_value(f, "title", track->title);
            tag_value(f, "artist", track->artist);
            tag_value(f, "album", track->album);
            fprintf(f, "track=%d\n", track->track_number);
            if (fclose(f)) ok = false;
        }
    }
    if (ok && rename(tagpart, tags)) ok = false;
    if (ok && rename(part, path)) { remove(tags); ok = false; }
    if (!ok) { remove(part); remove(tagpart); }
    return ok;
}

static void *worker(void *arg) {
    request_t *job = arg;
    int succeeded = 0, failed = 0;
    if (job->album_id[0]) {
        qobuz_track_t *tracks = calloc(PAGE, sizeof(*tracks));
        if (!tracks) failed++;
        else {
            for (int offset = 0; ; offset += PAGE) {
                int n = qobuz_album_tracks(job->album_id, offset, tracks, PAGE);
                if (n < 0) { failed++; break; }
                if (!n && !offset) failed++;
                for (int i = 0; i < n; i++) {
                    if (download(job, &tracks[i])) succeeded++; else failed++;
                }
                if (n < PAGE) break;
            }
            free(tracks);
        }
    } else if (download(job, &job->track)) succeeded++; else failed++;
    free(job);
    pthread_mutex_lock(&lock);
    running = false;
    pthread_mutex_unlock(&lock);
    if (failed) gui_notify_popup(succeeded ? "Some Qobuz tracks could not be downloaded" : "Qobuz download failed");
    else gui_notify_popup("Qobuz download complete");
    return NULL;
}

static bool start(const qobuz_track_t *track, const char *album, int format) {
    if ((format != QOBUZ_FORMAT_MP3 && format != QOBUZ_FORMAT_CD) ||
        !storage_sd_root() || !storage_card_attached() || !storage_sd_writable() || usb_storage_active() || !qobuz_logged_in()) return false;
    request_t *job = calloc(1, sizeof(*job));
    if (!job) return false;
    if (snprintf(job->root, sizeof(job->root), "%s", storage_sd_root()) >= (int)sizeof(job->root)) { free(job); return false; }
    if (track) job->track = *track;
    if (album) snprintf(job->album_id, sizeof(job->album_id), "%s", album);
    job->format = format;
    pthread_mutex_lock(&lock);
    if (running) { pthread_mutex_unlock(&lock); free(job); return false; }
    running = true;
    pthread_mutex_unlock(&lock);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    int rc = pthread_create(&thread, &attr, worker, job);
    pthread_attr_destroy(&attr);
    if (rc) {
        free(job);
        pthread_mutex_lock(&lock); running = false; pthread_mutex_unlock(&lock);
        return false;
    }
    return true;
}
bool qobuzdl_track(const qobuz_track_t *track, int format) { return track && start(track, NULL, format); }
bool qobuzdl_album(const char *album_id, int format) { return album_id && *album_id && start(NULL, album_id, format); }

static int compare_entries(const void *a, const void *b) {
    const qobuzdl_entry_t *x = a, *y = b;
    return x->modified < y->modified ? 1 : x->modified > y->modified ? -1 : strcmp(x->path, y->path);
}
int qobuzdl_history(qobuzdl_entry_t *entries, int max) {
    if (!entries || max <= 0 || !storage_sd_root() || !storage_card_attached()) return 0;
    char dir[768];
    if (snprintf(dir, sizeof(dir), "%s/Music/Qobuz Downloads", storage_sd_root()) >= (int)sizeof(dir)) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        char *end;
        long id = strtol(de->d_name, &end, 10);
        if (id <= 0 || (strcmp(end, ".mp3") && strcmp(end, ".flac"))) continue;
        qobuzdl_entry_t entry = {0};
        if (snprintf(entry.path, sizeof(entry.path), "%s/%s", dir, de->d_name) >= (int)sizeof(entry.path)) continue;
        struct stat st;
        if (stat(entry.path, &st) || !S_ISREG(st.st_mode) || !st.st_size) continue;
        entry.modified = (long long)st.st_mtime;
        char tags[800], line[512];
        snprintf(tags, sizeof(tags), "%s.tags", entry.path);
        FILE *f = fopen(tags, "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (!strncmp(line, "title=", 6)) snprintf(entry.title, sizeof(entry.title), "%.*s", (int)sizeof(entry.title)-1, line + 6);
                if (!strncmp(line, "artist=", 7)) snprintf(entry.artist, sizeof(entry.artist), "%.*s", (int)sizeof(entry.artist)-1, line + 7);
            }
            fclose(f);
        }
        if (!entry.title[0]) snprintf(entry.title, sizeof(entry.title), "%ld", id);
        if (count < max) entries[count++] = entry;
        else {
            int oldest = 0;
            for (int i = 1; i < count; i++) if (compare_entries(&entries[i], &entries[oldest]) > 0) oldest = i;
            if (compare_entries(&entry, &entries[oldest]) < 0) entries[oldest] = entry;
        }
    }
    closedir(d);
    qsort(entries, count, sizeof(*entries), compare_entries);
    return count;
}
