#include "qobuzdl.h"

#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/shell/gui.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/net/http.h"
#include "src/system/streaming/podcastdl.h"

#define CHUNK 32768
#define PAGE 50
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool running;
static qobuzdl_progress_t progress;

typedef struct {
    char root[512];
    char album_id[40];
    qobuz_track_t track;
    int format;
    char album_dir[512];
    bool cover_attempted;
    bool cover_failed;
    bool quality_lowered;
} request_t;

bool qobuzdl_busy(void) {
    pthread_mutex_lock(&lock);
    bool result = running;
    pthread_mutex_unlock(&lock);
    return result;
}

void qobuzdl_progress(qobuzdl_progress_t *out) {
    if (!out) return;
    pthread_mutex_lock(&lock);
    *out = progress;
    pthread_mutex_unlock(&lock);
}

static bool folder(const char *root, char *out, size_t size) {
    char music[640];
    if (snprintf(music, sizeof(music), "%s/Music", root) >= (int)sizeof(music) ||
        snprintf(out, size, "%s/Qobuz Downloads", music) >= (int)size) return false;
    if (mkdir(music, 0755) && access(music, F_OK)) return false;
    if (mkdir(out, 0755) && access(out, F_OK)) return false;
    return true;
}

// A hidden album-id marker disambiguates identical album names without putting
// the opaque Qobuz id in the visible folder name. Never merge different albums.
static bool album_folder(request_t *job, const qobuz_track_t *track) {
    if (job->album_dir[0]) return true;
    char base[768], artist[80], album[100], ident[48], fallback[40], year[16];
    if (!folder(job->root, base, sizeof(base))) return false;
    podcastdl_safe_name(track->album_artist[0] ? track->album_artist : track->artist,
                        "Unknown artist", artist, sizeof(artist));
    podcastdl_safe_name(track->album, "Unknown album", album, sizeof(album));
    snprintf(fallback, sizeof(fallback), "%ld", track->id);
    podcastdl_safe_name(job->album_id[0] ? job->album_id : track->album_id, fallback, ident, sizeof(ident));
    podcastdl_safe_name(track->released, "Unknown year", year, sizeof(year));
    for (int n = 1; n <= 100; n++) {
        char name[256], marker[560];
        if (n == 1) snprintf(name, sizeof(name), "%s - %s (%s)", artist, album, year);
        else snprintf(name, sizeof(name), "%s - %s (%s) (%d)", artist, album, year, n);
        if (snprintf(job->album_dir, sizeof(job->album_dir), "%s/%s", base, name) >= (int)sizeof(job->album_dir)) break;
        snprintf(marker, sizeof(marker), "%s/.qobuz-album-id", job->album_dir);
        if (mkdir(job->album_dir, 0755) == 0) {
            FILE *f = fopen(marker, "w");
            if (!f) { rmdir(job->album_dir); break; }
            bool ok = fprintf(f, "%s\n", ident) > 0;
            if (fclose(f)) ok = false;
            if (!ok) { remove(marker); rmdir(job->album_dir); break; }
            return true;
        }
        struct stat st;
        if (stat(job->album_dir, &st) || !S_ISDIR(st.st_mode)) continue;
        FILE *f = fopen(marker, "r");
        if (!f) continue; // an unrelated or old folder; leave it untouched
        char saved[48];
        bool matches = fgets(saved, sizeof(saved), f) != NULL;
        fclose(f);
        if (matches) { saved[strcspn(saved, "\r\n")] = 0; if (!strcmp(saved, ident)) return true; }
    }
    job->album_dir[0] = 0;
    return false;
}

