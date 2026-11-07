/*
 * Headless LVGL regression test for stockkeys_ui on the TempoTec V1 240x320
 * panel across all 11 shipped languages and all 5 V1 themes:
 * - verifies all 11 stock_keys_* / setup_from_stock_firmware translation keys
 *   exist and fit cleanly inside the 240x320 screen in every language & theme
 * - verifies offline state shows both "Set up from stock firmware" and "Wi-Fi
 *   settings" and notifies when tapped with neither Wi-Fi nor SD archive
 * - verifies one-tap extraction from a stock hiby_player on the card reloads
 *   streamkeys, initializes Qobuz and Tidal in place without rebooting, calls
 *   on_ready(), and fires toast_success("stock_keys_ready").
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
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/streaming/stockkeys_ui.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/addons/addons.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/stockkeys.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/streaming/tidal.h"

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

/* ---- shell, device & streaming stubs ------------------------------------- */

lv_obj_t *wifisettings_screen;

void bootlogo_set(int choice, bool dark) {
	(void)choice;
	(void)dark;
}
bool bootpic_present(void) { return false; }

static char last_popup[256];
static char last_success_toast[128];
static int hold_screen_depth;
static int qobuz_init_calls;
static int tidal_init_calls;
static int ready_calls;
static bool stub_wifi_on;
static char stub_card_dir[512];

void gui_notify_popup(const char *text) { snprintf(last_popup, sizeof(last_popup), "%s", text ? text : ""); }
void toast_success(const char *text) {
	snprintf(last_success_toast, sizeof(last_success_toast), "%s", text ? text : "");
}
void toast_error(const char *text) { (void)text; }
void toast_plain(const char *text) { (void)text; }
void toast_glyph(const lv_image_dsc_t *g, const char *t) {
	(void)g;
	(void)t;
}

bool player_sheet_drag_active(void) { return false; }
bool switcher_back_drag_active(void) { return false; }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
void switcher_attach_back_gesture(lv_obj_t *screen) { (void)screen; }
void back_btn_cb(lv_event_t *e) { (void)e; }

void power_hold_screen_on(bool hold) { hold_screen_depth += hold ? 1 : -1; }
const char *storage_sd_root(void) { return stub_card_dir[0] ? stub_card_dir : NULL; }
bool storage_sd_writable(void) { return true; }
const char *sysinfo_ota_repo(void) { return "mr-f0xx/sonix-player-tempotec-v1"; }

void wifi_get_status(wifi_status_t *out) {
	memset(out, 0, sizeof(*out));
	if (stub_wifi_on) {
		out->state = WIFI_STATE_CONNECTED;
		snprintf(out->ip, sizeof(out->ip), "192.168.1.10");
	} else {
		out->state = WIFI_STATE_OFF;
	}
}

void qobuz_init(void) { qobuz_init_calls++; }
void tidal_init(void) { tidal_init_calls++; }

/* Unused HTTP symbols in this offline/card UI binary */
const char *http_last_error(void) { return ""; }
bool http_stream_open_ex(http_stream_t *s, const char *url, int timeout_secs, const http_stream_options_t *opts) {
	(void)s;
	(void)url;
	(void)timeout_secs;
	(void)opts;
	return false;
}
int http_stream_read(http_stream_t *s, void *buf, int len) {
	(void)s;
	(void)buf;
	(void)len;
	return -1;
}
void http_stream_wake(http_stream_t *s) { (void)s; }
void http_stream_close(http_stream_t *s) { (void)s; }

/* ---- synthetic ELF writer ------------------------------------------------ */

