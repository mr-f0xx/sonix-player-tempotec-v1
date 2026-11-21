// The production Wi-Fi update card on the V1's 240x320 panel. The release body
// goes through the production HTML cleanup; OTA and hardware are substituted.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/fwupdate.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/firmware.h"
#include "src/system/device/ota.h"
#include "src/system/device/releasenotes.h"
#include "src/system/device/sysinfo.h"

lv_font_t font_ui_14, font_ui_20, font_ui_20_bold, font_ui_22, font_ui_22_bold, font_ui_24, font_ui_26;
lv_style_t theme_style_card, theme_style_text, theme_style_text_dim, theme_style_switch;
static theme_palette_t palette;
const theme_palette_t *theme(void) { return &palette; }
lv_color_t theme_on_accent(void) { return lv_color_black(); }
int bp_pick(int compact, int regular) { (void)regular; return compact; }
int bp_tile_radius(void) { return 10; }

const char *tr(const char *key) {
	static const struct { const char *key, *text; } strings[] = {
		{"system_update_firmware", "Firmware update"}, {"system_update_online", "Via internet"},
		{"system_update_from_sd", "From SD"}, {"cancel", "Cancel"}, {"system_update", "Update"},
		{"system_update_version_size", "Version %s - %s"}, {"system_update_no_notes", "No release notes"},
		{"system_update_checking", "Checking..."}, {"system_update_busy", "Busy"},
		{"system_update_up_to_date", "Up to date: %s"}, {"system_update_no_wifi", "No Wi-Fi"},
		{"system_update_check_failed", "Check failed"}, {"system_update_installing", "Installing..."},
		{"system_no_card", "No card"}, {"system_update_no_space", "No space"},
		{"system_update_corrupt", "Corrupt file"}, {"system_update_download_failed", "Download failed"},
		{"system_update_file_missing", "File missing"}, {"ok", "OK"}, {NULL, NULL},
	};
	for (size_t i = 0; strings[i].key; i++) {
		if (strcmp(key, strings[i].key) == 0) return strings[i].text;
	}
	return key;
}

static ota_release_t release;
static ota_state_t state;
static ota_result_t result;
const ota_release_t *ota_release(void) { return &release; }
bool ota_check_start(void) { state = OTA_FINISHED; result = OTA_OK; return true; }
bool ota_download_start(void) { return false; }
void ota_cancel(void) { state = OTA_IDLE; }
ota_state_t ota_state(void) { return state; }
ota_result_t ota_result(void) { return result; }
void ota_acknowledge(void) { state = OTA_IDLE; }
void ota_progress(long *done, long *total) { *done = *total = 0; }
const char *ota_installed_version(void) { return "1.6.2"; }
void power_hold_screen_on(bool hold) { (void)hold; }
bool usbhostmode_release(bool clear) { (void)clear; return true; }
bool firmware_update_file_find(char *out, size_t size) { (void)out; (void)size; return false; }
void firmware_update_start(void) {}
void ota_auto_release_taken(void) {}
void gui_notify_popup(const char *key) { (void)key; }
void gui_notify_popup_long(const char *key) { (void)key; }
void sysinfo_format_size(uint64_t bytes, char *out, size_t size) { snprintf(out, size, "%llu MB", (unsigned long long)(bytes / 1000000)); }

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) return obj;
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
		if (found) return found;
	}
	return NULL;
}

static bool has_span_text(lv_obj_t *obj, const char *text) {
	if (lv_obj_check_type(obj, &lv_spangroup_class)) {
		for (uint32_t i = 0; i < lv_spangroup_get_span_count(obj); i++) {
			if (strcmp(lv_span_get_text(lv_spangroup_get_child(obj, i)), text) == 0) return true;
		}
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		if (has_span_text(lv_obj_get_child(obj, i), text)) return true;
	}
	return false;
}

