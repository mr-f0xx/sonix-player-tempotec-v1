#include "corners.h"

#include <stdint.h>

#include "lvgl/lvgl.h"
// lv_image_cache_drop() moved here in LVGL 9.2; lvgl.h no longer pulls it in.
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include "src/gui/board_profile.h"
#include "src/system/core/config.h"

// HiBy 480-wide panels keep the original 28 px arc. The V1's 2" 240×320 IPS
// has a visible corner of about 2 mm, which is 16 px at this density: 28 would
// swallow the 24 px status bar, and 14 (half of 28 by pixel count) undershoots
// the glass. 16 is where the setting starts; see corners.h for the range the
// Appearance page lets it be moved through.

// Samples per pixel along each axis, for the antialiased edge of the arc.
#define CORNER_SAMPLES 4

enum { TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT };

static const lv_align_t corner_align[4] = {LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT};

// Alpha only (A8): LVGL draws such an image in its recolour, black by default,
// with the alpha as coverage -- a quarter of the memory of ARGB8888 and a plain
// fill rather than an image blend. Built, and the objects created, only once
// the option is on: with it off, which is the default, none of this is touched.
static lv_obj_t *corner[4];
static uint8_t pixels[4][CORNERS_RADIUS_MAX * CORNERS_RADIUS_MAX];
static lv_image_dsc_t image[4];
static bool enabled;
// The arc the masks in `pixels` were last built for; 0 until they are made.
static int built_radius;

static int clamp_radius(long r) {
	if (r < CORNERS_RADIUS_MIN) {
		return CORNERS_RADIUS_MIN;
	}
	if (r > CORNERS_RADIUS_MAX) {
		return CORNERS_RADIUS_MAX;
	}
	return (int)r;
}

// The config is the one place the radius is kept: asked every time, so there is
// no second copy to fall out of step with it. A key that was never written
// gives the board's own arc, which is what the corners were before it existed.
int corners_radius(void) { return clamp_radius(config_get_int("ui", "corner_radius", bp_screen_corner_radius())); }

// The share of the pixel that lies outside the circle, as alpha. Worked out for
// the top-left corner, whose arc is centred on (radius, radius), and mirrored
// into the other three. Packed at `radius` stride so a small mask does not
// carry the stride of the largest one.
static void build_masks(int radius) {
	const float r = (float)radius;
	for (int y = 0; y < radius; y++) {
		for (int x = 0; x < radius; x++) {
			int outside = 0;
			for (int sy = 0; sy < CORNER_SAMPLES; sy++) {
				for (int sx = 0; sx < CORNER_SAMPLES; sx++) {
					float dx = r - (x + (sx + 0.5f) / CORNER_SAMPLES);
					float dy = r - (y + (sy + 0.5f) / CORNER_SAMPLES);
					if (dx * dx + dy * dy > r * r) {
						outside++;
					}
				}
			}
			uint8_t alpha = (uint8_t)(outside * 255 / (CORNER_SAMPLES * CORNER_SAMPLES));

			int mx = radius - 1 - x;
			int my = radius - 1 - y;
			pixels[TOP_LEFT][y * radius + x] = alpha;
			pixels[TOP_RIGHT][y * radius + mx] = alpha;
			pixels[BOTTOM_LEFT][my * radius + x] = alpha;
			pixels[BOTTOM_RIGHT][my * radius + mx] = alpha;
		}
	}

	for (int i = 0; i < 4; i++) {
		image[i].header.magic = LV_IMAGE_HEADER_MAGIC;
		image[i].header.cf = LV_COLOR_FORMAT_A8;
		image[i].header.w = (uint32_t)radius;
		image[i].header.h = (uint32_t)radius;
		image[i].header.stride = (uint32_t)radius;
		image[i].data_size = (uint32_t)radius * (uint32_t)radius;
		image[i].data = (const uint8_t *)pixels[i];
	}
	built_radius = radius;
}

// Anything created on the system layer later (the busy veils, the screenshot
// flash) would otherwise land on top of the corners, so they are moved back up
// whenever its children change. Moving them sends the same event again, hence
// the guard.
static void sys_layer_changed_cb(lv_event_t *e) {
	static bool moving;
	lv_obj_t *layer = lv_event_get_current_target(e);
	uint32_t count = lv_obj_get_child_count(layer);
	if (moving || !corner[3] || count < 4 || lv_obj_get_index(corner[3]) == (int32_t)count - 1) {
		return;
	}
	moving = true;
	for (int i = 0; i < 4; i++) {
		lv_obj_move_foreground(corner[i]);
	}
	moving = false;
}

// The masks and the four objects, the first time the option is on.
static void create_corners(void) {
	if (corner[0]) {
		return;
	}
	build_masks(corners_radius());

	lv_obj_t *layer = lv_layer_sys();
	for (int i = 0; i < 4; i++) {
		corner[i] = lv_image_create(layer);
		lv_image_set_src(corner[i], &image[i]);
		lv_obj_set_style_image_recolor(corner[i], lv_color_black(), 0);
		lv_obj_set_clickable(corner[i], false);
		lv_obj_set_ignore_layout(corner[i], true);
		lv_obj_align(corner[i], corner_align[i], 0, 0);
	}
}

// The arc moved after the masks were made -- the slider, with the corners up
// behind the page it is on. The masks are drawn again in the same buffers, so
// the descriptors are the ones LVGL already holds; what it has to be told is
// that they changed. Dropping them from the image cache says so, and setting
// the source again makes each object take the new size (and repaint the old
// one, which is larger when the arc has shrunk) before it is put back in its
// corner.
static void redraw_corners(void) {
	int radius = corners_radius();
	if (!corner[0] || radius == built_radius) {
		return;
	}
	build_masks(radius);
	for (int i = 0; i < 4; i++) {
		lv_image_cache_drop(&image[i]);
		lv_image_set_src(corner[i], &image[i]);
		lv_obj_align(corner[i], corner_align[i], 0, 0);
	}
}

static void apply(void) {
	if (enabled) {
		create_corners();
		// The arc may have been moved while they were off, with the masks
		// already made for the old one.
		redraw_corners();
	}
	for (int i = 0; i < 4 && corner[i]; i++) {
		lv_obj_set_hidden(corner[i], !enabled);
	}
}

void corners_init(gui_config_t *cfg) {
	(void)cfg;
	lv_obj_add_event_cb(lv_layer_sys(), sys_layer_changed_cb, LV_EVENT_CHILD_CHANGED, NULL);

	enabled = config_get_int("ui", "rounded_corners", 0) != 0;
	apply();
}

bool corners_enabled(void) { return enabled; }

void corners_set_enabled(bool on) {
	if (on == enabled) {
		return;
	}
	enabled = on;
	config_set_int("ui", "rounded_corners", on ? 1 : 0);
	config_save();
	apply();
}

void corners_set_radius(int radius) {
	radius = clamp_radius(radius);
	if (radius == corners_radius()) {
		return;
	}
	config_set_int("ui", "corner_radius", radius);
	config_save();
	apply();
}