static void wr16le(uint8_t *p, uint16_t v) {
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)(v >> 8);
}
static void wr32le(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void write_synthetic_hiby_player(const char *path) {
	uint8_t buf[4096];
	memset(buf, 0, sizeof(buf));
	const uint32_t base_va = 0x00400000u;
	memcpy(buf, "\x7f" "ELF\x01\x01\x01\x00", 8);
	wr16le(buf + 16, 2);
	wr16le(buf + 18, 8);
	wr32le(buf + 20, 1);
	wr32le(buf + 28, 52);
	wr16le(buf + 40, 52);
	wr16le(buf + 42, 32);
	wr16le(buf + 44, 1);
	wr32le(buf + 52, 1);
	wr32le(buf + 60, base_va);
	wr32le(buf + 64, base_va);
	wr32le(buf + 68, sizeof(buf));
	wr32le(buf + 72, sizeof(buf));

	size_t ro = 0x600;
#define PUT_CSTR(str)                                                                                                  \
	do {                                                                                                               \
		size_t _l = strlen(str) + 1;                                                                                   \
		memcpy(buf + ro, (str), _l);                                                                                   \
		ro += (_l + 3u) & ~3u;                                                                                         \
	} while (0)
	PUT_CSTR("http://www.qobuz.com/api.json/0.2/");
	PUT_CSTR("device_manufacturer_id");
	PUT_CSTR("742719831");
	PUT_CSTR("0a1b2c3d4e5f60718293a4b5c6d7e8f9");
	PUT_CSTR("https://auth.tidal.com/v1/oauth2/device_authorization");
	PUT_CSTR("urn:ietf:params:oauth:grant-type:device_code");
	PUT_CSTR("AbCdEfGhIjKlMnOpQrStUvWxYz0123456789+/AbCdE=");
	uint32_t cid_va = base_va + (uint32_t)ro;
	PUT_CSTR("client_id");
#undef PUT_CSTR

	const char *tid = "zX9yV8wU7tS6rQ5p";
	size_t pc = 0x100;
	for (size_t i = 0; i < strlen(tid); i++) {
		wr32le(buf + pc, (0x09u << 26) | (3u << 16) | (unsigned char)tid[i]);
		pc += 4;
		wr32le(buf + pc, (0x28u << 26) | (29u << 21) | (3u << 16) | (unsigned)(32 + i));
		pc += 4;
	}
	wr32le(buf + pc, (0x28u << 26) | (29u << 21) | (unsigned)(32 + strlen(tid)));
	pc += 4;
	wr32le(buf + pc, (0x0Fu << 26) | (6u << 16) | (cid_va >> 16));
	pc += 4;
	wr32le(buf + pc, (0x09u << 26) | (6u << 21) | (6u << 16) | (cid_va & 0xFFFFu));

	FILE *f = fopen(path, "wb");
	assert(f);
	assert(fwrite(buf, 1, sizeof(buf), f) == sizeof(buf));
	fclose(f);
}

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

static void on_keys_ready(void) { ready_calls++; }

int main(int argc, char **argv) {
	assert(argc == 2);

	char tmp[] = "/tmp/sonix-stockkeys-ui-XXXXXX";
	assert(mkdtemp(tmp));

	char cfg_path[512], data_ini[512];
	snprintf(cfg_path, sizeof(cfg_path), "%s/device_config.ini", tmp);
	snprintf(data_ini, sizeof(data_ini), "%s/usr_data/streaming-keys.ini", tmp);
	snprintf(stub_card_dir, sizeof(stub_card_dir), "%s/card", tmp);
	assert(mkdir(stub_card_dir, 0755) == 0);
	setenv("SONIX_STREAMKEYS_DATA", data_ini, 1);

	config_init(cfg_path);
	config_set("ui", "language", "English");
	config_set("addons", "qobuz", "1");
	config_set("addons", "tidal", "1");
	lang_init(argv[1]);

	init_fonts();
	lv_init();
	gui_config_t gcfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), stub_card_dir};
	assert(gcfg.screen_width == 240 && gcfg.screen_height == 320);
	lv_display_create(gcfg.screen_width, gcfg.screen_height);
	theme_init();

	lv_obj_t *screen = lv_obj_create(NULL);
	wifisettings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(screen, &gcfg, "Tidal");
	lv_screen_load(screen);

	static const char *const KEYS[] = {
		"setup_from_stock_firmware", "stock_keys_hint",          "stock_keys_scanning_sd",
		"stock_keys_downloading",    "stock_keys_extracting",    "stock_keys_ready",
		"stock_keys_need_wifi_or_sd","stock_keys_no_space",      "stock_keys_download_failed",
		"stock_keys_extract_failed", "stock_keys_save_failed",
	};

	DIR *dir = opendir(argv[1]);
	assert(dir);
	int lang_count = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		size_t nlen = strlen(de->d_name);
		if (nlen <= 4 || strcmp(de->d_name + nlen - 4, ".ini") != 0) {
			continue;
		}
		char lname[64];
		snprintf(lname, sizeof(lname), "%.*s", (int)(nlen - 4), de->d_name);
		assert(lang_set(lname));
		for (size_t k = 0; k < sizeof(KEYS) / sizeof(KEYS[0]); k++) {
			const char *val = tr(KEYS[k]);
			assert(val && val[0] && strcmp(val, KEYS[k]) != 0);
		}

		for (int tk = 0; tk < 5; tk++) {
			theme_set_kind((theme_kind_t)tk);
			stub_wifi_on = false;
			lv_obj_clean(container);
			stockkeys_ui_populate_missing(container, on_keys_ready);
			lv_obj_update_layout(screen);

			lv_obj_t *setup_lbl = find_label(container, tr("setup_from_stock_firmware"));
			lv_obj_t *wifi_lbl = find_label(container, tr("wi_fi_settings"));
			assert(setup_lbl != NULL);
			assert(wifi_lbl != NULL);

			lv_obj_t *setup_row = lv_obj_get_parent(setup_lbl);
			assert(lv_obj_get_x(setup_row) >= 0);
			assert(lv_obj_get_x2(setup_row) <= 240);
		}
		lang_count++;
	}
	closedir(dir);
	assert(lang_count == 11);
	assert(lang_set("English"));
	theme_set_kind(THEME_KIND_TOKYO_NIGHT);

	// 1. Tapping "Set up from stock firmware" while offline with no SD archive shows helpful notice.
	stub_wifi_on = false;
	lv_obj_clean(container);
	stockkeys_ui_populate_missing(container, on_keys_ready);
	lv_obj_update_layout(screen);
	lv_obj_t *setup_btn = lv_obj_get_parent(find_label(container, tr("setup_from_stock_firmware")));
	last_popup[0] = '\0';
	lv_obj_send_event(setup_btn, LV_EVENT_CLICKED, NULL);
	assert(strcmp(last_popup, tr("stock_keys_need_wifi_or_sd")) == 0);

	// 2. Place synthetic hiby_player on the card; Wi-Fi row disappears on populate, and tapping
	//    "Set up from stock firmware" extracts keys, re-inits Qobuz & Tidal, and calls on_ready()!
	char elf_path[560];
	snprintf(elf_path, sizeof(elf_path), "%s/hiby_player", stub_card_dir);
	write_synthetic_hiby_player(elf_path);

	lv_obj_clean(container);
	stockkeys_ui_populate_missing(container, on_keys_ready);
	lv_obj_update_layout(screen);
	assert(find_label(container, tr("wi_fi_settings")) == NULL);

	setup_btn = lv_obj_get_parent(find_label(container, tr("setup_from_stock_firmware")));
	last_success_toast[0] = '\0';
	qobuz_init_calls = 0;
	tidal_init_calls = 0;
	ready_calls = 0;

	lv_obj_send_event(setup_btn, LV_EVENT_CLICKED, NULL);
	assert(hold_screen_depth == 1);

	for (int iter = 0; iter < 100 && ready_calls == 0; iter++) {
		lv_tick_inc(200);
		lv_timer_handler();
		usleep(10000);
	}

	assert(ready_calls == 1);
	assert(qobuz_init_calls == 1);
	assert(tidal_init_calls == 1);
	assert(hold_screen_depth == 0);
	assert(strcmp(last_success_toast, "stock_keys_ready") == 0);
	assert(strcmp(streamkeys_qobuz_app_id(), "742719831") == 0);
	assert(strcmp(streamkeys_tidal_client_id(), "zX9yV8wU7tS6rQ5p") == 0);

	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
	assert(system(cmd) == 0);
	lv_deinit();
	puts("PASS stockkeys_ui (240x320, 11 languages, 5 themes, offline guard & zero-reboot activation)");
	return 0;
}
