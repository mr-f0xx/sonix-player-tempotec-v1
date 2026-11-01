#include "petpage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h" // switch_screen
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/pet/pet.h"
#include "src/system/pet/petwatch.h"

lv_obj_t *petpage_screen;

// A part made of triangles rather than of a styled rectangle: the typedef is
// up here because the parts are, the drawing itself is further down.
//
// A shape can also carry one closed outline, drawn as a run of joined lines
// after the fills. That is how the fish's body is drawn: a dozen straight
// sides are round enough for a fish, but twelve triangles wearing twelve ink
// lines are a web, so the body is filled in triangles and outlined once.
#define PET_SHAPE_TRIS 16
#define PET_SHAPE_LINE 16

typedef struct {
	lv_obj_t *obj;
	int count;
	int16_t p[PET_SHAPE_TRIS][3][2];
	lv_color_t color[PET_SHAPE_TRIS];
	int line_count;
	int16_t line[PET_SHAPE_LINE][2];
} pet_shape_t;

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
// The box is taller than the animal by the height of a party hat: a hat is
// drawn above the top of the head, and a child cannot be drawn outside its
// parent (LVGL's overflow flag extends a parent by its draw extension, which a
// plain box does not have). The root is placed from the feet, so the animal
// still stands on the floor line whatever this number is.
#define PET_CREATURE_H 126
#define PET_BASE_Y 118 // where the animal's feet sit, from the root's top

// The two meters' column, measured from the card's right edge.
#define PET_HUD_W 88

// The creature's palette. It is the one thing on the page that does not follow
// the theme: a pastel animal reads on a white card, a black card and Tokyo
// Night's blue-black alike.
#define PET_INK lv_color_hex(0x2C3E36)
#define PET_CHEEK lv_color_hex(0xF2A0A1)
#define PET_TEAR lv_color_hex(0x7FC4E8)

// The wardrobe's own colours, fixed for the same reason the animal's are.
#define PET_HAT lv_color_hex(0xE2574C)
#define PET_PHONE lv_color_hex(0x2F3B52)
#define PET_SCARF lv_color_hex(0xC2453F)
#define PET_CROWN lv_color_hex(0xE8C15A)

// The rooms, as a wash over the card, and the water of the aquarium.
#define PET_ROOM_MEADOW lv_color_hex(0x9FD9B4)
#define PET_ROOM_SUNSET lv_color_hex(0xF0A868)
#define PET_ROOM_NIGHT lv_color_hex(0x46598C)
#define PET_WATER_MEADOW lv_color_hex(0x3FA9C4)
#define PET_WATER_SUNSET lv_color_hex(0x4FA6B0)
#define PET_WATER_NIGHT lv_color_hex(0x2B4C7E)
#define PET_SAND lv_color_hex(0xE0C795)
#define PET_SAND_DEEP lv_color_hex(0xC9AB74)
#define PET_PEBBLE lv_color_hex(0x9AA7B4)
#define PET_WEED lv_color_hex(0x3E8F5F)
#define PET_WEED_LIGHT lv_color_hex(0x66B87F)
#define PET_BUBBLE lv_color_hex(0xE4F7FC)
#define PET_BUBBLES 6
#define PET_WEEDS 4

// The four homes, in the same fixed colours as the animals: a cat's rug is not
// going to be Tokyo Night's blue. The two ground bands are the same place and
// the same height as the sand, so the words along the foot of the card always
// sit on the same thing.
#define PET_CAT_RUG lv_color_hex(0xE3A0A0)
#define PET_CAT_POT lv_color_hex(0xB5704F)
#define PET_PLANT lv_color_hex(0x4E9E63)
#define PET_YARN lv_color_hex(0xE8C15A)
#define PET_GRASS lv_color_hex(0x6FAE6A)
#define PET_GRASS_DEEP lv_color_hex(0x559353)
#define PET_FENCE lv_color_hex(0xBFA680)
#define PET_BONE lv_color_hex(0xE2D3AF)

typedef struct {
	lv_obj_t *root;
	lv_obj_t *shadow;


	// A fish's fins: a forked tail behind, a dorsal on top, a pectoral under
	// the belly.
	lv_obj_t *fin_top;
	lv_obj_t *fin_tail;

	// The trunk and the head. The head is drawn after the trunk, so its ink
	// line cuts across the shoulders -- that seam is most of what makes an
	// animal read as a head on a body rather than as one round potato.
	lv_obj_t *body;
	lv_obj_t *fish_body; // a fish's own shape, from snout to peduncle
	lv_obj_t *belly;
	lv_obj_t *head;
	lv_obj_t *foot_l;
	lv_obj_t *foot_r;
	lv_obj_t *arm_l;
	lv_obj_t *arm_r;

	// Heads: pointed ears and floppy ones.
	lv_obj_t *ear_l;
	lv_obj_t *ear_r;
	lv_obj_t *jowl_l; // the dog's hanging ears, drawn: a flap on the head
	lv_obj_t *jowl_r;
	lv_obj_t *whisker[4]; // the cat's

	// Faces: a muzzle for the dog, a nose on the end of it; a gill for the fish.
	lv_obj_t *muzzle;
	lv_obj_t *nose;

	lv_obj_t *eye_l;
	lv_obj_t *eye_r;
	lv_obj_t *glint_l;
	lv_obj_t *glint_r;
	lv_obj_t *lid_l;
	lv_obj_t *lid_r;
	lv_obj_t *lid_v_l; // the upright of the cross a dizzy pet has for an eye
	lv_obj_t *lid_v_r;
	lv_obj_t *cheek_l;
	lv_obj_t *cheek_r;
	lv_obj_t *gill;
	lv_obj_t *mouth;
	lv_obj_t *mouth_line;
	lv_obj_t *mouth_open;
	lv_obj_t *tongue;
	lv_obj_t *eye_arc_l; // a blissful pet's eyes, shut in a smile
	lv_obj_t *eye_arc_r;
	lv_obj_t *tear;
	lv_obj_t *zzz;
	lv_obj_t *jowl_in_l; // the pale inside of the dog's ears
	lv_obj_t *jowl_in_r;
	lv_obj_t *brow; // the two bars over a sad pet's eyes
	lv_obj_t *note; // the pair that floats over a listening one

	// What the wardrobe sells. Headwear hides the ears and the little "z z",
	// because two hats on one head is not a hat.
	lv_obj_t *hat_cone;
	lv_obj_t *hat_brim;
	lv_obj_t *phone_band;
	lv_obj_t *phone_left;
	lv_obj_t *phone_right;
	lv_obj_t *scarf;
	lv_obj_t *scarf_tail;
	lv_obj_t *crown_band;
	lv_obj_t *crown_spike[3];
} pet_parts_t;

static pet_parts_t parts;

static gui_config_t *page_cfg;
static bool page_built;
static bool page_visible;
static lv_timer_t *page_timer;

static lv_obj_t *habitat;
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

// The aquarium: the sand, four weeds and six slow bubbles, shown only for a
// fish.
static lv_obj_t *room_layer;
static lv_obj_t *surface; // the light band where the water meets the air
static lv_obj_t *sand;
static lv_obj_t *pebbles[3];
static lv_obj_t *weeds[PET_WEEDS];
static lv_obj_t *bubbles[PET_BUBBLES];

// The drifting notes, while something is playing.
static lv_obj_t *note_a;
static lv_obj_t *note_b;

// The wardrobe, the chooser and the name dialog: sheets over this page rather
// than screens of their own, so the shell's back chevron closes them in turn.
static lv_obj_t *wardrobe_screen;
static lv_obj_t *wardrobe_column;
static lv_obj_t *wardrobe_balance;
static lv_obj_t *wardrobe_value[PET_ITEM_COUNT];
static bool wardrobe_visible;

static lv_obj_t *customise_screen;
static lv_obj_t *customise_name_value;
static lv_obj_t *species_chip[PET_SPECIES_COUNT];
static lv_obj_t *species_chip_label[PET_SPECIES_COUNT];
static bool customise_visible;

// The month's listening, on the card under the stage.
static lv_obj_t *temperament_label;

// The Calm soundscape: four motes drifting up while the month says calm.
static lv_obj_t *motes[4];

// The pointed parts: the cat's ears and the three fins and the body a fish is
// made of. A fish has no head object -- it is one shape, snout to peduncle,
// with the tail behind it.
static pet_shape_t shape_ear_l;
static pet_shape_t shape_ear_r;
static pet_shape_t shape_fish_body;
static pet_shape_t shape_fin_tail;
static pet_shape_t shape_fin_top;
static pet_shape_t shape_jowl_l; // the dog's hanging ears
static pet_shape_t shape_jowl_r;
static pet_shape_t shape_jowl_in_l; // and the pale inside of each
static pet_shape_t shape_jowl_in_r;
static pet_shape_t shape_plant; // three leaves standing out of the cat's pot
static pet_shape_t shape_weed[PET_WEEDS]; // the tank's weeds, blade and fronds
static pet_shape_t shape_brow; // a sad pet's brows
static pet_shape_t shape_note; // and a listening one's notes

// What each animal has behind it, and whose it is. Built once with the page,
// shown and hidden by species -- no rebuilding when the pet changes.
#define PET_SCENE_PARTS 24

typedef struct {
	lv_obj_t *obj;
	pet_species_t species;
} scene_part_t;

static scene_part_t scene[PET_SCENE_PARTS];
static int scene_count;

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
static void wardrobe_refresh(void);
static void wardrobe_close(void);
static void customise_open(void);
static void customise_close(void);
static void customise_refresh(void);
static void customise_close_cb(lv_event_t *e);
static void name_dialog_close(void);
static void pet_tap_cb(lv_event_t *e);
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

