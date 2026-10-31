// dino.c -- Dino, from the More page.
//
// The rules are the ones everybody knows from being stuck on a plane: the ground
// goes by under you, something arrives from the right, a tap puts you in the air
// and a finger held down puts you on the ground, and the further you get the
// faster it goes. Every hundred points the sky changes hands.
//
// The sprites are tables of rectangles, drawn in units of the panel's own step:
// the dino is eight pieces, a cactus is three, a bird is three, and a pose is the
// list of where those pieces sit. Nothing is scaled, rotated or resized on the
// way round -- LVGL's image scaling is nearest neighbour and costs a pass over
// the pixels every frame, and a rectangle moving is a rectangle moving, which is
// both what this game looks like and what a player that is also decoding music
// can afford sixty times a second.
//
// The same reason shapes the physics: positions are kept as fractions of a unit
// and only become whole pixels when they are drawn, so the run never loses ground
// to rounding and a late frame cannot shorten a jump. The land, the clouds and the
// moon are all one number -- how far the run has gone -- divided by a different
// amount, which is what parallax is when there is no compositor to do it for you.

#include "dino.h"

#include <stdlib.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/games/games.h"
#include "src/gui/shell/icons.h"
#include "src/system/core/lang.h"

#define SECTION "dino"

// The clock, and the game's own units: two panel pixels on the V1, four on the
// HiBys, so one table of numbers gives both of them the same game with every edge
// landing on a whole pixel.
#define TICK_MS 16
#define UNIT frame.u

// Positions are held in 1/1024ths of a unit and turned into pixels on the way to
// the screen. Ninety units a second is 1.4 units per frame on the V1, and a run
// that rounded that away would lose a seventh of its speed and gain a shimmy.
#define FP_BITS 10
#define FP (1 << FP_BITS)

// The jump is 196 units a second against 800 of gravity: 24 units up, half a
// second in the air, which is long enough for a cactus and short enough that a
// bird at head height is a decision rather than an accident.
#define RUN_START 90
#define RUN_MAX 200
#define GRAVITY 800
#define JUMP_V0 196
#define DUCK_FALL 3 // ... times the gravity, for a finger held down in mid-air

#define DINO_X 6	  // where the dino stands, and the one number that is not
			  // about the dino's speed
#define GROUND_U 5	  // of field, left under the line, so the field's own edge
			  // reads as the far side of the world
#define PIECES_MAX 10	  // the most pieces in any one pose
#define OB_MAX 6	  // what can be in front of you at once

// ---------------------------------------------------------------------------
// the sprites
// ---------------------------------------------------------------------------

// One rectangle of a sprite: its bottom left corner in units, growing up and
// right, and a mark for the pieces that are the sky showing through rather than
// painted -- the dino's eye is a hole, because a hole is cheaper than a mask.
typedef struct {
	int8_t x, y, w, h;
	int8_t sky;
} piece_t;

typedef struct {
	const piece_t *pieces;
	int n;
	int8_t box[4]; // the part of it a collision is about, in the same units
	int8_t w, h;   // the whole of it, which is what the tables are drawn in
} sprite_t;

#define PIECE(x, y, w, h) {(x), (y), (w), (h), 0}
#define SKY(x, y, w, h) {(x), (y), (w), (h), 1}

// The dino, 20 units wide and 22 tall, facing right. The two running frames are
// the same animal with the legs swapped, which is how a runner is drawn when
// there are no frames to spare.
static const piece_t DINO_STAND[] = {PIECE(9, 15, 11, 7), SKY(16, 18, 2, 2), PIECE(14, 13, 6, 2), PIECE(5, 8, 10, 8),
				     PIECE(0, 10, 6, 4),   PIECE(13, 10, 3, 2),	  PIECE(6, 0, 3, 8),  PIECE(11, 0, 3, 8)};
static const piece_t DINO_RUN_A[] = {PIECE(9, 15, 11, 7), SKY(16, 18, 2, 2), PIECE(14, 13, 6, 2), PIECE(5, 8, 10, 8),
				     PIECE(0, 10, 6, 4),   PIECE(13, 10, 3, 2),	  PIECE(6, 0, 3, 8),  PIECE(11, 4, 3, 4)};
