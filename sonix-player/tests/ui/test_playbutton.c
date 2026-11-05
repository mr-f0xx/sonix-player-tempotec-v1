/*
 * The now-playing page's play/pause button, plain and legacy, on the real
 * renderer.
 *
 * playbutton_paint() decides a shape and two colours, and both only exist once
 * something has drawn them: whether the disc is round and whole, whether the
 * glyph sits clear of the rim and in the middle of it, whether a finger on the
 * button shows, and -- the reason the legacy look is more than a circle --
 * whether the glyph can be told from the disc under every palette and accent
 * the player can be in. OLED's white on a white disc and a yellow accent on one
 * are the two that make the difference. So the button is built the way
 * player_init() builds it, painted, and read back pixel by pixel from a
 * snapshot of the page behind it.
 *
 * Real: LVGL and its software renderer with its own button theme (which is what
 * darkens a pressed button), the theme module with its palettes and accents,
 * the glyph bitmaps, and the translations. Not built: the player around the
 * button, which is far too large to build off the device. What it hands the
 * button is stood in for -- a page of one colour for the sleeve, and the accent
 * as chrome_accent() in player.c chooses it.
 *
 * SONIX_PLAYBUTTON_SNAPSHOTS=<prefix> also writes <prefix>sheet.ppm: the button
 * in both looks under every palette and a spread of accents, to look at.
 */
#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/nowplaying/playbutton.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

/* LVGL's configured default font is one of the player's own faces. */
lv_font_t font_ui_14, font_ui_16;

/* theme.c has the boot logo follow the palette; there is no boot logo here. */
void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool bootpic_present(void) { return false; }

#define PANEL_W 240
#define PANEL_H 320
#define COMPACT_SIDE 44 /* the compact layout's play button: COMPACT_PLAY_BTN in player.c */
#define FULL_SIDE 84	/* the other layout's */

#define WHITE 0xFFFFFFu
#define BLACK 0x000000u

/* The disc has to be at least this clear of the glyph. The module aims at 2.5:1
 * on a cheaper curve than sRGB's; over every accent there is, the real figure
 * never falls below 2.42, and on a disc that had to become the accent it never
 * falls below 3. */
#define READABLE 2.4

// clang-format off
#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			fprintf(stderr, __VA_ARGS__);                                                                              \
			fputc('\n', stderr);                                                                                       \
			assert(!#cond);                                                                                            \
		}                                                                                                              \
	} while (0)
// clang-format on

static const lv_image_dsc_t *const GLYPHS[] = {&icon_play, &icon_pause, &icon_stop};
static const char *const GLYPH_NAMES[] = {"play", "pause", "stop"};
#define GLYPH_COUNT 3

/* ---- colours ------------------------------------------------------------- */

static uint32_t rgb(lv_color_t c) { return lv_color_to_u32(c) & 0xFFFFFF; }

