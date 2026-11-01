/*
 * The two pages the More menu gained -- the Calculator and the Notes shelf --
 * on the real renderer, at both panel sizes.
 *
 * The arithmetic (test_calculator.c) and the folder rules (test_notes.c) are
 * checked in plain C elsewhere; what is checked here is the part that only
 * exists with a screen: that both destinations are on the More page, that the
 * keys and rows are finger-sized and land inside a 240x320 V1 panel, that a
 * key or a row actually does what it is drawn promising, and that leaving the
 * editor with writing unsaved still ends with the writing on the card.
 */
#include <assert.h>
#include <dirent.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/board_profile.h"
#include "src/gui/calculator/calculator.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/pictureview.h"
#include "src/gui/library/textview.h"
#include "src/gui/metronome/metronome.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/notes/noteeditor.h"
#include "src/gui/notes/notes.h"
#include "src/gui/tagmanager/tagmanager.h"
#include "src/system/library/metadata.h"
#include "src/system/tagmanager/artsearch.h"
#include "src/gui/pet/petpage.h"
#include "src/gui/pet/peticon.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/gridpage.h"
#include "src/system/playback/device_state.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/input/kblayout.h"
#include "src/system/notes/notesstore.h"
#include "src/system/pet/pet.h"
#include "src/system/pet/petwatch.h"
#include "src/system/text/textfile.h"

/* ---- the parts of the shell these pages lean on, substituted ------------- */

lv_obj_t *music_screen, *streaming_screen, *wireless_screen;
lv_obj_t *audiobooks_screen, *settings_screen, *player_screen;
lv_obj_t *ebookpage_screen, *browser_screen;
lv_obj_t *dacpage_screen, *gearboypage_screen;

lv_font_t font_ui_14, font_ui_16, font_ui_18, font_ui_20, font_ui_20_bold;
lv_font_t font_ui_22, font_ui_22_bold, font_ui_24, font_ui_24_bold;
lv_font_t font_ui_26, font_ui_28, font_ui_32;
lv_font_t font_ui_36_bold, font_ui_64_bold;
static void (*font_callbacks[16])(void);
static int font_callback_count;
static int text_size = FONTS_TEXT_NORMAL;

static void init_fonts(void) {
	lv_font_t *small[] = {
		&font_ui_14, &font_ui_16, &font_ui_18, &font_ui_20, &font_ui_20_bold,
		&font_ui_22, &font_ui_22_bold, &font_ui_24, &font_ui_24_bold,
		&font_ui_26, &font_ui_28, &font_ui_32,
	};
	for (size_t i = 0; i < sizeof(small) / sizeof(small[0]); i++) {
		*small[i] = lv_font_montserrat_14;
	}
	// The one size the interface draws a step larger than the text around it:
	// a second metric, so the calculator's symbol keys can be checked to be
	// drawn bigger than its digits rather than merely differently styled.
	lv_font_t *large[] = {&font_ui_36_bold, &font_ui_64_bold};
	for (size_t i = 0; i < sizeof(large) / sizeof(large[0]); i++) {
		*large[i] = lv_font_montserrat_20;
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

void bootlogo_set(int choice, bool dark) { (void)choice; (void)dark; }
bool bootpic_present(void) { return false; }
void device_state_get(device_state_t *out) { memset(out, 0, sizeof(*out)); }
bool gui_card_available(void) { return true; }
void gui_notify_no_card(void) { assert(!"the card is there in this test"); }
void gui_notify_popup(const char *text) {
	fprintf(stderr, "popup: %s\n", text ? text : "");
	assert(!"unexpected popup");
}
static const char *expected_error;
void toast_error(const char *text) {
    if(expected_error && text && !strcmp(expected_error,text)) { expected_error=NULL; return; }
	fprintf(stderr, "error: %s\n", text ? text : "");
	assert(!"unexpected error toast");
}
void toast_plain(const char *text) { (void)text; }
void toast_success(const char *text) { (void)text; }
void topbar_set_battery_percent(bool shown) { (void)shown; }
void topbar_set_clock_position(int pos) { (void)pos; }
void switch_screen(lv_obj_t *screen) { lv_screen_load(screen); }
void switch_screen_cb(lv_event_t *e) { switch_screen(lv_event_get_user_data(e)); }
void ebookpage_open(void) { switch_screen(ebookpage_screen); }
void filespage_open(void) { switch_screen(browser_screen); }
void audiobooks_open(void) { switch_screen(audiobooks_screen); }
/* The real answer (main_menu.c is not linked into this suite): More asks the
 * home menu whether Books took its Audiobooks slot. */
bool main_menu_books(void) { return config_get_bool("ui", "home_books", false); }
void flappybird_open(void) { /* not reached from this test */ }
/* The games' page chrome: the way back in the header, and the status bar and
 * volume overlay a game hides while it is on screen. The games are opened
 * from More in this suite, so the shell they hide is a stub. */
void back_btn_cb(lv_event_t *e) { (void)e; }
void back_btn_force_hidden(bool hidden) { (void)hidden; }
void topbar_set_hidden(bool hidden) { (void)hidden; }
void volume_overlay_allow_outside_player(bool allow) { (void)allow; }

/* The metronome's click is a thread writing into the PCM, which this suite has
 * no output for. The page asks it only these questions, and the two counters
 * are driven from the test to stand for the thread having just written a beat
 * -- which is exactly the contract the page is built on: it reads the beat
 * back rather than timing one itself. */
static bool click_started;
static bool click_failed;
static int click_tempo;
static int click_bar;
static unsigned click_written;
static int click_last;

bool metronomeclick_start(int bpm, int beats) {
	click_started = true;
	click_failed = false;
	click_tempo = bpm;
	click_bar = beats;
	click_written = 0;
	click_last = -1;
	return true;
}
void metronomeclick_set_tempo(int bpm, int beats) {
	click_tempo = bpm;
	click_bar = beats;
}
void metronomeclick_stop(void) { click_started = false; }
bool metronomeclick_failed(void) { return click_failed; }
unsigned metronomeclick_beats(void) { return click_written; }
int metronomeclick_beat_index(void) { return click_last; }

/* The music pet's watcher: the page asks it four things, and the arithmetic
 * under it is the real pet.c (linked below). What is substituted is the
 * listening -- there is no audio engine in this suite -- so the pet is a
 * creature that has heard music and is being looked at. */
static pet_state_t pet_state;
static bool pet_listening;
static int pet_volume = 60; // what the page thinks the player is turned up to
static pet_genre_look_t pet_look; // and the accessory the tags are asking for

const pet_state_t *petwatch_state(void) { return &pet_state; }
bool petwatch_listening(void) { return pet_listening; }
// The test's clock, in UTC: noon on 15 July 2025, which is a summer afternoon
// and therefore a sunlit card. Time-dependent things are driven from here.
static int64_t pet_now = 1752580800;
int64_t petwatch_now(void) { return pet_now; }
void petwatch_track_title(char *out, size_t out_size) {
	if (out && out_size) {
		snprintf(out, out_size, "%s", "Test track");
	}
}
bool petwatch_pet_now(void) { return pet_pet(&pet_state, petwatch_now()); }
bool petwatch_hold(void) { return pet_hold(&pet_state); }
int petwatch_volume(void) { return pet_volume; }
petwatch_box_t petwatch_box(void) {
	petwatch_box_t box = {0};
	box.fortune_id = pet_fortune(&pet_state, pet_day_now(petwatch_now()));
	box.fortune_ready = pet_state.fortune_paid != pet_state.fortune_day;
	box.drops = pet_state.drop_pending;
	box.treats_ready = pet_treat_ready(&pet_state, petwatch_now());
	for (int i = 0; i < PET_DROP_COUNT; i++) {
		box.found[i] = pet_state.found[i];
	}
	return box;
}
int petwatch_drop_take(void) { return pet_drop_take(&pet_state); }
bool petwatch_fortune_pay(void) { return pet_fortune_pay(&pet_state, pet_day_now(petwatch_now())); }
int petwatch_treat_finish(int hits) { return pet_treat_finish(&pet_state, petwatch_now(), hits); }
pet_genre_look_t petwatch_look(void) { return pet_look; }
int petwatch_take_events(void) { return 0; }
int petwatch_take_achievement(void) { return 0; }
pet_vibe_t petwatch_vibe(void) { return PET_VIBE_NEUTRAL; }
bool petwatch_focus(void) { return false; }
bool petwatch_buy(pet_item_t item) { return pet_buy(&pet_state, item); }
bool petwatch_wear(pet_item_t item) { return pet_wear(&pet_state, item); }
bool petwatch_set_species(pet_species_t species) { return pet_set_species(&pet_state, species); }
bool petwatch_set_name(const char *name) {
	pet_set_name(name);
	return true;
}
pet_temperament_t petwatch_temperament(void) { return pet_temperament(&pet_state); }
bool petwatch_has_soundscape(void) { return pet_has_soundscape(&pet_state); }
bool petwatch_rhythm_ready(void) { return pet_rhythm_ready(&pet_state); }
int petwatch_rhythm_finish(int score) { return pet_rhythm_finish(&pet_state, score); }
void petwatch_flush(void) {}

/* The clock is set, because this suite's whole day is a fixed second. */
bool clock_is_set(void) { return true; }

/* A timer parked for the screen's dark half; there is no power manager here. */
void power_pause_in_standby(lv_timer_t *timer) { (void)timer; }
void switcher_attach_back_gesture(lv_obj_t *obj) { (void)obj; }
bool switcher_back_drag_active(void) { return false; }
static bool (*last_back_guard)(void);
void switcher_set_back_guard(lv_obj_t *screen, bool (*handler)(void)) { (void)screen; last_back_guard=handler; }
void switcher_set_back_guard_peek(lv_obj_t *screen, bool (*peek)(void)) { (void)screen; (void)peek; }
/* Counted rather than ignored: the two file viewers must not take the
 * player-sheet swipe (see the check after pictureview_init). */
static int player_drag_attaches;
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {
	(void)obj;
	(void)opening;
	player_drag_attaches++;
}
bool player_sheet_drag_active(void) { return false; }

/* The carousel's own hooks. The pages here never open it: what they are checked
 * for is that they leave the pull alone (see the tap guards in gridpage.c). */
void coverflow_attach_edge(lv_obj_t *obj, gui_config_t *cfg) { (void)obj; (void)cfg; }
bool coverflow_drag_active(void) { return false; }
bool coverflow_is_open(void) { return false; }


bool cover_load_image_file(const char *path, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	(void)fit;
	if (!path || !out || box_w <= 0 || box_h <= 0) {
		return false;
	}
	struct stat st;
	if (stat(path, &st) != 0 || st.st_size <= 0) {
		return false;
	}
	int w = box_w > 64 ? 64 : box_w;
	int h = box_h > 48 ? 48 : box_h;
	uint8_t *pixels = calloc((size_t)(w * h * 2), 1);
	assert(pixels);
	memset(out, 0, sizeof(*out));
	out->pixels = pixels;
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = w;
	out->dsc.header.h = h;
	out->dsc.header.stride = w * 2;
	out->dsc.data_size = (uint32_t)(w * h * 2);
	out->dsc.data = pixels;
	return true;
}

bool cover_load_for_file(const char *path, int w, int h, cover_fit_t fit, cover_image_t *out) {
    (void)path; (void)w; (void)h; (void)fit; (void)out; return false;
}
void metadata_read(const char *path, song_metadata_t *out) {
    (void)path; memset(out, 0, sizeof(*out)); strcpy(out->title,"Test title");
}

bool artsearch_find(const char *query, artsearch_result_t *results, int *count) {
    assert(query && *query); memset(results,0,sizeof(*results));
    strcpy(results[0].album,"Test album"); strcpy(results[0].artist,"Test artist"); strcpy(results[0].url,"https://example.invalid/test.jpg"); *count=1; return true;
}
bool artsearch_download(const char *url, uint8_t **data, size_t *size, int *width, int *height) {
    (void)url; static const uint8_t jpeg[]={0xff,0xd8,1,2,3,0xff,0xd9};
    *data=malloc(sizeof(jpeg)); assert(*data); memcpy(*data,jpeg,sizeof(jpeg)); *size=sizeof(jpeg); *width=32; *height=32; return true;
}
bool cover_load_image_memory(const void *data, size_t size, int w, int h, cover_fit_t fit, cover_image_t *out) {
    (void)data; (void)size; (void)w; (void)h; (void)fit;
    memset(out,0,sizeof(*out)); out->pixels=calloc(32*32*2,1); assert(out->pixels);
    out->dsc.header.magic=LV_IMAGE_HEADER_MAGIC; out->dsc.header.cf=LV_COLOR_FORMAT_RGB565;
    out->dsc.header.w=32; out->dsc.header.h=32; out->dsc.header.stride=64;
    out->dsc.data=out->pixels; out->dsc.data_size=32*32*2; return true;
}
void cover_free(cover_image_t *img) {
	if (!img) {
		return;
	}
	free(img->pixels);
	memset(img, 0, sizeof(*img));
}

/* ---- small helpers ------------------------------------------------------- */

static lv_obj_t *find_label(lv_obj_t *obj, const char *text) {
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

static lv_obj_t *find_image_button(lv_obj_t *obj, const lv_image_dsc_t *glyph);

static lv_obj_t *find_class(lv_obj_t *obj, const lv_obj_class_t *type) {
	if (lv_obj_check_type(obj, type)) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_class(lv_obj_get_child(obj, i), type);
		if (found) {
			return found;
		}
	}
	return NULL;
}

/* The picture that names an entry, rather than the chevron that every row
 * carries. A leading icon is added after the row's label and chevron, so the
 * last image child is the destination's own glyph. */
static const lv_image_dsc_t *entry_icon_source(lv_obj_t *entry) {
	if (!entry) {
		return NULL;
	}
	for (int32_t i = (int32_t)lv_obj_get_child_count(entry) - 1; i >= 0; i--) {
		lv_obj_t *child = lv_obj_get_child(entry, i);
		if (child && lv_obj_check_type(child, &lv_image_class)) {
			return lv_image_get_src(child);
		}
	}
	return NULL;
}

/* Whether a page holds an entry named `label` -- the tile or row whose
 * caption is that word, whole or cut short to fit the tile it is drawn on.
 * A narrowed caption carries LVGL's ellipsis, so the dotted tail is not part
 * of the comparison; the visible head still has to be the word itself. */
static bool has_entry(lv_obj_t *container, const char *label) {
	size_t full = strlen(label);
	for (uint32_t i = 0; i < lv_obj_get_child_count(container); i++) {
		lv_obj_t *caption = find_class(lv_obj_get_child(container, i), &lv_label_class);
		if (!caption) {
			continue;
		}
		const char *text = lv_label_get_text(caption);
		size_t cut = strlen(text);
		while (cut > 0 && text[cut - 1] == '.') {
			cut--;
		}
		if (cut == 0 || cut > full || strncmp(text, label, cut) != 0) {
			continue;
		}
		// A caption may be cut short, but not to a stub that could be the
		// start of any word: the whole label, or five characters of it.
		if (cut == full || cut >= 5) {
			return true;
		}
	}
	return false;
}

/* The entry whose caption is `label`, allowing for the ellipsis a narrow tile
 * puts on the end of it: has_entry()'s question, answered with the entry. */
static lv_obj_t *entry_with_label(lv_obj_t *container, const char *label) {
	size_t full = strlen(label);
	for (uint32_t i = 0; i < lv_obj_get_child_count(container); i++) {
		lv_obj_t *child = lv_obj_get_child(container, i);
		lv_obj_t *caption = find_class(child, &lv_label_class);
		if (!caption) {
			continue;
		}
		const char *text = lv_label_get_text(caption);
		size_t cut = strlen(text);
		while (cut > 0 && text[cut - 1] == '.') {
			cut--;
		}
		if (cut > 0 && cut <= full && strncmp(text, label, cut) == 0 && (cut == full || cut >= 5)) {
			return child;
		}
	}
	return NULL;
}

/* Presses what a finger would press to reach `text`: the button carrying it. */
/* The same search, but only through what is actually on the glass: the sheets
 * over the pet page each have their own OK, and the hidden ones must not be
 * pressed. */
static lv_obj_t *find_visible_label(lv_obj_t *obj, const char *text) {
	if (!obj || lv_obj_is_hidden(obj)) {
		return NULL;
	}
	if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) {
		return obj;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_visible_label(lv_obj_get_child(obj, (int32_t)i), text);
		if (found) {
			return found;
		}
	}
	return NULL;
}

static void press_visible_key(lv_obj_t *root, const char *text) {
	lv_obj_t *label = find_visible_label(root, text);
	assert(label);
	lv_obj_t *key = lv_obj_get_parent(label);
	while (key && !lv_obj_check_type(key, &lv_button_class)) {
		key = lv_obj_get_parent(key);
	}
	assert(key);
	lv_obj_send_event(key, LV_EVENT_CLICKED, NULL);
}

static void press_key(lv_obj_t *root, const char *text) {
	lv_obj_t *label = find_label(root, text);
	assert(label);
	lv_obj_t *key = lv_obj_get_parent(label);
	while (key && !lv_obj_check_type(key, &lv_button_class)) {
		key = lv_obj_get_parent(key);
	}
	assert(key);
	lv_obj_send_event(key, LV_EVENT_CLICKED, NULL);
}

/* Nothing drawn, anywhere on the page, may leave the panel.
 *
 * Sideways is never allowed. Downwards is, inside a container that scrolls
 * that way and no further: the More page's list and the note editor's text
 * area both hold more than the panel can show, and an object past the foot of
 * one of those is an object the user can scroll to. Everything outside such a
 * container -- the keys of the calculator, the tiles of a grid, the corner
 * controls -- has to be on the glass where it is drawn. */
static void check_within_panel_in(lv_obj_t *obj, gui_config_t *cfg, const char *what, bool scrolls) {
	if (lv_obj_is_hidden(obj)) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	bool overflow_x = area.x1 < 0 || area.x2 >= (int32_t)cfg->screen_width;
	bool overflow_y = area.y1 < 0 || area.y2 >= (int32_t)cfg->screen_height;
	if (overflow_x || (overflow_y && !scrolls)) {
		fprintf(stderr, "%s: an object at %d,%d..%d,%d is outside the %dx%d panel\n", what, area.x1, area.y1,
				area.x2, area.y2, (int)cfg->screen_width, (int)cfg->screen_height);
		assert(!"an object left the panel");
	}
	// A container that scrolls takes the rest of its subtree with it.
	if (lv_obj_get_scroll_dir(obj) & LV_DIR_VER) {
		scrolls = true;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		check_within_panel_in(lv_obj_get_child(obj, i), cfg, what, scrolls);
	}
}

// The shell's back button is drawn above every screen, in the top left corner
// in a corner button's box. A screen-sized sheet covers the page's own title
// row but not that button, so a sheet's heading has to start to the right of it
// -- otherwise the chevron lands on the first word and a player reads "ur pet".
static void check_clear_of_back_button(lv_obj_t *label, gui_config_t *cfg, const char *what) {
	assert(label);
	lv_area_t area;
	lv_obj_get_coords(label, &area);
	const int button_right = cfg->padding + settingsrow_corner_button_size(cfg);
	if (area.x1 < button_right) {
		printf("FAIL %s: heading starts at x %d, the back button ends at %d\n", what, (int)area.x1,
			   button_right);
	}
	assert(area.x1 >= button_right);
}

static void check_within_panel(lv_obj_t *obj, gui_config_t *cfg, const char *what) {
	check_within_panel_in(obj, cfg, what, false);
}

static void dump_snapshot(lv_obj_t *screen, const char *tag) {
	const char *prefix = getenv("SONIX_PAGE_SNAPSHOTS");
	if (!prefix || !prefix[0]) {
		return;
	}
	lv_obj_update_layout(screen);
	lv_draw_buf_t *shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	char path[512];
	snprintf(path, sizeof(path), "%s-%s.ppm", prefix, tag);
	FILE *file = fopen(path, "wb");
	assert(file);
	fprintf(file, "P6\n%u %u\n255\n", shot->header.w, shot->header.h);
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w; x++) {
			uint8_t rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
			assert(fwrite(rgb, sizeof(rgb), 1, file) == 1);
		}
	}
	assert(fclose(file) == 0);
	lv_draw_buf_destroy(shot);
}

