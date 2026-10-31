#include "petpage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/pet/peticon.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h" // switch_screen
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/pet/pet.h"
#include "src/system/pet/petwatch.h"

lv_obj_t *petpage_screen;

// The page's two cards and the button under them, on the V1's 240x320 panel.
// The numbers are the compact ones; bp_pick() gives the same construction room
// on a wider panel, as every other page does.
#define PET_CARD_TOP_INSET bp_pick(0, 0) // the content already starts below the header
#define PET_HABITAT_H bp_pick(124, 180)
#define PET_STATS_H bp_pick(80, 108)
#define PET_BUTTON_H bp_pick(32, 46)
#define PET_CARD_GAP bp_pick(3, 8)

// The creature's own stage, inside the habitat card. It is one size for every
// animal; the parts are placed inside it, so the ears and feet stay on it.
#define PET_ROOT_W 84
#define PET_ROOT_H 76
#define PET_BASE_Y 66 // where the body's feet sit, measured from the root's top

// The palette. These are the one thing on the page that does not follow the
// theme: a pastel creature reads on a white card, a black card and Tokyo
// Night's blue-black alike, and a body that turned into the accent colour would
// be a conversation with the theme rather than a pet. Everything around it --
// cards, words, hearts, bar, button -- is the theme's.
#define PET_BODY lv_color_hex(0x8FD6B0)
#define PET_BODY_DEEP lv_color_hex(0x62B78C)
#define PET_BELLY lv_color_hex(0xE7F8EE)
#define PET_INK lv_color_hex(0x2C3E36)
#define PET_CHEEK lv_color_hex(0xF2A0A1)
#define PET_TEAR lv_color_hex(0x7FC4E8)
#define PET_SPOT lv_color_hex(0x5FAF84)

typedef struct {
	lv_obj_t *root;

	lv_obj_t *shadow;
	lv_obj_t *ear_l;
	lv_obj_t *ear_r;
	lv_obj_t *antenna;
	lv_obj_t *antenna_tip;
	lv_obj_t *arm_l;
	lv_obj_t *arm_r;
	lv_obj_t *body;
	lv_obj_t *belly;
	lv_obj_t *foot_l;
	lv_obj_t *foot_r;
	lv_obj_t *spots[3];

	lv_obj_t *eye_l;
	lv_obj_t *eye_r;
	lv_obj_t *glint_l;
	lv_obj_t *glint_r;
	lv_obj_t *lid_l;
	lv_obj_t *lid_r;
	lv_obj_t *cheek_l;
	lv_obj_t *cheek_r;
	lv_obj_t *mouth; // the arc: a smile or a frown
	lv_obj_t *mouth_line;
	lv_obj_t *mouth_open;
	lv_obj_t *tongue;
	lv_obj_t *tear;
	lv_obj_t *star;
	lv_obj_t *zzz;
} pet_parts_t;

static pet_parts_t parts;

static gui_config_t *page_cfg;
static bool page_built;
static bool page_visible;
static bool bobbing;
static lv_timer_t *page_timer;

static lv_obj_t *name_label;
static lv_obj_t *stage_label;
static lv_obj_t *status_icon;
static lv_obj_t *status_label;
static lv_obj_t *hearts[5];
static lv_obj_t *energy_label;
static lv_obj_t *energy_bar;
static lv_obj_t *today_label;
static lv_obj_t *streak_label;
static lv_obj_t *pet_button;
static lv_obj_t *pet_button_label;
static lv_obj_t *note_a;
static lv_obj_t *note_b;

// The balance in the title row: a small note glyph and the number, to the left
// of the wardrobe's star. The pet's own card has no room for a fifth line on a
// 240 px panel, and the number belongs beside the way into the shop anyway.
static lv_obj_t *balance_label;

// The wardrobe. Built the first time it is opened -- a player who never looks
// at it pays nothing -- and a full-page sheet over the pet rather than a screen
// of its own, so the shell's back chevron closes it first (see pet_back_guard).
static lv_obj_t *wardrobe_screen;
static lv_obj_t *wardrobe_column;
static lv_obj_t *wardrobe_balance;
static lv_obj_t *wardrobe_value[PET_ITEM_COUNT];
static bool wardrobe_visible;

// What the bobbing animation was last started for, so a change of track's vibe
// restarts it at the new speed instead of every page tick restarting it.
static int last_vibe = -1;

