/*
 * Headless LVGL layout and workflow regression checks for the LRCLIB lyrics
 * search & preview screen (lyricsearch.c) on the TempoTec V1 240x320 panel:
 * - all lyrics_* translation keys across all 11 shipped V1 languages
 * - list + preview + Apply pill bounds within the 240x320 panel across all 11
 *   languages and all 5 V1 themes (Dark, Light, Tokyo Night, Amber on Black,
 *   OLED), including OLED's dark-on-white Apply pill contrast
 * - LRC tag/timestamp stripping in preview, back-guard navigation from preview
 *   to list, drag-active click suppression, and Apply writing the sidecar .lrc
 *   and reloading player lyrics.
 */

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/lyricsearch.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/lrclib.h"

/* ---- fonts --------------------------------------------------------------- */

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;
lv_font_t font_ui_36_bold, font_ui_64_bold, font_ui_72;
static void (*font_callbacks[16])(void);
static int font_callback_count;
static int text_size = FONTS_TEXT_NORMAL;

static void init_fonts(void) {
	lv_font_t *all[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold, &font_ui_22, &font_ui_22_bold,
		&font_ui_24, &font_ui_24_bold, &font_ui_26, &font_ui_28, &font_ui_32, &font_ui_36_bold,
		&font_ui_64_bold, &font_ui_72,
	};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		*all[i] = lv_font_montserrat_14;
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

/* ---- shell & player stubs ------------------------------------------------ */

void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool bootpic_present(void) { return false; }

static char last_error_toast[128];
static char last_success_toast[128];
void toast_error(const char *text) { snprintf(last_error_toast, sizeof(last_error_toast), "%s", text ? text : ""); }
void toast_plain(const char *text) { (void)text; }
void toast_success(const char *text) {
	snprintf(last_success_toast, sizeof(last_success_toast), "%s", text ? text : "");
}
void toast_glyph(const lv_image_dsc_t *glyph, const char *text) {
	(void)glyph;
	(void)text;
}

static bool sheet_drag;
static bool back_drag;
static int sheet_closed_calls;
static int player_return_calls;
static char reloaded_lyrics_path[1024];

bool player_sheet_drag_active(void) { return sheet_drag; }
bool switcher_back_drag_active(void) { return back_drag; }
void player_sheet_close(bool animated) {
	(void)animated;
	sheet_closed_calls++;
}
void player_lyrics_reload(const char *path) {
	snprintf(reloaded_lyrics_path, sizeof(reloaded_lyrics_path), "%s", path ? path : "");
}

static lv_obj_t *home_screen;
static bool (*guard_fn)(void);
static bool (*peek_fn)(void);

void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
void switcher_set_player_return(lv_obj_t *screen) {
	if (screen == lyricsearch_screen) {
		player_return_calls++;
	}
}
void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) {
	assert(screen == lyricsearch_screen);
	guard_fn = handler;
}
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) {
	assert(screen == lyricsearch_screen);
	peek_fn = peek;
}
void back_btn_cb(lv_event_t *e) {
	(void)e;
	if (guard_fn && guard_fn()) {
		return;
	}
	lv_screen_load(home_screen);
}

/* Stub HTTP symbols unused by lrclib_save/lrclib_free in this UI binary */
char *http_url_encode(const char *s, char *out, size_t n) {
	snprintf(out, n, "%s", s ? s : "");
	return out;
}
const char *http_last_error(void) { return ""; }
bool http_get_ex(const char *url, const char *h, bool ins, char **out, size_t *len, size_t lim, int tmo, int *st) {
	(void)url;
	(void)h;
	(void)ins;
	(void)out;
	(void)len;
	(void)lim;
	(void)tmo;
	if (st) {
		*st = 0;
	}
	return false;
}

/* ---- helpers ------------------------------------------------------------- */

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_is_hidden(obj)) {
		return NULL;
	}
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

static lv_obj_t *find_label_prefix(lv_obj_t *obj, const char *prefix) {
	if (lv_obj_is_hidden(obj)) {
		return NULL;
	}
	if (lv_obj_check_type(obj, &lv_label_class) && strncmp(lv_label_get_text(obj), prefix, strlen(prefix)) == 0) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label_prefix(lv_obj_get_child(obj, i), prefix);
		if (found) {
			return found;
		}
	}
	return NULL;
}

