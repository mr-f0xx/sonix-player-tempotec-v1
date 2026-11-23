// The actual LVGL setup, keyboard and browser at the V1's 240x320 size.
// The loopback suite exercises the real worker and network separately.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "../../src/gui/streaming/networkmusicpage.c"

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32, font_ui_36_bold;
static bool wifi = true, has_card = true, worker, ready;
static int cancels, played;
static const char *toast;
static networkmusic_result_t next;
static char sent_folder[SHARE_FOLDER_MAX];
static share_kind_t sent_kind;
static popover_item_t choices[2];

void fonts_register_change(void (*cb)(void)) { (void)cb; }
void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
void switcher_mark_fresh_start(lv_obj_t *obj) { (void)obj; }
bool switcher_back_drag_active(void) { return false; }
bool player_sheet_drag_active(void) { return false; }
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) { assert(obj && opening); }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_return_to(lv_obj_t *screen) { switch_screen(screen); }
void back_btn_cb(lv_event_t *e) { (void)e; switch_screen(networkmusic_screen); }
void toast_error(const char *text) { toast = text; }
void toast_plain(const char *text) { toast = text; }
bool wifi_get_enabled(void) { return wifi; }
bool storage_card_attached(void) { return has_card; }
bool usb_storage_active(void) { return false; }
void popover_show(lv_obj_t *anchor, const popover_item_t *items, int count) {
	assert(anchor && count == 2);
	memcpy(choices, items, sizeof(choices));
}
void popover_close(void) { memset(choices, 0, sizeof(choices)); }
void player_play_file(const char *path) { assert(!strcmp(path, "/card/Music/Track.flac")); played++; }
void player_sheet_open(bool animate) { assert(animate && played); }

share_status_t networkmusic_list_start(const share_server_t *s, const char *path) {
	assert(!worker && !ready);
	assert(networkshare_valid_folder(s, path));
	sent_kind = s->kind;
	strcpy(sent_folder, path);
	worker = true;
	return SHARE_OK;
}
share_status_t networkmusic_save_start(const share_server_t *s, const char *path, const share_entry_t *items, size_t count) {
	assert(!worker && !ready && s->kind == SHARE_WEBDAV && !strcmp(path, "/dav/Album & Friends/"));
	assert(count && !strcmp(items[0].name, "Track.flac"));
	if (!has_card) return SHARE_NO_CARD;
	worker = true;
	return SHARE_OK;
}
void networkmusic_progress(networkmusic_progress_t *out) {
	assert(worker);
	*out = (networkmusic_progress_t){.total = 1, .bytes_done = 5, .bytes_total = 10};
}
bool networkmusic_take_result(networkmusic_result_t *out) {
	if (!ready) return false;
	*out = next;
	next = (networkmusic_result_t){0};
	ready = false;
	return true;
}
void networkmusic_cancel(void) { cancels++; }

static void done(networkmusic_result_t result) {
	assert(worker && !ready);
	next = result;
	worker = false;
	ready = true;
	poll(poll_timer);
	assert(!ready && !worker && pending == NONE);
}

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *b) {
	(void)a;
	(void)b;
	lv_display_flush_ready(d);
}

static void within_panel(lv_obj_t *obj) {
	assert(!lv_obj_is_hidden(obj));
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	if (area.x1 < 0 || area.y1 < 0 || area.x2 >= 240 || area.y2 >= 320) {
		fprintf(stderr, "Off-screen object (%d,%d)–(%d,%d)\n", area.x1, area.y1, area.x2, area.y2);
		abort();
	}
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *match = find_label(lv_obj_get_child(obj, (int32_t)i), text);
		if (match) return match;
	}
	return NULL;
}

