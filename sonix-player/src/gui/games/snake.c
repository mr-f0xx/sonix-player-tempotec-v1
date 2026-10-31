// snake.c -- Snake, from the More page.
//
// The rules are the old ones: the snake goes forward, a tap or a swipe sends it
// along the lattice a quarter turn at a time, an apple makes it longer, and the
// run ends when it finds an edge it cannot leave or a part of itself.
//
// What the page adds is the way it is drawn. The snake is not a list of segments
// that are moved about; it is the path the head has walked, and every part of it
// sits one step behind the part in front. Keeping the path costs one write per
// step, and it is what lets the body be drawn anywhere along a line that already
// exists -- so the snake crawls at the rate the panel repaints at while the game
// itself still thinks in whole cells, and the two rates never have to agree.
//
// Nothing is created or destroyed in the middle of a run. The body's objects are
// made the first time the snake reaches that length and are kept for the next
// run, because the alternative is an allocation per apple at the moment the
// player is watching.

#include "snake.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/games/games.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

#define SECTION "snake"

// Numbered clockwise: a right turn is +1, a left turn is -1, and +2 is the one
// thing a player never means.
enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

// The clock the game is drawn at, and the rate the snake actually moves. The
// step is what the rules see; the tick is what the eye sees.
#define TICK_MS 16
#define STEP_START_MS 150
#define STEP_MIN_MS 70
#define STEP_FASTEN_MS 2 // ... per apple, so the limit is reached at 40 of them

#define APPLE_POINTS 10

// The most snake this page draws. A 19x19 board holds 361 cells, so the cap is
// out of reach of anybody who has to steer, and it is what keeps the object count
// bounded on a player that is running a decoder at the same time.
#define SEG_MAX 160

// The path, with room for the whole snake plus a step at either end: a segment
// is drawn between two entries of it, so the ring must never lap the tail it is
// on.
#define TRAIL_LEN (SEG_MAX * 4)

// The board is never narrower than this, whatever panel it is drawn on: below it
// a snake of four has no room to turn twice.
#define BOARD_MIN 9

lv_obj_t *snake_screen;

static game_frame_t frame;

static lv_obj_t *board;
static lv_obj_t *seg[SEG_MAX];
static int segs_made;
static lv_obj_t *food_obj;

typedef struct {
	int16_t x, y;
} lattice_t;

static lattice_t trail[TRAIL_LEN];
static int head_i; // where in the path the head stands
static int len;    // segments, the head among them
static int dir;

// One turn may wait, and only one, because a turn is not a command but an
// intention: a player who taps twice inside one step meant the corner, and a
// snake that took both would be a snake that bit itself.
static int turn[2];
static int turns;

static int cell_px, cols, rows, seg_side, bite;
static int step_ms;
static int64_t into_step_us;
static int food_x, food_y;
static bool food_up; // ... and there is an apple on it to be seen
static int apples;
static bool wrap;
static bool running;

static void draw(void);
static void fit_body(void);

// ---------------------------------------------------------------------------
// colours
// ---------------------------------------------------------------------------

// One step off the page: raised in a dark palette and pressed into a light one,
// as every surface in the app that holds something else is.
static lv_color_t board_colour(void) {
	return theme_is_dark() ? lv_color_lighten(theme()->screen_bg, LV_OPA_10)
			       : lv_color_darken(theme()->screen_bg, LV_OPA_10);
}

// The body is ink, which is the darkest thing on a light panel and the brightest
// on a dark one. The head takes the accent the player chose: it is the one colour
// on the page that is not the game's own, which is what makes the direction of a
// long curled-up snake legible without thinking about.
static lv_color_t ink_colour(void) {
	return game_frame_ink(theme_is_dark());
}

// ---------------------------------------------------------------------------
// the lattice
// ---------------------------------------------------------------------------

// The path is indexed from the head, backwards through the beginning of the
// array and out the other side: every part of the snake is a fixed distance
// behind the part in front of it, and the head's own history is the body's.
static int trail_i(int i) {
	int j = i % TRAIL_LEN;
	return j < 0 ? j + TRAIL_LEN : j;
}