// ---------------------------------------------------------------------------
// The small pieces the creature is made of
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

// The mouth. One arc is both a smile and a frown: LVGL draws the indicator
// clockwise from the first angle to the second, so the bottom half (20..160) is
// a smile and the top half (200..340) is the frown a sad pet wears.
static lv_obj_t *mouth_arc(lv_obj_t *parent) {
	lv_obj_t *a = lv_arc_create(parent);
	lv_obj_remove_style(a, NULL, LV_PART_KNOB);
	lv_obj_set_clickable(a, false);
	lv_obj_set_scrollable(a, false);
	lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
	lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, 0);
	lv_obj_set_style_arc_width(a, 2, LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(a, PET_INK, LV_PART_INDICATOR);
	return a;
}

static lv_obj_t *note_image(lv_obj_t *parent, int size) {
	lv_obj_t *img = lv_image_create(parent);
	lv_image_set_src(img, &icon_music_note);
	// The icon set is drawn at its own side, not at 32: divide by the source's
	// height so the glyph lands at `size` whatever the art was cut at.
	lv_image_set_scale(img, (uint32_t)((int64_t)LV_SCALE_NONE * size / (int)icon_music_note.header.h));
	lv_obj_set_size(img, size, size);
	lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
	return img;
}

// ---------------------------------------------------------------------------
// Building the creature
// ---------------------------------------------------------------------------

