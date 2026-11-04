#include "petpage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/pet/pikachu.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h" // switch_screen
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/device/clock.h"
#include "src/system/device/power.h"
#include "src/system/pet/pet.h"
#include "src/system/pet/petwatch.h"

lv_obj_t *petpage_screen;

// ---------------------------------------------------------------------------
// The shape of the page
//
// On the 240 x 320 panel there is room for one card and two lines of small
// text under it. The card is the animal's home: the name and the two meters in
// its top corners, the animal in the middle, what it is feeding on at the
// bottom. Everything is measured from the panel, so a wider player gets the
// same page with more air rather than a different one.
// ---------------------------------------------------------------------------

#define PET_CREATURE_W 140
// The box is the picture's height and a strip under its feet for the shadow.
// The root is placed from the feet, so the animal still stands on the floor
// line whatever this number is. The picture is drawn 1:1 and what goes over it
// is clipped to the picture, so a hat is drawn inside the picture's own rows:
// that is what the empty rows between the ears are for.
#define PET_CREATURE_H 126
#define PET_BASE_Y 120 // where the animal's feet sit, from the root's top

// The two meters' column, measured from the card's right edge.
#define PET_HUD_W 88

// The scenery's grid (see "The scenery"): three-pixel cells, 37 rows from y 65 on
// the card down to the line of words, and the Poke Ball's corner of it -- seven
// cells of ball and the room for a sparkle.
#define SCENE_CELL 3
#define SCENE_TOP 65
#define SCENE_ROWS 37
// The rows where the sky comes up out of the card, and the first row of ground.
#define SCENE_FADE 6
#define SCENE_HORIZON 21
#define BALL_COLS 9
#define BALL_ROWS 10

// What a sad pet cries. Everything else that is put on the picture is in the
// wardrobe's colours below or in the picture's own six, and none of it follows
// the theme: a yellow animal reads on a white card, a black card and Tokyo
// Night's blue-black alike.
#define PET_TEAR lv_color_hex(0x7FC4E8)

// The wardrobe's own colours, fixed for the same reason the animal's are.
#define PET_HAT lv_color_hex(0xE2574C)
#define PET_PHONE lv_color_hex(0x2F3B52)
#define PET_SCARF lv_color_hex(0xC2453F)
#define PET_CROWN lv_color_hex(0xE8C15A)
// What the genres ask for. One colour each, flat, as everything else here is:
// the shades are near-black, the chain is the crown's gold, a stud is a pale
// grey and the cup is cream with coffee in it.
#define PET_SHADE lv_color_hex(0x22282C)
#define PET_GOLD lv_color_hex(0xE8C15A)
#define PET_STUD lv_color_hex(0xC9CCD1)
#define PET_CUP lv_color_hex(0xF7F2E4)
#define PET_COFFEE lv_color_hex(0x6B4A32)
// The lenses of the shades are a mirror: a dark blue with the sky's blue in the
// lower half of it. A pet that is feeding has its mouth open, which is a dark red
// with a pink tongue in it, and sparks of electricity at its cheeks.
#define PET_LENS_DARK lv_color_hex(0x16305A)
#define PET_LENS_LIGHT lv_color_hex(0x2E67AE)
#define PET_MOUTH lv_color_hex(0x5A1C14)
#define PET_TONGUE lv_color_hex(0xF0877A)
#define PET_SPARK lv_color_hex(0xFFF27A)
// The black tip of the far ear is above the sky, on the card. The picture's black
// (252923, a dark grey) is as light as a dark card, so there the tip was not to be
// seen and the ear looked cut off; it is drawn in true black, which is darker than
// the card on every theme.
#define PET_TIP lv_color_hex(0x000000)
// The three things a month can make fall: they are over the scenery, so they
// are pale enough to be seen on the sky, the grass and the dark card alike.
#define PET_LEAF lv_color_hex(0xD9822B)
#define PET_SNOW lv_color_hex(0xF4F8FB)
#define PET_RAIN lv_color_hex(0xE3F0FF)

// The rooms, as a wash over the card. The scenery has the same three skies.
#define PET_ROOM_MEADOW lv_color_hex(0x9FD9B4)
#define PET_ROOM_SUNSET lv_color_hex(0xF0A868)
#define PET_ROOM_NIGHT lv_color_hex(0x46598C)

typedef struct {
	lv_obj_t *root;
	lv_obj_t *shadow;

	// The animal is one picture, and whatever he is wearing or feeling is drawn
	// over it by one more object (see "Over the picture"). Only the little
	// "z z" of a sleeper is a label of its own.
	lv_obj_t *sprite;
	lv_obj_t *gear;
	lv_obj_t *zzz;

	// The Poke Ball on the floor, and whatever it has turned up: a tap on the
	// ball opens the sheet, a tap on a find puts it on the shelf. The ball draws
	// itself (see "The Poke Ball").
	lv_obj_t *box;
	lv_obj_t *find;
	lv_obj_t *find_icon[PET_DROP_KINDS];

	// The little hearts: one that pops over a tapped head, and the three that
	// fill up under a held finger.
	lv_obj_t *heart;
	lv_obj_t *hold_heart[3];
} pet_parts_t;

static pet_parts_t parts;
static bool ball_ready; // something is in the ball: its button is gold, with a sparkle

static gui_config_t *page_cfg;
static bool page_built;
static bool page_visible;
static lv_timer_t *page_timer;

// The tiny answer to a button or a finger: the animal jumps, ducks, leans or
// waves for a couple of seconds, then goes back to breathing. A single value
// says which, and a single deadline says until when, because only one at a
// time can be seen anyway.
enum {
	REACT_NONE = 0,
	REACT_JUMP,
	REACT_DUCK,
	REACT_LEAN_L,
	REACT_LEAN_R,
	REACT_HEART,
	REACT_HOP,
	REACT_WAVE,
};
#define PET_REACTION_SECS 3
static int reaction;
static int64_t reaction_until;

// A little life between the breaths (see "A little life"): the seconds until his
// next blink and until he next moves of his own accord, the dice they are
// drawn with, and the blink's timer while his eyes are shut.
static int blink_wait;
static int fidget_wait;
static uint32_t dice = 20260;
static lv_timer_t *blink_timer;

// Petting Mode: the finger stayed down, and a heart meter fills in three steps
// of six hundred milliseconds. The last one pays the day's groom.
#define PET_HOLD_STEPS 3
#define PET_HOLD_STEP_MS 600
static bool holding;
static int hold_step;
static lv_timer_t *hold_timer;
static int64_t swallow_until; // a completed hold also reports a click
static bool hold_paid;       // the meter filled: the release is not a tap

static lv_obj_t *habitat;
// The card's dark after eight in the evening, and over it the route behind the
// animal, drawn in one piece.
static lv_obj_t *night_wash;
static lv_obj_t *scenery;
#define PET_FLAKES 5
static lv_obj_t *flake[PET_FLAKES];
static int flake_home_x[PET_FLAKES];
static int flake_step;
static bool clock_layer_built;
static lv_obj_t *name_label;
static lv_obj_t *stage_label;
static lv_obj_t *status_icon; // the still note, when nothing is playing
static lv_obj_t *eq_bar[3];    // the three bars that are the equalizer
static int eq_bottom;
static lv_obj_t *status_label;
static lv_obj_t *health_icon;
static lv_obj_t *energy_icon;
static lv_obj_t *health_bar;
static lv_obj_t *energy_bar;
static lv_obj_t *health_value;
static lv_obj_t *energy_value;
static lv_obj_t *notes_icon;
static lv_obj_t *notes_value;
static lv_obj_t *card_close_icon;
static lv_obj_t *today_label;
static lv_obj_t *streak_label;

// The room's wash over the card, under everything else in it.
static lv_obj_t *room_layer;

// The drifting notes, while something is playing.
static lv_obj_t *note_a;
static lv_obj_t *note_b;

// The wardrobe and the name dialog: sheets over this page rather than screens
// of their own, so the shell's back chevron closes them in turn.
static lv_obj_t *wardrobe_screen;
static lv_obj_t *wardrobe_column;
static lv_obj_t *wardrobe_balance;
static lv_obj_t *wardrobe_value[PET_ITEM_COUNT];
static bool wardrobe_visible;

static lv_obj_t *customise_screen;
static lv_obj_t *customise_name_value;
static bool customise_visible;

// The month's listening, on the card under the stage.
static lv_obj_t *temperament_label;

// The Calm soundscape: four motes drifting up while the month says calm.
static lv_obj_t *motes[4];

// The card's own measurements, kept so the animal can be put on the floor line
// the homes are drawn on rather than wherever a container happens to lay
// it out.
static int card_w_px;
static int card_h_px;

// The tap-along, once a day.
static lv_obj_t *tap_sheet;
static lv_obj_t *tap_lane;
static lv_obj_t *tap_note;
static lv_obj_t *tap_score_label;
static lv_obj_t *tap_hint_label;
static lv_timer_t *tap_timer;
static uint32_t tap_start_ms;
static uint32_t tap_beat_ms;
static int tap_next_beat;
static int tap_points;
static bool tap_visible;

static lv_obj_t *name_layer;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

// Forward: the timer and the unload path close sheets whose builders are
// further down the file.
static void consume_events(void);
static void paint_clock(void);
static void wardrobe_open_cb(lv_event_t *e);
static void wardrobe_refresh(void);
static void wardrobe_close(void);
static void customise_open(void);
static void customise_close(void);
static void customise_refresh(void);
static void customise_close_cb(lv_event_t *e);
static void name_dialog_close(void);
static void pet_tap_cb(lv_event_t *e);
static void pet_hold_cb(lv_event_t *e);
static void start_reaction(int what);
static void life_tick(void);
static void blink_stop(void);
static void box_open_cb(lv_event_t *e);
static void ball_draw_cb(lv_event_t *e);
static void find_take_cb(lv_event_t *e);
static void box_close(void);
static void treat_close(void);
static void end_reaction(void);
static void hide_hold_meter(void);
static void tap_close(void);
static void tap_open(void);

// ---------------------------------------------------------------------------
// The small pieces everything is made of
// ---------------------------------------------------------------------------

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_clickable(o, false);
	lv_obj_set_scrollable(o, false);
	return o;
}

