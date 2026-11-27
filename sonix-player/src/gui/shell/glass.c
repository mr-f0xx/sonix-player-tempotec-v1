#include "glass.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/theme.h"

// The blur currently installed on the bottom layer, owned here. Freed and
// replaced when a new one lands, and dropped when the sleeve goes away.
static cover_image_t glass_pic;
static lv_obj_t *glass_img;

// The always-on wallpaper under everything: a soft iOS-style colour field so
// the frosted panels have something to show through even with no track loaded.
// Rendered once per screen size into an RGB565 buffer, kept while glass is
// active and freed when the theme leaves it.
static cover_image_t glass_ambient;
static lv_obj_t *glass_ambient_img;
static int glass_ambient_w, glass_ambient_h;

// Bumped to discard any blur still in flight: a job whose generation no longer
// matches is freed instead of installed, so a track change (or a switch out of
// Liquid Glass) mid-blur never lands a stale picture. Same guard the lyrics
// background uses.
static unsigned glass_generation;

// The sleeve the blur in flight (or installed) was made from, as its pixel
// buffer's address. A theme refresh that has not changed the picture asks
// again here and skips the work; the buffer address moves on every decode, so
// a new sleeve is never mistaken for the old one.
static const void *glass_last_src;

static bool glass_ready;

typedef struct {
	unsigned generation;
	int w, h; // the screen
	cover_image_t src;
	cover_image_t out;
} glass_job_t;

// The ambient ground under the artwork: the palette's page colour with a faint
// lift towards the top, so a track with no sleeve still sits on something with
// a little depth rather than a flat black. The blurred picture, when there is
// one, covers it edge to edge.
static void glass_base_paint(void) {
	lv_obj_t *bottom = lv_layer_bottom();
	if (!bottom) {
		return;
	}
	// A wallpaper the main menu always sits on, even before any track has
	// artwork: the deep ground lifted towards the accent at the top and eased
	// back to black at the foot, a soft colour wash rather than a flat panel.
	// The blurred sleeve, when there is one, covers it edge to edge.
	lv_color_t base = theme()->screen_bg;
	lv_color_t top = lv_color_mix(theme()->accent, base, LV_OPA_30);
	top = lv_color_mix(lv_color_white(), top, LV_OPA_10);
	lv_obj_set_style_bg_opa(bottom, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bottom, top, 0);
	lv_obj_set_style_bg_grad_color(bottom, base, 0);
	lv_obj_set_style_bg_grad_dir(bottom, LV_GRAD_DIR_VER, 0);
}

// One soft colour glow in the wallpaper, positioned and sized as fractions of
// the screen so the field scales with the panel.
typedef struct {
	float cx, cy, r;
	uint8_t cr, cg, cb;
	float strength;
} glass_glow_t;

// A fixed iOS-style wallpaper: magenta and violet up top, blue through the
// middle, teal and cyan low down, over a deep navy ground. Smooth radial
// falloffs keep it looking like a blurred photograph rather than flat paint,
// so the 20%-white cards over it read as frosted glass.
static void glass_render_aurora(uint8_t *px, int w, int h) {
	static const glass_glow_t glows[] = {
		{0.88f, 0.10f, 0.95f, 255, 70, 150, 0.95f},  // magenta, top right
		{0.12f, 0.08f, 0.80f, 120, 70, 255, 0.75f},  // violet, top left
		{0.50f, 0.45f, 0.95f, 30, 120, 255, 0.65f},  // blue, centre
		{0.12f, 0.92f, 0.95f, 30, 220, 150, 0.90f},  // teal, bottom left
		{0.90f, 0.82f, 0.85f, 60, 200, 255, 0.60f},  // cyan, bottom right
	};
	const float base_r = 12, base_g = 14, base_b = 28;
	for (int y = 0; y < h; y++) {
		float fy = (float)y / (float)h;
		for (int x = 0; x < w; x++) {
			float fx = (float)x / (float)w;
			float r = base_r, g = base_g, b = base_b;
			for (unsigned i = 0; i < sizeof(glows) / sizeof(glows[0]); i++) {
				const glass_glow_t *gl = &glows[i];
				float dx = (fx - gl->cx) / gl->r;
				float dy = (fy - gl->cy) / gl->r;
				float d = sqrtf(dx * dx + dy * dy);
				float t = 1.0f - d;
				if (t <= 0.0f) {
					continue;
				}
				t = t * t * (3.0f - 2.0f * t); // smoothstep falloff
				float a = t * gl->strength;
				r += (float)gl->cr * a;
				g += (float)gl->cg * a;
				b += (float)gl->cb * a;
			}
			// A touch below full brightness so the field reads as a wallpaper
			// rather than a lamp; the smoked frost handles label contrast.
			r *= 0.85f;
			g *= 0.85f;
			b *= 0.85f;
			if (r > 255) r = 255;
			if (g > 255) g = 255;
			if (b > 255) b = 255;
			uint16_t c = (uint16_t)(((int)r >> 3) << 11 | ((int)g >> 2) << 5 | ((int)b >> 3));
			px[(y * w + x) * 2] = (uint8_t)(c & 0xFF);
			px[(y * w + x) * 2 + 1] = (uint8_t)(c >> 8);
		}
	}
}

static void glass_ambient_free(void) {
	if (glass_ambient_img) {
		lv_image_set_src(glass_ambient_img, NULL);
		lv_obj_set_hidden(glass_ambient_img, true);
	}
	cover_free(&glass_ambient);
	glass_ambient_w = glass_ambient_h = 0;
}

