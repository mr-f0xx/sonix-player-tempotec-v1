#include "networkmusicpage.h"
#include "streaming.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/networkmusic.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS_PER_PAGE 18

typedef struct {
	char address[SHARE_ADDRESS_MAX];
	char username[SHARE_LOGIN_MAX];
	char password[SHARE_LOGIN_MAX]; // on-screen session only, never in config
} profile_t;

static profile_t profiles[2];
static share_kind_t kind;
static gui_config_t *cfg;
lv_obj_t *networkmusic_screen;
static lv_obj_t *settings, *browser, *edit;
static lv_obj_t *settings_title, *address_value, *username_value, *password_value;
static lv_obj_t *edit_title, *edit_field;
static keyboard_t *keyboard;
static int editing;
static lv_obj_t *content, *folder_label, *status_label, *rows;
static lv_obj_t *retry_row, *up_row, *save_folder_row, *cancel_row;
static lv_timer_t *poll_timer;
static share_server_t server;
static share_listing_t *listing;
static char folder[SHARE_FOLDER_MAX], requested[SHARE_FOLDER_MAX];
static size_t offset, selected;
static enum { NONE, LIST, SAVE, SAVE_PLAY } pending;
static bool left_page;

static const char *section(share_kind_t which) { return which == SHARE_FTP ? "network_ftp" : "network_webdav"; }
static const char *protocol_name(share_kind_t which) { return which == SHARE_FTP ? "FTP" : "WebDAV"; }

static const char *error_label(share_status_t status) {
	switch (status) {
	case SHARE_BAD_ADDRESS: return "Enter a valid server URL and login.";
	case SHARE_UNAVAILABLE: return "This firmware does not support that network protocol.";
	case SHARE_AUTH_ERROR: return "lastfm_wrong_credentials";
	case SHARE_CERT_ERROR: return "Could not verify the server certificate.";
	case SHARE_TIMEOUT: return "The server did not respond in time.";
	case SHARE_SERVER_ERROR: return "The server refused the request or the file is unavailable.";
	case SHARE_BAD_LIST: return "The server did not return a WebDAV folder listing.";
	case SHARE_LIST_TOO_LARGE: return "This folder is too large to list. Open a smaller folder.";
	case SHARE_NO_CARD: return "Insert a writable microSD card not shared over USB.";
	case SHARE_DISK_ERROR: return "Could not write to the microSD card.";
	case SHARE_CARD_FULL: return "The microSD card is full.";
	case SHARE_FILE_TOO_LARGE: return "The file exceeds the 4 GiB download limit.";
	case SHARE_PATH_TOO_LONG: return "The folder or file name is too long.";
	case SHARE_CANCELLED: return "Request cancelled.";
	case SHARE_BUSY: return "Wait for the current request to finish.";
	case SHARE_NO_MEMORY: return "Not enough memory.";
	default: return "Could not reach the server. Check Wi-Fi and the address.";
	}
}

static lv_obj_t *note(lv_obj_t *parent, const char *text) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_16, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_label_set_text(label, tr(text));
	return label;
}

static void status_text(const char *text) {
	lv_label_set_text(status_label, text);
	lv_obj_set_hidden(status_label, !text[0]);
}

static bool busy(void) {
	if (pending == NONE) return false;
	toast_plain(error_label(SHARE_BUSY));
	return true;
}

static void track_cb(lv_event_t *e);
static void page_cb(lv_event_t *e);

// Only one small page of rows is built at a time. Use ordinary LVGL widgets:
// settings rows with callbacks are intended to live for the whole run.
static void row(const char *label, const lv_image_dsc_t *icon, lv_event_cb_t cb, uintptr_t data) {
	lv_obj_t *button = lv_button_create(rows);
	lv_obj_set_size(button, lv_pct(100), cfg->screen_width < 320 ? 40 : 56);
	lv_obj_add_style(button, &theme_style_card, 0);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_pad_hor(button, 8, 0);
	lv_obj_set_style_pad_column(button, 7, 0);
	lv_obj_set_event_bubble(button, true);
	lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, (void *)data);
	lv_obj_t *image = lv_image_create(button);
	lv_image_set_src(image, icon);
	lv_obj_add_style(image, &theme_style_icon, 0);
	lv_obj_t *name = lv_label_create(button);
	lv_obj_set_flex_grow(name, 1);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_label_set_text(name, label); // server names are not translation keys
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_18, 0);
}

