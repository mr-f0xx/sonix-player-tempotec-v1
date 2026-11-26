#ifndef REPLAYGAIN_H
#define REPLAYGAIN_H

#include <stdbool.h>
#include <stdint.h>

#include "src/system/library/metadata.h"

// ReplayGain: playing a whole card at one loudness.
//
// Whoever tagged the file worked out its loudness once and wrote the
// correction into the tags; the player applies it. Nothing is analysed here --
// an analysis pass over a card of music is minutes of work this device should
// not be doing.
//
// Four states, the same three the stock player offers and an extra option
// (`settings_replaygain_vg_off` / `_track` / `_album` in its binary): off, per
// track, per album, track when shuffled. Track and album are not degrees of 
// strictness: correcting each track of a record separately flattens a movement 
// the producer meant to be quiet, so an album is corrected by one figure and a 
// shuffle by each track's own.
// 'Track when shuffled' uses track gain if shuffle mode is on, album gain 
// otherwise.

typedef enum {
	REPLAYGAIN_OFF = 0,
	REPLAYGAIN_TRACK = 1,
	REPLAYGAIN_ALBUM = 2,
	REPLAYGAIN_TRACK_WHEN_SHUFFLED = 3,
} replaygain_mode_t;

// Apple Sound Check (`iTunNORM`), mutually exclusive with ReplayGain:
//   - Adaptive preserves volume differences between tracks on an album (using
//     an album-wide Sound Check gain when supplied), switching to per-track
//     gain when shuffle is active or when tracks are played from a playlist;
//   - Consistent applies each track's own Sound Check gain unconditionally.
typedef enum {
	SOUNDCHECK_OFF = 0,
	SOUNDCHECK_ADAPTIVE = 1,
	SOUNDCHECK_CONSISTENT = 2,
} soundcheck_mode_t;

replaygain_mode_t replaygain_mode(void);
void replaygain_set_mode(replaygain_mode_t mode);

soundcheck_mode_t soundcheck_mode(void);
void soundcheck_set_mode(soundcheck_mode_t mode);

// Supplies (or clears, when `has_album_gain` is false) the uniform album Sound
// Check gain and peak for the album currently queued, so Adaptive mode can
// preserve relative loudness across tracks of an album.
void soundcheck_set_album_gain(bool has_album_gain, float gain_db, float peak);

// Called at each track change with what the tags said. Works out the gain once
// and hands it to the audio thread; a file with no usable ReplayGain / Sound
// Check tags leaves playback untouched, whatever the mode.
void replaygain_load(const song_metadata_t *meta);

// True when Sound Check (rather than ReplayGain) is the active normalization
// mode selected in settings.
bool soundcheck_enabled(void);

// What is being applied right now, in dB, and whether anything is. For the
// interface to be able to say so.
bool replaygain_active(void);
double replaygain_current_db(void);

// Works out the Q15 ReplayGain / Sound Check multiplier (32768 = unity) for
// `meta` under the active mode, without modifying the loaded track's global
// gain. `soundcheck_compute_q15_with_album` overrides the album Sound Check
// figure explicitly (used when crossfade stages a track on a chained next
// album before the queue itself moves).
int replaygain_compute_q15(const song_metadata_t *meta);
int soundcheck_compute_q15_with_album(const song_metadata_t *meta, bool in_album_queue, bool has_album_gain,
									  float album_gain_db, float album_peak);

// The peak the correction has to stay clear of, for the track or the album
// pair: the true (inter-sample) peak when the file carries one, otherwise the
// sample peak. 0 when the file says neither. So the details screen can show
// the figure the gain was actually held back from.
double replaygain_effective_peak(const song_metadata_t *meta, bool album);

// In place on interleaved PCM, audio thread only. A no-op when off or when the
// track carries no usable tag.
void replaygain_process(short *frames, int frame_count, int channels);
void replaygain_process_s32(int32_t *frames, int frame_count, int channels);

// Explicit-gain variants for mixing two tracks with independent ReplayGain.
void replaygain_apply_q15_s16(short *frames, int frame_count, int channels, int gain_q15);
void replaygain_apply_q15_s32(int32_t *frames, int frame_count, int channels, int gain_q15);

#endif /* REPLAYGAIN_H */