static double linear(unsigned channel) {
	double v = channel / 255.0;
	return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double relative_luminance(uint32_t c) { return 0.2126 * linear((c >> 16) & 0xFF) + 0.7152 * linear((c >> 8) & 0xFF) + 0.0722 * linear(c & 0xFF); }

/* WCAG's contrast ratio, from sRGB's own curve rather than the cheaper one the
 * module decides on: the check has to be able to disagree with it. */
static double contrast(uint32_t a, uint32_t b) {
	double la = relative_luminance(a);
	double lb = relative_luminance(b);
	if (la < lb) {
		double t = la;
		la = lb;
		lb = t;
	}
	return (la + 0.05) / (lb + 0.05);
}

/* Whether two colours are the same to within `tolerance` in every channel. The
 * glyph is exact when it is drawn at its own size; scaled down, LVGL's transform
 * lands a couple of levels off in its solid interior -- not a thing the eye can
 * find, but not a bit-for-bit copy either. */
static bool close_to(uint32_t a, uint32_t b, int tolerance) {
	for (int shift = 0; shift <= 16; shift += 8) {
		int d = (int)((a >> shift) & 0xFF) - (int)((b >> shift) & 0xFF);
		if (d > tolerance || d < -tolerance) {
			return false;
		}
	}
	return true;
}

#define GLYPH_TOLERANCE 4

/* What chrome_accent() hands the button when no sleeve tints it. */
static lv_color_t player_accent(void) {
	if (theme_is_monochrome() || theme_is_tokyo_night()) {
		return theme_player_chrome_color(THEME_PLAYER_CHROME_PLAYBACK);
	}
	return theme()->accent;
}

/* The disc the original player drew: white over artwork or in a dark palette,
 * the palette's pressed surface on the light page. */
static bool white_disc_for(bool over_artwork) { return over_artwork || theme()->dark; }

/* ---- the button, on a page ---------------------------------------------- */

typedef struct {
	lv_obj_t *page;
	lv_obj_t *button;
	lv_obj_t *glyph;
	int side;
	int x, y; /* the button's top left; its middle is the panel's */
	uint32_t page_colour;
} fixture_t;

/* The button as player_init() builds it, in the middle of a page of one colour
 * that stands in for the sleeve. */
static fixture_t make_fixture(int side, uint32_t page_colour) {
	fixture_t f = {.side = side, .x = (PANEL_W - side) / 2, .y = (PANEL_H - side) / 2, .page_colour = page_colour};
	f.page = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(f.page, lv_color_hex(page_colour), 0);
	lv_obj_set_style_bg_opa(f.page, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(f.page, 0, 0);

	f.button = lv_btn_create(f.page);
	lv_obj_set_size(f.button, side, side);
	lv_obj_set_pos(f.button, f.x, f.y);
	lv_obj_set_style_bg_opa(f.button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(f.button, 0, 0);
	lv_obj_set_style_shadow_width(f.button, 0, 0);
	lv_obj_set_style_opa(f.button, LV_OPA_40, LV_STATE_DISABLED);

	f.glyph = lv_image_create(f.button);
	lv_image_set_src(f.glyph, &icon_play);
	lv_obj_center(f.glyph);
	return f;
}

static void free_fixture(fixture_t *f) {
	lv_obj_delete(f->page);
	memset(f, 0, sizeof(*f));
}

static lv_draw_buf_t *shoot(const fixture_t *f) {
	lv_obj_update_layout(f->page);
	lv_draw_buf_t *shot = lv_snapshot_take(f->page, LV_COLOR_FORMAT_XRGB8888);
	assert(shot);
	assert(shot->header.w == PANEL_W && shot->header.h == PANEL_H);
	return shot;
}

static uint32_t pixel_at(const lv_draw_buf_t *shot, int x, int y) {
	const uint8_t *p = shot->data + (size_t)y * shot->header.stride + (size_t)x * 4;
	return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

/* How far a pixel's middle is from the middle of the panel -- the middle of the
 * button, and so of its disc. */
static double centre_distance(int x, int y) { return hypot(x + 0.5 - PANEL_W / 2.0, y + 0.5 - PANEL_H / 2.0); }

/* What the button drew, inside `limit` of its middle, that is not `ground`. */
typedef struct {
	int count;	   /* pixels that are not the ground */
	int solid;	   /* of which are the glyph's colour, give or take GLYPH_TOLERANCE */
	double reach;  /* the farthest of them from the middle */
	double cx, cy; /* the middle of their bounding box */
} ink_t;

static ink_t measure_ink(const lv_draw_buf_t *shot, uint32_t ground, uint32_t ink, double limit) {
	ink_t m = {.cx = -1, .cy = -1};
	int x1 = PANEL_W, y1 = PANEL_H, x2 = -1, y2 = -1;
	for (int y = 0; y < PANEL_H; y++) {
		for (int x = 0; x < PANEL_W; x++) {
			double d = centre_distance(x, y);
			if (d > limit) {
				continue;
			}
			uint32_t c = pixel_at(shot, x, y);
			if (c == ground) {
				continue;
			}
			m.count++;
			if (close_to(c, ink, GLYPH_TOLERANCE)) {
				m.solid++;
			}
			if (d > m.reach) {
				m.reach = d;
			}
			x1 = x < x1 ? x : x1;
			y1 = y < y1 ? y : y1;
			x2 = x > x2 ? x : x2;
			y2 = y > y2 ? y : y2;
		}
	}
	if (m.count) {
		m.cx = (x1 + x2 + 1) / 2.0;
		m.cy = (y1 + y2 + 1) / 2.0;
	}
	return m;
}

/* A disc of `disc` behind a glyph of `ink`, drawn the size of the button: round
 * and no bigger, whole out to the rim, the glyph in the middle of it and well
 * clear of the edge, in the colour it was given. */
static void check_disc(const fixture_t *f, const lv_draw_buf_t *shot, uint32_t disc, uint32_t ink, const char *what) {
	double r = f->side / 2.0;

	/* Nothing past the rim, a pixel of antialiasing apart: not a rounded
	 * square, not a disc that spilled over the button. */
	for (int y = 0; y < PANEL_H; y++) {
		for (int x = 0; x < PANEL_W; x++) {
			if (centre_distance(x, y) >= r + 1.0) {
				CHECK(pixel_at(shot, x, y) == f->page_colour, "%s: pixel %d,%d is %06x, %.1f px out from the middle of a %d px button; the page is %06x", what, x, y, pixel_at(shot, x, y), centre_distance(x, y), f->side, f->page_colour);
			}
		}
	}

	/* Whole: the disc's colour from the glyph out to the rim, so that what is
	 * left over to measure is the glyph. A disc that stopped short, or a glyph
	 * that ran into the rim, both end up as something far from the middle. */
	ink_t m = measure_ink(shot, disc, ink, r - 1.5);
	CHECK(m.count > 0, "%s: no glyph on the disc", what);
	CHECK(m.reach <= r - 6.0,
		  "%s: something that is not the disc %06x ends %.1f px from the middle of a %d px button (the glyph should "
		  "clear the rim by 6)",
		  what, disc, m.reach, f->side);
	/* The play triangle is drawn a pixel and a half right of its box's middle,
	 * which shows at its own size and is under a pixel once it is scaled. */
	double off = lv_image_get_scale(f->glyph) == LV_SCALE_NONE ? 2.0 : 1.0;
	CHECK(fabs(m.cx - PANEL_W / 2.0) <= off && fabs(m.cy - PANEL_H / 2.0) <= off, "%s: the glyph's middle is at %.1f,%.1f, not %.1f,%.1f", what, m.cx, m.cy, PANEL_W / 2.0, PANEL_H / 2.0);
	CHECK(m.solid >= 100, "%s: %d pixels are the glyph's colour %06x", what, m.solid, ink);
}

/* The plain look: the glyph at the size it is drawn, and nothing else. */
static void check_bare(const fixture_t *f, const lv_draw_buf_t *shot, uint32_t ink, const char *what) {
	ink_t m = measure_ink(shot, f->page_colour, ink, PANEL_W);
	CHECK(m.count > 0, "%s: no glyph", what);
	/* The disc would reach the rim: 22 px out on the compact button, 42 on the
	 * other. The glyph's own farthest corner is 20. */
	CHECK(m.reach <= 21.0, "%s: something is drawn %.1f px from the middle; the plain button has no body", what, m.reach);
	/* Shrunk for a disc it would be under 13. */
	CHECK(m.reach >= 15.5, "%s: the glyph reaches only %.1f px; the plain look draws it at its own size", what, m.reach);
	CHECK(fabs(m.cx - PANEL_W / 2.0) <= 2.0 && fabs(m.cy - PANEL_H / 2.0) <= 2.0, "%s: the glyph's middle is at %.1f,%.1f", what, m.cx, m.cy);
	CHECK(m.solid >= 300, "%s: %d pixels are the glyph's colour %06x", what, m.solid, ink);
}

/* ---- the looks ------------------------------------------------------------ */

static lv_color_t blue(void) { return theme_accent_preset(0); }

static void test_plain_look(void) {
	static const int SIDES[] = {COMPACT_SIDE, FULL_SIDE};
	for (size_t s = 0; s < sizeof(SIDES) / sizeof(SIDES[0]); s++) {
		for (int g = 0; g < GLYPH_COUNT; g++) {
			char what[64];
			snprintf(what, sizeof(what), "plain %s, %d px", GLYPH_NAMES[g], SIDES[s]);
			fixture_t f = make_fixture(SIDES[s], 0x303030);
			lv_image_set_src(f.glyph, GLYPHS[g]);
			playbutton_paint(f.button, f.glyph, false, blue(), true);

			CHECK(lv_obj_get_style_bg_opa(f.button, LV_PART_MAIN) == LV_OPA_TRANSP, "%s: it has a body", what);
			CHECK(lv_image_get_scale(f.glyph) == LV_SCALE_NONE, "%s: the glyph is scaled", what);
			CHECK(rgb(lv_obj_get_style_image_recolor(f.glyph, LV_PART_MAIN)) == rgb(blue()), "%s: the glyph is not the accent", what);

			lv_draw_buf_t *shot = shoot(&f);
			check_bare(&f, shot, rgb(blue()), what);
			lv_draw_buf_destroy(shot);
			free_fixture(&f);
		}
	}
}

static void test_legacy_look(void) {
	static const int SIDES[] = {COMPACT_SIDE, FULL_SIDE};
	for (size_t s = 0; s < sizeof(SIDES) / sizeof(SIDES[0]); s++) {
		for (int g = 0; g < GLYPH_COUNT; g++) {
			char what[64];
			snprintf(what, sizeof(what), "legacy %s, %d px", GLYPH_NAMES[g], SIDES[s]);
			fixture_t f = make_fixture(SIDES[s], 0x303030);
			lv_image_set_src(f.glyph, GLYPHS[g]);
			playbutton_paint(f.button, f.glyph, true, blue(), true);

			CHECK(lv_obj_get_style_radius(f.button, LV_PART_MAIN) == LV_RADIUS_CIRCLE, "%s: not a circle", what);
			CHECK(lv_obj_get_style_bg_opa(f.button, LV_PART_MAIN) == LV_OPA_COVER, "%s: the disc is not solid", what);
			if (SIDES[s] == COMPACT_SIDE) {
				/* The bitmaps are drawn for a button with nothing behind
				 * it; on a 44 px disc they have to come in from the rim. */
				CHECK(lv_image_get_scale(f.glyph) < LV_SCALE_NONE, "%s: the glyph is full size on a small disc", what);
			} else {
				/* A bigger button keeps the glyph as it was drawn. */
				CHECK(lv_image_get_scale(f.glyph) == LV_SCALE_NONE, "%s: the glyph is scaled up or down", what);
			}

			lv_draw_buf_t *shot = shoot(&f);
			check_disc(&f, shot, WHITE, rgb(blue()), what);
			lv_draw_buf_destroy(shot);
			free_fixture(&f);
		}
	}
}

/* A pixel inside the glyph, as close to the middle as the glyph allows. */
static bool find_ink(const lv_draw_buf_t *shot, uint32_t ink, int *px, int *py) {
	double best = 1e9;
	bool found = false;
	for (int y = 0; y < PANEL_H; y++) {
		for (int x = 0; x < PANEL_W; x++) {
			double d = centre_distance(x, y);
			if (d < best && close_to(pixel_at(shot, x, y), ink, GLYPH_TOLERANCE)) {
				best = d;
				*px = x;
				*py = y;
				found = true;
			}
		}
	}
	return found;
}

static bool darker(uint32_t was, uint32_t now) {
	/* Every channel down, none to black: a press tints, it does not blank. */
	for (int shift = 0; shift <= 16; shift += 8) {
		uint32_t a = (was >> shift) & 0xFF;
		uint32_t b = (now >> shift) & 0xFF;
		if (a > 40 && !(b < a && b * 10 >= a * 7)) {
			return false;
		}
	}
	return was != now;
}

/* A finger on the button shows in the glyph, in both looks, and on the disc in
 * the legacy one -- and what it leaves behind is what was there. The darkening
 * is LVGL's own button theme's; what is checked is that the disc, which is a
 * fill of the button's own, takes it too. */
static void test_pressed(void) {
	for (int legacy = 0; legacy < 2; legacy++) {
		char what[64];
		snprintf(what, sizeof(what), "%s look pressed", legacy ? "legacy" : "plain");
		fixture_t f = make_fixture(COMPACT_SIDE, 0x303030);
		playbutton_paint(f.button, f.glyph, legacy, blue(), true);

		lv_draw_buf_t *rest = shoot(&f);
		int gx = 0, gy = 0;
		CHECK(find_ink(rest, rgb(blue()), &gx, &gy), "%s: no glyph pixel to follow", what);
		int dx = f.x + f.side - 5, dy = PANEL_H / 2; /* on the disc, off the glyph */
		uint32_t glyph_rest = pixel_at(rest, gx, gy);
		uint32_t disc_rest = pixel_at(rest, dx, dy);
		CHECK(disc_rest == (legacy ? WHITE : 0x303030u), "%s: %06x at the disc's edge", what, disc_rest);

		lv_obj_add_state(f.button, LV_STATE_PRESSED);
		lv_draw_buf_t *down = shoot(&f);
		CHECK(darker(glyph_rest, pixel_at(down, gx, gy)), "%s: the glyph went from %06x to %06x", what, glyph_rest, pixel_at(down, gx, gy));
		if (legacy) {
			CHECK(darker(disc_rest, pixel_at(down, dx, dy)), "%s: the disc went from %06x to %06x", what, disc_rest, pixel_at(down, dx, dy));
		} else {
			CHECK(pixel_at(down, dx, dy) == 0x303030u, "%s: pressing grew a body (%06x)", what, pixel_at(down, dx, dy));
		}

		lv_obj_remove_state(f.button, LV_STATE_PRESSED);
		lv_draw_buf_t *up = shoot(&f);
		CHECK(pixel_at(up, gx, gy) == glyph_rest && pixel_at(up, dx, dy) == disc_rest, "%s: letting go left %06x on the glyph and %06x on the disc", what, pixel_at(up, gx, gy), pixel_at(up, dx, dy));

		lv_draw_buf_destroy(rest);
		lv_draw_buf_destroy(down);
		lv_draw_buf_destroy(up);
		free_fixture(&f);
	}
}

/* The switch is read back at runtime: turning it off must take the disc and the
 * glyph's scale with it, and turning it on again must bring back exactly the
 * same button, whatever the glyph was showing in between. */
static void test_switching(void) {
	fixture_t f = make_fixture(COMPACT_SIDE, 0x303030);
	for (int round = 0; round < 3; round++) {
		for (int g = 0; g < GLYPH_COUNT; g++) {
			lv_image_set_src(f.glyph, GLYPHS[g]);

			playbutton_paint(f.button, f.glyph, true, blue(), true);
			lv_draw_buf_t *shot = shoot(&f);
			check_disc(&f, shot, WHITE, rgb(blue()), "switched on");
			lv_draw_buf_destroy(shot);

			playbutton_paint(f.button, f.glyph, false, blue(), true);
			shot = shoot(&f);
			check_bare(&f, shot, rgb(blue()), "switched off");
			/* The disc is gone as a pixel as well as a style. */
			CHECK(pixel_at(shot, f.x + f.side - 5, PANEL_H / 2) == 0x303030u, "a disc was left behind (%06x)", pixel_at(shot, f.x + f.side - 5, PANEL_H / 2));
			lv_draw_buf_destroy(shot);
		}
	}
	free_fixture(&f);
}

/* ---- the colours ---------------------------------------------------------- */

typedef struct {
	uint32_t disc;
	uint32_t ink;
} look_t;

/* What the button is painted, read back off its styles. */
static look_t painted(const fixture_t *f) {
	look_t l = {.disc = rgb(lv_obj_get_style_bg_color(f->button, LV_PART_MAIN)), .ink = rgb(lv_obj_get_style_image_recolor(f->glyph, LV_PART_MAIN))};
	return l;
}

typedef struct {
	const char *name;
	theme_kind_t kind;
	int accent;		 /* THEME_ACCENT_PRESET index */
	bool over_cover; /* the page stands over a loaded sleeve */
	uint32_t disc;
	uint32_t ink;
} promise_t;

/* The looks anybody will see, and so the ones that are written down. */
static void test_promises(void) {
	/* Dark, Light, OLED and the like are 5 palettes with 12 accents, and what is
	 * checked below is a property of all of them. These few are the answer to
	 * "what does it look like", and they are what the contact sheet shows. */
	theme_set_kind(THEME_KIND_DARK);
	theme_set_accent(0);
	uint32_t blue_accent = rgb(theme()->accent);
	theme_set_kind(THEME_KIND_LIGHT);
	uint32_t light_disc = rgb(theme()->surface_pressed);

	const promise_t PROMISES[] = {
		/* The original look, the default accent, in both themes and with or
		 * without a sleeve: a light disc, a blue glyph. */
		{"Dark, blue, over a sleeve", THEME_KIND_DARK, 0, true, WHITE, blue_accent},
		{"Dark, blue, no sleeve", THEME_KIND_DARK, 0, false, WHITE, blue_accent},
		{"Light, blue, over a sleeve", THEME_KIND_LIGHT, 0, true, WHITE, blue_accent},
		{"Light, blue, no sleeve", THEME_KIND_LIGHT, 0, false, light_disc, blue_accent},
		/* Where an accent would wash out on white, the disc is the accent and
		 * the glyph is black on it. */
		{"Dark, yellow", THEME_KIND_DARK, 3, false, 0xFFFFFFFF, BLACK},
		{"Dark, green", THEME_KIND_DARK, 5, false, 0xFFFFFFFF, BLACK},
		{"Light, yellow, no sleeve", THEME_KIND_LIGHT, 3, false, 0xFFFFFFFF, BLACK},
		/* The palettes that draw everything in one colour. */
		{"Tokyo Night", THEME_KIND_TOKYO_NIGHT, 0, false, 0xBB9AF7, BLACK},
		{"Amber on Black", THEME_KIND_AMBER_ON_BLACK, 0, false, 0x9C5A0D, WHITE},
		{"OLED", THEME_KIND_OLED, 0, false, WHITE, BLACK},
		{"OLED, yellow", THEME_KIND_OLED, 3, false, 0xFFFFFFFF, BLACK},
	};
	for (size_t i = 0; i < sizeof(PROMISES) / sizeof(PROMISES[0]); i++) {
		const promise_t *p = &PROMISES[i];
		theme_set_kind(p->kind);
		theme_set_accent(p->accent);
		uint32_t accent = rgb(player_accent());
		/* 0xFFFFFFFF stands for "the accent itself". */
		uint32_t want_disc = p->disc == 0xFFFFFFFFu ? accent : p->disc;

		fixture_t f = make_fixture(COMPACT_SIDE, 0x303030);
		playbutton_paint(f.button, f.glyph, true, player_accent(), white_disc_for(p->over_cover));
		look_t l = painted(&f);
		CHECK(l.disc == want_disc && l.ink == p->ink, "%s: disc %06x and glyph %06x, expected %06x and %06x", p->name, l.disc, l.ink, want_disc, p->ink);

		/* And that is what reaches the glass. */
		lv_draw_buf_t *shot = shoot(&f);
		check_disc(&f, shot, l.disc, l.ink, p->name);
		lv_draw_buf_destroy(shot);
		free_fixture(&f);
	}
	theme_set_kind(THEME_KIND_DARK);
	theme_set_accent(0);
}

/* Under every palette, every accent and both discs, the glyph can be told from
 * the disc, the original look survives wherever it reads, and nothing but the
 * monochrome palettes puts a second hue on the page. */
static double test_every_palette_and_accent(int *combinations) {
	double lowest = 99.0;
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		for (int preset = 0; preset < THEME_ACCENT_COUNT; preset++) {
			theme_set_kind((theme_kind_t)kind);
			theme_set_accent(preset);
			for (int over = 0; over < 2; over++) {
				if (over == 0 && theme()->dark) {
					continue; /* a dark palette draws the white disc either way */
				}
				bool white = white_disc_for(over);
				uint32_t base = white ? WHITE : rgb(theme()->surface_pressed);
				uint32_t accent = rgb(player_accent());
				char what[96];
				snprintf(what, sizeof(what), "palette %d, accent %d, %s", kind, preset, over ? "over a sleeve" : "no sleeve");

				fixture_t f = make_fixture(COMPACT_SIDE, 0x303030);
				playbutton_paint(f.button, f.glyph, true, player_accent(), white);
				look_t l = painted(&f);

				double c = contrast(l.disc, l.ink);
				CHECK(c >= READABLE, "%s: glyph %06x on disc %06x is %.2f:1", what, l.ink, l.disc, c);
				if (c < lowest) {
					lowest = c;
				}
				CHECK(l.disc != l.ink, "%s: the glyph is the disc", what);

				if (theme_is_monochrome()) {
					CHECK(l.disc == accent, "%s: the disc is %06x, not the palette's %06x", what, l.disc, accent);
				} else {
					double on_base = contrast(base, accent);
					if (on_base >= 2.9) {
						CHECK(l.disc == base && l.ink == accent, "%s: the accent reads on the disc at %.2f:1 and should stay (disc %06x, glyph %06x)", what, on_base, l.disc, l.ink);
					} else if (on_base < 2.3) {
						CHECK(l.disc == accent && (l.ink == WHITE || l.ink == BLACK), "%s: the accent is %.2f:1 on the disc and should become it (disc %06x, glyph %06x)", what, on_base, l.disc, l.ink);
					}
				}
				if (l.disc == accent) {
					CHECK(l.ink == WHITE || l.ink == BLACK, "%s: the glyph on the accent is %06x", what, l.ink);
				}

				/* The plain look is the accent, bare, however it was left. */
				playbutton_paint(f.button, f.glyph, false, player_accent(), white);
				look_t plain = painted(&f);
				CHECK(plain.ink == accent && lv_obj_get_style_bg_opa(f.button, LV_PART_MAIN) == LV_OPA_TRANSP, "%s: the plain look is not the bare accent", what);

				free_fixture(&f);
				(*combinations)++;
			}
		}
	}
	theme_set_kind(THEME_KIND_DARK);
	theme_set_accent(0);
	return lowest;
}

/* Custom palettes and a sleeve's tint hand the button any colour at all. */
static void test_any_accent(void) {
	uint32_t seed = 20240611u;
	fixture_t f = make_fixture(COMPACT_SIDE, 0x303030);
	for (int i = 0; i < 600; i++) {
		seed = seed * 1664525u + 1013904223u;
		uint32_t accent = seed >> 8;
		for (int light = 0; light < 2; light++) {
			theme_set_kind(light ? THEME_KIND_LIGHT : THEME_KIND_DARK);
			bool white = light ? (i & 1) != 0 : true;
			playbutton_paint(f.button, f.glyph, true, lv_color_hex(accent), white);
			look_t l = painted(&f);
			double c = contrast(l.disc, l.ink);
			CHECK(c >= READABLE, "accent %06x (%s, %s disc): glyph %06x on disc %06x is %.2f:1", accent, light ? "Light" : "Dark", white ? "white" : "grey", l.ink, l.disc, c);
		}
	}
	theme_set_kind(THEME_KIND_DARK);
	free_fixture(&f);
}

/* ---- the strings, in every language -------------------------------------- */

static bool is_ini(const char *name) {
	size_t n = strlen(name);
	return n > 4 && strcmp(name + n - 4, ".ini") == 0;
}

static int characters(const char *text) {
	int chars = 0;
	for (const char *c = text; *c; c++) {
		if (((unsigned char)*c & 0xC0) != 0x80) {
			chars++;
		}
	}
	return chars;
}

/* The switch and its note. tr() answers with the key it was given when a
 * language has no such line, which would put "musicsettings_legacy_play" on the
 * glass. */
static void test_strings(const char *language_dir) {
	static const struct {
		const char *key;
		int max_chars;
	} KEYS[] = {
		{"musicsettings_legacy_play", 40},		 /* a row's title: one line */
		{"musicsettings_legacy_play_note", 200}, /* the note under it wraps */
	};
	DIR *dir = opendir(language_dir);
	assert(dir);
	int languages = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (!is_ini(entry->d_name)) {
			continue;
		}
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int)(strlen(entry->d_name) - 4), entry->d_name);
		assert(lang_set(name));
		for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
			const char *text = tr(KEYS[i].key);
			CHECK(text && text[0] && strcmp(text, KEYS[i].key) != 0, "%s: \"%s\" is not translated", name, KEYS[i].key);
			CHECK(characters(text) <= KEYS[i].max_chars, "%s: \"%s\" is %d characters, over %d", name, KEYS[i].key, characters(text), KEYS[i].max_chars);
		}
		languages++;
	}
	closedir(dir);
	CHECK(languages == 11, "%d languages, expected 11", languages);
	assert(lang_set("English"));
}