static void render_listing(void) {
	lv_obj_clean(rows);
	bool ready = listing != NULL;
	lv_label_set_text(folder_label, ready ? folder : requested);
	lv_obj_set_hidden(up_row, !ready || !strcmp(folder, server.root));
	lv_obj_set_hidden(retry_row, ready || pending != NONE);
	bool has_track = false;
	if (ready) {
		for (size_t i = 0; i < listing->count; i++) has_track |= !listing->entries[i].directory;
		if (offset) row(tr("Previous page"), &icon_folder, page_cb, 0);
		for (size_t i = offset; i < listing->count && i < offset + ROWS_PER_PAGE; i++) {
			const share_entry_t *entry = &listing->entries[i];
			row(entry->name, entry->directory ? &icon_folder : &icon_music_note, track_cb, i);
		}
		if (offset + ROWS_PER_PAGE < listing->count) row(tr("Next page"), &icon_folder, page_cb, 1);
	}
	lv_obj_set_hidden(save_folder_row, !ready || !has_track || listing->truncated);
	lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
}

static void page_cb(lv_event_t *e) {
	if (busy() || switcher_back_drag_active()) return;
	if ((uintptr_t)lv_event_get_user_data(e)) offset += ROWS_PER_PAGE;
	else if (offset >= ROWS_PER_PAGE) offset -= ROWS_PER_PAGE;
	render_listing();
}

static void launch(int operation) {
	pending = operation;
	left_page = false;
	lv_obj_set_hidden(cancel_row, false);
	lv_obj_set_hidden(retry_row, true);
	status_text(tr(operation == LIST ? "Opening folder…" : "Saving to microSD…"));
	lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
	lv_timer_resume(poll_timer);
	lv_timer_reset(poll_timer);
}

static void open_folder(const char *path) {
	share_status_t status = networkmusic_list_start(&server, path);
	if (status != SHARE_OK) { toast_error(error_label(status)); return; }
	// `path` can be requested itself (Retry).
	if (path != requested) strcpy(requested, path);
	launch(LIST);
	if (!listing) lv_label_set_text(folder_label, requested);
}

static void retry_cb(lv_event_t *e) {
	(void)e;
	if (!busy()) open_folder(requested);
}

static void up_cb(lv_event_t *e) {
	(void)e;
	if (busy() || !listing || !strcmp(folder, server.root)) return;
	char path[sizeof(folder)];
	strcpy(path, folder);
	path[strlen(path) - 1] = '\0';
	strrchr(path, '/')[1] = '\0';
	open_folder(path);
}

static void save_entries(const share_entry_t *entries, size_t count, bool play) {
	share_status_t status = networkmusic_save_start(&server, folder, entries, count);
	if (status == SHARE_OK) launch(play ? SAVE_PLAY : SAVE);
	else toast_error(error_label(status));
}

static void save_folder_cb(lv_event_t *e) {
	(void)e;
	if (!busy() && listing && !listing->truncated) save_entries(listing->entries, listing->count, false);
}

static void save_action(void *play) {
	if (!busy() && listing && selected < listing->count) save_entries(&listing->entries[selected], 1, play != NULL);
}

static void track_cb(lv_event_t *e) {
	if (busy() || switcher_back_drag_active() || player_sheet_drag_active() || !listing) return;
	size_t index = (uintptr_t)lv_event_get_user_data(e);
	if (index >= listing->count) return;
	const share_entry_t *entry = &listing->entries[index];
	if (entry->directory) {
		char next[SHARE_FOLDER_MAX];
		if (networkshare_child_folder(&server, folder, entry->name, next, sizeof(next))) open_folder(next);
		else toast_error(error_label(SHARE_PATH_TOO_LONG));
	} else {
		selected = index;
		const popover_item_t items[] = {
			{.label = "Save to microSD", .action = save_action},
			{.label = "Save and play", .action = save_action, .user = (void *)(uintptr_t)1},
		};
		popover_show(lv_event_get_current_target(e), items, 2);
	}
}

static void cancel_cb(lv_event_t *e) {
	(void)e;
	networkmusic_cancel();
}

static void browser_left(lv_event_t *e) {
	(void)e;
	popover_close();
	if (pending != NONE) {
		left_page = true;
		networkmusic_cancel();
	}
}

