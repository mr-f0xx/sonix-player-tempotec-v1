// game2048.c -- 2048, from the More page.

#include "game2048.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/games/games.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

// ---------------------------------------------------------------------------
// the board
// ---------------------------------------------------------------------------
//
// Sixteen cells and two objects apiece: a well that is always there, and a tile
// painted from `board_exp[]`. A move is then one pass over the four lines, and
// nothing is created or destroyed while playing -- on this hardware an
// allocation per tile per move would show up as a stutter exactly when the
// board is full and every row is on the move.
//
// The tiles never move. While a slide runs they are hidden and a ghost -- one
// per tile that was on the board when the swipe began -- travels from its old
// cell to its new one, so the two tiles that merge are seen coming together
// rather than one appearing where the other had been. When the slide is over the
// tiles are painted afresh, the ghosts are put away, and the cells that merged
// get their pop.
//
// The rules are the published ones: a swipe sends every tile to that wall, two
// equal tiles that meet become the next power of two and score it once, a fresh
// 2 or 4 turns up after every move that did something, and the board is lost
// when nothing can slide and nothing can merge.

#define GRID 4
#define CELLS (GRID * GRID)

// The directions as the game's own ids: LV_DIR_* is a mask, and a mask is
// awkward to index an array with.
enum { DIR_LEFT, DIR_UP, DIR_RIGHT, DIR_DOWN };

#define SECTION "game2048"

// The slide and the pop after it, both short on purpose: the board is small, the
// eye is already on the cells the tiles arrive at, and a move that takes a
// quarter of a second to draw is a move that cannot be made twice at the speed
// the game is played at.
#define SLIDE_MS 90
#define POP_MS 120

// What the field can be under. The accent button of each is what a tap anywhere
// on the board means.
enum { CARD_NONE, CARD_INTRO, CARD_WON, CARD_LOST };

lv_obj_t *game2048_screen;

static game_frame_t frame;

static lv_obj_t *board;
static lv_obj_t *well[CELLS];
static lv_obj_t *tile[CELLS];
static lv_obj_t *ghost_obj[CELLS];
static int cell_side, cell_gap, board_x, board_y;

// The board, as exponents: 0 is an empty cell and 3 is a 4.
static uint8_t board_exp[CELLS];
static uint8_t undo_exp[CELLS];
static int undo_score;
static bool undo_ready;

// What is sliding and where to. The three arrays are indexed by ghost, and a
// ghost exists for every tile that was on the board when the move began.
static int ghost_from[CELLS];
static int ghost_to[CELLS];
static uint8_t ghost_exp[CELLS];
static int ghost_n;

// The cells to pop once the slide is over, and the tile that just turned up.
static int pop_cells[CELLS];
static int pop_n;
static int spawn_cell = -1;

static bool sliding;
static int pending_card; // the card waiting for the animation to be done
static bool stuck;	       // no move left
static bool reached;	   // 2048 has been made
static bool reached_shown; // ... and the player has been told
static int card;

static void show_card(int which);

// ---------------------------------------------------------------------------
// colours
// ---------------------------------------------------------------------------

// The value ramp: cold slate at 2, up through the blues and the greens into
// amber and red, with 2048 landing on the violet Tokyo Night keeps for the
// things worth finding. It is the same in every palette, because a tile's colour
// is the game's own and has to mean the same thing to everybody who plays.
static lv_color_t tile_colour(int exponent, lv_color_t *ink_out) {
	// Indexed by exponent, so a 2 is slot 1. Past 8192 the tiles keep the
	// last hue and only grow brighter, which is as far as a board of
	// sixteen cells ever comes. Saturation and value are LVGL's 0..100.
	static const uint16_t HUE[] = {0, 215, 205, 192, 175, 155, 130, 95, 60, 38, 18, 350, 285, 290};
	static const uint8_t SAT[] = {0, 26, 34, 42, 50, 55, 60, 66, 72, 78, 84, 80, 74, 78};
	static const uint8_t VAL[] = {0, 40, 45, 50, 55, 60, 64, 68, 72, 76, 80, 86, 90, 95};

	int i = exponent < 1 ? 1 : (exponent > 13 ? 13 : exponent);
	lv_color_t colour = lv_color_hsv_to_rgb(HUE[i], SAT[i], VAL[i]);

	// A bright tile takes dark letters and a dark one white. The ramp walks
	// across the middle of its range somewhere around 128, so reading the
	// colour back is what keeps this true if the ramp is retuned.
	if (ink_out) {
		*ink_out = lv_color_luminance(colour) > 140 ? lv_color_make(0x20, 0x20, 0x24) : lv_color_white();
	}
	return colour;
}

