#define PL_MPEG_IMPLEMENTATION
#include "src/system/video/pl_mpeg.h"

#include "videoplayer.h"

#if defined(__has_include)
#  if __has_include(<alsa/asoundlib.h>)
#    include <alsa/asoundlib.h>
#    define VP_HAS_ALSA 1
#  endif
#endif
#ifndef VP_HAS_ALSA
#  define VP_HAS_ALSA 0
typedef void snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
#define SND_PCM_FORMAT_S16_LE 0
#define SND_PCM_ACCESS_RW_INTERLEAVED 0
#define SND_PCM_STREAM_PLAYBACK 0
static inline int snd_pcm_open(snd_pcm_t **p, const char *n, int s, int m){(void)p;(void)n;(void)s;(void)m;return -1;}
static inline int snd_pcm_set_params(snd_pcm_t *p,int f,int a,unsigned c,unsigned r,int sr,unsigned lat){(void)p;(void)f;(void)a;(void)c;(void)r;(void)sr;(void)lat;return -1;}
static inline snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *p,const void *b,snd_pcm_uframes_t s){(void)p;(void)b;(void)s;return -1;}
static inline int snd_pcm_recover(snd_pcm_t *p,int e,int s){(void)p;(void)e;(void)s;return -1;}
static inline int snd_pcm_delay(snd_pcm_t *p,snd_pcm_sframes_t *d){(void)p;(void)d;return -1;}
static inline int snd_pcm_drop(snd_pcm_t *p){(void)p;return 0;}
static inline int snd_pcm_close(snd_pcm_t *p){(void)p;return 0;}
static inline int snd_pcm_pause(snd_pcm_t *p,int en){(void)p;(void)en;return 0;}
static inline int snd_pcm_prepare(snd_pcm_t *p){(void)p;return 0;}
static inline const char *snd_strerror(int e){(void)e;return "no alsa";}
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/core/panel.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static pthread_t vp_thread;
static volatile bool vp_thread_running;
static volatile bool vp_stop_requested;
static volatile vp_state_t vp_state = VP_STATE_STOPPED;

static pthread_mutex_t vp_lock = PTHREAD_MUTEX_INITIALIZER;

static char vp_path[1024];
static double vp_duration;
static volatile double vp_position;
static volatile double vp_seek_request; // >=0 means seek to this absolute sec
static volatile bool vp_seek_pending;
static volatile int vp_pause_req; // 0=none,1=pause,2=resume,3=toggle

// ALSA audio output owned by the video player
static snd_pcm_t *vp_pcm;
static unsigned vp_rate;
static int vp_channels;
static volatile double vp_audio_clock; // seconds played out
static uint64_t vp_samples_written;
static double vp_audio_base; // absolute seconds corresponding to samples_written==0

// Latest decoded video frame converted to RGB565 (320x240 or source size)
static uint16_t *vp_rgb565;
static int vp_frame_w;
static int vp_frame_h;
static volatile bool vp_frame_ready;
static double vp_frame_time;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double now_sec(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static void vp_lock_fn(void) { pthread_mutex_lock(&vp_lock); }
static void vp_unlock_fn(void) { pthread_mutex_unlock(&vp_lock); }

static void close_pcm(void) {
	if (vp_pcm) {
		snd_pcm_drop(vp_pcm);
		snd_pcm_close(vp_pcm);
		vp_pcm = NULL;
	}
}

static bool open_pcm(unsigned rate, int channels) {
	close_pcm();
	int err = snd_pcm_open(&vp_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "videoplayer: snd_pcm_open failed: %s\n", snd_strerror(err));
		vp_pcm = NULL;
		return false;
	}
	err = snd_pcm_set_params(vp_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
				 channels, rate, 1 /* soft_resample */, 200000 /* latency us */);
	if (err < 0) {
		fprintf(stderr, "videoplayer: snd_pcm_set_params failed: %s\n", snd_strerror(err));
		close_pcm();
		return false;
	}
	vp_rate = rate;
	vp_channels = channels;
	vp_samples_written = 0;
	vp_audio_base = 0.0;
	vp_audio_clock = 0.0;
	return true;
}

static void update_audio_clock(void) {
	if (!vp_pcm || vp_rate == 0) return;
	snd_pcm_sframes_t delay = 0;
	if (snd_pcm_delay(vp_pcm, &delay) < 0 || delay < 0) delay = 0;
	double played = (double)vp_samples_written - (double)delay;
	if (played < 0) played = 0;
	vp_audio_clock = vp_audio_base + played / (double)vp_rate;
}

static int16_t clamp_s16(float v) {
	if (v > 1.0f) v = 1.0f;
	if (v < -1.0f) v = -1.0f;
	return (int16_t)(v * 32767.0f);
}

