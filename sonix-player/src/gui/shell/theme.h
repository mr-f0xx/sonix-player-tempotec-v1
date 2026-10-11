#ifndef THEME_H
#define THEME_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// The three built-in appearances. Dark and Light keep their existing palettes;
// Tokyo Night is an additional blue-black palette designed for the V1 panel.
typedef enum {
	THEME_KIND_DARK = 0,
	THEME_KIND_LIGHT = 1,
	THEME_KIND_TOKYO_NIGHT = 2,
} theme_kind_t;

// Fixed semantic accents used by Tokyo Night's function-matched glyphs. The
// ordinary Dark and Light themes continue to use their existing single accent.
typedef enum {
	THEME_SEMANTIC_BLUE,
	THEME_SEMANTIC_CYAN,
	THEME_SEMANTIC_PURPLE,
	THEME_SEMANTIC_GREEN,
	THEME_SEMANTIC_AMBER,
	THEME_SEMANTIC_RED,
	THEME_SEMANTIC_MUTED,
} theme_semantic_t;

// The handful of colours the whole UI is built from. Everything on screen
// pulls from the active one; Tokyo Night remains a dark palette for the
// existing dark/light-specific layout decisions elsewhere in the player.
typedef struct {
	lv_color_t screen_bg;	   // page background
	lv_color_t surface;		   // list rows, cards
	lv_color_t surface_pressed; // the same, while a finger is on it
	lv_color_t panel;		   // status bar and the player's control block
	lv_color_t cover_bg;	   // behind the artwork, when a track has none
	lv_color_t text_primary;
	lv_color_t text_secondary;
	lv_color_t accent;		   // the blue used for active things
	bool dark;
} theme_palette_t;

// Shared styles. Widgets add these instead of setting colours themselves,
// which is what makes switching themes a one-liner: update the style, LVGL
// repaints everything using it.
extern lv_style_t theme_style_screen;
extern lv_style_t theme_style_panel;
extern lv_style_t theme_style_card;
extern lv_style_t theme_style_card_pressed;
extern lv_style_t theme_style_text;
extern lv_style_t theme_style_text_dim;
extern lv_style_t theme_style_icon;
extern lv_style_t theme_style_switch;
extern lv_style_t theme_style_switch_checked; // image_recolor for monochrome icons

// A plain accent-coloured fill. Anything that has to follow the accent colour
// without the page repainting it adds this instead of setting the colour by
// hand: the sliders' filled part, the pill buttons. A hand-set colour is a
// snapshot -- it keeps whichever accent was live when the page was built, and
// does not follow a later change.
extern lv_style_t theme_style_accent_bg;

// The unfilled part of a slider's track: the secondary text colour at low
// opacity, which reads as a quiet groove on both palettes.
extern lv_style_t theme_style_slider_track;

// Anything that can't be expressed as a style -- an icon recoloured at runtime,
// the blurred artwork behind the controls -- registers a callback here and gets
// called after every switch.
typedef void (*theme_refresh_cb_t)(void);
void theme_register_refresh(theme_refresh_cb_t cb);

// The ring that tells a slider's knob from what is behind it. In the dark
// theme a hairline of black is plenty; in the light one the cards are nearly
// white too, so a white knob needs a real grey outline not to vanish into
// them. Call it when the slider is built and from the page's theme-refresh
// callback -- the colour depends on the theme in force.
void theme_apply_slider_knob(lv_obj_t *slider);

// Builds the shared styles. Call once, before any screen is created.
void theme_init(void);

// Call with the page about to be put on screen. A theme, accent or tint change
// walks only the page the user is looking at; this is what walks a page that
// was hidden when the palette moved, and does nothing when it was not.
void theme_notify_screen_shown(lv_obj_t *screen);

const theme_palette_t *theme(void);
bool theme_is_dark(void);
bool theme_is_tokyo_night(void);
theme_kind_t theme_get_kind(void);

// Tokyo Night's blue, cyan, violet, green, amber and red roles. In Dark/Light
// these resolve to the chosen accent (or the secondary text colour for muted).
lv_color_t theme_semantic_color(theme_semantic_t role);

// Switches between Dark and Light, and repaints the UI. Tokyo Night is treated
// as dark by the legacy toggle, so one toggle from it goes to Light.
void theme_toggle(void);

// The accent colour, chosen from a fixed set of presets (Appearance > Accent
// colour): 0 blue (default), 1 red, 2 orange, 3 yellow, 4 lime, 5 green,
// 6 cyan, 7 indigo, 8 purple, 9 pink, 10 brown, 11 slate. Dark and Light
// retain the same presets; Tokyo Night uses its own blue (#7aa2f7) for the
// default and honors any explicitly selected non-default accent.
// Persists in the config.
#define THEME_ACCENT_COUNT 12
lv_color_t theme_accent_preset(int index);
int theme_get_accent(void);
void theme_set_accent(int index);

// Picks the legacy Dark or Light preset rather than flipping. In particular,
// asking for Dark while Tokyo Night is selected switches back to the original
// Dark palette rather than treating Tokyo Night as Dark.
void theme_set_dark(bool dark);

// Picks a specific built-in theme, saves it, and repaints the visible UI.
void theme_set_kind(theme_kind_t kind);

// The picture the next power-on shows (Appearance > Boot screen):
// BOOTLOGO_STOCK, the firmware's own light/dark pair, or one of the three
// added pictures -- BOOTLOGO_RETROSPACE, BOOTLOGO_SPACE, BOOTLOGO_TRAVELLING
// -- one image each for both palettes. The choice is kept in the config like
// every other setting, and pushed to the flash marker the boot script reads
// -- see bootlogo.h for why it cannot be a file. Retrospace is the default.
int theme_boot_screen(void);
void theme_set_boot_screen(int choice);

// The dynamic tint (Appearance > Dynamic tint): the neutral colours --
// backgrounds, cards, the status bar -- borrow the accent's hue, the way
// Material You colours a system from one seed. Each colour keeps its brightest
// channel, so a tinted surface stays at the level the preset put it at instead
// of drifting towards the accent's own; text and the monochrome icons are left
// alone entirely. Off by default, persists in the config.
bool theme_dynamic_tint(void);
void theme_set_dynamic_tint(bool on);

#endif // THEME_H