static const piece_t DINO_RUN_B[] = {PIECE(9, 15, 11, 7), SKY(16, 18, 2, 2), PIECE(14, 13, 6, 2), PIECE(5, 8, 10, 8),
				     PIECE(0, 10, 6, 4),   PIECE(13, 10, 3, 2),	  PIECE(6, 4, 3, 4),  PIECE(11, 0, 3, 8)};
static const piece_t DINO_JUMP[] = {PIECE(9, 15, 11, 7), SKY(16, 18, 2, 2), PIECE(14, 13, 6, 2), PIECE(5, 8, 10, 8),
				    PIECE(0, 10, 6, 4),  PIECE(13, 10, 3, 2),	  PIECE(5, 1, 4, 7),   PIECE(11, 1, 4, 7)};
static const piece_t DINO_DUCK[] = {PIECE(15, 5, 11, 7), SKY(22, 8, 2, 2), PIECE(20, 3, 6, 2), PIECE(4, 0, 14, 8),
				    PIECE(0, 3, 5, 4),   PIECE(5, 0, 3, 3),     PIECE(12, 0, 3, 3)};
// Dead is the standing pose with the legs stiff and the eye gone from a dot to a
// mark. It is one frame, and one frame is what a death needs.
static const piece_t DINO_DEAD[] = {PIECE(9, 15, 11, 7), SKY(16, 18, 2, 2), PIECE(14, 13, 6, 2), PIECE(5, 8, 10, 8),
				    PIECE(0, 10, 6, 4),  PIECE(13, 10, 3, 2),    PIECE(6, 0, 3, 8),   PIECE(11, 0, 3, 8),
				    PIECE(15, 18, 4, 1), PIECE(16, 17, 2, 3)};

enum { POSE_STAND, POSE_RUN_A, POSE_RUN_B, POSE_JUMP, POSE_DUCK, POSE_DEAD };

static const sprite_t POSES[] = {
    {DINO_STAND, 8, {3, 0, 14, 22}, 20, 22},  {DINO_RUN_A, 8, {3, 0, 14, 22}, 20, 22},
    {DINO_RUN_B, 8, {3, 0, 14, 22}, 20, 22},  {DINO_JUMP, 8, {3, 1, 14, 21}, 20, 22},
    {DINO_DUCK, 7, {2, 0, 23, 12}, 26, 12},   {DINO_DEAD, 10, {3, 0, 14, 22}, 20, 22},
};

// The cacti and the bird. Three small ones are one obstacle rather than three,
// because what the player has to read is the gap and not the count.
enum { OB_SMALL, OB_TALL, OB_TRIO, OB_BIRD_UP, OB_BIRD_DOWN, OB_COUNT };

static const piece_t CACTUS_SMALL[] = {PIECE(1, 0, 2, 10), PIECE(0, 4, 1, 4), PIECE(3, 6, 1, 3)};
static const piece_t CACTUS_TALL[] = {PIECE(1, 0, 3, 14), PIECE(0, 5, 1, 5), PIECE(4, 8, 1, 4)};
static const piece_t CACTUS_TRIO[] = {PIECE(0, 0, 3, 9), PIECE(5, 0, 3, 11), PIECE(10, 0, 3, 8)};
static const piece_t BIRD_UP[] = {PIECE(4, 3, 8, 3), PIECE(12, 4, 2, 2), PIECE(5, 6, 6, 4)};
static const piece_t BIRD_DOWN[] = {PIECE(4, 3, 8, 3), PIECE(12, 4, 2, 2), PIECE(5, 0, 6, 4)};

static const sprite_t SPR_OB[OB_COUNT] = {
    {CACTUS_SMALL, 3, {0, 0, 4, 10}, 4, 10},
    {CACTUS_TALL, 3, {0, 0, 5, 14}, 5, 14},
    {CACTUS_TRIO, 3, {0, 0, 13, 11}, 13, 11},
    {BIRD_UP, 3, {3, 2, 11, 5}, 14, 10},
    {BIRD_DOWN, 3, {3, 2, 11, 5}, 14, 10},
};