// Ensures the wallpaper image exists and is showing at the current screen
// size. The blurred sleeve, when present, is drawn on top of it.
static void glass_ambient_paint(void) {
	lv_obj_t *bottom = lv_layer_bottom();
	if (!bottom) {
		return;
	}
	int w = (int)lv_display_get_horizontal_resolution(lv_display_get_default());
	int h = (int)lv_display_get_vertical_resolution(lv_display_get_default());
	if (w <= 0 || h <= 0) {
		return;
	}
	if (!glass_ambient_img) {
		glass_ambient_img = lv_image_create(bottom);
		lv_obj_set_clickable(glass_ambient_img, false);
		lv_obj_move_to_index(glass_ambient_img, 0); // behind the sleeve blur
	}
	if (glass_ambient_w != w || glass_ambient_h != h || !glass_ambient.pixels) {
		cover_free(&glass_ambient);
		memset(&glass_ambient, 0, sizeof(glass_ambient));
		glass_ambient.pixels = malloc((size_t)w * h * 2);
		if (glass_ambient.pixels) {
			glass_render_aurora(glass_ambient.pixels, w, h);
			glass_ambient.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
			glass_ambient.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
			glass_ambient.dsc.header.w = w;
			glass_ambient.dsc.header.h = h;
			glass_ambient.dsc.header.stride = w * 2;
			glass_ambient.dsc.data = glass_ambient.pixels;
			glass_ambient.dsc.data_size = (size_t)w * h * 2;
			glass_ambient_w = w;
			glass_ambient_h = h;
		}
	}
	if (glass_ambient.pixels) {
		lv_obj_set_size(glass_ambient_img, w, h);
		lv_obj_align(glass_ambient_img, LV_ALIGN_TOP_LEFT, 0, 0);
		lv_image_set_src(glass_ambient_img, &glass_ambient.dsc);
		lv_obj_set_hidden(glass_ambient_img, false);
	}
}

// Drops the picture and discards any blur still being made of it. The ambient
// ground stays; this is what shows between tracks and outside Liquid Glass.
static void glass_clear(void) {
	glass_generation++;
	glass_last_src = NULL;
	if (glass_img) {
		lv_image_set_src(glass_img, NULL);
		lv_obj_set_hidden(glass_img, true);
	}
	cover_free(&glass_pic);
}

// The worker's answer, back on the UI thread through gui_post.
static void glass_arrived(void *user) {
	glass_job_t *job = user;
	if (job->generation == glass_generation && job->out.pixels && glass_img) {
		cover_free(&glass_pic);
		glass_pic = job->out;
		glass_pic.dsc.data = glass_pic.pixels; // struct moved, buffer didn't
		lv_image_set_src(glass_img, &glass_pic.dsc);
		lv_obj_set_hidden(glass_img, false);
		lv_obj_invalidate(glass_img);
	} else {
		cover_free(&job->out);
		if (job->generation == glass_generation) {
			glass_last_src = NULL; // the blur failed; let the next ask retry
		}
	}
	free(job);
}

static void *glass_worker(void *arg) {
	glass_job_t *job = arg;
	cover_blur_fill(&job->src, job->w, job->h, &job->out);
	cover_free(&job->src);
	if (!gui_post(glass_arrived, job)) {
		cover_free(&job->out);
		free(job);
	}
	return NULL;
}

// Copies the sleeve on screen and blurs it at the shape of the screen, off the
// UI thread. The copy is what makes this safe: the player frees its own buffer
// at the next track change, but the job holds its own.
static void glass_start_blur(const lv_image_dsc_t *dsc) {
	if (!glass_img || !dsc || !dsc->data || dsc->data_size == 0) {
		return;
	}
	glass_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->src.dsc = *dsc;
	job->src.pixels = malloc(dsc->data_size);
	if (!job->src.pixels) {
		free(job);
		return;
	}
	memcpy(job->src.pixels, dsc->data, dsc->data_size);
	job->src.dsc.data = job->src.pixels;
	job->w = (int)lv_display_get_horizontal_resolution(lv_display_get_default());
	job->h = (int)lv_display_get_vertical_resolution(lv_display_get_default());
	job->generation = ++glass_generation; // a newer blur supersedes the old
	glass_last_src = dsc->data;

	pthread_t thread;
	if (pthread_create(&thread, NULL, glass_worker, job) == 0) {
		pthread_detach(thread);
	} else {
		cover_free(&job->src);
		free(job);
		glass_last_src = NULL;
	}
}

// Brings the backdrop in line with the theme and the sleeve on screen. Run on
// the UI thread, from the theme-refresh list and the player's artwork observer.
static void glass_sync(void) {
	if (!glass_ready) {
		return;
	}
	if (!theme_is_liquid_glass()) {
		glass_clear(); // hidden behind the opaque pages of every other theme
		glass_ambient_free();
		return;
	}
	glass_base_paint();
	glass_ambient_paint();

	const lv_image_dsc_t *dsc = player_cover_image();
	if (!dsc || !dsc->data) {
		glass_clear(); // no artwork: the ambient ground on its own
		return;
	}
	if (dsc->data == glass_last_src && glass_pic.pixels) {
		lv_obj_set_hidden(glass_img, false); // already the right picture
		return;
	}
	glass_start_blur(dsc);
}

void glass_init(void) {
	lv_obj_t *bottom = lv_layer_bottom();
	if (!bottom) {
		return;
	}
	glass_img = lv_image_create(bottom);
	lv_obj_set_size(glass_img, (int)lv_display_get_horizontal_resolution(lv_display_get_default()),
					(int)lv_display_get_vertical_resolution(lv_display_get_default()));
	lv_obj_align(glass_img, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_clickable(glass_img, false);
	lv_obj_set_hidden(glass_img, true);

	glass_ready = true;
	theme_register_refresh(glass_sync);
	player_register_cover_observer(glass_sync);
	glass_sync();
}