static void tap(lv_obj_t *screen, const char *name) {
	lv_obj_t *label = find_label(screen, name);
	assert(label);
	lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	char dir[] = "/tmp/sonix-netfolders-ui-XXXXXX", config[128], cred_file[128];
	struct stat st;
	assert(mkdtemp(dir));
	snprintf(config, sizeof(config), "%s/device.ini", dir);
	snprintf(cred_file, sizeof(cred_file), "%s/network-credentials.json", dir);
	config_init(config);
	config_set("language", "name", "English");
	config_set("network_webdav", "address", "http://nas/dav/");
	config_set("network_ftp", "address", "ftp://nas/pub/");
	lang_init(argv[1]);
	assert(lang_set("English"));
	font_ui_14 = lv_font_montserrat_14;
	font_ui_16 = lv_font_montserrat_16;
	font_ui_18 = font_ui_20 = font_ui_20_bold = lv_font_montserrat_20;
	font_ui_22 = font_ui_22_bold = font_ui_24 = font_ui_24_bold = lv_font_montserrat_22;
	font_ui_26 = font_ui_28 = lv_font_montserrat_28;
	font_ui_32 = font_ui_36_bold = lv_font_montserrat_36;
	lv_init();
	gui_config_t display = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), dir};
	lv_display_t *d = lv_display_create(display.screen_width, display.screen_height);
	static uint16_t pixels[240 * 16];
	lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(d, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(d, flush);
	theme_init();
	networkmusic_page_init(&display);
	lv_screen_load(networkmusic_screen);
	lv_obj_update_layout(networkmusic_screen);
	within_panel(find_label(networkmusic_screen, "WebDAV"));
	within_panel(find_label(networkmusic_screen, "FTP"));
	tap(networkmusic_screen, "WebDAV");
	assert(lv_screen_active() == settings && kind == SHARE_WEBDAV);
	lv_obj_update_layout(settings);
	// The page scrolls: Connect sits just below the fold until we scroll to it.
	lv_obj_t *connect_label = find_label(settings, "Connect");
	assert(connect_label);
	lv_obj_scroll_to_view(lv_obj_get_parent(connect_label), LV_ANIM_OFF);
	within_panel(connect_label);
	tap(settings, tr("subsonic_username"));
	assert(lv_screen_active() == edit && !lv_textarea_get_password_mode(edit_field));
	lv_textarea_set_text(edit_field, "login");
	edit_ok(NULL);
	tap(settings, tr("password"));
	assert(lv_screen_active() == edit && lv_textarea_get_password_mode(edit_field));
	lv_obj_update_layout(edit);
	within_panel(edit_field);
	within_panel(keyboard_obj(keyboard));
	lv_textarea_set_text(edit_field, "secret");
	edit_ok(NULL);
	assert(!lv_textarea_get_text(edit_field)[0]);
	// Remember password is off by default; flipping it on applies the note
	// warning above it.
	assert(!lv_obj_has_state(remember_switch, LV_STATE_CHECKED));
	lv_obj_set_state(remember_switch, LV_STATE_CHECKED, true);
	lv_obj_send_event(remember_switch, LV_EVENT_VALUE_CHANGED, NULL);
	assert(lv_obj_has_state(remember_switch, LV_STATE_CHECKED));
	assert(strstr(lv_label_get_text(password_note), "unencrypted"));
	wifi = false;
	tap(settings, "Connect");
	assert(!worker && toast && lv_screen_active() == settings);
	wifi = true;
	tap(settings, "Connect");
	assert(worker && !strcmp(sent_folder, "/dav/") && lv_screen_active() == browser);
	assert(config_get("network_webdav", "password", NULL) == NULL);
	assert(!strcmp(config_get("network_webdav", "username", ""), "login"));
	// Remembered password: the private file exists with 0600 and holds the password.
	assert(stat(cred_file, &st) == 0 && (st.st_mode & 0777) == 0600);
	{
		FILE *f = fopen(cred_file, "r");
		assert(f);
		char content[512];
		size_t n = fread(content, 1, sizeof(content) - 1, f);
		fclose(f);
		content[n] = '\0';
		assert(strstr(content, "secret"));
		assert(!strstr(content, "\"ftp\""));
	}

	share_listing_t *root = calloc(1, sizeof(*root));
	assert(root);
	root->count = 1;
	root->entries[0] = (share_entry_t){.name = "Album & Friends", .directory = true};
	done((networkmusic_result_t){.listing = root});
	lv_obj_update_layout(browser);
	within_panel(find_label(browser, "Album & Friends"));
	assert(lv_obj_is_hidden(up_row) && lv_obj_is_hidden(save_folder_row));
	tap(browser, "Album & Friends");
	assert(worker && !strcmp(sent_folder, "/dav/Album & Friends/"));
	share_listing_t *album = calloc(1, sizeof(*album));
	assert(album);
	album->count = ROWS_PER_PAGE + 1;
	album->entries[0] = (share_entry_t){.name = "Track.flac"};
	for (size_t i = 1; i < album->count; i++) snprintf(album->entries[i].name, SHARE_NAME_MAX, "Track %zu.flac", i);
	done((networkmusic_result_t){.listing = album});
	lv_obj_update_layout(browser);
	within_panel(find_label(browser, "Track.flac"));
	assert(!lv_obj_is_hidden(up_row) && !lv_obj_is_hidden(save_folder_row));
	tap(browser, "Next page");
	assert(offset == ROWS_PER_PAGE);
	tap(browser, "Previous page");
	assert(!offset);

	has_card = false;
	tap(browser, "Track.flac");
	assert(!strcmp(choices[0].label, "Save to microSD"));
	choices[0].action(choices[0].user);
	assert(!worker && !strcmp(toast, error_label(SHARE_NO_CARD)));
	has_card = true;
	tap(browser, "Track.flac");
	choices[1].action(choices[1].user);
	assert(worker && pending == SAVE_PLAY && !played);
	poll(poll_timer);
	assert(strstr(lv_label_get_text(status_label), "50%"));
	done((networkmusic_result_t){.path = "/card/Music/Track.flac", .saved = 1});
	assert(played == 1);

	tap(browser, "Save folder's tracks");
	assert(worker && pending == SAVE);
	switch_screen(settings);
	assert(cancels == 1);
	done((networkmusic_result_t){.status = SHARE_CANCELLED});
	assert(played == 1);
	// Remember off: edit the password away and toggle remember back off.
	switch_screen(networkmusic_screen);
	tap(networkmusic_screen, "WebDAV");
	assert(lv_screen_active() == settings && kind == SHARE_WEBDAV);
	assert(lv_obj_has_state(remember_switch, LV_STATE_CHECKED));
	tap(settings, tr("password"));
	assert(lv_screen_active() == edit);
	lv_textarea_set_text(edit_field, "");
	edit_ok(NULL);
	// Remembering needs a password; leave it empty and refuse the connect.
	tap(settings, "Connect");
	assert(!worker && toast && lv_screen_active() == settings);
	// The guard refused before saving: the stored password is still on disk.
	assert(stat(cred_file, &st) == 0);
	lv_obj_set_state(remember_switch, LV_STATE_CHECKED, false);
	lv_obj_send_event(remember_switch, LV_EVENT_VALUE_CHANGED, NULL);
	assert(!lv_obj_has_state(remember_switch, LV_STATE_CHECKED));
	assert(!strstr(lv_label_get_text(password_note), "unencrypted"));
	tap(settings, "Connect");
	assert(worker && sent_kind == SHARE_WEBDAV);
	assert(access(cred_file, F_OK) != 0);
	done((networkmusic_result_t){.status = SHARE_CANCELLED});
	switch_screen(networkmusic_screen);
	tap(networkmusic_screen, "FTP");
	assert(lv_screen_active() == settings && kind == SHARE_FTP);
	assert(!lv_obj_has_state(remember_switch, LV_STATE_CHECKED));
	tap(settings, "Connect");
	assert(worker && sent_kind == SHARE_FTP && !strcmp(sent_folder, "/pub/"));
	done((networkmusic_result_t){.status = SHARE_AUTH_ERROR});
	assert(!lv_obj_is_hidden(retry_row));
	tap(browser, "Retry");
	assert(worker && !strcmp(sent_folder, "/pub/"));
	done((networkmusic_result_t){.status = SHARE_CANCELLED});
	lv_timer_handler();
	lv_deinit();
	unlink(config);
	assert(rmdir(dir) == 0);
	puts("PASS Network folders page (240x320)");
	return 0;
}
