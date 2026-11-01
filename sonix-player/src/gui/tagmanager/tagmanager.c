#define _DEFAULT_SOURCE
#include "tagmanager.h"
#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/library/metadata.h"
#include "src/system/tagmanager/tagwrite.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define PATH_SIZE 1024
#define ENTRY_MAX 200
struct entry {
	char name[256];
	bool dir;
};
static gui_config_t *config;
static lv_obj_t *screen, *list, *heading, *editor, *field, *editor_title;
static keyboard_t *keyboard;
static char root[PATH_SIZE], dir[PATH_SIZE], file[PATH_SIZE];
static struct entry entries[ENTRY_MAX];
static int count;
static bool details, truncated;
static tagwrite_field_t editing;
static cover_image_t cover;
static song_metadata_t meta;

static bool gesture(void) { return switcher_back_drag_active() || player_sheet_drag_active() || coverflow_drag_active(); }
static bool join(char *out, size_t size, const char *base, const char *name) {
	int n = snprintf(out, size, "%s%s%s", base, base[strlen(base) - 1] == '/' ? "" : "/", name);
	return n > 0 && (size_t)n < size;
}
static bool audio(const char *s) {
	const char *p = strrchr(s, '.');
	return p && (!strcasecmp(p, ".flac") || !strcasecmp(p, ".mp3"));
}
static int compare(const void *a, const void *b) {
	const struct entry *x = a, *y = b;
	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	return strcasecmp(x->name, y->name);
}
static void browse(void);
static void show_details(void);
static void clicked(lv_event_t *e) {
	if (gesture())
		return;
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= count)
		return;
	char path[PATH_SIZE];
	if (!join(path, sizeof(path), dir, entries[i].name)) {
		toast_error("Path too long");
		return;
	}
	struct stat st;
	if (lstat(path, &st) || (entries[i].dir ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) {
		toast_error("File unavailable");
		return;
	}
	if (entries[i].dir) {
		snprintf(dir, sizeof(dir), "%s", path);
		browse();
	} else {
		snprintf(file, sizeof(file), "%s", path);
		details = true;
		show_details();
	}
}
static void label(const char *text) {
	lv_obj_t *l = lv_label_create(list);
	lv_obj_set_width(l, lv_pct(100));
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_label_set_text(l, text);
	lv_obj_add_style(l, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(l, &font_ui_18, 0);
}
static void browse(void) {
	details = false;
	lv_obj_clean(list);
	cover_free(&cover);
	count = 0;
	truncated = false;
	lv_label_set_text(heading, "Tag Manager");
	const char *name = strrchr(dir, '/');
	label(!strcmp(dir, root) ? "MicroSD" : (name && name[1] ? name + 1 : dir));
	DIR *d = opendir(dir);
	if (!d) {
		label("Cannot read folder");
		return;
	}
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(entries[0].name))
			continue;
		char path[PATH_SIZE];
		struct stat st;
		if (!join(path, sizeof(path), dir, de->d_name) || lstat(path, &st))
			continue;
		bool isdir = S_ISDIR(st.st_mode);
		if (!isdir && !(S_ISREG(st.st_mode) && audio(de->d_name)))
			continue;
		if (count == ENTRY_MAX) {
			truncated = true;
			continue;
		}
		snprintf(entries[count].name, sizeof(entries[count].name), "%s", de->d_name);
		entries[count++].dir = isdir;
	}
	closedir(d);
	qsort(entries, count, sizeof(entries[0]), compare);
	for (int i = 0; i < count; i++) {
		lv_obj_t *r = settingsrow_add(list, "", NULL, clicked, (void *)(intptr_t)i);
		lv_label_set_text(settingsrow_name_label(r), entries[i].name);
		settingsrow_name_lines(r, 1);
		settingsrow_add_caption(r, entries[i].dir ? "Folder" : "FLAC / MP3");
	}
	if (!count)
		label("No folders or FLAC / MP3 files");
	if (truncated)
		label("First 200 entries shown");
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}
static void hide_editor(void) {
	keyboard_set_visible(keyboard, false);
	lv_obj_set_hidden(editor, true);
}
static void save_cb(lv_event_t *e) {
	(void)e;
	if (!gui_card_available()) {
		gui_notify_no_card();
		return;
	}
	tagwrite_result_t r = tagwrite_set(file, editing, lv_textarea_get_text(field));
	if (r == TAGWRITE_OK) {
		hide_editor();
		show_details();
		toast_success("Tag saved");
	} else if (r == TAGWRITE_INVALID)
		toast_error("Enter a valid value");
	else if (r == TAGWRITE_UNSUPPORTED)
		toast_error("Unsupported tag layout");
	else
		toast_error("Could not save file");
}
static void edit_cb(lv_event_t *e) {
	if (gesture())
		return;
	editing = (tagwrite_field_t)(intptr_t)lv_event_get_user_data(e);
	lv_label_set_text(editor_title, editing == TAGWRITE_TRACK ? "Track number" : "Genre");
	char track[24] = "";
	if (meta.track_number)
		snprintf(track, sizeof(track), "%d", meta.track_number);
	lv_textarea_set_text(field, editing == TAGWRITE_TRACK ? track : meta.genre);
	lv_obj_set_hidden(editor, false);
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
}
static void detail_line(const char *key, const char *value) {
	char text[400];
	snprintf(text, sizeof(text), "%s: %s", key, value && *value ? value : "—");
	label(text);
}
static void show_details(void) {
	lv_obj_clean(list);
	cover_free(&cover);
	lv_label_set_text(heading, "File tags");
	metadata_read(file, &meta);
	const char *name = strrchr(file, '/');
	label(name ? name + 1 : file);
	if (cover_load_for_file(file, 96, 96, COVER_FIT_CONTAIN, &cover)) {
		lv_obj_t *im = lv_image_create(list);
		lv_image_set_src(im, &cover.dsc);
	} else
		label("No readable cover art");
	detail_line("Title", meta.title);
	detail_line("Artist", meta.artist);
	detail_line("Album", meta.album);
	detail_line("Album artist", meta.album_artist);
	detail_line("Date", meta.date);
	char track[32];
	if (meta.track_number)
		snprintf(track, sizeof(track), "%d", meta.track_number);
	else
		strcpy(track, "—");
	lv_obj_t *r = settingsrow_add(list, "Track number", NULL, edit_cb, (void *)(intptr_t)TAGWRITE_TRACK);
	settingsrow_add_caption(r, track);
	r = settingsrow_add(list, "Genre", NULL, edit_cb, (void *)(intptr_t)TAGWRITE_GENRE);
	settingsrow_add_caption(r, meta.genre[0] ? meta.genre : "—");
	label("Tap a field to edit. For totals, enter 3/12.");
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}
static bool back_peek(void) { return (editor && !lv_obj_is_hidden(editor)) || details || strcmp(dir, root) != 0; }
static bool back(void) {
	if (editor && !lv_obj_is_hidden(editor)) {
		hide_editor();
		return true;
	}
	if (details) {
		browse();
		return true;
	}
	if (strcmp(dir, root)) {
		char *p = strrchr(dir, '/');
		if (p && p >= dir + strlen(root))
			*p = 0;
		else
			snprintf(dir, sizeof(dir), "%s", root);
		browse();
		return true;
	}
	return false;
}
static void build(void) {
	screen = lv_obj_create(NULL);
	list = settingsrow_page(screen, config, "Tag Manager");
	heading = settingsrow_page_title(screen);
	settingsrow_title_corner_slots(heading, config, 0);
	switcher_attach_back_gesture(screen);
	switcher_set_back_guard(screen, back);
	switcher_set_back_guard_peek(screen, back_peek);
	editor = lv_obj_create(screen);
	lv_obj_set_size(editor, config->screen_width, config->screen_height);
	lv_obj_add_style(editor, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(editor, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(editor, 0, 0);
	lv_obj_set_style_pad_all(editor, 0, 0);
	lv_obj_set_scrollable(editor, false);
	editor_title = lv_label_create(editor);
	lv_obj_add_style(editor_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(editor_title, &font_ui_24, 0);
	lv_obj_align(editor_title, LV_ALIGN_TOP_LEFT, settingsrow_header_text_left(config), settingsrow_header_text_top(config));
	field = lv_textarea_create(editor);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_max_length(field, 127);
	lv_obj_set_size(field, config->screen_width - 2 * config->padding, bp_pick(48, 62));
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, config->padding, settingsrow_content_top(config));
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	keyboard = keyboard_create(editor, config->screen_width, config->screen_width < 320 ? 144 : 316, field, NULL, "Save", save_cb, NULL);
	hide_editor();
}
void tagmanager_init(gui_config_t *cfg) { config = cfg; }
void tagmanager_open(void) {
	if (!config || !config->sd_root_path || !config->sd_root_path[0] || !gui_card_available()) {
		gui_notify_no_card();
		return;
	}
	if (strlen(config->sd_root_path) >= sizeof(root)) {
		toast_error("Path too long");
		return;
	}
	snprintf(root, sizeof(root), "%s", config->sd_root_path);
	size_t n = strlen(root);
	while (n > 1 && root[n - 1] == '/')
		root[--n] = 0;
	snprintf(dir, sizeof(dir), "%s", root);
	if (!screen)
		build();
	hide_editor();
	browse();
	switch_screen(screen);
}
