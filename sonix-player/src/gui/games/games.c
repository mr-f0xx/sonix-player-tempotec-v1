// games.c -- the chrome the games in the More menu are built out of.
//
// See games.h for what the frame is for. This file is only that: the header
// with the way back, the score line, the field, the line of instructions, the
// card between two runs and the clock that stops when the page or the panel
// does. Nothing here knows what a tile, a snake or a dinosaur is.

#include "games.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

// The three bands above and below the field. They are sizes rather than a flex
// layout because a game has to know the field's shape before it can put
// anything in it, and knowing it means subtracting these.
#define HEAD_H bp_pick(36, 52)
#define STATS_H bp_pick(24, 32)
#define HINT_H bp_pick(18, 26)
#define EDGE_INSET bp_pick(1, 4)
#define BAND_GAP bp_pick(2, 5)

// A touch has to travel this far, and mostly along one axis, before a tap
// becomes a swipe: a fifth of the V1's width, so the far edge of the board is
// still reachable and a thumb that shook on the glass is not a move.
#define SWIPE_PX bp_pick(20, 36)

#define OVERLAY_BUTTON_H bp_pick(32, 46)
#define OVERLAY_PAD bp_pick(10, 20)
#define STAT_BUTTON_H bp_pick(22, 30)

#define HEADER_BUTTON_MAX 2

// ---------------------------------------------------------------------------
// the live pages
//
// The theme callback takes no argument and there is one of them for the whole
// application, so the frames are listed here. Each is a file-static in its own
// game and lives as long as the player does, which is what makes holding a
// pointer to one safe.
// ---------------------------------------------------------------------------

#define GAME_FRAME_MAX 4

static game_frame_t *frames[GAME_FRAME_MAX];
static int frame_count;
static bool theme_hook_installed;

static void repaint_theme(game_frame_t *g);

static int64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int game_frame_unit(void) { return bp_pick(2, 4); }

void game_frame_format_score(int value, char *out, size_t size) {
	// Past ten thousand, 2048's score is a fourth of the V1's width again,
	// and a board that is a fourth of the screen is a board you cannot read
	// at a glance. The separator is an ASCII space: it is in every face the
	// player can be wearing, and it is the one the languages here already
	// use for numbers.
	char digits[16];
	int len = snprintf(digits, sizeof(digits), "%d", value < 0 ? 0 : value);
	char *w = out;
	char *end = out + size - 1;
	for (int i = 0; i < len && w < end; i++) {
		int left = len - i;
		*w++ = digits[i];
		if (left > 1 && left % 3 == 1 && w < end) {
			*w++ = ' ';
		}
	}
	*w = '\0';
}

lv_color_t game_frame_field_bg(bool dark) {
	// A field the game owns outright. Day is paper; night is the blue-black
	// Tokyo Night is made of, and in the app's own dark theme that same
	// blue-black darkened, so an inversion never asks the panel to be
	// brighter than the page behind it.
	if (!dark) {
		return lv_color_make(0xf4, 0xf3, 0xef);
	}
	if (theme_is_tokyo_night()) {
		return lv_color_make(0x16, 0x16, 0x1f);
	}
	if (theme_is_dark()) {
		return lv_color_darken(theme()->screen_bg, LV_OPA_40);
	}
	return lv_color_make(0x1c, 0x1c, 0x1e);
}

lv_color_t game_frame_ink(bool dark) {
	return dark ? lv_color_make(0xe6, 0xe6, 0xe2) : lv_color_make(0x2a, 0x2a, 0x2e);
}

// ---------------------------------------------------------------------------
// the pieces
// ---------------------------------------------------------------------------

