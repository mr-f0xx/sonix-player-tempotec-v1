/*
 * The three games in the More menu -- 2048, Snake and Dino -- on the real
 * renderer and the real theme code.
 *
 * What is checked here is the contract the games keep with the rest of the
 * interface, which is what was missing when they were taken out for a freeze on
 * theme change:
 *
 *   - Nothing is built at start-up. A game's page is built the first time it is
 *     opened, so a player who never opens one pays nothing for it.
 *   - A page is built once, and stays built: a second visit reuses it.
 *   - A palette change repaints the page on screen at once.
 *   - A palette change does not walk a page that is not on screen. That page
 *     keeps the colours it had and is repainted the next time it is shown, so
 *     the first frame the player sees is in the palette now in force.
 *
 * The games' own play (the rules, the clocks and the touches) has its own
 * behaviour and is not what this file is about.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/games/dino.h"
#include "src/gui/games/game2048.h"
#include "src/gui/games/snake.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

/* ---- the parts of the shell the games lean on, substituted --------------- */

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;
lv_font_t font_ui_36_bold;
static void (*font_callbacks[16])(void);
static int font_callback_count;
static int text_size = FONTS_TEXT_NORMAL;

static void init_fonts(void) {
	lv_font_t *small[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold,
		&font_ui_22, &font_ui_22_bold, &font_ui_24, &font_ui_24_bold,
		&font_ui_26, &font_ui_28, &font_ui_32,
	};
	for (size_t i = 0; i < sizeof(small) / sizeof(small[0]); i++) {
		*small[i] = lv_font_montserrat_14;
	}
	lv_font_t *large[] = {&font_ui_36_bold};
	for (size_t i = 0; i < sizeof(large) / sizeof(large[0]); i++) {
		*large[i] = lv_font_montserrat_20;
	}
}

void fonts_register_change(void (*cb)(void)) {
	assert(font_callback_count < 16);
	font_callbacks[font_callback_count++] = cb;
}
int fonts_text_size(void) { return text_size; }
bool fonts_set_text_size(int size) {
	text_size = size;
	for (int i = 0; i < font_callback_count; i++) {
		font_callbacks[i]();
	}
	return true;
}
const char *fonts_choice(void) { return "default"; }
bool fonts_set_face(const char *id) { return strcmp(id, "default") == 0; }
int fonts_choices(const char **ids, const char **labels, int max) {
	assert(max > 0);
	ids[0] = "default";
	labels[0] = "Test font";
	return 1;
}

void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool bootpic_present(void) { return false; }

void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
void back_btn_cb(lv_event_t *e) { (void)e; }
void back_btn_force_hidden(bool hidden) { (void)hidden; }
void topbar_set_hidden(bool hidden) { (void)hidden; }
void volume_overlay_allow_outside_player(bool allow) { (void)allow; }
void power_pause_in_standby(lv_timer_t *timer) { (void)timer; }

void gui_notify_popup(const char *text) {
	fprintf(stderr, "popup: %s\n", text ? text : "");
	assert(!"unexpected popup");
}
void toast_error(const char *text) {
	fprintf(stderr, "error: %s\n", text ? text : "");
	assert(!"unexpected error toast");
}
void toast_plain(const char *text) { (void)text; }
void toast_success(const char *text) { (void)text; }

/* ---- the checks ---------------------------------------------------------- */

static lv_color_t screen_bg(lv_obj_t *screen) { return lv_obj_get_style_bg_color(screen, LV_PART_MAIN); }

static bool bg_is_now(lv_obj_t *screen) { return lv_color_eq(screen_bg(screen), theme()->screen_bg); }

int main(int argc, char **argv) {
	assert(argc == 2);

	char card[] = "/tmp/sonix-games-test-XXXXXX";
	assert(mkdtemp(card));
	char config[512];
	snprintf(config, sizeof(config), "%s/device_config.ini", card);
	config_init(config);
	config_set("ui", "language", "English");
	lang_init(argv[1]);

	init_fonts();
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), card};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();
	theme_set_kind(THEME_KIND_DARK);

	lv_obj_t *home = lv_obj_create(NULL);
	lv_screen_load(home);

	/* Start-up: the games take their configuration and build nothing. */
	game2048_init(&cfg);
	snake_init(&cfg);
	dino_init(&cfg);
	assert(!game2048_screen && !snake_screen && !dino_screen);

	/* The first open builds the page and puts it on the glass. */
	snake_open();
	assert(snake_screen && lv_screen_active() == snake_screen);
	assert(bg_is_now(snake_screen));
	lv_obj_t *first = snake_screen;
	lv_screen_load(home);

	/* A second visit reuses that page; nothing is built again. */
	snake_open();
	assert(snake_screen == first && lv_screen_active() == snake_screen);
	assert(bg_is_now(snake_screen));
	lv_screen_load(home);
	assert(!dino_screen && !game2048_screen); /* the other two are still unbuilt */

	/* A palette change while the page is on screen repaints it at once. */
	snake_open();
	theme_set_kind(THEME_KIND_LIGHT);
	assert(bg_is_now(snake_screen));
	lv_screen_load(home);

	/* A page that is not on screen is not walked: it keeps the palette it was
	 * left in, and nothing is repainted behind the player's back. */
	game2048_open();
	assert(game2048_screen && lv_screen_active() == game2048_screen);
	assert(bg_is_now(game2048_screen));
	lv_screen_load(home);
	lv_color_t left_in = screen_bg(game2048_screen);
	theme_set_kind(THEME_KIND_DARK);
	assert(lv_color_eq(screen_bg(game2048_screen), left_in));
	assert(!lv_color_eq(left_in, theme()->screen_bg));

	/* Shown again, it is painted in the palette now in force before the player
	 * sees a frame of it. */
	game2048_open();
	assert(lv_screen_active() == game2048_screen);
	assert(bg_is_now(game2048_screen));
	lv_screen_load(home);

	/* The same holds for a page that was never on screen during a change: the
	 * Dino page is built after the palette has moved, and is in that palette. */
	theme_set_kind(THEME_KIND_OLED);
	dino_open();
	assert(dino_screen && lv_screen_active() == dino_screen);
	assert(bg_is_now(dino_screen));
	lv_screen_load(home);

	printf("PASS games: built on first open, repainted on show, and only the page on screen walked (%dx%d)\n",
	       cfg.screen_width, cfg.screen_height);
	return 0;
}