// One step off the page: raised in a dark theme, pressed into a light one, which
// is what the rest of the interface does with a surface that holds something
// else.
static lv_color_t board_colour(void) {
	return theme_is_dark() ? lv_color_lighten(theme()->screen_bg, LV_OPA_10)
			       : lv_color_darken(theme()->screen_bg, LV_OPA_10);
}

static lv_color_t well_colour(void) {
	return theme_is_dark() ? lv_color_lighten(board_colour(), LV_OPA_10)
			       : lv_color_darken(board_colour(), LV_OPA_10);
}

// ---------------------------------------------------------------------------
// the tiles
// ---------------------------------------------------------------------------

// The largest of the player's sizes the number fits into, which is what lets a
// board of 2s be a slab of a digit and a 131 072 still be read.
static const lv_font_t *tile_font(int digits) {
	static const lv_font_t *const SIZES[] = {&font_ui_32, &font_ui_28, &font_ui_26, &font_ui_24, &font_ui_22,
						 &font_ui_20, &font_ui_18, &font_ui_16, &font_ui_14};
	const int count = (int)(sizeof(SIZES) / sizeof(SIZES[0]));
	int room = cell_side - 2 * frame.u;

	for (int i = 0; i < count; i++) {
		char probe[8];
		int len = digits > 7 ? 7 : digits;
		for (int k = 0; k < len; k++) {
			probe[k] = '8'; // the widest digit, so this is the worst case
		}
		probe[len] = '\0';
		lv_point_t size;
		lv_text_get_size(&size, probe, SIZES[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x <= room && size.y <= room) {
			return SIZES[i];
		}
	}
	return SIZES[count - 1];
}

static void paint_object(lv_obj_t *obj, int exponent) {
	lv_color_t ink;
	lv_obj_set_style_bg_color(obj, tile_colour(exponent, &ink), 0);

	char text[16];
	snprintf(text, sizeof(text), "%d", 1 << exponent);
	lv_obj_t *label = lv_obj_get_child(obj, 0);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_color(label, ink, 0);
	lv_obj_set_style_text_font(label, tile_font((int)strlen(text)), 0);
}

static void paint_tiles(void) {
	for (int i = 0; i < CELLS; i++) {
		lv_obj_set_hidden(tile[i], board_exp[i] == 0);
		if (!board_exp[i]) {
			continue;
		}
		lv_obj_set_size(tile[i], cell_side, cell_side);
		lv_obj_set_style_opa(tile[i], LV_OPA_COVER, 0);
		paint_object(tile[i], board_exp[i]);
	}
}

// Where cell `i` sits in the field, which is what the ghosts -- the field's own
// children -- are placed at. The wells and the tiles are the board's, so
// whoever draws them takes board_x and board_y off.
static void cell_pos(int i, int *x, int *y) {
	*x = board_x + cell_gap + (i % GRID) * (cell_side + cell_gap);
	*y = board_y + cell_gap + (i / GRID) * (cell_side + cell_gap);
}

// ---------------------------------------------------------------------------
// the layout
// ---------------------------------------------------------------------------

static void layout(void) {
	cell_gap = 3 * frame.u;
	int usable = frame.field_w < frame.field_h ? frame.field_w : frame.field_h;
	// A square board with a gap of room at every edge, as big as the field
	// allows. Wells and tiles then sit on one lattice, and on the V1 the
	// whole page lands on multiples of two pixels.
	cell_side = (usable - cell_gap * (GRID + 1)) / GRID;
	int side = cell_side * GRID + cell_gap * (GRID + 1);
	board_x = (frame.field_w - side) / 2;
	board_y = (frame.field_h - side) / 2;

	lv_obj_set_pos(board, board_x, board_y);
	lv_obj_set_size(board, side, side);
	lv_obj_set_style_radius(board, bp_tile_radius(), 0);

	for (int i = 0; i < CELLS; i++) {
		int x, y;
		cell_pos(i, &x, &y);
		x -= board_x;
		y -= board_y;
		lv_obj_set_pos(well[i], x, y);
		lv_obj_set_size(well[i], cell_side, cell_side);
		lv_obj_set_pos(tile[i], x, y);
		lv_obj_set_size(tile[i], cell_side, cell_side);
	}
}

// ---------------------------------------------------------------------------
// the rules
// ---------------------------------------------------------------------------

// The cell `pos` steps in from the wall of `line`, counted the way a swipe
// moves: position 0 is the cell a tile ends up against.
static int cell_at(int dir, int line, int pos) {
	switch (dir) {
	case DIR_LEFT:
		return line * GRID + pos;
	case DIR_RIGHT:
		return line * GRID + (GRID - 1 - pos);
	case DIR_UP:
		return pos * GRID + line;
	default:
		return (GRID - 1 - pos) * GRID + line; // DIR_DOWN
	}
}

// One line, read in from the wall, slid and merged. `out[]` and `merged[]` are
// the line as it becomes, by position counted from the wall; `to[]` is the other
// way round and answers "which position did the tile that started at i end up
// at?", which is what the ghosts need and nothing else does. What the move is
// worth is added to `*gained`.
static bool slide_line(const int *line, int *out, int *to, int *gained, bool *merged) {
	int target = 0;
	int previous = -1; // the position a following equal tile may merge into
	bool moved = false;

	for (int i = 0; i < GRID; i++) {
		out[i] = 0;
		to[i] = i;
		merged[i] = false;
	}
	for (int i = 0; i < GRID; i++) {
		int e = line[i];
		if (!e) {
			continue;
		}
		if (previous >= 0 && out[previous] == e && !merged[previous]) {
			out[previous] = e + 1;
			merged[previous] = true;
			to[i] = previous;
			*gained += 1 << (e + 1); // a merge scores the tile it made
			previous = -1;		// three alike make a pair and a spare
			moved = true;
			continue;
		}
		out[target] = e;
		to[i] = target;
		if (target != i) {
			moved = true;
		}
		previous = target;
		target++;
	}
	return moved;
}

// True while some swipe would still do something: an empty cell, or two equal
// neighbours anywhere on the board.
static bool any_move_left(void) {
	for (int i = 0; i < CELLS; i++) {
		if (!board_exp[i]) {
			return true;
		}
		int c = i % GRID;
		int r = i / GRID;
		if (c + 1 < GRID && board_exp[i] == board_exp[i + 1]) {
			return true;
		}
		if (r + 1 < GRID && board_exp[i] == board_exp[i + GRID]) {
			return true;
		}
	}
	return false;
}

static void spawn(void) {
	int free_cells[CELLS];
	int n = 0;
	for (int i = 0; i < CELLS; i++) {
		if (!board_exp[i]) {
			free_cells[n++] = i;
		}
	}
	spawn_cell = n ? free_cells[rand() % n] : -1;
	if (spawn_cell >= 0) {
		board_exp[spawn_cell] = (rand() % 10) == 0 ? 2 : 1; // one in ten starts as a 4
	}
}

// The whole move at once: the four lines, the ghosts that draw it, the score it
// is worth and the state the board is in afterwards. False when the swipe
// changed nothing, which is not a turn -- it earns no tile, scores nothing and
// does not cost an undo.
static bool apply_move(int dir) {
	uint8_t new_exp[CELLS];
	memcpy(new_exp, board_exp, sizeof(new_exp));

	int gained = 0;
	bool moved = false;
	ghost_n = 0;
	pop_n = 0;

	for (int l = 0; l < GRID; l++) {
		int line[GRID], out[GRID], to[GRID];
		bool merged[GRID];
		for (int p = 0; p < GRID; p++) {
			line[p] = board_exp[cell_at(dir, l, p)];
		}
		if (!slide_line(line, out, to, &gained, merged)) {
			continue;
		}
		moved = true;
		// What the line becomes, in the destination's own order.
		for (int p = 0; p < GRID; p++) {
			new_exp[cell_at(dir, l, p)] = out[p];
			if (merged[p] && pop_n < CELLS) {
				pop_cells[pop_n++] = cell_at(dir, l, p);
			}
		}
		// And where each tile that was on it is going, which is the whole
		// of what there is to draw.
		for (int p = 0; p < GRID; p++) {
			if (!line[p] || ghost_n >= CELLS) {
				continue;
			}
			ghost_from[ghost_n] = cell_at(dir, l, p);
			ghost_to[ghost_n] = cell_at(dir, l, to[p]);
			ghost_exp[ghost_n] = (uint8_t)line[p];
			ghost_n++;
		}
	}
	if (!moved) {
		return false;
	}

	int highest = 0;
	for (int i = 0; i < CELLS; i++) {
		if (new_exp[i] > highest) {
			highest = new_exp[i];
		}
	}

	// The move goes on the record before the tiles are told about it, so that
	// an undo has the score back as well as the board.
	memcpy(undo_exp, board_exp, sizeof(undo_exp));
	undo_score = frame.score;
	undo_ready = true;

	memcpy(board_exp, new_exp, sizeof(board_exp));
	if (gained) {
		game_frame_add_score(&frame, gained);
	}
	// Two thousand and forty-eight is a win the first time and a milestone
	// afterwards; the board keeps going either way.
	if (highest >= 11 && !reached) {
		reached = true;
	}
	stuck = !any_move_left();
	return true;
}

// ---------------------------------------------------------------------------
// the slide and the pop
// ---------------------------------------------------------------------------

// v runs the whole slide from 0 to 1024, so one animation draws a move however
// many rows are on the go.
static void slide_exec(void *var, int32_t v) {
	(void)var;
	for (int i = 0; i < ghost_n; i++) {
		int fx, fy, tx, ty;
		cell_pos(ghost_from[i], &fx, &fy);
		cell_pos(ghost_to[i], &tx, &ty);
		lv_obj_set_pos(ghost_obj[i], fx + (tx - fx) * v / 1024, fy + (ty - fy) * v / 1024);
	}
}

// The pop, over a triangle of a bump: out to 14 % more than a cell and back,
// which is the one gesture that says two tiles have become one. The tile that
// just turned up fades in across the first third of it, so it is on the board
// before the pop is over rather than arriving late.
static void pop_exec(void *var, int32_t v) {
	(void)var;
	int bump = v < 500 ? v : 1000 - v;
	int side = cell_side * (1000 + 140 * bump / 500) / 1000;
	int offset = (cell_side - side) / 2;
	for (int i = 0; i < pop_n; i++) {
		int x, y;
		cell_pos(pop_cells[i], &x, &y);
		lv_obj_set_pos(tile[pop_cells[i]], x - board_x + offset, y - board_y + offset);
		lv_obj_set_size(tile[pop_cells[i]], side, side);
	}
	if (spawn_cell >= 0) {
		int opa = v * 3 > 1000 ? LV_OPA_COVER : LV_OPA_COVER * v * 3 / 1000;
		lv_obj_set_style_opa(tile[spawn_cell], (lv_opa_t)opa, 0);
	}
}

// The end of the move, whichever animation got there last.
static void move_over(void) {
	layout();
	for (int i = 0; i < CELLS; i++) {
		lv_obj_set_style_opa(tile[i], LV_OPA_COVER, 0);
	}
	if (pending_card) {
		int which = pending_card;
		pending_card = CARD_NONE;
		show_card(which);
	}
}

static void pop_end(lv_anim_t *a) {
	(void)a;
	move_over();
}

static void start_pop(void) {
	if (!pop_n && spawn_cell < 0) {
		move_over();
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &frame);
	lv_anim_set_values(&a, 0, 1000);
	lv_anim_set_duration(&a, POP_MS);
	lv_anim_set_exec_cb(&a, pop_exec);
	lv_anim_set_completed_cb(&a, pop_end);
	lv_anim_start(&a);
}

static void slide_end(lv_anim_t *a) {
	(void)a;
	sliding = false;
	for (int i = 0; i < CELLS; i++) {
		lv_obj_set_hidden(ghost_obj[i], true);
	}
	paint_tiles();
	// The new tile is painted with the rest and then taken back to nothing,
	// so the pop has something to bring up rather than a hole in the board.
	if (spawn_cell >= 0) {
		lv_obj_set_style_opa(tile[spawn_cell], LV_OPA_TRANSP, 0);
	}
	start_pop();
}

static void start_slide(void) {
	sliding = true;
	for (int i = 0; i < CELLS; i++) {
		lv_obj_set_hidden(tile[i], true);
		lv_obj_set_hidden(ghost_obj[i], true);
	}
	for (int i = 0; i < ghost_n; i++) {
		int x, y;
		cell_pos(ghost_from[i], &x, &y);
		lv_obj_set_pos(ghost_obj[i], x, y);
		lv_obj_set_size(ghost_obj[i], cell_side, cell_side);
		paint_object(ghost_obj[i], ghost_exp[i]);
		lv_obj_set_hidden(ghost_obj[i], false);
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &frame);
	lv_anim_set_values(&a, 0, 1024);
	lv_anim_set_duration(&a, SLIDE_MS);
	lv_anim_set_exec_cb(&a, slide_exec);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, slide_end);
	lv_anim_start(&a);
}