/* ---- the contact sheet ---------------------------------------------------- */

#define SHEET_ROW_H 54
#define SHEET_COL_W 56
#define SHEET_W 480

static void sheet_button(lv_obj_t *row, int col, const lv_image_dsc_t *glyph, bool legacy, lv_color_t accent, bool white, bool pressed) {
	lv_obj_t *button = lv_btn_create(row);
	lv_obj_set_size(button, COMPACT_SIDE, COMPACT_SIDE);
	lv_obj_set_pos(button, 6 + col * SHEET_COL_W, 5);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_t *image = lv_image_create(button);
	lv_image_set_src(image, glyph);
	lv_obj_center(image);
	playbutton_paint(button, image, legacy, accent, white);
	if (pressed) {
		lv_obj_add_state(button, LV_STATE_PRESSED);
	}
}

static lv_obj_t *sheet_row(lv_obj_t *sheet, int index, uint32_t page) {
	lv_obj_t *row = lv_obj_create(sheet);
	lv_obj_set_size(row, SHEET_W, SHEET_ROW_H);
	lv_obj_set_pos(row, 0, index * SHEET_ROW_H);
	lv_obj_set_style_radius(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_bg_color(row, lv_color_hex(page), 0);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(row, false);
	return row;
}

/* Rows: the five palettes, the light one with and without a sleeve, and a spread
 * of arbitrary tints. Columns: plain, legacy play, legacy pause, pressed, and
 * (Dark, Light) four more accents. */
static void write_sheet(const char *prefix) {
	static const struct {
		theme_kind_t kind;
		bool over_artwork;
		uint32_t page;
		// clang-format off
	} ROWS[] = {
		{THEME_KIND_DARK, true, 0x2A2A2E},          {THEME_KIND_DARK, false, 0x242424},
		{THEME_KIND_LIGHT, true, 0x2A2A2E},         {THEME_KIND_LIGHT, false, 0xFAFAFA},
		{THEME_KIND_TOKYO_NIGHT, false, 0x1A1B26},  {THEME_KIND_AMBER_ON_BLACK, false, 0x050505},
		{THEME_KIND_OLED, false, 0x000000},
	};
	// clang-format on
	static const uint32_t EXTRA[] = {0xE01B24, 0xE5A50A, 0x2EC27E, 0x9141AC};
	static const uint32_t TINTS[] = {0xFF7800, 0xD56199, 0x00B0D6, 0x78C896, 0xF0C878, 0xC878A0, 0x5A6EA0, 0xFAFAFA};
	enum { ROW_COUNT = sizeof(ROWS) / sizeof(ROWS[0]) + 1 };

	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_t *sheet = lv_obj_create(screen);
	lv_obj_set_size(sheet, SHEET_W, ROW_COUNT * SHEET_ROW_H);
	lv_obj_set_pos(sheet, 0, 0);
	lv_obj_set_style_radius(sheet, 0, 0);
	lv_obj_set_style_border_width(sheet, 0, 0);
	lv_obj_set_style_pad_all(sheet, 0, 0);
	lv_obj_set_scrollable(sheet, false);

	for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
		theme_set_kind(ROWS[i].kind);
		theme_set_accent(0);
		lv_obj_t *row = sheet_row(sheet, (int)i, ROWS[i].page);
		bool white = white_disc_for(ROWS[i].over_artwork);
		lv_color_t accent = player_accent();
		int col = 0;
		sheet_button(row, col++, &icon_play, false, accent, white, false);
		sheet_button(row, col++, &icon_play, true, accent, white, false);
		sheet_button(row, col++, &icon_pause, true, accent, white, false);
		sheet_button(row, col++, &icon_play, true, accent, white, true);
		if (ROWS[i].kind == THEME_KIND_DARK || ROWS[i].kind == THEME_KIND_LIGHT) {
			for (size_t e = 0; e < sizeof(EXTRA) / sizeof(EXTRA[0]); e++) {
				sheet_button(row, col++, &icon_play, true, lv_color_hex(EXTRA[e]), white, false);
			}
		}
	}
	theme_set_kind(THEME_KIND_DARK);
	theme_set_accent(0);
	lv_obj_t *row = sheet_row(sheet, ROW_COUNT - 1, 0x2A2A2E);
	for (size_t t = 0; t < sizeof(TINTS) / sizeof(TINTS[0]); t++) {
		sheet_button(row, (int)t, &icon_play, true, lv_color_hex(TINTS[t]), true, false);
	}

	lv_obj_update_layout(screen);
	lv_draw_buf_t *shot = lv_snapshot_take(sheet, LV_COLOR_FORMAT_XRGB8888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%ssheet.ppm", prefix);
	FILE *file = fopen(path, "wb");
	assert(file);
	fprintf(file, "P6\n%u %u\n255\n", (unsigned)shot->header.w, (unsigned)shot->header.h);
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row_px = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			uint8_t out[] = {row_px[x * 4 + 2], row_px[x * 4 + 1], row_px[x * 4]};
			assert(fwrite(out, sizeof(out), 1, file) == 1);
		}
	}
	assert(fclose(file) == 0);
	lv_draw_buf_destroy(shot);
	lv_obj_delete(screen);
	fprintf(stderr, "wrote %s\n", path);
}

int main(int argc, char **argv) {
	assert(argc == 2);

	char card[] = "/tmp/sonix-playbutton-test-XXXXXX";
	assert(mkdtemp(card));
	char config[512];
	snprintf(config, sizeof(config), "%s/device_config.ini", card);
	config_init(config);
	config_set("ui", "language", "English");
	lang_init(argv[1]);

	font_ui_14 = lv_font_montserrat_14;
	font_ui_16 = lv_font_montserrat_14;
	lv_init();
	lv_display_create(PANEL_W, PANEL_H);
	theme_init();

	test_plain_look();
	test_legacy_look();
	test_pressed();
	test_switching();
	test_promises();
	int combinations = 0;
	double lowest = test_every_palette_and_accent(&combinations);
	test_any_accent();
	test_strings(argv[1]);

	const char *snapshots = getenv("SONIX_PLAYBUTTON_SNAPSHOTS");
	if (snapshots && snapshots[0]) {
		write_sheet(snapshots);
	}

	printf("PASS playbutton (%d palette/accent/disc combinations, glyph never under %.2f:1 on its disc)\n", combinations, lowest);
	return 0;
}