// ---------------------------------------------------------------------------
// YUV420 -> RGB565
// ---------------------------------------------------------------------------

static inline uint16_t rgb565(int r, int g, int b) {
	if (r < 0) r = 0;
	if (r > 255) r = 255;
	if (g < 0) g = 0;
	if (g > 255) g = 255;
	if (b < 0) b = 0;
	if (b > 255) b = 255;
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void yuv420_to_rgb565(const plm_frame_t *frame, uint16_t *dst, int dst_stride) {
	const int w = frame->width;
	const int h = frame->height;
	for (int y = 0; y < h; y++) {
		int cy = y >> 1;
		const uint8_t *yrow = frame->y.data + y * frame->y.width;
		const uint8_t *cbrow = frame->cb.data + cy * frame->cb.width;
		const uint8_t *crrow = frame->cr.data + cy * frame->cr.width;
		uint16_t *drow = dst + y * dst_stride;
		for (int x = 0; x < w; x++) {
			int cx = x >> 1;
			int Y = (int)yrow[x] - 16;
			if (Y < 0) Y = 0;
			int Cb = (int)cbrow[cx] - 128;
			int Cr = (int)crrow[cx] - 128;
			// Integer BT.601
			int y1192 = 1192 * Y;
			int r = (y1192 + 1634 * Cr) >> 10;
			int g = (y1192 - 401 * Cb - 833 * Cr) >> 10;
			int b = (y1192 + 2066 * Cb) >> 10;
			drow[x] = rgb565(r, g, b);
		}
	}
}

// ---------------------------------------------------------------------------
// pl_mpeg callbacks
// ---------------------------------------------------------------------------

static void vp_audio_cb(plm_t *plm, plm_samples_t *samples, void *user) {
	(void)plm; (void)user;
	if (!samples || vp_stop_requested) return;
	if (!vp_pcm) {
		if (!open_pcm((unsigned)samples->count > 0 ? plm_get_samplerate(plm) : 44100, 2)) {
			// Fallback: try 44100 stereo
			if (!open_pcm(44100, 2)) return;
		}
	}

	// Convert float interleaved stereo to S16
	const int count = samples->count; // samples per channel
	int16_t *buf = (int16_t *)malloc((size_t)count * 2 * sizeof(int16_t));
	if (!buf) return;
	for (int i = 0; i < count; i++) {
		buf[i * 2 + 0] = clamp_s16(samples->interleaved[i * 2 + 0]);
		buf[i * 2 + 1] = clamp_s16(samples->interleaved[i * 2 + 1]);
	}

	int16_t *p = buf;
	int remaining = count;
	while (remaining > 0 && !vp_stop_requested) {
		snd_pcm_sframes_t wr = snd_pcm_writei(vp_pcm, p, (snd_pcm_uframes_t)remaining);
		if (wr < 0) {
			wr = snd_pcm_recover(vp_pcm, (int)wr, 0);
			if (wr < 0) {
				// Try reopen once
				close_pcm();
				if (!open_pcm(vp_rate ? vp_rate : 44100, 2)) break;
				continue;
			}
			continue;
		}
		p += wr * 2;
		remaining -= (int)wr;
		vp_samples_written += (uint64_t)wr;
		update_audio_clock();
	}
	free(buf);
}

static void vp_video_cb(plm_t *plm, plm_frame_t *frame, void *user) {
	(void)user;
	if (!frame || vp_stop_requested) return;

	double pts = frame->time;
	vp_position = pts;

	// Sync: if we have audio, compare to audio clock.
	// Never block here: callbacks run inside plm_decode, so sleeping would
	// starve audio decoding. ALSA write blocking provides backpressure.
	bool has_audio = plm_get_num_audio_streams(plm) > 0 && vp_pcm != NULL;
	if (has_audio) {
		update_audio_clock();
		double diff = pts - vp_audio_clock; // positive = video ahead
		if (diff < -0.10) {
			// Video behind audio: skip frame to catch up.
			return;
		}
		// If ahead, render anyway; ALSA backpressure paces decoding.
	} else {
		// No audio: pace by PTS vs wall clock.
		static double wall_base = 0;
		static double pts_base = 0;
		static bool have_base = false;
		if (!have_base) { wall_base = now_sec(); pts_base = pts; have_base = true; }
		double target = wall_base + (pts - pts_base);
		double now = now_sec();
		if (now < target) {
			double wait = target - now;
			if (wait > 0.05) wait = 0.05;
			usleep((useconds_t)(wait * 1000000.0));
		} else if (now - target > 0.15) {
			wall_base = now_sec();
			pts_base = pts;
		}
	}

	if (vp_stop_requested) return;

	// Convert YUV420 -> RGB565
	int w = frame->width;
	int h = frame->height;
	if (w <= 0 || h <= 0) return;

	// Ensure buffer
	vp_lock_fn();
	if (!vp_rgb565 || vp_frame_w != w || vp_frame_h != h) {
		free(vp_rgb565);
		vp_rgb565 = (uint16_t *)malloc((size_t)w * (size_t)h * sizeof(uint16_t));
		vp_frame_w = w;
		vp_frame_h = h;
	}
	if (vp_rgb565) {
		yuv420_to_rgb565(frame, vp_rgb565, w);
		vp_frame_time = pts;
		vp_frame_ready = true;
	}
	vp_unlock_fn();
}

// ---------------------------------------------------------------------------
// Decode thread
// ---------------------------------------------------------------------------

static void *vp_thread_fn(void *arg) {
	(void)arg;

	vp_lock_fn();
	char path[sizeof(vp_path)];
	snprintf(path, sizeof(path), "%s", vp_path);
	vp_unlock_fn();

	plm_t *plm = plm_create_with_filename(path);
	if (!plm) {
		fprintf(stderr, "videoplayer: failed to open %s\n", path);
		vp_lock_fn();
		vp_state = VP_STATE_ERROR;
		vp_thread_running = false;
		vp_unlock_fn();
		return NULL;
	}

	plm_set_video_decode_callback(plm, vp_video_cb, NULL);
	plm_set_audio_decode_callback(plm, vp_audio_cb, NULL);
	plm_set_loop(plm, 0);

	int has_audio = plm_get_num_audio_streams(plm) > 0;
	if (has_audio) {
		int sr = plm_get_samplerate(plm);
		if (sr <= 0) sr = 44100;
		open_pcm((unsigned)sr, 2);
		plm_set_audio_enabled(plm, 1);
		plm_set_audio_stream(plm, 0);
	} else {
		plm_set_audio_enabled(plm, 0);
	}

	vp_duration = plm_get_duration(plm);
	double fr = plm_get_framerate(plm);
	if (fr <= 0.0) fr = 25.0;
	double tick = 1.0 / fr;
	if (tick < 0.01) tick = 0.01;
	if (tick > 0.05) tick = 0.05;

	vp_lock_fn();
	vp_state = VP_STATE_PLAYING;
	vp_unlock_fn();

	// For no-audio pacing
	double wall_base = now_sec();
	double pts_base = 0.0;
	bool have_base = false;
	bool paused = false;

	while (!vp_stop_requested) {
		// Handle pause toggle/requests
		int pr = vp_pause_req;
		if (pr != 0) {
			vp_pause_req = 0;
			if (pr == 1) paused = true;
			else if (pr == 2) paused = false;
			else if (pr == 3) paused = !paused;
			vp_lock_fn();
			vp_state = paused ? VP_STATE_PAUSED : VP_STATE_PLAYING;
			vp_unlock_fn();
			if (paused && vp_pcm) {
				snd_pcm_drop(vp_pcm);
			}
			if (!paused && vp_pcm) {
				snd_pcm_prepare(vp_pcm);
				vp_samples_written = 0;
				vp_audio_base = vp_position;
				vp_audio_clock = vp_position;
				have_base = false;
			}
		}

		if (paused) {
			usleep(50000);
			continue;
		}

		// Handle seek
		if (vp_seek_pending) {
			double target = vp_seek_request;
			vp_seek_pending = false;
			if (target < 0) target = 0;
			if (vp_duration > 0 && target > vp_duration) target = vp_duration;
			if (vp_pcm) {
				snd_pcm_drop(vp_pcm);
				snd_pcm_prepare(vp_pcm);
				vp_samples_written = 0;
				vp_audio_base = target;
				vp_audio_clock = target;
			}
			plm_seek(plm, target, 1);
			vp_position = target;
			have_base = false;
		}

		if (plm_has_ended(plm)) {
			break;
		}

		plm_decode(plm, tick);

		// If no audio, pace ourselves roughly
		if (!has_audio) {
			double pts = plm_get_time(plm);
			vp_position = pts;
			if (!have_base) { wall_base = now_sec(); pts_base = pts; have_base = true; }
			double target = wall_base + (pts - pts_base);
			double now = now_sec();
			if (now < target) {
				double wait = target - now;
				if (wait > 0.05) wait = 0.05;
				usleep((useconds_t)(wait * 1000000.0));
			}
		} else {
			// With audio, the audio callback does blocking writes which pace us.
			// Just yield a tiny bit to avoid spinning when ahead.
			update_audio_clock();
		}
	}

	close_pcm();
	plm_destroy(plm);

	vp_lock_fn();
	free(vp_rgb565);
	vp_rgb565 = NULL;
	vp_frame_w = vp_frame_h = 0;
	vp_frame_ready = false;
	if (vp_state != VP_STATE_ERROR) vp_state = VP_STATE_STOPPED;
	vp_thread_running = false;
	vp_unlock_fn();

	return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool videoplayer_play(const char *path) {
	if (!path || !path[0]) return false;
	if (!display_video_supported()) {
		fprintf(stderr, "videoplayer: direct video display not supported\n");
		return false;
	}

	// Stop existing playback
	videoplayer_stop();

	// Stop music/audio system to free ALSA device
	audio_stop();
	// Give audio thread a moment to release device
	for (int i = 0; i < 50 && audio_device_is_open(); i++) {
		usleep(20000);
	}

	vp_lock_fn();
	snprintf(vp_path, sizeof(vp_path), "%s", path);
	vp_duration = 0;
	vp_position = 0;
	vp_seek_pending = false;
	vp_pause_req = 0;
	vp_stop_requested = false;
	vp_thread_running = true;
	vp_state = VP_STATE_PLAYING;
	vp_frame_ready = false;
	vp_unlock_fn();

	if (pthread_create(&vp_thread, NULL, vp_thread_fn, NULL) != 0) {
		vp_lock_fn();
		vp_thread_running = false;
		vp_state = VP_STATE_ERROR;
		vp_unlock_fn();
		return false;
	}
	return true;
}

void videoplayer_stop(void) {
	if (!vp_thread_running && vp_state == VP_STATE_STOPPED) {
		return;
	}
	vp_stop_requested = true;
	if (vp_thread_running) {
		pthread_join(vp_thread, NULL);
	}
	vp_thread_running = false;
	close_pcm();
	vp_lock_fn();
	free(vp_rgb565);
	vp_rgb565 = NULL;
	vp_frame_w = vp_frame_h = 0;
	vp_frame_ready = false;
	vp_state = VP_STATE_STOPPED;
	vp_position = 0;
	vp_duration = 0;
	vp_path[0] = '\0';
	vp_unlock_fn();
}

void videoplayer_toggle_pause(void) {
	if (!vp_thread_running) return;
	vp_pause_req = 3;
}

void videoplayer_pause(void) {
	if (!vp_thread_running) return;
	vp_pause_req = 1;
}

void videoplayer_resume(void) {
	if (!vp_thread_running) return;
	vp_pause_req = 2;
}

void videoplayer_seek(double delta_sec) {
	if (!vp_thread_running) return;
	double target = vp_position + delta_sec;
	vp_seek_request = target;
	vp_seek_pending = true;
}

void videoplayer_seek_to(double sec) {
	if (!vp_thread_running) return;
	vp_seek_request = sec;
	vp_seek_pending = true;
}

vp_state_t videoplayer_get_state(void) {
	return vp_state;
}

double videoplayer_get_position(void) {
	double pos = vp_position;
	if (vp_pcm && vp_rate > 0) {
		update_audio_clock();
		pos = vp_audio_clock;
	}
	if (pos < 0) pos = 0;
	if (vp_duration > 0 && pos > vp_duration) pos = vp_duration;
	return pos;
}

double videoplayer_get_duration(void) {
	return vp_duration;
}

void videoplayer_get_current_file(char *out, size_t out_size) {
	if (!out || out_size == 0) return;
	vp_lock_fn();
	snprintf(out, out_size, "%s", vp_path);
	vp_unlock_fn();
}

bool videoplayer_is_active(void) {
	return vp_thread_running || vp_state == VP_STATE_PLAYING || vp_state == VP_STATE_PAUSED;
}

static videoplayer_overlay_fn vp_overlay_cb;

void videoplayer_set_overlay_callback(videoplayer_overlay_fn fn) {
	vp_overlay_cb = fn;
}

void videoplayer_tick(void) {
	// Blit latest frame if ready. Also blit when overlay is active so the
	// overlay (time/scrub) stays responsive even while paused.
	if (!vp_thread_running) return;
	vp_lock_fn();
	if (vp_rgb565 && vp_frame_w > 0 && vp_frame_h > 0 && (vp_frame_ready || vp_overlay_cb)) {
		int w = vp_frame_w, h = vp_frame_h;
		size_t sz = (size_t)w * (size_t)h * sizeof(uint16_t);
		uint16_t *copy = (uint16_t *)malloc(sz);
		if (copy) {
			memcpy(copy, vp_rgb565, sz);
			vp_frame_ready = false;
			vp_unlock_fn();
			display_video_blit_rgb565(copy, w, h, w, (display_video_overlay_fn)vp_overlay_cb);
			free(copy);
			return;
		}
		vp_frame_ready = false;
	}
	vp_unlock_fn();
}