// Called before a new move and before anything else looks at the board. The
// logic is applied when the swipe is read, so settling is only about the
// picture: a player faster than the animation never waits for it and never sees
// a frame of a move they have already left behind.
static void settle(void) {
	if (!sliding) {
		return;
	}
	lv_anim_delete(&frame, slide_exec);
	lv_anim_delete(&frame, pop_exec);
	sliding = false;
	for (int i = 0; i < CELLS; i++) {
		lv_obj_set_hidden(ghost_obj[i], true);
	}
	paint_tiles();
	layout();
	for (int i = 0; i < CELLS; i++) {
		lv_obj_set_style_opa(tile[i], LV_OPA_COVER, 0);
	}
}

// ---------------------------------------------------------------------------
// the game
// ---------------------------------------------------------------------------

static void new_game(void) {
	pending_card = CARD_NONE;
	card = CARD_NONE;
	settle();
	game_frame_hide_overlay(&frame);
	memset(board_exp, 0, sizeof(board_exp));
	spawn();
	spawn();
	reached = false;
	reached_shown = false;
	stuck = false;
	undo_ready = false;
	spawn_cell = -1;
	game_frame_set_score(&frame, 0);
	paint_tiles();
	game_frame_set_hint(&frame, tr("game2048_hint"));
}

