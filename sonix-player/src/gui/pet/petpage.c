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

#define PET_CREATURE_W 104
#define PET_CREATURE_H 96
#define PET_BASE_Y 88 // where the animal's feet sit, from the root's top

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
#define PET_SAND lv_color_hex(0xE8D3A2)
#define PET_WEED lv_color_hex(0x4E9E6A)
#define PET_BUBBLE lv_color_hex(0xD8F1F8)

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
static bool animating;
static lv_timer_t *page_timer;

static lv_obj_t *habitat;
static lv_obj_t *name_label;
static lv_obj_t *stage_label;
static lv_obj_t *status_icon;
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
static lv_obj_t *sand;
static lv_obj_t *weed_l;
static lv_obj_t *weed_r;
static lv_obj_t *bubbles[3];

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

static lv_obj_t *name_layer;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

static int last_vibe = -1;
static bool last_listening;

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

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(icon, cfg);
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
// ---------------------------------------------------------------------------

static void build_creature(void) {
	memset(&parts, 0, sizeof(parts));

	parts.root = plain(habitat);
	lv_obj_set_size(parts.root, PET_CREATURE_W, PET_CREATURE_H);
	lv_obj_set_clickable(parts.root, true);
	lv_obj_add_event_cb(parts.root, pet_tap_cb, LV_EVENT_CLICKED, NULL);

	parts.shadow = disc(parts.root, lv_color_black());

	parts.tail = bar_piece(parts.root, PET_INK, 3);
	parts.tail_tip = disc(parts.root, PET_INK);
	parts.fin_top = bar_piece(parts.root, PET_INK, 4);
	parts.fin_side = bar_piece(parts.root, PET_INK, 3);
	parts.fin_tail = bar_piece(parts.root, PET_INK, 5);

	parts.body = disc(parts.root, PET_INK);
	parts.belly = disc(parts.root, PET_INK);

	parts.ear_l = bar_piece(parts.root, PET_INK, 4);
	parts.ear_r = bar_piece(parts.root, PET_INK, 4);
	parts.ear_in_l = bar_piece(parts.root, PET_CHEEK, 3);
	parts.ear_in_r = bar_piece(parts.root, PET_CHEEK, 3);
	parts.horn_l = bar_piece(parts.root, PET_INK, 3);
	parts.horn_r = bar_piece(parts.root, PET_INK, 3);

	parts.foot_l = disc(parts.root, PET_INK);
	parts.foot_r = disc(parts.root, PET_INK);
	parts.arm_l = disc(parts.root, PET_INK);
	parts.arm_r = disc(parts.root, PET_INK);

	parts.muzzle = disc(parts.root, PET_INK);
	parts.nose = disc(parts.root, PET_INK);

	parts.eye_l = disc(parts.root, PET_INK);
	parts.eye_r = disc(parts.root, PET_INK);
	parts.glint_l = disc(parts.root, lv_color_white());
	parts.glint_r = disc(parts.root, lv_color_white());
	parts.lid_l = bar_piece(parts.root, PET_INK, 1);
	parts.lid_r = bar_piece(parts.root, PET_INK, 1);
	parts.cheek_l = disc(parts.root, PET_CHEEK);
	parts.cheek_r = disc(parts.root, PET_CHEEK);

	parts.mouth = mouth_arc(parts.root);
	parts.mouth_line = bar_piece(parts.root, PET_INK, 1);
	parts.mouth_open = disc(parts.root, PET_INK);
	parts.tongue = disc(parts.root, PET_CHEEK);
	parts.tear = disc(parts.root, PET_TEAR);

	parts.zzz = lv_label_create(parts.root);
	lv_obj_add_style(parts.zzz, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(parts.zzz, &font_ui_14, 0);
	lv_label_set_text(parts.zzz, "z z");

	parts.hat_cone = bar_piece(parts.root, PET_HAT, 5);
	parts.hat_brim = bar_piece(parts.root, PET_HAT, 2);
	parts.phone_band = bar_piece(parts.root, PET_PHONE, 3);
	parts.phone_left = bar_piece(parts.root, PET_PHONE, 3);
	parts.phone_right = bar_piece(parts.root, PET_PHONE, 3);
	parts.scarf = bar_piece(parts.root, PET_SCARF, 3);
	parts.scarf_tail = bar_piece(parts.root, PET_SCARF, 2);
	parts.crown_band = bar_piece(parts.root, PET_CROWN, 2);
	for (int i = 0; i < 3; i++) {
		parts.crown_spike[i] = bar_piece(parts.root, PET_CROWN, 1);
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
	pet_stage_t stage = pet_stage(st);
	pet_mood_t mood = pet_mood(st, petwatch_now());
	pet_species_t species = pet_species(st);
	bool sleeping = mood == PET_MOOD_SLEEPING;
	bool listening = petwatch_listening() && !sleeping;
	bool fish = species == PET_SPECIES_FISH;
	bool cat = species == PET_SPECIES_CAT;
	bool dog = species == PET_SPECIES_DOG;
	bool dragon = species == PET_SPECIES_DRAGON;

	// One body per stage: the newest animal is round and small, and each stage
	// after that a little wider. Everything else is placed from these numbers.
	int body_w = 46;
	int body_h = 40;
	switch (stage) {
	case PET_STAGE_CHILD:
		body_w = 56;
		body_h = 46;
		break;
	case PET_STAGE_TEEN:
		body_w = 64;
		body_h = 50;
		break;
	case PET_STAGE_ADULT:
		body_w = 72;
		body_h = 56;
		break;
	case PET_STAGE_BABY:
	default:
		break;
	}
	if (fish) {
		body_h = body_h * 4 / 5; // wider than tall, as a fish is
	}

	// The palette: a ginger cat, a tan dog, a green dragon, a gold fish. The
	// belly is always the light one, the tips the deep one.
	lv_color_t body = lv_color_hex(0x8FD6B0);
	lv_color_t deep = lv_color_hex(0x62B78C);
	lv_color_t belly = lv_color_hex(0xE7F8EE);
	if (cat) {
		body = lv_color_hex(0xF2B27C);
		deep = lv_color_hex(0xD98F53);
		belly = lv_color_hex(0xFBEDDD);
	} else if (dog) {
		body = lv_color_hex(0xCBAA82);
		deep = lv_color_hex(0xAB855B);
		belly = lv_color_hex(0xF6ECDE);
	} else if (fish) {
		body = lv_color_hex(0xF2B44A);
		deep = lv_color_hex(0xD3902B);
		belly = lv_color_hex(0xFBE7C0);
	}

	set_color(parts.body, body);
	set_color(parts.belly, belly);
	set_color(parts.foot_l, deep);
	set_color(parts.foot_r, deep);
	set_color(parts.arm_l, body);
	set_color(parts.arm_r, body);
	set_color(parts.ear_l, body);
	set_color(parts.ear_r, body);
	set_color(parts.horn_l, belly);
	set_color(parts.horn_r, belly);
	set_color(parts.tail, deep);
	set_color(parts.tail_tip, deep);
	set_color(parts.fin_top, deep);
	set_color(parts.fin_side, deep);
	set_color(parts.fin_tail, deep);
	set_color(parts.muzzle, belly);
	set_color(parts.nose, PET_INK);

	const int cx = PET_CREATURE_W / 2;
	const int bx = cx - body_w / 2;
	const int by = PET_BASE_Y - body_h;
	const int eye_y = by + body_h * 34 / 100;
	const int eye_dx = body_w * 19 / 100;
	const int eye_w = fish ? 9 : 7;
	const int eye_h = fish ? 11 : 9;
	const int head_gap = 14; // how far a hat sits above the body's top

	// Shadow, always under everything.
	place(parts.shadow, cx - 19, PET_BASE_Y + 2, 38, 5);
	lv_obj_set_style_bg_opa(parts.shadow, theme_is_dark() ? LV_OPA_50 : LV_OPA_20, 0);
	// A fish swims, so there is nothing under it to leave a mark on.
	show(parts.shadow, !fish);

	// ---- what is behind the body --------------------------------------
	// The tail curls up from behind the body's bottom corner, so it reads as a
	// tail rather than as a bar across the chin.
	// The tail arcs up out of the body's right side, where it can be seen whole:
	// a stub tucked behind the body reads as an arm.
	place(parts.tail, bx + body_w - 4, by + body_h - 22, cat ? 20 : 17, 7);
	lv_obj_set_style_transform_rotation(parts.tail, cat ? -550 : -450, 0);
	show(parts.tail, (cat || dog || dragon) && !fish);

	place(parts.tail_tip, bx + body_w + 8, by + body_h - 28, 7, 7);
	show(parts.tail_tip, dragon);

	place(parts.fin_tail, bx - 10, by + body_h / 2 - 10, 16, 20);
	lv_obj_set_style_transform_rotation(parts.fin_tail, 450, 0);
	show(parts.fin_tail, fish);

	// ---- the body ------------------------------------------------------
	place(parts.body, bx, by, body_w, body_h);
	// A fish's light underside sits low on the body; a land animal's patch is a
	// belly, so it takes the lower half of a round body.
	if (fish) {
		place(parts.belly, cx - body_w * 30 / 100, by + body_h * 60 / 100, body_w * 60 / 100, body_h * 30 / 100);
	} else {
		place(parts.belly, cx - body_w * 30 / 100, by + body_h * 54 / 100, body_w * 60 / 100, body_h * 40 / 100);
	}

	place(parts.fin_top, cx - 11, by - 8, 22, 10);
	show(parts.fin_top, fish);
	place(parts.fin_side, bx + 8, by + body_h * 56 / 100, 12, 7);
	show(parts.fin_side, fish);

	// Feet and arms: the fish has neither, and the newest animal has no feet.
	place(parts.foot_l, cx - 17, by + body_h - 4, 13, 6);
	place(parts.foot_r, cx + 4, by + body_h - 4, 13, 6);
	show(parts.foot_l, !fish && stage >= PET_STAGE_CHILD);
	show(parts.foot_r, !fish && stage >= PET_STAGE_CHILD);
	place(parts.arm_l, bx - 5, by + body_h * 52 / 100, 9, 15);
	place(parts.arm_r, bx + body_w - 4, by + body_h * 52 / 100, 9, 15);
	show(parts.arm_l, !fish);
	show(parts.arm_r, !fish);

	// ---- ears, horns, whiskers, muzzle ---------------------------------
	if (dog) {
		// A dog's ears hang: the same bars, turned the other way and lower.
		place(parts.ear_l, bx - 7, by + 2, 14, 26);
		place(parts.ear_r, bx + body_w - 7, by + 2, 14, 26);
		lv_obj_set_style_transform_rotation(parts.ear_l, -70, 0);
		lv_obj_set_style_transform_rotation(parts.ear_r, 70, 0);
	} else {
		// A cat's ears are triangles in everything but name: short, set wide,
		// and leaning out of the head.
		place(parts.ear_l, bx + body_w / 2 - 24, by - 9, 13, 13);
		place(parts.ear_r, bx + body_w / 2 + 11, by - 9, 13, 13);
		lv_obj_set_style_transform_rotation(parts.ear_l, -300, 0);
		lv_obj_set_style_transform_rotation(parts.ear_r, 300, 0);
	}
	show(parts.ear_l, cat || dog);
	show(parts.ear_r, cat || dog);

	place(parts.ear_in_l, bx + body_w / 2 - 21, by - 6, 3, 7);
	place(parts.ear_in_r, bx + body_w / 2 + 17, by - 6, 3, 7);
	lv_obj_set_style_transform_rotation(parts.ear_in_l, -300, 0);
	lv_obj_set_style_transform_rotation(parts.ear_in_r, 300, 0);
	show(parts.ear_in_l, cat);
	show(parts.ear_in_r, cat);

	place(parts.horn_l, bx + 6, by - head_gap - 2, 7, 16);
	place(parts.horn_r, bx + body_w - 13, by - head_gap - 2, 7, 16);
	lv_obj_set_style_transform_rotation(parts.horn_l, -140, 0);
	lv_obj_set_style_transform_rotation(parts.horn_r, 140, 0);
	show(parts.horn_l, dragon);
	show(parts.horn_r, dragon);

	place(parts.muzzle, cx - body_w * 18 / 100, by + body_h * 50 / 100, body_w * 36 / 100, body_h * 26 / 100);
	show(parts.muzzle, dog || dragon);

	place(parts.nose, cx - 3, by + body_h * 56 / 100, 7, 5);
	show(parts.nose, dog || dragon);

	// ---- the face ------------------------------------------------------
	place(parts.eye_l, cx - eye_dx - eye_w / 2, eye_y, eye_w, eye_h);
	place(parts.eye_r, cx + eye_dx - eye_w / 2, eye_y, eye_w, eye_h);
	place(parts.glint_l, cx - eye_dx - eye_w / 2 + 1, eye_y + 1, 3, 3);
	place(parts.glint_r, cx + eye_dx - eye_w / 2 + 1, eye_y + 1, 3, 3);
	place(parts.lid_l, cx - eye_dx - 5, eye_y + 4, 10, 2);
	place(parts.lid_r, cx + eye_dx - 5, eye_y + 4, 10, 2);

	bool eyes_open = !sleeping;
	show(parts.eye_l, eyes_open);
	show(parts.eye_r, eyes_open);
	show(parts.glint_l, eyes_open);
	show(parts.glint_r, eyes_open);
	show(parts.lid_l, sleeping);
	show(parts.lid_r, sleeping);

	place(parts.cheek_l, cx - body_w * 34 / 100 - 4, eye_y + 11, 9, 4);
	place(parts.cheek_r, cx + body_w * 34 / 100 - 4, eye_y + 11, 9, 4);
	lv_obj_set_style_bg_opa(parts.cheek_l, mood == PET_MOOD_BLISSFUL ? LV_OPA_70 : LV_OPA_40, 0);
	lv_obj_set_style_bg_opa(parts.cheek_r, mood == PET_MOOD_BLISSFUL ? LV_OPA_70 : LV_OPA_40, 0);

	bool sad = mood == PET_MOOD_SAD;
	bool smile = !sad && (mood == PET_MOOD_HAPPY || mood == PET_MOOD_BLISSFUL);
	bool line = mood == PET_MOOD_OKAY || sleeping;

	place(parts.mouth, cx - 9, eye_y + 12, 18, 12);
	if (sad) {
		lv_arc_set_angles(parts.mouth, 200, 340);
	} else {
		lv_arc_set_angles(parts.mouth, 20, 160);
	}
	show(parts.mouth, !sleeping && !listening && (smile || sad));

	place(parts.mouth_line, cx - 5, eye_y + 18, 10, 2);
	show(parts.mouth_line, !listening && line);

	place(parts.mouth_open, cx - 4, eye_y + 15, 8, 7);
	place(parts.tongue, cx - 2, eye_y + 19, 4, 2);
	show(parts.mouth_open, listening);
	show(parts.tongue, listening);

	place(parts.tear, cx - eye_dx - 7, eye_y + 11, 3, 7);
	show(parts.tear, sad);

	place(parts.zzz, PET_CREATURE_W - 32, 2, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	show(parts.zzz, sleeping);

	// ---- the wardrobe --------------------------------------------------
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

	place(parts.hat_brim, cx - 13, by - head_gap + 10, 26, 4);
	place(parts.hat_cone, cx - 9, by - head_gap - 4, 18, 14);
	show(parts.hat_brim, hat);
	show(parts.hat_cone, hat);

	place(parts.phone_band, cx - 17, by - head_gap + 4, 34, 5);
	place(parts.phone_left, cx - 21, by - head_gap + 9, 6, 16);
	place(parts.phone_right, cx + 15, by - head_gap + 9, 6, 16);
	show(parts.phone_band, phones);
	show(parts.phone_left, phones);
	show(parts.phone_right, phones);

	place(parts.scarf, cx - 15, by + body_h - 3, 30, 6);
	place(parts.scarf_tail, cx + 7, by + body_h + 2, 6, 10);
	show(parts.scarf, scarf);
	show(parts.scarf_tail, scarf);

	place(parts.crown_band, cx - 11, by - head_gap + 6, 22, 4);
	place(parts.crown_spike[0], cx - 10, by - head_gap + 1, 5, 6);
	place(parts.crown_spike[1], cx - 2, by - head_gap, 5, 7);
	place(parts.crown_spike[2], cx + 6, by - head_gap + 1, 5, 6);
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
		lv_obj_set_style_bg_opa(room_layer, LV_OPA_30, 0);
	} else {
		lv_color_t wash = PET_ROOM_MEADOW;
		if (room == 1) {
			wash = PET_ROOM_SUNSET;
		} else if (room == 2) {
			wash = PET_ROOM_NIGHT;
		}
		lv_obj_set_style_bg_color(room_layer, wash, 0);
		lv_obj_set_style_bg_opa(room_layer, LV_OPA_20, 0);
	}

	show(sand, fish);
	show(weed_l, fish);
	show(weed_r, fish);
	for (int i = 0; i < 3; i++) {
		show(bubbles[i], fish);
	}
}

// ---------------------------------------------------------------------------
// The words and the meters
// ---------------------------------------------------------------------------

static void paint_words(void) {
	if (!page_built) {
		return;
	}

	const pet_state_t *st = petwatch_state();
	int64_t now = petwatch_now();
	pet_stage_t stage = pet_stage(st);
	pet_mood_t mood = pet_mood(st, now);

	char name[40];
	pet_name(name, sizeof(name));
	lv_label_set_text(name_label, name);
	// The stage, not the mood: two words would not fit beside the meters, and
	// the face is already saying how it feels.
	lv_label_set_text(stage_label, tr(pet_stage_tag(stage)));

	lv_bar_set_value(health_bar, st->happy, LV_ANIM_OFF);
	lv_bar_set_value(energy_bar, st->energy, LV_ANIM_OFF);
	lv_label_set_text_fmt(health_value, "%d", st->happy);
	lv_label_set_text_fmt(energy_value, "%d", st->energy);
	lv_label_set_text_fmt(notes_value, "%d", st->notes);

	lv_label_set_text_fmt(today_label, tr("pet_today"), st->today_secs / 60, PET_DAY_GOAL_SECS / 60);
	if (st->streak > 0) {
		lv_label_set_text_fmt(streak_label, tr("pet_streak"), st->streak);
	} else {
		lv_label_set_text(streak_label, tr("pet_streak_none"));
	}

	char title[128];
	petwatch_track_title(title, sizeof(title));
	if (petwatch_listening()) {
		if (title[0]) {
			lv_label_set_text_fmt(status_label, tr("pet_feeding"), title);
		} else {
			lv_label_set_text(status_label, tr("pet_feeding_music"));
		}
	} else if (mood == PET_MOOD_SLEEPING) {
		lv_label_set_text(status_label, tr("pet_asleep"));
	} else {
		lv_label_set_text(status_label, tr("pet_feed_me"));
	}

	if (wardrobe_visible) {
		wardrobe_refresh();
	}
	if (customise_visible) {
		customise_refresh();
	}
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

	lv_obj_set_style_image_recolor(status_icon,
								   theme_is_monochrome() ? theme()->accent : theme_semantic_color(THEME_SEMANTIC_GREEN), 0);

	const lv_color_t note_color = theme()->accent;
	lv_obj_set_style_image_recolor(note_a, note_color, 0);
	lv_obj_set_style_image_recolor(note_b, note_color, 0);
}

static void paint(void) {
	if (!page_built) {
		return;
	}
	paint_room();
	paint_creature();
	paint_words();
	paint_theme_colors();
}

// ---------------------------------------------------------------------------
// The animation: slow, and slower still when nothing is playing
// ---------------------------------------------------------------------------

static void bob_exec(void *var, int32_t value) { lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0); }

static void sway_exec(void *var, int32_t value) { lv_obj_set_style_transform_rotation((lv_obj_t *)var, value, 0); }

static void note_exec(void *var, int32_t value) {
	lv_obj_t *o = (lv_obj_t *)var;
	lv_obj_set_style_translate_y(o, value, 0);
	lv_obj_set_style_opa(o, (lv_opa_t)(LV_OPA_20 + value * 2), 0);
}

static void bubble_exec(void *var, int32_t value) { lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0); }