/* ---- reading back what a page writes ------------------------------------- */

static char *read_note(const char *dir, const char *name) {
	char path[1200];
	assert(notes_path(path, sizeof(path), dir, name));
	return notes_read(path, NULL);
}

/* ---- the strings, in every language -------------------------------------- */

static bool is_ini(const char *name) {
	size_t n = strlen(name);
	return n > 4 && strcmp(name + n - 4, ".ini") == 0;
}

/* Every language file has to carry the two pages' strings -- the More menu's
 * own two names, the calculator's error state, and the notes shelf's rows and
 * notices. tr() falls back to the key it is given, so a string missing from a
 * file does not fail; it shows the tag itself, which is how a page ends up
 * labelled "notes_new" on a Japanese player and nobody notices. */
static void check_strings(const char *language_dir) {
	static const char *const KEYS[] = {
		"calculator", "calculator_error", "notes", "notes_new", "notes_empty",
		"notes_more", "notes_placeholder", "notes_saved", "notes_cannot_save",
		"notes_cannot_open", "notes_too_long", "music_years", "music_most_played",
		"library_empty_most_played",
		"musicsettings_rearrange_main_page",
		"metronome", "metronome_bpm", "metronome_beats", "metronome_start",
		"metronome_stop", "metronome_tap", "metronome_no_audio",
		"pet", "pet_mood_sleeping", "pet_mood_sad", "pet_mood_okay", "pet_mood_happy",
		"pet_mood_blissful", "pet_stage_egg", "pet_stage_baby", "pet_stage_child",
		"pet_stage_teen", "pet_stage_adult", "pet_energy", "pet_today", "pet_streak",
		"pet_streak_none", "pet_feeding", "pet_feeding_music", "pet_feed_me",
		"pet_asleep", "pet_pet", "pet_purr", "pet_cooldown", "pet_goal_toast",
		"pet_stage_toast", "pet_customise", "pet_choose_pet", "pet_species",
		"pet_species_cat", "pet_species_dog", "pet_species_fish",
		"pet_mood_dizzy", "pet_temperament_none", "pet_temperament_upbeat",
		"pet_temperament_calm", "pet_temperament_curious", "pet_dizzy_hint",
		"pet_dizzy_toast", "pet_drop_toast", "pet_forage_toast", "pet_tap_game",
		"pet_tap_hint", "pet_tap", "pet_tap_score", "pet_tap_done", "pet_tap_played",
		"pet_wardrobe", "pet_notes_balance", "pet_wearing_now", "pet_owned", "pet_price",
		"pet_item_party_hat", "pet_item_headphones", "pet_item_scarf", "pet_item_crown",
		"pet_item_room_sunset", "pet_item_room_night", "pet_item_streak_freeze",
		"pet_achievement", "pet_ach_night_owl", "pet_ach_locked",
		"pet_freezes", "pet_bought", "pet_not_enough", "pet_worn", "pet_already_worn",
		"pet_freeze_note", "pet_room_note", "pet_item_none",
	};
	DIR *dir = opendir(language_dir);
	assert(dir);
	int languages = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (!is_ini(entry->d_name)) {
			continue;
		}
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int)(strlen(entry->d_name) - 4), entry->d_name);
		assert(lang_set(name));
		for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
			const char *text = tr(KEYS[i]);
			if (!text || !text[0] || strcmp(text, KEYS[i]) == 0) {
				fprintf(stderr, "%s: \"%s\" is not translated\n", name, KEYS[i]);
				assert(!"a string is missing from a language file");
			}
			// Long enough to be read on the page, short enough for the
			// notifications' own buffers: several pages build a popup in a
			// 160-byte array, and a translation longer than that would be
			// cut off there rather than here.
			assert(strlen(text) < 160);
		}
		languages++;
	}
	closedir(dir);
	// The eleven this firmware ships. A twelfth added to the folder is meant to
	// make this fail, so its texts get checked the same way.
	assert(languages == 11);
	assert(lang_set("English"));
}

