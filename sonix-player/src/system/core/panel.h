#ifndef PANEL_H
#define PANEL_H

#include <stdbool.h>

// Who owns the touchscreen.
//
// Declared here, defined in main.c -- that is where touch is opened -- with an
// empty version for the host build. Same shape as display_rotation_supported()
// and friends in power.h, which live there for the same reason (and
// display_get_rotated() is what a caller reading the panel itself needs:
// rotation has to be applied by hand, or the controls come out upside down).
//
// These exist for the emulator. A Game Boy is held with two thumbs and LVGL is
// not: its evdev indev is a pointer, a single contact, which cannot press the
// d-pad and A/B together. So while a game runs the panel changes owner: LVGL's
// indev is switched off, the emulator opens the same evdev node itself and
// reads the real multitouch protocol; on exit everything goes back.

// Switches LVGL's touch indev on and off. Off, LVGL does not read the node and
// moves nothing: the panel is free.
void panel_touch_enable(bool enabled);

// The evdev node LVGL's indev opened: the one that reports itself as a touch
// panel, or event1 and then event0 when none does. NULL when there is no touch
// device.
const char *panel_touch_device(void);

// Drains any events the touch panel queued while its indev was not being read,
// and makes the indev ignore the pointer until the finger is up, so wake-up
// transients or touches during screen-off do not trigger ghost clicks.
void panel_touch_flush(void);

// --- Direct framebuffer video mode ---
// Bypasses LVGL page-flip rendering so a video player can write RGB565 frames
// directly to /dev/fb0. Only available on the device's double-buffered fbdev
// display (not the host SDL/Web build). Enter before playing, leave when done.
bool display_video_supported(void);
bool display_video_enter(void);
void display_video_leave(void);
// Blit a video frame into the 240x320 portrait panel and present it.
// In landscape mode (default) the source is rotated 90° CW to fill the panel.
// In portrait mode the source is cover-fit directly to the panel.
// If overlay is non-NULL it is called with the panel buffer (pw x ph, tight
// stride) before panning, so the UI can draw controls in panel coordinates.
typedef void (*display_video_overlay_fn)(uint16_t *panel, int pw, int ph, int stride);
bool display_video_blit_rgb565(const uint16_t *src, int src_w, int src_h, int src_stride_px,
							   display_video_overlay_fn overlay);
void display_video_set_landscape(bool landscape);
bool display_video_is_landscape(void);
// Panel dimensions in portrait pixels.
int display_video_panel_w(void);
int display_video_panel_h(void);

#endif /* PANEL_H */