static void start_anim(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t duration,
					   uint32_t delay, bool back) {
	if (!obj) {
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, exec);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, duration);
	lv_anim_set_playback_duration(&a, back ? duration : 0);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_set_delay(&a, delay);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
	lv_anim_start(&a);
}

static void start_animations(void) {
	if (!page_built || animating) {
		return;
	}
	animating = true;

	// The hardware this runs on is slow, and the pet is meant to look it: a
	// breath takes five seconds when nothing is playing, and even the fastest
	// it moves is slower than a person's breathing.
	bool listening = petwatch_listening();
	uint32_t breath = 2600;
	int32_t rise = 2;
	if (listening) {
		switch (petwatch_vibe()) {
		case PET_VIBE_ENERGETIC:
			breath = 1100;
			rise = 3;
			break;
		case PET_VIBE_CALM:
			breath = 1900;
			break;
		case PET_VIBE_NEUTRAL:
		default:
			breath = 1500;
			break;
		}
	}
	start_anim(parts.root, bob_exec, 0, rise, breath, 0, true);

	// The tail, or the fin, sways the whole time: eight seconds one way, and
	// eight the other.
	lv_obj_t *sway = pet_species(petwatch_state()) == PET_SPECIES_FISH ? parts.fin_tail : parts.tail;
	start_anim(sway, sway_exec, -70, 70, 4000, 0, true);

	if (listening) {
		show(note_a, true);
		show(note_b, true);
		start_anim(note_a, note_exec, 0, 26, 4200, 0, true);
		start_anim(note_b, note_exec, 0, 22, 5200, 900, true);
	} else {
		show(note_a, false);
		show(note_b, false);
	}

	if (pet_species(petwatch_state()) == PET_SPECIES_FISH) {
		for (int i = 0; i < 3; i++) {
			start_anim(bubbles[i], bubble_exec, 0, -46, 9000 + i * 2400, i * 2600, false);
		}
	}
}

