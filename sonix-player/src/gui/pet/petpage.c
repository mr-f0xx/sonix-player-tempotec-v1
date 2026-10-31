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
#define PET_CREATURE_H 108
#define PET_BASE_Y 100 // where the animal's feet sit, from the root's top

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
#define PET_SAND lv_color_hex(0xEAD7A6)
#define PET_SAND_DEEP lv_color_hex(0xD8C083)
#define PET_PEBBLE lv_color_hex(0x9AA7B4)
#define PET_WEED lv_color_hex(0x3E8F5F)
#define PET_WEED_LIGHT lv_color_hex(0x66B87F)
#define PET_BUBBLE lv_color_hex(0xE4F7FC)
#define PET_BUBBLES 6

typedef struct {
	lv_obj_t *root;
	lv_obj_t *shadow;

	// Behind the body: the tail a cat, a dog and a dragon wag, and the fins a
	// fish swims with.
	lv_obj_t *tail;
	lv_obj_t *tail_tip;
	lv_obj_t *fin_top;
	lv_obj_t *fin_side;
	lv_obj_t *fin_tail;

	lv_obj_t *body;
	lv_obj_t *belly;
	lv_obj_t *foot_l;
	lv_obj_t *foot_r;
	lv_obj_t *arm_l;
	lv_obj_t *arm_r;

	// Heads: pointed ears, floppy ones, horns.
	lv_obj_t *ear_l;
	lv_obj_t *ear_r;
	lv_obj_t *ear_in_l;
	lv_obj_t *ear_in_r;
	lv_obj_t *horn_l;
	lv_obj_t *horn_r;

	// Faces: a muzzle for the dog and the dragon, whiskers for the cat.
	lv_obj_t *muzzle;
	lv_obj_t *nose;

	lv_obj_t *eye_l;
	lv_obj_t *eye_r;
	lv_obj_t *glint_l;
	lv_obj_t *glint_r;
	lv_obj_t *lid_l;
	lv_obj_t *lid_r;
	lv_obj_t *cheek_l;
	lv_obj_t *cheek_r;
	lv_obj_t *mouth;
	lv_obj_t *mouth_line;
	lv_obj_t *mouth_open;
	lv_obj_t *tongue;
	lv_obj_t *tear;
	lv_obj_t *zzz;

	// What the wardrobe sells. Headwear hides the ears, the horns and the
	// little "z z", because two hats on one head is not a hat.
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

// The aquarium: the sand, two weeds and three slow bubbles, shown only for a
// fish.
static lv_obj_t *room_layer;
static lv_obj_t *surface; // the light band where the water meets the air
static lv_obj_t *sand;
static lv_obj_t *pebbles[3];
static lv_obj_t *weeds[3];
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
static lv_obj_t *mouth_arc(lv_obj_t *parent) {
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
// The four animals are drawn the way the interface's own icons are: a flat
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
	lv_obj_set_clickable(parts.root, true);
	lv_obj_add_event_cb(parts.root, pet_tap_cb, LV_EVENT_CLICKED, NULL);

	parts.shadow = disc(parts.root, lv_color_black());

	// ---- behind the body ----------------------------------------------
	parts.tail = round_piece(parts.root, PET_INK);
	parts.tail_tip = round_piece(parts.root, PET_INK);
	parts.fin_top = round_piece(parts.root, PET_INK);
	parts.fin_side = round_piece(parts.root, PET_INK);
	parts.fin_tail = round_piece(parts.root, PET_INK);
	parts.ear_l = round_piece(parts.root, PET_INK);
	parts.ear_r = round_piece(parts.root, PET_INK);
	parts.ear_in_l = round_piece(parts.root, PET_CHEEK);
	parts.ear_in_r = round_piece(parts.root, PET_CHEEK);
	parts.horn_l = round_piece(parts.root, PET_INK);
	parts.horn_r = round_piece(parts.root, PET_INK);

	// ---- the body ------------------------------------------------------
	parts.body = round_piece(parts.root, PET_INK);
	parts.belly = round_piece(parts.root, PET_INK);
	parts.foot_l = round_piece(parts.root, PET_INK);
	parts.foot_r = round_piece(parts.root, PET_INK);
	parts.arm_l = round_piece(parts.root, PET_INK);
	parts.arm_r = round_piece(parts.root, PET_INK);

	// ---- the face ------------------------------------------------------
	parts.muzzle = round_piece(parts.root, PET_INK);
	parts.nose = round_piece(parts.root, PET_INK);
	parts.eye_l = round_piece(parts.root, PET_INK);
	parts.eye_r = round_piece(parts.root, PET_INK);
	parts.glint_l = disc(parts.root, lv_color_white());
	parts.glint_r = disc(parts.root, lv_color_white());
	parts.lid_l = round_piece(parts.root, PET_INK);
	parts.lid_r = round_piece(parts.root, PET_INK);
	parts.cheek_l = round_piece(parts.root, PET_CHEEK);
	parts.cheek_r = round_piece(parts.root, PET_CHEEK);

	parts.mouth = mouth_arc(parts.root);
	parts.mouth_line = round_piece(parts.root, PET_INK);
	parts.mouth_open = round_piece(parts.root, PET_INK);
	parts.tongue = round_piece(parts.root, PET_CHEEK);
	parts.tear = round_piece(parts.root, PET_TEAR);

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
		parts.body, parts.foot_l, parts.foot_r, parts.arm_l, parts.arm_r,
		parts.ear_l, parts.ear_r, parts.horn_l, parts.horn_r, parts.tail, parts.tail_tip,
		parts.fin_top, parts.fin_side, parts.fin_tail, parts.muzzle, parts.nose,
	};
	for (size_t i = 0; i < sizeof(outlined) / sizeof(outlined[0]); i++) {
		inked(outlined[i], 2);
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
	bool dragon = species == PET_SPECIES_DRAGON;

	// One body per stage, and a wider, flatter one for the fish: a pet is a
	// silhouette before it is anything else.
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
	if (fish) {
		body_w += 14;
		body_h = body_h * 3 / 4;
	}

	// The fills. The line around them is the same ink for all four, so the
	// animals are a set rather than four drawings.
	lv_color_t fill = lv_color_hex(0x7FCB6A);  // dragon green
	lv_color_t deep = lv_color_hex(0x5AA84A);
	lv_color_t light = lv_color_hex(0xD9F0A0);
	if (cat) {
		fill = lv_color_hex(0xF6C8A0);
		deep = lv_color_hex(0xE3A778);
		light = lv_color_hex(0xFFF1E1);
	} else if (dog) {
		fill = lv_color_hex(0xF2CF6B);
		deep = lv_color_hex(0xDDB64C);
		light = lv_color_hex(0xFFF6D5);
	} else if (fish) {
		fill = lv_color_hex(0xF4B94E);
		deep = lv_color_hex(0xD99A32);
		light = lv_color_hex(0xFFEFC2);
	}

	set_color(parts.body, fill);
	set_color(parts.belly, light);
	set_color(parts.foot_l, deep);
	set_color(parts.foot_r, deep);
	set_color(parts.arm_l, fill);
	set_color(parts.arm_r, fill);
	set_color(parts.ear_l, fill);
	set_color(parts.ear_r, fill);
	set_color(parts.horn_l, light);
	set_color(parts.horn_r, light);
	set_color(parts.tail, fill);
	set_color(parts.tail_tip, fill);
	set_color(parts.fin_top, deep);
	set_color(parts.fin_side, deep);
	set_color(parts.fin_tail, deep);
	set_color(parts.muzzle, light);
	set_color(parts.nose, PET_INK);

	const int cx = PET_CREATURE_W / 2;
	const int bx = cx - body_w / 2;
	const int by = PET_BASE_Y - body_h;
	const int eye_dx = body_w * 19 / 100;
	const int eye_y = by + body_h * 33 / 100;
	const int head_gap = 12; // how far a hat sits above the body's top

	// Shadow first, under everything.
	place(parts.shadow, cx - 20, PET_BASE_Y + 2, 40, 6);
	lv_obj_set_style_bg_opa(parts.shadow, theme_is_dark() ? LV_OPA_50 : LV_OPA_20, 0);
	show(parts.shadow, !fish);

	set_radius(parts.body, fish ? body_h / 2 : body_h * 42 / 100);
	place(parts.body, bx, by, body_w, body_h);
	// The body is one flat fill, as the reference art is. A lighter belly on a
	// round animal reads as a second muzzle, whatever colour it is.
	set_radius(parts.belly, body_h / 7);
	place(parts.belly, cx - body_w * 23 / 100, by + body_h * 72 / 100, body_w * 46 / 100, body_h * 20 / 100);
	show(parts.belly, false);

	// ---- what is behind the body --------------------------------------
	set_radius(parts.tail, 4);
	set_radius(parts.tail_tip, 3);
	set_radius(parts.fin_tail, 5);
	set_radius(parts.fin_top, 4);
	set_radius(parts.fin_side, 3);
	show(parts.tail, cat || dog || dragon);
	show(parts.tail_tip, dragon);
	show(parts.fin_tail, fish);
	show(parts.fin_top, fish);
	show(parts.fin_side, fish);

	if (fish) {
		// A tail fin is a diamond off the back, and the fins are what say
		// "fish" more than the body does.
		place(parts.fin_tail, bx - 5, by + body_h / 2 - 10, 21, 21);
		lv_obj_set_style_transform_rotation(parts.fin_tail, 450, 0);
		place(parts.fin_top, cx - 11, by - 7, 22, 10);
		lv_obj_set_style_transform_rotation(parts.fin_top, -170, 0);
		// The second fin hangs under the belly, which is what makes the
		// silhouette read as a fish rather than as a lozenge.
		place(parts.fin_side, cx - 8, by + body_h - 3, 18, 9);
		lv_obj_set_style_transform_rotation(parts.fin_side, 200, 0);
	} else if (cat) {
		place(parts.tail, bx + body_w - 4, by + body_h - 24, 20, 7);
		lv_obj_set_style_transform_rotation(parts.tail, -520, 0);
	} else if (dog) {
		place(parts.tail, bx + body_w - 3, by + body_h - 26, 14, 7);
		lv_obj_set_style_transform_rotation(parts.tail, -750, 0);
	} else if (dragon) {
		place(parts.tail, bx + body_w - 3, by + body_h - 20, 24, 8);
		lv_obj_set_style_transform_rotation(parts.tail, -380, 0);
		place(parts.tail_tip, bx + body_w + 12, by + body_h - 34, 10, 10);
		lv_obj_set_style_transform_rotation(parts.tail_tip, 450, 0);
	}

	// ---- ears, horns ---------------------------------------------------
	set_radius(parts.ear_l, 3);
	set_radius(parts.ear_r, 3);
	set_radius(parts.ear_in_l, 2);
	set_radius(parts.ear_in_r, 2);
	show(parts.ear_l, cat || dog);
	show(parts.ear_r, cat || dog);
	show(parts.ear_in_l, cat);
	show(parts.ear_in_r, cat);
	show(parts.horn_l, dragon);
	show(parts.horn_r, dragon);

	if (cat) {
		// Triangles in everything but name: small squares stood on a corner.
		place(parts.ear_l, bx + 1, by - 12, 15, 15);
		place(parts.ear_r, bx + body_w - 16, by - 12, 15, 15);
		lv_obj_set_style_transform_rotation(parts.ear_l, -350, 0);
		lv_obj_set_style_transform_rotation(parts.ear_r, 350, 0);
		place(parts.ear_in_l, bx + 5, by - 7, 7, 7);
		place(parts.ear_in_r, bx + body_w - 12, by - 7, 7, 7);
		lv_obj_set_style_transform_rotation(parts.ear_in_l, -350, 0);
		lv_obj_set_style_transform_rotation(parts.ear_in_r, 350, 0);
	} else if (dog) {
		// A dog's ears hang: long, soft, and out to the sides.
		set_radius(parts.ear_l, 7);
		set_radius(parts.ear_r, 7);
		place(parts.ear_l, bx - 7, by + 2, 16, 26);
		place(parts.ear_r, bx + body_w - 9, by + 2, 16, 26);
		lv_obj_set_style_transform_rotation(parts.ear_l, -120, 0);
		lv_obj_set_style_transform_rotation(parts.ear_r, 120, 0);
	}

	set_radius(parts.horn_l, 4);
	set_radius(parts.horn_r, 4);
	place(parts.horn_l, bx + 3, by - 15, 8, 18);
	place(parts.horn_r, bx + body_w - 11, by - 15, 8, 18);
	lv_obj_set_style_transform_rotation(parts.horn_l, -220, 0);
	lv_obj_set_style_transform_rotation(parts.horn_r, 220, 0);

	// ---- feet and arms ---------------------------------------------------
	set_radius(parts.foot_l, 4);
	set_radius(parts.foot_r, 4);
	set_radius(parts.arm_l, 4);
	set_radius(parts.arm_r, 4);
	bool feet = !fish && stage >= PET_STAGE_CHILD;
	show(parts.foot_l, feet);
	show(parts.foot_r, feet);
	place(parts.foot_l, cx - body_w * 22 / 100 - 7, by + body_h - 6, 14, 9);
	place(parts.foot_r, cx + body_w * 22 / 100 - 7, by + body_h - 6, 14, 9);
	show(parts.arm_l, !fish);
	show(parts.arm_r, !fish);
	place(parts.arm_l, bx - 5, by + body_h * 52 / 100, 10, 16);
	place(parts.arm_r, bx + body_w - 5, by + body_h * 52 / 100, 10, 16);
	lv_obj_set_style_transform_rotation(parts.arm_l, 120, 0);
	lv_obj_set_style_transform_rotation(parts.arm_r, -120, 0);

	// ---- the face --------------------------------------------------------
	// A muzzle for the animals that have one, and a nose on the end of it.
	set_radius(parts.muzzle, 9);
	show(parts.muzzle, dog || dragon);
	if (dog) {
		place(parts.muzzle, cx - 13, by + body_h * 48 / 100, 26, 19);
	} else if (dragon) {
		place(parts.muzzle, cx - 11, by + body_h * 49 / 100, 22, 16);
	}
	set_radius(parts.nose, 3);
	show(parts.nose, dog || dragon);
	place(parts.nose, cx - 4, by + body_h * 50 / 100, 8, 6);

	int eye_w = fish ? 11 : 10;
	int eye_h = 12;
	set_radius(parts.eye_l, 5);
	set_radius(parts.eye_r, 5);
	bool eyes_open = !sleeping && !dizzy;
	place(parts.eye_l, cx - eye_dx - eye_w / 2, eye_y, eye_w, eye_h);
	place(parts.eye_r, cx + eye_dx - eye_w / 2, eye_y, eye_w, eye_h);
	show(parts.eye_l, eyes_open);
	show(parts.eye_r, eyes_open);
	place(parts.glint_l, cx - eye_dx - eye_w / 2 + 2, eye_y + 2, 4, 4);
	place(parts.glint_r, cx + eye_dx - eye_w / 2 + 2, eye_y + 2, 4, 4);
	show(parts.glint_l, eyes_open);
	show(parts.glint_r, eyes_open);

	// A dizzy pet has crosses where its eyes were: two bars over each eye, one
	// each way. Sleeping keeps the single flat lid.
	set_radius(parts.lid_l, 1);
	set_radius(parts.lid_r, 1);
	show(parts.lid_l, sleeping || dizzy);
	show(parts.lid_r, sleeping || dizzy);
	if (dizzy) {
		place(parts.lid_l, cx - eye_dx - 7, eye_y + 4, 14, 2);
		place(parts.lid_r, cx + eye_dx - 7, eye_y + 4, 14, 2);
		lv_obj_set_style_transform_rotation(parts.lid_l, 450, 0);
		lv_obj_set_style_transform_rotation(parts.lid_r, -450, 0);
	} else {
		place(parts.lid_l, cx - eye_dx - 5, eye_y + 5, 10, 2);
		place(parts.lid_r, cx + eye_dx - 5, eye_y + 5, 10, 2);
		lv_obj_set_style_transform_rotation(parts.lid_l, 0, 0);
		lv_obj_set_style_transform_rotation(parts.lid_r, 0, 0);
	}

	set_radius(parts.cheek_l, 3);
	set_radius(parts.cheek_r, 3);
	place(parts.cheek_l, cx - body_w * 34 / 100 - 4, eye_y + 12, 9, 5);
	place(parts.cheek_r, cx + body_w * 34 / 100 - 4, eye_y + 12, 9, 5);
	lv_obj_set_style_bg_opa(parts.cheek_l, mood == PET_MOOD_BLISSFUL ? LV_OPA_80 : LV_OPA_60, 0);
	lv_obj_set_style_bg_opa(parts.cheek_r, mood == PET_MOOD_BLISSFUL ? LV_OPA_80 : LV_OPA_60, 0);

	bool sad = mood == PET_MOOD_SAD;
	bool smile = !sad && (mood == PET_MOOD_HAPPY || mood == PET_MOOD_BLISSFUL);
	bool line = mood == PET_MOOD_OKAY || sleeping || dizzy;

	int mouth_y = by + body_h * ((dog || dragon) ? 60 : 57) / 100;
	place(parts.mouth, cx - 7, mouth_y, 14, 9);
	if (sad) {
		lv_arc_set_angles(parts.mouth, 200, 340);
	} else {
		lv_arc_set_angles(parts.mouth, 20, 160);
	}
	show(parts.mouth, !sleeping && !dizzy && !listening && (smile || sad));

	set_radius(parts.mouth_line, 1);
	place(parts.mouth_line, cx - 5, mouth_y + 3, 10, 2);
	show(parts.mouth_line, !listening && line);

	set_radius(parts.mouth_open, 4);
	place(parts.mouth_open, cx - 4, mouth_y, 8, 7);
	set_radius(parts.tongue, 2);
	place(parts.tongue, cx - 2, mouth_y + 4, 4, 3);
	show(parts.mouth_open, listening);
	show(parts.tongue, listening);

	set_radius(parts.tear, 2);
	place(parts.tear, cx - eye_dx - 7, eye_y + 11, 4, 7);
	show(parts.tear, sad);

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
		show(parts.ear_in_l, false);
		show(parts.ear_in_r, false);
		show(parts.horn_l, false);
		show(parts.horn_r, false);
	}

	set_radius(parts.hat_cone, 5);
	place(parts.hat_brim, cx - 14, by - head_gap + 9, 28, 5);
	place(parts.hat_cone, cx - 9, by - head_gap - 5, 18, 15);
	show(parts.hat_brim, hat);
	show(parts.hat_cone, hat);

	set_radius(parts.phone_band, 3);
	set_radius(parts.phone_left, 4);
	set_radius(parts.phone_right, 4);
	place(parts.phone_band, cx - 18, by - head_gap + 3, 36, 6);
	place(parts.phone_left, cx - 22, by - head_gap + 8, 7, 17);
	place(parts.phone_right, cx + 15, by - head_gap + 8, 7, 17);
	show(parts.phone_band, phones);
	show(parts.phone_left, phones);
	show(parts.phone_right, phones);

	set_radius(parts.scarf, 4);
	set_radius(parts.scarf_tail, 3);
	place(parts.scarf, cx - 16, by + body_h - 4, 32, 7);
	place(parts.scarf_tail, cx + 8, by + body_h + 1, 7, 11);
	show(parts.scarf, scarf);
	show(parts.scarf_tail, scarf);

	set_radius(parts.crown_band, 2);
	place(parts.crown_band, cx - 12, by - head_gap + 5, 24, 5);
	place(parts.crown_spike[0], cx - 11, by - head_gap, 6, 7);
	place(parts.crown_spike[1], cx - 3, by - head_gap - 1, 6, 8);
	place(parts.crown_spike[2], cx + 5, by - head_gap, 6, 7);
	show(parts.crown_band, crown);
	for (int i = 0; i < 3; i++) {
		show(parts.crown_spike[i], crown);
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

	show(surface, fish);
	show(sand, fish);
	for (int i = 0; i < 3; i++) {
		show(pebbles[i], fish);
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

	// The tail -- or the fin a fish swims with -- leans with it.
	lv_obj_t *sway = species == PET_SPECIES_FISH ? parts.fin_tail : parts.tail;
	int base = -380;
	if (species == PET_SPECIES_FISH) {
		base = 450;
	} else if (species == PET_SPECIES_CAT) {
		base = -520;
	} else if (species == PET_SPECIES_DOG) {
		base = -750;
	}
	lv_obj_set_style_transform_rotation(sway, base + (frame ? 90 : -90), 0);

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
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, pad + 4, pad + page_cfg->top_bar_height + 10);

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
	lv_obj_align(tap_hint_label, LV_ALIGN_TOP_LEFT, pad + 4, pad + page_cfg->top_bar_height + 34);

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
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, pad + 4, pad + page_cfg->top_bar_height + 10);

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

	// Four tiles: two rows of two, wide enough for the longest name in any
	// language ("Дракон") at the small size.
	int chip_y = y + 80;
	int gap = 8;
	int chip_w = (card_w - gap) / 2;
	int chip_h = 46;
	for (int i = 1; i < PET_SPECIES_COUNT; i++) {
		int col = (i - 1) % 2;
		int row = (i - 1) / 2;
		lv_obj_t *chip = lv_btn_create(customise_screen);
		lv_obj_set_size(chip, chip_w, chip_h);
		lv_obj_set_pos(chip, pad + col * (chip_w + gap), chip_y + row * (chip_h + gap));
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
	lv_obj_align(name_heading, LV_ALIGN_TOP_LEFT, pad + 4, pad + page_cfg->top_bar_height + 10);

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
	// readable over the sand.
	const int weed_x[3] = {14, 0, 0};
	const int weed_h[3] = {54, 34, 46};
	for (int i = 0; i < 3; i++) {
		int x = i == 0 ? weed_x[0] : (i == 1 ? card_w - 56 : card_w - 26);
		weeds[i] = bar_piece(habitat, i == 1 ? PET_WEED_LIGHT : PET_WEED, 3);
		place(weeds[i], x, habitat_h - 18 - weed_h[i], 6, weed_h[i]);
		// Two leaves, so a weed is a plant rather than a pole.
		lv_obj_t *leaf = bar_piece(weeds[i], i == 1 ? PET_WEED_LIGHT : PET_WEED, 2);
		place(leaf, -6, 10, 12, 4);
		lv_obj_set_style_transform_rotation(leaf, 320, 0);
		lv_obj_t *leaf2 = bar_piece(weeds[i], i == 1 ? PET_WEED_LIGHT : PET_WEED, 2);
		place(leaf2, 1, weed_h[i] / 2, 12, 4);
		lv_obj_set_style_transform_rotation(leaf2, -320, 0);
	}

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