static void undo_move(void) {
	if (!undo_ready) {
		return;
	}
	pending_card = CARD_NONE;
	settle();
	memcpy(board_exp, undo_exp, sizeof(board_exp));
	undo_ready = false;
	stuck = false;
	spawn_cell = -1;
	game_frame_set_score(&frame, undo_score);
	paint_tiles();
	game_frame_set_hint(&frame, tr("game2048_undone"));
}

// The three cards, written once because they are the same shape with a different
// sentence in the middle.
static void show_card(int which) {
	card = which;
	switch (which) {
	case CARD_INTRO:
		game_frame_show_overlay(&frame, tr("2048"), tr("game2048_intro"), tr("game_start"), NULL);
		break;
	case CARD_WON:
		game_frame_show_overlay(&frame, tr("2048"), tr("game2048_won"), tr("game2048_keep_going"),
					tr("game2048_new_game"));
		break;
	case CARD_LOST:
		game_frame_show_overlay(&frame, tr("game2048_lost"), tr("game2048_lost_body"), tr("game2048_new_game"),
					undo_ready ? tr("game2048_undo") : NULL);
		break;
	default:
		game_frame_hide_overlay(&frame);
		break;
	}
}

static void do_move(int dir) {
	settle();
	if (card != CARD_NONE) {
		return; // the card decides what the next touch means
	}
	if (!apply_move(dir)) {
		return;
	}
	spawn();
	start_slide();

	// The win and the end of the board wait for the last tile to be where it
	// belongs, so the move that decided it is seen before it is explained.
	if (reached && !reached_shown) {
		reached_shown = true;
		pending_card = CARD_WON;
	} else if (stuck) {
		pending_card = CARD_LOST;
	}
}

