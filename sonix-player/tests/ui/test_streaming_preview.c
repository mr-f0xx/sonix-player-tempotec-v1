// The Pages paths through the real 240x320 LVGL menus and pages. No libcurl,
// transfer workers, discovery, or Listen Together backend is linked here.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/networkmusicpage.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/listentogether.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

// Keep unrelated streaming add-ons off: these two destinations have no plugin
// switch and must remain visible even with the rest of Streaming empty.
lv_obj_t *radiopage_screen, *podcast_screen, *soundcloud_screen, *tidal_screen;
lv_obj_t *qobuz_screen, *subsonic_screen, *jellyfin_screen;
bool addons_enabled(const char *id) { assert(id); return false; }
void qobuzpage_init(gui_config_t *cfg) { (void)cfg; abort(); }
void tidalpage_init(gui_config_t *cfg) { (void)cfg; abort(); }
void podcastpage_init(gui_config_t *cfg) { (void)cfg; abort(); }
void subsonicpage_init(gui_config_t *cfg) { (void)cfg; abort(); }
void jellyfinpage_init(gui_config_t *cfg) { (void)cfg; abort(); }
void soundcloudpage_init(gui_config_t *cfg) { (void)cfg; abort(); }

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold, font_ui_26, font_ui_28, font_ui_32;
void fonts_register_change(void (*cb)(void)) { (void)cb; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) { (void)obj; (void)opening; }
bool switcher_back_drag_active(void) { return false; }
bool player_sheet_drag_active(void) { return false; }
bool coverflow_drag_active(void) { return false; }
void switch_screen(lv_obj_t *screen) { assert(screen); lv_screen_load(screen); lv_obj_update_layout(screen); }
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
void switch_screen_return_to(lv_obj_t *screen) { switch_screen(screen); }
void back_btn_cb(lv_event_t *e) { (void)e; switch_screen(streaming_screen); }
void gui_notify_popup(const char *s) { (void)s; abort(); }
void toast_error(const char *s) { (void)s; abort(); }
void toast_plain(const char *s) { (void)s; abort(); }
void popover_show(lv_obj_t *anchor, const popover_item_t *items, int count) {
	(void)anchor; (void)items; (void)count; abort();
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_is_hidden(obj)) return NULL; // Don't accidentally tap the other theme's hidden menu.
	if (lv_obj_check_type(obj, &lv_label_class)) {
		const char *shown = lv_label_get_text(obj);
		if (!strcmp(shown, text)) return obj;
		// The grid ellipsizes long captions with the built-in test font.
		size_t n = strlen(shown);
		if (lv_label_get_long_mode(obj) == LV_LABEL_LONG_DOT && n > 3 && !strcmp(shown + n - 3, "...") &&
			!strncmp(shown, text, n - 3)) return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *match = find_label(lv_obj_get_child(obj, i), text);
		if (match) return match;
	}
	return NULL;
}

static lv_obj_t *button(const char *label) {
	lv_obj_t *text = find_label(lv_screen_active(), label);
	if (!text) fprintf(stderr, "Missing visible label: %s\n", label);
	assert(text);
	return lv_obj_get_parent(text);
}

static void tap(const char *label) {
	lv_obj_t *btn = button(label);
	assert(!lv_obj_is_disabled(btn));
	lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
}

static void check_disabled(const char *label) {
	lv_obj_t *page = lv_screen_active(), *btn = button(label);
	assert(lv_obj_is_disabled(btn));
	// Bypass normal touch hit testing: even a synthetic click must do nothing.
	lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == page);
}

static void check_pages(theme_kind_t theme) {
	theme_set_kind(theme);
	switch_screen(streaming_screen);
	lv_obj_t *menu = lv_obj_get_parent(button("Network folders"));
	assert(lv_obj_get_style_flex_flow(menu, LV_PART_MAIN) ==
		   (theme == THEME_KIND_DARK ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN));
	tap("Network folders");
	assert(lv_screen_active() == networkmusic_screen);
	const char *profiles[] = {"WebDAV", "FTP"};
	for (size_t i = 0; i < 2; i++) {
		tap(profiles[i]);
		lv_obj_t *profile = lv_screen_active();
		assert(profile != networkmusic_screen && find_label(profile, profiles[i]));
		assert(find_label(button("Connect"), "Unavailable in browser"));
		check_disabled("Connect");
		tap(tr("jellyfin_server_url")); // the real editor still opens
		lv_obj_t *field = lv_obj_get_child_by_type(lv_screen_active(), 0, &lv_textarea_class);
		assert(field && !lv_obj_is_disabled(field));
		const char *url = i == 0 ? "https://nas/" : "ftp://nas/";
		lv_textarea_set_text(field, url);
		tap(tr("ok"));
		assert(lv_screen_active() == profile && find_label(profile, url));
		check_disabled("Connect");
		switch_screen(networkmusic_screen);
	}
	switch_screen(streaming_screen);
	tap("Listen Together");
	assert(lv_screen_active() == listentogether_screen);
	assert(find_label(listentogether_screen, "Hosting and joining are unavailable in the browser."));
	check_disabled("Start Hosting");
	check_disabled("Join Stream");
}

int main(int argc, char **argv) {
	assert(argc == 2);
	char dir[] = "/tmp/sonix-lan-preview-XXXXXX", ini[128];
	assert(mkdtemp(dir));
	snprintf(ini, sizeof(ini), "%s/device.ini", dir);
	config_init(ini);
	lang_init(argv[1]);
	assert(lang_set("English"));
	font_ui_14 = lv_font_montserrat_14;
	font_ui_16 = lv_font_montserrat_16;
	font_ui_18 = font_ui_20 = font_ui_20_bold = lv_font_montserrat_20;
	font_ui_22 = font_ui_22_bold = font_ui_24 = font_ui_24_bold = lv_font_montserrat_22;
	font_ui_26 = font_ui_28 = font_ui_32 = lv_font_montserrat_28;
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), dir};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();
	streaming_screen = lv_obj_create(NULL);
	streaming_init(&cfg);
	check_pages(THEME_KIND_DARK);        // tiles
	check_pages(THEME_KIND_TOKYO_NIGHT); // settings rows
	lv_deinit();
	unlink(ini);
	assert(rmdir(dir) == 0);
	puts("PASS browser Streaming menus and disabled LAN actions");
}