// Cover files are bounded and written atomically; an HTML error or interrupted
// transfer must never be mistaken for a sleeve. A missing cover is nonfatal.
static bool fetch_cover(const request_t *job, const char *dir, const char *url) {
    if (!url || !url[0]) return false;
    char jpg[560], png[560], part[560];
    snprintf(jpg, sizeof(jpg), "%s/cover.jpg", dir);
    snprintf(png, sizeof(png), "%s/cover.png", dir);
    snprintf(part, sizeof(part), "%s/.cover.part", dir);
    struct stat st;
    if ((!stat(jpg, &st) && st.st_size > 0) || (!stat(png, &st) && st.st_size > 0)) return true;
    http_stream_t *stream = calloc(1, sizeof(*stream));
    if (!stream) return false;
    http_stream_options_t opts = {.no_icy = true, .no_https_downgrade = true};
    bool opened = http_stream_open_ex(stream, url, 30, &opts);
    FILE *f = opened ? fopen(part, "wb") : NULL;
    unsigned char buf[8192], head[8] = {0};
    size_t head_len = 0;
    uint64_t done = 0;
    bool ok = f != NULL;
    while (ok) {
        if (!storage_card_attached() || !storage_sd_writable() || usb_storage_active() ||
            !storage_sd_root() || strcmp(storage_sd_root(), job->root)) { ok = false; break; }
        int n = http_stream_read(stream, buf, sizeof(buf));
        if (n < 0) { ok = false; break; }
        if (!n) break;
        if (done + (unsigned)n > 4 * 1024 * 1024) { ok = false; break; }
        if (head_len < sizeof(head)) {
            size_t take = (size_t)n < sizeof(head) - head_len ? (size_t)n : sizeof(head) - head_len;
            memcpy(head + head_len, buf, take);
            head_len += take;
        }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
        done += (unsigned)n;
    }
    if (f && fclose(f)) ok = false;
    if (!done || (stream->content_length64 && done != stream->content_length64)) ok = false;
    if (opened) http_stream_close(stream);
    free(stream);
    bool is_jpg = head_len >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff;
    bool is_png = head_len >= 8 && !memcmp(head, "\x89PNG\r\n\x1a\n", 8);
    if (!is_jpg && !is_png) ok = false;
    if (ok && rename(part, is_jpg ? jpg : png)) ok = false;
    if (!ok) remove(part);
    return ok;
}

static void tag_value(FILE *f, const char *key, const char *value) {
    fprintf(f, "%s=", key);
    for (const char *p = value; *p; ++p) fputc(*p == '\n' || *p == '\r' ? ' ' : *p, f);
    fputc('\n', f);
}

static bool download(request_t *job, const qobuz_track_t *track) {
    if (!track->streamable || track->id <= 0 || !storage_card_attached() || !storage_sd_writable() ||
        usb_storage_active() || !storage_sd_root() || strcmp(storage_sd_root(), job->root)) return false;
    char url[QOBUZ_URL_MAX], mime[64];
    int actual = 0;
    if (!qobuz_track_url_for_format(track->id, job->format, url, sizeof(url), mime, sizeof(mime), &actual)) return false;
    bool mp3 = actual == QOBUZ_FORMAT_MP3 && strstr(mime, "mpeg");
    bool flac = (actual == QOBUZ_FORMAT_CD || actual == QOBUZ_FORMAT_HIRES96 ||
                 actual == QOBUZ_FORMAT_HIRES192) && strstr(mime, "flac");
    if ((job->format == QOBUZ_FORMAT_MP3 && !mp3) || (job->format != QOBUZ_FORMAT_MP3 && !flac)) return false;
    // Qobuz may serve a lower format than requested. Keep it under its actual
    // quality so a later hi-res request is not mistaken for a CD download.
    if (job->format == QOBUZ_FORMAT_HIRES192 && actual == QOBUZ_FORMAT_CD) job->quality_lowered = true;
    // Playback and the library accept paths of at most 511 bytes.
    char path[512], part[528], tags[528], tagpart[544];
    if (!album_folder(job, track)) return false;
    const char *suffix = mp3 ? ".mp3" : actual == QOBUZ_FORMAT_CD ? ".flac" : " (Hi-Res).flac";
    char artist[80], title[120], stem[210];
    podcastdl_safe_name(track->artist, "Unknown artist", artist, sizeof(artist));
    podcastdl_safe_name(track->title, "Unknown title", title, sizeof(title));
    snprintf(stem, sizeof(stem), "%s - %s", artist, title);
    // Track ids live in the sidecar, not in the user-visible filename. If two
    // distinct tracks have the same name, disambiguate without overwriting.
    bool found_slot = false;
    for (int n = 1; n <= 100; n++) {
        int written = n == 1 ? snprintf(path, sizeof(path), "%s/%s%s", job->album_dir, stem, suffix) :
            snprintf(path, sizeof(path), "%s/%s (%d)%s", job->album_dir, stem, n, suffix);
        if (written >= (int)sizeof(path)) return false;
        struct stat st;
        if (stat(path, &st)) { found_slot = true; break; }
        char sidecar[528], line[128];
        snprintf(sidecar, sizeof(sidecar), "%s.tags", path);
        FILE *f = fopen(sidecar, "r");
        bool matches = false;
        if (f) {
            while (fgets(line, sizeof(line), f))
                if (!strncmp(line, "qobuz_id=", 9) && strtol(line + 9, NULL, 10) == track->id) matches = true;
            fclose(f);
        }
        if (matches) { found_slot = true; break; }
    }
    if (!found_slot) return false;
    if (!job->cover_attempted) {
        job->cover_attempted = true;
        job->cover_failed = !fetch_cover(job, job->album_dir, track->cover);
    }
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
    pthread_mutex_lock(&lock);
    progress.bytes_total = opened ? stream->content_length64 : 0;
    progress.bytes_done = 0;
    pthread_mutex_unlock(&lock);
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
        pthread_mutex_lock(&lock);
        progress.bytes_done = done;
        pthread_mutex_unlock(&lock);
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
            tag_value(f, "album_artist", track->album_artist);
            tag_value(f, "year", track->released);
            fprintf(f, "track=%d\ndisc=%d\nqobuz_id=%ld\n", track->track_number, track->disc_number, track->id);
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
                    pthread_mutex_lock(&lock);
                    progress.total_tracks = tracks[i].album_track_count;
                    progress.bytes_done = progress.bytes_total = 0;
                    snprintf(progress.title, sizeof(progress.title), "%s", tracks[i].title);
                    pthread_mutex_unlock(&lock);
                    if (download(job, &tracks[i])) succeeded++; else failed++;
                    pthread_mutex_lock(&lock);
                    progress.completed_tracks = succeeded + failed;
                    progress.failed_tracks = failed;
                    progress.bytes_done = progress.bytes_total = 0;
                    pthread_mutex_unlock(&lock);
                }
                if (n < PAGE) break;
            }
            free(tracks);
        }
    } else if (download(job, &job->track)) succeeded++; else failed++;
    bool cover_failed = job->cover_failed;
    bool quality_lowered = job->quality_lowered;
    free(job);
    pthread_mutex_lock(&lock);
    running = false;
    progress.running = false;
    pthread_mutex_unlock(&lock);
    if (failed) gui_notify_popup(succeeded ? (quality_lowered ? "Some tracks failed; others used lower quality" : "Some Qobuz tracks could not be downloaded") : "Qobuz download failed");
    else if (quality_lowered) gui_notify_popup(cover_failed ? "Downloaded at lower quality; cover unavailable" : "Downloaded at lower quality than requested");
    else gui_notify_popup(cover_failed ? "Qobuz download complete; cover unavailable" : "Qobuz download complete");
    return NULL;
}