static void swipe_cb(lv_dir_t dir, const lv_point_t *p) {
	(void)p;
	do_move(dir == LV_DIR_LEFT ? DIR_LEFT : dir == LV_DIR_RIGHT ? DIR_RIGHT
				   : dir == LV_DIR_TOP	      ? DIR_UP
								   : DIR_DOWN);
}

static void undo_pressed(void) {
	if (card == CARD_LOST) {
		card = CARD_NONE;
		game_frame_hide_overlay(&frame);
	}
	undo_move();
}

static void new_game_pressed(void) {
	new_game();
}

// The card's accent button, which is also what a tap anywhere on the field means.
static void card_primary(void) {
	switch (card) {
	case CARD_INTRO:
		card = CARD_NONE;
		game_frame_hide_overlay(&frame);
		break;
	case CARD_WON:
		card = CARD_NONE;
		game_frame_hide_overlay(&frame);
		if (stuck) {
			show_card(CARD_LOST);
		}
		break;
	case CARD_LOST:
		new_game();
		break;
	default:
		break;
	}
}

// The quiet one: the other thing the player might have meant.
static void card_secondary(void) {
	if (card == CARD_WON) {
		new_game();
	} else if (card == CARD_LOST) {
		card = CARD_NONE;
		game_frame_hide_overlay(&frame);
		undo_move();
	}
}

