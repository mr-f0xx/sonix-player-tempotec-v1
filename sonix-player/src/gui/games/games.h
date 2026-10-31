// games.h -- the page the games in the More menu are built out of.
//
// Three of them live here: 2048, Snake and Dino. All three are played on the
// glass with the same two gestures -- a tap, and a swipe -- all three keep a
// best score in the config, and all three have to leave the panel alone the
// moment the player is switched off. The chrome they share is worth building
// once for it: a header with the way back, a score line, the field the game
// draws into, a line of instructions under it, and the card that covers the
// field between two runs.
//
// The field belongs to the game. game_frame_build() hands it over sized and
// empty, and everything inside it is the game's own business -- including its
// colours, which is why a game that owns the whole panel asks for
// game_frame_field_bg() rather than reading the theme directly.
//
// Three rules the shared code exists to enforce:
//
//   * A game never paints while the page is not on screen. The clock is a
//     timer that is paused when the screen is left and when the panel goes dark
//     (power_pause_in_standby), so nothing is drawn -- and nothing is missed --
//     in between. A long gap between two ticks is read as the screen having
//     been off: the run is held until the next tap rather than resumed on a
//     score nobody saw, and the tick that follows reports the gap rather than
//     spending it.
//   * Nothing runs under the card. While the overlay is up the clock is paused,
//     which is what makes "tap anywhere to start" safe: a touch is either the
//     card's business or the game's, never both.
//   * A game does not interrupt the music. Flappy Bird has sound effects of its
//     own and stops the player for them (see flappysound); these three have
//     none, so nothing is asked of the audio here and a track keeps playing
//     through a run.
//
// Every length comes from the panel, so one page fills both the V1's 240x320
// and the HiBys' 480x720. A game that wants a lattice of its own takes the step
// from game_frame_t::u: 2 pixels on the V1, 4 there.

#ifndef GUI_GAMES_H
#define GUI_GAMES_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"

// What the game adds to the frame. Every one is optional, and all run on the
// interface thread, out of the page's own timer and its touch handlers.
typedef struct {
	void (*on_show)(void); // the page is on screen: the field is sized and empty
	void (*on_hide)(void); // the page is being left: give everything back
	// One turn of the clock. g->tick_us says how much time it covers, which is
	// all a game needs to move: a clock on a player that is also decoding
	// music arrives late, and a game that assumes its own period drifts.
	void (*on_tick)(void);
	void (*on_tap)(const lv_point_t *p);		// a finger went down in the field
	void (*on_swipe)(lv_dir_t dir, const lv_point_t *p); // a finger crossed the field
	void (*on_frozen)(bool frozen); // the panel went dark, or came back
	// The palette moved: recolour what is yours. Called while the page is on
	// screen when the palette changes; a page that was not on screen then is
	// called the next time it is shown, and not before.
	void (*on_theme)(void);
} game_hooks_t;

typedef struct {
	// --- the game's side of the deal, filled in before game_frame_build() ---
	const game_hooks_t *hooks;
	const char *section; // the config section: [snake] best, [dino] best
	const char *title;   // the heading in the header, already translated
	void (*on_overlay_primary)(void);
	void (*on_overlay_secondary)(void);

	// --- the page, built by game_frame_build ---
	lv_obj_t *screen;
	lv_obj_t *header;
	lv_obj_t *title_label;
	lv_obj_t *back;
	lv_obj_t *stats;
	lv_obj_t *score_value;
	lv_obj_t *best_value;
	lv_obj_t *field;
	lv_obj_t *hint;
	lv_obj_t *overlay;
	lv_obj_t *overlay_title;
	lv_obj_t *overlay_body;
	lv_obj_t *overlay_primary;
	lv_obj_t *overlay_secondary;
	lv_timer_t *timer;

	// The header's own controls, kept so a game can put a glyph on a pause
	// button and take it off again. See game_frame_add_header_button().
	lv_obj_t *header_button[2];
	int header_buttons;

	int panel_w, panel_h;
	int field_top;	  // the field's own y, for a game that covers the page
	int field_w, field_h; // what is left for the game
	int u;		  // 2 on the V1, 4 on the HiBy panels

	// The clock. `wanted_period_ms` is what the game asked for; the frame
	// runs it at that rate only while the page is up, unfrozen and bare.
	int wanted_period_ms;
	int64_t last_us;
	int64_t opened_us;
	// The span the tick now running covers, in microseconds. A game integrates
	// over this instead of reading a clock of its own; see game_hooks_t.
	int64_t tick_us;
	uint32_t ticks;
	uint32_t slow_ticks;

	int score, best;
	bool best_beaten;
	bool best_unsaved;
	bool active; // the page is on screen
	bool frozen; // held by a dark panel, or by the game itself
	bool overlay_up;
	bool theme_stale; // the palette moved while the page was not on screen

	// What the glass is doing under the game's fingers: `down` while a touch
	// that the game was given is still held, `swiped` once that touch has
	// turned into a swipe. Dino's duck reads the first, Snake's pending turn
	// needs neither, and both are here rather than in the games for the same
	// reason the rest of the chrome is.
	struct {
		bool down;
		bool swiped;
		lv_point_t from;
	} touch;
} game_frame_t;