static void check_text(lv_obj_t *obj) {
	if (lv_obj_check_type(obj, &lv_label_class)) {
		const char *text = lv_label_get_text(obj);
		assert(!strstr(text, "<img") && !strstr(text, "width=") && !strstr(text, "alt="));
		assert(!strstr(text, "user-attachments"));
		assert(!strstr(text, "http://") && !strstr(text, "https://"));
		assert(!strstr(text, "Full Changelog"));
		assert(!strstr(text, "@mr-f0xx"));
	}
	if (lv_obj_check_type(obj, &lv_spangroup_class)) {
		for (uint32_t i = 0; i < lv_spangroup_get_span_count(obj); i++) {
			const char *text = lv_span_get_text(lv_spangroup_get_child(obj, i));
			assert(!strstr(text, "<img") && !strstr(text, "width=") && !strstr(text, "alt="));
			assert(!strstr(text, "user-attachments"));
			assert(!strstr(text, "http://") && !strstr(text, "https://"));
			assert(!strstr(text, "Full Changelog"));
			assert(!strstr(text, "@mr-f0xx"));
		}
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) check_text(lv_obj_get_child(obj, i));
}

static void check_on_screen(lv_obj_t *obj) {
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	assert(a.x1 >= 0 && a.y1 >= 0 && a.x2 < 240 && a.y2 < 320);
}

static void show_release(bool changelog) {
	fwupdate_show();
	lv_obj_t *online = find_label(lv_layer_top(), "Via internet");
	assert(online);
	lv_obj_send_event(lv_obj_get_parent(online), LV_EVENT_CLICKED, NULL);
	lv_tick_inc(201);
	lv_timer_handler();
	if (changelog) {
		assert(find_label(lv_layer_top(), "What's Changed"));
		assert(has_span_text(lv_layer_top(), "Calculator: frame the display card in the accent colour"));
		assert(has_span_text(lv_layer_top(), "Music: search corner button on compact Dark/Light headers"));
		assert(has_span_text(lv_layer_top(), "Thanks u/nightdriveavenger for the idea!"));
		assert(has_span_text(lv_layer_top(), "Streaming: Subsonic & Radio on the same 2x2 tile home page"));
	} else {
		assert(find_label(lv_layer_top(), "No release notes"));
	}
	lv_obj_t *update = find_label(lv_layer_top(), "Update");
	lv_obj_t *cancel = find_label(lv_layer_top(), "Cancel");
	assert(update && cancel && !lv_obj_is_hidden(lv_obj_get_parent(update)) && !lv_obj_is_hidden(lv_obj_get_parent(cancel)));
	check_on_screen(lv_obj_get_parent(update));
	check_on_screen(lv_obj_get_parent(cancel));
	check_text(lv_layer_top());
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}

static void snapshot(const char *name) {
	const char *prefix = getenv("SONIX_PAGE_SNAPSHOTS");
	if (!prefix || !prefix[0]) return;
	lv_obj_update_layout(lv_screen_active());
	lv_draw_buf_t *shot = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_RGB888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%s-%s.ppm", prefix, name);
	FILE *f = fopen(path, "wb");
	assert(f);
	fprintf(f, "P6\n%u %u\n255\n", shot->header.w, shot->header.h);
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			uint8_t rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
			assert(fwrite(rgb, 1, 3, f) == 3);
		}
	}
	assert(fclose(f) == 0);
	lv_draw_buf_destroy(shot);
}

