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
#include "src/system/tagmanager/artsearch.h"
#include "src/system/tagmanager/tagwrite.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
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
static bool searching;
// 0: file details, 1: results, 2: preview, 3/4/5: search/download/save.
static int art_state;
static unsigned art_generation;
static artsearch_result_t results[ARTSEARCH_MAX_RESULTS];
static int result_count;
static uint8_t *art_data;
static size_t art_size;
static int art_width, art_height;
static cover_image_t art_preview;
static lv_timer_t *art_timer;

typedef struct {
	int operation;
	unsigned generation;
	bool done, ok;
	char text[1024], path[PATH_SIZE];
	artsearch_result_t found[ARTSEARCH_MAX_RESULTS];
	int count;
	uint8_t *data;
	size_t size;
	int width, height;
	tagwrite_result_t write_result;
} art_job_t;
static pthread_mutex_t art_lock = PTHREAD_MUTEX_INITIALIZER;
static art_job_t *art_job;
static void art_results(void);
static void art_preview_show(void);
static void art_tick(lv_timer_t *timer);
static void art_clear(void) {
	cover_free(&art_preview);
	free(art_data);
	art_data = NULL;
	art_size = 0;
}
static void *art_worker(void *arg) {
	art_job_t *job = arg;
	if (job->operation == 3)
		job->ok = artsearch_find(job->text, job->found, &job->count);
	else if (job->operation == 4)
		job->ok = artsearch_download(job->text, &job->data, &job->size, &job->width, &job->height);
	else if (job->operation == 5) {
		job->write_result = tagwrite_cover(job->path, job->data, job->size, job->width, job->height);
		job->ok = job->write_result == TAGWRITE_OK;
	}
	pthread_mutex_lock(&art_lock);
	job->done = true;
	pthread_mutex_unlock(&art_lock);
	return NULL;
}
static bool art_launch(art_job_t *job) {
	if (art_job) {
		free(job->data);
		free(job);
		toast_error("Please wait for the previous request");
		return false;
	}
	pthread_t thread;
	job->generation = art_generation;
	art_job = job;
	if (pthread_create(&thread, NULL, art_worker, job)) {
		art_job = NULL;
		free(job->data);
		free(job);
		toast_error("Could not start search");
		return false;
	}
	pthread_detach(thread);
	lv_timer_resume(art_timer);
	return true;
}

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
static void find_cover_cb(lv_event_t *e);
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
	art_clear();
	count = 0;
	truncated = false;
	lv_label_set_text(heading, "Tag Manager");
	const char *name = strrchr(dir, '/');
	label(!strcmp(dir, root) ? "Music" : (name && name[1] ? name + 1 : dir));
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
	if (searching) {
		const char *query = lv_textarea_get_text(field);
		if (!*query || strlen(query) > 255) {
			toast_error("Enter a search");
			return;
		}
		art_job_t *job = calloc(1, sizeof(*job));
		if (!job) {
			toast_error("Out of memory");
			return;
		}
		job->operation = 3;
		snprintf(job->text, sizeof(job->text), "%s", query);
		if (!art_launch(job))
			return;
		hide_editor();
		art_state = 3;
		lv_obj_clean(list);
		cover_free(&cover);
		lv_label_set_text(heading, "Cover search");
		label("Searching online...");
		return;
	}
	// Cached streaming tracks can have a sidecar that overrides these fields
	// in the library. Never report success while the visible value stays old.
	if (editing != TAGWRITE_GENRE) {
		char sidecar[PATH_SIZE + 6];
		struct stat st;
		int n = snprintf(sidecar, sizeof(sidecar), "%s.tags", file);
		if (n < 0 || (size_t)n >= sizeof(sidecar)) {
			toast_error("Path too long");
			return;
		}
		if (!lstat(sidecar, &st)) {
			toast_error("Track has a tag override file");
			return;
		}
		if (errno != ENOENT) {
			toast_error("Cannot check tag override");
			return;
		}
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
	searching = false;
	const char *title = "Genre", *value = meta.genre;
	char track[24] = "";
	if (editing == TAGWRITE_TRACK) {
		title = "Track number";
		if (meta.track_number)
			snprintf(track, sizeof(track), "%d", meta.track_number);
		value = track;
	} else if (editing == TAGWRITE_TITLE) {
		title = "Track title";
		value = meta.title;
	} else if (editing == TAGWRITE_ARTIST) {
		title = "Artist";
		value = meta.artist;
	}
	lv_label_set_text(editor_title, title);
	lv_textarea_set_text(field, value);
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
	settingsrow_add(list, "Find cover art", NULL, find_cover_cb, NULL);
	detail_line("Title", meta.title);
	settingsrow_add(list, "Edit title", NULL, edit_cb, (void *)(intptr_t)TAGWRITE_TITLE);
	detail_line("Artist", meta.artist);
	settingsrow_add(list, "Edit artist", NULL, edit_cb, (void *)(intptr_t)TAGWRITE_ARTIST);
	detail_line("Album", meta.album);
	detail_line("Album artist", meta.album_artist);
	detail_line("Date", meta.date);
	char track[32];
	if (meta.track_number)
		snprintf(track, sizeof(track), "%d", meta.track_number);
	else
		strcpy(track, "—");
	detail_line("Track number", track);
	settingsrow_add(list, "Edit track number", NULL, edit_cb, (void *)(intptr_t)TAGWRITE_TRACK);
	detail_line("Genre", meta.genre);
	settingsrow_add(list, "Edit genre", NULL, edit_cb, (void *)(intptr_t)TAGWRITE_GENRE);
	label("Track totals: enter 3/12.");
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}
// Search is explicit and editable; no track information is sent until OK.
static void find_cover_cb(lv_event_t *e) {
	(void)e;
	if (gesture())
		return;
	searching = true;
	char query[512];
	snprintf(query, sizeof(query), "%s %s", meta.album_artist[0] ? meta.album_artist : meta.artist, meta.album);
	lv_label_set_text(editor_title, "Find cover");
	lv_textarea_set_text(field, query);
	lv_obj_set_hidden(editor, false);
	keyboard_reset(keyboard);
	keyboard_set_visible(keyboard, true);
}
static void result_cb(lv_event_t *e) {
	if (gesture())
		return;
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= result_count)
		return;
	art_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error("Out of memory");
		return;
	}
	job->operation = 4;
	snprintf(job->text, sizeof(job->text), "%s", results[index].url);
	if (!art_launch(job))
		return;
	art_state = 4;
	lv_obj_clean(list);
	label("Downloading preview...");
}
static void art_results(void) {
	art_state = 1;
	lv_obj_clean(list);
	art_clear();
	lv_label_set_text(heading, "Cover results");
	if (!result_count)
		label("No matching albums found");
	for (int i = 0; i < result_count; i++) {
		char text[400];
		snprintf(text, sizeof(text), "%s — %s", results[i].artist, results[i].album);
		lv_obj_t *r = settingsrow_add(list, "", NULL, result_cb, (void *)(intptr_t)i);
		lv_label_set_text(settingsrow_name_label(r), text);
		settingsrow_name_lines(r, 2);
	}
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}
static void apply_cover_cb(lv_event_t *e) {
	(void)e;
	if (gesture())
		return;
	if (!gui_card_available()) {
		gui_notify_no_card();
		return;
	}
	if (!art_data || !art_size)
		return;
	art_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error("Out of memory");
		return;
	}
	job->data = malloc(art_size);
	if (!job->data) {
		free(job);
		toast_error("Out of memory");
		return;
	}
	memcpy(job->data, art_data, art_size);
	job->size = art_size;
	job->width = art_width;
	job->height = art_height;
	job->operation = 5;
	snprintf(job->path, sizeof(job->path), "%s", file);
	if (!art_launch(job))
		return;
	art_state = 5;
	lv_obj_clean(list);
	cover_free(&art_preview);
	label("Saving cover... Keep the card inserted.");
}
static void art_preview_show(void) {
	art_state = 2;
	lv_obj_clean(list);
	cover_free(&art_preview);
	lv_label_set_text(heading, "Cover preview");
	if (!cover_load_image_memory(art_data, art_size, 120, 120, COVER_FIT_CONTAIN, &art_preview)) {
		art_results();
		toast_error("Cannot decode this image");
		return;
	}
	lv_obj_t *im = lv_image_create(list);
	lv_image_set_src(im, &art_preview.dsc);
	label("Replace the selected file's front cover?");
	settingsrow_add(list, "Use this cover", NULL, apply_cover_cb, NULL);
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}
static void art_tick(lv_timer_t *timer) {
	pthread_mutex_lock(&art_lock);
	art_job_t *job = art_job;
	bool done = job && job->done;
	pthread_mutex_unlock(&art_lock);
	if (!done)
		return;
	art_job = NULL;
	lv_timer_pause(timer);
	if (job->generation == art_generation) {
		if (job->operation == 3) {
			result_count = job->count;
			memcpy(results, job->found, sizeof(results));
			art_results();
			if (!job->ok)
				toast_error("Cover search failed; check Wi-Fi");
		} else if (job->operation == 4) {
			if (job->ok) {
				art_clear();
				art_data = job->data;
				job->data = NULL;
				art_size = job->size;
				art_width = job->width;
				art_height = job->height;
				art_preview_show();
			} else {
				art_results();
				toast_error("Could not download cover");
			}
		} else {
			if (job->ok) {
				art_state = 0;
				art_clear();
				show_details();
				toast_success("Cover saved");
			} else {
				art_preview_show();
				toast_error(job->write_result == TAGWRITE_UNSUPPORTED ? "Unsupported tag layout" : "Could not save cover");
			}
		}
	}
	free(job->data);
	free(job);
}
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	art_generation++;
	art_state = 0; // late network replies cannot change the next screen
}
static bool back_peek(void) { return (editor && !lv_obj_is_hidden(editor)) || art_state || details || strcmp(dir, root) != 0; }
static bool back(void) {
	if (editor && !lv_obj_is_hidden(editor)) {
		hide_editor();
		return true;
	}
	if (art_state == 5) {
		toast_plain("Saving cover...");
		return true;
	}
	if (art_state == 3 || art_state == 4) {
		art_generation++;
		art_state = 0;
		show_details();
		return true;
	}
	if (art_state == 2) {
		art_results();
		return true;
	}
	if (art_state == 1) {
		art_state = 0;
		lv_obj_clean(list);
		art_clear();
		show_details();
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
	lv_textarea_set_max_length(field, 255);
	lv_obj_set_size(field, config->screen_width - 2 * config->padding, bp_pick(48, 62));
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, config->padding, settingsrow_content_top(config));
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	keyboard = keyboard_create(editor, config->screen_width, config->screen_width < 320 ? 144 : 316, field, NULL, "OK", save_cb, NULL);
	art_timer = lv_timer_create(art_tick, 100, NULL);
	lv_timer_pause(art_timer);
	lv_obj_add_event_cb(screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	hide_editor();
}
void tagmanager_init(gui_config_t *cfg) { config = cfg; }
void tagmanager_open(void) {
	if (!config || !config->sd_root_path || !config->sd_root_path[0] || !gui_card_available()) {
		gui_notify_no_card();
		return;
	}
	char music[PATH_SIZE];
	struct stat st;
	if (!join(music, sizeof(music), config->sd_root_path, "Music")) {
		toast_error("Path too long");
		return;
	}
	if (lstat(music, &st) || !S_ISDIR(st.st_mode)) {
		// FAT is case-insensitive; host/exFAT setups may use music or MUSIC.
		DIR *card = opendir(config->sd_root_path);
		bool found = false;
		if (card) {
			struct dirent *de;
			while ((de = readdir(card))) {
				if (strcasecmp(de->d_name, "Music"))
					continue;
				if (join(music, sizeof(music), config->sd_root_path, de->d_name) && !lstat(music, &st) && S_ISDIR(st.st_mode)) {
					found = true;
					break;
				}
			}
			closedir(card);
		}
		if (!found) {
			toast_error("Music folder not found on card");
			return;
		}
	}
	snprintf(root, sizeof(root), "%s", music);
	snprintf(dir, sizeof(dir), "%s", root);
	art_generation++;
	art_state = 0;
	if (!screen)
		build();
	hide_editor();
	browse();
	switch_screen(screen);
}