static void stop_animations(void) {
	if (!page_built) {
		return;
	}
	lv_anim_delete(parts.root, bob_exec);
	lv_anim_delete(parts.tail, sway_exec);
	lv_anim_delete(parts.fin_tail, sway_exec);
	lv_anim_delete(note_a, note_exec);
	lv_anim_delete(note_b, note_exec);
	for (int i = 0; i < 3; i++) {
		lv_anim_delete(bubbles[i], bubble_exec);
		lv_obj_set_style_translate_y(bubbles[i], 0, 0);
	}
	lv_obj_set_style_translate_y(parts.root, 0, 0);
	show(note_a, false);
	show(note_b, false);
	animating = false;
	last_vibe = -1;
}

static void sync_animation(void) {
	if (!page_visible || !page_built) {
		return;
	}
	bool listening = petwatch_listening() && pet_mood(petwatch_state(), petwatch_now()) != PET_MOOD_SLEEPING;
	int vibe = (int)petwatch_vibe();
	if (animating && (listening != last_listening || (listening && vibe != last_vibe))) {
		stop_animations();
	}
	last_listening = listening;
	last_vibe = vibe;
	start_animations();
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
	lv_obj_t *close_icon = lv_image_create(close);
	lv_image_set_src(close_icon, &icon_close);
	lv_obj_add_style(close_icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(close_icon, page_cfg);
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
	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(cancel_icon, page_cfg);
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
	if (events & PET_EVENT_STAGE_UP) {
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
	paint();
	sync_animation();
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
	settingsrow_title_corner_slots(title, page_cfg, 2);

	// Two corner controls: the wardrobe behind the star (slot 0, the outermost)
	// and the name and the animal behind the pencil.
	corner_button(page_cfg, 0, &icon_star, wardrobe_open_cb);
	corner_button(page_cfg, 1, &icon_pencil, customise_open_cb);

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

	// The aquarium: a strip of sand, two weeds and three slow bubbles.
	sand = bar_piece(habitat, PET_SAND, 6);
	place(sand, 4, habitat_h - 12, card_w - 8, 10);
	weed_l = bar_piece(habitat, PET_WEED, 3);
	place(weed_l, 16, habitat_h - 54, 6, 42);
	weed_r = bar_piece(habitat, PET_WEED, 3);
	place(weed_r, card_w - 24, habitat_h - 48, 5, 36);
	for (int i = 0; i < 3; i++) {
		bubbles[i] = disc(habitat, PET_BUBBLE);
		lv_obj_set_style_bg_opa(bubbles[i], LV_OPA_50, 0);
	}
	place(bubbles[0], card_w - 34, habitat_h - 70, 8, 8);
	place(bubbles[1], card_w - 52, habitat_h - 96, 5, 5);
	place(bubbles[2], card_w - 22, habitat_h - 118, 6, 6);

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

	const int hud_x = card_w - 10 - PET_HUD_W;
	health_icon = icon_image(habitat, &icon_heart_filled, 12);
	place(health_icon, hud_x, 11, 12, 12);
	health_bar = lv_bar_create(habitat);
	lv_obj_set_size(health_bar, 44, 6);
	lv_obj_set_pos(health_bar, hud_x + 16, 14);
	lv_bar_set_range(health_bar, 0, PET_HAPPY_MAX);
	lv_bar_set_value(health_bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_radius(health_bar, 3, 0);
	lv_obj_set_style_radius(health_bar, 3, LV_PART_INDICATOR);
	lv_obj_set_style_border_width(health_bar, 0, 0);
	health_value = lv_label_create(habitat);
	lv_obj_add_style(health_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(health_value, &font_ui_14, 0);
	lv_obj_set_width(health_value, 24);
	lv_obj_set_style_text_align(health_value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(health_value, hud_x + 64, 7);
	lv_label_set_text(health_value, "0");

	energy_icon = icon_image(habitat, &icon_zap, 12);
	place(energy_icon, hud_x, 29, 12, 12);
	energy_bar = lv_bar_create(habitat);
	lv_obj_set_size(energy_bar, 44, 6);
	lv_obj_set_pos(energy_bar, hud_x + 16, 32);
	lv_bar_set_range(energy_bar, 0, PET_ENERGY_MAX);
	lv_bar_set_value(energy_bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_radius(energy_bar, 3, 0);
	lv_obj_set_style_radius(energy_bar, 3, LV_PART_INDICATOR);
	lv_obj_set_style_border_width(energy_bar, 0, 0);
	energy_value = lv_label_create(habitat);
	lv_obj_add_style(energy_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(energy_value, &font_ui_14, 0);
	lv_obj_set_width(energy_value, 24);
	lv_obj_set_style_text_align(energy_value, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(energy_value, hud_x + 64, 25);
	lv_label_set_text(energy_value, "0");

	notes_icon = icon_image(habitat, &icon_music_note, 12);
	place(notes_icon, hud_x, 48, 12, 12);
	notes_value = lv_label_create(habitat);
	lv_obj_add_style(notes_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(notes_value, &font_ui_14, 0);
	lv_obj_set_width(notes_value, 60);
	lv_obj_set_pos(notes_value, hud_x + 16, 46);
	lv_label_set_text(notes_value, "0");

	// ---- the animal ------------------------------------------------------
	build_creature();
	lv_obj_set_pos(parts.root, (card_w - PET_CREATURE_W) / 2, 44);

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
	status_icon = icon_image(habitat, &icon_music_note, 12);
	place(status_icon, 12, habitat_h - 28, 12, 12);

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