static lv_obj_t *button_of_label_prefix(lv_obj_t *root, const char *prefix) {
	lv_obj_t *label = find_label_prefix(root, prefix);
	if (!label) {
		fprintf(stderr, "label starting with '%s' not found\n", prefix);
	}
	assert(label);
	lv_obj_t *button = lv_obj_get_parent(label);
	while (button && !lv_obj_check_type(button, &lv_button_class)) {
		button = lv_obj_get_parent(button);
	}
	assert(button);
	return button;
}

static void check_within_panel_in(lv_obj_t *obj, gui_config_t *cfg, const char *what, bool scrolls) {
	if (lv_obj_is_hidden(obj)) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	bool overflow_x = area.x1 < 0 || area.x2 >= (int32_t)cfg->screen_width;
	bool overflow_y = area.y1 < 0 || area.y2 >= (int32_t)cfg->screen_height;
	if (overflow_x || (overflow_y && !scrolls)) {
		fprintf(stderr, "%s: object at %d,%d..%d,%d overflows %dx%d panel\n", what, area.x1, area.y1, area.x2, area.y2,
				(int)cfg->screen_width, (int)cfg->screen_height);
		assert(!"an object left the panel");
	}
	if (lv_obj_get_scroll_dir(obj) & LV_DIR_VER) {
		scrolls = true;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		check_within_panel_in(lv_obj_get_child(obj, i), cfg, what, scrolls);
	}
}

static bool is_ini(const char *name) {
	size_t n = strlen(name);
	return n > 4 && strcmp(name + n - 4, ".ini") == 0;
}

static lrclib_record_t *sample_records(int *count_out) {
	*count_out = 2;
	lrclib_record_t *recs = calloc(2, sizeof(*recs));
	assert(recs);
	snprintf(recs[0].track, sizeof(recs[0].track), "Everything In Its Right Place");
	snprintf(recs[0].artist, sizeof(recs[0].artist), "Radiohead");
	snprintf(recs[0].album, sizeof(recs[0].album), "Kid A");
	recs[0].duration = 251;
	recs[0].synced = true;
	recs[0].text = strdup("[ar:Radiohead]\n[ti:Everything In Its Right Place]\n"
						  "[00:01.00][00:05.00] Everything\n\n[00:12.34] In its right place\r\n");
	assert(recs[0].text);

	snprintf(recs[1].track, sizeof(recs[1].track), "Short Track");
	snprintf(recs[1].artist, sizeof(recs[1].artist), "Solo Performer");
	recs[1].duration = 95;
	recs[1].synced = false;
	recs[1].text = strdup("Untimed line one\nUntimed line two");
	assert(recs[1].text);
	return recs;
}

static void check_all_languages_and_themes(const char *language_dir, gui_config_t *cfg, const char *track_path) {
	static const char *const KEYS[] = {
		"lyrics_none",			  "lyrics_download_title", "lyrics_download_note", "lyrics_download",
		"lyrics_downloading",	  "lyrics_downloaded",	   "lyrics_not_found",	   "lyrics_instrumental",
		"lyrics_no_tags",		  "lyrics_not_saved",	   "lyrics_download_failed", "lyrics_no_wifi",
		"lyrics_wifi_off_note",	  "lyrics_enable_wifi",	   "lyrics_wifi_connecting", "lyrics_wifi_failed",
		"lyrics_results",		  "lyrics_synced",		   "lyrics_plain",		   "lyrics_apply",
		"lyrics_search_title_only",
	};

	DIR *dir = opendir(language_dir);
	assert(dir);
	int languages = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (!is_ini(entry->d_name)) {
			continue;
		}
		char lang_name[256];
		snprintf(lang_name, sizeof(lang_name), "%.*s", (int)(strlen(entry->d_name) - 4), entry->d_name);
		assert(lang_set(lang_name));

		for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
			const char *text = tr(KEYS[i]);
			if (!text || !text[0] || strcmp(text, KEYS[i]) == 0) {
				fprintf(stderr, "%s: \"%s\" is missing\n", lang_name, KEYS[i]);
				assert(!"missing lyrics translation");
			}
			assert(strlen(text) < 220);
		}

		for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
			theme_set_kind((theme_kind_t)kind);
			int count = 0;
			lrclib_record_t *recs = sample_records(&count);
			lyricsearch_open(track_path, recs, count);
			lv_obj_update_layout(lyricsearch_screen);

			char ctx[300];
			snprintf(ctx, sizeof(ctx), "%s/theme-%d/list", lang_name, kind);
			check_within_panel_in(lyricsearch_screen, cfg, ctx, false);

			// Ensure the left title/artist column still has healthy width next to
			// localized Synced / Text only labels on the 240px row.
			lv_obj_t *row0 = button_of_label_prefix(lyricsearch_screen, "Every");
			assert(find_label_prefix(lyricsearch_screen, "Short") != NULL);
			lv_obj_t *names_col = lv_obj_get_child(row0, 0);
			if (lv_obj_get_width(names_col) < 70) {
				fprintf(stderr, "%s: names_col width=%d\n", lang_name, (int)lv_obj_get_width(names_col));
			}
			assert(lv_obj_get_width(names_col) >= 70);

			// Open preview and verify geometry + OLED Apply pill contrast.
			lv_obj_send_event(row0, LV_EVENT_CLICKED, NULL);
			lv_obj_update_layout(lyricsearch_screen);
			assert(peek_fn && peek_fn());
			snprintf(ctx, sizeof(ctx), "%s/theme-%d/preview", lang_name, kind);
			check_within_panel_in(lyricsearch_screen, cfg, ctx, false);

			lv_obj_t *apply_lbl = find_label(lyricsearch_screen, tr("lyrics_apply"));
			assert(apply_lbl);
			lv_obj_t *apply_btn = lv_obj_get_parent(apply_lbl);
			lv_color_t bg = lv_obj_get_style_bg_color(apply_btn, LV_PART_MAIN);
			lv_color_t fg = lv_obj_get_style_text_color(apply_lbl, LV_PART_MAIN);
			assert(!lv_color_eq(bg, fg));

			// Back returns to list.
			back_btn_cb(NULL);
			assert(!peek_fn());
		}
		languages++;
	}
	closedir(dir);
	assert(languages == 11);
	assert(lang_set("English"));
	theme_set_kind(THEME_KIND_TOKYO_NIGHT);
}