// ---------------------------------------------------------------------------
// what is kept between sessions
// ---------------------------------------------------------------------------
//
// Sixteen cells of four bits apiece is sixteen characters of hex, which is a
// board that fits on one line of the config, reads back with nothing to allocate
// and refuses to parse when it is not a board at all. A file edited by hand, or
// left by a build that numbered the cells differently, is then simply no board,
// and the game starts afresh instead of showing a row of tiles that cannot exist.

#define BOARD_CHARS CELLS

static bool load_board(void) {
	const char *text = config_get(SECTION, "board", NULL);
	if (!text || strlen(text) != BOARD_CHARS) {
		return false;
	}
	int seen = 0;
	uint8_t read[CELLS];
	for (int i = 0; i < CELLS; i++) {
		char c = text[i];
		int v;
		if (c >= '0' && c <= '9') {
			v = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			v = c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			v = c - 'A' + 10;
		} else {
			return false;
		}
		// An exponent past 13 is a tile of 8192 and over: no board this
		// game ever had got that far.
		if (v > 13) {
			return false;
		}
		read[i] = (uint8_t)v;
		seen += v != 0;
	}
	if (seen < 2) {
		return false;
	}

	memcpy(board_exp, read, sizeof(board_exp));
	reached = config_get_bool(SECTION, "reached", false);
	reached_shown = true; // the board was saved after the win had been seen
	stuck = !any_move_left();
	undo_ready = false;
	spawn_cell = -1;
	game_frame_set_score(&frame, (int)config_get_int(SECTION, "score", 0));
	return true;
}

static void save_board(void) {
	static const char HEX[] = "0123456789abcdef";
	char text[BOARD_CHARS + 1];
	for (int i = 0; i < CELLS; i++) {
		text[i] = HEX[board_exp[i] & 15];
	}
	text[BOARD_CHARS] = '\0';
	config_set(SECTION, "board", text);
	config_set_int(SECTION, "score", frame.score);
	config_set_bool(SECTION, "reached", reached);
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

// A tile, whether it lives in a cell or is one of the ghosts: a rounded square
// with a number in it and nothing else. Nothing in the field is clickable -- the
// field takes the presses, which is what lets a swipe start on a tile -- and the
// pop is allowed to lean over the board's edge, because that is what it looks
// like when two tiles become one.
static void make_tile(lv_obj_t *obj) {
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(obj, 0, 0);
	lv_obj_set_style_shadow_width(obj, 0, 0);
	lv_obj_set_style_radius(obj, frame.u * 2, 0);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_clickable(obj, false);

	lv_obj_t *label = lv_label_create(obj);
	lv_label_set_text(label, "");
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);
}

