// The artwork as a way into the queue (issue #379): a press held still on the
// sleeve opens it, and the lift that ends the press is not delivered as a tap
// on the page that has just come up.
//
// The real player.c is compiled in, and the press comes through LVGL's own
// pointer and timer dispatch rather than from a hand-sent event: what is being
// checked is the gesture as the device sees it -- the timer running out with a
// finger held still, the slop that drops a finger that wanders, and the release
// that must not arrive twice.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/gui/nowplaying/player.c"

// ---------------------------------------------------------------------------
// What player_init() would build, without the shell it hangs off
// ---------------------------------------------------------------------------

lv_font_t font_ui_14, font_ui_18, font_ui_24;
lv_style_t theme_style_icon, theme_style_card, theme_style_card_pressed, theme_style_text;
static const theme_palette_t palette;

const theme_palette_t *theme(void) { return &palette; }
bool theme_is_monochrome(void) { return false; }
bool theme_is_tokyo_night(void) { return false; }
lv_color_t theme_player_chrome_color(theme_player_chrome_t role) {
	(void)role;
	return palette.accent;
}
int back_btn_centre_y(void) { return 42; }

static int opened;

// Two entries in the queue, which is what a page with something behind the
// playing track has. The list itself is never read by the gesture.
static void two_tracks(void) {
	static const char *const list[] = {"/music/one.flac", "/music/two.flac"};
	playlist_load_paths(list, 2, 0);
}

// The page the hold opens. The real one switches screens; here that is done
// too, by a page of this test's own, so that the lift can be watched arriving
// over the queue's rows rather than over the sleeve.
static lv_obj_t *queue_page;
static bool queue_page_on_open;

void trackmenu_open_queue(void) {
	opened++;
	player_sheet_close(false); // as the real one does, wherever it is called from
	if (queue_page_on_open) {
		lv_screen_load(queue_page);
	}
}

// Back on the player, as picking a track from the queue leaves it: the sheet is
// up again, and the sleeve is where the next gesture finds it.
static void back_to_player(void) {
	// As player_sheet_open() leaves the page: shown again, with its own layout,
	// since the close hid it and a hidden object takes no presses.
	sheet_open = true;
	lv_screen_load(player_screen);
	lv_obj_set_hidden(player_screen, false);
	lv_obj_update_layout(player_screen);
}

// Three things the player translation unit reaches that the queue has no
// business touching. The gesture reads the queue's length, never its contents.
static bool casting;
bool dlna_owns_playback(void) { return casting; }

// The rest of the panel the artwork is dragged by: the drag handler is the real
// one, because it shares this press.
const lv_font_t *fonts_lyrics(int size) { (void)size; return &font_ui_14; }
bool theme_is_oled(void) { return false; }
bool btreceiver_is_active(void) { return false; }
void topbar_set_hidden(bool hidden) { (void)hidden; }
void topbar_set_player_color(bool on, lv_color_t c) { (void)on; (void)c; }
void topbar_set_player_frost(const lv_image_dsc_t *image) { (void)image; }
void topbar_set_player_ground(bool on, lv_color_t c) { (void)on; (void)c; }
void topbar_set_player_palette(bool on, lv_color_t accent) { (void)on; (void)accent; }
void back_btn_sync_visibility(void) {}
void back_btn_translate(int32_t x) { (void)x; }
void back_btn_player_mode(bool on) { (void)on; }
void back_btn_player_top_inset(int v) { (void)v; }
void back_btn_player_ink_inset(int v) { (void)v; }
void medialist_refresh_visible(void) {}
void switch_screen_no_history(lv_obj_t *screen) { lv_screen_load(screen); }
void library_index_close(library_index_t *ix) { (void)ix; }
void library_queue_save_album(const char *album) { (void)album; }

// ---------------------------------------------------------------------------
// The press
// ---------------------------------------------------------------------------

static lv_point_t point;
static lv_indev_state_t state;

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	data->point = point;
	data->state = state;
}

static void read_at(lv_indev_t *indev, int x, int y, lv_indev_state_t s) {
	point.x = x;
	point.y = y;
	state = s;
	lv_indev_read(indev);
}

// A whole timer pass: what the main loop does, and what sends the long press
// and runs out the hold's own timer.
static void tick(lv_indev_t *indev, int ms) {
	lv_tick_inc((uint32_t)ms);
	lv_timer_handler();
	lv_indev_read(indev);
}

// A press held still until the hold comes due, then lifted. The lift goes to
// whatever is under the finger at that moment, which after the page has
// changed is not the sleeve: what it does there is the point of the queue-page
// step in main().
static void hold(lv_indev_t *indev, int x, int y) {
	int before = opened;
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED);
	tick(indev, HOLD_SLEEVE_MS / 2);
	tick(indev, HOLD_SLEEVE_MS / 2 + 20);
	assert(sleeve_hold_timer == NULL);
	read_at(indev, x, y, LV_INDEV_STATE_RELEASED);
	if (opened != before) {
		// A hold that opened the queue left the player down with it. The press
		// was still on the sleeve when the page changed, and the same press is
		// watched by the drag that puts the player away: the lift must not read
		// to it as a swipe that puts the player back over the queue.
		assert(!sheet_open);
	}
}