// Caption and number on one line, which is what fits in a 24-pixel band on the
// V1. The stacked form is kept for the 480-pixel panels, where the band is
// twice as tall and the caption has room to sit over its number.
static void stat_group(game_frame_t *g, const char *caption, lv_obj_t **value_out, bool grow) {
	lv_obj_t *group = lv_obj_create(g->stats);
	lv_obj_remove_style_all(group);
	lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_scrollable(group, false);
	lv_obj_set_flex_flow(group, bp_is_tempotec_v1() ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(group, grow ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(group, 4, 0);
	if (grow) {
		lv_obj_set_flex_grow(group, 1);
	}

	lv_obj_t *cap = lv_label_create(group);
	lv_label_set_text(cap, caption);
	lv_obj_set_style_text_color(cap, theme()->text_secondary, 0);
	lv_obj_set_style_text_font(cap, &font_ui_14, 0);

	lv_obj_t *value = lv_label_create(group);
	lv_label_set_text(value, "0");
	lv_obj_set_style_text_color(value, theme()->text_primary, 0);
	lv_obj_set_style_text_font(value, bp_title_font(), 0);
	if (value_out) {
		*value_out = value;
	}
}

static lv_obj_t *overlay_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, "");
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_color(label, color, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	return label;
}

static lv_obj_t *overlay_button(lv_obj_t *parent, bool accent) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_size(button, lv_pct(100), OVERLAY_BUTTON_H);
	lv_obj_set_style_radius(button, bp_tile_radius(), 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	if (accent) {
		lv_obj_add_style(button, &theme_style_accent_bg, 0);
	} else {
		lv_obj_set_style_bg_color(button, theme()->surface, 0);
		lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	}

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, "");
	lv_obj_set_style_text_color(label, accent ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_set_style_text_font(label, bp_artist_font(), 0);
	lv_obj_center(label);
	return button;
}

// A button with a glyph in it, for the header. The same construction as the
// app's own corner buttons, so a game's Undo is not a different thing to press.
static lv_obj_t *glyph_button(lv_obj_t *parent, const lv_image_dsc_t *icon) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_size(button, HEAD_H - 2 * EDGE_INSET, HEAD_H - 2 * EDGE_INSET);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);

	lv_obj_t *image = lv_image_create(button);
	lv_image_set_src(image, icon);
	lv_obj_add_style(image, &theme_style_icon, 0);
	lv_obj_center(image);
	if (bp_is_tempotec_v1()) {
		// The glyphs are drawn for a 480-pixel panel; at their own size
		// they fill the whole 32-pixel target and read larger than
		// anything else on the row.
		lv_image_set_scale(image, 192);
	}
	return button;
}

static void action_cb(lv_event_t *e) {
	void (*action)(void) = (void (*)(void))lv_event_get_user_data(e);
	if (action) {
		action();
	}
}

// The card's two buttons. They go through the frame rather than straight to the
// game, so a tap anywhere on the field does the same thing as the accent button:
// one gesture, one meaning, whichever half of the card it landed on.
static void overlay_primary_cb(lv_event_t *e) {
	game_frame_t *g = lv_event_get_user_data(e);
	if (g->on_overlay_primary) {
		g->on_overlay_primary();
	}
}

static void overlay_secondary_cb(lv_event_t *e) {
	game_frame_t *g = lv_event_get_user_data(e);
	if (g->on_overlay_secondary) {
		g->on_overlay_secondary();
	}
}

// ---------------------------------------------------------------------------
// the score line
// ---------------------------------------------------------------------------

static void paint_best(game_frame_t *g) {
	char text[24];
	game_frame_format_score(g->best, text, sizeof(text));
	lv_label_set_text(g->best_value, text);
}

static void refresh_stat_colours(game_frame_t *g) {
	if (!g->score_value || !g->best_value) {
		return;
	}
	lv_obj_set_style_text_color(g->score_value, theme()->text_primary, 0);
	// The number a run has just beaten is the one thing on the page worth
	// pointing at, so it is the one thing allowed a colour of its own.
	lv_obj_set_style_text_color(g->best_value,
				    g->best_beaten ? theme_semantic_color(THEME_SEMANTIC_GREEN) : theme()->text_primary,
				    0);
}