static bool start(const qobuz_track_t *track, const char *album, int format) {
    if ((format != QOBUZ_FORMAT_MP3 && format != QOBUZ_FORMAT_CD && format != QOBUZ_FORMAT_HIRES192) ||
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
    memset(&progress, 0, sizeof(progress));
    progress.running = true;
    progress.album = album && album[0];
    if (track) snprintf(progress.title, sizeof(progress.title), "%s", track->title);
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
        pthread_mutex_lock(&lock); running = false; progress.running = false; pthread_mutex_unlock(&lock);
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
// Read both the original flat layout and the new one-level album folders.
static void scan_history(const char *dir, qobuzdl_entry_t *entries, int max, int *count, bool albums) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char full[768];
        if (snprintf(full, sizeof(full), "%s/%s", dir, de->d_name) >= (int)sizeof(full)) continue;
        struct stat st;
        if (lstat(full, &st)) continue;
        if (albums && S_ISDIR(st.st_mode)) {
            scan_history(full, entries, max, count, false);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !st.st_size) continue;
        const char *ext = strrchr(de->d_name, '.');
        if (!ext || (strcasecmp(ext, ".mp3") && strcasecmp(ext, ".flac"))) continue;
        qobuzdl_entry_t entry = {0};
        snprintf(entry.path, sizeof(entry.path), "%s", full);
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
        if (!entry.title[0]) snprintf(entry.title, sizeof(entry.title), "%.190s", de->d_name);
        if (*count < max) entries[(*count)++] = entry;
        else {
            int oldest = 0;
            for (int i = 1; i < *count; i++) if (compare_entries(&entries[i], &entries[oldest]) > 0) oldest = i;
            if (compare_entries(&entry, &entries[oldest]) < 0) entries[oldest] = entry;
        }
    }
    closedir(d);
}

int qobuzdl_history(qobuzdl_entry_t *entries, int max) {
    if (!entries || max <= 0 || !storage_sd_root() || !storage_card_attached()) return 0;
    char dir[768];
    if (snprintf(dir, sizeof(dir), "%s/Music/Qobuz Downloads", storage_sd_root()) >= (int)sizeof(dir)) return 0;
    int count = 0;
    scan_history(dir, entries, max, &count, true);
    qsort(entries, count, sizeof(*entries), compare_entries);
    return count;
}