static void press_and_lift(lv_indev_t *indev, int x, int y) {
	read_at(indev, x, y, LV_INDEV_STATE_PRESSED);
	tick(indev, 40);
	read_at(indev, x, y, LV_INDEV_STATE_RELEASED);
}

static int centre_x, centre_y;

static void hold_centre(lv_indev_t *indev) { hold(indev, centre_x, centre_y); }

// The queue's own row, on the page the hold brings up: a tap on it starts that
// track, which is exactly what the lift must not do.
static lv_obj_t *queue_row;
static int rows_tapped;

static void queue_row_cb(lv_event_t *e) {
	(void)e;
	rows_tapped++;
}

int main(int argc, char **argv) {
	assert(argc == 2);

	char dir[] = "/tmp/sonix-sleeve-hold-XXXXXX";
	assert(mkdtemp(dir));
	char path[160];
	snprintf(path, sizeof(path), "%s/config.ini", dir);
	(void)config_init(path);

	lv_init();
	lv_display_create(240, 320);
	lang_init(argv[1]);
	assert(lang_set("English"));
	font_ui_14 = lv_font_montserrat_14;
	font_ui_18 = lv_font_montserrat_14;
	font_ui_24 = lv_font_montserrat_20;
	lv_style_init(&theme_style_icon);
	lv_style_init(&theme_style_card);
	lv_style_init(&theme_style_card_pressed);
	lv_style_init(&theme_style_text);

	// The page as far as this gesture knows it: the sheet is up, and the panel
	// is the top of the screen with the sleeve centred in it.
	player_screen = lv_screen_active();
	lv_obj_remove_style_all(player_screen);
	lv_obj_set_size(player_screen, 240, 320);
	sheet_open = true;

	cover_panel = lv_obj_create(player_screen);
	lv_obj_remove_style_all(cover_panel);
	lv_obj_set_size(cover_panel, 240, 240);
	lv_obj_set_pos(cover_panel, 0, 0);
	// The same attachment player_init() makes on the panel it builds.
	sleeve_hold_attach(cover_panel);
	player_sheet_attach_drag(cover_panel, false);

	// The panel is the page's own body and takes presses; the sleeve frame over
	// it is deliberately not clickable (see player_init), so presses on the
	// picture belong to the panel underneath.
	cover_art_group = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(cover_art_group);
	lv_obj_set_size(cover_art_group, 180, 180);
	lv_obj_center(cover_art_group);
	// The frame takes no presses of its own: they belong to the panel the sheet
	// is dragged by, which is what the gestures on the artwork listen on.
	lv_obj_set_clickable(cover_art_group, false);
	centre_x = 120;
	centre_y = 120;

	// As player_sheet_attach_drag() leaves the panel, and as the page's own size
	// makes every object reachable: a non-scrollable object takes presses.
	lv_obj_set_clickable(cover_panel, true);
	// There is no draw buffer here, so nothing else would settle the layout the
	// hit test reads the sleeve's rectangle out of.
	lv_obj_update_layout(player_screen);

	lv_indev_t *indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, read_pointer);

	// The device's first report says the finger is not down, so that every
	// press below is one LVGL sees arriving rather than one already down.
	read_at(indev, 0, 0, LV_INDEV_STATE_RELEASED);

	// A queue of one is nothing to show, and the ellipsis does not show it
	// either: the hold does nothing.
	assert(!sleeve_hold_queue_there());
	hold_centre(indev);
	assert(opened == 0);

	// Two tracks: the hold opens it, once.
	two_tracks();
	assert(sleeve_hold_queue_there());
	hold_centre(indev);
	assert(opened == 1);

	// The queue took the player down with it: a second press is on the queue and
	// not on the sleeve, which is what the lift of the first hold must leave.
	// Picking a track there brings the player back, and this is that.
	back_to_player();

	// A tap on the picture is still a tap, and not another hold.
	press_and_lift(indev, centre_x, centre_y);
	assert(opened == 1);

	// A press that wanders is a drag and never a hold. Downwards here, which is
	// the one direction nothing else on this page wants -- a sideways drag is
	// the sheet's or the words', and either may well close the player.
	read_at(indev, centre_x, centre_y, LV_INDEV_STATE_PRESSED);
	read_at(indev, centre_x, centre_y + HOLD_SLOP_PX + 8, LV_INDEV_STATE_PRESSED);
	tick(indev, HOLD_SLEEVE_MS + 40);
	assert(sleeve_hold_timer == NULL);
	read_at(indev, centre_x, centre_y + HOLD_SLOP_PX + 8, LV_INDEV_STATE_RELEASED);
	assert(opened == 1);

	// The same sideways, where the sheet's own drag takes the press: the hold
	// drops out of its way rather than opening the queue on the way past.
	read_at(indev, centre_x, centre_y, LV_INDEV_STATE_PRESSED);
	read_at(indev, centre_x + HOLD_SLOP_PX + 8, centre_y, LV_INDEV_STATE_PRESSED);
	tick(indev, HOLD_SLEEVE_MS + 40);
	assert(sleeve_hold_timer == NULL);
	read_at(indev, centre_x + HOLD_SLOP_PX + 8, centre_y, LV_INDEV_STATE_RELEASED);
	assert(opened == 1);
	back_to_player();

	// Off the picture: the panel is wider than the sleeve on the V1, and a
	// press beside it belongs to whatever else is there.
	lv_area_t sleeve;
	lv_obj_get_coords(cover_art_group, &sleeve);
	int side_x = sleeve.x1 - 10;
	if (side_x >= 0) {
		hold(indev, side_x, centre_y);
		assert(opened == 1);
	}

	// A book has chapters instead of a queue, and a phone casting to the
	// player keeps the queue on the phone: neither opens it.
	back_to_player();
	audiobook_mode = true;
	assert(!sleeve_hold_queue_there());
	hold_centre(indev);
	assert(opened == 1);
	audiobook_mode = false;

	casting = true;
	assert(!sleeve_hold_queue_there());
	hold_centre(indev);
	assert(opened == 1);
	casting = false;

	back_to_player();
	// The words are over the page: the press belongs to them.
	lyrics_progress = 1;
	hold_centre(indev);
	assert(opened == 1);
	lyrics_progress = 0;

	back_to_player();
	// Album Art Max has the double tap on the same picture, and its strip
	// carries the queue count already: the hold keeps out of its way.
	layout_aam_now = true;
	hold_centre(indev);
	assert(opened == 1);
	layout_aam_now = false;

	back_to_player();
	// Studio draws a square of its own, and the frame's rectangle is not where
	// the picture is: a hold there is a hold on nothing.
	layout_studio_now = true;
	studio_empty = lv_obj_create(player_screen);
	lv_obj_remove_style_all(studio_empty);
	lv_obj_set_pos(studio_empty, 20, 60);
	lv_obj_set_size(studio_empty, 120, 120);
	// A shape, not a target: presses on it belong to the panel, as in Studio.
	lv_obj_set_clickable(studio_empty, false);
	lv_obj_update_layout(player_screen);
	hold(indev, 120, 280); // the frame's rectangle, where the words are
	assert(opened == 1);
	hold(indev, 80, 120); // Studio's own square
	assert(opened == 2);

	back_to_player();
	lv_obj_delete(studio_empty);
	studio_empty = NULL;
	layout_studio_now = false;
	lv_obj_set_size(cover_art_group, 180, 180);
	lv_obj_center(cover_art_group);
	lv_obj_update_layout(player_screen);

	// The lift belongs to the page the finger went down on. The queue comes up
	// under that finger, with one of its rows right where it is, and a tap on
	// that row would start a track: the single tap the hold replaced.
	queue_page = lv_obj_create(NULL);
	queue_row = lv_obj_create(queue_page);
	lv_obj_remove_style_all(queue_row);
	lv_obj_set_pos(queue_row, centre_x - 40, centre_y - 40);
	lv_obj_set_size(queue_row, 80, 80);
	lv_obj_add_event_cb(queue_row, queue_row_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_clickable(queue_row, true);
	lv_obj_update_layout(queue_page);

	back_to_player();
	queue_page_on_open = true;
	hold_centre(indev);
	queue_page_on_open = false;
	assert(opened == 3);
	assert(rows_tapped == 0);
	// And the next gesture on that page is a gesture of its own.
	press_and_lift(indev, centre_x, centre_y);
	assert(rows_tapped == 1);
	queue_row = NULL;

	back_to_player();
	// Poster's picture reaches past the foot of the panel, where the scrub bar
	// is: only what is seen of it above the deck holds.
	layout_poster_now = true;
	lv_obj_set_size(cover_art_group, 240, 240);
	lv_obj_set_pos(cover_art_group, 0, 0);
	lv_obj_set_size(cover_panel, 240, 160);
	lv_obj_update_layout(player_screen);
	hold(indev, 120, 200); // under the deck's edge: the scrub bar's
	assert(opened == 3);
	hold(indev, 120, 40);
	assert(opened == 4);
	layout_poster_now = false;
	lv_obj_set_size(cover_panel, 240, 240);
	lv_obj_set_size(cover_art_group, 180, 180);
	lv_obj_center(cover_art_group);
	lv_obj_update_layout(player_screen);

	back_to_player();
	// A press that is never reported as over -- the screen went dark under the
	// finger, the words came up over it -- leaves the hold armed, and the reset
	// the indev sends then is what takes it away: nothing fires onto a page
	// nobody made the gesture on.
	read_at(indev, centre_x, centre_y, LV_INDEV_STATE_PRESSED);
	assert(sleeve_hold_timer != NULL);
	lv_indev_reset(indev, NULL);
	assert(sleeve_hold_timer == NULL);
	tick(indev, HOLD_SLEEVE_MS + 40);
	assert(opened == 4);
	read_at(indev, centre_x, centre_y, LV_INDEV_STATE_RELEASED);

	printf("sleeve hold: ok\n");
	return 0;
}