static void poll(lv_timer_t *timer) {
	networkmusic_result_t result;
	if (!networkmusic_take_result(&result)) {
		if (pending != LIST && lv_screen_active() == browser) {
			networkmusic_progress_t p;
			networkmusic_progress(&p);
			char message[144];
			if (p.bytes_total && p.completed < p.total) {
				unsigned percent = p.bytes_done >= p.bytes_total ? 100 : (unsigned)(100 * p.bytes_done / p.bytes_total);
				snprintf(message, sizeof(message), "%s %u/%u · %u%%", tr("Saving to microSD…"),
						 (unsigned)p.completed + 1, (unsigned)p.total, percent);
			} else {
				snprintf(message, sizeof(message), "%s %u/%u", tr("Saving to microSD…"),
						 (unsigned)p.completed, (unsigned)p.total);
			}
			status_text(message);
		}
		return;
	}
	bool play = pending == SAVE_PLAY;
	bool was_list = pending == LIST;
	pending = NONE;
	lv_timer_pause(timer);
	lv_obj_set_hidden(cancel_row, true);
	if (left_page) { free(result.listing); return; }
	if (result.status != SHARE_OK) {
		status_text(tr(error_label(result.status)));
		lv_obj_set_hidden(retry_row, listing != NULL);
		if (result.saved) toast_plain("Completed tracks are on the card.");
		return;
	}
	if (was_list) {
		free(listing);
		listing = result.listing;
		strcpy(folder, requested);
		offset = 0;
		status_text(listing->truncated ? tr("Showing the first 384 entries.") :
					listing->count == 0 ? tr("No folders or playable music here.") : "");
		render_listing();
		return;
	}
	char message[128];
	snprintf(message, sizeof(message), tr("Saved: %u · Already on card: %u"), (unsigned)result.saved, (unsigned)result.existing);
	status_text(message);
	if (play && result.path[0] && lv_screen_active() == browser && storage_card_attached() && !usb_storage_active()) {
		player_play_file(result.path);
		player_sheet_open(true);
	}
}

static const char *const edit_keys[] = {"jellyfin_server_url", "subsonic_username", "password"};

static void refresh_settings(void) {
	profile_t *profile = &profiles[kind];
	lv_label_set_text(settings_title, protocol_name(kind));
	lv_label_set_text(address_value, profile->address[0] ? profile->address : "—");
	lv_label_set_text(username_value, profile->username[0] ? profile->username : "—");
	lv_label_set_text(password_value, profile->password[0] ? "••••" : "—");
}

static void edit_cb(lv_event_t *e) {
	if (busy()) return;
	profile_t *p = &profiles[kind];
	const char *values[] = {p->address, p->username, p->password};
	editing = (int)(uintptr_t)lv_event_get_user_data(e);
	lv_label_set_text(edit_title, tr(edit_keys[editing]));
	lv_textarea_set_text(edit_field, "");
	lv_textarea_set_max_length(edit_field, editing == 0 ? SHARE_ADDRESS_MAX - 1 : SHARE_LOGIN_MAX - 1);
	lv_textarea_set_password_mode(edit_field, editing == 2);
	keyboard_refresh_password(edit_field);
	lv_textarea_set_placeholder_text(edit_field, editing == 0 ?
		(kind == SHARE_FTP ? "ftp://nas/Music/" : "https://nas/dav/Music/") : tr(edit_keys[editing]));
	lv_textarea_set_text(edit_field, values[editing]);
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
	switch_screen(edit);
}

static void edit_ok(lv_event_t *e) {
	(void)e;
	profile_t *p = &profiles[kind];
	char *fields[] = {p->address, p->username, p->password};
	const char *text = lv_textarea_get_text(edit_field);
	if (strlen(text) >= (editing == 0 ? SHARE_ADDRESS_MAX : SHARE_LOGIN_MAX)) {
		toast_error("The value is too long.");
		return;
	}
	strcpy(fields[editing], text);
	refresh_settings();
	switch_screen_return_to(settings);
}

static void edit_left(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(edit_field, "");
}

static void connect_cb(lv_event_t *e) {
	(void)e;
	if (busy()) return;
	if (!wifi_get_enabled()) { toast_error("Connect to Wi-Fi first."); return; }
	profile_t *p = &profiles[kind];
	share_server_t next;
	if (!networkshare_server_init(&next, kind, p->address, p->username, p->password, config_get("network", "ca_bundle", NULL))) {
		toast_error(error_label(SHARE_BAD_ADDRESS));
		return;
	}
	share_status_t status = networkmusic_list_start(&next, next.root);
	if (status != SHARE_OK) { toast_error(error_label(status)); return; }
	server = next;
	config_set(section(kind), "address", p->address);
	config_set(section(kind), "username", p->username);
	(void)config_save();
	free(listing);
	listing = NULL;
	strcpy(requested, server.root);
	launch(LIST);
	render_listing();
	switch_screen(browser);
}