static void paint_food(void) {
	lv_obj_set_pos(food_obj, food_x * cell_px + (cell_px - bite) / 2, food_y * cell_px + (cell_px - bite) / 2);
}

// The cell is a fixed number of pixels rather than a number of cells squeezed
// into whatever the field happens to be, so that a turn is the same distance on
// either panel and everything lands on whole panel pixels. On the V1's 228x235
// field that is nineteen cells of twelve; on the 480-pixel players nineteen of
// twenty-four, which is the same game with room to breathe.
static void layout(void) {
	int field_w = frame.field_w;
	int field_h = frame.field_h;

	cell_px = bp_pick(12, 24);
	while (cell_px > 4 && (field_w / cell_px < BOARD_MIN || field_h / cell_px < BOARD_MIN)) {
		cell_px--;
	}
	cols = field_w / cell_px;
	rows = field_h / cell_px;

	// One step of the lattice in, so that a body of a hundred still reads as a
	// line of parts rather than a bar.
	seg_side = cell_px - frame.u;
	bite = cell_px * 3 / 5;

	lv_obj_set_pos(board, (field_w - cols * cell_px) / 2, (field_h - rows * cell_px) / 2);
	lv_obj_set_size(board, cols * cell_px, rows * cell_px);
	lv_obj_set_style_radius(board, frame.u * 2, 0);
	lv_obj_set_style_bg_color(board, board_colour(), 0);
	lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0);

	// The wall, when it kills, is the board's own edge. Switch the mode off and
	// the line goes with it: the board says which game is being played, before
	// the player has read a word of the card.
	lv_obj_set_style_border_width(board, wrap ? 0 : 1, 0);
	lv_obj_set_style_border_color(board, ink_colour(), 0);
	lv_obj_set_style_border_opa(board, LV_OPA_40, 0);

	for (int i = 0; i < segs_made; i++) {
		lv_obj_set_size(seg[i], seg_side, seg_side);
	}
	lv_obj_set_size(food_obj, bite, bite);
	lv_obj_set_style_radius(food_obj, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(food_obj, ink_colour(), 0);
	lv_obj_set_style_bg_opa(food_obj, LV_OPA_COVER, 0);

	if (food_up) {
		paint_food(); // an apple on the board moves with the board
	}
}

// Brightest at the head, faded at the tail: the tail is the part a player has to
// avoid and the part they can least afford to spend attention on.
static void taper(void) {
	int span = len > 1 ? len - 1 : 1;
	for (int i = 0; i < len && i < segs_made; i++) {
		lv_obj_set_style_bg_opa(seg[i], (lv_opa_t)(LV_OPA_COVER - (LV_OPA_COVER - 110) * i / span), 0);
	}
}

// Enough objects for the body as it now stands, and the colours of it. Called
// when a run starts and when the snake grows, which is the only twice the length
// changes.
static void fit_body(void) {
	while (segs_made < len && segs_made < SEG_MAX) {
		lv_obj_t *obj = lv_obj_create(board);
		lv_obj_remove_style_all(obj);
		lv_obj_set_clickable(obj, false); // the field takes every press, so that a
		// swipe can start on top of a segment
		lv_obj_set_scrollable(obj, false);
		lv_obj_set_size(obj, seg_side, seg_side);
		lv_obj_set_style_radius(obj, frame.u, 0);
		lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
		seg[segs_made++] = obj;
	}
	for (int i = 0; i < segs_made; i++) {
		lv_obj_set_hidden(seg[i], i >= len);
		lv_obj_set_style_bg_color(seg[i], i ? ink_colour() : theme()->accent, 0);
	}
	taper();
}

// ---------------------------------------------------------------------------
// the cards
// ---------------------------------------------------------------------------