static void recolour(void) {
	lv_obj_set_style_bg_color(board, board_colour(), 0);
	for (int i = 0; i < CELLS; i++) {
		lv_obj_set_style_bg_color(well[i], well_colour(), 0);
	}
}

static void show_cb(void) {
	layout();
	if (!load_board()) {
		new_game();
		// The first time only: the rules of 2048 are one sentence, but it
		// is a sentence worth reading before the first swipe.
		if (!config_get_bool(SECTION, "seen", false)) {
			config_set_bool(SECTION, "seen", true);
			show_card(CARD_INTRO);
		}
		return;
	}
	paint_tiles();
	game_frame_set_hint(&frame, tr("game2048_hint"));
	if (stuck) {
		show_card(CARD_LOST);
	}
}

static void hide_cb(void) {
	settle();
	save_board();
}

static void theme_cb(void) {
	recolour();
	paint_tiles();
}

// The page is built the first time it is opened, not at start-up. A game the player
// never opens costs nothing but the configuration it is handed, and a theme change
// never has to walk a board nobody can see. Once built it stays, as the other pages
// do, so the next visit is instant and the run is where it was left.
static gui_config_t *game2048_cfg;

static void game2048_build(gui_config_t *cfg) {
	static const game_hooks_t hooks = {
		.on_show = show_cb,
		.on_hide = hide_cb,
		.on_swipe = swipe_cb,
		.on_theme = theme_cb,
	};

	frame.hooks = &hooks;
	frame.section = SECTION;
	frame.title = tr("2048");
	game2048_screen = lv_obj_create(NULL);
	frame.screen = game2048_screen;
	frame.on_overlay_primary = card_primary;
	frame.on_overlay_secondary = card_secondary;
	game_frame_build(&frame, cfg);

	board = lv_obj_create(frame.field);
	lv_obj_remove_style_all(board);
	lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0); // recolour() gives it a
	// colour, and a colour without an opacity is nothing to draw
	lv_obj_set_scrollable(board, false);
	lv_obj_set_scrollable(board, false);
	lv_obj_set_clickable(board, false);
	lv_obj_set_overflow_visible(board, true);

	for (int i = 0; i < CELLS; i++) {
		well[i] = lv_obj_create(board);
		lv_obj_remove_style_all(well[i]);
		lv_obj_set_style_bg_opa(well[i], LV_OPA_COVER, 0);
		lv_obj_set_style_radius(well[i], frame.u * 2, 0);
		lv_obj_set_scrollable(well[i], false);
		lv_obj_set_clickable(well[i], false);

		tile[i] = lv_obj_create(board);
		lv_obj_remove_style_all(tile[i]);
		make_tile(tile[i]);
		lv_obj_set_hidden(tile[i], true);
	}

	// The ghosts belong to the field rather than to the board: they are the
	// thing that crosses the gap between one cell and the next.
	for (int i = 0; i < CELLS; i++) {
		ghost_obj[i] = lv_obj_create(frame.field);
		lv_obj_remove_style_all(ghost_obj[i]);
		make_tile(ghost_obj[i]);
		lv_obj_set_hidden(ghost_obj[i], true);
	}

	// The card is built before the game's own objects, so it has to be put
	// back on top of them.
	lv_obj_move_foreground(frame.overlay);

	game_frame_add_header_button(&frame, &icon_reset, undo_pressed);
	game_frame_add_header_button(&frame, &icon_refresh, new_game_pressed);

	recolour();
	layout();
}

void game2048_init(gui_config_t *cfg) {
	game2048_cfg = cfg;
}

void game2048_open(void) {
	if (!game2048_screen) {
		game2048_build(game2048_cfg);
	}
	srand((unsigned)time(NULL));
	game_frame_open(&frame);
}