// The three heights a bird comes in. Low is a cactus with wings and wants a jump;
// middle wants a duck; high is only in the way of a jump, which is the one that
// makes the difference between holding on and letting go.
#define BIRD_LOW 0
#define BIRD_MID 11
#define BIRD_HIGH 20

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

lv_obj_t *dino_screen;

static game_frame_t frame;

static lv_obj_t *sky;
static lv_obj_t *ground;
static lv_obj_t *dino_piece[PIECES_MAX];
static lv_obj_t *ob_piece[OB_MAX][3];
static lv_obj_t *cloud[3];
static lv_obj_t *pebble[4];
static lv_obj_t *star[5];
static lv_obj_t *moon;
static lv_obj_t *moon_bite;

static int64_t run_fp;     // how far the land has gone by, and so where everything is
static int64_t y_fp, vy_fp; // the dino's height above the ground and its rate
static int64_t until_next_fp;
static int64_t ob_at[OB_MAX]; // where in the run each obstacle will be at the left edge
static int8_t ob_type[OB_MAX];
static int8_t ob_height[OB_MAX];
static bool ob_live[OB_MAX];
static int speed;	// units a second
static int score;
static int ground_y;	// the line, in panel pixels from the top of the field
static bool grounded;
static bool night;
static bool dead;
static bool running;

// ---------------------------------------------------------------------------
// putting pieces on the panel
// ---------------------------------------------------------------------------

static const sprite_t *dino_sprite(void) {
	if (dead) {
		return &POSES[POSE_DEAD];
	}
	if (!grounded) {
		return &POSES[POSE_JUMP];
	}
	if (game_frame_held(&frame)) {
		return &POSES[POSE_DUCK];
	}
	// Every ten units of ground, which is a stride a third of a second long at
	// the start and about one and a half times a second at the top speed.
	return &POSES[POSE_RUN_A + (int)((run_fp / (10 * FP)) & 1)];
}

static int dino_bottom(void) { return ground_y - (int)(y_fp >> FP_BITS) * UNIT; }

// A rectangle on the field with nothing else about it: no press of its own, no
// scroll of its own, no style inherited from the page. Every piece of every
// sprite, and every cloud, is one of these.
static lv_obj_t *make_prop(void) {
	lv_obj_t *obj = lv_obj_create(frame.field);
	lv_obj_remove_style_all(obj);
	// Taking every style off a thing takes its background opacity with it, and a
	// rectangle with a colour and no opacity in LVGL is a rectangle that is not
	// drawn at all. Everything made here is a flat block of one colour, so the
	// cover belongs with it; the props that are meant to be see-through say
	// their own on the way past, which is how the ground is a line of ink at
	// three fifths and a cloud is one at a fifth.
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_clickable(obj, false); // every press belongs to the field, so that
	// a tap or a swipe can start anywhere over the
	// world the game drew
	return obj;
}

// A sprite, by the ground line under it and the edge it starts from. The pieces
// are painted in the order the table lists them, which is why the eye comes after
// the head it is cut into.
static void put_sprite(lv_obj_t **objects, int capacity, const sprite_t *s, int px, int bottom, lv_color_t colour) {
	for (int i = 0; i < capacity; i++) {
		if (i >= s->n) {
			lv_obj_set_hidden(objects[i], true);
			continue;
		}
		const piece_t *p = &s->pieces[i];
		lv_obj_set_hidden(objects[i], false);
		lv_obj_set_pos(objects[i], px + p->x * UNIT, bottom - (p->y + p->h) * UNIT);
		lv_obj_set_size(objects[i], p->w * UNIT, p->h * UNIT);
		lv_obj_set_style_bg_color(objects[i], p->sky ? game_frame_field_bg(night) : colour, 0);
	}
}

// Where an obstacle's left edge is, in panel pixels, which can be negative
// before it arrives and after it has gone.
static int ob_x(int i) { return (int)(((ob_at[i] - run_fp) * UNIT) >> FP_BITS); }

static void draw_dino(void) {
	put_sprite(dino_piece, PIECES_MAX, dino_sprite(), DINO_X * UNIT, dino_bottom(), game_frame_ink(night));
}