// The number on the score line, and the best with it. A game that scores by
// events -- an apple, a merged pair of tiles -- adds to it; a game that scores by
// how far it has got sets it. The best has to follow either of them, which is why
// this is where it lives rather than in `add_score`.
void game_frame_set_score(game_frame_t *g, int points) {
	g->score = points < 0 ? 0 : points;
	char text[24];
	game_frame_format_score(g->score, text, sizeof(text));
	lv_label_set_text(g->score_value, text);

	if (g->score > g->best) {
		if (!g->best_beaten) {
			g->best_beaten = true;
			refresh_stat_colours(g);
		}
		g->best = g->score;
		g->best_unsaved = true;
	}
	paint_best(g);
}

void game_frame_add_score(game_frame_t *g, int points) { game_frame_set_score(g, g->score + points); }

int game_frame_load_best(game_frame_t *g) {
	g->best = (int)config_get_int(g->section, "best", 0);
	paint_best(g);
	return g->best;
}

void game_frame_save_best(game_frame_t *g) {
	if (!g->best_unsaved) {
		return;
	}
	g->best_unsaved = false;
	config_set_int(g->section, "best", g->best);
	config_save();
}

// ---------------------------------------------------------------------------
// the card between two runs
// ---------------------------------------------------------------------------

static void overlay_layout(game_frame_t *g) {
	// Wide enough for a sentence, narrow enough that the field still shows
	// round it: what is behind the card is the game the player is about to
	// start again.
	lv_obj_set_width(g->overlay, g->field_w - 4 * g->u);
	lv_obj_set_height(g->overlay, LV_SIZE_CONTENT);
	lv_obj_center(g->overlay);
}

void game_frame_show_overlay(game_frame_t *g, const char *title, const char *body, const char *primary,
			     const char *secondary) {
	if (!g->overlay) {
		return;
	}
	lv_label_set_text(g->overlay_title, title ? title : "");
	lv_obj_set_hidden(g->overlay_title, !title);
	lv_label_set_text(g->overlay_body, body ? body : "");
	lv_obj_set_hidden(g->overlay_body, !body);

	lv_obj_set_hidden(g->overlay_primary, !primary || !g->on_overlay_primary);
	if (primary) {
		lv_label_set_text(lv_obj_get_child(g->overlay_primary, 0), primary);
	}
	lv_obj_set_hidden(g->overlay_secondary, !secondary || !g->on_overlay_secondary);
	if (secondary) {
		lv_label_set_text(lv_obj_get_child(g->overlay_secondary, 0), secondary);
	}

	g->overlay_up = true;
	lv_obj_set_hidden(g->overlay, false);
	lv_obj_move_foreground(g->overlay);
	overlay_layout(g);
	game_frame_set_period(g, 0); // nothing moves under the card
}

void game_frame_hide_overlay(game_frame_t *g) {
	if (!g->overlay) {
		return;
	}
	g->overlay_up = false;
	lv_obj_set_hidden(g->overlay, true);
	game_frame_set_period(g, g->wanted_period_ms);
}

void game_frame_set_hint(game_frame_t *g, const char *text) {
	if (g->hint) {
		lv_label_set_text(g->hint, text ? text : "");
	}
}

// ---------------------------------------------------------------------------
// the clock
// ---------------------------------------------------------------------------

void game_frame_set_period(game_frame_t *g, int period_ms) {
	if (!g->timer) {
		return;
	}
	g->wanted_period_ms = period_ms;
	if (period_ms <= 0 || !g->active || g->frozen || g->overlay_up) {
		lv_timer_pause(g->timer);
		return;
	}
	lv_timer_set_period(g->timer, (uint32_t)period_ms);
	lv_timer_resume(g->timer);
	// Arming the clock starts it from here. A timer stopped while the card was
	// up, or while the panel was dark, or while the game held itself, has not
	// been running for any of that time, and the tick after a resume would
	// otherwise report the whole of it -- long enough to be read as a panel that
	// went out mid-run, which is the one gap the check in tick_cb holds a run
	// for. So a run started off a card that was read for a moment would freeze
	// before it moved a pixel: the gap that counts is one that happens with the
	// clock running, and every resume says where its own clock began.
	g->last_us = now_us();
}

