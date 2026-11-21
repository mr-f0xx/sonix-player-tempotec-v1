#include "bootpicpage.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/theme_icons.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/bootlogo.h"
#include "src/system/device/bootpic.h"

lv_obj_t *bootpicpage_screen;

// However many pictures the folder holds. The page scrolls; a folder of a
// thousand is somebody's photo library, and the list is only the first part
// of it.
#define MAX_PICTURES 64

// The same green the settings rows use to mark a page whose feature is on.
#define CHECK_GREEN lv_color_make(46, 194, 126)

typedef struct {
	char name[BOOTPIC_NAME_MAX + 1];
	lv_obj_t *mark; // the tick, in the place a chevron would be
} picture_t;

static picture_t pictures[MAX_PICTURES];
static int picture_count;

static lv_obj_t *list;		 // the column the rows go into
static lv_obj_t *empty_note; // what the page says when there are none
static int panel_w = 240, panel_h = 320;

// One install at a time. A second tap while the first picture is being
// decoded has nothing new to say, and two threads writing the same file is
// two answers to one question.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool busy;

static void pick_cb(lv_event_t *e);

typedef struct {
	char name[BOOTPIC_NAME_MAX + 1];
	int result;
} job_t;

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

// Marks the picture the boot screen was last made from, and only it. Nothing
// is marked when that picture is no longer in the folder: the copy on the
// partition is still what boots, but the row that made it is gone, and a tick
// on the wrong row is worse than no tick at all.
static void paint_checks(void) {
	const char *chosen = theme_boot_screen_custom();
	for (int i = 0; i < picture_count; i++) {
		if (!pictures[i].mark) {
			continue;
		}
		lv_obj_set_hidden(pictures[i].mark, !(chosen[0] && strcmp(chosen, pictures[i].name) == 0));
	}
}

// What the page says with no pictures to list: no card, or no folder, or a
// folder with nothing in it. The last two are the same sentence, and the
// folder's path is in it because a folder that is merely in the wrong place
// is the usual reason a page like this one is empty.
static void refresh_empty_note(void) {
	if (!empty_note) {
		return;
	}
	const char *dir = bootpic_dir();
	char text[512];
	if (!dir[0]) {
		snprintf(text, sizeof(text), "%s", tr("bootpic_no_card"));
	} else {
		snprintf(text, sizeof(text), "%s\n%s", tr("bootpic_empty"), dir);
	}
	lv_label_set_text(empty_note, text);
	lv_obj_set_hidden(empty_note, picture_count > 0);
}

// Reads the folder again and rebuilds the rows. On every visit: a card
// swapped while the player was running, or a picture copied over beside it,
// is only visible if the list is read now rather than once at startup.
static void rebuild(void) {
	if (!list) {
		return;
	}
	lv_obj_clean(list);
	picture_count = 0;

	char names[MAX_PICTURES][BOOTPIC_NAME_MAX + 1];
	int found = bootpic_list(names, MAX_PICTURES);

	for (int i = 0; i < found; i++) {
		snprintf(pictures[i].name, sizeof(pictures[0].name), "%s", names[i]);

		// A row that marks itself rather than leading somewhere: the tick
		// goes where a chevron would be, so every picture sits on the
		// same grid whether it is the one in use or not.
		lv_obj_t *row = settingsrow_action(list, names[i], pick_cb, (void *)(intptr_t)i);
		settingsrow_name_lines(row, 2);

		int w = 0, h = 0;
		if (bootpic_size(names[i], &w, &h)) {
			char caption[64];
			snprintf(caption, sizeof(caption), "%d x %d", w, h);
			settingsrow_add_caption(row, caption);
		}

		lv_obj_t *mark = lv_image_create(row);
		lv_image_set_src(mark, theme_icon(&icon_check));
		lv_obj_set_style_image_recolor(mark, CHECK_GREEN, 0);
		lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
		lv_obj_align(mark, LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_set_hidden(mark, true);

		pictures[i].mark = mark;
		picture_count = i + 1;
	}

	paint_checks();
	refresh_empty_note();
}

// ---------------------------------------------------------------------------
// The install
// ---------------------------------------------------------------------------

static void job_done(void *user) {
	job_t *j = (job_t *)user;
	toast_busy_end();

	pthread_mutex_lock(&lock);
	busy = false;
	pthread_mutex_unlock(&lock);

	if (j->result == BOOTPIC_OK) {
		// The choice follows the copy: a boot screen pointing at a
		// picture that has not been made yet is a boot screen that
		// draws the stock one.
		theme_set_boot_screen_custom(j->name);
		theme_set_boot_screen(BOOTLOGO_CUSTOM);
		toast_success("bootpic_done");
		paint_checks();
	} else {
		toast_error(bootpic_result_key((bootpic_result_t)j->result));
	}
	free(j);
}

static void *job_main(void *arg) {
	job_t *j = (job_t *)arg;
	thread_be_background("boot picture");

	j->result = (int)bootpic_install(j->name, panel_w, panel_h);

	// Back to the interface thread: everything above is a file and a
	// decode, but the toast and the tick are LVGL.
	if (!gui_post(job_done, j)) {
		pthread_mutex_lock(&lock);
		busy = false;
		pthread_mutex_unlock(&lock);
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

	// Stays until the result comes back: a decode of a camera-sized JPEG is
	// most of a second on this core, and a page that does nothing for a
	// moment reads as a page that did not hear the tap.
	toast_busy("bootpic_working");

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

void bootpicpage_init(gui_config_t *cfg) {
	panel_w = (int)cfg->screen_width;
	panel_h = (int)cfg->screen_height;

	// The folder's root, from the card the storage layer settled on. It is
	// set again when a card is attached (see system.c), which is what makes
	// a card swapped while the player runs readable here.
	bootpic_set_root(cfg->sd_root_path);

	lv_obj_t *container = settingsrow_page(bootpicpage_screen, cfg, "bootpic_title");

	// The note first, so it sits above the list rather than under a page
	// and a half of empty space when the folder is bare.
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

	lv_obj_add_event_cb(bootpicpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	theme_register_refresh(paint_checks);
}
