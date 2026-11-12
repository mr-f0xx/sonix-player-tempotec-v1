#ifndef SOUNDCLOUDCACHE_H
#define SOUNDCLOUDCACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "soundcloud.h"

// Bounded on-card cache for SoundCloud streams:
//   <sd_root>/.local/soundcloud-cache/<track_id>.<ext>
//   <sd_root>/.local/soundcloud-cache/<track_id>.<ext>.tags
//   <sd_root>/.local/soundcloud-cache/<track_id>.jpg
//
// Progressive MP3, HLS MP3 and HLS Opus tracks announce via growfile_announce()
// and begin decoding as soon as the initial prebuffer arrives while a detached
// background thread completes the download. Go+ fMP4 AAC segments are unwrapped
// into standard ADTS .aac files before decoder_open().

#define SOUNDCLOUDCACHE_MAX_BYTES (512LL * 1024 * 1024)
#define SOUNDCLOUDCACHE_MIN_FREE (256LL * 1024 * 1024)

void soundcloudcache_set_root(const char *sd_root);
const char *soundcloudcache_dir(void);
bool soundcloudcache_available(void);
const char *soundcloudcache_last_error(void);

bool soundcloudcache_get(const soundcloud_track_t *track, char *out_path, size_t out_size);
bool soundcloudcache_prefetch(const soundcloud_track_t *track);

void soundcloudcache_cancel(void);
bool soundcloudcache_downloading(void);
long long soundcloudcache_current_id(void);

bool soundcloudcache_is_soundcloud_path(const char *path);
long long soundcloudcache_track_id_of(const char *path);
bool soundcloudcache_placeholder_path(long long id, char *out_path, size_t out_size);
bool soundcloudcache_find_playable(long long id, char *out_path, size_t out_size);

void soundcloudcache_protect(const char *path);
void soundcloudcache_clear(void);
void soundcloudcache_clear_on_exit(void);
void soundcloudcache_stats(int *files, long long *bytes);

// Test hook: override aacdec_available() (-1 = auto, 0 = force off, 1 = force on).
void soundcloudcache_set_aac_override_for_test(int mode);

#endif /* SOUNDCLOUDCACHE_H */