void game_frame_freeze(game_frame_t *g, bool frozen) {
	if (g->frozen == frozen) {
		return;
	}
	g->frozen = frozen;
	if (!frozen) {
		// Picking the page up again must not be read as a gap in the clock:
		// the tick after an unfreeze would otherwise report the whole time the
		// panel spent dark, decide it is another stall, and freeze the run
		// again before the player has seen a frame of it.
		g->last_us = now_us();
	}
	if (g->hooks && g->hooks->on_frozen) {
		g->hooks->on_frozen(frozen);
	}
	game_frame_set_period(g, g->wanted_period_ms);
}

static void tick_cb(lv_timer_t *timer) {
	game_frame_t *g = lv_timer_get_user_data(timer);
	if (!g->active || g->overlay_up) {
		lv_timer_pause(timer);
		return;
	}

	int64_t now = now_us();
	int64_t elapsed = now - g->last_us;
	g->last_us = now;

	// A gap this long is the panel having been dark rather than the player
	// having been busy: a run that carried on in the dark ends on a score
	// nobody saw, so the page waits for a tap instead.
	if (elapsed > GAME_STALL_US) {
		game_frame_freeze(g, true);
		return;
	}

	if (!g->hooks || !g->hooks->on_tick) {
		return;
	}
	g->tick_us = elapsed; // what this tick covers, for the game's own clock
	int64_t started = now_us();
	g->hooks->on_tick();
	g->ticks++;
	// Worth one line in the log, because the question anybody asks of a game
	// on this hardware is whether it kept up.
	if (elapsed > 0 && now_us() - started > elapsed) {
		g->slow_ticks++;
	}
}


// ---------------------------------------------------------------------------
// touch
// ---------------------------------------------------------------------------

// The direction a drag has taken, once it is far enough along one axis to mean
// something. False while it is still a tap waiting to happen.
static bool swipe_dir(const lv_point_t *from, const lv_point_t *to, lv_dir_t *out) {
	int dx = to->x - from->x;
	int dy = to->y - from->y;
	int ax = dx < 0 ? -dx : dx;
	int ay = dy < 0 ? -dy : dy;
	if (ax < SWIPE_PX && ay < SWIPE_PX) {
		return false;
	}
	// One axis has to lead the other clearly, or a thumb that came down and
	// drifted diagonally picks a direction nobody meant.
	if (ax * 3 >= ay * 4) {
		*out = dx < 0 ? LV_DIR_LEFT : LV_DIR_RIGHT;
		return true;
	}
	if (ay * 3 >= ax * 4) {
		*out = dy < 0 ? LV_DIR_TOP : LV_DIR_BOTTOM;
		return true;
	}
	return false;
}

static void press_cb(lv_event_t *e) {
	game_frame_t *g = lv_event_get_user_data(e);
	if (!g->active) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);

	// The panel going dark leaves the run held. The touch that picks it up
	// again is for that alone: it must not also jump, turn or undo.
	if (g->frozen) {
		game_frame_freeze(g, false);
		return;
	}
	if (g->overlay_up) {
		if (g->on_overlay_primary) {
			g->on_overlay_primary();
		}
		return;
	}

	g->touch.down = true;
	g->touch.swiped = false;
	g->touch.from = p;
	if (g->hooks && g->hooks->on_tap) {
		g->hooks->on_tap(&p);
	}
}

static void drag_cb(lv_event_t *e) {
	game_frame_t *g = lv_event_get_user_data(e);
	if (!g->active || !g->touch.down || g->touch.swiped) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	lv_dir_t dir;
	if (!swipe_dir(&g->touch.from, &p, &dir)) {
		return;
	}
	// The swipe replaces whatever the press that began it already asked for:
	// a turn the finger then drew over the glass is not a second order.
	g->touch.swiped = true;
	if (g->hooks && g->hooks->on_swipe) {
		g->hooks->on_swipe(dir, &p);
	}
}