// The card the game is always opened on. A run that is interrupted is a run that
// is over -- there is no state worth coming back to, unlike 2048 -- and the mode
// switch belongs before the first step rather than in the middle of a corner.
static void show_ready(void) {
	running = false;
	game_frame_set_period(&frame, 0);
	game_frame_set_hint(&frame, tr("snake_hint"));
	game_frame_set_header_button_icon(&frame, 0, &icon_pause);
	// The quiet button is the other mode rather than the one already on: the
	// wall is what a player either wants or does not, and a label that names the
	// thing you get by pressing it is the one that does not have to be learned.
	game_frame_show_overlay(&frame, tr("snake"), tr("snake_ready"), tr("game_start"),
			       wrap ? tr("snake_walls") : tr("snake_wrap"));
}

static void show_dead(const char *how) {
	running = false;
	game_frame_set_period(&frame, 0);
	// The picture is left with every part on the cell it is on: a run that ends
	// half a step short of where it stopped looks like the snake hesitated
	// before the wall rather than met it.
	into_step_us = (int64_t)step_ms * 1000;
	draw();
	game_frame_show_overlay(&frame, tr("game_over"), how, tr("game_play_again"), NULL);
}

// ---------------------------------------------------------------------------
// the run
// ---------------------------------------------------------------------------

// A free cell, found by throwing at the board and walking on from wherever the
// throw landed. Walking rather than re-throwing is what keeps this ending in a
// bounded time once the snake has eaten most of what is left.
static void place_food(void) {
	int first = rand() % (cols * rows);
	for (int n = 0; n < cols * rows; n++) {
		int at = (first + n) % (cols * rows);
		int x = at % cols;
		int y = at / cols;
		bool busy = false;
		for (int i = 0; i < len; i++) {
			lattice_t c = trail[trail_i(head_i - i)];
			if (c.x == x && c.y == y) {
				busy = true;
				break;
			}
		}
		if (busy) {
			continue;
		}
		food_x = x;
		food_y = y;
		food_up = true;
		paint_food();
		lv_obj_set_hidden(food_obj, false);
		return;
	}
	// No room left, which the cap on the snake's length makes unreachable on
	// any of these boards. The apple stays where it is and the run carries on.
}

static void reset_run(void) {
	int start_x = cols / 2 + 2;
	int start_y = rows / 2;

	len = 4;
	if (start_x < len + 1) {
		start_x = len + 1;
	}
	dir = DIR_RIGHT;
	turns = 0;
	apples = 0;
	step_ms = STEP_START_MS;
	into_step_us = 0;
	head_i = 0;

	// The path is laid out before the run starts, head first, so that the
	// entries behind the body are the cells the body came from rather than
	// whatever an earlier run left there. Without the one in front of the tail
	// the last segment would be drawn sliding backwards out of nowhere.
	for (int i = 0; i < TRAIL_LEN; i++) {
		trail[i].x = (int16_t)start_x;
		trail[i].y = (int16_t)start_y;
	}
	for (int i = 0; i <= len; i++) {
		trail[trail_i(-i)].x = (int16_t)(start_x - i);
	}

	food_up = false;
	lv_obj_set_hidden(food_obj, true);

	fit_body();
	game_frame_set_score(&frame, 0);
	game_frame_set_hint(&frame, tr("snake_hint"));
	place_food();
	draw();
}

// The turn the snake takes now, out of the ones the player has asked for.
static int take_turn(void) {
	while (turns > 0) {
		int want = turn[0];
		turns--;
		turn[0] = turn[1];
		if (want != dir && want != (dir + 2) % 4) {
			return want;
		}
	}
	return dir;
}

static void offer_turn(int want) {
	if (turns < (int)(sizeof(turn) / sizeof(turn[0]))) {
		turn[turns++] = want;
	}
}

