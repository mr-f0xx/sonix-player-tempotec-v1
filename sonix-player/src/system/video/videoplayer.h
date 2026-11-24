#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	VP_STATE_STOPPED = 0,
	VP_STATE_PLAYING,
	VP_STATE_PAUSED,
	VP_STATE_ERROR,
} vp_state_t;

// Start playing a video file (MPEG-PS / MPEG-1 video). Stops any current
// playback. Returns true if playback started successfully.
bool videoplayer_play(const char *path);

// Stop playback and release resources.
void videoplayer_stop(void);

// Toggle pause/resume.
void videoplayer_toggle_pause(void);
void videoplayer_pause(void);
void videoplayer_resume(void);

// Seek by seconds relative to current position (negative = back).
void videoplayer_seek(double delta_sec);

// Seek to absolute position in seconds.
void videoplayer_seek_to(double sec);

vp_state_t videoplayer_get_state(void);
double videoplayer_get_position(void);
double videoplayer_get_duration(void);
void videoplayer_get_current_file(char *out, size_t out_size);
bool videoplayer_is_active(void);

// Overlay hook: called before panning with the panel buffer (240x320 portrait,
// tight stride) so the UI can draw controls in panel coordinates.
typedef void (*videoplayer_overlay_fn)(uint16_t *panel, int pw, int ph, int stride);
void videoplayer_set_overlay_callback(videoplayer_overlay_fn fn);

// Called from the main/UI thread to pump overlay updates if needed.
// The video decode/blit runs on its own thread.
void videoplayer_tick(void);

#endif /* VIDEOPLAYER_H */