static void release_cb(lv_event_t *e) {
	game_frame_t *g = lv_event_get_user_data(e);
	g->touch.down = false;
	g->touch.swiped = false;
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void back_cb(lv_event_t *e) {
	(void)e;
	back_btn_cb(NULL); // the frame's own chevron: the app's is hidden here
}

lv_obj_t *game_frame_add_header_button(game_frame_t *g, const lv_image_dsc_t *icon, void (*action)(void)) {
	if (g->header_buttons >= HEADER_BUTTON_MAX) {
		return NULL;
	}
	lv_obj_t *button = glyph_button(g->header, icon);
	lv_obj_add_event_cb(button, action_cb, LV_EVENT_CLICKED, (void *)action);
	g->header_button[g->header_buttons++] = button;
	return button;
}

// Puts a different glyph on one of the header's own buttons. The pause button of
// a game is the only thing that needs it, and it needs it twice a run.
void game_frame_set_header_button_icon(game_frame_t *g, int which, const lv_image_dsc_t *icon) {
	if (which < 0 || which >= g->header_buttons) {
		return;
	}
	lv_obj_t *image = lv_obj_get_child(g->header_button[which], 0);
	if (image) {
		lv_image_set_src(image, icon);
	}
}


static void loaded_cb(lv_event_t *e) {
	game_frame_t *g = lv_event_get_user_data(e);

	// The panel is the game's: no status bar along the top of it and no
	// floating chevron in its corner, because the page has a header of its
	// own and a tap in the top left corner belongs to the game.
	topbar_set_hidden(true);
	back_btn_force_hidden(true);
	volume_overlay_allow_outside_player(true);

	g->active = true;
	g->frozen = false;
	g->best_beaten = false;
	g->touch.down = false;
	g->touch.swiped = false;
	g->ticks = g->slow_ticks = 0;
	g->opened_us = g->last_us = now_us();

	// A palette that moved while this page was not on screen is painted now,
	// before the game's own show runs, so the first frame shows the palette in
	// force rather than the one the page was left in.
	if (g->theme_stale) {
		repaint_theme(g);
	}

	if (g->hooks && g->hooks->on_show) {
		g->hooks->on_show();
	}
	// Whatever the game left the clock on runs from here; a page that was
	// never given a period has no timer to leave it on.
	game_frame_set_period(g, g->wanted_period_ms);
}

static void unloaded_cb(lv_event_t *e) {
	game_frame_t *g = lv_event_get_user_data(e);
	g->active = false;
	g->touch.down = false;
	if (g->timer) {
		lv_timer_pause(g->timer);
	}

	if (g->ticks > 0) {
		printf("%s: %u ticks in %.1f s, %u slower than the time they covered\n", g->section, g->ticks,
		       (double)(now_us() - g->opened_us) / 1e6, g->slow_ticks);
	}
	// The game's part first: a game that keeps the run on disk writes the
	// config file from here, and the best is then written after it only when
	// the run actually beat one.
	if (g->hooks && g->hooks->on_hide) {
		g->hooks->on_hide();
	}
	game_frame_save_best(g);

	volume_overlay_allow_outside_player(false);
	back_btn_force_hidden(false);
	topbar_set_hidden(false);
}

static void refresh_chrome(game_frame_t *g) {
	lv_obj_set_style_bg_color(g->screen, theme()->screen_bg, 0);
	lv_obj_set_style_text_color(g->title_label, theme()->text_primary, 0);
	lv_obj_set_style_text_color(g->hint, theme()->text_secondary, 0);
	if (g->overlay) {
		lv_obj_set_style_bg_color(g->overlay, theme()->surface, 0);
		lv_obj_set_style_text_color(g->overlay_title, theme()->text_primary, 0);
		lv_obj_set_style_text_color(g->overlay_body, theme()->text_secondary, 0);
	}
	refresh_stat_colours(g);
}

// Repaints one page in the palette now in force. The same work whether the page
// is on screen at the time of the change or is only being shown again.
static void repaint_theme(game_frame_t *g) {
	refresh_chrome(g);
	if (g->hooks && g->hooks->on_theme) {
		g->hooks->on_theme();
	}
	g->theme_stale = false;
}

// Called by the theme after every palette change, for every registered page.
// Only the page on screen is repainted here, because that is the only one the
// player can see change. Every other page is marked and repainted when it is
// next shown (see loaded_cb). Walking all of them on each change, as this used
// to, spent the change's time on boards nobody was looking at.
static void games_theme_refresh(void) {
	for (int i = 0; i < frame_count; i++) {
		game_frame_t *g = frames[i];
		if (g->active) {
			repaint_theme(g);
		} else {
			g->theme_stale = true;
		}
	}
}

static void build_header(game_frame_t *g, gui_config_t *cfg) {
	g->header = lv_obj_create(g->screen);
	lv_obj_remove_style_all(g->header);
	lv_obj_set_pos(g->header, cfg->padding, EDGE_INSET);
	lv_obj_set_size(g->header, g->panel_w - 2 * cfg->padding, HEAD_H);
	lv_obj_set_flex_flow(g->header, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(g->header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(g->header, 2, 0);
	lv_obj_set_scrollable(g->header, false);

	g->back = lv_btn_create(g->header);
	lv_obj_set_size(g->back, HEAD_H - 2 * EDGE_INSET, HEAD_H - 2 * EDGE_INSET);
	lv_obj_set_style_bg_opa(g->back, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(g->back, 0, 0);
	lv_obj_set_style_shadow_width(g->back, 0, 0);
	lv_obj_set_style_pad_all(g->back, 0, 0);
	lv_obj_set_ext_click_area(g->back, 4);

	lv_obj_t *chevron = lv_image_create(g->back);
	lv_image_set_src(chevron, &icon_chevron_left);
	lv_obj_add_style(chevron, &theme_style_icon, 0);
	lv_obj_center(chevron);
	lv_obj_add_event_cb(g->back, back_cb, LV_EVENT_CLICKED, NULL);

	g->title_label = lv_label_create(g->header);
	lv_label_set_text(g->title_label, g->title ? g->title : "");
	lv_label_set_long_mode(g->title_label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(g->title_label, 1);
	lv_obj_set_style_text_align(g->title_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(g->title_label, theme()->text_primary, 0);
	lv_obj_set_style_text_font(g->title_label, bp_title_font(), 0);
}

static void build_stats(game_frame_t *g, gui_config_t *cfg) {
	g->stats = lv_obj_create(g->screen);
	lv_obj_remove_style_all(g->stats);
	lv_obj_set_pos(g->stats, cfg->padding, EDGE_INSET + HEAD_H + BAND_GAP);
	lv_obj_set_size(g->stats, g->panel_w - 2 * cfg->padding, STATS_H);
	lv_obj_set_flex_flow(g->stats, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(g->stats, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(g->stats, bp_pick(5, 12), 0);
	lv_obj_set_scrollable(g->stats, false);

	stat_group(g, tr("score"), &g->score_value, true);
	stat_group(g, tr("best"), &g->best_value, false);
}

static void build_field(game_frame_t *g, gui_config_t *cfg) {
	g->field_top = EDGE_INSET + HEAD_H + BAND_GAP + STATS_H + BAND_GAP;
	g->field_h = (int)cfg->screen_height - g->field_top - HINT_H - BAND_GAP - EDGE_INSET;
	g->field_w = (int)cfg->screen_width - 2 * cfg->padding;

	g->field = lv_obj_create(g->screen);
	lv_obj_remove_style_all(g->field);
	lv_obj_set_pos(g->field, cfg->padding, g->field_top);
	lv_obj_set_size(g->field, g->field_w, g->field_h);
	lv_obj_set_style_bg_opa(g->field, LV_OPA_TRANSP, 0);
	lv_obj_set_scrollable(g->field, false);
	lv_obj_set_scroll_dir(g->field, LV_DIR_NONE);
	lv_obj_set_clickable(g->field, true);

	// The touches are read here rather than on the screen: the header, the
	// score line and the card are all clickable and keep their own presses,
	// so what reaches the field is a touch on the play area and nothing
	// else. The player sheet and the swipe-back are not attached either, so
	// a sideways drag belongs to the game alone.
	lv_obj_add_event_cb(g->field, press_cb, LV_EVENT_PRESSED, g);
	lv_obj_add_event_cb(g->field, drag_cb, LV_EVENT_PRESSING, g);
	lv_obj_add_event_cb(g->field, release_cb, LV_EVENT_RELEASED, g);
	lv_obj_add_event_cb(g->field, release_cb, LV_EVENT_PRESS_LOST, g);

	g->hint = lv_label_create(g->screen);
	lv_label_set_long_mode(g->hint, LV_LABEL_LONG_DOT);
	lv_obj_set_pos(g->hint, cfg->padding, (int)cfg->screen_height - HINT_H - EDGE_INSET);
	lv_obj_set_size(g->hint, g->panel_w - 2 * cfg->padding, HINT_H);
	lv_obj_set_style_text_color(g->hint, theme()->text_secondary, 0);
	lv_obj_set_style_text_font(g->hint, &font_ui_14, 0);
	lv_obj_set_style_text_align(g->hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_scrollable(g->hint, false);
}

void game_frame_build(game_frame_t *g, gui_config_t *cfg) {
	if (frame_count < GAME_FRAME_MAX) {
		frames[frame_count++] = g;
	}
	if (!theme_hook_installed) {
		theme_hook_installed = true;
		theme_register_refresh(games_theme_refresh);
	}

	g->panel_w = (int)cfg->screen_width;
	g->panel_h = (int)cfg->screen_height;
	g->u = game_frame_unit();
	g->wanted_period_ms = 16;

	lv_obj_set_style_bg_color(g->screen, theme()->screen_bg, 0);
	lv_obj_set_style_bg_opa(g->screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(g->screen, false);

	build_header(g, cfg);
	build_stats(g, cfg);
	build_field(g, cfg);

	g->overlay = lv_obj_create(g->field);
	lv_obj_remove_style_all(g->overlay);
	lv_obj_add_style(g->overlay, &theme_style_card, 0);
	lv_obj_set_style_bg_opa(g->overlay, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(g->overlay, 0, 0);
	lv_obj_set_style_shadow_width(g->overlay, 0, 0);
	lv_obj_set_style_radius(g->overlay, bp_tile_radius(), 0);
	lv_obj_set_style_pad_all(g->overlay, OVERLAY_PAD, 0);
	lv_obj_set_style_pad_row(g->overlay, bp_pick(6, 10), 0);
	lv_obj_set_scrollable(g->overlay, false);
	lv_obj_set_flex_flow(g->overlay, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(g->overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	g->overlay_title = overlay_label(g->overlay, bp_title_font(), theme()->text_primary);
	g->overlay_body = overlay_label(g->overlay, bp_queue_label_font(), theme()->text_secondary);
	g->overlay_primary = overlay_button(g->overlay, true);
	g->overlay_secondary = overlay_button(g->overlay, false);
	lv_obj_add_event_cb(g->overlay_primary, overlay_primary_cb, LV_EVENT_CLICKED, g);
	lv_obj_add_event_cb(g->overlay_secondary, overlay_secondary_cb, LV_EVENT_CLICKED, g);
	lv_obj_set_hidden(g->overlay, true);
	overlay_layout(g);

	lv_obj_add_event_cb(g->screen, loaded_cb, LV_EVENT_SCREEN_LOADED, g);
	lv_obj_add_event_cb(g->screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, g);

	if (g->hooks && g->hooks->on_tick) {
		g->timer = lv_timer_create(tick_cb, (uint32_t)g->wanted_period_ms, g);
		lv_timer_pause(g->timer);
		// Nothing is presented while the panel is dark, so a timer whose
		// only job is to paint has no business running.
		power_pause_in_standby(g->timer);
	}

	game_frame_load_best(g);
	game_frame_set_score(g, 0);
	refresh_chrome(g);
}

void game_frame_open(game_frame_t *g) {
	switch_screen(g->screen);
}
