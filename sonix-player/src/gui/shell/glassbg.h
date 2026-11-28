#ifndef GLASSBG_H
#define GLASSBG_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// The Liquid Glass background: the supplied abstract-glass art by default, or
// a picture of the user's own from a folder named "Wallpaper" in the root of
// the card (beside Bootlogo and Screensaver). The chosen picture is decoded,
// fitted to the panel and kept in memory as the wallpaper every glass surface
// shows -- the bottom layer under the menus, the control centre and the
// top-layer pages alike. A dim control darkens or lightens it everywhere at
// once. Only Liquid Glass uses any of this; every other theme is opaque.
//
// The choice is a file NAME in the folder, kept in the settings, so it follows
// the card: a card swapped while the player runs is read again, and a picture
// that has since been removed falls back to the supplied art rather than
// leaving the glass bare.

#define GLASSBG_NAME_MAX 255

// The dim range, as a percentage of black laid over the picture. 0 is the
// picture at full strength (lightest); the default sits dark enough for white
// labels to read; the top of the range is nearly black.
#define GLASSBG_DIM_DEFAULT 70
#define GLASSBG_DIM_MAX 90

// Read the saved choice and dim. Call once at startup, before glass_init().
void glassbg_init(void);

// Where the card is mounted. Called at startup and again when a card is
// attached; passing NULL or "" leaves the folder unreachable. Safe to call off
// the interface thread: it only caches the directory and asks the UI thread to
// read the chosen picture again.
void glassbg_set_root(const char *card_root);

// The wallpaper every glass surface should draw: the custom picture when one
// is loaded, the supplied art otherwise. Never NULL.
const lv_image_dsc_t *glassbg_source(void);

// The dim as an LVGL opacity (black recolor over the picture).
lv_opa_t glassbg_dim_opa(void);

// The dim as a percentage, for the slider.
int glassbg_dim_percent(void);

// Set the dim (clamped to the range), save it and repaint every glass surface.
// Interface thread.
void glassbg_set_dim(int percent);

// The same without saving: the live preview while the slider is dragged. The
// flash write waits for the finger to lift (glassbg_set_dim). Interface thread.
void glassbg_preview_dim(int percent);

// The chosen picture's file name, or "" when the supplied art is in use.
const char *glassbg_current_name(void);

// The folder itself, or "" when there is no card. Shown on the picker page.
const char *glassbg_dir(void);

// The pictures in the folder, as file names, alphabetical and without their
// path, up to `max` of them. Returns how many were found.
int glassbg_list(char names[][GLASSBG_NAME_MAX + 1], int max);

// Decode one picture to a panel-sized RGB565 buffer (w*h*2 bytes, malloc'd,
// caller frees). NULL when the file cannot be read or decoded. Touches no LVGL
// state, so it is safe to run off the interface thread.
uint8_t *glassbg_decode_file(const char *path, int panel_w, int panel_h);

// Make a decoded buffer the wallpaper: takes ownership of `px`, saves `name`
// as the choice and repaints. Interface thread. A NULL `px` is ignored.
void glassbg_install(uint8_t *px, int panel_w, int panel_h, const char *name);

// Back to the supplied art: forget the choice, free the picture, repaint.
// Interface thread.
void glassbg_reset(void);

// Read the chosen picture if one is pending (startup, or a card change).
// Called from the glass theme refresh on the interface thread.
void glassbg_sync(void);

#endif /* GLASSBG_H */