/* ---- the More page ------------------------------------------------------- */

/* The More page has two shapes: the tiles every board shares, and the V1's
 * single-column list, used by the themes built on function glyphs. Whichever
 * is on screen is the one that has to be right.
 *
 * They are told apart by their layout -- the tiles are a wrapping row, the
 * list a column -- because their entries look alike: both are buttons with a
 * caption, so an entry cannot say which shape it is part of. */
static lv_obj_t *visible_shape(lv_obj_t *page, bool *is_grid) {
	lv_obj_t *tiles = NULL;
	lv_obj_t *rows = NULL;
	for (uint32_t i = 0; i < lv_obj_get_child_count(page); i++) {
		lv_obj_t *child = lv_obj_get_child(page, i);
		if (lv_obj_get_child_count(child) < 2) {
			continue; // the title, or a page's own floating control
		}
		if (lv_obj_get_style_flex_flow(child, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP) {
			tiles = child;
		} else {
			rows = child;
		}
	}
	// The list is a V1 shape; the boards with the wide panel draw the tiles
	// only. Either way there is exactly one shape on screen.
	assert(tiles || rows);
	if (tiles && !lv_obj_is_hidden(tiles)) {
		assert(!rows || lv_obj_is_hidden(rows));
		*is_grid = true;
		return tiles;
	}
	assert(rows && !lv_obj_is_hidden(rows));
	*is_grid = false;
	return rows;
}

/* The theme a snapshot was taken under, for its file name: the More page is
 * laid out as tiles or as the V1 list depending on the theme, so a run that
 * keeps one snapshot per shape would keep the last theme that used it. */
static const char *theme_tag(void) {
	switch (theme_get_kind()) {
	case THEME_KIND_LIGHT:
		return "light";
	case THEME_KIND_TOKYO_NIGHT:
		return "tokyo";
	case THEME_KIND_AMBER_ON_BLACK:
		return "amber";
	case THEME_KIND_OLED:
		return "oled";
	default:
		return "dark";
	}
}

static void check_more_page(gui_config_t *cfg, const char *tag) {
	lv_screen_load(morepage_screen);
	lv_obj_update_layout(morepage_screen);
	check_within_panel(morepage_screen, cfg, "More");
	dump_snapshot(morepage_screen, tag);

	bool tiles = false;
	lv_obj_t *shown = visible_shape(morepage_screen, &tiles);
	lv_area_t area;
	// Both new destinations are on the shape that is actually drawn, and so is
	// everything the page offered before them: the page gained two entries, it
	// did not lose any.
	assert(has_entry(shown, tr("calculator")));
	assert(has_entry(shown, tr("notes")));
	assert(has_entry(shown, tr("dac")));
	assert(has_entry(shown, tr("books")));
	assert(has_entry(shown, tr("file_explorer")));
	assert(has_entry(shown, "Tag Manager"));
	assert(has_entry(shown, tr("flappy_bird")));
	assert(has_entry(shown, tr("metronome")));
	assert(has_entry(shown, tr("pet")));
	// The three games are on the page too, after Flappy Bird, which they sit
	// beside: they are the rows the More menu gained back.
	assert(has_entry(shown, tr("2048")));
	assert(has_entry(shown, tr("snake")));
	assert(has_entry(shown, tr("dino")));

	// Dark/Light use full-colour section art, not enlarged toolbar glyphs.
	// This also runs after theme changes, catching stale icon sources.
	if (theme_get_kind() == THEME_KIND_DARK || theme_get_kind() == THEME_KIND_LIGHT) {
		const char *labels[] = {"2048", "snake", "dino", "Tag Manager", "pet"};
		const lv_image_dsc_t *art[] = {
			&icon_menu_game_2048, &icon_menu_game_snake, &icon_menu_game_dino,
			&icon_menu_tag_manager, &icon_menu_pet,
		};
		assert(tiles);
		for (size_t i = 0; i < sizeof(art) / sizeof(art[0]); i++) {
			lv_obj_t *entry = entry_with_label(shown, tr(labels[i]));
			assert(entry);
			lv_obj_t *image = find_class(entry, &lv_image_class);
			assert(image && lv_image_get_src(image) == art[i]);
			assert(art[i]->header.w == icon_menu_calculator.header.w);
			assert(art[i]->header.h == icon_menu_calculator.header.h);
		}
		// Neither new tile may be a second copy of the File Manager's folder,
		// even if the art is later rerendered with the same palette.
		assert(icon_menu_tag_manager.data_size == icon_menu_file_explorer.data_size);
		assert(icon_menu_pet.data_size == icon_menu_file_explorer.data_size);
		assert(memcmp(icon_menu_tag_manager.data, icon_menu_file_explorer.data,
					  icon_menu_tag_manager.data_size) != 0);
		assert(memcmp(icon_menu_pet.data, icon_menu_file_explorer.data,
					  icon_menu_pet.data_size) != 0);
	}

	// The two destinations also have to stay apart in the compact list and on
	// the wide panel's function-glyph grid: no theme may fall back to the
	// coloured File Manager art under a tint.
	lv_obj_t *tag_entry = entry_with_label(shown, "Tag Manager");
	lv_obj_t *file_entry = entry_with_label(shown, tr("file_explorer"));
	assert(tag_entry && file_entry);
	const lv_image_dsc_t *tag_source = entry_icon_source(tag_entry);
	const lv_image_dsc_t *file_source = entry_icon_source(file_entry);
	assert(tag_source && file_source && tag_source != file_source);
	if (theme_uses_function_glyphs()) {
		assert(tag_source == &icon_tag_manager);
		assert(file_source == &icon_folder);
		lv_obj_t *pet_entry = entry_with_label(shown, tr("pet"));
		assert(pet_entry && entry_icon_source(pet_entry) == &icon_pet);
	}

	// A tile of a page that does not scroll has to be on the glass, every one
	// of them: an entry laid out past the foot of a page that cannot be
	// scrolled is an entry nobody can reach. (The list does scroll, so its
	// rows may run past the foot; there, what matters is that a row is a
	// finger target.) A grid scrolls once it holds more entries than it has
	// places for, and then the tiles past the fold are reached by scrolling,
	// the same as the list's rows.
	bool scrolls = lv_obj_is_scrollable(shown);
	int entries = 0;
	for (uint32_t i = 0; i < lv_obj_get_child_count(shown); i++) {
		lv_obj_t *entry = lv_obj_get_child(shown, i);
		if (lv_obj_is_hidden(entry)) {
			continue;
		}
		entries++;
		lv_obj_get_coords(entry, &area);
		assert(lv_obj_get_width(entry) <= (int32_t)cfg->screen_width);
		assert(area.x1 >= 0 && area.x2 < (int32_t)cfg->screen_width);
		if (tiles) {
			if (!scrolls) {
				assert(area.y1 >= 0 && area.y2 < (int32_t)cfg->screen_height);
			}
			// Small enough to fit seven on the V1's panel, never smaller
			// than a tile the label font can still be read on.
			assert(lv_obj_get_width(entry) >= bp_pick(70, 90));
			assert(lv_obj_get_height(entry) >= bp_pick(72, 90));
		} else {
			// A row is a finger target, and as wide as the list it is in.
			// Its foot is allowed past the panel: the list scrolls, which is
			// how the rows below the fold are reached at all.
			assert(lv_obj_get_height(entry) >= bp_pick(44, 56));
			assert(lv_obj_get_width(entry) >= (int32_t)cfg->screen_width - 3 * (int32_t)cfg->padding);
			if (entries == 1) {
				assert(area.y1 >= 0 && area.y1 < (int32_t)cfg->screen_height);
			}
		}
	}
	assert(entries >= 5);
	// Nine places on the V1 grid, three rows of three. Past that the page has to
	// scroll, or the last entries could not be reached at all.
	if (tiles && entries > 9) {
		assert(scrolls);
	}
}

/* The two libraries share one slot on the home screen. When Books takes it,
 * Audiobooks takes over More's library entry, so the two are never offered
 * twice and neither is ever unreachable: the choice made on the Appearance page
 * has to reach a page that was built at startup. */
static lv_obj_t *morepage_shape(bool *tiles) {
	// Leave and come back: the page catches the choice up on the way in, and
	// loading the screen that is already active would not enter it again.
	lv_screen_load(music_screen);
	lv_screen_load(morepage_screen);
	lv_obj_update_layout(morepage_screen);
	return visible_shape(morepage_screen, tiles);
}

static void check_more_audiobooks_option(gui_config_t *cfg) {
	// Both shapes: the tiles every board uses and the V1's function-glyph
	// list, each rebuilt from the same entries.
	const theme_kind_t shape_themes[] = {THEME_KIND_DARK, THEME_KIND_TOKYO_NIGHT};
	int shapes = bp_is_tempotec_v1() ? 2 : 1;

	for (int shape = 0; shape < shapes; shape++) {
		theme_set_kind(shape_themes[shape]);
		bool tiles = false;

		// Off: More is exactly what it always was.
		lv_obj_t *shown = morepage_shape(&tiles);
		assert(has_entry(shown, tr("books")));
		assert(!has_entry(shown, tr("audiobooks")));

		// On: Books has moved to the home screen, so More offers the other
		// library in its place rather than repeating Books.
		config_set_bool("ui", "home_books", true);
		shown = morepage_shape(&tiles);
		assert(has_entry(shown, tr("audiobooks")));
		assert(!has_entry(shown, tr("books")));
		check_within_panel(morepage_screen, cfg, "More with Audiobooks");

		// And the row opens the section it names.
		lv_obj_t *entry = entry_with_label(shown, tr("audiobooks"));
		assert(entry);
		lv_obj_send_event(entry, LV_EVENT_CLICKED, NULL);
		assert(lv_screen_active() == audiobooks_screen);

		// Off again: the row goes away rather than staying as a duplicate.
		config_set_bool("ui", "home_books", false);
		shown = morepage_shape(&tiles);
		assert(!has_entry(shown, tr("audiobooks")));
	}
	theme_set_kind(THEME_KIND_DARK);
}

/* ---- the Calculator page ------------------------------------------------- */

/* The card the sum is drawn in: one wide label alone in its own card, unlike
 * a key (a button, whether it wears a caption or a glyph) or the keypad (which
 * holds the twenty of them). */
static lv_obj_t *calc_display_label(lv_obj_t *page) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(page); i++) {
		lv_obj_t *child = lv_obj_get_child(page, i);
		if (lv_obj_check_type(child, &lv_button_class) || lv_obj_get_child_count(child) != 1) {
			continue;
		}
		lv_obj_t *only = lv_obj_get_child(child, 0);
		if (lv_obj_check_type(only, &lv_label_class)) {
			return only;
		}
	}
	return NULL;
}