// The horizon and what hangs over it. `scale` is how much slower a thing goes by
// than the land does: four for a cloud, twelve for a star, twenty-four for the
// moon. Each is placed by folding its offset into a strip a little wider than the
// field, so there is always another one coming and none of them has to be kept
// track of.
static void draw_props(lv_obj_t **pool, int count, int scale, const int *xs, const int *ys, const int *ws,
		       const int *hs, lv_color_t colour, lv_opa_t opa) {
	int strip = frame.field_w / UNIT + 8;
	if (strip < 8) {
		strip = 8; // a field with no room for a cloud still has a sky
	}
	int64_t gone = (run_fp >> FP_BITS) / scale;

	for (int i = 0; i < count; i++) {
		int at = (int)((xs[i] - gone) % strip);
		if (at < 0) {
			at += strip;
		}
		lv_obj_set_pos(pool[i], (at - 4) * UNIT, ground_y - (ys[i] + hs[i]) * UNIT);
		lv_obj_set_size(pool[i], ws[i] * UNIT, hs[i] * UNIT);
		lv_obj_set_style_bg_color(pool[i], colour, 0);
		lv_obj_set_style_bg_opa(pool[i], opa, 0);
	}
}

static void draw_sky(void) {
	static const int CLOUD_X[3] = {0, 34, 68};
	static const int CLOUD_Y[3] = {26, 34, 30};
	static const int CLOUD_W[3] = {11, 13, 9};
	static const int CLOUD_H[3] = {3, 3, 2};
	static const int STAR_X[5] = {4, 22, 41, 57, 73};
	static const int STAR_Y[5] = {40, 28, 46, 33, 24};
	static const int ONE[5] = {1, 1, 1, 1, 1};

	draw_props(cloud, 3, 4, CLOUD_X, CLOUD_Y, CLOUD_W, CLOUD_H, game_frame_ink(night),
		   night ? LV_OPA_40 : LV_OPA_20);
	draw_props(star, 5, 12, STAR_X, STAR_Y, ONE, ONE, game_frame_ink(night), LV_OPA_60);
	for (int i = 0; i < 5; i++) {
		lv_obj_set_hidden(star[i], !night); // stars are a night thing
	}

	// The moon is the one thing up there that is round, and the bite out of it
	// is the sky: a crescent for the price of two circles and a subtraction.
	int strip = frame.field_w / UNIT + 8;
	int64_t gone = (run_fp >> FP_BITS) / 24;
	int at = (int)((12 - gone) % strip);
	if (at < 0) {
		at += strip;
	}
	lv_obj_set_size(moon, 9 * UNIT, 9 * UNIT);
	lv_obj_set_size(moon_bite, 8 * UNIT, 8 * UNIT);
	lv_obj_set_pos(moon, (at - 4) * UNIT, ground_y - 45 * UNIT);
	lv_obj_set_pos(moon_bite, (at - 3) * UNIT + UNIT, ground_y - 44 * UNIT);
	lv_obj_set_style_bg_color(moon, game_frame_ink(night), 0);
	lv_obj_set_style_bg_color(moon_bite, game_frame_field_bg(night), 0);
	lv_obj_set_hidden(moon, !night);
	lv_obj_set_hidden(moon_bite, !night);
}

static void draw_ground(void) {
	static const int PEBBLE_X[4] = {0, 19, 41, 63};
	static const int PEBBLE_Y[4] = {1, 0, 1, 0};
	static const int PEBBLE_W[4] = {3, 2, 4, 2};
	static const int ONE[4] = {1, 1, 1, 1};

	draw_props(pebble, 4, 1, PEBBLE_X, PEBBLE_Y, PEBBLE_W, ONE, game_frame_ink(night), LV_OPA_40);

	for (int i = 0; i < OB_MAX; i++) {
		if (!ob_live[i]) {
			continue;
		}
		const sprite_t *s = &SPR_OB[ob_type[i]];
		// A bird flaps, twice a second, which is the only animation the
		// things in front of you get.
		if (ob_type[i] >= OB_BIRD_UP && ((run_fp / (FP * 25)) & 1)) {
			s = &SPR_OB[OB_BIRD_DOWN];
		}
		put_sprite(ob_piece[i], 3, s, ob_x(i), ground_y - ob_height[i] * UNIT, game_frame_ink(night));
	}
}