static lv_obj_t *disc(lv_obj_t *parent, lv_color_t color) {
	lv_obj_t *o = plain(parent);
	lv_obj_set_style_bg_color(o, color, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
	return o;
}

static lv_obj_t *bar_piece(lv_obj_t *parent, lv_color_t color, int radius) {
	lv_obj_t *o = plain(parent);
	lv_obj_set_style_bg_color(o, color, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, radius, 0);
	return o;
}

static void place(lv_obj_t *o, int x, int y, int w, int h) {
	if (!o) {
		return;
	}
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
}

static void show(lv_obj_t *o, bool visible) {
	if (o) {
		lv_obj_set_hidden(o, !visible);
	}
}

// An icon from the shell's set, drawn at `size` whatever the art was cut at.
static lv_obj_t *icon_image(lv_obj_t *parent, const lv_image_dsc_t *source, int size) {
	lv_obj_t *img = lv_image_create(parent);
	lv_image_set_src(img, source);
	lv_image_set_scale(img, (uint32_t)((int64_t)LV_SCALE_NONE * size / (int)source->header.h));
	lv_obj_set_size(img, size, size);
	lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
	return img;
}

// ---------------------------------------------------------------------------
// Over the picture
//
// The animal is one picture (pikachu.c), drawn exactly as it is. Everything
// that can be on him besides -- a hat, shades, a scarf, a tear, a heart -- is
// drawn over it by one more object, in the picture's own grid: a stamp is a few
// rows of letters, a letter is one cell, and a run of one letter is one filled
// rectangle. The cells are as big as the picture's, so a stamp lands on the
// picture the way a sticker lands on graph paper, and nothing is ever turned,
// scaled or smoothed.
//
// What is stamped, and where, is decided when the animal is painted; the
// callback only draws that list. A stamp can also have a second place on the
// animation's other frame, which is how the stars turn and the hearts float.
// ---------------------------------------------------------------------------

typedef struct {
	const char *const *rows; // one string a row; '.' leaves the picture as it is
	uint8_t w;
	uint8_t h;
} stamp_t;

// The stamps are drawn out letter by letter, so the formatter is kept off them.
// clang-format off
#define STAMP(name, width, ...)                                                                                    \
	static const char *const name##_rows[] = {__VA_ARGS__};                                                        \
	static const stamp_t name = {name##_rows, (width), (uint8_t)(sizeof(name##_rows) / sizeof(name##_rows[0]))}

// The letters are the picture's own (K ink, Y yellow, O orange, R red, N brown,
// W white) and the wardrobe's: r hat, g gold, p phones, s scarf, c shades and
// collar, l and L the lenses, b tear, t stud and glint, C cup, E coffee, m and T
// the inside of an open mouth and the tongue, z a spark and q the tip of an ear.
// A sleeping pet's eyes: lids over the open ones, one dark line each (the
// only place the picture itself is changed).
STAMP(stamp_eyes_shut, 4,
      ".YY.",
      "YYYY",
      "KKKK",
      ".YY.");

// The sweat drop of a sad pet, beside the head.
STAMP(stamp_drop, 4,
      ".K..",
      "KbK.",
      "KbK.",
      "KbbK",
      "KbbK",
      ".KK.");

// A dizzy pet's stars, in the crown's gold.
STAMP(stamp_star, 3,
      ".g.",
      "ggg",
      ".g.");

// A blissful pet's hearts, in the cheeks' red.
STAMP(stamp_heart, 5,
      ".R.R.",
      "RRRRR",
      ".RRR.",
      "..R..");
STAMP(stamp_heart_small, 3,
      "R.R",
      "RRR",
      ".R.");

// A party hat between the ears, base on the head's top line.
STAMP(stamp_hat, 9,
      "....g....",
      "...KrK...",
      "...KrK...",
      "..KrrrK..",
      "..KrrrK..",
      ".KrrrrrK.",
      "KKKKKKKKK");

// A crown on the same line. It is seven cells wide, where the hat is nine: at the
// grown size the wallet's note, in the corner of the card, stands over the right
// of the gap between the ears, and a crown nine wide covered a quarter of it.
STAMP(stamp_crown, 7,
      "K.K.K.K",
      "KgKgKgK",
      "KgggggK",
      "KgggggK",
      "KKKKKKK");

// Headphones: the band over the head, then a cup on each side.
STAMP(stamp_phones_band, 21,
      "......ppppppppp......",
      "...ppp.........ppp...",
      ".pp...............pp.",
      "p...................p");
STAMP(stamp_phone_l, 4,
      ".ppp",
      "pppp",
      "pppp",
      "pppp",
      "pppp",
      ".ppp");
STAMP(stamp_phone_r, 4,
      "ppp.",
      "pppp",
      "pppp",
      "pppp",
      "pppp",
      "ppp.");

// A scarf round the neck, with its end hanging over the chest.
STAMP(stamp_scarf, 19,
      "sssssssssssssssssss",
      "sssssssssssssssssss",
      ".............ssss..",
      ".............ssss..",
      ".............ssss..",
      ".............ssss..",
      "..............ss...");

// Shades, in one piece: two mirror lenses -- dark blue, with a lighter blue under
// a white glint that runs from the top right corner to the bottom left one --
// in a black frame, the bridge between them and an arm to each side that reaches
// the head's outline. The lenses are big enough to cover the eyes of every face.
STAMP(stamp_shades, 17,
      ".cccccc....cccccc",
      "ccllWlccccccllWlc",
      ".clWllc....clWllc",
      ".cWLLLc....cWLLLc",
      "..cccc......cccc.");
// The glint of a lens when it catches the light: the white cell grows a cross.
STAMP(stamp_shine, 3,
      ".W.",
      "WWW",
      ".W.");

// Gold links at the neck. No dark-ringed pendant: it looked like a hole.
STAMP(stamp_chain, 18,
      "KgK............KgK",
      "...KgKg....gKgK...",
      ".......gggg.......");

// A studded collar.
STAMP(stamp_collar, 19,
      "ccccccccccccccccccc",
      "ctccctccctccctccctc");

// A cup of coffee on the floor, outlined in coffee so it shows on any card.
STAMP(stamp_cup, 6,
      "EEEEE.",
      "ECCCEE",
      "ECCCEE",
      "ECCCE.",
      ".EEE..");

// The face of a pet that is feeding: eyes round and wide, a glint at the top left
// of each -- they cover the picture's own -- and a mouth open on a pink tongue.
STAMP(stamp_gasp_eye, 5,
      ".KKK.",
      "KWKKK",
      "KKKKK",
      "KKKKK",
      ".KKK.");
// The same eyes shut, for a blink.
STAMP(stamp_gasp_lid, 5,
      ".YYY.",
      "YYYYY",
      "YYYYY",
      "KKKKK",
      ".YYY.");
// A happy, open smile with raised corners and a pink tongue, like the
// reference. A compact smile fits above a scarf or collar without a stray lip.
STAMP(stamp_happy_mouth, 8,
      "KYYKKYYK",
      "YKKmmKKY",
      "YYKmmKYY",
      "YYKTTKYY",
      "YYYKKYYY");
STAMP(stamp_happy_mouth_small, 8,
      "KYYKKYYK",
      "YKKTTKKY");

// The electricity at the cheeks of a pet that is feeding: a bolt, then a spark.
STAMP(stamp_bolt, 3,
      "..z",
      ".z.",
      "zzz",
      ".z.",
      "z..");
STAMP(stamp_spark, 3,
      ".z.",
      "zzz",
      ".z.");

// The tip of the far ear, over the picture's own: ink cells in the true black
// that shows on a dark card. It is the one place the picture is above the sky.
STAMP(stamp_tip, 8,
      "......qq",
      "....qqqq",
      "..qqqqqq",
      ".q...qqq",
      "q....qq.");
// clang-format on

#define GEAR_MAX 20 // a face's stamps, a mood's, a garment's and a genre's, at the most

// Which of the two frames a stamp is drawn on.
enum {
	GEAR_ALWAYS = 0,
	GEAR_FIRST,
	GEAR_SECOND,
};

// What the eyes are, so a blink knows what to shut: nothing (they are shut
// already), the picture's own, the wide eyes of a feeding pet, or the lenses of
// a pair of shades, which cannot shut and glint instead.
enum {
	GEAR_EYES_NONE = 0,
	GEAR_EYES_PLAIN,
	GEAR_EYES_WIDE,
	GEAR_EYES_SHADED,
	GEAR_EYES_KINDS,
};

typedef struct {
	const stamp_t *stamp;
	int8_t col; // the cell of the picture its top left corner lands on
	int8_t row;
	int8_t dcol; // and how far it has moved on the animation's other frame
	int8_t drow;
	uint8_t when; // GEAR_ALWAYS, or the one frame it is drawn on
} gear_item_t;

static struct {
	gear_item_t item[GEAR_MAX];
	int count;
	int k;      // pixels a cell is drawn at: 2 for a hatchling, 3 for the rest
	bool moves; // some stamp has a second place, so the other frame redraws it
	bool other; // the animation is on its second frame
	int eyes;   // GEAR_EYES_*
	bool lids;  // a blink: the eyes are shut, or the lenses are bright
} gear;

// Whether the animal as it was last drawn is feeding -- music is audible and he is
// awake -- which is what dances, and what changes his face.
static bool feeding_now;

static void gear_clear(int k) {
	gear.count = 0;
	gear.k = k;
	gear.moves = false;
	gear.eyes = GEAR_EYES_PLAIN;
}

static void gear_add(const stamp_t *stamp, int col, int row) {
	if (gear.count < GEAR_MAX) {
		gear.item[gear.count++] = (gear_item_t){stamp, (int8_t)col, (int8_t)row, 0, 0, GEAR_ALWAYS};
	}
}

// One that is somewhere else on the other frame: it moves by (dcol, drow) cells.
static void gear_add_moving(const stamp_t *stamp, int col, int row, int dcol, int drow) {
	if (gear.count < GEAR_MAX) {
		gear.item[gear.count++] = (gear_item_t){stamp, (int8_t)col, (int8_t)row, (int8_t)dcol, (int8_t)drow, GEAR_ALWAYS};
		gear.moves = true;
	}
}

// One that is there on one of the frames and gone on the other.
static void gear_add_on(const stamp_t *stamp, int col, int row, int when) {
	if (gear.count < GEAR_MAX) {
		gear.item[gear.count++] = (gear_item_t){stamp, (int8_t)col, (int8_t)row, 0, 0, (uint8_t)when};
		gear.moves = true;
	}
}

static lv_color_t stamp_color(char letter) {
	switch (letter) {
	case 'K':
		return lv_color_hex(PIKACHU_INK);
	case 'Y':
		return lv_color_hex(PIKACHU_YELLOW);
	case 'O':
		return lv_color_hex(PIKACHU_ORANGE);
	case 'R':
		return lv_color_hex(PIKACHU_RED);
	case 'N':
		return lv_color_hex(PIKACHU_BROWN);
	case 'W':
		return lv_color_hex(PIKACHU_WHITE);
	case 'r':
		return PET_HAT;
	case 'g':
		return PET_GOLD;
	case 'p':
		return PET_PHONE;
	case 's':
		return PET_SCARF;
	case 'c':
		return PET_SHADE;
	case 'b':
		return PET_TEAR;
	case 't':
		return PET_STUD;
	case 'C':
		return PET_CUP;
	case 'E':
		return PET_COFFEE;
	case 'l':
		return PET_LENS_DARK;
	case 'L':
		return PET_LENS_LIGHT;
	case 'm':
		return PET_MOUTH;
	case 'T':
		return PET_TONGUE;
	case 'z':
		return PET_SPARK;
	case 'q':
		return PET_TIP;
	default:
		return lv_color_hex(0xFF00FF); // a letter nobody drew: loud on purpose
	}
}

// A stamp drawn with its top left corner at pixel (x, y), a cell being `cell`
// pixels square, in the colour `color` makes of each letter. A run of one letter
// is one rectangle, however many cells wide, and a '.' leaves what is there.
static void stamp_draw(lv_layer_t *layer, lv_draw_rect_dsc_t *rect, const stamp_t *stamp, int x, int y, int cell, lv_color_t (*color)(char)) {
	for (int row = 0; row < stamp->h; row++) {
		const char *line = stamp->rows[row];
		for (int col = 0; col < stamp->w && line[col];) {
			int run = 1;
			while (col + run < stamp->w && line[col + run] == line[col]) {
				run++;
			}
			if (line[col] != '.') {
				rect->bg_color = color(line[col]);
				lv_area_t cells;
				cells.x1 = x + col * cell;
				cells.y1 = y + row * cell;
				cells.x2 = x + (col + run) * cell - 1;
				cells.y2 = cells.y1 + cell - 1;
				lv_draw_rect(layer, rect, &cells);
			}
			col += run;
		}
	}
}

static void gear_draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t box;
	lv_obj_get_coords(parts.gear, &box);

	lv_draw_rect_dsc_t rect;
	lv_draw_rect_dsc_init(&rect);
	rect.bg_opa = LV_OPA_COVER;

	const int k = gear.k;
	for (int i = 0; i < gear.count; i++) {
		const gear_item_t *item = &gear.item[i];
		if ((item->when == GEAR_FIRST && gear.other) || (item->when == GEAR_SECOND && !gear.other)) {
			continue;
		}
		const int col = item->col + (gear.other ? item->dcol : 0);
		const int row = item->row + (gear.other ? item->drow : 0);
		stamp_draw(layer, &rect, item->stamp, box.x1 + col * k, box.y1 + row * k, k, stamp_color);
	}

	// A blink is over all of it, so it is drawn last: a lid on each of the eyes
	// there are, and a cross of light on each lens when there are shades, which
	// are what is over the eyes then.
	if (gear.lids && gear.eyes > GEAR_EYES_NONE && gear.eyes < GEAR_EYES_KINDS) {
		static const struct {
			const stamp_t *lid;
			int8_t left; // the cell of the left one's top left corner, and the right one's
			int8_t right;
			int8_t row;
		} lids[GEAR_EYES_KINDS] = {
			[GEAR_EYES_PLAIN] = {&stamp_eyes_shut, 15, 25, 12},
			[GEAR_EYES_WIDE] = {&stamp_gasp_lid, 15, 24, 11},
			[GEAR_EYES_SHADED] = {&stamp_shine, 16, 26, 11},
		};
		const stamp_t *lid = lids[gear.eyes].lid;
		stamp_draw(layer, &rect, lid, box.x1 + lids[gear.eyes].left * k, box.y1 + lids[gear.eyes].row * k, k, stamp_color);
		stamp_draw(layer, &rect, lid, box.x1 + lids[gear.eyes].right * k, box.y1 + lids[gear.eyes].row * k, k, stamp_color);
	}
}

static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(petpage_screen);
	settingsrow_place_corner_button(button, cfg, slot);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	// The glyph is drawn at the slot's size rather than scaled from its source:
	// the shell's icons are cut at anything from 14 to 128 pixels, and
	// settingsrow_scale_corner_icon() assumes the 34 px ones.
	lv_obj_t *icon = icon_image(button, glyph, settingsrow_corner_button_size(cfg) * 5 / 9);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return button;
}

// The card's own way out, bottom right: a page that fills the panel should not
// leave the chevron as the only door.
static void card_close_cb(lv_event_t *e) {
	(void)e;
	back_btn_cb(NULL);
}

// ---------------------------------------------------------------------------
// Building the animal
//
// One picture, one layer over it, a shadow, the ball and the little hearts, made
// back to front: the layer is over the picture and the hearts are over all of it.
// ---------------------------------------------------------------------------