static void build_pages(void) {
	settings = lv_obj_create(NULL);
	lv_obj_t *setup = settingsrow_page(settings, cfg, "Network folders");
	settings_title = settingsrow_page_title(settings);
	settingsrow_add(setup, edit_keys[0], &address_value, edit_cb, NULL);
	settingsrow_add(setup, edit_keys[1], &username_value, edit_cb, (void *)(uintptr_t)1);
	settingsrow_add(setup, edit_keys[2], &password_value, edit_cb, (void *)(uintptr_t)2);
	if (STREAMING_LAN_PREVIEW_ONLY) {
		// Keep the reason on the action itself, even when the note below it is off-screen.
		lv_obj_t *reason;
		lv_obj_t *connect = settingsrow_add(setup, "Connect", &reason, NULL, NULL);
		lv_obj_set_width(reason, lv_pct(55));
		lv_obj_set_style_text_font(reason, &font_ui_14, 0);
		lv_obj_set_style_text_align(reason, LV_TEXT_ALIGN_RIGHT, 0);
		lv_label_set_long_mode(reason, LV_LABEL_LONG_WRAP);
		lv_label_set_text(reason, tr("Unavailable in browser"));
		streaming_disable_preview_action(connect, settingsrow_name_label(connect));
	} else {
		settingsrow_action(setup, "Connect", connect_cb, NULL);
	}
	note(setup, "Passwords are kept until restart. Leave both login fields blank for public shares.");

	edit = lv_obj_create(NULL);
	lv_obj_add_style(edit, &theme_style_screen, 0);
	edit_title = settingsrow_title(edit, cfg, edit_keys[0]);
	edit_field = lv_textarea_create(edit);
	lv_textarea_set_one_line(edit_field, true);
	lv_obj_add_style(edit_field, &theme_style_card, 0);
	lv_obj_set_style_text_font(edit_field, &font_ui_24, 0);
	lv_obj_set_style_pad_all(edit_field, 8, 0);
	lv_obj_set_size(edit_field, cfg->screen_width - 2 * cfg->padding, cfg->screen_width < 320 ? 44 : 64);
	lv_obj_align(edit_field, LV_ALIGN_TOP_LEFT, cfg->padding, settingsrow_content_top(cfg));
	keyboard_style_caret(edit_field);
	keyboard_style_password(edit_field, 0);
	lv_textarea_set_password_mode(edit_field, false);
	keyboard = keyboard_create(edit, cfg->screen_width, cfg->screen_width < 320 ? 144 : 316, edit_field, NULL, "ok", edit_ok, NULL);
	lv_obj_add_event_cb(edit, edit_left, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(edit);

	// The profile and keyboard work in the preview, but no transfer can start:
	// don't construct a remote listing, its worker callbacks or its polling timer.
	if (STREAMING_LAN_PREVIEW_ONLY) return;

	browser = lv_obj_create(NULL);
	content = settingsrow_page(browser, cfg, "Network folders");
	folder_label = note(content, "");
	lv_label_set_long_mode(folder_label, LV_LABEL_LONG_DOT);
	status_label = note(content, "");
	cancel_row = settingsrow_action(content, "cancel", cancel_cb, NULL);
	retry_row = settingsrow_action(content, "Retry", retry_cb, NULL);
	up_row = settingsrow_action(content, "Parent folder", up_cb, NULL);
	save_folder_row = settingsrow_action(content, "Save folder's tracks", save_folder_cb, NULL);
	rows = lv_obj_create(content);
	lv_obj_remove_style_all(rows);
	lv_obj_set_size(rows, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(rows, cfg->screen_width < 320 ? 4 : 8, 0);
	lv_obj_set_scrollable(rows, false);
	lv_obj_set_event_bubble(rows, true);
	player_sheet_attach_drag(content, true);
	lv_obj_add_event_cb(browser, browser_left, LV_EVENT_SCREEN_UNLOADED, NULL);
	poll_timer = lv_timer_create(poll, 200, NULL);
	lv_timer_pause(poll_timer);
}

static void choose_profile(lv_event_t *e) {
	if (busy()) return;
	kind = (share_kind_t)(uintptr_t)lv_event_get_user_data(e);
	if (!settings) build_pages();
	refresh_settings();
	switch_screen(settings);
}

void networkmusic_page_init(gui_config_t *config) {
	cfg = config;
	for (int i = 0; i < 2; i++) {
		const char *name = section((share_kind_t)i);
		snprintf(profiles[i].address, sizeof(profiles[i].address), "%s", config_get(name, "address", ""));
		snprintf(profiles[i].username, sizeof(profiles[i].username), "%s", config_get(name, "username", ""));
	}
	networkmusic_screen = lv_obj_create(NULL);
	lv_obj_t *menu = settingsrow_page(networkmusic_screen, cfg, "Network folders");
	settingsrow_add_icon(settingsrow_add(menu, "WebDAV", NULL, choose_profile, NULL), &icon_folder, THEME_SEMANTIC_CYAN);
	settingsrow_add_icon(settingsrow_add(menu, "FTP", NULL, choose_profile, (void *)(uintptr_t)SHARE_FTP),
						 &icon_folder, THEME_SEMANTIC_BLUE);
	note(menu, STREAMING_LAN_PREVIEW_ONLY ? "Browser preview: try the settings without connecting to a server." :
		 "Browse music on a server and save it to microSD. Use HTTPS WebDAV or FTPS for encrypted connections.");
}