static void recolour(void) {
	lv_obj_set_style_bg_color(sky, game_frame_field_bg(night), 0);
	lv_obj_set_style_bg_opa(sky, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(ground, game_frame_ink(night), 0);
	draw_sky();
	draw_ground();
	draw_dino();
}

static void layout(void) {
	ground_y = frame.field_h - GROUND_U * UNIT;

	lv_obj_set_pos(sky, 0, 0);
	lv_obj_set_size(sky, frame.field_w, frame.field_h);
	lv_obj_set_pos(ground, 0, ground_y);
	lv_obj_set_size(ground, frame.field_w, UNIT);
	recolour();
}

// ---------------------------------------------------------------------------
// the run
// ---------------------------------------------------------------------------

static void clear_obstacles(void) {
	for (int i = 0; i < OB_MAX; i++) {
		ob_live[i] = false;
		for (int p = 0; p < 3; p++) {
			lv_obj_set_hidden(ob_piece[i][p], true);
		}
	}
}

static void reset_run(void) {
	run_fp = 0;
	y_fp = 0;
	vy_fp = 0;
	until_next_fp = (int64_t)RUN_START * FP * 12 / 10; // a breath before the first one
	grounded = true;
	dead = false;
	night = false;
	speed = RUN_START;
	score = 0;
	clear_obstacles();
	game_frame_set_score(&frame, 0);
	game_frame_set_hint(&frame, tr("dino_hint"));
}

// What comes next. A bird is held back until the run has some speed in it,
// because a bird at walking pace is a cactus that has not arrived yet, and a trio
// until the jump is comfortably long enough to clear thirteen units of it.
static int pick_type(void) {
	int birds = score > 150;
	int trios = score > 60;
	int n = 2 + birds + trios;
	int which = rand() % n;
	if (birds && which == n - 1) {
		return rand() & 1 ? OB_BIRD_UP : OB_BIRD_DOWN;
	}
	if (trios && which == n - 2) {
		return OB_TRIO;
	}
	return rand() & 1 ? OB_SMALL : OB_TALL;
}

static int pick_height(int type) {
	if (type != OB_BIRD_UP && type != OB_BIRD_DOWN) {
		return 0;
	}
	int which = rand() % 3;
	return which == 0 ? BIRD_LOW : which == 1 ? BIRD_MID : BIRD_HIGH;
}

static void spawn(void) {
	for (int i = 0; i < OB_MAX; i++) {
		if (ob_live[i]) {
			continue;
		}
		int type = pick_type();
		ob_live[i] = true;
		ob_type[i] = (int8_t)type;
		ob_height[i] = (int8_t)pick_height(type);
		ob_at[i] = run_fp + (int64_t)(frame.field_w / UNIT + 2) * FP; // just off the right edge
		return;
	}
}

// Two boxes in panel pixels. They are inset from the pictures on purpose: a
// cactus's arm and a bird's wing are not the part of it that would stop you, and
// a game that measures the pixel you were on against the pixel it was on is a
// game that is seen to be unfair rather than felt to be hard.
static void dino_box(int *x, int *y, int *w, int *h) {
	const sprite_t *s = dino_sprite();
	*x = DINO_X * UNIT + s->box[0] * UNIT;
	*y = dino_bottom() - (s->box[1] + s->box[3]) * UNIT;
	*w = s->box[2] * UNIT;
	*h = s->box[3] * UNIT;
}

static void ob_box(int i, int *x, int *y, int *w, int *h) {
	const sprite_t *s = &SPR_OB[ob_type[i]];
	*x = ob_x(i) + s->box[0] * UNIT;
	*y = ground_y - (ob_height[i] + s->box[1] + s->box[3]) * UNIT;
	*w = s->box[2] * UNIT;
	*h = s->box[3] * UNIT;
}

static bool crossed(void) {
	int ax, ay, aw, ah;
	dino_box(&ax, &ay, &aw, &ah);
	for (int i = 0; i < OB_MAX; i++) {
		if (!ob_live[i]) {
			continue;
		}
		int bx, by, bw, bh;
		ob_box(i, &bx, &by, &bw, &bh);
		if (ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// the card, the header, the page
// ---------------------------------------------------------------------------

static void show_card(void) {
	dead = true;
	running = false;
	game_frame_set_period(&frame, 0);
	game_frame_set_header_button_icon(&frame, 0, &icon_pause);
	draw_dino();
	game_frame_show_overlay(&frame, tr("game_over"), tr("dino_lost"), tr("game_play_again"), NULL);
}

static void start_run(void) {
	reset_run();
	game_frame_hide_overlay(&frame);
	running = true;
	game_frame_set_period(&frame, TICK_MS);
	game_frame_set_header_button_icon(&frame, 0, &icon_pause);
	draw_dino();
	draw_ground();
}

static void tick_cb(void) {
	if (!running) {
		return;
	}
	int64_t us = frame.tick_us;
	if (us < 0) {
		us = 0;
	}
	// Four and a half frames' worth, which is the most a run is allowed to
	// make up at once: long enough that the game keeps real time when the panel
	// is briefly busy, short enough that nothing can be jumped through rather
	// than over. (A panel that has been away for longer than a quarter of a
	// second has gone dark, and the frame has frozen the run before this tick
	// was ever asked.)
	if (us > 75000) {
		us = 75000;
	}

	if (us > 0) {
		run_fp += (int64_t)speed * FP * us / 1000000;
	}

	if (!grounded) {
		vy_fp -= (int64_t)GRAVITY * FP * us / 1000000;
		y_fp += vy_fp * us / 1000000;
		if (game_frame_held(&frame) && vy_fp < 0) {
			vy_fp -= (int64_t)GRAVITY * DUCK_FALL * FP * us / 1000000;
		}
		if (y_fp <= 0) {
			y_fp = 0;
			vy_fp = 0;
			grounded = true;
		}
	}

	until_next_fp -= (int64_t)speed * FP * us / 1000000;
	if (until_next_fp <= 0) {
		spawn();
		// The gap is a length of time rather than of ground, so a faster run
		// is harder because there is less to think in, and not also because
		// there is less room to land in.
		until_next_fp = (int64_t)speed * FP * (7 + rand() % 9) / 10;
	}

	for (int i = 0; i < OB_MAX; i++) {
		if (ob_live[i] && ob_x(i) < -SPR_OB[ob_type[i]].w * UNIT) {
			ob_live[i] = false;
		}
	}

	if (us > 0) {
		score = (int)((run_fp >> FP_BITS) / 10);
		if (score != frame.score) {
			game_frame_set_score(&frame, score);
		}
		if (((score / 100) & 1) != night) {
			night = (score / 100) & 1;
			recolour();
		}
		int faster = RUN_START + score / 3;
		speed = faster > RUN_MAX ? RUN_MAX : faster;
	}

	// What the dino is standing under is looked at whether or not the land
	// moved on this tick. A pose is part of the box that gets hit, so a
	// finger that comes down between two ticks has already changed it, and a
	// run that only notices on the next frame of ground is one where a duck is
	// seen to walk through a cactus.
	if (crossed()) {
		draw_ground();
		show_card();
		return;
	}

	draw_dino();
	draw_ground();
	draw_sky();
}

// A tap is a jump, from the ground only, and a finger that stays down is a duck
// or a faster fall. That is the whole control, and the whole of the game's depth:
// the same press is a hop over a cactus and a short hop over a low bird, and the
// difference is whether you let go.
static void tap_cb(const lv_point_t *p) {
	(void)p;
	if (!running || !grounded) {
		return;
	}
	vy_fp = (int64_t)JUMP_V0 * FP;
	grounded = false;
	draw_dino();
}

static void card_primary(void) { start_run(); }

static void pause_pressed(void) {
	if (frame.overlay_up) {
		return; // there is no run to hold
	}
	game_frame_freeze(&frame, !frame.frozen);
}

static void show_cb(void) {
	game_frame_load_best(&frame);
	layout();
	reset_run();
	running = false;
	game_frame_set_header_button_icon(&frame, 0, &icon_pause);
	// The dino stands on the card, waiting: the field is the game's own and
	// the card sits in it, so what is behind the words is the run about to
	// happen rather than a picture of one.
	draw_dino();
	draw_ground();
	draw_sky();
	game_frame_show_overlay(&frame, tr("dino"), tr("dino_ready"), tr("game_start"), NULL);
}

// Nothing is kept between visits beyond the best score, which the frame takes to
// the config: a run that was interrupted is a run that ended, and the only
// interesting number about it is how far it got.
static void hide_cb(void) {
	running = false;
	game_frame_set_period(&frame, 0);
}

static void frozen_cb(bool frozen) {
	game_frame_set_header_button_icon(&frame, 0, frozen ? &icon_play : &icon_pause);
	game_frame_set_hint(&frame, frozen ? tr("game_paused") : tr("dino_hint"));
}

static void theme_cb(void) { layout(); }

// A square of a field, and everything in it the game's own: no wells, no board,
// nothing that the player could mistake for a control. The pieces are made once
// and are moved and repainted for as long as the page lives.
// The page is built the first time it is opened, not at start-up. A game the player
// never opens costs nothing but the configuration it is handed, and a theme change
// never has to walk a board nobody can see. Once built it stays, as the other pages
// do, so the next visit is instant and the run is where it was left.
static gui_config_t *dino_cfg;

static void dino_build(gui_config_t *cfg) {
	static const game_hooks_t hooks = {
		.on_show = show_cb,
		.on_hide = hide_cb,
		.on_tick = tick_cb,
		.on_tap = tap_cb,
		.on_frozen = frozen_cb,
		.on_theme = theme_cb,
	};

	frame.hooks = &hooks;
	frame.section = SECTION;
	frame.title = tr("dino");
	dino_screen = lv_obj_create(NULL);
	frame.screen = dino_screen;
	frame.on_overlay_primary = card_primary;
	game_frame_build(&frame, cfg);

	sky = make_prop();
	ground = make_prop();
	lv_obj_set_style_bg_opa(ground, LV_OPA_60, 0);

	for (int i = 0; i < PIECES_MAX; i++) {
		dino_piece[i] = make_prop();
		lv_obj_set_hidden(dino_piece[i], true);
	}

	for (int i = 0; i < OB_MAX; i++) {
		for (int p = 0; p < 3; p++) {
			ob_piece[i][p] = make_prop();
			lv_obj_set_hidden(ob_piece[i][p], true);
		}
	}

	for (int i = 0; i < 3; i++) {
		cloud[i] = make_prop();
		lv_obj_set_style_radius(cloud[i], LV_RADIUS_CIRCLE, 0);
	}
	for (int i = 0; i < 4; i++) {
		pebble[i] = make_prop();
	}
	for (int i = 0; i < 5; i++) {
		star[i] = make_prop();
	}
	moon = lv_obj_create(frame.field);
	moon_bite = lv_obj_create(frame.field);
	lv_obj_remove_style_all(moon);
	lv_obj_remove_style_all(moon_bite);
	lv_obj_set_scrollable(moon, false);
	lv_obj_set_scrollable(moon_bite, false);
	lv_obj_set_clickable(moon, false);
	lv_obj_set_clickable(moon_bite, false);
	lv_obj_set_style_radius(moon, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_radius(moon_bite, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(moon, LV_OPA_70, 0);
	lv_obj_set_style_bg_opa(moon_bite, LV_OPA_COVER, 0); // the bite has to be a
	// piece of the sky laid over the moon, and half a bite is a moon with a
	// faint mouth rather than a crescent
	lv_obj_set_hidden(moon, true);
	lv_obj_set_hidden(moon_bite, true);

	game_frame_add_header_button(&frame, &icon_pause, pause_pressed);

	// The card is built before the game's own objects, so it has to be put back
	// on top of them.
	lv_obj_move_foreground(frame.overlay);

	layout();
}

void dino_init(gui_config_t *cfg) {
	dino_cfg = cfg;
}

void dino_open(void) {
	if (!dino_screen) {
		dino_build(dino_cfg);
	}
	srand((unsigned)time(NULL) ^ (unsigned)lv_tick_get());
	game_frame_open(&frame);
}
