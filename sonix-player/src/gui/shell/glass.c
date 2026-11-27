#include "glass.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/glasswallpaper.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/theme.h"

// The blur currently installed on the bottom layer, owned here. Freed and
// replaced when a new one lands, and dropped when the sleeve goes away.
static cover_image_t glass_pic;
static lv_obj_t *glass_img;

// The always-on wallpaper under everything: the supplied abstract-glass art,
// shown so the frosted panels have something to show through even with no
// track loaded. Static art, no allocation; hidden when the theme leaves glass.
static lv_obj_t *glass_ambient_img;

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

static void glass_ambient_free(void) {
	if (glass_ambient_img) {
		lv_image_set_src(glass_ambient_img, NULL);
		lv_obj_set_hidden(glass_ambient_img, true);
	}
}

// Shows the supplied abstract-glass wallpaper on the bottom layer, scaled to
// cover the panel. It is static art (no allocation), always present while glass
// is active so the frosted panels have something to show through even with no
// track; the blurred sleeve, when present, is drawn on top of it.
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
	lv_obj_set_size(glass_ambient_img, w, h);
	lv_obj_align(glass_ambient_img, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_image_set_src(glass_ambient_img, &glass_wallpaper_dsc);
	// Cover the panel: scale by the larger axis ratio so no edge is left bare.
	uint32_t sx = (uint32_t)(LV_SCALE_NONE * w / (int)glass_wallpaper_dsc.header.w);
	uint32_t sy = (uint32_t)(LV_SCALE_NONE * h / (int)glass_wallpaper_dsc.header.h);
	lv_image_set_scale(glass_ambient_img, sx > sy ? sx : sy);
	lv_image_set_inner_align(glass_ambient_img, LV_IMAGE_ALIGN_CENTER);
	// Only a whisper of dim: iOS 26 clear glass shows the wallpaper almost at
	// full strength, so the art stays vivid through the clear tiles.
	lv_obj_set_style_image_recolor(glass_ambient_img, lv_color_black(), 0);
	lv_obj_set_style_image_recolor_opa(glass_ambient_img, LV_OPA_10, 0);
	lv_obj_set_hidden(glass_ambient_img, false);
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
