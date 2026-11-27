/* Liquid Glass: the theme wiring (kind, persistence, the translucent shared
 * styles) and the backdrop module (glass.c) -- its artwork following, the
 * generation guard that drops a blur superseded mid-flight, and the ambient
 * fallback. Real LVGL and the real theme engine; the player, the cover blur and
 * the cross-thread post are substituted so the module's own logic is what runs. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/glass.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"

// --- the player's artwork, lent on demand -----------------------------------
static const lv_image_dsc_t *stub_cover;
static void (*stub_observer)(void);

const lv_image_dsc_t *player_cover_image(void) { return stub_cover; }
void player_register_cover_observer(void (*cb)(void)) { stub_observer = cb; }

// --- the cover blur, stubbed to a solid pane so installation is observable --
static int blur_calls;
bool cover_blur_fill(const cover_image_t *src, int out_w, int out_h, cover_image_t *out) {
	(void)src;
	memset(out, 0, sizeof(*out));
	size_t size = (size_t)out_w * out_h * 2;
	out->pixels = malloc(size);
	assert(out->pixels);
	memset(out->pixels, 0x5A, size); // a marker the installed picture carries
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = out_w;
	out->dsc.header.h = out_h;
	out->dsc.header.stride = out_w * 2;
	out->dsc.data = out->pixels;
	out->dsc.data_size = size;
	blur_calls++;
	return true;
}
void cover_free(cover_image_t *img) {
	if (!img) {
		return;
	}
	free(img->pixels);
	memset(img, 0, sizeof(*img));
}

// --- gui_post, stubbed to a queue the test drains on its own thread ---------
static pthread_mutex_t post_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
	void (*cb)(void *);
	void *user;
} post_queue[16];
static int post_count;

bool gui_post(void (*cb)(void *), void *user) {
	pthread_mutex_lock(&post_mu);
	if (post_count >= 16) {
		pthread_mutex_unlock(&post_mu);
		return false;
	}
	post_queue[post_count].cb = cb;
	post_queue[post_count].user = user;
	post_count++;
	pthread_mutex_unlock(&post_mu);
	return true;
}

static void wait_posts(int n) {
	for (int i = 0; i < 4000; i++) {
		pthread_mutex_lock(&post_mu);
		int c = post_count;
		pthread_mutex_unlock(&post_mu);
		if (c >= n) {
			return;
		}
		usleep(1000);
	}
	assert(0 && "the blur worker never posted its answer");
}

static void flush_posts(void) {
	for (;;) {
		pthread_mutex_lock(&post_mu);
		if (post_count == 0) {
			pthread_mutex_unlock(&post_mu);
			return;
		}
		void (*cb)(void *) = post_queue[0].cb;
		void *user = post_queue[0].user;
		for (int i = 1; i < post_count; i++) {
			post_queue[i - 1] = post_queue[i];
		}
		post_count--;
		pthread_mutex_unlock(&post_mu);
		cb(user);
	}
}

// LVGL's configured default font; the theme and backdrop draw no text, so a
// built-in face stands in for the firmware's UI fonts.
lv_font_t font_ui_14;

// --- the boot marker the theme writes, stubbed ------------------------------
void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool bootpic_present(void) { return false; }

// --- helpers ----------------------------------------------------------------
static lv_obj_t *glass_image(void) {
	lv_obj_t *bottom = lv_layer_bottom();
	for (uint32_t i = 0; i < lv_obj_get_child_count(bottom); i++) {
		lv_obj_t *child = lv_obj_get_child(bottom, (int32_t)i);
		if (lv_obj_check_type(child, &lv_image_class)) {
			return child;
		}
	}
	return NULL;
}

// Reads a shared style's opacity through LVGL's own cascade: a bare probe
// object carrying just that style, so the value is what a widget would draw.
static lv_opa_t style_bg_opa(const lv_style_t *style) {
	lv_obj_t *probe = lv_obj_create(lv_screen_active());
	lv_obj_remove_style_all(probe);
	lv_obj_add_style(probe, style, 0);
	lv_opa_t opa = lv_obj_get_style_bg_opa(probe, LV_PART_MAIN);
	lv_obj_delete(probe);
	return opa;
}

// A small sleeve to lend the backdrop. Two distinct buffers so a track change
// is a change of address, exactly as a real decode produces.
static lv_image_dsc_t make_cover(uint8_t fill) {
	static uint8_t buf[3][4 * 4 * 2];
	int idx = (fill >= 1 && fill <= 3) ? fill - 1 : 0;
	memset(buf[idx], fill, sizeof(buf[idx]));
	lv_image_dsc_t dsc;
	memset(&dsc, 0, sizeof(dsc));
	dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	dsc.header.w = 4;
	dsc.header.h = 4;
	dsc.header.stride = 4 * 2;
	dsc.data = buf[idx];
	dsc.data_size = sizeof(buf[idx]);
	return dsc;
}

int main(void) {
	char dir[] = "/tmp/sonix-glass-test-XXXXXX";
	assert(mkdtemp(dir));
	char config[256];
	snprintf(config, sizeof(config), "%s/device_config.ini", dir);
	config_init(config); // a fresh path starts from defaults (returns false)

	lv_init();
	font_ui_14 = lv_font_montserrat_14;
	lv_display_create(240, 320);
	theme_init();
	glass_init();

	lv_obj_t *img = glass_image();
	assert(img); // the backdrop owns an image on the bottom layer
	assert(lv_obj_is_hidden(img)); // Dark: nothing to show

	// --- selecting Liquid Glass --------------------------------------------
	theme_set_kind(THEME_KIND_LIQUID_GLASS);
	assert(theme_is_liquid_glass());
	assert(theme_get_kind() == THEME_KIND_LIQUID_GLASS);
	assert(strcmp(config_get("ui", "theme", ""), "liquid-glass") == 0);
	assert(theme()->dark);
	// the iOS system blue is its native accent
	assert(theme()->accent.red == 10 && theme()->accent.green == 132 && theme()->accent.blue == 255);
	// the page is clear; the surfaces are films, not solid fills
	assert(style_bg_opa(&theme_style_screen) == LV_OPA_TRANSP);
	assert(style_bg_opa(&theme_style_panel) == THEME_GLASS_PANEL_OPA);
	assert(style_bg_opa(&theme_style_card) == THEME_GLASS_CARD_OPA);
	assert(style_bg_opa(&theme_style_card_pressed) == THEME_GLASS_CARD_PRESSED_OPA);
	assert(style_bg_opa(&theme_style_accent_bg) == THEME_GLASS_ACCENT_OPA);
	assert(lv_obj_is_hidden(img)); // still no artwork: the ambient ground alone

	// --- a sleeve lands: the backdrop blurs it ------------------------------
	lv_image_dsc_t a = make_cover(1);
	stub_cover = &a;
	assert(stub_observer);
	stub_observer();
	wait_posts(1);
	flush_posts();
	assert(!lv_obj_is_hidden(img));
	assert(lv_image_get_src(img) != NULL);
	assert(blur_calls == 1);

	// the same picture again costs nothing
	stub_observer();
	assert(blur_calls == 1);
	assert(!lv_obj_is_hidden(img));

	// --- a new sleeve supersedes the old ------------------------------------
	lv_image_dsc_t b = make_cover(2);
	stub_cover = &b;
	stub_observer();
	wait_posts(1);
	flush_posts();
	assert(blur_calls == 2);
	assert(!lv_obj_is_hidden(img));

	// --- a blur superseded mid-flight is dropped ----------------------------
	lv_image_dsc_t c = make_cover(3);
	stub_cover = &c;
	stub_observer(); // starts a blur (generation N)
	wait_posts(1);
	stub_cover = NULL;
	stub_observer(); // the sleeve goes away before the answer is drained (N+1)
	flush_posts();
	assert(lv_obj_is_hidden(img)); // the stale answer was freed, not installed
	assert(lv_image_get_src(img) == NULL);

	// --- leaving Liquid Glass restores the opaque styles --------------------
	theme_set_kind(THEME_KIND_DARK);
	assert(!theme_is_liquid_glass());
	assert(lv_obj_is_hidden(img));
	assert(style_bg_opa(&theme_style_screen) == LV_OPA_COVER);
	assert(style_bg_opa(&theme_style_panel) == LV_OPA_COVER);
	assert(style_bg_opa(&theme_style_card) == LV_OPA_COVER);
	assert(style_bg_opa(&theme_style_accent_bg) == LV_OPA_COVER);

	// --- the choice survives a restart --------------------------------------
	theme_set_kind(THEME_KIND_LIQUID_GLASS);
	assert(config_save());
	assert(config_init(config));
	theme_init();
	assert(theme_get_kind() == THEME_KIND_LIQUID_GLASS);
	assert(theme_is_liquid_glass());
	assert(style_bg_opa(&theme_style_screen) == LV_OPA_TRANSP);

	printf("PASS liquid-glass theme and backdrop\n");
	return 0;
}