// One arc is both a smile and a frown: LVGL draws the indicator clockwise from
// the first angle, so the bottom half is a smile and the top half is the frown.
static lv_obj_t *ink_arc(lv_obj_t *parent) {
	lv_obj_t *a = lv_arc_create(parent);
	lv_obj_remove_style(a, NULL, LV_PART_KNOB);
	lv_obj_set_clickable(a, false);
	lv_obj_set_scrollable(a, false);
	lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(a, 0, 0);
	lv_obj_set_style_arc_width(a, 2, LV_PART_MAIN);
	lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_arc_width(a, 2, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(a, PET_INK, LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
	return a;
}

// ---------------------------------------------------------------------------
// Triangles
//
// A rectangle cannot be a pointed ear or a wing, and a turned one loses its
// corners -- and its ink line -- to the layer LVGL draws it through. So the
// shapes that have to be pointed are drawn rather than styled: a handful of
// triangles in a DRAW_MAIN callback, the ink one first and grown about the
// middle, the fill on top of it. Nothing is allocated while the page is up,
// nothing is animated on a timer, and nothing is drawn until the animal has
// changed, which is what makes a drawing as cheap as a rectangle here.
// ---------------------------------------------------------------------------

static void shape_draw_cb(lv_event_t *e) {
	const pet_shape_t *shape = lv_event_get_user_data(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!shape || !shape->obj) {
		return;
	}
	lv_area_t coords;
	lv_obj_get_coords(shape->obj, &coords);
	for (int i = 0; i < shape->count; i++) {
		lv_draw_triangle_dsc_t dsc;
		lv_draw_triangle_dsc_init(&dsc);
		dsc.color = shape->color[i];
		dsc.opa = LV_OPA_COVER;
		for (int k = 0; k < 3; k++) {
			dsc.p[k].x = coords.x1 + shape->p[i][k][0];
			dsc.p[k].y = coords.y1 + shape->p[i][k][1];
		}
		lv_draw_triangle(layer, &dsc);
	}

	// The outline, if this shape is a polygon: one line per side, each with a
	// round cap so the corners close up. Nothing is allocated for it -- a
	// polyline dsc would copy its points into LVGL's heap on every draw.
	for (int i = 0; i < shape->line_count; i++) {
		const int j = (i + 1) % shape->line_count;
		lv_draw_line_dsc_t dsc;
		lv_draw_line_dsc_init(&dsc);
		dsc.color = PET_INK;
		dsc.opa = LV_OPA_COVER;
		dsc.width = 2;
		dsc.round_start = 1;
		dsc.round_end = 1;
		dsc.p1.x = coords.x1 + shape->line[i][0];
		dsc.p1.y = coords.y1 + shape->line[i][1];
		dsc.p2.x = coords.x1 + shape->line[j][0];
		dsc.p2.y = coords.y1 + shape->line[j][1];
		lv_draw_line(layer, &dsc);
	}
}

// One corner of the outline that goes round a shape.
static void shape_line(pet_shape_t *s, int x, int y) {
	if (s->line_count < PET_SHAPE_LINE) {
		s->line[s->line_count][0] = (int16_t)x;
		s->line[s->line_count][1] = (int16_t)y;
		s->line_count++;
	}
}

static lv_obj_t *shape_create(lv_obj_t *parent, pet_shape_t *shape) {
	memset(shape, 0, sizeof(*shape));
	shape->obj = plain(parent);
	lv_obj_add_event_cb(shape->obj, shape_draw_cb, LV_EVENT_DRAW_MAIN, shape);
	return shape->obj;
}

static void shape_reset(pet_shape_t *s) {
	s->count = 0;
	s->line_count = 0;
}

// One triangle, scaled about its own middle: `pct` under 100 shrinks it, over
// 100 grows it. The ink line is the same triangle at 118 per cent, drawn first.
static void shape_push(pet_shape_t *s, lv_color_t color, int pct, int dx, int dy,
					   int x0, int y0, int x1, int y1, int x2, int y2) {
	if (s->count >= PET_SHAPE_TRIS) {
		return;
	}
	const int mx = (x0 + x1 + x2) / 3;
	const int my = (y0 + y1 + y2) / 3;
	int16_t(*p)[2] = s->p[s->count];
	p[0][0] = (int16_t)(mx + (x0 - mx) * pct / 100 + dx);
	p[0][1] = (int16_t)(my + (y0 - my) * pct / 100 + dy);
	p[1][0] = (int16_t)(mx + (x1 - mx) * pct / 100 + dx);
	p[1][1] = (int16_t)(my + (y1 - my) * pct / 100 + dy);
	p[2][0] = (int16_t)(mx + (x2 - mx) * pct / 100 + dx);
	p[2][1] = (int16_t)(my + (y2 - my) * pct / 100 + dy);
	s->color[s->count] = color;
	s->count++;
}

// A filled triangle wearing the ink line every other part wears.
static void shape_triangle(pet_shape_t *s, lv_color_t fill,
						   int x0, int y0, int x1, int y1, int x2, int y2) {
	shape_push(s, PET_INK, 124, 0, 0, x0, y0, x1, y1, x2, y2);
	shape_push(s, fill, 100, 0, 0, x0, y0, x1, y1, x2, y2);
}

// Place the object over its drawing and ask for it again.
static void shape_place(pet_shape_t *s, int x, int y, int w, int h) {
	if (!s->obj) {
		return;
	}
	place(s->obj, x, y, w, h);
	lv_obj_invalidate(s->obj);
}

// A whole polygon: filled in triangles, then outlined once. The triangles of
// the fan each reach two corners on rather than one, so neighbours lap over
// each other instead of sharing an edge -- two of the same flat colour that
// only touch leave a hairline of what is behind them, which is what made the
// first fish look drawn in pieces. The outline is one line round the lot, so
// the number of corners costs nothing.
static void shape_fan(pet_shape_t *s, lv_color_t color, const int *xs, const int *ys, int n) {
	for (int i = 1; i + 2 < n && s->count < PET_SHAPE_TRIS; i++) {
		shape_push(s, color, 100, 0, 0, xs[0], ys[0], xs[i], ys[i], xs[i + 2], ys[i + 2]);
	}
}

static void shape_polygon(pet_shape_t *s, lv_color_t color, const int *xs, const int *ys, int n) {
	shape_reset(s);
	shape_fan(s, color, xs, ys, n);
	for (int i = 0; i < n; i++) {
		shape_line(s, xs[i], ys[i]);
	}
}

// A dog's two hanging ears, one table mirrored: the corners are percentages of
// the ear's own box, so they follow the head through the stages.
//
// The first corner is where the ear meets the crown, and it is on the head's
// own outline: an ear drawn with its top out in the air leaves a step in the
// silhouette, which is what the turned rectangles had. From there the ear's
// outer edge swells to its widest at the level of the eyes and comes back to a
// rounded tip at the jaw -- a flap that hangs, rather than a bean stuck to the
// side of the head. The inner edge is a line down the head, and the face is
// drawn after it, so the cheeks are not covered.
static void draw_jowls(int cx, int head_w, int head_h, int head_top, lv_color_t fill,
		       lv_color_t pale) {
	// The ear in head units: x from the middle of the head in per cents of
	// half its width -- so a hundred is the edge of the head -- and y from the
	// top of the head in per cents of its height. Written that way the table
	// says what the ear is: it starts on the head's outline a tenth of the way
	// down from the crown, covers the side of the head down to the jaw, and
	// reaches out to a hundred and twenty-four at its middle, which is a
	// quarter of a head's width out in the air.
	static const int16_t ear[12][2] = {
		{-62, 10}, {-88, 14}, {-110, 26}, {-122, 44}, {-124, 62}, {-116, 80},
		{-102, 94}, {-84, 100}, {-72, 90}, {-70, 72}, {-72, 50}, {-76, 30},
	};
	const int n = 12;
	int min_x = 0;
	int max_x = 0;
	int min_y = 0;
	int max_y = 0;
	for (int i = 0; i < n; i++) {
		if (ear[i][0] < min_x) {
			min_x = ear[i][0];
		}
		if (ear[i][0] > max_x) {
			max_x = ear[i][0];
		}
		if (ear[i][1] < min_y) {
			min_y = ear[i][1];
		}
		if (ear[i][1] > max_y) {
			max_y = ear[i][1];
		}
	}
	const int w = (max_x - min_x) * head_w / 200;
	const int h = (max_y - min_y) * head_h / 100;
	const int left = cx + min_x * head_w / 200;
	const int top = head_top + min_y * head_h / 100;
	int xs[PET_SHAPE_LINE];
	int ys[PET_SHAPE_LINE];
	for (int side = 0; side < 2; side++) {
		// The corners are relative to the shape's own box: the draw callback
		// adds the box's corner, so absolute page positions would land
		// wherever the ear happens to be twice.
		for (int i = 0; i < n; i++) {
			const int px = (ear[i][0] - min_x) * head_w / 200;
			const int py = (ear[i][1] - min_y) * head_h / 100;
			xs[i] = side ? w - 1 - px : px;
			ys[i] = py;
		}
		pet_shape_t *shape = side ? &shape_jowl_r : &shape_jowl_l;
		shape_polygon(shape, fill, xs, ys, n);
		shape_place(shape, side ? 2 * cx - left - w : left, top, w, h);

		// The pale inside: the same corners pulled in towards the middle of
		// the ear and a little up it, so the ear keeps a rim of its own
		// colour all the way round. No ink -- an inner ear is a colour, not an
		// edge. The cat's ears say the same thing in pink.
		for (int i = 0; i < n; i++) {
			const int px = (ear[i][0] - min_x) * head_w / 200;
			const int py = (ear[i][1] - min_y) * head_h / 100;
			const int ix = w / 2 + (px - w / 2) * 62 / 100;
			const int iy = h / 2 + (py - h / 2) * 62 / 100 - h * 6 / 100;
			xs[i] = side ? w - 1 - ix : ix;
			ys[i] = iy;
		}
		pet_shape_t *inner = side ? &shape_jowl_in_r : &shape_jowl_in_l;
		shape_reset(inner);
		shape_fan(inner, pale, xs, ys, n);
		shape_place(inner, side ? 2 * cx - left - w : left, top, w, h);
	}
}

// One blade of weed: a ribbon that sways as it rises, wide at the sand and
// narrow at the tip. `phase` swaps the sway for the other direction, so two
// blades out of the same clump lean away from each other instead of lying on
// top of one another.
static int ribbon_corners(int *xs, int *ys, int base_x, int base_y, int height, int lean, bool phase) {
	enum { JOINTS = 3 };
	// The sway only ever goes one way: a blade that curls back over its own
	// root is not star-shaped from its first corner, and the fan that fills a
	// shape cannot fill it -- the top of the blade comes out with a notch in it.
	static const int sway_a[JOINTS] = {0, 2, 5};
	static const int sway_b[JOINTS] = {0, -2, -5};
	static const int half[JOINTS] = {3, 3, 2};
	int cx[JOINTS];
	int cy[JOINTS];
	for (int i = 0; i < JOINTS; i++) {
		const int sway = phase ? sway_b[i] : sway_a[i];
		cx[i] = base_x + lean * i / (JOINTS - 1) + sway;
		cy[i] = base_y - height * i / (JOINTS - 1);
	}
	int n = 0;
	for (int i = 0; i < JOINTS; i++) {
		xs[n] = cx[i] - half[i];
		ys[n] = cy[i];
		n++;
	}
	// A blunt cap rather than a point: a strand of weed has a rounded end, and
	// a point makes it a leaf.
	xs[n] = cx[JOINTS - 1];
	ys[n] = cy[JOINTS - 1] - half[JOINTS - 1];
	n++;
	for (int i = JOINTS - 2; i > 0; i--) {
		xs[n] = cx[i] + half[i];
		ys[n] = cy[i];
		n++;
	}
	return n;
}

// A clump of weed for the tank: three ribbons of different heights swaying
// away from each other, in the tank's two greens. One ribbon is a leaf; three
// out of one root is a plant.
//
// The tank used to have three straight bars with two turned bars for leaves,
// and a turned bar is rasterised through a layer the size of its own box, so
// the leaves were cut away to nothing and what was left was a pole. This is
// drawn instead, without ink: it is water behind the fish, not an object in
// front of it.
static void weed_clump(pet_shape_t *s, lv_color_t dark, lv_color_t light, int base_x, int base_y, int height,
					   int lean) {
	enum { BLADES = 4, PTS = 8 };
	static const struct {
		int height;  // per cent of the clump's
		int lean;    // and of its lean
		int dx;      // the root of this blade, along the sand
		bool phase;  // which way it sways
		bool light;  // which green
	} blade[BLADES] = {
		{100, 100, 0, false, false},
		{84, 45, 4, true, true},
		{70, -55, 7, false, true},
		{58, -95, 10, true, false},
	};
	int px[BLADES][PTS];
	int py[BLADES][PTS];
	int corners[BLADES];
	int min_x = base_x;
	int max_x = base_x;
	int min_y = base_y;
	int max_y = base_y;
	for (int b = 0; b < BLADES; b++) {
		corners[b] = ribbon_corners(px[b], py[b], base_x + blade[b].dx, base_y, height * blade[b].height / 100,
									lean * blade[b].lean / 100, blade[b].phase);
		for (int i = 0; i < corners[b]; i++) {
			min_x = LV_MIN(min_x, px[b][i]);
			max_x = LV_MAX(max_x, px[b][i]);
			min_y = LV_MIN(min_y, py[b][i]);
			max_y = LV_MAX(max_y, py[b][i]);
		}
	}

	// The drawing is placed by its own corner, so the points go in relative to
	// it: absolute points would land wherever the clump is, twice over.
	shape_reset(s);
	for (int b = 0; b < BLADES; b++) {
		for (int i = 0; i < corners[b]; i++) {
			px[b][i] -= min_x;
			py[b][i] -= min_y;
		}
		shape_fan(s, blade[b].light ? light : dark, px[b], py[b], corners[b]);
	}
	shape_place(s, min_x, min_y, max_x - min_x + 1, max_y - min_y + 2);
}

// The tank's other plant: one stem with rounded leaves alternating up it, which
// is what an aquarium plant looks like when it is not a blade of grass. The
// leaves are six-cornered ovals: the fan that fills a shape needs a corner
// inside every triangle it makes, so a leaf cannot be a four-sided diamond.
static void weed_leafy(pet_shape_t *s, lv_color_t dark, lv_color_t light, int base_x, int base_y, int height,
					   int lean) {
	enum { LEAVES = 4, CORNERS = 6 };
	static const int at[LEAVES] = {22, 46, 70, 90}; // per cent of the height
	int stem_x[3];
	int stem_y[3];
	int leaf_x[LEAVES][CORNERS];
	int leaf_y[LEAVES][CORNERS];
	int min_x = base_x;
	int max_x = base_x;
	int min_y = base_y;
	int max_y = base_y;
	for (int i = 0; i < 3; i++) {
		const int t = i * 100 / 2;
		stem_x[i] = base_x + lean * t * t / 10000;
		stem_y[i] = base_y - height * t / 100;
	}
	for (int l = 0; l < LEAVES; l++) {
		const int side = (l & 1) ? -1 : 1;
		const int t = at[l];
		const int cx = base_x + lean * t * t / 10000 + side * 2;
		const int cy = base_y - height * t / 100;
		// A leaf that leans up and out, so it grows rather than hangs.
		const int xs[CORNERS] = {cx, cx + side * 4, cx + side * 9, cx + side * 12, cx + side * 8, cx + side * 2};
		const int ys[CORNERS] = {cy + 1, cy - 5, cy - 7, cy - 4, cy + 1, cy + 3};
		for (int i = 0; i < CORNERS; i++) {
			leaf_x[l][i] = xs[i];
			leaf_y[l][i] = ys[i];
			min_x = LV_MIN(min_x, xs[i]);
			max_x = LV_MAX(max_x, xs[i]);
			min_y = LV_MIN(min_y, ys[i]);
			max_y = LV_MAX(max_y, ys[i]);
		}
	}

	shape_reset(s);
	int local_x[CORNERS];
	int local_y[CORNERS];
	// The stem first: a thin taper from the sand to the top leaf.
	const int sx[CORNERS] = {stem_x[0] - 1, stem_x[1] - 1, stem_x[2], stem_x[2], stem_x[1] + 1, stem_x[0] + 1};
	const int sy[CORNERS] = {stem_y[0], stem_y[1], stem_y[2], stem_y[2], stem_y[1], stem_y[0]};
	for (int i = 0; i < CORNERS; i++) {
		local_x[i] = sx[i] - min_x;
		local_y[i] = sy[i] - min_y;
	}
	shape_fan(s, dark, local_x, local_y, CORNERS);
	for (int l = 0; l < LEAVES; l++) {
		for (int i = 0; i < CORNERS; i++) {
			local_x[i] = leaf_x[l][i] - min_x;
			local_y[i] = leaf_y[l][i] - min_y;
		}
		shape_fan(s, (l & 1) ? light : dark, local_x, local_y, CORNERS);
	}
	shape_place(s, min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
}

// A sad pet's brows: two short bars, each turned up at the middle. The frown
// alone is a mouth, and a mouth on its own reads as a mouth; the brows are what
// make the face sad. Drawn rather than turned, because a turned bar is
// rasterised through a layer the size of its own box and comes out cut off.
static void draw_brows(int eye_l_x, int eye_r_x, int eye_w, int y, bool both) {
	const int w = eye_w + 2;
	const int x0 = eye_l_x - 1;
	const int right = (eye_r_x - 1) - x0; // where the second brow starts, inside the box
	shape_reset(&shape_brow);
	// The left brow rises to the right, the right one rises to the left: both
	// are high at the middle of the face.
	shape_push(&shape_brow, PET_INK, 100, 0, 0, 0, 4, w, 0, w, 3);
	shape_push(&shape_brow, PET_INK, 100, 0, 0, 0, 4, w, 3, 0, 6);
	if (both) {
		shape_push(&shape_brow, PET_INK, 100, 0, 0, right, 0, right + w, 4, right + w, 6);
		shape_push(&shape_brow, PET_INK, 100, 0, 0, right, 0, right + w, 6, right, 3);
	}
	shape_place(&shape_brow, x0, y, (both ? right + w : w) + 1, 7);
}

// A pair of notes over a listening pet's head. The open mouth says it is
// singing along; these say what it is singing along to, and they are the one
// thing on the whole page that is only there while music is going in.
static void draw_notes(int x, int y) {
	enum { W = 24, H = 20 };
	static const int16_t disc[6][2] = {{-2, 0}, {-1, -2}, {1, -2}, {2, 0}, {1, 2}, {-1, 2}};
	shape_reset(&shape_note);
	for (int i = 0; i < 2; i++) {
		const int head_x = i ? 15 : 4;
		const int head_y = i ? 13 : 18;
		const int top = head_y - 13;
		int xs[6];
		int ys[6];
		for (int k = 0; k < 6; k++) {
			xs[k] = head_x + disc[k][0];
			ys[k] = head_y + disc[k][1];
		}
		shape_fan(&shape_note, PET_INK, xs, ys, 6);
		// The stem, and the flag that makes it a note rather than a bubble.
		shape_push(&shape_note, PET_INK, 100, 0, 0, head_x + 1, head_y - 2, head_x + 3, head_y - 2, head_x + 3, top);
		shape_push(&shape_note, PET_INK, 100, 0, 0, head_x + 1, head_y - 2, head_x + 3, top, head_x + 1, top);
		shape_push(&shape_note, PET_INK, 100, 0, 0, head_x + 2, top, head_x + 8, top + 3, head_x + 2, top + 5);
	}
	shape_place(&shape_note, x, y, W, H);
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
// The animals are drawn the way the interface's own icons are: a flat
// fill inside a heavy ink line, cut out of a handful of shapes. Everything is
// created back to front -- tail and ears first, so the body's line runs over
// them -- and every part that has an edge wears the same 2 px outline, which is
// what makes a cat read as a cat at 46 pixels wide.
// ---------------------------------------------------------------------------

static void inked(lv_obj_t *o, int width) {
	if (!o) {
		return;
	}
	lv_obj_set_style_outline_width(o, width, 0);
	lv_obj_set_style_outline_color(o, PET_INK, 0);
	lv_obj_set_style_outline_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_outline_pad(o, 0, 0);
}

// The same ink line, drawn inside the box instead of outside it. A rotated
// object is rendered through a temporary layer the size of its own box, and
// that layer clips the outline away: a turned ear would come out with no line
// at all. A border is inside the box, so it survives the turn.
static void bordered(lv_obj_t *o, int width) {
	if (!o) {
		return;
	}
	lv_obj_set_style_border_width(o, width, 0);
	lv_obj_set_style_border_color(o, PET_INK, 0);
	lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_border_side(o, LV_BORDER_SIDE_FULL, 0);
}

// A rounded shape whose corner radius is a number: the body, the muzzle, the
// belly. `disc` is the round one and `bar_piece` the squared one.
static lv_obj_t *round_piece(lv_obj_t *parent, lv_color_t color) {
	lv_obj_t *o = plain(parent);
	lv_obj_set_style_bg_color(o, color, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	return o;
}

static void build_creature(void) {
	memset(&parts, 0, sizeof(parts));

	parts.root = plain(habitat);
	lv_obj_set_size(parts.root, PET_CREATURE_W, PET_CREATURE_H);
	lv_obj_set_pos(parts.root, (card_w_px - PET_CREATURE_W) / 2, card_h_px - 42 - PET_BASE_Y + 2);
	lv_obj_set_clickable(parts.root, true);
	lv_obj_add_event_cb(parts.root, pet_tap_cb, LV_EVENT_CLICKED, NULL);

	parts.shadow = disc(parts.root, lv_color_black());

	// ---- behind the trunk ----------------------------------------------
	parts.fin_top = shape_create(parts.root, &shape_fin_top);
	parts.fin_tail = shape_create(parts.root, &shape_fin_tail);
	parts.ear_l = shape_create(parts.root, &shape_ear_l);
	parts.ear_r = shape_create(parts.root, &shape_ear_r);
	for (int i = 0; i < 4; i++) {
		parts.whisker[i] = round_piece(parts.root, PET_INK);
	}
	// ---- the trunk -----------------------------------------------------
	parts.body = round_piece(parts.root, PET_INK);
	parts.fish_body = shape_create(parts.root, &shape_fish_body);
	parts.belly = round_piece(parts.root, PET_INK);
	parts.foot_l = round_piece(parts.root, PET_INK);
	parts.foot_r = round_piece(parts.root, PET_INK);
	parts.arm_l = round_piece(parts.root, PET_INK);
	parts.arm_r = round_piece(parts.root, PET_INK);

	// ---- the head, over the shoulders ----------------------------------
	parts.head = round_piece(parts.root, PET_INK);
	// A dog's ears hang on the sides of its head, so they are drawn over it --
	// and each one carries the pale inner face of an ear. That pale shape is
	// what tells an ear from hair: a lobe of the animal's own colour laid down
	// a round head is the same ink line whether it is an ear or a bob, and no
	// outline will settle it. The cat's ears say it with a pink triangle; the
	// dog's say it with the cream of its muzzle.
	parts.jowl_l = shape_create(parts.root, &shape_jowl_l);
	parts.jowl_r = shape_create(parts.root, &shape_jowl_r);
	parts.jowl_in_l = shape_create(parts.root, &shape_jowl_in_l);
	parts.jowl_in_r = shape_create(parts.root, &shape_jowl_in_r);
	parts.brow = shape_create(parts.root, &shape_brow);

	// ---- the face ------------------------------------------------------
	parts.muzzle = round_piece(parts.root, PET_INK);
	parts.nose = round_piece(parts.root, PET_INK);
	parts.gill = ink_arc(parts.root); // over the fish's body, not under it
	parts.eye_l = round_piece(parts.root, PET_INK);
	parts.eye_r = round_piece(parts.root, PET_INK);
	parts.eye_arc_l = ink_arc(parts.root);
	parts.eye_arc_r = ink_arc(parts.root);
	parts.glint_l = disc(parts.root, lv_color_white());
	parts.glint_r = disc(parts.root, lv_color_white());
	parts.lid_l = round_piece(parts.root, PET_INK);
	parts.lid_r = round_piece(parts.root, PET_INK);
	parts.lid_v_l = round_piece(parts.root, PET_INK);
	parts.lid_v_r = round_piece(parts.root, PET_INK);
	parts.cheek_l = round_piece(parts.root, PET_CHEEK);
	parts.cheek_r = round_piece(parts.root, PET_CHEEK);

	parts.mouth = ink_arc(parts.root);
	parts.mouth_line = round_piece(parts.root, PET_INK);
	parts.mouth_open = round_piece(parts.root, PET_INK);
	parts.tongue = round_piece(parts.root, PET_CHEEK);
	parts.tear = round_piece(parts.root, PET_TEAR);
	parts.note = shape_create(parts.root, &shape_note);

	parts.zzz = lv_label_create(parts.root);
	lv_obj_add_style(parts.zzz, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(parts.zzz, &font_ui_14, 0);
	lv_label_set_text(parts.zzz, "z z");

	// ---- the wardrobe ---------------------------------------------------
	parts.hat_cone = round_piece(parts.root, PET_HAT);
	parts.hat_brim = round_piece(parts.root, PET_HAT);
	parts.phone_band = round_piece(parts.root, PET_PHONE);
	parts.phone_left = round_piece(parts.root, PET_PHONE);
	parts.phone_right = round_piece(parts.root, PET_PHONE);
	parts.scarf = round_piece(parts.root, PET_SCARF);
	parts.scarf_tail = round_piece(parts.root, PET_SCARF);
	parts.crown_band = round_piece(parts.root, PET_CROWN);
	for (int i = 0; i < 3; i++) {
		parts.crown_spike[i] = round_piece(parts.root, PET_CROWN);
	}

	// The line every part with an edge wears. Eyes and glints are left alone:
	// a 2 px outline on a 3 px glint is a smudge.
	lv_obj_t *const outlined[] = {
		parts.body, parts.head, parts.foot_l, parts.foot_r, parts.muzzle, parts.nose,
	};
	for (size_t i = 0; i < sizeof(outlined) / sizeof(outlined[0]); i++) {
		inked(outlined[i], 2);
	}
	// Everything that gets turned: the ears, the arms and the fins.
	lv_obj_t *const bordered_parts[] = {
		parts.arm_l, parts.arm_r,
		parts.whisker[0], parts.whisker[1], parts.whisker[2], parts.whisker[3],
	};
	for (size_t i = 0; i < sizeof(bordered_parts) / sizeof(bordered_parts[0]); i++) {
		bordered(bordered_parts[i], 2);
	}
	for (int i = 0; i < 3; i++) {
		inked(parts.crown_spike[i], 1);
	}
	inked(parts.hat_cone, 2);
	inked(parts.hat_brim, 2);
	inked(parts.crown_band, 1);
	inked(parts.scarf, 1);
}

// ---------------------------------------------------------------------------
// The animal, drawn
// ---------------------------------------------------------------------------

static void set_color(lv_obj_t *o, lv_color_t color) {
	if (o) {
		lv_obj_set_style_bg_color(o, color, 0);
	}
}

static void set_radius(lv_obj_t *o, int radius) {
	if (o) {
		lv_obj_set_style_radius(o, radius, 0);
	}
}

static void paint_creature(void) {
	if (!page_built) {
		return;
	}

	const pet_state_t *st = petwatch_state();
	pet_stage_t stage = pet_stage(st);
	pet_mood_t mood = pet_mood(st, petwatch_now());
	pet_species_t species = pet_species(st);
	bool sleeping = mood == PET_MOOD_SLEEPING;
	bool dizzy = mood == PET_MOOD_DIZZY;
	bool listening = petwatch_listening() && !sleeping;
	bool fish = species == PET_SPECIES_FISH;
	bool cat = species == PET_SPECIES_CAT;
	bool dog = species == PET_SPECIES_DOG;

	// One animal per stage: a hatchling is small and round, an adult is the
	// same animal with room to stand up in.
	int body_w = 78;
	int body_h = 64;
	switch (stage) {
	case PET_STAGE_CHILD:
		body_w = 86;
		body_h = 70;
		break;
	case PET_STAGE_TEEN:
		body_w = 94;
		body_h = 76;
		break;
	case PET_STAGE_ADULT:
		body_w = 100;
		body_h = 82;
		break;
	case PET_STAGE_BABY:
	default:
		break;
	}

	// ---- the two masses ------------------------------------------------
	//
	// A small animal is mostly head. The trunk is the round bottom it sits on
	// and the shoulders the arms hang from, and the head is drawn over it: the
	// part of the trunk that still shows is that bottom, the shoulders and the
	// feet, which is enough. A fish has no such seam -- it is one long body
	// with its head on the front of it -- so there the two are the same box.
	int trunk_w = fish ? body_w + 10 : body_w;
	int trunk_h = fish ? body_h * 78 / 100 : body_h * 58 / 100;
	int trunk_top = fish ? PET_BASE_Y - 6 - trunk_h : PET_BASE_Y - trunk_h;
	// The dog's head is the widest: a dog is a head with a dog on it. Both
	// heads are three quarters of the body rather than nine tenths: at the
	// size they were it was a head with a little animal under it.
	int head_w = fish ? trunk_w : body_w * (dog ? 82 : 76) / 100;
	int head_h = fish ? trunk_h : head_w;
	// Sitting a little deeper on the trunk, so that a smaller head still
	// meets the shoulders rather than resting on top of them.
	int head_bottom = fish ? PET_BASE_Y - 6 : trunk_top + head_h * 44 / 100;
	int head_top = head_bottom - head_h;

	// The fills. The line around them is the same ink for all three, so the
	// animals are a set rather than three drawings. The cat is grey and the
	// dog is a sandy yellow, so the two are told apart by colour alone even
	// before the ears are read.
	lv_color_t fill = lv_color_hex(0xC3C7CC); // the cat's grey
	lv_color_t deep = lv_color_hex(0xA2A7AE);
	lv_color_t light = lv_color_hex(0xE6E9EC);
	if (dog) {
		fill = lv_color_hex(0xF2CF6B);
		deep = lv_color_hex(0xDDB64C);
		light = lv_color_hex(0xFFF6D5);
	} else if (fish) {
		fill = lv_color_hex(0xF4B94E);
		deep = lv_color_hex(0xD99A32);
		light = lv_color_hex(0xFFEFC2);
	}

	set_color(parts.body, fill);
	set_color(parts.head, fill);
	set_color(parts.belly, light);
	set_color(parts.foot_l, deep);
	set_color(parts.foot_r, deep);
	set_color(parts.arm_l, fill);
	set_color(parts.arm_r, fill);
	set_color(parts.muzzle, light);
	set_color(parts.nose, cat ? PET_CHEEK : PET_INK);

	const int cx = PET_CREATURE_W / 2;
	const int bx = cx - trunk_w / 2;
	const int eye_w = fish ? 12 : 11;
	const int eye_h = fish ? 13 : 12;
	const int eye_dx = head_w * 19 / 100;
	const int eye_y = head_top + head_h * (fish ? 34 : 38) / 100;

	// Shadow first, under everything.
	place(parts.shadow, cx - 19, PET_BASE_Y + 2, 38, 5);
	lv_obj_set_style_bg_opa(parts.shadow, theme_is_dark() ? (lv_opa_t)(LV_OPA_COVER * 35 / 100) : LV_OPA_20, 0);
	show(parts.shadow, !fish);

	// ---- the trunk -------------------------------------------------------
	//
	// The three animals that stand are a round trunk with the head drawn over
	// it. A fish is one shape: an oval that comes to a snout in front and a
	// narrow peduncle behind, with the tail behind that. A plain oval with
	// fins stuck to it reads as a blob, whatever is drawn on it.
	set_radius(parts.body, trunk_h * 44 / 100);
	place(parts.body, bx, trunk_top, trunk_w, trunk_h);
	show(parts.body, !fish);
	// And the fish's own body is shown only for the fish. It is placed inside
	// `if (fish)` below, so it keeps the last box and the last triangles it was
	// given: without this, a player who was a fish and changed their mind keeps
	// a fish-shaped patch half behind the new animal's head.
	show(parts.fish_body, fish);

	int fish_w = 0; // the fish body's own box, for the face that goes on it
	int fish_h = 0;
	int fish_y = 0;
	if (fish) {
		// Longer than it is tall: a body as wide as it is high is a ball with a
		// tail on it, whatever the fins say.
		fish_w = trunk_w * 90 / 100;
		fish_h = trunk_h + 4;
		fish_y = trunk_top - 4;
		const int w = fish_w;
		const int h = fish_h;
		// The hull, as the corners on the way round from the snout's top: an
		// oval with a flat snout in front and a narrow peduncle behind. Six
		// corners read as a kite and eight as an octagon; a fish is round, and
		// round takes more than a dozen.
		static const int16_t hull[14][2] = {
			{2, 38}, {8, 20}, {22, 6}, {40, 2}, {60, 6}, {80, 20}, {94, 40},
			{94, 60}, {80, 80}, {60, 94}, {40, 98}, {22, 94}, {8, 80}, {2, 62},
		};
		const int n = 14;
		int xs[14];
		int ys[14];
		for (int i = 0; i < n; i++) {
			xs[i] = hull[i][0] * w / 100 - 4;
			ys[i] = hull[i][1] * h / 100;
		}
		shape_polygon(&shape_fish_body, fill, xs, ys, n);
		shape_place(&shape_fish_body, bx, fish_y, fish_w + 4, fish_h);
	}
	// The body is one flat fill, as the reference art is. A lighter belly on a
	// round animal reads as a second muzzle, whatever colour it is.
	set_radius(parts.belly, trunk_h / 7);
	place(parts.belly, cx - trunk_w * 23 / 100, trunk_top + trunk_h * 72 / 100, trunk_w * 46 / 100, trunk_h * 20 / 100);
	show(parts.belly, false);

	// ---- what is behind the trunk ---------------------------------------
	set_radius(parts.fin_tail, 5);
	set_radius(parts.fin_top, 4);
	// There is no tail on the two that sit facing you. A tail behind them is
	// invisible, and one beside the hip is a turned bar, which LVGL rasters
	// through a layer the size of the bar's own box: it comes out clipped, a
	// stick. The ears and the whiskers are the cat, the ears and the snout the
	// dog, and only the fish has a tail -- a fish is drawn side on.
	show(parts.fin_tail, fish);
	show(parts.fin_top, fish);

	if (fish) {
		// A forked tail on the peduncle and a dorsal fin on the spine: two
		// fins, and the body between them is the fish.
		const int tail_w = trunk_w * 34 / 100;
		const int tail_h = trunk_h * 64 / 100;
		const int tail_x = bx + fish_w - 18; // the hub goes under the body
		const int tail_y = fish_y + (fish_h - tail_h) / 2;
		// Two lobes with a notch between them: one triangle would be a fan
		// and read as a fin, two are a tail. The inks go down first, as on
		// the hull, so the join between them is one line and not a cross.
		static const int16_t fork[2][6] = {
			{6, 50, 100, 2, 44, 50},
			{6, 50, 44, 50, 100, 98},
		};
		shape_reset(&shape_fin_tail);
		for (int i = 0; i < 2; i++) {
			const int16_t *t = fork[i];
			shape_push(&shape_fin_tail, PET_INK, 124, 0, 0, t[0] * tail_w / 100, t[1] * tail_h / 100,
					   t[2] * tail_w / 100, t[3] * tail_h / 100, t[4] * tail_w / 100, t[5] * tail_h / 100);
		}
		for (int i = 0; i < 2; i++) {
			const int16_t *t = fork[i];
			shape_push(&shape_fin_tail, deep, 100, 0, 0, t[0] * tail_w / 100, t[1] * tail_h / 100,
					   t[2] * tail_w / 100, t[3] * tail_h / 100, t[4] * tail_w / 100, t[5] * tail_h / 100);
		}
		shape_place(&shape_fin_tail, tail_x, tail_y, tail_w, tail_h);

		// The dorsal fin stands out of the back and sinks into it: its base
		// is inside the body, which is drawn over it.
		const int top_w = fish_w * 42 / 100;
		const int top_h = fish_h * 52 / 100;
		shape_reset(&shape_fin_top);
		shape_triangle(&shape_fin_top, deep, 4, top_h - 2, top_w * 44 / 100, 3, top_w - 3, top_h - 6);
		shape_place(&shape_fin_top, bx + fish_w * 40 / 100, fish_y + fish_h * 30 / 100 - top_h, top_w, top_h);
	}

	// ---- ears ------------------------------------------------------------
	show(parts.ear_l, cat);
	show(parts.ear_r, cat);
	show(parts.jowl_l, dog);
	show(parts.jowl_r, dog);
	show(parts.jowl_in_l, dog);
	show(parts.jowl_in_r, dog);

	if (cat) {
		// Ears, leaning out of the top of the head: a triangle with a pink
		// triangle inside it, and the head's own line drawn over the base.
		const int ear_w = head_w * 36 / 100;
		const int ear_h = head_h * 50 / 100;
		const int ear_y = head_top - ear_h * 64 / 100;
		const int ear_l_x = cx - head_w * 31 / 100 - ear_w / 2;
		const int ear_r_x = cx + head_w * 31 / 100 - ear_w / 2;

		shape_reset(&shape_ear_l);
		shape_triangle(&shape_ear_l, fill, ear_w - 5, ear_h - 7, 5, ear_h - 5, ear_w * 34 / 100, 6);
		shape_push(&shape_ear_l, PET_CHEEK, 52, 0, 4, ear_w - 5, ear_h - 7, 5, ear_h - 5, ear_w * 34 / 100, 6);
		shape_place(&shape_ear_l, ear_l_x, ear_y, ear_w, ear_h);

		shape_reset(&shape_ear_r);
		shape_triangle(&shape_ear_r, fill, 5, ear_h - 7, ear_w - 5, ear_h - 5, ear_w * 66 / 100, 6);
		shape_push(&shape_ear_r, PET_CHEEK, 52, 0, 4, 5, ear_h - 7, ear_w - 5, ear_h - 5, ear_w * 66 / 100, 6);
		shape_place(&shape_ear_r, ear_r_x, ear_y, ear_w, ear_h);
	} else if (dog) {
		// A dog's ears hang: soft, wide at the shoulder of the head and
		// rounded at the tip, all the way down to the jaw. Drawn rather than
		// styled -- a rounded rectangle is a bean whatever it is turned by,
		// and a turned one is cut off at the corners by its own layer.
		draw_jowls(cx, head_w, head_h, head_top, fill, light);
	}

	// ---- feet and arms ---------------------------------------------------
	set_radius(parts.foot_l, 4);
	set_radius(parts.foot_r, 4);
	set_radius(parts.arm_l, 4);
	set_radius(parts.arm_r, 4);
	bool feet = !fish && stage >= PET_STAGE_CHILD;
	show(parts.foot_l, feet);
	show(parts.foot_r, feet);
	place(parts.foot_l, cx - trunk_w * 22 / 100 - 7, PET_BASE_Y - 8, 14, 9);
	place(parts.foot_r, cx + trunk_w * 22 / 100 - 7, PET_BASE_Y - 8, 14, 9);
	show(parts.arm_l, !fish);
	show(parts.arm_r, !fish);
	place(parts.arm_l, bx - 1, PET_BASE_Y - 20, 11, 13);
	place(parts.arm_r, bx + trunk_w - 10, PET_BASE_Y - 20, 11, 13);
	lv_obj_set_style_transform_rotation(parts.arm_l, 60, 0);
	lv_obj_set_style_transform_rotation(parts.arm_r, -60, 0);

	// ---- the head --------------------------------------------------------
	set_radius(parts.head, head_h / 2);
	place(parts.head, cx - head_w / 2, head_top, head_w, head_h);
	show(parts.head, !fish);

	// ---- the face --------------------------------------------------------
	//
	// The fish is seen from the side: its snout is at the front of its own
	// shape, it has one eye and a gill behind it, and the mouth is a small
	// curve at the end of the snout. Everything else on this page is seen
	// from the front, so the face is placed off the body box rather than off
	// the middle of the card.
	const int mouth_cx = fish ? bx + fish_w * 12 / 100 : cx;
	const int eye_l_x = fish ? bx + fish_w * 30 / 100 - eye_w / 2 : cx - eye_dx - eye_w / 2;
	const int eye_r_x = cx + eye_dx - eye_w / 2;
	const int eye_ly = fish ? fish_y + fish_h * 30 / 100 : eye_y;

	// A muzzle for the dog, with a nose on the end of it.
	int muzzle_w = dog ? 34 : 26;
	int muzzle_h = dog ? 26 : 18;
	int muzzle_y = head_top + head_h * (dog ? 52 : 56) / 100;
	set_radius(parts.muzzle, dog ? muzzle_h / 2 : 7);
	show(parts.muzzle, dog);
	place(parts.muzzle, cx - muzzle_w / 2, muzzle_y, muzzle_w, muzzle_h);

	set_radius(parts.nose, dog ? 4 : 2);
	show(parts.nose, !fish);
	if (dog) {
		place(parts.nose, cx - 6, muzzle_y + 5, 12, 8);
	} else {
		// The cat: a small pink nose, above a mouth that is all curve.
		place(parts.nose, cx - 4, head_top + head_h * 54 / 100, 8, 6);
	}

	// The gill: two thirds of a circle, bowing forward behind the head.
	show(parts.gill, fish);
	if (fish) {
		place(parts.gill, bx + fish_w * 46 / 100, fish_y + fish_h * 21 / 100, 14, 23);
		lv_arc_set_angles(parts.gill, 90, 270);
	}

	set_radius(parts.eye_l, 5);
	set_radius(parts.eye_r, 5);
	// A pet at the top of its happiness shuts its eyes in a smile, which is the
	// one thing that tells blissful from happy: every other mood shows the same
	// round eye, so a closed one says the mood without a word.
	const bool blissful = mood == PET_MOOD_BLISSFUL;
	bool eyes_open = !sleeping && !dizzy && !blissful;
	place(parts.eye_l, eye_l_x, eye_ly, eye_w, eye_h);
	place(parts.eye_r, eye_r_x, eye_y, eye_w, eye_h);
	show(parts.eye_l, eyes_open);
	show(parts.eye_r, eyes_open && !fish);
	show(parts.eye_arc_l, blissful && !sleeping && !dizzy);
	show(parts.eye_arc_r, blissful && !sleeping && !dizzy && !fish);
	if (blissful) {
		place(parts.eye_arc_l, eye_l_x - 1, eye_ly + 1, eye_w + 2, 8);
		lv_arc_set_angles(parts.eye_arc_l, 200, 340);
		if (!fish) {
			place(parts.eye_arc_r, eye_r_x - 1, eye_y + 1, eye_w + 2, 8);
			lv_arc_set_angles(parts.eye_arc_r, 200, 340);
		}
	}
	place(parts.glint_l, eye_l_x + 2, eye_ly + 2, 4, 4);
	place(parts.glint_r, eye_r_x + 2, eye_y + 2, 4, 4);
	show(parts.glint_l, eyes_open);
	show(parts.glint_r, eyes_open && !fish);

	// A dizzy pet has crosses where its eyes were: two bars over each eye, one
	// each way. Sleeping keeps the single flat lid.
	set_radius(parts.lid_l, 1);
	set_radius(parts.lid_r, 1);
	show(parts.lid_l, sleeping || dizzy);
	show(parts.lid_r, (sleeping || dizzy) && !fish);
	// A turned bar is clipped to its own box, so a dizzy pet's cross is drawn
	// from two bars that are not turned at all.
	show(parts.lid_v_l, dizzy);
	show(parts.lid_v_r, dizzy && !fish);
	if (dizzy) {
		place(parts.lid_l, eye_l_x + eye_w / 2 - 6, eye_ly + 5, 12, 2);
		place(parts.lid_r, eye_r_x + eye_w / 2 - 6, eye_y + 5, 12, 2);
		place(parts.lid_v_l, eye_l_x + eye_w / 2 - 1, eye_ly, 2, 12);
		place(parts.lid_v_r, eye_r_x + eye_w / 2 - 1, eye_y, 2, 12);
	} else {
		place(parts.lid_l, eye_l_x + 1, eye_ly + 5, 10, 2);
		place(parts.lid_r, eye_r_x + 1, eye_y + 5, 10, 2);
	}

	set_radius(parts.cheek_l, 3);
	set_radius(parts.cheek_r, 3);
	place(parts.cheek_l, cx - head_w * 32 / 100 - 4, eye_y + 13, 9, 5);
	place(parts.cheek_r, cx + head_w * 32 / 100 - 4, eye_y + 13, 9, 5);
	// A fish seen from the side has one cheek at most, and it has a gill
	// there instead.
	show(parts.cheek_l, !fish);
	show(parts.cheek_r, !fish);
	lv_obj_set_style_bg_opa(parts.cheek_l, mood == PET_MOOD_BLISSFUL ? LV_OPA_80 : LV_OPA_60, 0);
	lv_obj_set_style_bg_opa(parts.cheek_r, mood == PET_MOOD_BLISSFUL ? LV_OPA_80 : LV_OPA_60, 0);

	bool sad = mood == PET_MOOD_SAD;
	bool smile = !sad && (mood == PET_MOOD_HAPPY || mood == PET_MOOD_BLISSFUL);
	bool line = mood == PET_MOOD_OKAY || sleeping || dizzy;

	int mouth_y = fish ? fish_y + fish_h * 50 / 100 : (dog ? muzzle_y + muzzle_h - 9 : head_top + head_h * 66 / 100);
	place(parts.mouth, mouth_cx - 7, mouth_y, 14, 9);
	if (sad) {
		lv_arc_set_angles(parts.mouth, 200, 340);
	} else {
		lv_arc_set_angles(parts.mouth, 20, 160);
	}
	show(parts.mouth, !sleeping && !dizzy && !listening && (smile || sad));

	set_radius(parts.mouth_line, 1);
	place(parts.mouth_line, mouth_cx - 5, mouth_y + 3, 10, 2);
	show(parts.mouth_line, !listening && line);

	set_radius(parts.mouth_open, 4);
	place(parts.mouth_open, mouth_cx - 4, mouth_y, 8, 7);
	set_radius(parts.tongue, 2);
	place(parts.tongue, mouth_cx - 2, mouth_y + 4, 4, 3);
	show(parts.mouth_open, listening);
	show(parts.tongue, listening);

	// A cat's whiskers: two a side, thin enough that a few degrees of turn
	// does them no harm.
	for (int i = 0; i < 4; i++) {
		bool left = i < 2;
		int y = eye_y + 14 + (i % 2) * 4;
		int x = left ? cx - head_w * 30 / 100 - 9 : cx + head_w * 30 / 100 - 2;
		set_radius(parts.whisker[i], 1);
		lv_obj_set_style_bg_opa(parts.whisker[i], (lv_opa_t)(LV_OPA_COVER * 75 / 100), 0);
		place(parts.whisker[i], x, y, 11, 1);
		show(parts.whisker[i], cat);
	}

	set_radius(parts.tear, 2);
	place(parts.tear, eye_l_x - 3, eye_ly + eye_h + 1, 5, 9);
	show(parts.tear, sad);

	// A sad face wears its brows, and a listening one has music over its head.
	show(parts.brow, sad);
	if (sad) {
		draw_brows(eye_l_x, eye_r_x, eye_w, eye_ly - 6, !fish);
	}
	show(parts.note, listening);
	if (listening) {
		// Beside the head, in the air: high enough to be out of the meters'
		// column and low enough to be out of the counters above it. A fish has
		// its tail on that side, so its notes go in front of its snout.
		draw_notes(fish ? bx + 2 : cx + head_w / 2 + 2, fish ? fish_y - 22 : head_top + 6);
	}

	place(parts.zzz, PET_CREATURE_W - 32, 2, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	show(parts.zzz, sleeping);

	// ---- the wardrobe ----------------------------------------------------
	int wearing = pet_wearing(st);
	bool hat = wearing == (int)PET_ITEM_PARTY_HAT;
	bool phones = wearing == (int)PET_ITEM_HEADPHONES;
	bool scarf = wearing == (int)PET_ITEM_SCARF;
	bool crown = wearing == (int)PET_ITEM_CROWN;
	bool headwear = hat || phones || crown;

	if (headwear) {
		show(parts.ear_l, false);
		show(parts.ear_r, false);
	}

	// Hats sit on the head, which for the fish is the whole animal -- and high
	// enough to clear the dorsal fin.
	const int hat_y = fish ? fish_y - 6 : head_top;

	set_radius(parts.hat_cone, 5);
	place(parts.hat_brim, cx - 14, hat_y - 3, 28, 5);
	place(parts.hat_cone, cx - 9, hat_y - 17, 18, 15);
	show(parts.hat_brim, hat);
	show(parts.hat_cone, hat);

	set_radius(parts.phone_band, 3);
	set_radius(parts.phone_left, 4);
	set_radius(parts.phone_right, 4);
	place(parts.phone_band, cx - 18, hat_y + 6, 36, 6);
	place(parts.phone_left, cx - 22, hat_y + 12, 7, 17);
	place(parts.phone_right, cx + 15, hat_y + 12, 7, 17);
	show(parts.phone_band, phones);
	show(parts.phone_left, phones);
	show(parts.phone_right, phones);

	// A scarf goes round the neck, which is where the head meets the trunk.
	const int neck_y = fish ? trunk_top + trunk_h / 4 : trunk_top + 4;
	set_radius(parts.scarf, 4);
	set_radius(parts.scarf_tail, 3);
	place(parts.scarf, cx - 16, neck_y, 32, 7);
	place(parts.scarf_tail, cx + 8, neck_y + 5, 7, 11);
	show(parts.scarf, scarf);
	show(parts.scarf_tail, scarf);

	set_radius(parts.crown_band, 2);
	place(parts.crown_band, cx - 12, hat_y - 1, 24, 5);
	place(parts.crown_spike[0], cx - 11, hat_y - 6, 6, 7);
	place(parts.crown_spike[1], cx - 3, hat_y - 7, 6, 8);
	place(parts.crown_spike[2], cx + 5, hat_y - 6, 6, 7);
	show(parts.crown_band, crown);
	for (int i = 0; i < 3; i++) {
		show(parts.crown_spike[i], crown);
	}
}

// ---------------------------------------------------------------------------
// The four homes
//
// One wash of the pet's chosen room was not enough: a cat on a green card and a
// dog on a green card are the same card. Each animal now has flat things of its
// own behind it -- a rug and a plant, a fence and a bone, a tank -- drawn from
// the same shapes as the animal, in colours of their own, and shown or hidden
// by species. Only the ones that belong to the current animal are ever drawn.
// ---------------------------------------------------------------------------

static lv_obj_t *scene_register(lv_obj_t *o, pet_species_t species) {
	if (o && scene_count < PET_SCENE_PARTS) {
		scene[scene_count].obj = o;
		scene[scene_count].species = species;
		scene_count++;
	}
	return o;
}

static lv_obj_t *scene_add(lv_obj_t *parent, pet_species_t species) {
	return scene_register(plain(parent), species);
}

static lv_obj_t *scene_rect(lv_obj_t *parent, pet_species_t species, lv_color_t color, int radius,
							int x, int y, int w, int h) {
	lv_obj_t *o = scene_add(parent, species);
	lv_obj_set_style_bg_color(o, color, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, radius, 0);
	place(o, x, y, w, h);
	return o;
}

static void build_scenes(int card_w, int card_h) {
	const int cx = card_w / 2;
	// The floor everyone stands on, and the strip the words live in under it.
	const int floor_y = card_h - 42;
	const int floor_h = 10;

	// ---- the cat's room -------------------------------------------------
	// A rug under it, a plant in the corner, a ball of yarn it is not
	// supposed to have.
	scene_rect(habitat, PET_SPECIES_CAT, PET_CAT_RUG, 7, cx - 60, floor_y - 5, 120, 16);
	// A potted plant: a pot, a rounded mass of leaves over it, and three leaves
	// standing out of the top of the mass. The leaves are drawn as triangles
	// rather than turned bars -- a turned bar is rastered through a layer the
	// size of its own box and came out a stick -- and lobes set on top of one
	// round mass read as a pair of ears.
	//
	// Everything here is centred on one line: the pot under the leaves, the
	// mass over the pot and the middle of the leaf fan. Placed one at a time
	// they drifted apart by a couple of pixels each, which on a plant this
	// small is a plant that leans.
	const int plant_cx = 23;
	scene_rect(habitat, PET_SPECIES_CAT, PET_CAT_POT, 3, plant_cx - 9, floor_y - 13, 18, 13);
	scene_rect(habitat, PET_SPECIES_CAT, PET_PLANT, 9, plant_cx - 13, floor_y - 32, 26, 20);
	scene_register(shape_create(habitat, &shape_plant), PET_SPECIES_CAT);
	{
		const int leaf_w = 34;
		const int leaf_h = 26;
		shape_reset(&shape_plant);
		// Fill only: the scene is flat and wears no outline. The leaves are
		// mirrored about the middle of the box, and they overlap each other so
		// the notches between them stay shallow.
		shape_push(&shape_plant, PET_PLANT, 100, 0, 0, 4, leaf_h - 2, 16, leaf_h - 6, 5, 5);
		shape_push(&shape_plant, PET_PLANT, 100, 0, 0, leaf_w / 2 - 4, leaf_h, leaf_w / 2 + 4, leaf_h, leaf_w / 2, 1);
		shape_push(&shape_plant, PET_PLANT, 100, 0, 0, leaf_w - 4, leaf_h - 2, leaf_w - 5, 5, leaf_w - 16, leaf_h - 6);
		// The fan's own middle is leaf_w / 2, so the box goes where that lands on
		// the pot's centre.
		shape_place(&shape_plant, plant_cx - leaf_w / 2, floor_y - 47, leaf_w, leaf_h);
	}
	scene_rect(habitat, PET_SPECIES_CAT, PET_YARN, LV_RADIUS_CIRCLE, card_w - 36, floor_y - 24, 15, 15);
	lv_obj_t *thread = scene_rect(habitat, PET_SPECIES_CAT, PET_YARN, 1, card_w - 50, floor_y - 14, 16, 2);
	lv_obj_set_style_transform_rotation(thread, 250, 0);

	// ---- the dog's yard -------------------------------------------------
	// Grass along the bottom, a picket fence at the side, a bone in it.
	scene_rect(habitat, PET_SPECIES_DOG, PET_GRASS, 3, 0, floor_y, card_w, floor_h);
	scene_rect(habitat, PET_SPECIES_DOG, PET_GRASS_DEEP, 2, 0, floor_y, card_w, 4);
	for (int i = 0; i < 4; i++) {
		scene_rect(habitat, PET_SPECIES_DOG, PET_FENCE, 2, 6 + i * 11, floor_y - 36, 8, 34);
	}
	scene_rect(habitat, PET_SPECIES_DOG, PET_FENCE, 2, 5, floor_y - 22, 44, 4);
	// A bone lying on the grass: the bar and the four knobs that make it a bone.
	// The bar used to be turned with its two knobs left straight, so the ends
	// did not meet it and it read as a pale squiggle.
	const int bone_x = card_w - 52;
	const int bone_y = floor_y - 14;
	scene_rect(habitat, PET_SPECIES_DOG, PET_BONE, 3, bone_x, bone_y, 20, 7);
	for (int i = 0; i < 2; i++) {
		const int end_x = i ? bone_x + 13 : bone_x - 5;
		scene_rect(habitat, PET_SPECIES_DOG, PET_BONE, LV_RADIUS_CIRCLE, end_x, bone_y - 4, 11, 11);
		scene_rect(habitat, PET_SPECIES_DOG, PET_BONE, LV_RADIUS_CIRCLE, end_x, bone_y + 1, 11, 11);
	}

}

// ---------------------------------------------------------------------------
// The room, or the aquarium
// ---------------------------------------------------------------------------

static void paint_room(void) {
	if (!room_layer) {
		return;
	}

	const pet_state_t *st = petwatch_state();
	int room = pet_room(st);
	bool fish = pet_species(st) == PET_SPECIES_FISH;

	if (fish) {
		// A fish lives in water, and the room it was given colours the water:
		// the meadow's teal, the sunset's warm aqua, the night's deep blue.
		lv_color_t water = PET_WATER_MEADOW;
		if (room == 1) {
			water = PET_WATER_SUNSET;
		} else if (room == 2) {
			water = PET_WATER_NIGHT;
		}
		lv_obj_set_style_bg_color(room_layer, water, 0);
		lv_obj_set_style_bg_opa(room_layer, (lv_opa_t)(LV_OPA_COVER * 25 / 100), 0);
		lv_obj_set_style_bg_color(surface, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(surface, LV_OPA_30, 0);
	} else {
		lv_color_t wash = PET_ROOM_MEADOW;
		if (room == 1) {
			wash = PET_ROOM_SUNSET;
		} else if (room == 2) {
			wash = PET_ROOM_NIGHT;
		}
		lv_obj_set_style_bg_color(room_layer, wash, 0);
		lv_obj_set_style_bg_opa(room_layer, LV_OPA_20, 0);
		lv_obj_set_style_bg_opa(surface, LV_OPA_TRANSP, 0);
	}

	// Each animal's own few things, and nothing of anybody else's.
	pet_species_t species = pet_species(st);
	for (int i = 0; i < scene_count; i++) {
		show(scene[i].obj, scene[i].species == species);
	}

	show(surface, fish);
	show(sand, fish);
	for (int i = 0; i < 3; i++) {
		show(pebbles[i], fish);
	}
	for (int i = 0; i < PET_WEEDS; i++) {
		show(weeds[i], fish);
	}
	for (int i = 0; i < PET_BUBBLES; i++) {
		show(bubbles[i], fish);
	}

	// The Calm soundscape: a month of jazz and classical buys drifting motes
	// over whatever room the pet is in.
	bool calm = petwatch_has_soundscape();
	for (int i = 0; i < 4; i++) {
		show(motes[i], calm);
	}
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
	int species, room, wearing;
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
				  shown.species != (int)pet_species(st) || shown.wearing != pet_wearing(st) ||
				  shown.room != pet_room(st);

	if (animal) {
		paint_room();
		paint_creature();
		paint_theme_colors();
		shown.species = (int)pet_species(st);
		shown.wearing = pet_wearing(st);
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

	const lv_color_t note_color = theme()->accent;
	lv_obj_set_style_image_recolor(note_a, note_color, 0);
	lv_obj_set_style_image_recolor(note_b, note_color, 0);
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
	paint_creature();
	paint_words();
	paint_theme_colors();
}

// ---------------------------------------------------------------------------
// The animation: two frames, one timer
//
// The animal is not interpolated. It has two frames -- a breath in and a breath
// out -- and the page's own one-second timer swaps them: the root moves two
// pixels, the tail leans one way and then the other, the notes drift and the
// bubbles rise. Nothing here runs at more than 1 Hz, nothing asks LVGL to
// interpolate, and the whole thing is a dozen integer fields written into a
// dozen objects once a second. On a 240 x 320 panel that is the difference
// between a smooth animation the processor cannot afford and one it can.
// ---------------------------------------------------------------------------

static bool animating;
static int frame;       // which of the two frames is up
static int bubble_step; // how far the tank's bubbles have risen
static int mote_step;   // and the calm soundscape's motes

// Where each bubble and mote started, so the rise is a running offset rather
// than a fixed animation per object.
static int bubble_home[PET_BUBBLES];
static int mote_home[4];

static void apply_frame(void) {
	if (!page_built) {
		return;
	}

	const pet_species_t species = pet_species(petwatch_state());
	const bool listening = petwatch_listening();

	// The breath: two pixels, three when something is playing.
	lv_obj_set_style_translate_y(parts.root, frame ? (listening ? 3 : 2) : 0, 0);

	// The fish's tail leans with the breath. Nothing else sways: the other two
	// are drawn facing you, and the tail was the one part of them that showed
	// the seam of a turned bar.
	if (species == PET_SPECIES_FISH) {
		lv_obj_set_style_transform_rotation(parts.fin_tail, frame ? 45 : -45, 0);
	}

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

	// The tank's bubbles rise a few pixels a second, each on its own phase, and
	// wrap round off the top edge where nothing can be seen of them.
	for (int i = 0; i < PET_BUBBLES; i++) {
		int span = bubble_home[i] + 12;
		int risen = (bubble_step + i * 17) % (span > 0 ? span : 1);
		lv_obj_set_y(bubbles[i], bubble_home[i] - risen);
	}
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
	apply_frame();
}

static void stop_animations(void) {
	if (!page_built) {
		return;
	}
	animating = false;
	lv_obj_set_style_translate_y(parts.root, 0, 0);
	show(note_a, false);
	show(note_b, false);
	bubble_step = 0;
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
		bubble_step++;
		if (mote_step < 4096) {
			mote_step += 1;
		}
	}
	apply_frame();
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
// The chooser: which animal, and its name
// ---------------------------------------------------------------------------

static void customise_refresh(void) {
	if (!customise_screen) {
		return;
	}
	const pet_state_t *st = petwatch_state();
	char name[40];
	pet_name(name, sizeof(name));
	lv_label_set_text(customise_name_value, name);

	// The chosen animal is the lit tile rather than a word saying so: one
	// glance, no extra text.
	pet_species_t current = pet_species(st);
	for (int i = 1; i < PET_SPECIES_COUNT; i++) {
		if (!species_chip[i]) {
			continue;
		}
		bool on = current == (pet_species_t)i;
		lv_obj_set_style_bg_color(species_chip[i], on ? theme()->accent : theme()->surface, 0);
		lv_obj_set_style_bg_opa(species_chip[i], on ? LV_OPA_COVER : LV_OPA_20, 0);
		lv_obj_set_style_text_color(species_chip_label[i], on ? theme_on_accent() : theme()->text_primary, 0);
	}
}

static void species_pick_cb(lv_event_t *e) {
	pet_species_t species = (pet_species_t)(intptr_t)lv_event_get_user_data(e);
	if (!petwatch_set_species(species)) {
		return;
	}
	customise_refresh();
	paint();
	// A chosen animal is shown, not described: the sheet closes on the tap.
	customise_close();
	stop_animations();
	sync_animation();
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

	// The name is a row of its own: the one thing here that is typed rather
	// than tapped.
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

	lv_obj_t *pick = lv_label_create(customise_screen);
	lv_label_set_text(pick, tr("pet_choose_pet"));
	lv_obj_add_style(pick, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(pick, &font_ui_14, 0);
	lv_obj_set_pos(pick, pad + 4, y + 58);

	// One tile per animal: the cat and the dog side by side, and the fish
	// across the row under them. Three names at 16 px fit that in every
	// language the player speaks, the longest being "Poisson".
	int chip_y = y + 80;
	int gap = 8;
	int chip_w = (card_w - gap) / 2;
	int chip_h = 46;
	const int pick_count = PET_SPECIES_COUNT - 1;
	for (int i = 1; i < PET_SPECIES_COUNT; i++) {
		// An odd number of animals leaves the last one a row of its own, so
		// it takes the whole width rather than sitting in half of it.
		const bool alone = (pick_count % 2 == 1) && i == PET_SPECIES_COUNT - 1;
		int col = (i - 1) % 2;
		int row = (i - 1) / 2;
		lv_obj_t *chip = lv_btn_create(customise_screen);
		lv_obj_set_size(chip, alone ? card_w : chip_w, chip_h);
		lv_obj_set_pos(chip, pad + (alone ? 0 : col * (chip_w + gap)), chip_y + row * (chip_h + gap));
		lv_obj_set_style_radius(chip, 12, 0);
		lv_obj_set_style_shadow_width(chip, 0, 0);
		lv_obj_set_style_border_width(chip, 0, 0);
		lv_obj_set_style_pad_all(chip, 0, 0);
		lv_obj_add_event_cb(chip, species_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		species_chip[i] = chip;

		lv_obj_t *label = lv_label_create(chip);
		lv_label_set_text(label, tr(pet_species_tag((pet_species_t)i)));
		lv_obj_set_style_text_font(label, &font_ui_16, 0);
		lv_obj_center(label);
		species_chip_label[i] = label;
	}

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

static void pet_tap_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (petwatch_pet_now()) {
		toast_plain(tr("pet_purr"));
	} else {
		toast_plain(tr("pet_cooldown"));
	}
	paint();
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
	paint_values();
	advance_frame();
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
	// A pet that has never been chosen asks first: the sheet is the first
	// thing the page shows.
	if (pet_species(petwatch_state()) == PET_SPECIES_NONE) {
		customise_open();
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

static void build(void) {
	if (!page_cfg || page_built) {
		return;
	}

	lv_obj_add_style(petpage_screen, &theme_style_screen, 0);
	lv_obj_t *title = settingsrow_title(petpage_screen, page_cfg, "pet");
	settingsrow_title_corner_slots(title, page_cfg, 3);

	// Three corner controls: the wardrobe behind the star (slot 0, the
	// outermost), the name and the animal behind the pencil, and the day's
	// tap-along behind the note.
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

	// The room (or the water) under everything else in the card.
	room_layer = plain(habitat);
	lv_obj_set_size(room_layer, card_w, habitat_h);
	lv_obj_set_pos(room_layer, 0, 0);
	lv_obj_set_style_radius(room_layer, radius, 0);
	lv_obj_set_style_bg_opa(room_layer, LV_OPA_20, 0);

	// The aquarium. A fish's card is a tank: light at the surface, sand and
	// stones on the floor, three weeds of different heights, and six slow
	// bubbles. Drawn back to front like the animals, with the same ink.
	surface = bar_piece(habitat, PET_BUBBLE, 4);
	place(surface, 0, 2, card_w, 7);
	lv_obj_set_style_bg_opa(surface, LV_OPA_30, 0);

	sand = bar_piece(habitat, PET_SAND, 8);
	place(sand, 4, habitat_h - 16, card_w - 8, 13);
	lv_obj_set_style_bg_color(sand, PET_SAND_DEEP, 0);
	lv_obj_set_style_bg_opa(sand, LV_OPA_COVER, 0);
	lv_obj_t *sand_top = bar_piece(sand, PET_SAND, 6);
	place(sand_top, 2, -2, card_w - 12, 8);

	lv_obj_t *rock = round_piece(sand, PET_PEBBLE);
	place(rock, card_w - 74, -14, 30, 18);
	lv_obj_set_style_radius(rock, 7, 0);
	lv_obj_set_style_bg_opa(rock, LV_OPA_COVER, 0);

	pebbles[0] = disc(sand, PET_PEBBLE);
	place(pebbles[0], 18, -6, 11, 8);
	pebbles[1] = disc(sand, PET_PEBBLE);
	place(pebbles[1], 40, -3, 8, 6);
	pebbles[2] = disc(sand, PET_PEBBLE);
	place(pebbles[2], card_w - 52, -5, 10, 7);

	// The weeds stand clear of the status line's column, so the words stay
	// readable over the sand: two on the left of the fish and two on the
	// right, of different heights and both greens, each leaning its own way.
	// Their roots are a couple of pixels into the sand.
	static const struct {
		int x;
		int h;
		int lean;
		bool leafy; // a bush of leaves rather than a fan of blades
	} weed_at[PET_WEEDS] = {
		{14, 56, 12, false},
		{36, 36, 7, true},
		{-66, 50, 9, false},
		{-36, 32, 5, true},
	};
	for (int i = 0; i < PET_WEEDS; i++) {
		const int x = weed_at[i].x > 0 ? weed_at[i].x : card_w + weed_at[i].x;
		weeds[i] = shape_create(habitat, &shape_weed[i]);
		if (weed_at[i].leafy) {
			weed_leafy(&shape_weed[i], PET_WEED, PET_WEED_LIGHT, x, habitat_h - 16, weed_at[i].h, weed_at[i].lean);
		} else {
			weed_clump(&shape_weed[i], PET_WEED, PET_WEED_LIGHT, x, habitat_h - 16, weed_at[i].h, weed_at[i].lean);
		}
	}

	build_scenes(card_w, habitat_h);

	for (int i = 0; i < PET_BUBBLES; i++) {
		int size = 4 + (i % 3) * 3;
		bubbles[i] = disc(habitat, PET_BUBBLE);
		lv_obj_set_style_bg_opa(bubbles[i], LV_OPA_60, 0);
		bubble_home[i] = habitat_h - 60 - (i % 3) * 30;
		place(bubbles[i], card_w - 30 - (i % 4) * 16, bubble_home[i], size, size);
	}

	// ---- the top of the card -------------------------------------------
	name_label = lv_label_create(habitat);
	lv_obj_add_style(name_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(name_label, &font_ui_20, 0);
	lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name_label, card_w - PET_HUD_W - 24);
	lv_obj_set_pos(name_label, 12, 6);
	lv_label_set_text(name_label, "Momo");

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

	// ---- the animal ------------------------------------------------------
	build_creature();
	lv_obj_set_pos(parts.root, (card_w - PET_CREATURE_W) / 2, 52);

	// The two notes drift up out of the card's top.
	note_a = icon_image(habitat, &icon_music_note, 14);
	place(note_a, 26, 64, 14, 14);
	note_b = icon_image(habitat, &icon_music_note, 12);
	place(note_b, card_w - 34, 78, 12, 12);
	lv_obj_set_style_opa(note_a, LV_OPA_TRANSP, 0);
	lv_obj_set_style_opa(note_b, LV_OPA_TRANSP, 0);
	show(note_a, false);
	show(note_b, false);

	// ---- what it is feeding on ------------------------------------------
	// The status line's picture: a still note when nothing is playing, and
	// three bars that swap on the frame timer while something is.
	status_icon = icon_image(habitat, &icon_music_note, 12);
	place(status_icon, 12, habitat_h - 28, 12, 12);

	eq_bottom = habitat_h - 21;
	for (int i = 0; i < 3; i++) {
		eq_bar[i] = bar_piece(habitat, lv_color_white(), 1);
		place(eq_bar[i], 12 + i * 5, eq_bottom - 4, 3, 4);
		show(eq_bar[i], false);
	}

	status_label = lv_label_create(habitat);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_14, 0);
	lv_obj_set_pos(status_label, 28, habitat_h - 31);
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
