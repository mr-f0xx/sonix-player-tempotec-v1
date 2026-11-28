#include "glassbgpage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/glassbg.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"

lv_obj_t *glassbgpage_screen;

#define MAX_PICTURES 64
#define CHECK_GREEN lv_color_make(46, 194, 126)

typedef struct {
	char name[GLASSBG_NAME_MAX + 1];
	lv_obj_t *mark;
} picture_t;

static picture_t pictures[MAX_PICTURES];
static int picture_count;

static lv_obj_t *list;
static lv_obj_t *empty_note;
static lv_obj_t *default_mark;
static int panel_w = 240, panel_h = 320;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool busy;

static void pick_cb(lv_event_t *e);

typedef struct {
	char name[GLASSBG_NAME_MAX + 1];
	uint8_t *px;
} job_t;

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

// Mark the picture in use, or the Default row when the supplied art is showing.
static void paint_checks(void) {
	const char *chosen = glassbg_current_name();
	if (default_mark) {
		lv_obj_set_hidden(default_mark, chosen[0] != '\0');
	}
	for (int i = 0; i < picture_count; i++) {
		if (pictures[i].mark) {
			lv_obj_set_hidden(pictures[i].mark,
							  !(chosen[0] && strcmp(chosen, pictures[i].name) == 0));
		}
	}
}

static void refresh_empty_note(void) {
	if (!empty_note) {
		return;
	}
	if (picture_count > 0) {
		lv_obj_set_hidden(empty_note, true);
		return;
	}
	const char *dir = glassbg_dir();
	char note[800];
	if (dir[0]) {
		snprintf(note, sizeof(note), "%s\n%s", tr("glassbg_empty"), dir);
	} else {
		snprintf(note, sizeof(note), "%s", tr("glassbg_no_card"));
	}
	lv_label_set_text(empty_note, note);
	lv_obj_set_hidden(empty_note, false);
}

static lv_obj_t *add_mark(lv_obj_t *row) {
	lv_obj_t *mark = lv_image_create(row);
	lv_image_set_src(mark, &icon_check);
	lv_obj_set_style_image_recolor(mark, CHECK_GREEN, 0);
	lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
	lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_hidden(mark, true);
	return mark;
}

// Back to the supplied art: no decode, so it happens right on the tap.
static void default_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	glassbg_reset();
	paint_checks();
}

static void rebuild(void) {
	if (!list) {
		return;
	}
	lv_obj_clean(list);
	picture_count = 0;
	default_mark = NULL;

	// The supplied art is always a choice, first in the list, so a picture
	// that has since left the card still leaves a way back.
	lv_obj_t *def = settingsrow_action(list, tr("glassbg_default"), default_cb, NULL);
	default_mark = add_mark(def);

	char names[MAX_PICTURES][GLASSBG_NAME_MAX + 1];
	int found = glassbg_list(names, MAX_PICTURES);
	for (int i = 0; i < found; i++) {
		snprintf(pictures[i].name, sizeof(pictures[0].name), "%s", names[i]);
		lv_obj_t *row = settingsrow_action(list, names[i], pick_cb, (void *)(intptr_t)i);
		settingsrow_name_lines(row, 2);
		pictures[i].mark = add_mark(row);
		picture_count = i + 1;
	}

	paint_checks();
	refresh_empty_note();
}

// ---------------------------------------------------------------------------
// The pick (decode off the interface thread, install on it)
// ---------------------------------------------------------------------------

static void job_done(void *user) {
	job_t *j = (job_t *)user;
	toast_busy_end();
	pthread_mutex_lock(&lock);
	busy = false;
	pthread_mutex_unlock(&lock);

	if (j->px) {
		glassbg_install(j->px, panel_w, panel_h, j->name);
		toast_success("glassbg_done");
		paint_checks();
	} else {
		toast_error("glassbg_failed");
	}
	free(j);
}

static void *job_main(void *arg) {
	job_t *j = (job_t *)arg;
	thread_be_background("glass background");

	char path[800];
	snprintf(path, sizeof(path), "%s/%s", glassbg_dir(), j->name);
	j->px = glassbg_decode_file(path, panel_w, panel_h);

	if (!gui_post(job_done, j)) {
		pthread_mutex_lock(&lock);
		busy = false;
		pthread_mutex_unlock(&lock);
		free(j->px);
		free(j);
	}
	return NULL;
}

static void pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= picture_count) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (busy) {
		pthread_mutex_unlock(&lock);
		return;
	}
	busy = true;
	pthread_mutex_unlock(&lock);

	job_t *j = calloc(1, sizeof(*j));
	if (!j) {
		pthread_mutex_lock(&lock);
		busy = false;
		pthread_mutex_unlock(&lock);
		return;
	}
	snprintf(j->name, sizeof(j->name), "%s", pictures[index].name);

	toast_busy("glassbg_working");
	pthread_t thread;
	if (pthread_create(&thread, NULL, job_main, j) != 0) {
		toast_busy_end();
		pthread_mutex_lock(&lock);
		busy = false;
		pthread_mutex_unlock(&lock);
		free(j);
		return;
	}
	pthread_detach(thread);
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();
}

void glassbgpage_init(gui_config_t *cfg) {
	panel_w = (int)cfg->screen_width;
	panel_h = (int)cfg->screen_height;

	glassbg_set_root(cfg->sd_root_path);

	lv_obj_t *container = settingsrow_page(glassbgpage_screen, cfg, "glassbg_title");

	empty_note = lv_label_create(container);
	lv_label_set_long_mode(empty_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(empty_note, lv_pct(100));
	lv_obj_add_style(empty_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_note, &font_ui_22, 0);

	list = lv_obj_create(container);
	lv_obj_set_width(list, lv_pct(100));
	lv_obj_set_height(list, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_pad_row(list, bp_is_tempotec_v1() ? 6 : 12, 0);
	lv_obj_set_scrollable(list, false);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	rebuild();

	lv_obj_add_event_cb(glassbgpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	theme_register_refresh(paint_checks);
}
