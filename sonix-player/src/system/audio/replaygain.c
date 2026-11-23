#include "replaygain.h"

#include "src/system/core/config.h"
#include "src/system/playback/playlist.h"

#include <math.h>
#include <stdio.h>

// See replaygain.h for what this is and why nothing is analysed here.

// Bounds on the correction. A tag asking for more than +12 dB is either wrong
// or a very quiet recording; either way that much boost is dangerous once the
// next, untagged track starts.
#define GAIN_MAX_DB 12.0
#define GAIN_MIN_DB (-30.0)

// The gain, in Q15: 32768 is untouched. Read on the audio thread, written on
// the interface thread at each track change -- a single aligned int, and the
// worst a torn read could do is apply the previous track's gain to one block.
static volatile int gain_q15 = 32768;
static volatile int active;
static volatile double current_db;

replaygain_mode_t replaygain_mode(void) {
	long value = config_get_int("audio", "replaygain", REPLAYGAIN_OFF);
	if (value != REPLAYGAIN_TRACK && value != REPLAYGAIN_ALBUM && value != REPLAYGAIN_TRACK_WHEN_SHUFFLED) {
		return REPLAYGAIN_OFF;
	}
	return (replaygain_mode_t)value;
}

void replaygain_set_mode(replaygain_mode_t mode) {
	config_set_int("audio", "replaygain", (long)mode);
	config_save();
}

// The peak the correction has to stay clear of. A file tagged with a true
// (inter-sample) peak is protected by that figure: the sample peak can read
// below full scale while the signal between samples overshoots it, and the
// overshoot is what clips. A file with both is protected by the larger -- a
// tag can contradict itself, and the larger is the safe reading.
static double peak_effective(double sample_peak, double true_peak) {
	return true_peak > sample_peak ? true_peak : sample_peak;
}

static int compute_gain(const song_metadata_t *meta, replaygain_mode_t *resolved_out, double *db_out, double *peak_out) {
	replaygain_mode_t mode = replaygain_mode();
	if (resolved_out) {
		*resolved_out = mode;
	}
	if (db_out) {
		*db_out = 0.0;
	}
	if (peak_out) {
		*peak_out = 0.0;
	}
	if (!meta || mode == REPLAYGAIN_OFF) {
		return 32768;
	}

	if (mode == REPLAYGAIN_TRACK_WHEN_SHUFFLED) {
		playback_mode_t playback_mode = (playback_mode_t)config_get_int("player", "playback_mode", 0);
		if (playback_mode == PLAYBACK_MODE_SHUFFLE || playback_mode == PLAYBACK_MODE_SHUFFLE_REPEAT) {
			mode = REPLAYGAIN_TRACK;
		} else {
			mode = REPLAYGAIN_ALBUM;
		}
	}
	if (resolved_out) {
		*resolved_out = mode;
	}

	double db;
	double peak;
	if (mode == REPLAYGAIN_ALBUM && meta->has_album_gain) {
		db = meta->album_gain_db;
		peak = peak_effective(meta->album_peak, meta->album_true_peak);
	} else if (meta->has_track_gain) {
		// Album mode falls back to the track figure rather than to nothing: a
		// file tagged by a track-only tagger is still better corrected than
		// left alone, and it is what every other player does.
		db = meta->track_gain_db;
		peak = peak_effective(meta->track_peak, meta->track_true_peak);
	} else if (meta->has_album_gain) {
		db = meta->album_gain_db;
		peak = peak_effective(meta->album_peak, meta->album_true_peak);
	} else {
		return 32768; // nothing to go on
	}

	if (db > GAIN_MAX_DB) {
		db = GAIN_MAX_DB;
	}
	if (db < GAIN_MIN_DB) {
		db = GAIN_MIN_DB;
	}

	double factor = pow(10.0, db / 20.0);

	// The peak keeps the correction from clipping: a track measured at 0.95
	// full scale takes about half a decibel before the loudest sample folds
	// over, and the fold-over is more audible than the loudness difference.
	// With a true peak tag, `peak` is the inter-sample peak, which is the
	// figure that actually decides it.
	if (peak > 0.0 && factor * peak > 1.0) {
		factor = 1.0 / peak;
		db = 20.0 * log10(factor);
	}

	int q = (int)(factor * 32768.0 + 0.5);
	if (q < 1) {
		q = 1;
	}
	if (q > 32768 * 4) {
		q = 32768 * 4; // +12 dB, the ceiling above, expressed in the same units
	}

	if (db_out) {
		*db_out = db;
	}
	if (peak_out) {
		*peak_out = peak;
	}
	return q;
}

int replaygain_compute_q15(const song_metadata_t *meta) {
	return compute_gain(meta, NULL, NULL, NULL);
}

double replaygain_effective_peak(const song_metadata_t *meta, bool album) {
	if (!meta) {
		return 0.0;
	}
	if (album) {
		return peak_effective(meta->album_peak, meta->album_true_peak);
	}
	return peak_effective(meta->track_peak, meta->track_true_peak);
}

void replaygain_load(const song_metadata_t *meta) {
	replaygain_mode_t mode = REPLAYGAIN_OFF;
	double db = 0.0;
	double peak = 0.0;
	int q = compute_gain(meta, &mode, &db, &peak);

	gain_q15 = q;
	current_db = (q != 32768) ? db : 0.0;
	active = (q != 32768);

	if (active) {
		printf("replaygain: %s %+.2f dB (peak %.4f)\n", mode == REPLAYGAIN_ALBUM ? "album" : "track", db, peak);
	}
}

bool replaygain_active(void) { return active != 0; }
double replaygain_current_db(void) { return current_db; }

void replaygain_apply_q15_s16(short *frames, int frame_count, int channels, int g) {
	if (g == 32768 || frame_count <= 0 || channels <= 0 || !frames) {
		return;
	}

	int samples = frame_count * channels;
	for (int i = 0; i < samples; i++) {
		// The product can exceed 16 bits when the gain is above unity. The peak
		// check above makes that rare, not impossible: a tag can lie about its
		// own peak.
		int v = (frames[i] * g) >> 15;
		if (v > 32767) {
			v = 32767;
		} else if (v < -32768) {
			v = -32768;
		}
		frames[i] = (short)v;
	}
}

void replaygain_apply_q15_s32(int32_t *frames, int frame_count, int channels, int gain) {
	int64_t g = gain;
	if (g == 32768 || frame_count <= 0 || channels <= 0 || !frames) {
		return;
	}

	int samples = frame_count * channels;
	for (int i = 0; i < samples; i++) {
		int64_t v = ((int64_t)frames[i] * g) >> 15;
		if (v > INT32_MAX) {
			v = INT32_MAX;
		} else if (v < INT32_MIN) {
			v = INT32_MIN;
		}
		frames[i] = (int32_t)v;
	}
}

void replaygain_process(short *frames, int frame_count, int channels) {
	if (!active) {
		return;
	}
	replaygain_apply_q15_s16(frames, frame_count, channels, gain_q15);
}

void replaygain_process_s32(int32_t *frames, int frame_count, int channels) {
	if (!active) {
		return;
	}
	replaygain_apply_q15_s32(frames, frame_count, channels, gain_q15);
}