// A flat coloured piece with its corners left for the caller to set: what the
// finds are drawn from. `disc` is the round one and `bar_piece` the squared one.
static lv_obj_t *round_piece(lv_obj_t *parent, lv_color_t color) {
	lv_obj_t *o = plain(parent);
	lv_obj_set_style_bg_color(o, color, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	return o;
}

// ---------------------------------------------------------------------------
// What the floor turns up
//
// Four finds, drawn flat out of a few rounded pieces, at whatever size the
// caller has room for: fourteen pixels in the corner of the card, twenty in a
// cell of the shelf. The fractions are of that size, so one drawing does both.
// ---------------------------------------------------------------------------

static lv_obj_t *drop_piece(lv_obj_t *parent, lv_color_t color, int x, int y, int w, int h, int size, bool round) {
	lv_obj_t *o = round_piece(parent, color);
	lv_obj_set_size(o, w * size / 20, h * size / 20);
	lv_obj_set_pos(o, x * size / 20, y * size / 20);
	lv_obj_set_style_radius(o, round ? LV_RADIUS_CIRCLE : 2, 0);
	return o;
}

static lv_obj_t *drop_icon(lv_obj_t *parent, int kind, int size) {
	lv_obj_t *box = plain(parent);
	lv_obj_set_size(box, size, size);

	switch (kind) {
	case PET_DROP_NOTE: {
		// A gold music note, both on the floor and on the shelf.
		drop_piece(box, PET_GOLD, 8, 3, 3, 11, size, false);
		drop_piece(box, PET_GOLD, 8, 3, 8, 3, size, false);
		drop_piece(box, PET_GOLD, 4, 12, 7, 5, size, true);
		break;
	}
	case PET_DROP_CASSETTE: {
		drop_piece(box, lv_color_hex(0x5A6470), 2, 5, 16, 10, size, false);
		drop_piece(box, lv_color_hex(0xEDEFF2), 4, 7, 12, 3, size, false);
		drop_piece(box, lv_color_hex(0xEDEFF2), 6, 11, 3, 3, size, true);
		drop_piece(box, lv_color_hex(0xEDEFF2), 11, 11, 3, 3, size, true);
		break;
	}
	case PET_DROP_STICKER: {
		drop_piece(box, PET_CROWN, 2, 2, 16, 16, size, true);
		drop_piece(box, PET_HAT, 7, 7, 6, 6, size, true);
		break;
	}
	case PET_DROP_VINYL:
	default: {
		drop_piece(box, PET_SHADE, 2, 2, 16, 16, size, true);
		drop_piece(box, PET_GOLD, 7, 7, 6, 6, size, true);
		drop_piece(box, lv_color_black(), 9, 9, 2, 2, size, true);
		break;
	}
	}
	return box;
}

static void build_creature(void) {
	memset(&parts, 0, sizeof(parts));

	parts.root = plain(habitat);
	lv_obj_set_size(parts.root, PET_CREATURE_W, PET_CREATURE_H);
	lv_obj_set_pos(parts.root, (card_w_px - PET_CREATURE_W) / 2, card_h_px - 42 - PET_BASE_Y + 2);
	lv_obj_set_clickable(parts.root, true);
	lv_obj_add_event_cb(parts.root, pet_tap_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(parts.root, pet_hold_cb, LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_add_event_cb(parts.root, pet_hold_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(parts.root, pet_hold_cb, LV_EVENT_PRESS_LOST, NULL);

	parts.shadow = disc(parts.root, lv_color_black());

	// The ball and the find are on the floor of the room, not on the animal, so
	// they are made before it and never move with it.
	parts.box = plain(parts.root);
	lv_obj_set_clickable(parts.box, true);
	lv_obj_add_event_cb(parts.box, box_open_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(parts.box, ball_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	parts.find = plain(parts.root);
	lv_obj_set_clickable(parts.find, true);
	lv_obj_add_event_cb(parts.find, find_take_cb, LV_EVENT_CLICKED, NULL);
	for (int i = 0; i < PET_DROP_KINDS; i++) {
		parts.find_icon[i] = drop_icon(parts.find, i, 16);
	}

	// ---- the animal ------------------------------------------------------
	//
	// The picture, and over it the one object that draws what he wears and
	// feels. The picture is a plain image blitted 1:1 from one of two sizes cut
	// for the purpose: LVGL's own scaler samples between pixels, which is fine
	// for a photograph and ruinous for pixel art.
	parts.sprite = lv_image_create(parts.root);
	lv_obj_set_clickable(parts.sprite, false);
	lv_image_set_src(parts.sprite, &pikachu_3x);
	parts.gear = plain(parts.root);
	lv_obj_add_event_cb(parts.gear, gear_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

	parts.zzz = lv_label_create(parts.root);
	lv_obj_add_style(parts.zzz, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(parts.zzz, &font_ui_14, 0);
	lv_label_set_text(parts.zzz, "z z");

	// Last, so they are over everything: one heart that pops over a tapped
	// head, three small ones that fill the Petting Mode meter. It is the same
	// heart the health row wears, which is the point -- a heart means a heart --
	// in the red of the cheeks.
	parts.heart = icon_image(parts.root, &icon_heart_filled, 16);
	lv_obj_set_style_image_recolor(parts.heart, lv_color_hex(PIKACHU_RED), 0);
	show(parts.heart, false);
	for (int i = 0; i < 3; i++) {
		parts.hold_heart[i] = icon_image(parts.root, &icon_heart_filled, 10);
		lv_obj_set_style_image_recolor(parts.hold_heart[i], lv_color_hex(PIKACHU_RED), 0);
		show(parts.hold_heart[i], false);
	}
}

// ---------------------------------------------------------------------------
// The animal, drawn
// ---------------------------------------------------------------------------

static void set_color(lv_obj_t *o, lv_color_t color) {
	if (o) {
		lv_obj_set_style_bg_color(o, color, 0);
	}
}

static void paint_creature(void) {
	if (!page_built) {
		return;
	}

	const pet_state_t *st = petwatch_state();
	const pet_stage_t stage = pet_stage(st);
	const pet_mood_t mood = pet_mood(st, petwatch_now());

	// One picture, two sizes: a hatchling is drawn at two pixels a cell and
	// everybody else at three, so the same animal is small and round to begin
	// with and has room to stand up in later. The picture is as wide as the
	// tail and the far ear make it, so it is the whole of it that is centred.
	const int k = stage == PET_STAGE_BABY ? 2 : 3;
	const int sw = PIKACHU_COLS * k;
	const int sh = PIKACHU_ROWS * k;
	const int sx = (PET_CREATURE_W - sw) / 2;
	const int sy = PET_BASE_Y - sh;
	// The face's axis falls between the 21st cell and the 22nd.
	const int axis = sx + 22 * k;

	lv_image_set_src(parts.sprite, k == 2 ? &pikachu_2x : &pikachu_3x);
	lv_obj_set_pos(parts.sprite, sx, sy);
	place(parts.gear, sx, sy, sw, sh);

	// The shadow, under the feet: they span the 12th cell to the 30th.
	place(parts.shadow, sx + 11 * k, PET_BASE_Y - 1, 21 * k, k + 2);
	lv_obj_set_style_bg_opa(parts.shadow, theme_is_dark() ? (lv_opa_t)(LV_OPA_COVER * 35 / 100) : LV_OPA_20, 0);

	// ---- his face --------------------------------------------------------
	//
	// An awake listener smiles; shades still cover the eyes and a compact
	// smile stays clear of neckwear. Sleep keeps the picture's sleeping face.
	const pet_genre_look_t look = petwatch_look();
	const bool shades = look == PET_LOOK_SHADES;
	const bool neck = pet_wearing(st) == (int)PET_ITEM_SCARF || look == PET_LOOK_COLLAR;
	const bool feeding = petwatch_listening() && mood != PET_MOOD_SLEEPING;
	feeding_now = feeding;

	gear_clear(k);
	gear.eyes = mood == PET_MOOD_SLEEPING ? GEAR_EYES_NONE : shades ? GEAR_EYES_SHADED : feeding ? GEAR_EYES_WIDE : GEAR_EYES_PLAIN;
	if (k == 3 && theme_is_dark()) {
		gear_add(&stamp_tip, 29, 0);
	}
	if (feeding) {
		if (!shades) {
			gear_add(&stamp_gasp_eye, 15, 11);
			gear_add(&stamp_gasp_eye, 24, 11);
		}
		if (neck) {
			gear_add(&stamp_happy_mouth_small, 18, 18);
		} else {
			gear_add(&stamp_happy_mouth, 18, 18);
		}
		// A spark on each side, a bolt that is a spark on the other frame, over the
		// sky beside the head and the tail. The sweat drop of a sad pet has the
		// right side.
		gear_add_on(&stamp_spark, 5, 13, GEAR_FIRST);
		gear_add_on(&stamp_bolt, 5, 11, GEAR_SECOND);
		if (mood != PET_MOOD_SAD) {
			gear_add_on(&stamp_bolt, 34, 12, GEAR_FIRST);
			gear_add_on(&stamp_spark, 34, 14, GEAR_SECOND);
		}
	}

	// ---- what he feels ---------------------------------------------------
	//
	// A mood is something added next to the face: a sweat drop, stars, hearts.
	// Only a sleeper changes the picture, by shutting its eyes.
	switch (mood) {
	case PET_MOOD_SLEEPING:
		gear_add(&stamp_eyes_shut, 15, 12);
		gear_add(&stamp_eyes_shut, 25, 12);
		break;
	case PET_MOOD_SAD:
		gear_add(&stamp_drop, 33, 12);
		break;
	case PET_MOOD_DIZZY:
		// Two drift over the head. The third stays in the notch between the head
		// and the far ear, which is clear of the wallet's note in the corner of
		// the card when the animal is big enough to reach it.
		gear_add_moving(&stamp_star, 14, 1, 1, 1);
		gear_add_moving(&stamp_star, 19, 0, 0, 1);
		gear_add(&stamp_star, 24, 4);
		break;
	case PET_MOOD_BLISSFUL:
		gear_add(&stamp_heart, 9, 0);
		gear_add_moving(&stamp_heart_small, 3, 1, 0, -1);
		break;
	default:
		break;
	}

	// The "z z" goes above the left ear, which is the one corner of the box
	// with nothing in it.
	place(parts.zzz, sx + 10 * k, sy > 6 ? sy - 6 : 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	show(parts.zzz, mood == PET_MOOD_SLEEPING);

	// ---- the wardrobe ----------------------------------------------------
	//
	// One garment at a time. The hat and the crown stand between the ears and
	// leave them where they are; the scarf owns the neck.
	const int wearing = pet_wearing(st);
	const bool scarf = wearing == (int)PET_ITEM_SCARF;
	if (wearing == (int)PET_ITEM_PARTY_HAT) {
		gear_add(&stamp_hat, 17, 0);
	} else if (wearing == (int)PET_ITEM_CROWN) {
		gear_add(&stamp_crown, 17, 2);
	} else if (wearing == (int)PET_ITEM_HEADPHONES) {
		gear_add(&stamp_phones_band, 11, 7);
		gear_add(&stamp_phone_l, 8, 10);
		gear_add(&stamp_phone_r, 30, 10);
	} else if (scarf) {
		gear_add(&stamp_scarf, 12, 20);
	}

	// ---- the genre's accessory -------------------------------------------
	//
	// One thing at a time, and only while there is music to have a genre from.
	// A scarf owns the neck, so the chain and the collar give way to it; the
	// shades sit under a hat, which is what sunglasses do.
	if (shades) {
		gear_add(&stamp_shades, 13, 11);
	} else if (look == PET_LOOK_CHAIN && !scarf) {
		gear_add(&stamp_chain, 13, 23);
	} else if (look == PET_LOOK_COLLAR && !scarf) {
		gear_add(&stamp_collar, 12, 20);
	} else if (look == PET_LOOK_CUP) {
		// On the floor beside the tail, where a cup would be.
		gear_add(&stamp_cup, 3, 35);
	}
	lv_obj_invalidate(parts.gear);

	// ---- the ball on the floor -------------------------------------------
	//
	// It lies in the corner of the card all day, and its button is gold, with a
	// sparkle at its shoulder, when there is something in it: today's fortune,
	// an hour's game, or what the listening turned up. The object is the ball's
	// own cells and the room for the sparkle, three pixels to a cell; it ends on
	// the floor line, a row below the animal's feet.
	const petwatch_box_t box = petwatch_box();
	const int find_x = PET_CREATURE_W - 22;
	place(parts.box, 1, PET_BASE_Y - 27, BALL_COLS * SCENE_CELL, BALL_ROWS * SCENE_CELL);
	const bool ready = box.fortune_ready || box.drops > 0 || box.treats_ready;
	if (ball_ready != ready) {
		ball_ready = ready;
		lv_obj_invalidate(parts.box);
	}

	int shelf_total = 0;
	for (int i = 0; i < PET_DROP_COUNT; i++) {
		shelf_total += box.found[i];
	}
	const int pending_kind = shelf_total % PET_DROP_KINDS;
	place(parts.find, find_x, PET_BASE_Y - 16, 16, 16);
	show(parts.find, box.drops > 0);
	for (int i = 0; i < PET_DROP_KINDS; i++) {
		show(parts.find_icon[i], box.drops > 0 && i == pending_kind);
	}

	// The hearts float between the ears, where the picture has room. Whether
	// they can be seen is the reaction's business, not the paint's.
	const int heart_y = sy > 4 ? sy - 4 : 0;
	place(parts.heart, axis - 8, heart_y, 16, 16);
	for (int i = 0; i < 3; i++) {
		place(parts.hold_heart[i], axis - 17 + i * 12, heart_y + 3, 10, 10);
	}
}

// ---------------------------------------------------------------------------
// The scenery
//
// Behind the animal is a stretch of a route, the sort a trainer walks: a sky
// that comes up out of the card, hills, a round tree and a pine, tall grass,
// flowers, and a platform for the animal to stand on. It is one object with a
// draw callback and a handful of stamps -- the same graph paper as what the
// animal wears -- so it costs one object where fifty rectangles would have cost
// fifty, and nothing in it is scaled or smoothed.
//
// The cells are three pixels, as a grown animal's are, and the rows are counted
// from the floor: the feet stand on the top of row 35. The card is 228 pixels
// across on the V1, which is 76 cells. What is placed from the right is placed
// from the right edge, so a wider card gets more meadow in the middle.
//
// What is in the sky is up to the room and the clock (see paint_clock): the sun
// by day, a low orange one in the sunset room, the moon and the stars at night
// and in the night room, grey cloud and no sun when it rains. The hills, the
// ground and the trees take a tint of the same light.
//
// It is in colours of its own, the same on every theme: a hill is not going to
// be Tokyo Night's blue. The one place it meets the theme is the top, where the
// sky is drawn in thin over the card, which is what lets the name and the
// meters above it sit on the card's own colour.
// ---------------------------------------------------------------------------

// clang-format off
// Clouds: a small one, a big one with a tail, a middle one, and the one that rains.
STAMP(scene_cloud_a, 8,
	"..ww....",
	".wwwww..",
	"wwwwwwww",
	".vvvvvv.");
STAMP(scene_cloud_b, 13,
	"....ww.......",
	"...wwww.ww...",
	".wwwwwwwwww..",
	"wwwwwwwwwwwww",
	".vvvvvvvvvvv.");
STAMP(scene_cloud_c, 7,
	"..www..",
	".wwwww.",
	"wwwwwww",
	".vvvvv.");
STAMP(scene_cloud_rain, 14,
	"....www.......",
	"..wwwwwww.ww..",
	".wwwwwwwwwwww.",
	"wwwwwwwwwwwwww",
	".vvvvvvvvvvvv.");
STAMP(scene_sun, 7,
	"..SSS..",
	".SssSS.",
	"SsSSSSS",
	"SSSSSSS",
	"SSSSSSS",
	".SSSSS.",
	"..SSS..");
STAMP(scene_moon, 7,
	"..MMM..",
	".MMM...",
	"MMM....",
	"MMM....",
	"MMM....",
	".MMM...",
	"..MMM..");
STAMP(scene_star, 3,
	".z.",
	"zzz",
	".z.");
STAMP(scene_tree, 11,
	"...TTTTT...",
	"..TcccbbT..",
	".TcccbbbbT.",
	"TccbbbbbbaT",
	"TcbbbbbbaaT",
	"TbbbbbbbaaT",
	".TbbbbbaaT.",
	"..TaaaaaT..",
	"...TTTTT...",
	"....onn....",
	"....onn....",
	"....onn....");
STAMP(scene_pine, 9,
	"....T....",
	"...TcT...",
	"..TcbaT..",
	".TcbbbaT.",
	"..TbbaT..",
	".TcbbbaT.",
	"TcbbbbbaT",
	".TbbbbaT.",
	"TcbbbbbaT",
	"TTaaaaaTT",
	"...onn...",
	"...onn...");
STAMP(scene_tuft, 3,
	"d.d",
	"ded",
	"eee");
STAMP(scene_tall, 9,
	".d..d..d.",
	"dfd.dfd.d",
	"efeddfedf",
	"eeeeeeeee");
STAMP(scene_flower_red, 3,
	".F.",
	"FXF",
	".F.");
STAMP(scene_flower_white, 3,
	".P.",
	"PXP",
	".P.");
STAMP(scene_platform, 28,
	"....pppppppppppppppppppp....",
	"..pppppppppppppppppppppppp..",
	"qqppppppppppppppppppppppppqq",
	".qqqqqqqqqqqqqqqqqqqqqqqqqq.",
	"..uuuuuuuuuuuuuuuuuuuuuuuu..");
STAMP(scene_lamp, 7,
	"..LLL..",
	".LLLLL.",
	".LLLLL.",
	"..lll..",
	"...l...",
	"...l...",
	"...l...",
	"...l...",
	"...l...",
	"..lll..");
STAMP(scene_lamp_glow, 11,
	"...GGGGG...",
	"..GGGGGGG..",
	".GGGGGGGGG.",
	".GGGGGGGGG.",
	".GGGGGGGGG.",
	"..GGGGGGG..",
	"...GGGGG...");
// clang-format on

// The hills, a digit a column: how many rows each one rises above the horizon.
static const char scene_far[] = "7777666655555555554433221111222334444444444556677777776333222223333333222111";
static const char scene_near[] = "2221111100001112233334333332222222221111111011112223334443333222221111111111";
#define SCENE_HILLS 76 // the columns they are cut for; a wider card repeats them

// A stamp's place: the column counts from the right edge when it is below zero.
typedef struct {
	const stamp_t *stamp;
	int8_t col, row;
} scene_item_t;

// clang-format off
static const scene_item_t scene_trees[] = {
	{&scene_tree, 2, 15},
	{&scene_pine, -11, 15},
};
static const scene_item_t scene_ground[] = {
	{&scene_tuft, 3, 28},
	{&scene_tuft, 24, 28},
	{&scene_tuft, -16, 34},
	{&scene_tuft, -4, 27},
	{&scene_flower_white, 10, 29},
	{&scene_flower_red, -13, 30},
	{&scene_flower_red, -6, 30},
};
static const scene_item_t scene_grass[] = {
	{&scene_tall, 0, 32},
	{&scene_tall, -10, 33},
	{&scene_tall, -17, 26},
};
// The stars at night: one cell each, and two that twinkle.
static const int8_t scene_stars[][2] = {{6, 7}, {14, 10}, {22, 8}, {30, 12}, {41, 7}, {47, 11}, {52, 8}, {-3, 13}, {9, 15}, {36, 15}, {-8, 6}};
static const scene_item_t scene_twinkles[] = {
	{&scene_star, 17, 11},
	{&scene_star, 44, 8},
};
// clang-format on

enum { SKY_DAY, SKY_SUNSET, SKY_NIGHT, SKY_RAIN, SKY_KINDS };

// Four bands of sky, from the top down to the horizon.
static const uint32_t scene_sky[SKY_KINDS][4] = {
	{0x3F8FE6, 0x62A9EE, 0x86C3F5, 0xAEDBFA},
	{0x4A3A86, 0xB0508C, 0xF0764F, 0xF9BE6A},
	{0x081029, 0x0D1840, 0x142256, 0x1B2E65},
	{0x6C84A8, 0x869EBF, 0xA0B5CF, 0xB9C9DC},
};

// The light on what is not sky: a tint over the hills, the ground and the trees,
// and one over the clouds, each as a colour and how much of 255 it is.
static const struct {
	uint32_t ground;
	uint8_t ground_amount;
	uint32_t cloud;
	uint8_t cloud_amount;
} scene_light[SKY_KINDS] = {
	{0x000000, 0, 0x000000, 0},
	{0xF08040, 56, 0xFF9A80, 102},
	{0x0B1228, 178, 0x1A223F, 186},
	{0x9AA8B8, 46, 0x000000, 0},
};

// What is up, as paint_clock and paint_room last left it.
static struct {
	int sky;
	bool sun, moon, lamp;
	int cols; // how many cells wide the card is
} scene;

// clang-format off
static uint32_t scene_hex(char letter) {
	switch (letter) {
	case 'w': return 0xF6FAFF; // a cloud
	case 'v': return 0xC4D8F0; //   and the shade under it
	case 'S': return 0xFFD93D; // the sun
	case 's': return 0xFFF2A8; //   and its shine
	case 'M': return 0xF5EFC9; // the moon
	case 'z': return 0xFFFFFF; // a star
	case 'G': return 0xFFE98A; // a lamp's glow, drawn thin
	case 'H': return 0x9CD9B0; // the far hills
	case 'I': return 0x72C486; //   the near ones
	case 'g': return 0x7BCB57; // the ground, a light band
	case 'h': return 0x6EBF4C; //   a darker one
	case 'j': return 0x569F3A; //   and the line at its foot
	case 'T': return 0x1E6A35; // a tree: its outline
	case 'a': return 0x2F8A45; //   shade
	case 'b': return 0x43AE58; //   leaf
	case 'c': return 0x72D872; //   light
	case 'o': return 0x6B4423; //   the dark side of the trunk
	case 'n': return 0x8B5A2B; //   and the lit one
	case 'd': return 0x2E8B3E; // tall grass: dark
	case 'e': return 0x4FB04E; //   middle
	case 'f': return 0x8BDF6B; //   tips
	case 'F': return 0xE5484D; // a red flower
	case 'P': return 0xF8F2F2; //   a white one
	case 'X': return 0xF6B73C; //   and the middle of both
	case 'p': return 0xB6EC86; // the platform: top
	case 'q': return 0x5FA83C; //   its rim
	case 'u': return 0x3C7A2C; //   and its underside
	case 'L': return 0xFFE9A0; // the lamp's shade
	case 'l': return 0x5A6270; //   and its post
	}
	return 0xFF00FF;
}
// clang-format on

static lv_color_t scene_color(char letter) {
	const int sky = scene.sky;
	const lv_color_t color = lv_color_hex(scene_hex(letter));
	if (letter == 'w' || letter == 'v') {
		if (sky == SKY_RAIN) {
			return lv_color_hex(letter == 'w' ? 0xAEB9C8 : 0x8693A8);
		}
		return lv_color_mix(lv_color_hex(scene_light[sky].cloud), color, scene_light[sky].cloud_amount);
	}
	if (letter == 'S' && sky == SKY_SUNSET) {
		return lv_color_hex(0xFF9A3C); // the low sun is orange
	}
	if (strchr("SsMzGL", letter)) {
		return color; // lights of their own: the sky does not tint them
	}
	return lv_color_mix(lv_color_hex(scene_light[sky].ground), color, scene_light[sky].ground_amount);
}

static int scene_x(const lv_area_t *box, int col) { return box->x1 + (col < 0 ? scene.cols + col : col) * SCENE_CELL; }

static int scene_y(const lv_area_t *box, int row) { return box->y1 + row * SCENE_CELL; }

static void scene_fill(lv_layer_t *layer, lv_draw_rect_dsc_t *rect, const lv_area_t *box, int col, int row, int cols, int rows, lv_color_t color) {
	lv_area_t cells;
	cells.x1 = scene_x(box, col);
	cells.y1 = scene_y(box, row);
	cells.x2 = cells.x1 + cols * SCENE_CELL - 1;
	cells.y2 = cells.y1 + rows * SCENE_CELL - 1;
	rect->bg_color = color;
	lv_draw_rect(layer, rect, &cells);
}

static void scene_put(lv_layer_t *layer, lv_draw_rect_dsc_t *rect, const lv_area_t *box, const stamp_t *stamp, int col, int row) { stamp_draw(layer, rect, stamp, scene_x(box, col), scene_y(box, row), SCENE_CELL, scene_color); }

static void scene_put_all(lv_layer_t *layer, lv_draw_rect_dsc_t *rect, const lv_area_t *box, const scene_item_t *item, int count) {
	for (int i = 0; i < count; i++) {
		scene_put(layer, rect, box, item[i].stamp, item[i].col, item[i].row);
	}
}

// A ridge of hills: for each height, a rectangle for every run of columns that
// is at least that tall.
static void scene_hills(lv_layer_t *layer, lv_draw_rect_dsc_t *rect, const lv_area_t *box, const char *table, char letter) {
	for (int rise = 1; rise <= 9; rise++) {
		for (int col = 0; col < scene.cols;) {
			if (table[col % SCENE_HILLS] - '0' < rise) {
				col++;
				continue;
			}
			int run = 1;
			while (col + run < scene.cols && table[(col + run) % SCENE_HILLS] - '0' >= rise) {
				run++;
			}
			scene_fill(layer, rect, box, col, SCENE_HORIZON - rise, run, 1, scene_color(letter));
			col += run;
		}
	}
}

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void scene_draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t box;
	lv_obj_get_coords(scenery, &box);

	lv_draw_rect_dsc_t rect;
	lv_draw_rect_dsc_init(&rect);
	rect.bg_opa = LV_OPA_COVER;

	const int cols = scene.cols;
	const uint32_t *sky = scene_sky[scene.sky];

	// The sky comes up out of the card: the first rows are the top band laid
	// thinly over whatever the card is, then the four bands in full.
	static const lv_opa_t fade[SCENE_FADE / 2] = {46, 102, 168};
	for (int i = 0; i < SCENE_FADE / 2; i++) {
		rect.bg_opa = fade[i];
		scene_fill(layer, &rect, &box, 0, i * 2, cols, 2, lv_color_hex(sky[0]));
	}
	rect.bg_opa = LV_OPA_COVER;
	for (int i = 0; i < 4; i++) {
		const int top = SCENE_FADE + i * 4;
		scene_fill(layer, &rect, &box, 0, top, cols, i < 3 ? 4 : SCENE_HORIZON - top, lv_color_hex(sky[i]));
	}

	if (scene.moon) {
		for (int i = 0; i < COUNT_OF(scene_stars); i++) {
			scene_fill(layer, &rect, &box, scene_stars[i][0], scene_stars[i][1], 1, 1, scene_color('z'));
		}
		scene_put_all(layer, &rect, &box, scene_twinkles, COUNT_OF(scene_twinkles));
		scene_put(layer, &rect, &box, &scene_moon, -17, 6);
	}
	if (scene.sun) {
		scene_put(layer, &rect, &box, &scene_sun, -18, scene.sky == SKY_SUNSET ? 13 : 5);
	}

	// The clouds are over the sun, and the big one is left out of a sunset,
	// when the sun is where it would be.
	scene_put(layer, &rect, &box, &scene_cloud_a, 3, 8);
	scene_put(layer, &rect, &box, &scene_cloud_c, 33, 5);
	if (scene.sky != SKY_SUNSET) {
		scene_put(layer, &rect, &box, &scene_cloud_b, -22, 12);
	}
	if (scene.sky == SKY_RAIN) {
		scene_put(layer, &rect, &box, &scene_cloud_rain, -24, 9);
	}

	scene_hills(layer, &rect, &box, scene_far, 'H');
	scene_hills(layer, &rect, &box, scene_near, 'I');

	// The ground is mown in bands three rows deep, with a darker line at the foot.
	for (int row = SCENE_HORIZON, band = 0; row < SCENE_ROWS - 1; row += 3, band++) {
		scene_fill(layer, &rect, &box, 0, row, cols, 3, scene_color(band % 2 ? 'h' : 'g'));
	}
	scene_fill(layer, &rect, &box, 0, SCENE_ROWS - 1, cols, 1, scene_color('j'));

	scene_put_all(layer, &rect, &box, scene_trees, COUNT_OF(scene_trees));
	scene_put_all(layer, &rect, &box, scene_ground, COUNT_OF(scene_ground));
	scene_put(layer, &rect, &box, &scene_platform, (cols - scene_platform.w) / 2, 32);
	scene_put_all(layer, &rect, &box, scene_grass, COUNT_OF(scene_grass));

	// The lamp that comes on at night, with a thin glow round its shade. It
	// stands in front of the grass.
	if (scene.lamp) {
		rect.bg_opa = 56;
		scene_put(layer, &rect, &box, &scene_lamp_glow, -21, 23);
		rect.bg_opa = LV_OPA_COVER;
		scene_put(layer, &rect, &box, &scene_lamp, -19, 25);
	}
}

// The room, the hour, the month and whether the clock is set decide the sky.
// They arrive from two places -- paint_room knows the room, paint_clock the
// rest -- so each leaves its part and calls this to put them together. Only a
// change of sky is drawn again.
static bool clock_known, clock_night, clock_rain;

static void scene_refresh(void) {
	if (!scenery) {
		return;
	}
	const bool dark = pet_room(petwatch_state()) == 2 || clock_night;
	int sky = SKY_DAY;
	if (dark) {
		sky = SKY_NIGHT;
	} else if (clock_rain) {
		sky = SKY_RAIN;
	} else if (pet_room(petwatch_state()) == 1) {
		sky = SKY_SUNSET;
	}
	const bool sun = clock_known && !dark && !clock_rain;
	if (scene.sky != sky || scene.sun != sun || scene.moon != dark || scene.lamp != clock_night) {
		scene.sky = sky;
		scene.sun = sun;
		scene.moon = dark;
		scene.lamp = clock_night;
		lv_obj_invalidate(scenery);
	}
}

static void build_scenery(int card_w) {
	scene.cols = (card_w + SCENE_CELL - 1) / SCENE_CELL;
	scenery = plain(habitat);
	lv_obj_set_pos(scenery, 0, SCENE_TOP);
	lv_obj_set_size(scenery, card_w, SCENE_ROWS * SCENE_CELL);
	lv_obj_add_event_cb(scenery, scene_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
}

// ---------------------------------------------------------------------------
// The Poke Ball
//
// The box that holds the day's fortune, an hour's game and whatever the
// listening turned up is drawn as a Poke Ball lying in the grass, in the same
// three-pixel cells as the scenery it lies in. Its button is white while
// there is nothing in it and gold, with a sparkle at the ball's shoulder, when
// there is. It is its own object because it is the thing to tap.
// ---------------------------------------------------------------------------

// clang-format off
STAMP(ball_art, 7,
	"..KKK..",
	".KRRRK.",
	"KRRRRRK",
	"KKKBKKK",
	"KWWWWWK",
	".KWWWK.",
	"..KKK..");
STAMP(ball_spark, 3,
	".G.",
	"GGG",
	".G.");
// clang-format on

// clang-format off
static lv_color_t ball_color(char letter) {
	switch (letter) {
	case 'K': return lv_color_hex(PIKACHU_INK);
	case 'R': return lv_color_hex(0xE5352B);
	case 'W': return lv_color_hex(PIKACHU_WHITE);
	case 'B': return ball_ready ? PET_GOLD : lv_color_hex(PIKACHU_WHITE);
	case 'G': return PET_GOLD;
	}
	return lv_color_hex(0xFF00FF);
}
// clang-format on

static void ball_draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t box;
	lv_obj_get_coords(parts.box, &box);

	lv_draw_rect_dsc_t rect;
	lv_draw_rect_dsc_init(&rect);
	rect.bg_opa = LV_OPA_COVER;

	stamp_draw(layer, &rect, &ball_art, box.x1, box.y1 + 3 * SCENE_CELL, SCENE_CELL, ball_color);
	if (ball_ready) {
		stamp_draw(layer, &rect, &ball_spark, box.x1 + 6 * SCENE_CELL, box.y1, SCENE_CELL, ball_color);
	}
}

// ---------------------------------------------------------------------------
// The room
// ---------------------------------------------------------------------------

static void paint_room(void) {
	if (!room_layer) {
		return;
	}

	// The room the player chose washes the card: the meadow's green, the
	// sunset's orange, the night's blue.
	lv_color_t wash = PET_ROOM_MEADOW;
	const int room = pet_room(petwatch_state());
	if (room == 1) {
		wash = PET_ROOM_SUNSET;
	} else if (room == 2) {
		wash = PET_ROOM_NIGHT;
	}
	lv_obj_set_style_bg_color(room_layer, wash, 0);
	lv_obj_set_style_bg_opa(room_layer, LV_OPA_20, 0);

	// The Calm soundscape: a month of jazz and classical buys drifting motes
	// over whatever room the pet is in.
	bool calm = petwatch_has_soundscape();
	for (int i = 0; i < 4; i++) {
		show(motes[i], calm);
	}

	// The same room is the sky's: a sunset room is a sunset, a night room night.
	scene_refresh();
}

// ---------------------------------------------------------------------------
// The words and the meters
// ---------------------------------------------------------------------------

static void paint_theme_colors(void);

// What the page last drew, so the one-second timer can leave alone everything
// that has not changed. The meters move a point every twelve to twenty seconds
// and the strings less often than that; the alternative is rebuilding fifty
// objects a second for arithmetic that mostly says "the same".
typedef struct {
	bool valid;
	int happy, energy, notes, today_secs, streak;
	pet_stage_t stage;
	pet_mood_t mood;
	bool listening;
	bool calm;
	pet_temperament_t temperament;
	char status[192];
	int room, wearing;
	pet_genre_look_t look;
} shown_t;

static shown_t shown;

static void paint_words(void) {
	if (!page_built) {
		return;
	}

	const pet_state_t *st = petwatch_state();
	int64_t now = petwatch_now();
	pet_stage_t stage = pet_stage(st);
	pet_mood_t mood = pet_mood(st, now);
	bool listening = petwatch_listening();

	// The words that only change when the animal does.
	if (!shown.valid || shown.stage != stage) {
		// The stage, not the mood: two words would not fit beside the meters,
		// and the face is already saying how it feels.
		lv_label_set_text(stage_label, tr(pet_stage_tag(stage)));
		shown.stage = stage;
	}

	if (!shown.valid) {
		char name[40];
		pet_name(name, sizeof(name));
		lv_label_set_text(name_label, name);
	}

	// The month's listening, under the stage: one word, or nothing at all while
	// there are not yet three days to call a character.
	pet_temperament_t temperament = pet_temperament(st);
	if (!shown.valid || shown.calm != pet_has_soundscape(st) || shown.temperament != temperament) {
		if (temperament == PET_TEMPERAMENT_NONE) {
			lv_label_set_text(temperament_label, "");
		} else {
			lv_label_set_text(temperament_label, tr(pet_temperament_tag(temperament)));
		}
		shown.temperament = temperament;
	}

	// The two meters, and the wallet under them. A bar is only set when its
	// number moved, which is what keeps the card's canvas still.
	if (!shown.valid || shown.happy != st->happy) {
		lv_bar_set_value(health_bar, st->happy, LV_ANIM_OFF);
		lv_label_set_text_fmt(health_value, "%d", st->happy);
		shown.happy = st->happy;
	}
	if (!shown.valid || shown.energy != st->energy) {
		lv_bar_set_value(energy_bar, st->energy, LV_ANIM_OFF);
		lv_label_set_text_fmt(energy_value, "%d", st->energy);
		shown.energy = st->energy;
	}
	if (!shown.valid || shown.notes != st->notes) {
		lv_label_set_text_fmt(notes_value, "%d", st->notes);
		shown.notes = st->notes;
	}

	// The two lines under the card.
	if (!shown.valid || shown.today_secs != st->today_secs) {
		lv_label_set_text_fmt(today_label, tr("pet_today"), st->today_secs / 60, PET_DAY_GOAL_SECS / 60);
		shown.today_secs = st->today_secs;
	}
	if (!shown.valid || shown.streak != st->streak) {
		if (st->streak > 0) {
			lv_label_set_text_fmt(streak_label, tr("pet_streak"), st->streak);
		} else {
			lv_label_set_text(streak_label, tr("pet_streak_none"));
		}
		shown.streak = st->streak;
	}

	// The line's picture: a still note when nothing is playing, three bars that
	// the frame timer swaps while something is.
	if (!shown.valid || shown.listening != listening) {
		lv_obj_set_hidden(status_icon, listening);
		for (int i = 0; i < 3; i++) {
			show(eq_bar[i], listening);
		}
	}

	// The status line changes with the track, so it is built and compared
	// rather than rebuilt: a track's title arriving is one label write.
	char title[128];
	petwatch_track_title(title, sizeof(title));
	char line[192];
	if (listening) {
		if (title[0]) {
			snprintf(line, sizeof(line), tr("pet_feeding"), title);
		} else {
			snprintf(line, sizeof(line), "%s", tr("pet_feeding_music"));
		}
	} else if (mood == PET_MOOD_DIZZY) {
		snprintf(line, sizeof(line), "%s", tr("pet_dizzy_hint"));
	} else if (mood == PET_MOOD_SLEEPING) {
		snprintf(line, sizeof(line), "%s", tr("pet_asleep"));
	} else {
		snprintf(line, sizeof(line), "%s", tr("pet_feed_me"));
	}
	if (!shown.valid || strcmp(line, shown.status) != 0) {
		lv_label_set_text(status_label, line);
		snprintf(shown.status, sizeof(shown.status), "%s", line);
	}
	shown.listening = listening;

	if (wardrobe_visible) {
		wardrobe_refresh();
	}
	if (customise_visible) {
		customise_refresh();
	}
}

// The page's one-second work: only what changed. The animal is drawn again when
// it has a different face, a different hat, a different room -- not because a
// clock moved.
static void paint_values(void) {
	if (!page_built) {
		return;
	}

	const pet_state_t *st = petwatch_state();
	bool animal = !shown.valid || shown.stage != pet_stage(st) || shown.mood != pet_mood(st, petwatch_now()) ||
				  shown.listening != petwatch_listening() || shown.calm != pet_has_soundscape(st) ||
				  shown.wearing != pet_wearing(st) || shown.room != pet_room(st) || shown.look != petwatch_look();

	if (animal) {
		paint_room();
		paint_creature();
		paint_theme_colors();
		shown.wearing = pet_wearing(st);
		shown.look = petwatch_look();
		shown.room = pet_room(st);
		shown.calm = pet_has_soundscape(st);
		shown.mood = pet_mood(st, petwatch_now());
	}
	paint_words();
	shown.valid = true;
}

static void paint_theme_colors(void) {
	if (!page_built) {
		return;
	}

	lv_obj_set_style_bg_color(health_bar, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_color(health_bar, theme_is_monochrome() ? theme()->accent : lv_color_hex(0xE2574C),
							  LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(energy_bar, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_color(energy_bar, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_style_image_recolor(health_icon, theme_is_monochrome() ? theme()->accent : lv_color_hex(0xE2574C), 0);
	lv_obj_set_style_image_recolor(energy_icon, theme()->accent, 0);
	lv_obj_set_style_image_recolor(notes_icon, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor(card_close_icon, theme()->text_secondary, 0);

	const lv_color_t dim = theme()->text_secondary;
	lv_obj_set_style_text_color(health_value, dim, 0);
	lv_obj_set_style_text_color(energy_value, dim, 0);
	lv_obj_set_style_text_color(notes_value, dim, 0);
	lv_obj_set_style_text_color(today_label, dim, 0);
	lv_obj_set_style_text_color(streak_label, dim, 0);

	const lv_color_t playing = theme_is_monochrome() ? theme()->accent : theme_semantic_color(THEME_SEMANTIC_GREEN);
	lv_obj_set_style_image_recolor(status_icon, playing, 0);
	for (int i = 0; i < 3; i++) {
		lv_obj_set_style_bg_color(eq_bar[i], playing, 0);
	}

	// The drifting notes are over the sky, a blue on one theme and nearly white
	// on another, so they are white on all of them.
	lv_obj_set_style_image_recolor(note_a, lv_color_white(), 0);
	lv_obj_set_style_image_recolor(note_b, lv_color_white(), 0);
	lv_obj_set_style_text_color(temperament_label,
								theme_is_monochrome() ? theme()->accent : theme_semantic_color(THEME_SEMANTIC_BLUE), 0);
	for (int i = 0; i < 4; i++) {
		lv_obj_set_style_bg_color(motes[i], theme_is_dark() ? lv_color_hex(0xFFFFFF) : theme()->accent, 0);
	}
}

static void paint(void) {
	if (!page_built) {
		return;
	}
	shown.valid = false;
	paint_room();
	paint_clock();
	paint_creature();
	paint_words();
	paint_theme_colors();
}

// ---------------------------------------------------------------------------
// The animation: two frames, one timer
//
// The animal is not interpolated. It has two frames -- a breath in and a breath
// out -- and the page's own one-second timer swaps them: the root moves two
// pixels (and, while he is feeding, steps from side to side), the stars turn and
// the hearts float, the sparks change, the notes drift and the
// motes rise. Nothing here runs at more than 1 Hz but a blink, nothing asks LVGL
// to interpolate, and the whole thing is a dozen integer fields written into a
// dozen objects once a second. On a 240 x 320 panel that is the difference
// between a smooth animation the processor cannot afford and one it can.
// ---------------------------------------------------------------------------

static bool animating;
static int frame;     // which of the two frames is up
static int mote_step; // how far the calm soundscape's motes have risen

// Where each mote started, so the rise is a running offset rather than a fixed
// animation per object.
static int mote_home[4];

static void apply_frame(void) {
	if (!page_built) {
		return;
	}

	const bool listening = petwatch_listening();

	// The breath: two pixels, three when something is playing.
	int dy = frame ? (listening ? 3 : 2) : 0;
	int dx = 0;
	switch (reaction) {
	case REACT_JUMP:
		dy -= frame ? 10 : 6;
		break;
	case REACT_DUCK:
		dy += frame ? 6 : 4;
		break;
	case REACT_HOP:
		dy -= frame ? 8 : 1;
		break;
	case REACT_HEART:
		dy -= frame ? 5 : 1;
		break;
	case REACT_LEAN_L:
		dx = frame ? -4 : -2;
		break;
	case REACT_LEAN_R:
		dx = frame ? 4 : 2;
		break;
	case REACT_WAVE:
		dx = frame ? 3 : -3; // there is no arm to wave: he shakes from side to side
		break;
	default:
		break;
	}
	// Fed, he dances: a step to each side on top of the breath, unless a finger or
	// a key has just asked for something else.
	if (feeding_now && reaction == REACT_NONE) {
		dx = frame ? 2 : -2;
	}
	lv_obj_set_style_translate_x(parts.root, dx, 0);
	lv_obj_set_style_translate_y(parts.root, dy, 0);
	// A jump has a shadow under it: it stays on the floor while he is above it.
	lv_obj_set_style_translate_y(parts.shadow, dy < 0 ? -dy : 0, 0);
	// The heart bobs independently of the little hop.
	lv_obj_set_style_translate_y(parts.heart, frame ? 4 : 0, 0);
	// The ball and the find stand on the floor of the room, not on the animal:
	// when it jumps, the furniture stays where it is. A child's own translate
	// adds to its parent's, so cancelling it is exact.
	lv_obj_set_style_translate_x(parts.box, -dx, 0);
	lv_obj_set_style_translate_y(parts.box, -dy, 0);
	lv_obj_set_style_translate_x(parts.find, -dx, 0);
	lv_obj_set_style_translate_y(parts.find, -dy, 0);
	// The stars turn and the hearts float: the overlay's other frame. Nothing
	// else on the animal moves -- it is one picture.
	const bool other = frame != 0;
	if (gear.other != other) {
		gear.other = other;
		if (gear.moves) {
			lv_obj_invalidate(parts.gear);
		}
	}

	// Notes are behind the animal, and visible only while music is audible.
	show(note_a, listening);
	show(note_b, listening);
	// The two notes drift while something is playing: one rises as the other
	// fades, which is all the movement two stills can suggest.
	if (listening) {
		lv_obj_set_style_translate_y(note_a, frame ? 8 : 22, 0);
		lv_obj_set_style_opa(note_a, frame ? LV_OPA_40 : LV_OPA_COVER, 0);
		lv_obj_set_style_translate_y(note_b, frame ? 20 : 6, 0);
		lv_obj_set_style_opa(note_b, frame ? LV_OPA_COVER : LV_OPA_40, 0);
	}

	// The equalizer: three bars; the two frames are the two bars of a beat.
	if (listening) {
		static const uint8_t heights[2][3] = {{4, 9, 6}, {9, 5, 10}};
		for (int i = 0; i < 3; i++) {
			int h = heights[frame][i];
			lv_obj_set_height(eq_bar[i], h);
			lv_obj_set_y(eq_bar[i], eq_bottom - h);
		}
	}

	// What the month drops: five small things falling slowly, each on its own
	// phase, wrapping round at the bottom of the card. Nothing moves unless the
	// season has something to drop, and a still thing costs nothing.
	if (clock_layer_built && !lv_obj_is_hidden(flake[0])) {
		const int span = card_h_px - 104;
		for (int i = 0; i < PET_FLAKES; i++) {
			const int drop = span > 0 ? (flake_step + i * span / PET_FLAKES) % span : 0;
			lv_obj_set_pos(flake[i], flake_home_x[i] + (drop / 12) % 9 - 4, 54 + drop);
		}
	}

	// The Calm soundscape's motes rise a few pixels a second, each on its own
	// phase, and wrap round off the top edge where nothing can be seen of them.
	for (int i = 0; i < 4; i++) {
		int span = mote_home[i] + 12;
		int risen = (mote_step + i * 23) % (span > 0 ? span : 1);
		lv_obj_set_y(motes[i], mote_home[i] - risen);
	}
}

static void start_animations(void) {
	if (!page_built) {
		return;
	}
	animating = true;
	frame = 0;
	blink_wait = 3;
	fidget_wait = 9;
	apply_frame();
}

static void stop_animations(void) {
	if (!page_built) {
		return;
	}
	animating = false;
	reaction = REACT_NONE;
	blink_stop();
	holding = false;
	hold_paid = false;
	hide_hold_meter();
	show(parts.heart, false);
	lv_obj_set_style_translate_x(parts.root, 0, 0);
	lv_obj_set_style_translate_y(parts.root, 0, 0);
	show(note_a, false);
	show(note_b, false);
	mote_step = 0;
}

static void sync_animation(void) {
	if (!page_visible || !page_built) {
		return;
	}
	if (!animating) {
		start_animations();
	}
	apply_frame();
}

// One second of the page's life: the next frame, and whatever the listening did
// to it. Called from the page timer, which stops when the page is left and is
// paused in standby.
static void advance_frame(void) {
	if (!page_built || !animating) {
		return;
	}
	frame ^= 1;
	if (frame == 0) {
		flake_step++;
		if (mote_step < 4096) {
			mote_step += 1;
		}
	}
	apply_frame();
	life_tick();
}

// ---------------------------------------------------------------------------
// The tap-along: sixteen beats, once a day
// ---------------------------------------------------------------------------

#define TAP_TIMER_MS 40
#define TAP_TRAVEL_MS 1100 // how long a note takes to fall the lane
#define TAP_LANE_H 150

static int tap_score(void) { return tap_points * 100 / (PET_RHYTHM_BEATS * 2); }

static void tap_show_score(void) { lv_label_set_text_fmt(tap_score_label, tr("pet_tap_score"), tap_score()); }

static void tap_close(void) {
	if (tap_timer) {
		lv_timer_delete(tap_timer);
		tap_timer = NULL;
	}
	if (tap_sheet) {
		lv_obj_set_hidden(tap_sheet, true);
	}
	tap_visible = false;
}

// Pays for the game, says what it was worth, and puts the sheet away. The
// score is the share of the sixteen beats hit at all; the notes are the pet's
// business and the page only says how many arrived.
static void tap_finish(void) {
	int score = tap_score();
	int notes = petwatch_rhythm_finish(score);
	tap_close();
	if (notes > 0) {
		char line[64];
		snprintf(line, sizeof(line), tr("pet_tap_done"), notes);
		toast_success(line);
	} else {
		toast_plain(tr("pet_tap_played"));
	}
	paint();
}

static void tap_tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (!tap_visible) {
		return;
	}

	int32_t elapsed = (int32_t)lv_tick_elaps(tap_start_ms);

	// The note for the next beat falls towards the line. Once its moment has
	// passed it keeps going a little, then waits to be judged.
	if (tap_next_beat < PET_RHYTHM_BEATS) {
		int32_t until = (int32_t)((uint32_t)(tap_next_beat + 1) * tap_beat_ms) - elapsed;
		if (until > TAP_TRAVEL_MS) {
			until = TAP_TRAVEL_MS;
		}
		if (until < -PET_RHYTHM_GOOD_MS) {
			until = -PET_RHYTHM_GOOD_MS;
		}
		int32_t travel = TAP_LANE_H - 34;
		int32_t y = travel - (until * travel / TAP_TRAVEL_MS);
		lv_obj_set_y(tap_note, y);
		lv_obj_set_hidden(tap_note, false);
	} else {
		lv_obj_set_hidden(tap_note, true);
	}

	// A beat nobody tapped in time is a miss, and the game moves on without it.
	while (tap_next_beat < PET_RHYTHM_BEATS &&
		   elapsed > (int32_t)((uint32_t)(tap_next_beat + 1) * tap_beat_ms) + PET_RHYTHM_GOOD_MS) {
		tap_next_beat++;
		tap_show_score();
	}

	if (elapsed > (int32_t)((uint32_t)PET_RHYTHM_BEATS * tap_beat_ms) + 700) {
		tap_finish();
	}
}

static void tap_press_cb(lv_event_t *e) {
	(void)e;
	if (!tap_visible) {
		return;
	}

	int32_t elapsed = (int32_t)lv_tick_elaps(tap_start_ms);
	while (tap_next_beat < PET_RHYTHM_BEATS &&
		   elapsed > (int32_t)((uint32_t)(tap_next_beat + 1) * tap_beat_ms) + PET_RHYTHM_GOOD_MS) {
		tap_next_beat++;
	}
	if (tap_next_beat >= PET_RHYTHM_BEATS) {
		return;
	}

	int32_t delta = elapsed - (int32_t)((uint32_t)(tap_next_beat + 1) * tap_beat_ms);
	if (delta < 0) {
		delta = -delta;
	}
	if (delta > PET_RHYTHM_GOOD_MS) {
		return; // nowhere near a beat: the tap is simply lost
	}

	tap_points += delta <= PET_RHYTHM_PERFECT_MS ? 2 : 1;
	tap_next_beat++;
	tap_show_score();
}

static void tap_close_cb(lv_event_t *e) {
	(void)e;
	tap_close();
}

static void tap_done_cb(lv_event_t *e) {
	(void)e;
	if (!tap_visible) {
		return;
	}
	tap_finish();
}

static void build_tap(void);
static void place_sheet_heading(lv_obj_t *label);

static void tap_open(void) {
	if (!petwatch_rhythm_ready()) {
		toast_plain(tr("pet_tap_played"));
		return;
	}
	build_tap();
	if (!tap_sheet) {
		return;
	}

	tap_points = 0;
	tap_next_beat = 0;
	tap_show_score();
	lv_label_set_text(tap_hint_label, tr("pet_tap_hint"));
	lv_obj_set_y(tap_note, 0);
	lv_obj_set_hidden(tap_note, false);
	lv_obj_set_hidden(tap_sheet, false);
	lv_obj_move_foreground(tap_sheet);
	tap_visible = true;

	tap_start_ms = lv_tick_get();
	if (!tap_timer) {
		tap_timer = lv_timer_create(tap_tick_cb, TAP_TIMER_MS, NULL);
	}
}

static void tap_open_cb(lv_event_t *e) {
	(void)e;
	if (tap_visible) {
		tap_close();
		return;
	}
	tap_open();
}

static void build_tap(void) {
	if (tap_sheet || !page_cfg) {
		return;
	}

	const int pad = page_cfg->padding;
	const int w = page_cfg->screen_width;
	const int h = page_cfg->screen_height;
	const int lane_y = pad + page_cfg->top_bar_height + 60;

	tap_sheet = lv_obj_create(petpage_screen);
	lv_obj_set_size(tap_sheet, w, h);
	lv_obj_set_pos(tap_sheet, 0, 0);
	lv_obj_set_style_pad_all(tap_sheet, 0, 0);
	lv_obj_set_style_border_width(tap_sheet, 0, 0);
	lv_obj_add_style(tap_sheet, &theme_style_screen, 0);
	lv_obj_set_scrollable(tap_sheet, false);
	lv_obj_set_hidden(tap_sheet, true);

	// The whole sheet is the drum, so a finger anywhere on it counts. It is
	// created first and everything else sits over it: the labels and the lane
	// are not clickable, so the press falls through to it.
	lv_obj_t *drum = lv_btn_create(tap_sheet);
	lv_obj_set_size(drum, w, h);
	lv_obj_set_pos(drum, 0, 0);
	lv_obj_set_style_bg_opa(drum, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(drum, 0, 0);
	lv_obj_set_style_shadow_width(drum, 0, 0);
	lv_obj_set_style_pad_all(drum, 0, 0);
	lv_obj_add_event_cb(drum, tap_press_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *heading = lv_label_create(tap_sheet);
	lv_label_set_text(heading, tr("pet_tap_game"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_18, 0);
	place_sheet_heading(heading);

	lv_obj_t *close = lv_btn_create(tap_sheet);
	settingsrow_place_corner_button(close, page_cfg, 0);
	lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(close, 0, 0);
	lv_obj_set_style_shadow_width(close, 0, 0);
	lv_obj_set_style_pad_all(close, 0, 0);
	lv_obj_add_event_cb(close, tap_close_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *close_icon = icon_image(close, &icon_close, settingsrow_corner_button_size(page_cfg) * 5 / 9);
	lv_obj_add_style(close_icon, &theme_style_icon, 0);
	lv_obj_center(close_icon);

	tap_hint_label = lv_label_create(tap_sheet);
	lv_obj_add_style(tap_hint_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(tap_hint_label, &font_ui_14, 0);
	lv_label_set_text(tap_hint_label, tr("pet_tap_hint"));
	lv_obj_align(tap_hint_label, LV_ALIGN_TOP_LEFT, pad + 4, settingsrow_header_text_top(page_cfg) + 32);

	// The lane: a card the note falls through, with a line where the beat is.
	lv_obj_t *lane = lv_obj_create(tap_sheet);
	lv_obj_add_style(lane, &theme_style_card, 0);
	lv_obj_set_size(lane, 96, TAP_LANE_H);
	lv_obj_align(lane, LV_ALIGN_TOP_MID, 0, lane_y);
	lv_obj_set_style_radius(lane, 14, 0);
	lv_obj_set_style_border_width(lane, 0, 0);
	lv_obj_set_style_shadow_width(lane, 0, 0);
	lv_obj_set_style_pad_all(lane, 0, 0);
	lv_obj_set_scrollable(lane, false);
	tap_lane = lane;

	lv_obj_t *line = bar_piece(lane, theme()->accent, 2);
	lv_obj_set_size(line, 72, 3);
	lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, -18);

	tap_note = disc(lane, theme()->accent);
	lv_obj_set_size(tap_note, 16, 16);
	lv_obj_set_pos(tap_note, 40, 0);

	lv_obj_t *tap_word = lv_label_create(lane);
	lv_label_set_text(tap_word, tr("pet_tap"));
	lv_obj_add_style(tap_word, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(tap_word, &font_ui_14, 0);
	lv_obj_align(tap_word, LV_ALIGN_BOTTOM_MID, 0, -2);

	tap_score_label = lv_label_create(tap_sheet);
	lv_obj_add_style(tap_score_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(tap_score_label, &font_ui_18, 0);
	lv_obj_set_width(tap_score_label, w - 2 * pad);
	lv_obj_set_style_text_align(tap_score_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(tap_score_label, LV_ALIGN_TOP_MID, 0, lane_y + TAP_LANE_H + 8);
	lv_label_set_text_fmt(tap_score_label, tr("pet_tap_score"), 0);

	lv_obj_t *done = lv_btn_create(tap_sheet);
	lv_obj_set_size(done, 104, 40);
	lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_set_style_radius(done, 12, 0);
	lv_obj_set_style_bg_color(done, theme()->accent, 0);
	lv_obj_set_style_bg_opa(done, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(done, 0, 0);
	lv_obj_set_style_shadow_width(done, 0, 0);
	lv_obj_add_event_cb(done, tap_done_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *done_label = lv_label_create(done);
	lv_label_set_text(done_label, tr("ok"));
	lv_obj_set_style_text_color(done_label, theme_on_accent(), 0);
	lv_obj_set_style_text_font(done_label, &font_ui_16, 0);
	lv_obj_center(done_label);
}

// ---------------------------------------------------------------------------
// The box
//
// One sheet, opened from the Poke Ball on the floor -- the code calls it the
// box, as the system layer does -- and everything the watching turns up goes
// through it: the day's fortune (a line to keep or to ignore, worth fifty notes
// when it has been carried out), the shelf of what the listening found, and the
// hour's note game.
//
// Nothing here is a new timer except the game. A find is counted in pet.c from
// the seconds the watcher has already credited, so being away does not stop it
// and being away does not multiply it: the box holds three.
// ---------------------------------------------------------------------------

#define SHELF_CELL 44
#define SHELF_COUNT PET_DROP_KINDS

static lv_obj_t *box_sheet;
static lv_obj_t *box_fortune_label;
static lv_obj_t *box_done_button;
static lv_obj_t *box_done_label;
static lv_obj_t *box_plus_label;
static lv_obj_t *box_shelf_icon[SHELF_COUNT];
static lv_obj_t *box_shelf_count[SHELF_COUNT];
static lv_obj_t *box_game_label;
static bool box_visible;

static lv_timer_t *treat_timer;
static lv_obj_t *treat_sheet;
static lv_obj_t *treat_lane;
static lv_obj_t *treat_note;
static lv_obj_t *treat_zone;
static bool treat_visible;
static int treat_lane_w;
static int treat_pass;
static int treat_hits;
static bool treat_hit_this;
static int32_t treat_start_ms;

#define TREAT_NOTES 6      // notes to hit in one game: about nine seconds
#define TREAT_PASS_MS 1400 // how long one note takes to cross the lane
#define TREAT_TIMER_MS 30
#define TREAT_ZONE_PX 26 // half the width of the window a hit is inside

static void box_refresh(void) {
	if (!box_sheet) {
		return;
	}
	const petwatch_box_t box = petwatch_box();

	char key[32];
	snprintf(key, sizeof(key), "pet_fortune_%d", box.fortune_id % PET_FORTUNE_COUNT + 1);
	lv_label_set_text(box_fortune_label, tr(key));

	// Answered is not the same as not yet asked: the day's line stays on the
	// card after it has been paid for, and says so.
	show(box_done_button, box.fortune_ready);
	show(box_plus_label, true);
	lv_label_set_text(box_plus_label, box.fortune_ready ? "+50" : tr("pet_box_answered"));

	for (int i = 0; i < SHELF_COUNT; i++) {
		show(box_shelf_icon[i], true);
		lv_label_set_text_fmt(box_shelf_count[i], "%d", box.found[i]);
	}
	lv_label_set_text(box_game_label, box.treats_ready ? tr("pet_box_play") : tr("pet_box_hour"));
}

static void box_close(void) {
	if (box_sheet) {
		lv_obj_set_hidden(box_sheet, true);
	}
	box_visible = false;
	treat_close();
}

static void box_close_cb(lv_event_t *e) {
	(void)e;
	box_close();
}

static void box_done_cb(lv_event_t *e) {
	(void)e;
	if (!petwatch_fortune_pay()) {
		toast_plain(tr("pet_tap_played")); // already answered today
	} else {
		char line[64];
		snprintf(line, sizeof(line), tr("pet_tap_done"), PET_FORTUNE_NOTES);
		toast_success(line);
	}
	box_refresh();
	paint();
}

// ---- the note game -------------------------------------------------------

static void treat_close(void) {
	if (treat_timer) {
		lv_timer_delete(treat_timer);
		treat_timer = NULL;
	}
	if (treat_sheet) {
		lv_obj_set_hidden(treat_sheet, true);
	}
	treat_visible = false;
}

static void treat_finish(void) {
	const int hits = treat_hits;
	treat_close();
	const int notes = petwatch_treat_finish(hits);
	if (notes > 0) {
		char line[64];
		snprintf(line, sizeof(line), tr("pet_tap_done"), notes);
		toast_success(line);
	} else {
		toast_plain(tr("pet_tap_played"));
	}
	if (box_visible) {
		box_refresh();
	}
	paint();
}

static void treat_tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (!treat_visible) {
		return;
	}
	const int32_t elapsed = (int32_t)lv_tick_elaps(treat_start_ms);
	const int pass = elapsed / TREAT_PASS_MS;
	if (pass != treat_pass) {
		treat_pass = pass;
		treat_hit_this = false;
	}
	if (pass >= TREAT_NOTES) {
		treat_finish();
		return;
	}
	const int32_t into = elapsed % TREAT_PASS_MS;
	const int span = treat_lane_w - 16;
	lv_obj_set_x(treat_note, (int32_t)into * span / TREAT_PASS_MS);
}

static void treat_press_cb(lv_event_t *e) {
	(void)e;
	if (!treat_visible || treat_hit_this) {
		return;
	}
	const int centre = lv_obj_get_x(treat_note) + 8;
	const int target = treat_lane_w / 2;
	if (centre < target - TREAT_ZONE_PX || centre > target + TREAT_ZONE_PX) {
		return;
	}
	treat_hit_this = true;
	treat_hits++;
	set_color(treat_note, PET_GOLD);
}

static void treat_open_cb(lv_event_t *e) {
	(void)e;
	if (!petwatch_box().treats_ready) {
		toast_plain(tr("pet_box_hour"));
		return;
	}

	if (!treat_sheet) {
		return;
	}
	treat_pass = 0;
	treat_hits = 0;
	treat_hit_this = false;
	set_color(treat_note, theme()->accent);
	lv_obj_set_x(treat_note, 0);
	lv_obj_set_hidden(treat_sheet, false);
	lv_obj_move_foreground(treat_sheet);
	treat_visible = true;
	treat_start_ms = lv_tick_get();
	if (!treat_timer) {
		treat_timer = lv_timer_create(treat_tick_cb, TREAT_TIMER_MS, NULL);
	}
}

static void build_treat(void) {
	if (treat_sheet || !page_cfg) {
		return;
	}
	const int pad = page_cfg->padding;
	const int w = page_cfg->screen_width;
	const int h = page_cfg->screen_height;

	treat_sheet = lv_obj_create(petpage_screen);
	lv_obj_set_size(treat_sheet, w, h);
	lv_obj_set_pos(treat_sheet, 0, 0);
	lv_obj_set_style_pad_all(treat_sheet, 0, 0);
	lv_obj_set_style_border_width(treat_sheet, 0, 0);
	lv_obj_add_style(treat_sheet, &theme_style_screen, 0);
	lv_obj_set_scrollable(treat_sheet, false);
	lv_obj_set_hidden(treat_sheet, true);

	// The whole sheet is the drum: a note has to be caught with the hand that
	// is already there.
	lv_obj_t *drum = lv_btn_create(treat_sheet);
	lv_obj_set_size(drum, w, h);
	lv_obj_set_pos(drum, 0, 0);
	lv_obj_set_style_bg_opa(drum, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(drum, 0, 0);
	lv_obj_set_style_shadow_width(drum, 0, 0);
	lv_obj_set_style_pad_all(drum, 0, 0);
	lv_obj_add_event_cb(drum, treat_press_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *heading = lv_label_create(treat_sheet);
	lv_label_set_text(heading, tr("pet_box_game"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_18, 0);
	place_sheet_heading(heading);

	lv_obj_t *close = lv_btn_create(treat_sheet);
	settingsrow_place_corner_button(close, page_cfg, 0);
	lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(close, 0, 0);
	lv_obj_set_style_shadow_width(close, 0, 0);
	lv_obj_set_style_pad_all(close, 0, 0);
	lv_obj_add_event_cb(close, (lv_event_cb_t)box_close_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *close_icon = icon_image(close, &icon_close, settingsrow_corner_button_size(page_cfg) * 5 / 9);
	lv_obj_add_style(close_icon, &theme_style_icon, 0);
	lv_obj_center(close_icon);

	lv_obj_t *hint = lv_label_create(treat_sheet);
	lv_obj_add_style(hint, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(hint, &font_ui_14, 0);
	lv_label_set_text(hint, tr("pet_box_game_hint"));
	lv_obj_set_width(hint, w - 2 * pad - 8);
	lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
	lv_obj_align(hint, LV_ALIGN_TOP_LEFT, pad + 4, settingsrow_header_text_top(page_cfg) + 34);

	// The lane runs left to right, and the window the note has to cross sits
	// in the middle of it: one line, one note, one hand.
	const int lane_y = pad + page_cfg->top_bar_height + 96;
	const int lane_h = 72;
	treat_lane_w = w - 2 * pad - 16;
	treat_lane = lv_obj_create(treat_sheet);
	lv_obj_add_style(treat_lane, &theme_style_card, 0);
	lv_obj_set_size(treat_lane, treat_lane_w, lane_h);
	lv_obj_align(treat_lane, LV_ALIGN_TOP_MID, 0, lane_y);
	lv_obj_set_style_radius(treat_lane, 14, 0);
	lv_obj_set_style_border_width(treat_lane, 0, 0);
	lv_obj_set_style_shadow_width(treat_lane, 0, 0);
	lv_obj_set_style_pad_all(treat_lane, 0, 0);
	lv_obj_set_scrollable(treat_lane, false);

	treat_zone = bar_piece(treat_lane, theme()->accent, 4);
	lv_obj_set_size(treat_zone, TREAT_ZONE_PX * 2, lane_h - 20);
	lv_obj_align(treat_zone, LV_ALIGN_LEFT_MID, treat_lane_w / 2 - TREAT_ZONE_PX, 0);
	lv_obj_set_style_bg_opa(treat_zone, LV_OPA_30, 0);

	lv_obj_t *line = bar_piece(treat_lane, theme()->accent, 1);
	lv_obj_set_size(line, treat_lane_w - 16, 2);
	lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, -10);

	treat_note = disc(treat_lane, theme()->accent);
	lv_obj_set_size(treat_note, 16, 16);
	lv_obj_align(treat_note, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_x(treat_note, 0);
}

static void build_box(void) {
	if (box_sheet || !page_cfg) {
		return;
	}
	const int pad = page_cfg->padding;
	const int w = page_cfg->screen_width;
	const int h = page_cfg->screen_height;

	box_sheet = lv_obj_create(petpage_screen);
	lv_obj_set_size(box_sheet, w, h);
	lv_obj_set_pos(box_sheet, 0, 0);
	lv_obj_set_style_pad_all(box_sheet, 0, 0);
	lv_obj_set_style_border_width(box_sheet, 0, 0);
	lv_obj_add_style(box_sheet, &theme_style_screen, 0);
	lv_obj_set_scrollable(box_sheet, false);
	lv_obj_set_hidden(box_sheet, true);

	lv_obj_t *heading = lv_label_create(box_sheet);
	lv_label_set_text(heading, tr("pet_box"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_18, 0);
	place_sheet_heading(heading);

	lv_obj_t *close = lv_btn_create(box_sheet);
	settingsrow_place_corner_button(close, page_cfg, 0);
	lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(close, 0, 0);
	lv_obj_set_style_shadow_width(close, 0, 0);
	lv_obj_set_style_pad_all(close, 0, 0);
	lv_obj_add_event_cb(close, box_close_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *close_icon = icon_image(close, &icon_close, settingsrow_corner_button_size(page_cfg) * 5 / 9);
	lv_obj_add_style(close_icon, &theme_style_icon, 0);
	lv_obj_center(close_icon);

	// The fortune, on a card of its own: it is the only long sentence on the
	// page and it should look like something to read rather than a setting.
	int y = settingsrow_header_text_top(page_cfg) + 40;
	lv_obj_t *card = lv_obj_create(box_sheet);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_size(card, w - 2 * pad, 62);
	lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 8, 0);
	lv_obj_set_scrollable(card, false);

	box_fortune_label = lv_label_create(card);
	lv_obj_add_style(box_fortune_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(box_fortune_label, &font_ui_14, 0);
	lv_obj_set_width(box_fortune_label, w - 2 * pad - 16);
	lv_label_set_long_mode(box_fortune_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_pos(box_fortune_label, 0, 0);
	lv_label_set_text(box_fortune_label, "");

	// The answer, and what it pays.
	y += 70;
	box_done_button = lv_btn_create(box_sheet);
	lv_obj_set_size(box_done_button, 128, 34);
	lv_obj_set_pos(box_done_button, pad, y);
	lv_obj_set_style_radius(box_done_button, 10, 0);
	lv_obj_set_style_bg_color(box_done_button, theme()->accent, 0);
	lv_obj_set_style_bg_opa(box_done_button, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(box_done_button, 0, 0);
	lv_obj_set_style_shadow_width(box_done_button, 0, 0);
	lv_obj_add_event_cb(box_done_button, box_done_cb, LV_EVENT_CLICKED, NULL);
	box_done_label = lv_label_create(box_done_button);
	lv_label_set_text(box_done_label, tr("pet_box_done"));
	lv_obj_set_style_text_color(box_done_label, theme_on_accent(), 0);
	lv_obj_set_style_text_font(box_done_label, &font_ui_14, 0);
	lv_obj_center(box_done_label);

	box_plus_label = lv_label_create(box_sheet);
	lv_obj_add_style(box_plus_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(box_plus_label, &font_ui_14, 0);
	lv_obj_set_pos(box_plus_label, pad + 136, y + 8);
	lv_label_set_text(box_plus_label, "");

	// The shelf: one cell a kind, a picture and a number in it.
	y += 48;
	lv_obj_t *shelf = lv_label_create(box_sheet);
	lv_obj_add_style(shelf, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(shelf, &font_ui_14, 0);
	lv_obj_set_pos(shelf, pad, y);
	lv_label_set_text(shelf, tr("pet_box_shelf"));

	y += 22;
	const int gap = 6;
	const int total = SHELF_COUNT * SHELF_CELL + (SHELF_COUNT - 1) * gap;
	int x = (w - total) / 2;
	for (int i = 0; i < SHELF_COUNT; i++) {
		lv_obj_t *cell = lv_obj_create(box_sheet);
		lv_obj_add_style(cell, &theme_style_card, 0);
		lv_obj_set_size(cell, SHELF_CELL, SHELF_CELL);
		lv_obj_set_pos(cell, x, y);
		lv_obj_set_style_radius(cell, 10, 0);
		lv_obj_set_style_border_width(cell, 0, 0);
		lv_obj_set_style_shadow_width(cell, 0, 0);
		lv_obj_set_style_pad_all(cell, 0, 0);
		lv_obj_set_scrollable(cell, false);
		box_shelf_icon[i] = drop_icon(cell, i, 22);
		lv_obj_align(box_shelf_icon[i], LV_ALIGN_TOP_MID, 0, 6);
		box_shelf_count[i] = lv_label_create(cell);
		lv_obj_add_style(box_shelf_count[i], &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(box_shelf_count[i], &font_ui_14, 0);
		lv_label_set_text(box_shelf_count[i], "0");
		lv_obj_align(box_shelf_count[i], LV_ALIGN_BOTTOM_MID, 0, -4);
		x += SHELF_CELL + gap;
	}

	// The hour's game, at the foot of the sheet.
	y += SHELF_CELL + 16;
	lv_obj_t *game = lv_btn_create(box_sheet);
	lv_obj_set_size(game, w - 2 * pad, 36);
	lv_obj_set_pos(game, pad, y);
	lv_obj_set_style_radius(game, 10, 0);
	lv_obj_set_style_bg_color(game, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(game, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(game, 0, 0);
	lv_obj_set_style_shadow_width(game, 0, 0);
	lv_obj_add_event_cb(game, treat_open_cb, LV_EVENT_CLICKED, NULL);
	box_game_label = lv_label_create(game);
	lv_obj_add_style(box_game_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(box_game_label, &font_ui_14, 0);
	lv_obj_center(box_game_label);

	build_treat();
}

static void box_open(void) {
	build_box();
	if (!box_sheet) {
		return;
	}
	box_refresh();
	lv_obj_set_hidden(box_sheet, false);
	lv_obj_move_foreground(box_sheet);
	box_visible = true;
}

static void box_open_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (box_visible) {
		box_close();
		return;
	}
	box_open();
}

static void find_take_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || box_visible) {
		return;
	}
	if (petwatch_drop_take() < 0) {
		return;
	}
	toast_success(tr("pet_forage_toast"));
	if (box_visible) {
		box_refresh();
	}
	paint();
}

// ---------------------------------------------------------------------------
// The wardrobe
// ---------------------------------------------------------------------------

static bool item_is_on(const pet_state_t *st, int item) {
	if (item == PET_ITEM_STREAK_FREEZE) {
		return false; // a freeze is a stock, not something being worn
	}
	if (item >= (int)PET_ITEM_FIRST_ROOM) {
		return pet_room(st) == 1 + (item - (int)PET_ITEM_FIRST_ROOM);
	}
	return pet_wearing(st) == item;
}

static void wardrobe_refresh(void) {
	if (!wardrobe_screen) {
		return;
	}
	const pet_state_t *st = petwatch_state();
	lv_label_set_text_fmt(wardrobe_balance, "%d", st->notes);

	for (int i = 0; i < PET_ITEM_COUNT; i++) {
		if (!wardrobe_value[i]) {
			continue;
		}
		if (i == PET_ITEM_STREAK_FREEZE && pet_freezes(st) > 0) {
			lv_label_set_text_fmt(wardrobe_value[i], tr("pet_freezes"), pet_freezes(st));
		} else if (item_is_on(st, i)) {
			lv_label_set_text(wardrobe_value[i], tr("pet_wearing_now"));
		} else if (pet_owned(st, (pet_item_t)i)) {
			lv_label_set_text(wardrobe_value[i], tr("pet_owned"));
		} else {
			lv_label_set_text_fmt(wardrobe_value[i], tr("pet_price"), pet_item_price((pet_item_t)i));
		}
	}
}

static void wardrobe_item_cb(lv_event_t *e) {
	pet_item_t item = (pet_item_t)(intptr_t)lv_event_get_user_data(e);
	const pet_state_t *st = petwatch_state();

	if (item == PET_ITEM_STREAK_FREEZE) {
		// Stackable, so it is bought every time rather than worn.
		if (petwatch_buy(item)) {
			toast_success(tr("pet_bought"));
		} else {
			toast_plain(tr("pet_not_enough"));
		}
	} else if (pet_owned(st, item)) {
		if (item_is_on(st, item)) {
			toast_plain(tr("pet_already_worn"));
		} else if (petwatch_wear(item)) {
			toast_plain(tr("pet_worn"));
		}
	} else if (petwatch_buy(item)) {
		toast_success(tr("pet_bought"));
	} else {
		toast_plain(tr("pet_not_enough"));
	}

	wardrobe_refresh();
	paint();
}

static void build_wardrobe(void) {
	if (wardrobe_screen || !page_cfg) {
		return;
	}

	wardrobe_screen = lv_obj_create(petpage_screen);
	lv_obj_set_size(wardrobe_screen, page_cfg->screen_width, page_cfg->screen_height);
	lv_obj_set_pos(wardrobe_screen, 0, 0);
	lv_obj_set_style_pad_all(wardrobe_screen, 0, 0);
	lv_obj_set_style_border_width(wardrobe_screen, 0, 0);
	lv_obj_set_scrollable(wardrobe_screen, false);
	lv_obj_set_hidden(wardrobe_screen, true);

	wardrobe_column = settingsrow_page(wardrobe_screen, page_cfg, "pet_wardrobe");
	settingsrow_add(wardrobe_column, "pet_notes_balance", &wardrobe_balance, NULL, NULL);

	for (int i = 0; i < PET_ITEM_COUNT; i++) {
		pet_item_t item = (pet_item_t)i;
		lv_obj_t *row = settingsrow_add(wardrobe_column, pet_item_tag(item), &wardrobe_value[i], wardrobe_item_cb,
										(void *)(intptr_t)i);
		if (item == PET_ITEM_STREAK_FREEZE || item >= PET_ITEM_FIRST_ROOM) {
			settingsrow_add_caption(row, item == PET_ITEM_STREAK_FREEZE ? tr("pet_freeze_note") : tr("pet_room_note"));
		}
	}

	wardrobe_refresh();
}

static void wardrobe_open_cb(lv_event_t *e) {
	(void)e;
	if (!page_built) {
		return;
	}
	if (wardrobe_visible) {
		// The star is a door that closes behind you.
		wardrobe_close();
		return;
	}
	build_wardrobe();
	if (!wardrobe_screen) {
		return;
	}
	wardrobe_refresh();
	lv_obj_set_hidden(wardrobe_screen, false);
	lv_obj_move_foreground(wardrobe_screen);
	wardrobe_visible = true;
}

static void wardrobe_close(void) {
	if (!wardrobe_screen) {
		return;
	}
	lv_obj_set_hidden(wardrobe_screen, true);
	wardrobe_visible = false;
}

// ---------------------------------------------------------------------------
// The name
// ---------------------------------------------------------------------------

static void customise_refresh(void) {
	if (!customise_screen) {
		return;
	}
	char name[40];
	pet_name(name, sizeof(name));
	lv_label_set_text(customise_name_value, name);
}

static void name_dialog_open(void) {
	if (!name_layer) {
		return;
	}
	char name[40];
	pet_name(name, sizeof(name));
	lv_textarea_set_text(name_field, name);
	if (name_keyboard) {
		keyboard_set_field(name_keyboard, name_field);
		keyboard_reset(name_keyboard);
		keyboard_set_visible(name_keyboard, true);
	}
	lv_obj_set_hidden(name_layer, false);
	lv_obj_move_foreground(name_layer);
	lv_obj_add_state(name_field, LV_STATE_FOCUSED);
}

static void name_dialog_close(void) {
	if (name_keyboard) {
		keyboard_set_visible(name_keyboard, false);
	}
	if (name_layer) {
		lv_obj_set_hidden(name_layer, true);
	}
}

static void name_accept_cb(lv_event_t *e) {
	(void)e;
	if (!name_field) {
		return;
	}
	petwatch_set_name(lv_textarea_get_text(name_field));
	name_dialog_close();
	customise_refresh();
	paint();
}

static void name_cancel_cb(lv_event_t *e) {
	(void)e;
	name_dialog_close();
}

static void name_row_cb(lv_event_t *e) {
	(void)e;
	name_dialog_open();
}

static void customise_open(void) {
	if (!customise_screen) {
		return;
	}
	customise_refresh();
	lv_obj_set_hidden(customise_screen, false);
	lv_obj_move_foreground(customise_screen);
	customise_visible = true;
}

static void customise_close(void) {
	if (!customise_screen) {
		return;
	}
	lv_obj_set_hidden(customise_screen, true);
	customise_visible = false;
}

static void customise_close_cb(lv_event_t *e) {
	(void)e;
	customise_close();
}

static void customise_open_cb(lv_event_t *e) {
	(void)e;
	if (customise_visible) {
		customise_close();
		return;
	}
	customise_open();
}

// The heading of a sheet. A sheet fills the screen and covers the page's own
// title row -- but not the shell's back button, which is drawn above every
// screen. Its heading therefore starts where the page titles start, beside the
// chevron on the chevron's line, or the first word reads as "‹ur pet".
static void place_sheet_heading(lv_obj_t *label) {
	const int left = settingsrow_header_text_left(page_cfg);
	const int button = settingsrow_corner_button_size(page_cfg);
	const int air = page_cfg->screen_width < 320 ? 6 : 14;
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, page_cfg->screen_width - left - page_cfg->padding - button - air);
	lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
	lv_obj_align(label, LV_ALIGN_TOP_LEFT, left, settingsrow_header_text_top(page_cfg));
}

static void build_customise(void) {
	if (customise_screen || !page_cfg) {
		return;
	}

	const int pad = page_cfg->padding;
	const int card_w = page_cfg->screen_width - 2 * pad;

	customise_screen = lv_obj_create(petpage_screen);
	lv_obj_set_size(customise_screen, page_cfg->screen_width, page_cfg->screen_height);
	lv_obj_set_pos(customise_screen, 0, 0);
	lv_obj_set_style_pad_all(customise_screen, 0, 0);
	lv_obj_set_style_border_width(customise_screen, 0, 0);
	lv_obj_add_style(customise_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(customise_screen, false);
	lv_obj_set_hidden(customise_screen, true);

	lv_obj_t *heading = lv_label_create(customise_screen);
	lv_label_set_text(heading, tr("pet_customise"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_18, 0);
	place_sheet_heading(heading);

	lv_obj_t *close = lv_btn_create(customise_screen);
	settingsrow_place_corner_button(close, page_cfg, 0);
	lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(close, 0, 0);
	lv_obj_set_style_shadow_width(close, 0, 0);
	lv_obj_set_style_pad_all(close, 0, 0);
	lv_obj_add_event_cb(close, (lv_event_cb_t)customise_close_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *close_icon = icon_image(close, &icon_close, settingsrow_corner_button_size(page_cfg) * 5 / 9);
	lv_obj_add_style(close_icon, &theme_style_icon, 0);
	lv_obj_center(close_icon);

	// The name is a row of its own: the one thing on this sheet, and the one
	// thing on the page that is typed rather than tapped.
	int y = pad + page_cfg->top_bar_height + 44;
	lv_obj_t *name_row = lv_btn_create(customise_screen);
	lv_obj_set_size(name_row, card_w, 48);
	lv_obj_set_pos(name_row, pad, y);
	lv_obj_set_style_radius(name_row, 12, 0);
	lv_obj_set_style_bg_color(name_row, theme()->surface, 0);
	lv_obj_set_style_bg_opa(name_row, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(name_row, 0, 0);
	lv_obj_set_style_shadow_width(name_row, 0, 0);
	lv_obj_set_style_pad_all(name_row, 0, 0);
	lv_obj_add_event_cb(name_row, name_row_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *name_caption = lv_label_create(name_row);
	lv_label_set_text(name_caption, tr("name"));
	lv_obj_add_style(name_caption, &theme_style_text, 0);
	lv_obj_set_style_text_font(name_caption, &font_ui_16, 0);
	lv_obj_align(name_caption, LV_ALIGN_LEFT_MID, 14, 0);

	customise_name_value = lv_label_create(name_row);
	lv_obj_add_style(customise_name_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(customise_name_value, &font_ui_16, 0);
	lv_obj_set_width(customise_name_value, card_w - 100);
	lv_obj_set_style_text_align(customise_name_value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode(customise_name_value, LV_LABEL_LONG_DOT);
	lv_obj_align(customise_name_value, LV_ALIGN_RIGHT_MID, -14, 0);
	lv_label_set_text(customise_name_value, "");

	// The name's own layer, with the keyboard under it.
	name_layer = lv_obj_create(customise_screen);
	lv_obj_set_size(name_layer, page_cfg->screen_width, page_cfg->screen_height);
	lv_obj_set_pos(name_layer, 0, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_scrollable(name_layer, false);
	lv_obj_set_hidden(name_layer, true);

	lv_obj_t *name_heading = lv_label_create(name_layer);
	lv_label_set_text(name_heading, tr("name"));
	lv_obj_add_style(name_heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(name_heading, &font_ui_18, 0);
	place_sheet_heading(name_heading);

	lv_obj_t *cancel = lv_btn_create(name_layer);
	settingsrow_place_corner_button(cancel, page_cfg, 0);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, name_cancel_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *cancel_icon = icon_image(cancel, &icon_close, settingsrow_corner_button_size(page_cfg) * 5 / 9);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	name_field = lv_textarea_create(name_layer);
	lv_textarea_set_one_line(name_field, true);
	lv_textarea_set_max_length(name_field, 24);
	lv_textarea_set_placeholder_text(name_field, tr("name"));
	lv_obj_set_size(name_field, card_w, 54);
	lv_obj_set_scrollbar_mode(name_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(name_field, LV_ALIGN_TOP_LEFT, pad, pad + page_cfg->top_bar_height + 46);
	lv_obj_add_style(name_field, &theme_style_card, 0);
	lv_obj_set_style_radius(name_field, 12, 0);
	lv_obj_set_style_border_width(name_field, 0, 0);
	lv_obj_set_style_shadow_width(name_field, 0, 0);
	lv_obj_set_style_pad_all(name_field, 12, 0);
	lv_obj_set_style_text_font(name_field, &font_ui_18, 0);

	name_keyboard = keyboard_create(name_layer, page_cfg->screen_width,
									page_cfg->screen_width < 320 ? 150 : 316, name_field, NULL, "ok",
									name_accept_cb, NULL);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static void consume_events(void) {
	int achievement = petwatch_take_achievement();
	if (achievement != 0) {
		char line[72];
		snprintf(line, sizeof(line), "%s: %s", tr("pet_achievement"), tr(pet_achievement_tag(achievement)));
		toast_success(line);
	}

	int events = petwatch_take_events();
	if (events & PET_EVENT_DIZZY) {
		toast_plain(tr("pet_dizzy_toast"));
	} else if (events & PET_EVENT_FORAGE) {
		toast_success(tr("pet_forage_toast"));
	} else if (events & PET_EVENT_DROP) {
		toast_success(tr("pet_drop_toast"));
	} else if (events & PET_EVENT_STAGE_UP) {
		toast_success(tr("pet_stage_toast"));
	} else if (events & PET_EVENT_GOAL_MET) {
		toast_success(tr("pet_goal_toast"));
	}
}

// ---------------------------------------------------------------------------
// The tiny reactions
//
// A button or a finger is answered by the animal itself, for a few seconds: a
// jump when the volume goes up, a duck when it comes down, a lean for the next
// track, and a bobbing heart for a tap. A reaction costs what a breath costs, and the
// one-second frame that already runs draws it. Nothing is timed with a new
// timer except the Petting Mode meter, which needs to fill faster than that.
// ---------------------------------------------------------------------------

static void start_reaction_for(int what, int secs) {
	if (!page_built) {
		return;
	}
	reaction = what;
	reaction_until = petwatch_now() + secs;
	show(parts.heart, what == REACT_HEART);
	paint(); // the hearts are placed against the head as it is now
	apply_frame();
}

static void start_reaction(int what) { start_reaction_for(what, PET_REACTION_SECS); }

static void end_reaction(void) {
	if (reaction == REACT_NONE) {
		return;
	}
	reaction = REACT_NONE;
	show(parts.heart, false);
	apply_frame();
}

// ---------------------------------------------------------------------------
// A little life
//
// Between the two breaths he does a few things nobody asked for. He blinks every
// three to six seconds. When nobody is touching the page he fidgets every eight
// to fourteen -- a hop, a lean to one side, a shake -- for two seconds. And while
// he is feeding he dances (see apply_frame()). The dice are one small generator
// and the seconds are the page's own tick, so nothing here asks the clock for
// more than the page already does.
//
// The blink is the one thing quicker than the tick: it lasts a hundred and forty
// milliseconds, so it has a timer of its own, made when the eyes shut and gone
// when they open.
// ---------------------------------------------------------------------------

#define PET_BLINK_MS 140
#define PET_FIDGET_SECS 2

static int roll(int sides) {
	dice = dice * 1664525u + 1013904223u;
	return (int)((dice >> 16) % (uint32_t)sides);
}

static void blink_end_cb(lv_timer_t *timer) {
	(void)timer;
	blink_timer = NULL; // a one-shot: LVGL deletes it when this returns
	if (page_built && gear.lids) {
		gear.lids = false;
		lv_obj_invalidate(parts.gear);
	}
}

static void blink_stop(void) {
	if (blink_timer) {
		lv_timer_delete(blink_timer);
		blink_timer = NULL;
	}
	gear.lids = false;
}

static void blink_start(void) {
	if (!page_built || blink_timer || gear.eyes == GEAR_EYES_NONE) {
		return; // a sleeper's eyes are shut already
	}
	blink_timer = lv_timer_create(blink_end_cb, PET_BLINK_MS, NULL);
	if (!blink_timer) {
		return; // no memory for it: no blink, rather than eyes that stay shut
	}
	lv_timer_set_repeat_count(blink_timer, 1);
	gear.lids = true;
	lv_obj_invalidate(parts.gear);
}

// He fidgets only when nobody is looking for anything else of him: awake, not
// feeding (that is dance enough), no reaction under way, no finger down and no
// sheet open over the card.
static bool fidget_free(void) {
	const bool sheet = box_visible || treat_visible || tap_visible || wardrobe_visible || customise_visible || (name_layer && !lv_obj_is_hidden(name_layer));
	return reaction == REACT_NONE && !holding && !feeding_now && gear.eyes != GEAR_EYES_NONE && !sheet;
}

static void life_tick(void) {
	if (!page_built) {
		return;
	}
	if (--blink_wait <= 0) {
		blink_start();
		blink_wait = 3 + roll(4);
	}
	if (--fidget_wait <= 0) {
		fidget_wait = 8 + roll(7);
		if (fidget_free()) {
			static const int moves[4] = {REACT_HOP, REACT_LEAN_L, REACT_LEAN_R, REACT_WAVE};
			start_reaction_for(moves[roll(4)], PET_FIDGET_SECS);
		}
	}
}

bool petpage_blinking(void) { return page_built && gear.lids; }

static void hide_hold_meter(void) {
	for (int i = 0; i < 3; i++) {
		show(parts.hold_heart[i], false);
	}
}

// ---------------------------------------------------------------------------
// Petting Mode
//
// A two-second hold is a fuss, not a tap: the meter fills a heart at a time,
// and when it is full the day's groom is paid -- once a day, because a heart
// that refills by holding a finger down is not worth six points.
// ---------------------------------------------------------------------------

static void hold_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (!holding) {
		if (hold_timer) {
			lv_timer_delete(hold_timer);
			hold_timer = NULL;
		}
		return;
	}
	if (hold_step < PET_HOLD_STEPS) {
		show(parts.hold_heart[hold_step], true);
		hold_step++;
		return;
	}

	// The meter is full. A hold that paid is not also a tap, and the click its
	// release reports is thrown away by pet_hold_cb(); a hold that has not
	// filled is a slow tap, and it still pets the animal.
	holding = false;
	hold_paid = true;
	if (hold_timer) {
		lv_timer_delete(hold_timer);
		hold_timer = NULL;
	}
	hide_hold_meter();
	(void)petwatch_hold();
	start_reaction(REACT_HEART);
	paint_values();
}

static void pet_hold_cb(lv_event_t *e) {
	const lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_LONG_PRESSED) {
		if (holding || switcher_back_drag_active()) {
			return;
		}
		// The meter shows from the first moment, because the hand that is
		// holding needs to see that holding is doing something.
		holding = true;
		hold_paid = false;
		hold_step = 0;
		hide_hold_meter();
		show(parts.hold_heart[0], true); // the first heart is the press itself
		hold_step = 1;
		if (!hold_timer) {
			hold_timer = lv_timer_create(hold_timer_cb, PET_HOLD_STEP_MS, NULL);
		}
		return;
	}

	// LVGL reports a hold's release as a click as well, so a hold that *paid*
	// would be counted twice. Only that one is swallowed: LVGL calls a press
	// long after 400 ms, which is what an ordinary tap on the glass lasts, and
	// swallowing every long press is a page where tapping the animal does
	// nothing at all.
	if (code == LV_EVENT_RELEASED) {
		if (hold_paid) {
			swallow_until = petwatch_now() + 2;
			hold_paid = false;
		}
	} else {
		hold_paid = false; // PRESS_LOST: no click follows, nothing to swallow
	}
	if (holding) {
		holding = false;
		hide_hold_meter();
	}
}

lv_obj_t *petpage_box(void) { return page_built ? parts.box : NULL; }

lv_obj_t *petpage_find(void) { return page_built ? parts.find : NULL; }

lv_obj_t *petpage_scenery(void) { return page_built ? scenery : NULL; }

// The hardware side of the same trick: a volume or a track key, wherever it was
// pressed, is passed in from the shell and answered here. Both keys keep doing
// their proper job -- this only adds a face to them.
void petpage_poke(pet_poke_t what) {
	if (!page_visible || !page_built) {
		return;
	}
	switch (what) {
	case PET_POKE_VOLUME_UP:
		start_reaction(REACT_JUMP);
		break;
	case PET_POKE_VOLUME_DOWN:
		start_reaction(REACT_DUCK);
		break;
	case PET_POKE_NEXT:
		start_reaction(REACT_LEAN_R);
		break;
	case PET_POKE_PREV:
		start_reaction(REACT_LEAN_L);
		break;
	default:
		break;
	}
}

static void pet_tap_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (petwatch_now() <= swallow_until) {
		return; // the click that closed a hold
	}
	// Keep the reward and its cooldown, but always give a visible response.
	(void)petwatch_pet_now();
	start_reaction(REACT_HEART);
}

// The shell's back chevron closes whatever sheet is on top before it leaves.
static bool pet_back_guard(void) {
	if (name_layer && !lv_obj_is_hidden(name_layer)) {
		name_dialog_close();
		return true;
	}
	if (customise_visible) {
		customise_close();
		return true;
	}
	if (tap_visible) {
		tap_close();
		return true;
	}
	if (wardrobe_visible) {
		wardrobe_close();
		return true;
	}
	return false;
}

static void page_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (!page_visible) {
		return;
	}
	consume_events();
	paint_clock();
	paint_values();
	advance_frame();
	if (reaction != REACT_NONE && petwatch_now() >= reaction_until) {
		end_reaction();
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	page_visible = true;
	consume_events();
	paint();
	sync_animation();
	if (!page_timer) {
		page_timer = lv_timer_create(page_timer_cb, 1000, NULL);
		if (page_timer) {
			power_pause_in_standby(page_timer);
		}
	}
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	page_visible = false;
	name_dialog_close();
	customise_close();
	tap_close();
	wardrobe_close();
	if (page_timer) {
		lv_timer_delete(page_timer);
		page_timer = NULL;
	}
	stop_animations();
	// The last couple of minutes of music are in memory only: put them on the
	// card now rather than losing them to a player switched off from here.
	petwatch_flush();
}

static void theme_change_cb(void) { paint(); }

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The hour and the month
//
// The card knows what time it is, because the player does: after eight in the
// evening it goes dark, a moon comes up where the sun was, and a small lamp
// stands on the grass. The month decides what falls through the air -- rain in
// spring, leaves in autumn, snow in winter, nothing in summer.
//
// None of this is a timer of its own. The scenery is told the sky when the hour
// changes, and the five things falling are moved by the frame the page already
// draws. A card with nothing falling costs nothing at all.
// ---------------------------------------------------------------------------

static lv_obj_t *build_flake(void) {
	lv_obj_t *o = bar_piece(habitat, PET_SNOW, 2);
	lv_obj_set_size(o, 4, 4);
	show(o, false);
	return o;
}

// The dark goes in first, under the scenery, which has a night of its own: it
// is a wash of the card the name and the words are on, not of the animal and
// not of the moon, which stay as bright as they were.
static void build_night_wash(int card_w, int card_h) {
	night_wash = plain(habitat);
	lv_obj_set_size(night_wash, card_w, card_h);
	lv_obj_set_pos(night_wash, 0, 0);
	lv_obj_set_style_bg_color(night_wash, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(night_wash, LV_OPA_40, 0);
	lv_obj_set_style_radius(night_wash, bp_pick(14, 22), 0);
	show(night_wash, false);
}

// What falls goes in over the scenery.
static void build_falling(int card_w) {
	for (int i = 0; i < PET_FLAKES; i++) {
		flake[i] = build_flake();
		flake_home_x[i] = 16 + i * (card_w - 32) / (PET_FLAKES - 1);
	}
	clock_layer_built = true;
}

static int shown_hour = -1;
static int shown_month = -1;
static int shown_known = -1;

static void paint_clock(void) {
	if (!page_built || !clock_layer_built) {
		return;
	}

	const time_t now = (time_t)petwatch_now();
	struct tm tm;
	int hour = 12;
	int month = 7;
	if (localtime_r(&now, &tm)) {
		hour = tm.tm_hour;
		month = tm.tm_mon + 1;
	}

	const int known = clock_is_set() ? 1 : 0;
	// Nothing changes but a handful of times a day; the page asks every second,
	// so it answers from what it already drew until the answer is different.
	if (hour == shown_hour && month == shown_month && known == shown_known) {
		return;
	}
	shown_hour = hour;
	shown_month = month;
	shown_known = known;

	const bool night = known && (hour >= 20 || hour < 7);

	show(night_wash, night);

	// What falls through the air, by month: rain in spring, leaves in autumn,
	// snow in winter. Summer has nothing falling -- the sun is the season.
	int falling = 0; // 1 rain, 2 leaf, 3 snow
	if (month == 12 || month <= 2) {
		falling = 3;
	} else if (month >= 3 && month <= 5) {
		falling = 1;
	} else if (month >= 9 && month <= 11) {
		falling = 2;
	}

	// The sky is the scenery's: the sun by day, the moon and the lamp at night,
	// and in the spring's rain, grey cloud and no sun.
	clock_known = known;
	clock_night = night;
	clock_rain = falling == 1 && !night;
	scene_refresh();

	for (int i = 0; i < PET_FLAKES; i++) {
		show(flake[i], falling != 0);
		if (falling == 1) {
			set_color(flake[i], PET_RAIN);
			lv_obj_set_size(flake[i], 2, 6);
		} else if (falling == 2) {
			set_color(flake[i], PET_LEAF);
			lv_obj_set_size(flake[i], 7, 4);
		} else {
			set_color(flake[i], PET_SNOW);
			lv_obj_set_size(flake[i], 4, 4);
		}
	}
}

static void build(void) {
	if (!page_cfg || page_built) {
		return;
	}

	lv_obj_add_style(petpage_screen, &theme_style_screen, 0);
	lv_obj_t *title = settingsrow_title(petpage_screen, page_cfg, "pet");
	settingsrow_title_corner_slots(title, page_cfg, 3);

	// Three corner controls: the wardrobe behind the star (slot 0, the
	// outermost), the name behind the pencil, and the day's tap-along behind the
	// note.
	corner_button(page_cfg, 0, &icon_star, wardrobe_open_cb);
	corner_button(page_cfg, 1, &icon_pencil, customise_open_cb);
	corner_button(page_cfg, 2, &icon_music_note, tap_open_cb);

	const int pad = page_cfg->padding;
	const int card_w = page_cfg->screen_width - 2 * pad;
	const int content_top = settingsrow_content_top(page_cfg);
	const int line_h = lv_font_get_line_height(&font_ui_14);
	const int footer_h = 2 * line_h + 6;
	int habitat_h = page_cfg->screen_height - content_top - footer_h - pad;
	if (habitat_h < 140) {
		habitat_h = 140;
	}
	const int radius = bp_pick(14, 22);
	card_w_px = card_w;
	card_h_px = habitat_h;

	habitat = lv_obj_create(petpage_screen);
	lv_obj_add_style(habitat, &theme_style_card, 0);
	lv_obj_set_size(habitat, card_w, habitat_h);
	lv_obj_set_pos(habitat, pad, content_top);
	lv_obj_set_style_radius(habitat, radius, 0);
	lv_obj_set_style_shadow_width(habitat, 0, 0);
	// No outline: the card is a colour, not a box.
	lv_obj_set_style_border_width(habitat, 0, 0);
	lv_obj_set_style_pad_all(habitat, 0, 0);
	lv_obj_set_scrollable(habitat, false);

	// The room under everything else in the card.
	room_layer = plain(habitat);
	lv_obj_set_size(room_layer, card_w, habitat_h);
	lv_obj_set_pos(room_layer, 0, 0);
	lv_obj_set_style_radius(room_layer, radius, 0);
	lv_obj_set_style_bg_opa(room_layer, LV_OPA_20, 0);

	build_night_wash(card_w, habitat_h);
	build_scenery(card_w);
	build_falling(card_w);

	// ---- the top of the card -------------------------------------------
	name_label = lv_label_create(habitat);
	lv_obj_add_style(name_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(name_label, &font_ui_20, 0);
	lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name_label, card_w - PET_HUD_W - 24);
	lv_obj_set_pos(name_label, 12, 6);
	lv_label_set_text(name_label, PET_DEFAULT_NAME);

	stage_label = lv_label_create(habitat);
	lv_obj_add_style(stage_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(stage_label, &font_ui_14, 0);
	// One line only: LV_LABEL_LONG_DOT needs a pinned height to have an edge
	// to dot at, or a long animal-and-mood pair wraps into the creature.
	lv_obj_set_width(stage_label, card_w - PET_HUD_W - 24);
	lv_obj_set_height(stage_label, lv_font_get_line_height(&font_ui_14));
	lv_label_set_long_mode(stage_label, LV_LABEL_LONG_DOT);
	lv_obj_set_pos(stage_label, 12, 30);
	lv_label_set_text(stage_label, "");

	// The month's listening, under the stage: one word, or nothing at all
	// while there are not yet three days to call a character.
	temperament_label = lv_label_create(habitat);
	lv_obj_add_style(temperament_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(temperament_label, &font_ui_14, 0);
	lv_obj_set_pos(temperament_label, 12, 48);
	lv_obj_set_width(temperament_label, card_w - PET_HUD_W - 24);
	lv_label_set_long_mode(temperament_label, LV_LABEL_LONG_DOT);
	lv_label_set_text(temperament_label, "");

	const int hud_x = card_w - 10 - PET_HUD_W;
	health_icon = icon_image(habitat, &icon_heart_filled, 12);
	place(health_icon, hud_x, 11, 12, 12);
	health_bar = lv_bar_create(habitat);
	lv_obj_set_size(health_bar, 40, 6);
	lv_obj_set_pos(health_bar, hud_x + 16, 14);
	lv_bar_set_range(health_bar, 0, PET_HAPPY_MAX);
	lv_bar_set_value(health_bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_radius(health_bar, 3, 0);
	lv_obj_set_style_radius(health_bar, 3, LV_PART_INDICATOR);
	lv_obj_set_style_border_width(health_bar, 0, 0);
	health_value = lv_label_create(habitat);
	lv_obj_add_style(health_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(health_value, &font_ui_14, 0);
	lv_obj_set_width(health_value, 28);
	lv_obj_set_height(health_value, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_style_text_align(health_value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(health_value, hud_x + 60, 7);
	lv_label_set_text(health_value, "0");

	energy_icon = icon_image(habitat, &icon_zap, 12);
	place(energy_icon, hud_x, 29, 12, 12);
	energy_bar = lv_bar_create(habitat);
	lv_obj_set_size(energy_bar, 40, 6);
	lv_obj_set_pos(energy_bar, hud_x + 16, 32);
	lv_bar_set_range(energy_bar, 0, PET_ENERGY_MAX);
	lv_bar_set_value(energy_bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_radius(energy_bar, 3, 0);
	lv_obj_set_style_radius(energy_bar, 3, LV_PART_INDICATOR);
	lv_obj_set_style_border_width(energy_bar, 0, 0);
	energy_value = lv_label_create(habitat);
	lv_obj_add_style(energy_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(energy_value, &font_ui_14, 0);
	lv_obj_set_width(energy_value, 28);
	lv_obj_set_height(energy_value, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_style_text_align(energy_value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(energy_value, hud_x + 60, 25);
	lv_label_set_text(energy_value, "0");

	notes_icon = icon_image(habitat, &icon_music_note, 12);
	place(notes_icon, hud_x, 48, 12, 12);
	notes_value = lv_label_create(habitat);
	lv_obj_add_style(notes_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(notes_value, &font_ui_14, 0);
	lv_obj_set_width(notes_value, 28);
	lv_obj_set_height(notes_value, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_style_text_align(notes_value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(notes_value, hud_x + 60, 46);
	lv_label_set_text(notes_value, "0");

	// The Calm soundscape: four motes drifting up through the card, only for a
	// pet whose month was jazz and classical.
	for (int i = 0; i < 4; i++) {
		motes[i] = disc(habitat, lv_color_white());
		lv_obj_set_style_bg_opa(motes[i], LV_OPA_40, 0);
		mote_home[i] = habitat_h - 60 - (i % 2) * 24;
		place(motes[i], 18 + i * (card_w - 60) / 4, mote_home[i], 5 + (i % 2) * 2, 5 + (i % 2) * 2);
		show(motes[i], false);
	}

	// Notes sit in the scenery, behind the animal, and drift only during playback.
	note_a = icon_image(habitat, &icon_music_note, 14);
	place(note_a, 20, 86, 14, 14);
	note_b = icon_image(habitat, &icon_music_note, 12);
	place(note_b, card_w - 32, 100, 12, 12);
	show(note_a, false);
	show(note_b, false);

	// ---- the animal ------------------------------------------------------
	build_creature();
	// Feet on the floor line, a little into the grass: PET_BASE_Y down from here.
	lv_obj_set_pos(parts.root, (card_w - PET_CREATURE_W) / 2, 50);

	// ---- what it is feeding on ------------------------------------------
	// The status line's picture: a still note when nothing is playing, and
	// three bars that swap on the frame timer while something is.
	// The status row sits a little under the floor line. It used to begin one
	// pixel below it, which on the device -- where the grass is the same colour
	// the words are dimmed against -- read as words standing on the grass.
	status_icon = icon_image(habitat, &icon_music_note, 12);
	place(status_icon, 12, habitat_h - 23, 12, 12);

	eq_bottom = habitat_h - 16;
	for (int i = 0; i < 3; i++) {
		eq_bar[i] = bar_piece(habitat, lv_color_white(), 1);
		place(eq_bar[i], 12 + i * 5, eq_bottom - 4, 3, 4);
		show(eq_bar[i], false);
	}

	status_label = lv_label_create(habitat);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_14, 0);
	lv_obj_set_pos(status_label, 28, habitat_h - 26);
	lv_obj_set_width(status_label, card_w - 74);
	lv_obj_set_height(status_label, lv_font_get_line_height(&font_ui_14));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
	lv_label_set_text(status_label, "");

	// ---- the way out ------------------------------------------------------
	lv_obj_t *close_btn = lv_btn_create(habitat);
	lv_obj_set_size(close_btn, 30, 30);
	lv_obj_set_pos(close_btn, card_w - 36, habitat_h - 36);
	lv_obj_set_style_bg_opa(close_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(close_btn, 0, 0);
	lv_obj_set_style_shadow_width(close_btn, 0, 0);
	lv_obj_set_style_pad_all(close_btn, 0, 0);
	lv_obj_add_event_cb(close_btn, card_close_cb, LV_EVENT_CLICKED, NULL);
	card_close_icon = icon_image(close_btn, &icon_close, 14);
	lv_obj_center(card_close_icon);

	// ---- the two lines under the card ------------------------------------
	today_label = lv_label_create(petpage_screen);
	lv_obj_add_style(today_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(today_label, &font_ui_14, 0);
	lv_obj_set_width(today_label, card_w);
	lv_obj_set_pos(today_label, pad + 4, content_top + habitat_h + 6);
	lv_label_set_long_mode(today_label, LV_LABEL_LONG_DOT);
	lv_label_set_text(today_label, "");

	streak_label = lv_label_create(petpage_screen);
	lv_obj_add_style(streak_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(streak_label, &font_ui_14, 0);
	lv_obj_set_width(streak_label, card_w);
	lv_obj_set_pos(streak_label, pad + 4, content_top + habitat_h + 6 + line_h);
	lv_label_set_long_mode(streak_label, LV_LABEL_LONG_DOT);
	lv_label_set_text(streak_label, "");

	build_customise();

	lv_obj_add_event_cb(petpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(petpage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	page_built = true;
}

void petpage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	theme_register_refresh(theme_change_cb);
	// The sheets are over this page, so the chevron closes them before it can
	// close the pet.
	switcher_set_back_guard(petpage_screen, pet_back_guard);
}

void petpage_open(void) {
	if (!page_built) {
		build();
	}
	paint();
	sync_animation();
	switch_screen(petpage_screen);
}

lv_obj_t *petpage_creature(void) { return page_built ? parts.root : NULL; }
