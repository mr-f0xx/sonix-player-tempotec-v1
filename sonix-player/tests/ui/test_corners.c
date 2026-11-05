/*
 * The rounded screen corners and how round they are, on the real renderer.
 *
 * The Appearance page owns the switch and the slider (test_main_menu.c drives
 * them); what is checked here is what they ask of corners.c: that the arc is
 * the one the setting says, drawn the right size in all four corners and
 * nowhere else, that moving it while the corners are on screen leaves nothing
 * of the old arc behind, and that the number is kept, pulled into range, and
 * read back after a restart.
 *
 * Real: LVGL and its software renderer, the corner masks, board_profile and the
 * config file. The display is a plain framebuffer in DIRECT mode, which redraws
 * only what was invalidated -- the same way the panel is driven -- so a corner
 * that was not repainted after it shrank would still be there to be found.
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/shell/corners.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/config.h"

/* LVGL's configured default font is one of the player's own faces. */
lv_font_t font_ui_14, font_ui_16;

#define W 240
#define H 320

static uint32_t framebuffer[W * H];
static uint32_t reference[W * H];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
	/* DIRECT mode renders into framebuffer itself; there is nowhere to copy to. */
	(void)area;
	(void)px_map;
	lv_display_flush_ready(disp);
}

static uint8_t red_at(const uint32_t *fb, int x, int y) { return (uint8_t)((fb[y * W + x] >> 16) & 0xFF); }

static void refresh(lv_display_t *disp) {
	lv_tick_inc(50); /* no timer thread: move the clock by hand */
	lv_refr_now(disp);
}

/* The whole panel again, from nothing: what the incremental update has to equal. */
static void redraw_everything(lv_display_t *disp) {
	lv_obj_invalidate(lv_screen_active());
	lv_obj_invalidate(lv_layer_top());
	lv_obj_invalidate(lv_layer_sys());
	refresh(disp);
}

static void assert_nothing_drawn(const char *what) {
	for (int i = 0; i < W * H; i++) {
		if ((framebuffer[i] & 0xFFFFFF) != 0xFFFFFF) {
			fprintf(stderr, "%s: pixel %d,%d is %06x on a white page\n", what, i % W, i / W, framebuffer[i] & 0xFFFFFF);
			assert(!"something was drawn that should not be");
		}
	}
}

/* The four corners of a white page, for an arc of `radius`: black outside it,
 * white inside it, a blend only in the pixels the edge crosses, the same in all
 * four corners, and not one pixel touched anywhere else. */
static void check_corners(int radius, const char *what) {
	int blended = 0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			/* Distance to the nearest corner's arc centre, in pixels, from the middle
			 * of this pixel; only the corner boxes are inside any arc. */
			int cx = x < W / 2 ? radius : W - radius;
			int cy = y < H / 2 ? radius : H - radius;
			bool in_box = (x < radius || x >= W - radius) && (y < radius || y >= H - radius);
			uint8_t v = red_at(framebuffer, x, y);
			if (!in_box) {
				if (v != 255) {
					fprintf(stderr, "%s (r=%d): pixel %d,%d outside the corner boxes is %d\n", what, radius, x, y, v);
					assert(!"drawn outside the corner boxes");
				}
				continue;
			}
			double d = hypot((double)cx - (x + 0.5), (double)cy - (y + 0.5));
			if (d > radius + 1.0) {
				if (v > 8) {
					fprintf(stderr, "%s (r=%d): pixel %d,%d is %d, outside the arc (d=%.2f)\n", what, radius, x, y, v, d);
					assert(!"a pixel outside the arc is not black");
				}
			} else if (d < radius - 1.0) {
				if (v != 255) {
					fprintf(stderr, "%s (r=%d): pixel %d,%d is %d, inside the arc (d=%.2f)\n", what, radius, x, y, v, d);
					assert(!"a pixel inside the arc is covered");
				}
			} else if (v != 0 && v != 255) {
				blended++;
			}
			/* Mirrored exactly into the other three corners. */
			assert(v == red_at(framebuffer, W - 1 - x, y));
			assert(v == red_at(framebuffer, x, H - 1 - y));
			assert(v == red_at(framebuffer, W - 1 - x, H - 1 - y));
		}
	}
	/* The edge is antialiased, not a staircase of black and white. */
	assert(blended >= radius);
	/* And the corner pixel itself is the one the arc takes first. */
	assert(red_at(framebuffer, 0, 0) <= 8 || radius < 4);
}

/* Moved while on screen: what the update left must be what a fresh draw gives. */
static void check_incremental_equals_full(lv_display_t *disp, const char *what) {
	memcpy(reference, framebuffer, sizeof(reference));
	redraw_everything(disp);
	if (memcmp(reference, framebuffer, sizeof(reference)) != 0) {
		for (int i = 0; i < W * H; i++) {
			if (reference[i] != framebuffer[i]) {
				fprintf(stderr, "%s: pixel %d,%d is %06x after the update and %06x after a full redraw\n", what, i % W, i / W, reference[i] & 0xFFFFFF, framebuffer[i] & 0xFFFFFF);
				break;
			}
		}
		assert(!"the corners were not repainted where the old arc was");
	}
}