int main(void) {
	lv_init();
	font_ui_14 = lv_font_montserrat_14;
	font_ui_20 = lv_font_montserrat_20;
	font_ui_20_bold = lv_font_montserrat_20;
	font_ui_22 = lv_font_montserrat_22;
	font_ui_22_bold = lv_font_montserrat_22;
	font_ui_24 = lv_font_montserrat_22;
	font_ui_26 = lv_font_montserrat_28;
	palette.text_primary = lv_color_white();
	palette.text_secondary = lv_color_hex(0xaaaaaa);
	palette.accent = lv_color_hex(0xff9944);
	palette.surface = lv_color_hex(0x202020);
	lv_style_init(&theme_style_card);
	lv_style_set_bg_color(&theme_style_card, palette.surface);
	lv_style_set_bg_opa(&theme_style_card, LV_OPA_COVER);
	lv_style_init(&theme_style_text);
	lv_style_set_text_color(&theme_style_text, palette.text_primary);
	lv_style_init(&theme_style_text_dim);
	lv_style_set_text_color(&theme_style_text_dim, palette.text_secondary);
	lv_style_init(&theme_style_switch);
	lv_style_set_bg_color(&theme_style_switch, lv_color_hex(0x404040));
	lv_style_set_bg_opa(&theme_style_switch, LV_OPA_COVER);

	lv_display_t *display = lv_display_create(240, 320);
	static uint8_t buffer[240 * 320 * 4];
	lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(display, flush_cb);
	lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
	lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

	snprintf(release.tag, sizeof(release.tag), "1.6.2.1");
	release.size = 51312640;
	// The body of the published release, with the banner and the auto-generated
	// link lines as GitHub writes them.
	release_notes_clean(
		"<img width=\"1664\" height=\"624\" alt=\"D8As1YMQxmTvAAAAJXRFWHRkYXRlOmNyZWF0ZQ\" "
		"src=\"https://github.com/user-attachments/assets/cdaf32ef-81f0-4c23-8d11-962c5a540be2\" />\r\n\r\n"
		"## What's Changed\r\n\r\n"
		"* Calculator: frame the display card in the accent colour by @mr-f0xx in https://github.com/mr-f0xx/sonix-player-tempotec-v1/pull/307\r\n"
		"* Music: search corner button on compact Dark/Light headers by @mr-f0xx in https://github.com/mr-f0xx/sonix-player-tempotec-v1/pull/308\r\n"
		"* Thanks [u/nightdriveavenger](https://www.reddit.com/user/nightdriveavenger/) for the idea! by @mr-f0xx in https://github.com/mr-f0xx/sonix-player-tempotec-v1/pull/277\r\n"
		"* Streaming: Subsonic & Radio on the same 2x2 tile home page by @mr-f0xx in https://github.com/mr-f0xx/sonix-player-tempotec-v1/pull/294 /https://github.com/mr-f0xx/sonix-player-tempotec-v1/pull/295\r\n\r\n"
		"**Full Changelog**: https://github.com/mr-f0xx/sonix-player-tempotec-v1/compare/1.6.2...1.6.2.1",
		release.notes, sizeof(release.notes));
	show_release(true);
	snapshot("changelog");

	// The notes have their own scroll area; both actions remain on the glass.
	lv_obj_t *update = find_label(lv_layer_top(), "Update");
	lv_obj_t *column = lv_obj_get_parent(lv_obj_get_parent(update));
	lv_obj_t *card = lv_obj_get_parent(column);
	check_on_screen(card);
	lv_obj_t *notes = lv_obj_get_child(card, 2);
	assert(lv_obj_get_height(notes) <= 320 * 26 / 100);
	assert(lv_obj_get_scroll_bottom(notes) > 0);
	lv_obj_scroll_to_y(notes, lv_obj_get_scroll_bottom(notes), LV_ANIM_OFF);
	lv_obj_update_layout(card);
	assert(lv_obj_get_scroll_y(notes) > 0);
	snapshot("scrolled");

	// A banner-only release uses the existing translated empty-notes message.
	release_notes_clean("<img src=\"https://example.invalid/banner.png\" alt=\"banner\">", release.notes, sizeof(release.notes));
	show_release(false);
	check_text(lv_layer_top());
	lv_obj_send_event(lv_obj_get_parent(find_label(lv_layer_top(), "Cancel")), LV_EVENT_CLICKED, NULL);
	puts("PASS firmware update card: changelog headings/bullets, no HTML, URLs or attribution, scrolling, visible actions, banner-only fallback");
	return 0;
}