/* The keyboard a page created, found by its shape: the tray is the child that
 * holds a panel of four rows. Its own header keeps it opaque, so the page test
 * looks for the object keyboard_set_visible() hides. */
static lv_obj_t *keyboard_tray_of(lv_obj_t *screen) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
		lv_obj_t *child = lv_obj_get_child(screen, i);
		for (uint32_t j = 0; j < lv_obj_get_child_count(child); j++) {
			if (lv_obj_get_child_count(lv_obj_get_child(child, j)) >= 4) {
				return child;
			}
		}
	}
	return NULL;
}

/* A page's corner button: the one button that is a child of the screen itself,
 * rather than a key inside the keyboard. */
static lv_obj_t *corner_button_of(lv_obj_t *screen) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
		lv_obj_t *child = lv_obj_get_child(screen, i);
		if (lv_obj_check_type(child, &lv_button_class)) {
			return child;
		}
	}
	return NULL;
}

/* A note past what the editor will hold, and inside what the store will read:
 * one byte per character, so it is exactly the editor's limit plus a hundred. */
static const char *long_note_text(void) {
	static char text[NOTES_MAX_CHARS + 101];
	if (!text[0]) {
		for (size_t i = 0; i < NOTES_MAX_CHARS + 100; i++) {
			text[i] = (i % 61 == 60) ? '\n' : (char)('a' + (i % 26));
		}
		text[NOTES_MAX_CHARS + 100] = '\0';
	}
	return text;
}

/* A note just inside what the editor will hold: the largest note that is
 * opened in the editor rather than handed to the viewer. */
static const char *big_note_text(void) {
	static char text[NOTES_MAX_CHARS - 383];
	if (!text[0]) {
		for (size_t i = 0; i < sizeof(text) - 1; i++) {
			text[i] = (i % 61 == 60) ? '\n' : (char)('a' + (i % 26));
		}
		text[sizeof(text) - 1] = '\0';
	}
	return text;
}

static double seconds_since(const struct timespec *from) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - from->tv_sec) + (now.tv_nsec - from->tv_nsec) / 1e9;
}

static void check_calculator_page(gui_config_t *cfg, const char *tag) {
	calculator_open();
	assert(lv_screen_active() == calculator_screen);
	lv_obj_update_layout(calculator_screen);
	check_within_panel(calculator_screen, cfg, "Calculator");
	dump_snapshot(calculator_screen, tag);

	lv_obj_t *display = calc_display_label(calculator_screen);
	assert(display);

	// Twenty keys, every one of them a finger target: the V1's own corner
	// buttons are 36 px, so a key is not allowed to be smaller than that.
	lv_obj_t *keypad = NULL;
	for (uint32_t i = 0; i < lv_obj_get_child_count(calculator_screen); i++) {
		lv_obj_t *child = lv_obj_get_child(calculator_screen, i);
		if (lv_obj_get_child_count(child) == 20) {
			keypad = child;
		}
	}
	assert(keypad);

	/* The keys whose caption is a sign are drawn a size up and in the bold
	 * weight, the digits at the keypad's own size: a face sets a
	 * multiplication sign a little over half a digit's height and a hyphen a
	 * tenth of it, so at the digits' size the operator column reads as specks
	 * beside the numbers it works on. Both fonts are measured here, and every
	 * caption is required to stay inside the key it is drawn on, so a step up
	 * too far is a failure rather than a clipped glyph. */
	const lv_font_t *digit_font = NULL;
	const lv_font_t *symbol_font = NULL;
	for (uint32_t i = 0; i < lv_obj_get_child_count(keypad); i++) {
		lv_obj_t *key = lv_obj_get_child(keypad, i);
		assert(lv_obj_get_width(key) >= bp_pick(34, 60));
		assert(lv_obj_get_height(key) >= bp_pick(28, 60));

		// The backspace key draws an icon rather than a caption.
		lv_obj_t *label = find_class(key, &lv_label_class);
		if (!label) {
			continue;
		}
		const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
		assert(font);
		const char *text = lv_label_get_text(label);
		if (text[0] >= '0' && text[0] <= '9') {
			digit_font = font;
		} else if (strcmp(text, "AC") != 0 && strcmp(text, "%") != 0 && strcmp(text, ".") != 0) {
			symbol_font = font;
		}
		lv_area_t key_area, label_area;
		lv_obj_get_coords(key, &key_area);
		lv_obj_get_coords(label, &label_area);
		assert(label_area.y1 >= key_area.y1 && label_area.y2 <= key_area.y2);
	}
	assert(digit_font && symbol_font);
	assert(lv_font_get_line_height(symbol_font) > lv_font_get_line_height(digit_font));

	// The keys that carry a glyph rather than a letter are there too, and the
	// four operations are spelled the way the rest of the interface spells
	// them: a multiplication sign, not an asterisk, and the minus sign the
	// face draws half as long again as the hyphen it is often written with.
	assert(find_label(keypad, "AC"));
	assert(find_label(keypad, "="));
	assert(find_label(keypad, "\xc2\xb1"));
	assert(find_label(keypad, "%"));
	assert(find_label(keypad, "\xc3\xb7"));
	assert(find_label(keypad, "\xc3\x97"));
	assert(find_label(keypad, "\xe2\x88\x92"));

	// A sum typed on the glass. The display follows every key, which is the
	// whole of what the page does with the model.
	press_key(keypad, "AC");
	assert(strcmp(lv_label_get_text(display), "0") == 0);
	press_key(keypad, "1");
	press_key(keypad, "2");
	assert(strcmp(lv_label_get_text(display), "12") == 0);
	press_key(keypad, "+");
	press_key(keypad, "7");
	assert(strcmp(lv_label_get_text(display), "7") == 0);
	press_key(keypad, "=");
	assert(strcmp(lv_label_get_text(display), "19") == 0);

	// The error state is drawn in the language's own words, not the model's.
	press_key(keypad, "AC");
	press_key(keypad, "9");
	press_key(keypad, "\xc3\xb7");
	press_key(keypad, "0");
	press_key(keypad, "=");
	assert(strcmp(lv_label_get_text(display), tr("calculator_error")) == 0);
	press_key(keypad, "AC");
	assert(strcmp(lv_label_get_text(display), "0") == 0);

	// The display label stays inside its card with the widest number it can
	// hold -- a result too long for the card is the one thing that would put
	// ink outside the panel.
	press_key(keypad, "9");
	for (int i = 0; i < 11; i++) {
		press_key(keypad, "9");
	}
	assert(lv_obj_get_self_width(display) <= lv_obj_get_width(lv_obj_get_parent(display)));

	// Leaving and coming back shows the sum where it was left: the page is
	// built once and the model outlives the visit.
	press_key(keypad, "AC");
	press_key(keypad, "7");
	press_key(keypad, "+");
	press_key(keypad, "5");
	lv_screen_load(morepage_screen);
	calculator_open();
	assert(strcmp(lv_label_get_text(display), "5") == 0);
	press_key(keypad, "=");
	assert(strcmp(lv_label_get_text(display), "12") == 0);
	press_key(keypad, "AC");
}

/* ---- the Metronome page -------------------------------------------------- */

/* The page's two cards, in the order they are built: the tempo with its start
 * and tap, then the bar. Found through the container settingsrow_page() made,
 * which is the one child that is not a label and holds more than one thing. */
static lv_obj_t *metronome_card(lv_obj_t *page, int index) {
	lv_obj_t *container = NULL;
	for (uint32_t i = 0; i < lv_obj_get_child_count(page); i++) {
		lv_obj_t *child = lv_obj_get_child(page, i);
		if (!lv_obj_check_type(child, &lv_label_class) && lv_obj_get_child_count(child) >= 2) {
			container = child;
			break;
		}
	}
	assert(container);
	return lv_obj_get_child(container, index);
}

/* The row of one dot per beat: the second thing on the tempo card, under the
 * number and its unit. */
static lv_obj_t *metronome_dots(lv_obj_t *page) {
	lv_obj_t *readout = lv_obj_get_child(metronome_card(page, 0), 0);
	return lv_obj_get_child(readout, 1);
}

/* The tempo itself, big, beside the word for it. */
static lv_obj_t *metronome_readout(lv_obj_t *page) {
	lv_obj_t *unit = find_label(page, tr("metronome_bpm"));
	assert(unit);
	lv_obj_t *group = lv_obj_get_parent(unit);
	for (uint32_t i = 0; i < lv_obj_get_child_count(group); i++) {
		lv_obj_t *child = lv_obj_get_child(group, i);
		if (child != unit && lv_obj_check_type(child, &lv_label_class)) {
			return child;
		}
	}
	assert(!"the metronome has no tempo readout");
	return NULL;
}

/* One of the seven beats of a bar, by its number. */
static lv_obj_t *beat_choice(lv_obj_t *page, int beats) {
	char text[4];
	snprintf(text, sizeof(text), "%d", beats);
	lv_obj_t *label = find_label(lv_obj_get_child(metronome_card(page, 1), 1), text);
	assert(label);
	return label;
}

static void choose_beats(lv_obj_t *page, int beats) {
	lv_obj_send_event(lv_obj_get_parent(beat_choice(page, beats)), LV_EVENT_CLICKED, NULL);
}

/* The pet: its page is built the first time it is opened, everything it draws
 * is inside the panel, the words are translated, and the button really pets
 * the creature rather than only moving a picture. */
/* The page's two corner controls, outer first: the wardrobe's star and the
 * pencil. They carry icons rather than words, so they are found by the place
 * they are drawn in. */
static void find_corner_buttons(lv_obj_t *screen, lv_obj_t **outer, lv_obj_t **inner, lv_obj_t **third) {
	*outer = NULL;
	*inner = NULL;
	if (third) {
		*third = NULL;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
		lv_obj_t *child = lv_obj_get_child(screen, (int32_t)i);
		if (!lv_obj_check_type(child, &lv_button_class)) {
			continue;
		}
		lv_area_t area;
		lv_obj_get_coords(child, &area);
		if (area.y1 > 80) {
			continue; // not a corner control
		}
		if (!*outer) {
			*outer = child;
		} else if (!*inner) {
			*inner = child;
		} else if (third && !*third) {
			*third = child;
		}
	}
}