int main(void) {
	char dir[] = "/tmp/sonix-corners-test-XXXXXX";
	assert(mkdtemp(dir));
	char path[256];
	snprintf(path, sizeof(path), "%s/device_config.ini", dir);
	config_init(path);

	font_ui_14 = font_ui_16 = lv_font_montserrat_14;
	lv_init();
	lv_display_t *disp = lv_display_create(W, H);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush_cb);

	/* A white page, so that black corners are as visible as they can be. */
	lv_obj_t *page = lv_screen_active();
	lv_obj_set_style_bg_color(page, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);

	gui_config_t cfg = {W, H, bp_padding(), bp_status_bar_h(), "/tmp"};
	corners_init(&cfg);

	/* ---- out of the box: off, and the arc is the board's own ------------------- */
	assert(bp_screen_corner_radius() == 16); /* the V1's glass */
	assert(!corners_enabled());
	assert(corners_radius() == bp_screen_corner_radius());
	refresh(disp);
	assert_nothing_drawn("corners off");

	/* The slider's ends are in range of what the masks can hold, and 16, where
	 * everyone who already had the corners on is, is one of its stops. */
	assert(CORNERS_RADIUS_MIN >= 1 && CORNERS_RADIUS_MIN < CORNERS_RADIUS_MAX);
	assert(CORNERS_RADIUS_MAX < bp_status_bar_h()); /* an arc taller than the bar would eat its clock */
	assert((bp_screen_corner_radius() - CORNERS_RADIUS_MIN) % CORNERS_RADIUS_STEP == 0);
	assert((CORNERS_RADIUS_MAX - CORNERS_RADIUS_MIN) % CORNERS_RADIUS_STEP == 0);

	/* ---- turned on with nothing chosen: exactly the arc they always had -------- */
	corners_set_enabled(true);
	refresh(disp);
	check_corners(16, "default arc");
	corners_set_enabled(false);
	refresh(disp);
	assert_nothing_drawn("switched off again");

	/* ---- the arc moved while the corners are off, before any mask was drawn at it -- */
	corners_set_radius(10);
	assert(corners_radius() == 10);
	assert(config_get_int("ui", "corner_radius", 0) == 10);
	refresh(disp);
	assert_nothing_drawn("radius changed while off");
	corners_set_enabled(true);
	refresh(disp);
	check_corners(10, "turned on at the new arc");

	/* ---- moved with the corners on screen -------------------------------------- */
	corners_set_radius(4); /* smaller: the old arc has to be wiped */
	refresh(disp);
	check_corners(4, "shrunk to 4");
	check_incremental_equals_full(disp, "shrunk to 4");

	corners_set_radius(CORNERS_RADIUS_MAX); /* and larger again */
	refresh(disp);
	check_corners(CORNERS_RADIUS_MAX, "grown to the largest");
	check_incremental_equals_full(disp, "grown to the largest");

	corners_set_radius(CORNERS_RADIUS_MIN);
	refresh(disp);
	check_corners(CORNERS_RADIUS_MIN, "shrunk to the smallest");
	check_incremental_equals_full(disp, "shrunk to the smallest");

	/* Every stop the slider can land on is drawn as its own arc. */
	for (int r = CORNERS_RADIUS_MIN; r <= CORNERS_RADIUS_MAX; r += CORNERS_RADIUS_STEP) {
		corners_set_radius(r);
		refresh(disp);
		check_corners(r, "every stop");
		check_incremental_equals_full(disp, "every stop");
	}

	/* ---- off with masks that exist, moved, then on again ----------------------- */
	corners_set_enabled(false);
	refresh(disp);
	assert_nothing_drawn("off after being on");
	corners_set_radius(8);
	refresh(disp);
	assert_nothing_drawn("moved while off, masks already built");
	corners_set_enabled(true);
	refresh(disp);
	check_corners(8, "on at an arc moved while off");
	check_incremental_equals_full(disp, "on at an arc moved while off");

	/* ---- always in range ------------------------------------------------------- */
	corners_set_radius(1);
	assert(corners_radius() == CORNERS_RADIUS_MIN);
	corners_set_radius(-5);
	assert(corners_radius() == CORNERS_RADIUS_MIN);
	corners_set_radius(1000);
	assert(corners_radius() == CORNERS_RADIUS_MAX);
	refresh(disp);
	check_corners(CORNERS_RADIUS_MAX, "clamped to the largest");

	/* A number written into the file by hand is pulled into range, not refused
	 * and not trusted: the masks are only as big as the largest stop. */
	config_set_int("ui", "corner_radius", 5000);
	assert(corners_radius() == CORNERS_RADIUS_MAX);
	config_set_int("ui", "corner_radius", 0);
	assert(corners_radius() == CORNERS_RADIUS_MIN);
	config_set_int("ui", "corner_radius", -40);
	assert(corners_radius() == CORNERS_RADIUS_MIN);
	/* One between two stops is honoured as it is. */
	config_set_int("ui", "corner_radius", 7);
	assert(corners_radius() == 7);

	/* ---- kept across a restart ------------------------------------------------- */
	corners_set_radius(12);
	assert(config_get_int("ui", "rounded_corners", 0) == 1);
	assert(config_get_int("ui", "corner_radius", 0) == 12);
	assert(config_init(path)); /* the file, read again */
	assert(corners_radius() == 12);
	assert(config_get_int("ui", "rounded_corners", 0) == 1);

	/* Setting it to what it already is leaves it as it was. */
	corners_set_radius(12);
	assert(corners_radius() == 12);

	/* ---- a key that was never written is the board's arc ----------------------- */
	char other[256];
	snprintf(other, sizeof(other), "%s/fresh_config.ini", dir);
	assert(!config_init(other)); /* no file yet: everything is a default */
	assert(corners_radius() == bp_screen_corner_radius());

	printf("PASS corners (radius %d..%d px in steps of %d, default %d)\n", CORNERS_RADIUS_MIN, CORNERS_RADIUS_MAX, CORNERS_RADIUS_STEP, bp_screen_corner_radius());
	return 0;
}
