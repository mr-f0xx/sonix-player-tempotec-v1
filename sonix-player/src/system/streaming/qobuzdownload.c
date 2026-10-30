#define _GNU_SOURCE
#include "qobuzdownload.h"
#include "streamturn.h"
#include "src/system/net/http.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static atomic_bool busy, cancelled;
typedef struct {
    char root[1024];
    qobuz_track_t track;
    int format;
    void (*done)(const char *);
} transfer_t;

bool qobuzdownload_busy(void) { return atomic_load(&busy); }
void qobuzdownload_cancel(void) { atomic_store(&cancelled, true); }

/* Metadata is untrusted: never allow it to form a path component. */
static void safe_name(const char *in, char *out, size_t size) {
    size_t n = 0;
    for (; *in && n + 1 < size; in++) {
        unsigned char c = (unsigned char)*in;
        out[n++] = c < 32 || strchr("/\\:*?\"<>|", c) ? '_' : c;
    }
    while (n && (out[n-1] == '.' || out[n-1] == ' ')) n--;
    if (!n) out[n++] = '_';
    out[n] = 0;
}

static void *worker(void *arg) {
    transfer_t *t = arg;
    char error[256] = "", url[QOBUZ_URL_MAX], mime[64];
    char dir[1100], path[1600], part[1620], artist[100], title[160];
    int fd = -1;
    bool opened = false, created = false;
    http_stream_t st;
    streamturn_acquire();
    if (atomic_load(&cancelled)) goto finish;
    if (!qobuz_track_url_format(t->track.id, t->format, url, sizeof(url), mime, sizeof(mime), NULL)) {
        snprintf(error, sizeof(error), "%s", qobuz_last_error());
        goto finish;
    }
    const char *ext = !strcmp(mime, "audio/mpeg") ? "mp3" :
        (!strcmp(mime, "audio/flac") || !strcmp(mime, "audio/x-flac")) ? "flac" : NULL;
    if (!ext) { snprintf(error, sizeof(error), "Unsupported Qobuz audio format"); goto finish; }
    snprintf(dir, sizeof(dir), "%s/Music", t->root);
    if (mkdir(dir, 0755) && errno != EEXIST) goto disk_error;
    snprintf(dir, sizeof(dir), "%s/Music/Qobuz", t->root);
    if (mkdir(dir, 0755) && errno != EEXIST) goto disk_error;
    safe_name(t->track.artist, artist, sizeof(artist));
    safe_name(t->track.title, title, sizeof(title));
    /* ID avoids collisions; format distinguishes MP3/CD/hi-res requests. */
    snprintf(path, sizeof(path), "%s/%s - %s [%ld-q%d].%s", dir, artist, title,
             t->track.id, t->format, ext);
    struct stat sb;
    if (!lstat(path, &sb)) { snprintf(error, sizeof(error), "File already exists; not overwritten"); goto finish; }
    snprintf(part, sizeof(part), "%s.part", path);
    fd = open(part, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) goto disk_error;
    created = true;
    http_stream_options_t options = {.no_icy = true, .no_https_downgrade = true};
    if (strncmp(url, "https://", 8)) { snprintf(error, sizeof(error), "Refusing non-HTTPS audio URL"); goto finish; }
    if (!http_stream_open_ex(&st, url, 15, &options)) {
        snprintf(error, sizeof(error), "%s", http_last_error()); goto finish;
    }
    opened = true;
    char buf[16384];
    uint64_t total = 0;
    int n;
    while (!atomic_load(&cancelled) && (n = http_stream_read(&st, buf, sizeof(buf))) > 0) {
        int pos = 0;
        while (pos < n) {
            ssize_t wrote = write(fd, buf + pos, n - pos);
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) goto disk_error;
            pos += wrote;
        }
        total += n;
    }
    if (atomic_load(&cancelled)) goto finish;
    if (n < 0 || !total || (st.content_length64 && total != st.content_length64)) {
        snprintf(error, sizeof(error), "Incomplete audio download"); goto finish;
    }
    if (fsync(fd)) goto disk_error;
    if (close(fd)) { fd = -1; goto disk_error; }
    fd = -1;
    if (rename(part, path)) goto disk_error;
    created = false;
    goto finish;
disk_error:
    snprintf(error, sizeof(error), "Music folder: %s", strerror(errno));
finish:
    if (opened) http_stream_close(&st);
    if (fd >= 0) close(fd);
    if (created) unlink(part);
    if (atomic_load(&cancelled)) snprintf(error, sizeof(error), "Download cancelled");
    streamturn_release();
    t->done(error[0] ? error : NULL);
    free(t);
    atomic_store(&busy, false);
    return NULL;
}

bool qobuzdownload_start(const char *root, const qobuz_track_t *track,
                        void (*done)(const char *)) {
    bool expected = false;
    if (!root || !*root || strlen(root) >= 1024 || !track || !track->streamable || !done ||
        !atomic_compare_exchange_strong(&busy, &expected, true)) return false;
    transfer_t *t = calloc(1, sizeof(*t));
    if (!t) { atomic_store(&busy, false); return false; }
    strcpy(t->root, root); t->track = *track; t->format = qobuz_get_format(); t->done = done;
    atomic_store(&cancelled, false);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int rc = pthread_create(&thread, &attr, worker, t);
    pthread_attr_destroy(&attr);
    if (rc) { free(t); atomic_store(&busy, false); return false; }
    return true;
}