// One cell on. False ends the run, and says so out loud rather than leaving the
// player to work it out from where the snake stopped.
static bool advance(void) {
	dir = take_turn();

	static const int16_t DX[4] = {0, 1, 0, -1};
	static const int16_t DY[4] = {-1, 0, 1, 0};

	lattice_t was = trail[trail_i(head_i)];
	lattice_t next;
	next.x = (int16_t)(was.x + DX[dir]);
	next.y = (int16_t)(was.y + DY[dir]);

	if (next.x < 0 || next.x >= cols || next.y < 0 || next.y >= rows) {
		if (!wrap) {
			show_dead(tr("snake_lost_wall"));
			return false;
		}
		next.x = (int16_t)((next.x + cols) % cols);
		next.y = (int16_t)((next.y + rows) % rows);
	}

	// Everything but the tail is in the way. The tail is not, because it is
	// gone by the time the head gets there -- and when the head is eating, the
	// apple it is reaching for was never standing on the tail.
	for (int i = 1; i < len - 1; i++) {
		lattice_t c = trail[trail_i(head_i - i)];
		if (c.x == next.x && c.y == next.y) {
			show_dead(tr("snake_lost_self"));
			return false;
		}
	}

	head_i = trail_i(head_i + 1);
	trail[head_i] = next;

	if (next.x == food_x && next.y == food_y) {
		apples++;
		game_frame_add_score(&frame, APPLE_POINTS);
		if (len < SEG_MAX) {
			len++;
			fit_body();
		}
		int now = STEP_START_MS - apples * STEP_FASTEN_MS;
		step_ms = now < STEP_MIN_MS ? STEP_MIN_MS : now;
		place_food();
	}
	return true;
}

// Where segment `i` is right now: on the line between the cell it has arrived at
// and the one it left. A segment that came through the wall has no such line --
// its two cells are at opposite ends of the board -- so it is simply shown at the
// far edge, which is what a wrap looks like from outside anyway.
static void segment_pos(int i, int t, int *x, int *y) {
	lattice_t to = trail[trail_i(head_i - i)];
	lattice_t from = trail[trail_i(head_i - i - 1)];

	int dx = to.x - from.x;
	int dy = to.y - from.y;
	int px, py;
	if ((dx < 0 ? -dx : dx) > 1 || (dy < 0 ? -dy : dy) > 1) {
		px = to.x;
		py = to.y;
	} else {
		px = from.x + dx * t / 1024;
		py = from.y + dy * t / 1024;
	}

	*x = px * cell_px + (cell_px - seg_side) / 2;
	*y = py * cell_px + (cell_px - seg_side) / 2;
}

static void draw(void) {
	int t = (int)(into_step_us * 1024 / ((int64_t)step_ms * 1000));
	if (t > 1024) {
		t = 1024;
	}

	for (int i = 0; i < len && i < segs_made; i++) {
		int x, y;
		segment_pos(i, t, &x, &y);
		lv_obj_set_pos(seg[i], x, y);
	}
}

// ---------------------------------------------------------------------------
// the clock and the glass
// ---------------------------------------------------------------------------

static void tick_cb(void) {
	if (!running) {
		return;
	}

	int64_t us = frame.tick_us;
	if (us <= 0) {
		return;
	}
	// A tick is worth at most three steps. Long enough that the run keeps real
	// time when the panel is briefly busy, and short enough that a stutter
	// cannot carry the snake through a wall the player never saw it approach.
	// What is left over is dropped rather than owed: the point of the step is
	// that the picture and the rules agree at the end of it. (A panel that is
	// behind by a quarter of a second has gone dark, and the frame has frozen
	// the run for that before this tick was ever asked.)
	if (us > (int64_t)step_ms * 3000) {
		us = (int64_t)step_ms * 3000;
	}

	into_step_us += us;
	while (into_step_us >= (int64_t)step_ms * 1000) {
		into_step_us -= (int64_t)step_ms * 1000;
		if (!advance()) {
			return;
		}
	}
	draw();
}

// A tap is relative -- the left half of the board turns the snake left, the right
// half turns it right -- because a snake is always going somewhere, and the
// question a player is asking is which way to turn, not which way is north. A
// swipe is absolute, for the same reason in reverse: a finger that has drawn a
// direction across the glass has already answered which way is north.
static void tap_cb(const lv_point_t *p) {
	if (!running) {
		return;
	}
	int mid = lv_obj_get_x(frame.field) + frame.field_w / 2;
	offer_turn((dir + (p->x < mid ? 3 : 1)) % 4);
}