static void check_pet_page(gui_config_t *cfg, const char *tag) {
	// A pet nobody has chosen yet opens on the chooser, with three notes in the
	// wallet: enough to dress it and leave something for the wardrobe.
	pet_state.notes = 500;
	pet_state.species = PET_SPECIES_NONE;
	petpage_open();
	assert(lv_screen_active() == petpage_screen);
	lv_obj_update_layout(petpage_screen);
	check_within_panel(petpage_screen, cfg, "Pet chooser");
	char chooser_tag[64];
	snprintf(chooser_tag, sizeof(chooser_tag), "%s-chooser", tag);
	dump_snapshot(petpage_screen, chooser_tag);

	// The chooser is the first thing a new pet shows: the three animals, and
	// the name it will answer to.
	lv_obj_t *chooser_title = find_label(petpage_screen, tr("pet_customise"));
	assert(chooser_title);
	assert(lv_obj_is_visible(chooser_title));
	check_clear_of_back_button(chooser_title, cfg, "Pet chooser heading");
	assert(find_label(petpage_screen, tr("pet_species_cat")));
	assert(find_label(petpage_screen, tr("pet_species_dog")));
	assert(find_label(petpage_screen, tr("pet_species_fish")));
	assert(find_label(petpage_screen, tr("name")));

	// Choosing one closes the sheet and the page is that animal.
	press_key(petpage_screen, tr("pet_species_cat"));
	assert(pet_species(&pet_state) == PET_SPECIES_CAT);
	assert(!lv_obj_is_visible(chooser_title));
	lv_obj_update_layout(petpage_screen);
	check_within_panel(petpage_screen, cfg, "Pet");
	dump_snapshot(petpage_screen, tag);

	char name[40];
	pet_name(name, sizeof(name));
	assert(find_label(petpage_screen, name));
	// The two meters sit in the card's top right corner, as numbers as well as
	// bars, and the wallet is under them.
	assert(find_label(petpage_screen, "500"));
	assert(find_label(petpage_screen, "50")); // the happiness it starts with
	assert(find_label(petpage_screen, "80")); // and the energy
	assert(find_label(petpage_screen, tr("pet_feed_me")));

	// The dog, for its ears. They are the one part of the drawing that has been
	// argued about more than any other, and the suite only ever looked at the
	// cat -- so a pair of ears could go back to being a bear's and nothing
	// would notice.
	pet_state.species = PET_SPECIES_DOG;
	petpage_open();
	assert(pet_species(&pet_state) == PET_SPECIES_DOG);
	lv_obj_update_layout(petpage_screen);
	check_within_panel(petpage_screen, cfg, "Pet (dog)");
	char dog_tag[64];
	snprintf(dog_tag, sizeof(dog_tag), "%s-dog", tag);
	dump_snapshot(petpage_screen, dog_tag);
	pet_state.species = PET_SPECIES_CAT;
	petpage_open();
	lv_obj_update_layout(petpage_screen);

	// A pat is a tap on the animal, not a button at the foot of the page.
	int pets = pet_state.pets;
	lv_obj_t *creature = petpage_creature();
	assert(creature);
	lv_obj_send_event(creature, LV_EVENT_CLICKED, NULL);
	assert(pet_state.pets == pets + 1);

	// A tap on the glass is a press and a release, and a finger on this panel
	// takes about half a second over it -- long enough for LVGL to call the
	// press a long one and start the Petting Mode meter. It is still a tap:
	// the animal is petted, and the click is not thrown away by a meter that
	// never filled. (The suite used to send CLICKED straight at the animal,
	// which is exactly why it did not notice this.)
	pet_state.last_pet = 0;
	int slow_pets = pet_state.pets;
	lv_obj_send_event(creature, LV_EVENT_PRESSED, NULL);
	lv_obj_send_event(creature, LV_EVENT_LONG_PRESSED, NULL);
	lv_tick_inc(300);
	lv_timer_handler();
	lv_obj_send_event(creature, LV_EVENT_RELEASED, NULL);
	lv_obj_send_event(creature, LV_EVENT_CLICKED, NULL);
	assert(pet_state.pets == slow_pets + 1);
	assert(pet_state.hold_day == 0); // a slow tap is not a groom

	// And the pat is answered: one of three tiny reactions, picked from the
	// clock. The clock here is fixed at a second that divides by three, so it
	// is always the heart -- and the heart is a picture over the head, not a
	// speech bubble and not a page of its own.
	lv_obj_update_layout(petpage_screen);
	char pat_tag[64];
	snprintf(pat_tag, sizeof(pat_tag), "%s-pat", tag);
	check_within_panel(petpage_screen, cfg, "Pet pat");
	dump_snapshot(petpage_screen, pat_tag);

	// Petting Mode: the finger stays down and the meter fills a heart at a
	// time; the third heart pays the day's groom, once.
	pet_state.hold_day = 0;
	const int happy_before_hold = pet_state.happy;
	lv_obj_send_event(creature, LV_EVENT_LONG_PRESSED, NULL);
	lv_tick_inc(600);
	lv_timer_handler();
	lv_obj_update_layout(petpage_screen);
	char hold_tag[64];
	snprintf(hold_tag, sizeof(hold_tag), "%s-hold", tag);
	dump_snapshot(petpage_screen, hold_tag);
	assert(pet_state.hold_day == 0); // one heart of three
	for (int i = 0; i < 3; i++) {
		lv_tick_inc(600);
		lv_timer_handler();
	}
	assert(pet_state.hold_day != 0);
	assert(pet_state.happy > happy_before_hold);

	// The release of a hold arrives as a click as well. It is not a second pat,
	// and it must not be: the hand that held the animal is the same hand that
	// lets go of it.
	pets = pet_state.pets;
	lv_obj_send_event(creature, LV_EVENT_RELEASED, NULL);
	lv_obj_send_event(creature, LV_EVENT_CLICKED, NULL);
	assert(pet_state.pets == pets);

	// The volume keys never reached the interface before; now they do, and the
	// animal answers them without leaving the page. A jump for a step up.
	petpage_poke(PET_POKE_VOLUME_UP);
	lv_tick_inc(1100); // the second frame of the same jump
	lv_timer_handler();
	lv_obj_update_layout(petpage_screen);
	check_within_panel(petpage_screen, cfg, "Pet volume up");
	char jump_tag[64];
	snprintf(jump_tag, sizeof(jump_tag), "%s-jump", tag);
	dump_snapshot(petpage_screen, jump_tag);
	petpage_poke(PET_POKE_VOLUME_DOWN);
	lv_obj_update_layout(petpage_screen);
	char duck_tag[64];
	snprintf(duck_tag, sizeof(duck_tag), "%s-duck", tag);
	dump_snapshot(petpage_screen, duck_tag);
	// It is a pose, not a permanent state: a few seconds later the animal is
	// breathing again on its own.
	lv_tick_inc(1100);
	lv_timer_handler();
	lv_tick_inc(1100);
	lv_timer_handler();
	lv_tick_inc(1100);
	lv_timer_handler();
	check_within_panel(petpage_screen, cfg, "Pet after the reaction");

	// The genre's accessory. One thing, from the tags, and only while there is
	// music: shades for the electronic family, gold at the neck for hip hop,
	// studs for rock, and a cup on the floor for the calm ones. Which one is
	// pet_genre_look()'s business and is tested with the other rules; here it
	// is the drawing that is looked at.
	pet_listening = true;
	static const struct {
		pet_genre_look_t look;
		const char *tag;
	} looks[] = {
		{PET_LOOK_SHADES, "shades"},
		{PET_LOOK_CHAIN, "chain"},
		{PET_LOOK_COLLAR, "collar"},
		{PET_LOOK_CUP, "cup"},
	};
	for (size_t i = 0; i < sizeof(looks) / sizeof(looks[0]); i++) {
		pet_look = looks[i].look;
		petpage_open();
		lv_obj_update_layout(petpage_screen);
		check_within_panel(petpage_screen, cfg, "Pet accessory");
		char look_tag[64];
		snprintf(look_tag, sizeof(look_tag), "%s-%s", tag, looks[i].tag);
		dump_snapshot(petpage_screen, look_tag);
	}
	pet_look = PET_LOOK_NONE;

	// And the volume, worn on the face: past eighty the eyes are wide open,
	// between twenty and forty the pet rests with a heavy lid. Neither is a
	// mood of its own -- the mood is where it always was.
	pet_volume = 90;
	petpage_open();
	lv_obj_update_layout(petpage_screen);
	char hyped_tag[64];
	snprintf(hyped_tag, sizeof(hyped_tag), "%s-hyped", tag);
	dump_snapshot(petpage_screen, hyped_tag);
	pet_volume = 30;
	petpage_open();
	lv_obj_update_layout(petpage_screen);
	char resting_tag[64];
	snprintf(resting_tag, sizeof(resting_tag), "%s-resting", tag);
	dump_snapshot(petpage_screen, resting_tag);
	pet_volume = 60;
	pet_listening = false;
	petpage_open();
	lv_obj_update_layout(petpage_screen);

	// The box on the floor: the day's fortune, the shelf, and the hour's game.
	pet_state.fortune_day = pet_day_now(petwatch_now());
	pet_state.fortune_paid = 0;
	pet_state.drop_pending = 1;
	for (int i = 0; i < PET_DROP_COUNT; i++) {
		pet_state.found[i] = 0;
	}
	pet_state.treat_hour = 0;
	petpage_open();
	lv_obj_update_layout(petpage_screen);

	// What the floor looks like with something on it.
	char find_tag[64];
	snprintf(find_tag, sizeof(find_tag), "%s-find", tag);
	dump_snapshot(petpage_screen, find_tag);

	lv_obj_t *box = petpage_box();
	assert(box);
	assert(!lv_obj_is_hidden(box));
	lv_obj_send_event(box, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(petpage_screen);
	lv_obj_t *box_title = find_label(petpage_screen, tr("pet_box"));
	assert(box_title);
	assert(lv_obj_is_visible(box_title));
	assert(find_label(petpage_screen, tr("pet_box_shelf")));
	assert(find_label(petpage_screen, tr("pet_box_play")));
	check_within_panel(petpage_screen, cfg, "Pet box");
	char box_tag[64];
	snprintf(box_tag, sizeof(box_tag), "%s-box", tag);
	dump_snapshot(petpage_screen, box_tag);

	// Answering the fortune pays fifty notes, once.
	int boxed_notes = pet_state.notes;
	press_visible_key(petpage_screen, tr("pet_box_done"));
	assert(pet_state.notes == boxed_notes + PET_FORTUNE_NOTES);
	assert(pet_state.fortune_paid == pet_state.fortune_day);
	lv_obj_update_layout(petpage_screen);
	assert(find_label(petpage_screen, tr("pet_box_answered")));

	// The find is on the floor beside the animal, and a tap puts it on the
	// shelf: that is the whole inventory.
	lv_obj_send_event(box, LV_EVENT_CLICKED, NULL); // the box closes itself
	lv_obj_update_layout(petpage_screen);
	assert(!lv_obj_is_visible(box_title));
	lv_obj_t *find = petpage_find();
	assert(find);
	assert(!lv_obj_is_hidden(find));
	lv_obj_send_event(find, LV_EVENT_CLICKED, NULL);
	assert(pet_state.drop_pending == 0);
	assert(pet_state.found[PET_DROP_BONE] == 1);
	lv_obj_update_layout(petpage_screen);
	assert(lv_obj_is_hidden(find));

	// The hour's game: six notes cross the line, and turning up is worth five
	// even with none of them caught. It takes about nine seconds.
	lv_obj_send_event(box, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(petpage_screen);
	press_visible_key(petpage_screen, tr("pet_box_play"));
	int game_notes = pet_state.notes;
	// A second of the game, so the snapshot has a note in the lane.
	for (int i = 0; i < 24; i++) {
		lv_tick_inc(30);
		lv_timer_handler();
	}
	lv_obj_update_layout(petpage_screen);
	char game_tag[64];
	snprintf(game_tag, sizeof(game_tag), "%s-game", tag);
	dump_snapshot(petpage_screen, game_tag);
	for (int i = 0; i < 460; i++) { // 460 * 30 ms, a bit more than the game
		lv_tick_inc(30);
		lv_timer_handler();
	}
	assert(pet_state.notes == game_notes + PET_TREAT_MIN_NOTES);
	assert(pet_treat_ready(&pet_state, petwatch_now()) == false);
	lv_obj_update_layout(petpage_screen);
	assert(find_label(petpage_screen, tr("pet_box_hour")));
	lv_obj_send_event(box, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(petpage_screen);
	assert(!lv_obj_is_visible(box_title));

	// The card knows the hour and the month: the same animal at midnight in
	// October, with the lamp lit, a moon up and the autumn leaves coming down.
	pet_now = 1761868800; // 2025-10-31, 00:00 UTC
	petpage_open();
	lv_obj_update_layout(petpage_screen);
	check_within_panel(petpage_screen, cfg, "Pet at night");
	char night_tag[64];
	snprintf(night_tag, sizeof(night_tag), "%s-night", tag);
	dump_snapshot(petpage_screen, night_tag);
	// And in April, with a cloud up and the rain coming down.
	pet_now = 1744286400; // 2025-04-10, 12:00 UTC
	petpage_open();
	lv_obj_update_layout(petpage_screen);
	char spring_tag[64];
	snprintf(spring_tag, sizeof(spring_tag), "%s-spring", tag);
	dump_snapshot(petpage_screen, spring_tag);
	pet_now = 1752580800; // back to the summer afternoon
	petpage_open();
	lv_obj_update_layout(petpage_screen);

	// The pencil brings the chooser back; the pencil closes it again.
	lv_obj_t *star = NULL;
	lv_obj_t *pencil = NULL;
	lv_obj_t *notebutton = NULL;
	find_corner_buttons(petpage_screen, &star, &pencil, &notebutton);
	assert(star && pencil && notebutton);
	lv_obj_send_event(pencil, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(petpage_screen);
	assert(lv_obj_is_visible(chooser_title));
	lv_obj_send_event(pencil, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(petpage_screen);
	assert(!lv_obj_is_visible(chooser_title));

	// The wardrobe: a hat costs 120 of the 500, bought and worn without a
	// second tap. The wallet is put back to 500 first: the fortune and the
	// game above have paid into it, and this is a test of a price.
	pet_state.notes = 500;
	lv_obj_send_event(star, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(petpage_screen);
	lv_obj_t *wardrobe_title = find_label(petpage_screen, tr("pet_wardrobe"));
	assert(wardrobe_title);
	assert(lv_obj_is_visible(wardrobe_title));
	assert(find_label(petpage_screen, tr("pet_notes_balance")));

	char wardrobe_tag[64];
	snprintf(wardrobe_tag, sizeof(wardrobe_tag), "%s-wardrobe", tag);
	check_within_panel(petpage_screen, cfg, "Pet wardrobe");
	dump_snapshot(petpage_screen, wardrobe_tag);

	press_key(petpage_screen, tr("pet_item_party_hat"));
	assert(pet_state.notes == 380);
	assert(pet_owned(&pet_state, PET_ITEM_PARTY_HAT));
	assert(pet_wearing(&pet_state) == (int)PET_ITEM_PARTY_HAT);
	assert(find_label(petpage_screen, tr("pet_wearing_now")));

	// A freeze is a stock, not a garment: it can be bought twice, and it is
	// never worn.
	press_key(petpage_screen, tr("pet_item_streak_freeze"));
	assert(pet_freezes(&pet_state) == 1);
	press_key(petpage_screen, tr("pet_item_streak_freeze"));
	assert(pet_freezes(&pet_state) == 2);
	assert(pet_wearing(&pet_state) == (int)PET_ITEM_PARTY_HAT);

	// The star closes what it opened, and the page is the pet again.
	lv_obj_send_event(star, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(petpage_screen);
	assert(!lv_obj_is_visible(wardrobe_title));

	// The day's tap-along: the note in the corner opens it, and finishing pays
	// whatever the score was worth -- two notes for turning up.
	assert(pet_rhythm_ready(&pet_state));
	int before_game = pet_state.notes;
	lv_obj_send_event(notebutton, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(petpage_screen);
	lv_obj_t *tap_title = find_label(petpage_screen, tr("pet_tap_game"));
	assert(tap_title);
	assert(lv_obj_is_visible(tap_title));
	check_within_panel(petpage_screen, cfg, "Pet tap-along");
	char tap_tag[64];
	snprintf(tap_tag, sizeof(tap_tag), "%s-tap", tag);
	dump_snapshot(petpage_screen, tap_tag);

	press_visible_key(petpage_screen, tr("ok"));
	lv_obj_update_layout(petpage_screen);
	assert(!lv_obj_is_visible(tap_title));
	assert(pet_state.notes == before_game + 2);
	assert(!pet_rhythm_ready(&pet_state));

	// Leaving gives the timer and the animations back: the page is a still
	// picture again, and the state has been written out.
	lv_screen_load(morepage_screen);
}

static void check_metronome_page(gui_config_t *cfg, const char *tag) {
	metronome_open();
	assert(lv_screen_active() == metronome_screen);
	lv_obj_update_layout(metronome_screen);
	check_within_panel(metronome_screen, cfg, "Metronome");
	dump_snapshot(metronome_screen, tag);

	lv_obj_t *readout = metronome_readout(metronome_screen);
	lv_obj_t *slider = find_class(metronome_screen, &lv_slider_class);
	assert(slider);
	lv_obj_t *dots = metronome_dots(metronome_screen);
	lv_obj_t *segments = lv_obj_get_child(metronome_card(metronome_screen, 1), 1);
	// Seven beats, drawn as seven: up to seven is the whole of what the bar
	// offers, and a bar that could not be counted to seven would be a promise
	// the page does not keep.
	assert(lv_obj_get_child_count(dots) == 7);
	assert(lv_obj_get_child_count(segments) == 7);

	// The page opens at the tempo it was built with, 120 in a fresh config, and
	// the knob is where that number says it is.
	assert(strcmp(lv_label_get_text(readout), "120") == 0);
	assert(lv_slider_get_value(slider) == 120);

	// Both ends of the scale, and nothing outside them: the slider is asked for
	// 900 and the number beside it still reads the top of the range.
	lv_slider_set_value(slider, 30, LV_ANIM_OFF);
	lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
	assert(strcmp(lv_label_get_text(readout), "30") == 0);
	lv_slider_set_value(slider, 250, LV_ANIM_OFF);
	lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
	assert(strcmp(lv_label_get_text(readout), "250") == 0);
	lv_slider_set_value(slider, 900, LV_ANIM_OFF);
	lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
	assert(strcmp(lv_label_get_text(readout), "250") == 0);
	lv_slider_set_value(slider, 0, LV_ANIM_OFF);
	lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
	assert(strcmp(lv_label_get_text(readout), "30") == 0);

	// Tap tempo: two taps a quarter of a second apart are 240 BPM.
	lv_obj_t *tap = lv_obj_get_parent(find_label(metronome_screen, tr("metronome_tap")));
	lv_tick_inc(250);
	lv_obj_send_event(tap, LV_EVENT_CLICKED, NULL);
	lv_tick_inc(250);
	lv_obj_send_event(tap, LV_EVENT_CLICKED, NULL);
	assert(strcmp(lv_label_get_text(readout), "240") == 0);
	// The slider follows a tap: there is one tempo on the page, shown twice.
	assert(lv_slider_get_value(slider) == 240);

	// Taps faster than the scale allows leave the tempo at the top of it.
	for (int i = 0; i < 3; i++) {
		lv_tick_inc(60);
		lv_obj_send_event(tap, LV_EVENT_CLICKED, NULL);
	}
	assert(strcmp(lv_label_get_text(readout), "250") == 0);

	// A bar of seven is seven dots, and the chosen beat wears the accent.
	choose_beats(metronome_screen, 7);
	assert(config_get_int("metronome", "beats", 0) == 7);
	lv_obj_update_layout(metronome_screen);
	for (int i = 0; i < 7; i++) {
		assert(!lv_obj_is_hidden(lv_obj_get_child(dots, i)));
	}
	lv_color_t accent = theme()->accent;
	assert(lv_color_eq(lv_obj_get_style_bg_color(lv_obj_get_parent(beat_choice(metronome_screen, 7)), LV_PART_MAIN),
					   accent));
	assert(!lv_color_eq(lv_obj_get_style_bg_color(lv_obj_get_parent(beat_choice(metronome_screen, 1)), LV_PART_MAIN),
						accent));

	// A bar of three is three dots; the rest are not drawn at all.
	choose_beats(metronome_screen, 3);
	for (int i = 0; i < 3; i++) {
		assert(!lv_obj_is_hidden(lv_obj_get_child(dots, i)));
	}
	for (int i = 3; i < 7; i++) {
		assert(lv_obj_is_hidden(lv_obj_get_child(dots, i)));
	}

	// Running: the click is given the tempo and the bar the page is showing.
	assert(!click_started);
	lv_obj_t *start = lv_obj_get_parent(find_label(metronome_screen, tr("metronome_start")));
	lv_obj_send_event(start, LV_EVENT_CLICKED, NULL);
	assert(click_started);
	assert(click_tempo == 250);
	assert(click_bar == 3);

	// The dots follow the beat the click thread has written, not a clock of the
	// page's own: the first of the bar is the lit one, in the accent colour.
	click_written = 1;
	click_last = 0;
	lv_tick_inc(40);
	lv_timer_handler();
	assert(lv_obj_get_style_bg_opa(lv_obj_get_child(dots, 0), LV_PART_MAIN) == LV_OPA_COVER);
	assert(lv_color_eq(lv_obj_get_style_bg_color(lv_obj_get_child(dots, 0), LV_PART_MAIN), accent));
	assert(lv_obj_get_style_bg_opa(lv_obj_get_child(dots, 1), LV_PART_MAIN) != LV_OPA_COVER);

	// The second beat moves the flash on, so what is lit is a count and not a
	// decoration that happens to be on the first dot.
	click_written = 2;
	click_last = 1;
	lv_tick_inc(40);
	lv_timer_handler();
	assert(lv_obj_get_style_bg_opa(lv_obj_get_child(dots, 1), LV_PART_MAIN) == LV_OPA_COVER);
	assert(lv_obj_get_style_bg_opa(lv_obj_get_child(dots, 0), LV_PART_MAIN) != LV_OPA_COVER);

	// A new bar while it is running reaches the click without stopping it.
	choose_beats(metronome_screen, 2);
	assert(click_started);
	assert(click_bar == 2);

	// Leaving the page stops the click: the page is the only place it can be
	// stopped from.
	lv_screen_load(morepage_screen);
	assert(!click_started);

	// And coming back shows the tempo and the bar where they were left.
	metronome_open();
	assert(lv_screen_active() == metronome_screen);
	assert(strcmp(lv_label_get_text(readout), "250") == 0);
	assert(config_get_int("metronome", "bpm", 0) == 250);
	assert(find_label(metronome_screen, tr("metronome_start")));
	lv_screen_load(morepage_screen);
}

/* ---- the Notes page ------------------------------------------------------ */

static void check_notes_page(gui_config_t *cfg, const char *dir, const char *tag, const char *tag_editor,
							 const char *tag_editor_edit, const char *tag_after, const char *tag_long) {
	// Through the page's own door, as the More menu opens it: that is what
	// reads the card and then shows the listing.
	notes_open();
	assert(lv_screen_active() == notes_screen);
	lv_obj_update_layout(notes_screen);
	check_within_panel(notes_screen, cfg, "Notes");
	dump_snapshot(notes_screen, tag);

	// The way to write something, then the notes the card holds, by the names
	// they are shown under -- without the .txt they are stored under.
	assert(find_label(notes_screen, tr("notes_new")));
	lv_obj_t *row = find_label(notes_screen, "shopping");
	assert(row);
	assert(find_label(notes_screen, "recipe"));
	// A picture in the folder is not offered: it is not a note.
	assert(!find_label(notes_screen, "cover"));

	// A row is a finger target on the V1 as well.
	lv_obj_t *row_btn = lv_obj_get_parent(row);
	while (row_btn && !lv_obj_check_type(row_btn, &lv_button_class)) {
		row_btn = lv_obj_get_parent(row_btn);
	}
	assert(row_btn);
	assert(lv_obj_get_height(row_btn) >= bp_pick(40, 56));

	// Tapping it opens that note, to be read.
	lv_obj_send_event(row_btn, LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == noteeditor_screen);
	lv_obj_update_layout(noteeditor_screen);
	check_within_panel(noteeditor_screen, cfg, "Note editor");
	dump_snapshot(noteeditor_screen, tag_editor);

	lv_obj_t *field = find_class(noteeditor_screen, &lv_textarea_class);
	assert(field);
	// The shared caret is a one-pixel accent line in either state, so thin
	// letters remain legible while typing.
	assert(lv_obj_get_style_border_width(field, LV_PART_CURSOR) == 1);
	lv_obj_add_state(field, LV_STATE_FOCUSED);
	assert(lv_obj_get_style_border_width(field, LV_PART_CURSOR) == 1);
	lv_obj_remove_state(field, LV_STATE_FOCUSED);
	// The file holds a final newline -- textfile_write() puts one on every note
	// -- and the editor shows the file as it is.
	assert(strcmp(lv_textarea_get_text(field), "milk and bread\n") == 0);
	// The title is the note's name without the extension.
	assert(find_label(noteeditor_screen, "shopping"));

	// Opening a note is reading it: no keyboard, no caret, and the note has the
	// whole page rather than the part above a keyboard.
	assert(lv_obj_is_hidden(keyboard_tray_of(noteeditor_screen)));
	int32_t read_height = lv_obj_get_height(field);
	assert(read_height > bp_pick(200, 400));

	// The corner button is the way into writing, and out of it again.
	lv_obj_t *toggle = corner_button_of(noteeditor_screen);
	assert(toggle);
	assert(find_image_button(toggle, &icon_check) == NULL);
	assert(find_image_button(toggle, &icon_pencil));

	// A tap on the text of a note is a tap on the note: it does not move a
	// caret, and it changes nothing on the card.
	assert(lv_textarea_get_cursor_click_pos(field) == false);
	char *text = read_note(dir, "shopping.txt");
	assert(text && strcmp(text, "milk and bread\n") == 0);
	free(text);

	// Writing: the keyboard comes up, the note makes room for it, and the
	// pencil becomes the check.
	lv_obj_send_event(toggle, LV_EVENT_CLICKED, NULL);
	lv_obj_update_layout(noteeditor_screen);
	assert(!lv_obj_is_hidden(keyboard_tray_of(noteeditor_screen)));
	assert(lv_obj_get_height(field) < read_height);
	assert(find_image_button(toggle, &icon_check));
	dump_snapshot(noteeditor_screen, tag_editor_edit);

	// The 12-key Ukrainian ЙЦУКЕН rows, T9 keypad and held-key variants fit on
	// the 240x320 panel as well.
	assert(strcmp(kblayout_name(KB_LAYOUT_UKRAINIAN), "Українська") == 0);
	assert(kblayout_row_len(KB_LAYOUT_UKRAINIAN, 0) == 12);
	assert(kblayout_row_len(KB_LAYOUT_UKRAINIAN, 1) == 11);
	assert(kblayout_row_len(KB_LAYOUT_UKRAINIAN, 2) == 9);
	assert(strcmp(kblayout_letter(KB_LAYOUT_UKRAINIAN, 0, 11, false), "ї") == 0);
	assert(strcmp(kblayout_letter(KB_LAYOUT_UKRAINIAN, 1, 10, true), "Є") == 0);
	assert(kblayout_variant_count("г") == 1 && strcmp(kblayout_variant("г", 0), "ґ") == 0);
	assert(kblayout_variant_count("Г") == 1 && strcmp(kblayout_variant("Г", 0), "Ґ") == 0);
	assert(kblayout_variant_count("і") == 1 && strcmp(kblayout_variant("і", 0), "ї") == 0);
	kblayout_move_in_use(KB_LAYOUT_UKRAINIAN, 0);
	keyboard_refresh_layout();
	lv_obj_update_layout(noteeditor_screen);
	check_within_panel(noteeditor_screen, cfg, "Note editor (Ukrainian keyboard)");
	config_set_int("other", "keyboard_t9", 1);
	keyboard_refresh_type();
	lv_obj_update_layout(noteeditor_screen);
	check_within_panel(noteeditor_screen, cfg, "Note editor (Ukrainian T9)");
	config_set_int("other", "keyboard_t9", 0);
	keyboard_refresh_type();
	kblayout_move_in_use(KB_LAYOUT_ENGLISH, 0);
	kblayout_move_others(KB_LAYOUT_UKRAINIAN, 0);
	keyboard_refresh_layout();

	// Typing, then leaving the page: the writing reaches the card even though
	// the check button was never pressed. Nothing here is the only copy.
	lv_textarea_set_text(field, "milk, bread and eggs");
	notes_open();
	text = read_note(dir, "shopping.txt");
	assert(text && strcmp(text, "milk, bread and eggs\n") == 0);
	free(text);
	// And the row still shows the note's name, not its extension.
	assert(find_label(notes_screen, "shopping"));

	// Reopened: the note reads back, and it is not being written.
	lv_obj_t *row_again = find_label(notes_screen, "shopping");
	assert(row_again);
	lv_obj_send_event(lv_obj_get_parent(row_again), LV_EVENT_CLICKED, NULL);
	field = find_class(noteeditor_screen, &lv_textarea_class);
	assert(field);
	assert(strcmp(lv_textarea_get_text(field), "milk, bread and eggs\n") == 0);
	assert(lv_obj_is_hidden(keyboard_tray_of(noteeditor_screen)));
	// Leaving without writing leaves the file as it was: read mode is read
	// only, and this is what makes opening a note safe.
	notes_open();
	text = read_note(dir, "shopping.txt");
	assert(text && strcmp(text, "milk, bread and eggs\n") == 0);
	free(text);

	// A note too long to be typed into is shown, not refused, and it is shown
	// by the text viewer rather than the editor: the editor is the page that
	// would cut it off on the next save.
	lv_obj_t *long_row = find_label(notes_screen, "long-note");
	assert(long_row);
	lv_obj_send_event(lv_obj_get_parent(long_row), LV_EVENT_CLICKED, NULL);
	assert(lv_screen_active() == textview_screen);
	lv_obj_update_layout(textview_screen);
	check_within_panel(textview_screen, cfg, "Note shown by the viewer");
	dump_snapshot(textview_screen, tag_long);
	assert(find_label(textview_screen, "long-note"));
	assert(find_label(textview_screen, tr("notes_too_long")));
	notes_open();
	assert(lv_screen_active() == notes_screen);

	// The largest note that opens in the editor opens in the editor, whole, and
	// quickly. This is the regression this page was fixed for: setting a
	// textarea's text with a limit in force inserts it a character at a time,
	// which cost a walk of the text for every character -- sixteen thousand
	// characters took forty-five seconds and arrived cut off at the field's
	// limit. Reading a note is not a wait.
	lv_obj_t *big_row = find_label(notes_screen, "big-note");
	assert(big_row);
	struct timespec started;
	clock_gettime(CLOCK_MONOTONIC, &started);
	lv_obj_send_event(lv_obj_get_parent(big_row), LV_EVENT_CLICKED, NULL);
	double took = seconds_since(&started);
	assert(lv_screen_active() == noteeditor_screen);
	lv_obj_t *big_field = find_class(noteeditor_screen, &lv_textarea_class);
	assert(big_field);
	// Every character of it, not the first sixteen thousand. The file ends in
	// the newline textfile_write() puts on every note, and the editor shows the
	// file as it is.
	const char *big_shown = lv_textarea_get_text(big_field);
	size_t big_len = strlen(big_note_text());
	assert(strncmp(big_shown, big_note_text(), big_len) == 0);
	assert(big_shown[big_len] == '\n' && big_shown[big_len + 1] == '\0');
	assert(took < 3.0);
	// And it is a note to read: the keyboard is down, and nothing is written
	// until the pencil is asked for.
	assert(lv_obj_is_hidden(keyboard_tray_of(noteeditor_screen)));
	notes_open();

	// A new note: the naming dialog, the accept key, and the note it leaves
	// behind -- written before anything is typed into it, so it is in the list
	// the moment it exists.
	notes_open();
	lv_obj_t *new_row = lv_obj_get_parent(find_label(notes_screen, tr("notes_new")));
	while (new_row && !lv_obj_check_type(new_row, &lv_button_class)) {
		new_row = lv_obj_get_parent(new_row);
	}
	assert(new_row);
	lv_obj_send_event(new_row, LV_EVENT_CLICKED, NULL);

	// The dialog is up, with the name it suggests already in the field.
	lv_obj_t *name_field = find_class(notes_screen, &lv_textarea_class);
	assert(name_field);
	assert(!lv_obj_is_hidden(name_field));
	const char *suggested = lv_textarea_get_text(name_field);
	assert(suggested && suggested[0]);
	assert(notes_name_ok(suggested));

	// A name of the test's own, so the file it makes is known here.
	lv_textarea_set_text(name_field, "test-note");
	press_key(notes_screen, tr("create"));

	// The editor is open on the note that was just made, and the file is on
	// the card already -- empty, and listed.
	assert(lv_screen_active() == noteeditor_screen);
	lv_obj_t *field2 = find_class(noteeditor_screen, &lv_textarea_class);
	assert(field2 && strcmp(lv_textarea_get_text(field2), "") == 0);
	assert(find_label(noteeditor_screen, "test-note"));
	text = read_note(dir, "test-note.txt");
	assert(text && text[0] == '\0');
	free(text);

	// Typing something and pressing the corner check writes it. The check is
	// drawn rather than written, so it is found by its glyph.
	lv_textarea_set_text(field2, "bought at nine");
	lv_obj_t *save = find_image_button(noteeditor_screen, &icon_check);
	assert(save);
	lv_obj_send_event(save, LV_EVENT_CLICKED, NULL);
	text = read_note(dir, "test-note.txt");
	assert(text && strcmp(text, "bought at nine\n") == 0);
	free(text);
	assert(lv_screen_active() == noteeditor_screen);

	// Back to the list: the note made a moment ago is on it.
	notes_open();
	lv_obj_update_layout(notes_screen);
	check_within_panel(notes_screen, cfg, "Notes (with the new note)");
	dump_snapshot(notes_screen, tag_after);
	assert(find_label(notes_screen, "test-note"));
	assert(find_label(notes_screen, tr("notes_new")));
}

/* The button carrying a glyph: a corner control has an image on it, not a
 * caption, so it cannot be found by the text it shows. */
static lv_obj_t *find_image_button(lv_obj_t *obj, const lv_image_dsc_t *glyph) {
	if (lv_obj_check_type(obj, &lv_image_class) && lv_image_get_src(obj) == glyph) {
		lv_obj_t *parent = lv_obj_get_parent(obj);
		if (parent && lv_obj_check_type(parent, &lv_button_class)) {
			return parent;
		}
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
		lv_obj_t *found = find_image_button(lv_obj_get_child(obj, i), glyph);
		if (found) {
			return found;
		}
	}
	return NULL;
}

int main(int argc, char **argv) {
	assert(argc == 2);

	/* On the clock's terms: the suite's fixed second is read in UTC, so the
	 * hour and the month a snapshot shows do not depend on where it runs. */
	setenv("TZ", "UTC", 1);
	tzset();

	/* The card, as a folder the test owns and throws away. */
	char card[] = "/tmp/sonix-tools-test-XXXXXX";
	assert(mkdtemp(card));
	char config[512];
	snprintf(config, sizeof(config), "%s/device_config.ini", card);
	config_init(config);
	config_set("ui", "language", "English");
	lang_init(argv[1]);
	check_strings(argv[1]);

	init_fonts();
	lv_init();
	gui_config_t cfg = {bp_screen_w(), bp_screen_h(), bp_padding(), bp_status_bar_h(), card};
	lv_display_create(cfg.screen_width, cfg.screen_height);
	theme_init();

	music_screen = lv_obj_create(NULL);
	streaming_screen = lv_obj_create(NULL);
	wireless_screen = lv_obj_create(NULL);
	audiobooks_screen = lv_obj_create(NULL);
	settings_screen = lv_obj_create(NULL);
	player_screen = lv_obj_create(NULL);
	ebookpage_screen = lv_obj_create(NULL);
	browser_screen = lv_obj_create(NULL);
	dacpage_screen = lv_obj_create(NULL);
	gearboypage_screen = lv_obj_create(NULL);
	textview_screen = lv_obj_create(NULL);
	pictureview_screen = lv_obj_create(NULL);
	morepage_screen = lv_obj_create(NULL);
	calculator_screen = lv_obj_create(NULL);
	notes_screen = lv_obj_create(NULL);
	metronome_screen = lv_obj_create(NULL);
	petpage_screen = lv_obj_create(NULL);
	noteeditor_screen = lv_obj_create(NULL);

	/* The notes the card already has, put there the way a computer would
	 * leave them: one of each extension the page knows, and a picture. */
	char dir[1024];
	assert(notes_dir(dir, sizeof(dir), card));
	assert(mkdir(dir, 0755) == 0);
	assert(notes_write(dir, "shopping", "milk and bread"));
	assert(notes_write(dir, "recipe.md", "# soup"));
	// A note past the editor's character limit, and inside the store's byte
	// limit: one byte a character, so it is the limit plus a hundred.
	assert(notes_write(dir, "long-note", long_note_text()));
	// And one at the editor's own ceiling, which is opened in the editor.
	assert(notes_write(dir, "big-note", big_note_text()));
	char path[1400];
	snprintf(path, sizeof(path), "%s/cover.jpg", dir);
	FILE *other = fopen(path, "wb");
	assert(other);
	assert(fputs("not a note", other) >= 0);
	assert(fclose(other) == 0);
	char png_path[1400];
	snprintf(png_path, sizeof(png_path), "%s/photo.png", dir);
	FILE *png_file = fopen(png_path, "wb");
	assert(png_file);
	assert(fputs("png image", png_file) >= 0);
	assert(fclose(png_file) == 0);

	/* Built in the order gui_init() builds them: the editor before the shelf
	 * that opens it, both before the menu that points at them. */
	calculator_init(&cfg);
	noteeditor_init(&cfg);
	notes_init(&cfg);
	tagmanager_init(&cfg);
	metronome_init(&cfg);
	pet_defaults(&pet_state, petwatch_now(), pet_day_now(petwatch_now()));
	petpage_init(&cfg);
	morepage_init(&cfg);
	player_drag_attaches = 0; // counting from here: only the viewers are built below
	textview_init(&cfg); // the notes shelf shows a long note through it
	pictureview_init(&cfg);
	/* Neither viewer takes the player-sheet swipe. A leftward drag across a text
	 * file or a picture is not a way to the player: it was pulling the sheet
	 * over what is being read or looked at, and the text viewer never did. The
	 * back swipe is still the way out of both, attached separately. */
	assert(player_drag_attaches == 0);

	assert(pictureview_open(path));
	assert(lv_screen_active() == pictureview_screen);
	lv_obj_update_layout(pictureview_screen);
	check_within_panel(pictureview_screen, &cfg, "Picture viewer (JPEG)");
	assert(find_label(pictureview_screen, "cover.jpg"));

	assert(pictureview_open(png_path));
	assert(lv_screen_active() == pictureview_screen);
	lv_obj_update_layout(pictureview_screen);
	check_within_panel(pictureview_screen, &cfg, "Picture viewer (PNG)");
	assert(find_label(pictureview_screen, "photo.png"));

	/* Both shapes of the More page, each under the themes that use it. Dark
	 * and Light draw the tiles on every board; the V1's function-glyph themes
	 * draw the single-column list. */
	for (int kind = 0; kind < THEME_KIND_COUNT; kind++) {
		theme_set_kind((theme_kind_t)kind);
		const char *shape = (bp_is_tempotec_v1() && theme_uses_function_glyphs()) ? "rows" : "tiles";
		char tag[64];
		snprintf(tag, sizeof(tag), "%s-more-%s-%s", bp_is_tempotec_v1() ? "v1" : "hiby", shape, theme_tag());
		check_more_page(&cfg, tag);
	}
	theme_set_kind(THEME_KIND_DARK);
	check_more_audiobooks_option(&cfg);

    // Music is the only browser root; unrelated card folders are not shown.
    char musicdir[1200]; snprintf(musicdir,sizeof(musicdir),"%s/Music",card); assert(!mkdir(musicdir,0755));
    char subdir[1400], linkpath[1400]; snprintf(subdir,sizeof(subdir),"%s/Nested",musicdir); assert(!mkdir(subdir,0755));
    snprintf(linkpath,sizeof(linkpath),"%s/Escape",musicdir); assert(!symlink(card,linkpath));
    char tagpath[1400]; snprintf(tagpath,sizeof(tagpath),"%s/test.flac",musicdir);
    FILE *tagfile=fopen(tagpath,"wb"); assert(tagfile);
    const unsigned char flac_header[42]={'f','L','a','C',0x80,0,0,34};
    assert(fwrite(flac_header,1,sizeof(flac_header),tagfile)==sizeof(flac_header));
    assert(fputs("audio",tagfile)>=0); assert(!fclose(tagfile));
    tagmanager_open(); lv_obj_t *tagpage=lv_screen_active(); lv_obj_update_layout(tagpage);
    check_within_panel(tagpage,&cfg,"Tag Manager browser");
    assert(!find_label(tagpage,"Notes")); assert(!find_label(tagpage,"Folder")); assert(!find_label(tagpage,"Escape"));
    assert(last_back_guard && !last_back_guard()); // cannot leave Music for card root
    lv_obj_t *nested=find_label(tagpage,"Nested"); assert(nested);
    lv_obj_send_event(lv_obj_get_parent(nested),LV_EVENT_CLICKED,NULL);
    assert(find_label(tagpage,"No folders or FLAC / MP3 files")); assert(last_back_guard());
    lv_obj_t *filelabel=find_label(tagpage,"test.flac"); assert(filelabel);
    lv_obj_send_event(lv_obj_get_parent(filelabel),LV_EVENT_CLICKED,NULL);
    lv_obj_update_layout(tagpage); assert(find_label(tagpage,"Title: Test title"));
    lv_obj_t *track=find_label(tagpage,"Edit track number"); assert(track);
    lv_obj_send_event(lv_obj_get_parent(track),LV_EVENT_CLICKED,NULL);
    lv_obj_update_layout(tagpage); assert(find_label(tagpage,"OK"));
    check_within_panel(tagpage,&cfg,"Tag Manager editor");
    lv_obj_t *input=find_class(tagpage,&lv_textarea_class); assert(input);
    lv_textarea_set_text(input,"3/12");
    lv_obj_send_event(lv_obj_get_parent(find_label(tagpage,"OK")),LV_EVENT_CLICKED,NULL);
    tagfile=fopen(tagpath,"rb"); assert(tagfile); char bytes[512];
    size_t len=fread(bytes,1,sizeof(bytes),tagfile); fclose(tagfile);
    bool found=false; for(size_t i=0;i+16<=len;i++) if(!memcmp(bytes+i,"TRACKNUMBER=3/12",16)) found=true;
    assert(found);
    // The artist and title rows really open their respective editors.
    lv_obj_t *title_row=find_label(tagpage,"Edit title"); assert(title_row);
    lv_obj_send_event(lv_obj_get_parent(title_row),LV_EVENT_CLICKED,NULL);
    lv_textarea_set_text(input,"New title"); lv_obj_send_event(lv_obj_get_parent(find_label(tagpage,"OK")),LV_EVENT_CLICKED,NULL);
    lv_obj_t *artist_row=find_label(tagpage,"Edit artist"); assert(artist_row);
    lv_obj_send_event(lv_obj_get_parent(artist_row),LV_EVENT_CLICKED,NULL);
    lv_textarea_set_text(input,"New artist");
    char sidecar[1450]; snprintf(sidecar,sizeof(sidecar),"%s.tags",tagpath);
    FILE *sc=fopen(sidecar,"wb"); assert(sc); assert(fputs("artist=Cached artist\n",sc)>=0); assert(!fclose(sc));
    expected_error="Track has a tag override file";
    lv_obj_send_event(lv_obj_get_parent(find_label(tagpage,"OK")),LV_EVENT_CLICKED,NULL); assert(!expected_error);
    assert(!unlink(sidecar));
    lv_obj_send_event(lv_obj_get_parent(find_label(tagpage,"OK")),LV_EVENT_CLICKED,NULL);
    tagfile=fopen(tagpath,"rb"); assert(tagfile); len=fread(bytes,1,sizeof(bytes),tagfile); fclose(tagfile);
    bool title_found=false, artist_found=false;
    for(size_t i=0;i<len;i++) { if(i+15<=len && !memcmp(bytes+i,"TITLE=New title",15)) title_found=true; if(i+17<=len && !memcmp(bytes+i,"ARTIST=New artist",17)) artist_found=true; }
    assert(title_found && artist_found);
    // Mocked network results: search, preview, and explicitly apply.
    lv_obj_t *search=find_label(tagpage,"Find cover art"); assert(search);
    lv_obj_send_event(lv_obj_get_parent(search),LV_EVENT_CLICKED,NULL);
    lv_textarea_set_text(input,"Test artist Test album");
    lv_obj_send_event(lv_obj_get_parent(find_label(tagpage,"OK")),LV_EVENT_CLICKED,NULL);
    for(int i=0;i<200 && !find_label(tagpage,"Test artist — Test album");i++) { usleep(1000); lv_tick_inc(100); lv_timer_handler(); }
    lv_obj_t *result=find_label(tagpage,"Test artist — Test album"); assert(result);
    lv_obj_send_event(lv_obj_get_parent(result),LV_EVENT_CLICKED,NULL);
    for(int i=0;i<200 && !find_label(tagpage,"Use this cover");i++) { usleep(1000); lv_tick_inc(100); lv_timer_handler(); }
    lv_obj_t *apply=find_label(tagpage,"Use this cover"); assert(apply); lv_obj_update_layout(tagpage);
    check_within_panel(tagpage,&cfg,"Tag Manager cover preview");
    lv_obj_send_event(lv_obj_get_parent(apply),LV_EVENT_CLICKED,NULL);
    for(int i=0;i<200 && !find_label(tagpage,"Find cover art");i++) { usleep(1000); lv_tick_inc(100); lv_timer_handler(); }
    assert(find_label(tagpage,"Find cover art"));
    tagfile=fopen(tagpath,"rb"); assert(tagfile); len=fread(bytes,1,sizeof(bytes),tagfile); fclose(tagfile);
    found=false; for(size_t i=0;i+10<=len;i++) if(!memcmp(bytes+i,"image/jpeg",10)) found=true;
    assert(found); unlink(tagpath); unlink(linkpath); rmdir(subdir); rmdir(musicdir);

	check_calculator_page(&cfg, bp_is_tempotec_v1() ? "v1-calculator" : "hiby-calculator");
	check_metronome_page(&cfg, bp_is_tempotec_v1() ? "v1-metronome" : "hiby-metronome");
	check_pet_page(&cfg, bp_is_tempotec_v1() ? "v1-pet" : "hiby-pet");
	check_notes_page(&cfg, dir, bp_is_tempotec_v1() ? "v1-notes" : "hiby-notes",
					 bp_is_tempotec_v1() ? "v1-note-editor" : "hiby-note-editor",
					 bp_is_tempotec_v1() ? "v1-note-editor-writing" : "hiby-note-editor-writing",
					 bp_is_tempotec_v1() ? "v1-notes-after" : "hiby-notes-after",
					 bp_is_tempotec_v1() ? "v1-note-long" : "hiby-note-long");

	lv_deinit();
	printf("PASS calculator, notes, metronome and pet pages (%s)\n", bp_is_tempotec_v1() ? "240x320" : "480x720");
	return 0;
}