static void build_pet(lv_obj_t *parent) {
	memset(&parts, 0, sizeof(parts));
	parts.root = plain(parent);
	lv_obj_set_size(parts.root, PET_ROOT_W, PET_ROOT_H);
	lv_obj_set_clickable(parts.root, true);
	lv_obj_set_style_bg_opa(parts.root, LV_OPA_TRANSP, 0);

	// The shadow is the first child, so it is drawn under everything else.
	parts.shadow = disc(parts.root, lv_color_black());

	// Behind the body: the ears, the antenna and the arms. The body is drawn
	// over their lower halves, which is what keeps the silhouette one shape.
	parts.ear_l = disc(parts.root, PET_BODY);
	parts.ear_r = disc(parts.root, PET_BODY);
	parts.antenna = bar_piece(parts.root, PET_BODY_DEEP, 1);
	parts.antenna_tip = disc(parts.root, PET_BODY);
	parts.arm_l = bar_piece(parts.root, PET_BODY_DEEP, 3);
	parts.arm_r = bar_piece(parts.root, PET_BODY_DEEP, 3);

	// The body, with the belly in front of it.
	parts.body = disc(parts.root, PET_BODY);
	lv_obj_set_style_bg_grad_color(parts.body, PET_BODY_DEEP, 0);
	lv_obj_set_style_bg_grad_dir(parts.body, LV_GRAD_DIR_VER, 0);
	parts.belly = disc(parts.root, PET_BELLY);
	lv_obj_set_style_bg_opa(parts.belly, LV_OPA_50, 0);

	// The egg's spots, cut into it by being drawn the darker green.
	for (int i = 0; i < 3; i++) {
		parts.spots[i] = disc(parts.root, PET_SPOT);
		lv_obj_set_style_bg_opa(parts.spots[i], LV_OPA_60, 0);
	}

	parts.foot_l = bar_piece(parts.root, PET_BODY_DEEP, 3);
	parts.foot_r = bar_piece(parts.root, PET_BODY_DEEP, 3);

	// The face. The two eyes and their highlights are circles; the lids, the
	// cheeks and the closed mouth are little rounded bars.
	for (int i = 0; i < 2; i++) {
		lv_obj_t **eye = i == 0 ? &parts.eye_l : &parts.eye_r;
		lv_obj_t **glint = i == 0 ? &parts.glint_l : &parts.glint_r;
		lv_obj_t **lid = i == 0 ? &parts.lid_l : &parts.lid_r;
		lv_obj_t **cheek = i == 0 ? &parts.cheek_l : &parts.cheek_r;
		*eye = disc(parts.root, PET_INK);
		*glint = disc(parts.root, lv_color_white());
		*lid = bar_piece(parts.root, PET_INK, 1);
		*cheek = disc(parts.root, PET_CHEEK);
	}

	parts.mouth = mouth_arc(parts.root);
	parts.mouth_line = bar_piece(parts.root, PET_INK, 1);
	parts.mouth_open = disc(parts.root, PET_INK);
	parts.tongue = disc(parts.root, PET_CHEEK);
	parts.tear = disc(parts.root, PET_TEAR);

	// The hat the last stage wears, and the snore.
	parts.star = note_image(parts.root, 14);
	lv_obj_add_style(parts.star, &theme_style_icon, 0);

	parts.zzz = lv_label_create(parts.root);
	lv_obj_add_style(parts.zzz, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(parts.zzz, &font_ui_14, 0);
	lv_label_set_text(parts.zzz, "z z");

}

// ---------------------------------------------------------------------------
// The mood, drawn
// ---------------------------------------------------------------------------

static void paint_pet(void) {
	if (!page_built) {
		return;
	}

	const pet_state_t *st = petwatch_state();
	pet_stage_t stage = pet_stage(st);
	pet_mood_t mood = pet_mood(st, petwatch_now());
	bool sleeping = mood == PET_MOOD_SLEEPING;
	bool egg = stage == PET_STAGE_EGG;
	bool listening = petwatch_listening() && !sleeping;

	// One body per stage: the egg is tall and narrow, the hatchling round, and
	// each stage after that a little wider. Everything else is placed from
	// these three numbers.
	int body_w = 48;
	int body_h = 42;
	switch (stage) {
	case PET_STAGE_EGG:
		body_w = 40;
		body_h = 50;
		break;
	case PET_STAGE_BABY:
		body_w = 48;
		body_h = 42;
		break;
	case PET_STAGE_CHILD:
		body_w = 54;
		body_h = 46;
		break;
	case PET_STAGE_TEEN:
		body_w = 60;
		body_h = 48;
		break;
	case PET_STAGE_ADULT:
		body_w = 64;
		body_h = 52;
		break;
	default:
		break;
	}

	const int cx = PET_ROOT_W / 2;
	const int bx = cx - body_w / 2;
	const int by = PET_BASE_Y - body_h;
	const int eye_y = by + body_h * 36 / 100;
	const int eye_dx = body_w * 18 / 100;
	const int eye_w = 7;
	const int eye_h = 9;

	// Shadow, always under everything.
	place(parts.shadow, cx - 19, PET_BASE_Y + 2, 38, 5);
	lv_obj_set_style_bg_color(parts.shadow, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(parts.shadow, theme_is_dark() ? LV_OPA_50 : LV_OPA_20, 0);

	// Ears, arms, antenna.
	place(parts.ear_l, bx + 1, by - 7, 15, 15);
	place(parts.ear_r, bx + body_w - 16, by - 7, 15, 15);
	place(parts.arm_l, bx - 5, by + body_h * 50 / 100, 8, 17);
	place(parts.arm_r, bx + body_w - 3, by + body_h * 50 / 100, 8, 17);
	place(parts.antenna, cx - 1, by - 12, 2, 13);
	place(parts.antenna_tip, cx - 3, by - 18, 7, 7);

	show(parts.ear_l, stage >= PET_STAGE_CHILD);
	show(parts.ear_r, stage >= PET_STAGE_CHILD);
	show(parts.arm_l, stage >= PET_STAGE_TEEN);
	show(parts.arm_r, stage >= PET_STAGE_TEEN);
	show(parts.antenna, stage == PET_STAGE_BABY);
	show(parts.antenna_tip, stage == PET_STAGE_BABY);

	// Body and belly.
	place(parts.body, bx, by, body_w, body_h);
	place(parts.belly, cx - body_w * 31 / 100, by + body_h * 56 / 100, body_w * 62 / 100, body_h * 40 / 100);

	// Feet, peeking out from under the body.
	place(parts.foot_l, cx - 17, by + body_h - 4, 13, 6);
	place(parts.foot_r, cx + 4, by + body_h - 4, 13, 6);
	show(parts.foot_l, stage >= PET_STAGE_CHILD);
	show(parts.foot_r, stage >= PET_STAGE_CHILD);

	// The egg's markings.
	place(parts.spots[0], cx - 10, by + 10, 6, 6);
	place(parts.spots[1], cx + 4, by + 18, 5, 5);
	place(parts.spots[2], cx - 4, by + 29, 6, 6);
	for (int i = 0; i < 3; i++) {
		show(parts.spots[i], egg);
	}

	// The face.
	place(parts.eye_l, cx - eye_dx - eye_w / 2, eye_y, eye_w, eye_h);
	place(parts.eye_r, cx + eye_dx - eye_w / 2, eye_y, eye_w, eye_h);
	place(parts.glint_l, cx - eye_dx - eye_w / 2 + 1, eye_y + 1, 2, 2);
	place(parts.glint_r, cx + eye_dx - eye_w / 2 + 1, eye_y + 1, 2, 2);
	place(parts.lid_l, cx - eye_dx - 5, eye_y + 4, 10, 2);
	place(parts.lid_r, cx + eye_dx - 5, eye_y + 4, 10, 2);

	bool eyes_open = !egg && !sleeping;
	show(parts.eye_l, eyes_open);
	show(parts.eye_r, eyes_open);
	show(parts.glint_l, eyes_open);
	show(parts.glint_r, eyes_open);
	show(parts.lid_l, !egg && sleeping);
	show(parts.lid_r, !egg && sleeping);

	place(parts.cheek_l, cx - body_w * 34 / 100 - 4, eye_y + 11, 9, 4);
	place(parts.cheek_r, cx + body_w * 34 / 100 - 4, eye_y + 11, 9, 4);
	show(parts.cheek_l, !egg);
	show(parts.cheek_r, !egg);
	lv_obj_set_style_bg_opa(parts.cheek_l, mood == PET_MOOD_BLISSFUL ? LV_OPA_70 : LV_OPA_40, 0);
	lv_obj_set_style_bg_opa(parts.cheek_r, mood == PET_MOOD_BLISSFUL ? LV_OPA_70 : LV_OPA_40, 0);

	// The mouth: a smile, a frown, a quiet line, a snore, or open for a song.
	bool sad = mood == PET_MOOD_SAD;
	bool smile = !sad && (mood == PET_MOOD_HAPPY || mood == PET_MOOD_BLISSFUL);
	bool line = mood == PET_MOOD_OKAY || sleeping;

	place(parts.mouth, cx - 9, eye_y + 11, 18, 12);
	if (sad) {
		lv_arc_set_angles(parts.mouth, 200, 340);
	} else {
		lv_arc_set_angles(parts.mouth, 20, 160);
	}
	show(parts.mouth, !egg && !sleeping && !listening && (smile || sad));

	place(parts.mouth_line, cx - 5, eye_y + 17, 10, 2);
	show(parts.mouth_line, !egg && !listening && line);

	place(parts.mouth_open, cx - 4, eye_y + 14, 8, 7);
	place(parts.tongue, cx - 2, eye_y + 18, 4, 2);
	show(parts.mouth_open, !egg && listening);
	show(parts.tongue, !egg && listening);

	place(parts.tear, cx - eye_dx - 7, eye_y + 11, 3, 7);
	show(parts.tear, !egg && sad);

	// The last stage wears a music note; an egg has no head to wear one on.
	place(parts.star, cx - 7, by - 10, 14, 14);
	show(parts.star, stage == PET_STAGE_ADULT);

	place(parts.zzz, 62, 7, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	show(parts.zzz, sleeping);
}

// ---------------------------------------------------------------------------
// The words and the meters
// ---------------------------------------------------------------------------

static void paint_stats(void) {
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
	lv_label_set_text_fmt(stage_label, "%s - %s", tr(pet_stage_tag(stage)), tr(pet_mood_tag(mood)));

	int filled = pet_hearts(st);
	for (int i = 0; i < 5; i++) {
		lv_image_set_src(hearts[i], i < filled ? &icon_heart_filled : &icon_heart_outline);
		lv_obj_set_style_image_recolor(hearts[i],
									   i < filled ? (theme_is_monochrome() ? theme()->accent : lv_color_hex(0xE2574C))
												  : theme()->text_secondary,
									   0);
		lv_obj_set_style_image_recolor_opa(hearts[i], i < filled ? LV_OPA_COVER : LV_OPA_50, 0);
	}

	lv_bar_set_value(energy_bar, st->energy, LV_ANIM_OFF);
	lv_label_set_text_fmt(balance_label, "%d", st->notes);
	lv_label_set_text_fmt(today_label, tr("pet_today"), st->today_secs / 60, PET_DAY_GOAL_SECS / 60);
	if (st->streak > 0) {
		lv_label_set_text_fmt(streak_label, tr("pet_streak"), st->streak);
	} else {
		lv_label_set_text(streak_label, tr("pet_streak_none"));
	}

	// What it is feeding on. A radio station, a track's title, or the name of
	// a file that carries no tags; the watcher has already done the asking.
	char title[128];
	petwatch_track_title(title, sizeof(title));
	if (petwatch_listening()) {
		if (title[0]) {
			lv_label_set_text_fmt(status_label, tr("pet_feeding"), title);
		} else {
			lv_label_set_text(status_label, tr("pet_feeding_music"));
		}
	} else if (pet_mood(st, now) == PET_MOOD_SLEEPING) {
		lv_label_set_text(status_label, tr("pet_asleep"));
	} else {
		lv_label_set_text(status_label, tr("pet_feed_me"));
	}
}

// ---------------------------------------------------------------------------
// Colours that follow the theme
// ---------------------------------------------------------------------------

static void paint_theme_colors(void) {
	if (!page_built) {
		return;
	}

	// The button's fill and the energy bar's fill come from the shared accent
	// style, but the ink on the button is read at paint time.
	lv_obj_set_style_text_color(pet_button_label, theme_on_accent(), 0);
	lv_obj_set_style_bg_color(energy_bar, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_color(energy_bar, theme()->accent, LV_PART_INDICATOR);

	lv_obj_set_style_image_recolor(status_icon,
								   theme_is_monochrome() ? theme()->accent : theme_semantic_color(THEME_SEMANTIC_GREEN), 0);
	lv_obj_set_style_text_color(energy_label, theme()->text_secondary, 0);
	lv_obj_set_style_text_color(today_label, theme()->text_secondary, 0);
	lv_obj_set_style_text_color(streak_label, theme()->text_secondary, 0);

	const lv_color_t note_color = theme()->accent;
	lv_obj_set_style_image_recolor(note_a, note_color, 0);
	lv_obj_set_style_image_recolor(note_b, note_color, 0);
}

static void paint(void) {
	if (!page_built) {
		return;
	}
	paint_pet();
	paint_stats();
	paint_theme_colors();
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
	if (balance_label) {
		lv_label_set_text_fmt(balance_label, "%d", st->notes);
	}

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
}

static void build_wardrobe(void) {
	if (wardrobe_screen || !page_cfg) {
		return;
	}

	wardrobe_screen = lv_obj_create(petpage_screen);
	lv_obj_set_size(wardrobe_screen, page_cfg->screen_width, page_cfg->screen_height);
	lv_obj_set_pos(wardrobe_screen, 0, 0);
	lv_obj_set_style_pad_all(wardrobe_screen, 0, 0);
	lv_obj_set_scrollable(wardrobe_screen, false);
	lv_obj_set_hidden(wardrobe_screen, true);

	wardrobe_column = settingsrow_page(wardrobe_screen, page_cfg, "pet_wardrobe");
	settingsrow_add(wardrobe_column, "pet_notes_balance", &wardrobe_balance, NULL, NULL);

	for (int i = 0; i < PET_ITEM_COUNT; i++) {
		pet_item_t item = (pet_item_t)i;
		lv_obj_t *row = settingsrow_add(wardrobe_column, pet_item_tag(item), &wardrobe_value[i],
										wardrobe_item_cb, (void *)(intptr_t)i);
		if (item == PET_ITEM_STREAK_FREEZE || item >= PET_ITEM_FIRST_ROOM) {
			// The freezes and the room themes are the two halves of the shop
			// that are not clothes; a caption says what they are for.
			settingsrow_add_caption(row, item == PET_ITEM_STREAK_FREEZE ? tr("pet_freeze_note") : tr("pet_room_note"));
		}
	}

	wardrobe_refresh();
}

static void wardrobe_close(void);

static void wardrobe_open_cb(lv_event_t *e) {
	(void)e;
	if (!page_built) {
		return;
	}
	if (wardrobe_visible) {
		// The star is a door that closes behind you: a second press puts the
		// wardrobe away, which saves a trip to the corner of the screen.
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

// The shell's back chevron: while the wardrobe is up, back closes it rather
// than leaving the pet. False hands the press back to the chevron.
static bool pet_back_guard(void) {
	if (!wardrobe_visible) {
		return false;
	}
	wardrobe_close();
	return true;
}

// ---------------------------------------------------------------------------
// The animation: a bob and two drifting notes, only while there is music
// ---------------------------------------------------------------------------

static void bob_exec(void *var, int32_t value) { lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0); }

static void note_exec(void *var, int32_t value) {
	lv_obj_t *o = (lv_obj_t *)var;
	lv_obj_set_style_translate_y(o, value, 0);
	lv_obj_set_style_opa(o, (lv_opa_t)(LV_OPA_20 + value * 2), 0);
}

static void start_anim_for(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t duration,
						   uint32_t delay) {
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, obj);
	lv_anim_set_exec_cb(&a, exec);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, duration);
	lv_anim_set_playback_duration(&a, duration);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_set_delay(&a, delay);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
	lv_anim_start(&a);
}

// How the creature moves follows the vibe of what it is hearing: a slow sway
// for the calm genres, a faster bob for the loud ones. It is the same drawing,
// and the difference is read in the speed rather than in a new animation the
// V1's frame budget would notice.
static int bob_height(void) {
	pet_vibe_t vibe = petwatch_vibe();
	return vibe == PET_VIBE_ENERGETIC ? 5 : (vibe == PET_VIBE_CALM ? 2 : 3);
}

static uint32_t bob_duration(void) {
	pet_vibe_t vibe = petwatch_vibe();
	return vibe == PET_VIBE_ENERGETIC ? (uint32_t)520 : (vibe == PET_VIBE_CALM ? (uint32_t)1500 : (uint32_t)900);
}

static void start_bobbing(void) {
	if (bobbing || !page_built) {
		return;
	}
	bobbing = true;
	uint32_t duration = bob_duration();
	start_anim_for(parts.root, bob_exec, 0, bob_height(), duration, 0);
	start_anim_for(note_a, note_exec, 0, 34, duration * 3 / 2, 0);
	start_anim_for(note_b, note_exec, 0, 30, duration * 2, duration / 2);
}

static void stop_bobbing(void) {
	if (!page_built) {
		return;
	}
	lv_anim_delete(parts.root, bob_exec);
	lv_anim_delete(note_a, note_exec);
	lv_anim_delete(note_b, note_exec);
	lv_obj_set_style_translate_y(parts.root, 0, 0);
	lv_obj_set_style_translate_y(note_a, 0, 0);
	lv_obj_set_style_translate_y(note_b, 0, 0);
	lv_obj_set_style_opa(note_a, LV_OPA_TRANSP, 0);
	lv_obj_set_style_opa(note_b, LV_OPA_TRANSP, 0);
	bobbing = false;
}

static void sync_animation(void) {
	bool listening = petwatch_listening() && pet_mood(petwatch_state(), petwatch_now()) != PET_MOOD_SLEEPING;
	show(note_a, listening);
	show(note_b, listening);
	if (listening) {
		// A new track of a different vibe restarts the bob at its own speed.
		int vibe = (int)petwatch_vibe();
		if (bobbing && vibe != last_vibe) {
			stop_bobbing();
		}
		last_vibe = vibe;
		start_bobbing();
	} else {
		last_vibe = -1;
		stop_bobbing();
	}
	if (wardrobe_visible) {
		wardrobe_refresh();
	}
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static void consume_events(void) {
	int achievement = petwatch_take_achievement();
	if (achievement != 0) {
		// The achievement's own name, after the word that says what it is.
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

static void pet_click_cb(lv_event_t *e) {
	(void)e;
	if (petwatch_pet_now()) {
		toast_plain(tr("pet_purr"));
	} else {
		toast_plain(tr("pet_cooldown"));
	}
	paint();
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
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	page_visible = false;
	wardrobe_close();
	if (page_timer) {
		lv_timer_delete(page_timer);
		page_timer = NULL;
	}
	stop_bobbing();
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
	settingsrow_title_corner_slots(title, page_cfg, 1);

	// The wardrobe's way in, and the notes beside it: the same 36 px corner
	// control every other page puts there.
	lv_obj_t *wardrobe_button = lv_btn_create(petpage_screen);
	settingsrow_place_corner_button(wardrobe_button, page_cfg, 0);
	lv_obj_set_style_bg_opa(wardrobe_button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(wardrobe_button, 0, 0);
	lv_obj_set_style_shadow_width(wardrobe_button, 0, 0);
	lv_obj_set_style_pad_all(wardrobe_button, 0, 0);
	lv_obj_add_event_cb(wardrobe_button, wardrobe_open_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *star = lv_image_create(wardrobe_button);
	lv_image_set_src(star, &icon_star);
	lv_obj_add_style(star, &theme_style_icon, 0);
	settingsrow_scale_corner_icon(star, page_cfg);
	lv_obj_center(star);

	lv_obj_t *balance = plain(petpage_screen);
	lv_obj_set_size(balance, LV_SIZE_CONTENT, 20);
	lv_obj_set_flex_flow(balance, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(balance, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(balance, 2, 0);
	lv_obj_align(balance, LV_ALIGN_TOP_RIGHT,
				 -(page_cfg->padding + settingsrow_corner_button_size(page_cfg) +
				   settingsrow_corner_button_gap(page_cfg) + 2),
				 page_cfg->padding + page_cfg->top_bar_height + 9);

	lv_obj_t *balance_icon = note_image(balance, 14);
	lv_obj_add_style(balance_icon, &theme_style_icon, 0);

	balance_label = lv_label_create(balance);
	lv_obj_add_style(balance_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(balance_label, &font_ui_14, 0);
	lv_label_set_text(balance_label, "0");

	const int pad = page_cfg->padding;
	const int card_w = page_cfg->screen_width - 2 * pad;
	const int content_top = settingsrow_content_top(page_cfg);

	// ---- the habitat ------------------------------------------------------
	lv_obj_t *habitat = lv_obj_create(petpage_screen);
	lv_obj_add_style(habitat, &theme_style_card, 0);
	lv_obj_set_size(habitat, card_w, PET_HABITAT_H);
	lv_obj_set_pos(habitat, pad, content_top + PET_CARD_TOP_INSET);
	lv_obj_set_style_radius(habitat, bp_pick(12, 18), 0);
	lv_obj_set_style_shadow_width(habitat, 0, 0);
	lv_obj_set_style_pad_all(habitat, 0, 0);
	lv_obj_set_scrollable(habitat, false);

	name_label = lv_label_create(habitat);
	lv_obj_add_style(name_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(name_label, &font_ui_18, 0);
	lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name_label, 64);
	lv_obj_set_pos(name_label, 10, 5);
	lv_label_set_text(name_label, "Momo");

	stage_label = lv_label_create(habitat);
	lv_obj_add_style(stage_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(stage_label, &font_ui_14, 0);
	// One line only: LV_LABEL_LONG_DOT needs a pinned height to have an edge
	// to dot at, or a long stage-and-mood pair wraps onto a second line and
	// grows into the creature's ears.
	lv_obj_set_width(stage_label, card_w - 84);
	lv_obj_set_height(stage_label, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_style_text_align(stage_label, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode(stage_label, LV_LABEL_LONG_DOT);
	lv_obj_align(stage_label, LV_ALIGN_TOP_RIGHT, -10, 3);
	lv_label_set_text(stage_label, "");

	build_pet(habitat);
	lv_obj_set_pos(parts.root, (card_w - PET_ROOT_W) / 2, bp_pick(20, 46));
	lv_obj_add_event_cb(parts.root, pet_click_cb, LV_EVENT_CLICKED, NULL);

	// The notes drift up from the creature and out of the card's top; they
	// live in the habitat so a drift cannot cross the card's edge.
	note_a = note_image(habitat, 16);
	lv_obj_set_pos(note_a, 36, 66);
	note_b = note_image(habitat, 13);
	lv_obj_set_pos(note_b, card_w - 50, 72);
	lv_obj_set_style_opa(note_a, LV_OPA_TRANSP, 0);
	lv_obj_set_style_opa(note_b, LV_OPA_TRANSP, 0);

	status_icon = note_image(habitat, 12);
	lv_obj_set_pos(status_icon, 10, bp_pick(103, 147));

	status_label = lv_label_create(habitat);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_14, 0);
	lv_obj_set_pos(status_label, 28, bp_pick(102, 146));
	lv_obj_set_width(status_label, card_w - 38);
	lv_obj_set_height(status_label, lv_font_get_line_height(&font_ui_14));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
	lv_label_set_text(status_label, "");

	// ---- the two meters ---------------------------------------------------
	lv_obj_t *stats = lv_obj_create(petpage_screen);
	lv_obj_add_style(stats, &theme_style_card, 0);
	lv_obj_set_size(stats, card_w, PET_STATS_H);
	lv_obj_set_pos(stats, pad, content_top + PET_HABITAT_H + PET_CARD_GAP);
	lv_obj_set_style_radius(stats, bp_pick(12, 18), 0);
	lv_obj_set_style_shadow_width(stats, 0, 0);
	lv_obj_set_style_pad_all(stats, 0, 0);
	lv_obj_set_scrollable(stats, false);

	lv_obj_t *heart_row = plain(stats);
	lv_obj_set_size(heart_row, card_w, 20);
	lv_obj_set_pos(heart_row, 0, bp_pick(2, 8));
	lv_obj_set_flex_flow(heart_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(heart_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(heart_row, 5, 0);

	for (int i = 0; i < 5; i++) {
		hearts[i] = lv_image_create(heart_row);
		lv_image_set_src(hearts[i], &icon_heart_outline);
		lv_image_set_scale(hearts[i], (uint32_t)(LV_SCALE_NONE * 18 / 32));
		lv_obj_set_size(hearts[i], 18, 18);
		lv_image_set_inner_align(hearts[i], LV_IMAGE_ALIGN_CENTER);
		lv_obj_set_style_image_recolor_opa(hearts[i], LV_OPA_COVER, 0);
	}

	energy_label = lv_label_create(stats);
	lv_obj_add_style(energy_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(energy_label, &font_ui_14, 0);
	lv_label_set_long_mode(energy_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(energy_label, 60);
	lv_obj_set_pos(energy_label, 12, bp_pick(25, 38));
	lv_label_set_text(energy_label, tr("pet_energy"));

	energy_bar = lv_bar_create(stats);
	lv_obj_set_size(energy_bar, card_w - 88, 8);
	lv_obj_set_pos(energy_bar, 76, bp_pick(28, 41));
	lv_bar_set_range(energy_bar, 0, PET_ENERGY_MAX);
	lv_bar_set_value(energy_bar, 0, LV_ANIM_OFF);
	lv_obj_set_style_radius(energy_bar, 4, 0);
	lv_obj_set_style_radius(energy_bar, 4, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(energy_bar, LV_OPA_COVER, 0);

	today_label = lv_label_create(stats);
	lv_obj_add_style(today_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(today_label, &font_ui_14, 0);
	lv_obj_set_width(today_label, card_w - 24);
	lv_obj_set_pos(today_label, 12, bp_pick(43, 62));
	lv_label_set_long_mode(today_label, LV_LABEL_LONG_DOT);
	lv_label_set_text(today_label, "");

	streak_label = lv_label_create(stats);
	lv_obj_add_style(streak_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(streak_label, &font_ui_14, 0);
	lv_obj_set_width(streak_label, card_w - 24);
	lv_obj_set_pos(streak_label, 12, bp_pick(59, 84));
	lv_label_set_long_mode(streak_label, LV_LABEL_LONG_DOT);
	lv_label_set_text(streak_label, "");

	// ---- the button -------------------------------------------------------
	pet_button = lv_btn_create(petpage_screen);
	lv_obj_add_style(pet_button, &theme_style_accent_bg, 0);
	lv_obj_add_style(pet_button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_size(pet_button, card_w, PET_BUTTON_H);
	lv_obj_set_pos(pet_button, pad, content_top + PET_HABITAT_H + PET_STATS_H + 2 * PET_CARD_GAP);
	lv_obj_set_style_radius(pet_button, bp_pick(8, 12), 0);
	lv_obj_set_style_shadow_width(pet_button, 0, 0);
	lv_obj_set_style_pad_all(pet_button, 0, 0);
	lv_obj_add_event_cb(pet_button, pet_click_cb, LV_EVENT_CLICKED, NULL);

	pet_button_label = lv_label_create(pet_button);
	lv_obj_set_style_text_font(pet_button_label, &font_ui_18, 0);
	lv_label_set_text(pet_button_label, tr("pet_pet"));
	lv_obj_center(pet_button_label);

	lv_obj_add_event_cb(petpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(petpage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	page_built = true;
}

void petpage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	theme_register_refresh(theme_change_cb);
	// The wardrobe is a sheet over this page, so the chevron has to close it
	// before it can close the pet.
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