static void swipe_cb(lv_dir_t d, const lv_point_t *p) {
	(void)p;
	if (!running) {
		return;
	}
	offer_turn(d == LV_DIR_TOP ? DIR_UP : d == LV_DIR_RIGHT ? DIR_RIGHT
							 : d == LV_DIR_BOTTOM	 ? DIR_DOWN
								       : DIR_LEFT);
}

// ---------------------------------------------------------------------------
// the header, and what the card's buttons mean
// ---------------------------------------------------------------------------

static void pause_pressed(void) {
	game_frame_freeze(&frame, !frame.frozen);
}

static void start_run(void) {
	reset_run();
	game_frame_hide_overlay(&frame);
	running = true;
	game_frame_set_period(&frame, TICK_MS);
	game_frame_set_header_button_icon(&frame, 0, &icon_pause);
}

// The mode is a preference rather than a turn of play, so it is written down
// where the rest of the app's preferences live, and it is only reachable from
// the card, where it cannot be mistaken for a move.
static void toggle_wrap(void) {
	wrap = !wrap;
	config_set_bool(SECTION, "wrap", wrap);
	config_save();
	layout(); // the wall is the board's edge, so the board changes with it
	show_ready();
}

static void card_primary(void) {
	start_run();
}

static void card_secondary(void) {
	toggle_wrap();
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void show_cb(void) {
	wrap = config_get_bool(SECTION, "wrap", false);
	game_frame_load_best(&frame);
	layout();
	// The board is laid out and the snake set up behind the card, so that the
	// card has a game to sit on and the first step is drawn from a position the
	// player has already seen.
	reset_run();
	show_ready();
}

// Nothing to give back: the frame stops the clock when the page is left and takes
// the best score to the config with it, and a run in progress is worth no more
// keeping than a song that was never started.
static void hide_cb(void) {
	running = false;
}

static void frozen_cb(bool frozen) {
	game_frame_set_header_button_icon(&frame, 0, frozen ? &icon_play : &icon_pause);
	game_frame_set_hint(&frame, frozen ? tr("game_paused") : tr("snake_hint"));
}

static void theme_cb(void) {
	layout();
	fit_body();
	draw();
}

// The page is built the first time it is opened, not at start-up. A game the player
// never opens costs nothing but the configuration it is handed, and a theme change
// never has to walk a board nobody can see. Once built it stays, as the other pages
// do, so the next visit is instant and the run is where it was left.
static gui_config_t *snake_cfg;

static void snake_build(gui_config_t *cfg) {
	static const game_hooks_t hooks = {
		.on_show = show_cb,
		.on_hide = hide_cb,
		.on_tick = tick_cb,
		.on_tap = tap_cb,
		.on_swipe = swipe_cb,
		.on_frozen = frozen_cb,
		.on_theme = theme_cb,
	};

	frame.hooks = &hooks;
	frame.section = SECTION;
	frame.title = tr("snake");
	snake_screen = lv_obj_create(NULL);
	frame.screen = snake_screen;
	frame.on_overlay_primary = card_primary;
	frame.on_overlay_secondary = card_secondary;
	game_frame_build(&frame, cfg);

	board = lv_obj_create(frame.field);
	lv_obj_remove_style_all(board);
	lv_obj_set_clickable(board, false);
	lv_obj_set_scrollable(board, false);

	food_obj = lv_obj_create(board);
	lv_obj_remove_style_all(food_obj);
	lv_obj_set_clickable(food_obj, false);
	lv_obj_set_scrollable(food_obj, false);
	lv_obj_set_style_bg_opa(food_obj, LV_OPA_COVER, 0);
	lv_obj_set_hidden(food_obj, true);

	game_frame_add_header_button(&frame, &icon_pause, pause_pressed);

	// The card is built before the game's own objects, so it has to be put back
	// on top of them.
	lv_obj_move_foreground(frame.overlay);
}

void snake_init(gui_config_t *cfg) {
	snake_cfg = cfg;
}

void snake_open(void) {
	if (!snake_screen) {
		snake_build(snake_cfg);
	}
	srand((unsigned)time(NULL) ^ (unsigned)lv_tick_get());
	game_frame_open(&frame);
}