int main(int argc, char **argv) {
	assert(argc == 2);

	char card[] = "/tmp/sonix-lyricsearch-ui-XXXXXX";
	assert(mkdtemp(card));
	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/device_config.ini", card);
	config_init(config_path);
	config_set("ui", "language", "English");
	lang_init(argv[1]);

	init_fonts();
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), card};
	assert(cfg.screen_width == 240 && cfg.screen_height == 320);
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();

	home_screen = lv_obj_create(NULL);
	lyricsearch_screen = lv_obj_create(NULL);
	lyricsearch_init(&cfg);
	lv_screen_load(home_screen);

	char track_path[512], lrc_path[512];
	snprintf(track_path, sizeof(track_path), "%s/01 - Everything.flac", card);
	snprintf(lrc_path, sizeof(lrc_path), "%s/01 - Everything.lrc", card);

	check_all_languages_and_themes(argv[1], &cfg, track_path);

	// Workflow check: drag guard, LRC stripping, and Apply saving .lrc + reloading player.
	int count = 0;
	lrclib_record_t *recs = sample_records(&count);
	sheet_closed_calls = 0;
	player_return_calls = 0;
	lyricsearch_open(track_path, recs, count);
	assert(lv_screen_active() == lyricsearch_screen);
	assert(sheet_closed_calls == 1 && player_return_calls == 1);
	lv_obj_update_layout(lyricsearch_screen);

	lv_obj_t *row0 = button_of_label_prefix(lyricsearch_screen, "Everything");
	back_drag = true;
	lv_obj_send_event(row0, LV_EVENT_CLICKED, NULL);
	assert(!peek_fn()); // ignored during swipe-back drag
	back_drag = false;

	lv_obj_send_event(row0, LV_EVENT_CLICKED, NULL);
	assert(peek_fn());
	// Tags ([ar:...], [ti:...]) and timestamps ([00:01.00]) stripped; blank verse line preserved.
	assert(find_label(lyricsearch_screen, "Everything\n\nIn its right place") != NULL);

	lv_obj_t *apply_btn = button_of_label_prefix(lyricsearch_screen, tr("lyrics_apply"));
	last_success_toast[0] = '\0';
	reloaded_lyrics_path[0] = '\0';
	lv_obj_send_event(apply_btn, LV_EVENT_CLICKED, NULL);

	assert(lv_screen_active() == home_screen);
	assert(strcmp(reloaded_lyrics_path, track_path) == 0);
	assert(strcmp(last_success_toast, "lyrics_downloaded") == 0);

	struct stat st;
	assert(stat(lrc_path, &st) == 0 && st.st_size > 0);
	unlink(lrc_path);
	unlink(config_path);

	lv_deinit();
	rmdir(card);
	printf("PASS lyricsearch (%dx%d, 11 languages, 5 themes)\n", (int)cfg.screen_width, (int)cfg.screen_height);
	return 0;
}