// Builds the chrome on `g->screen`, which the caller created, and wires the
// page's events. The timer exists only when the hooks have an on_tick.
void game_frame_build(game_frame_t *g, gui_config_t *cfg);

// Switches to the page. The game's on_show runs when it comes up.
void game_frame_open(game_frame_t *g);

// The score line. Either way the number moves, the best follows it in the same
// call; the best is written to the config once, when the page is left, because a
// config write rewrites the whole file and has no business in the middle of a run.
void game_frame_add_score(game_frame_t *g, int points);
void game_frame_set_score(game_frame_t *g, int points);
int game_frame_load_best(game_frame_t *g);
void game_frame_save_best(game_frame_t *g);

// The card over the field. NULL leaves a part of it out. `secondary` is the
// quiet button under the accent one, for a choice rather than an exit (Snake's
// walls-or-wrap, 2048's keep-going). The text is set as given: a page that
// wants the player's language calls tr() itself, as every other page does.
void game_frame_show_overlay(game_frame_t *g, const char *title, const char *body, const char *primary,
			     const char *secondary);
void game_frame_hide_overlay(game_frame_t *g);

// The line under the field: how to play, or what just happened.
void game_frame_set_hint(game_frame_t *g, const char *text);

// A round glyph at the right end of the header, in the style of the app's own
// corner buttons, `action` on a click. At most two per page.
lv_obj_t *game_frame_add_header_button(game_frame_t *g, const lv_image_dsc_t *icon, void (*action)(void));

// Puts a different glyph on one of them: a pause button that becomes a play
// button. `which` is the order they were added in.
void game_frame_set_header_button_icon(game_frame_t *g, int which, const lv_image_dsc_t *icon);

// The clock. A game that does not move on its own -- 2048 -- never touches
// these, and was given no timer at all. Zero stops it.
void game_frame_set_period(game_frame_t *g, int period_ms);

// Held and let go again, which is how a game pauses itself: the panel going
// dark does it on its own, and the first touch after it undoes it. `on_frozen`
// is where a game says so on screen.
void game_frame_freeze(game_frame_t *g, bool frozen);

#define GAME_STALL_US 250000

// The step of a game's own lattice: `bp_is_tempotec_v1() ? 2 : 4`, so that
// everything drawn on a multiple of it lands on whole panel pixels on either
// board.
int game_frame_unit(void);

// A score with a space between the thousands: 2048 gets to six digits and the
// V1 is 240 pixels wide.
void game_frame_format_score(int value, char *out, size_t size);

// The two colours of a field the game owns outright -- Dino's night, Snake's
// burrow. `dark` is the game's own day and night, so an inversion can be a
// mechanic and still start from what the player is wearing.
lv_color_t game_frame_field_bg(bool dark);
lv_color_t game_frame_ink(bool dark);

// True while a touch the game was given is still on the glass.
static inline bool game_frame_held(const game_frame_t *g) { return g->touch.down; }

#endif /* GUI_GAMES_H */
