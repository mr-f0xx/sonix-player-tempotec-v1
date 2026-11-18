#ifndef THEME_H
#define THEME_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// The built-in appearances. Dark and Light keep their existing palettes;
// Tokyo Night, Amber on Black and OLED add three dark palettes designed for
// the V1. OLED is true black with white as its accent.
typedef enum {
	THEME_KIND_DARK = 0,
	THEME_KIND_LIGHT = 1,
	THEME_KIND_TOKYO_NIGHT = 2,
	THEME_KIND_AMBER_ON_BLACK = 3,
	THEME_KIND_OLED = 4,
	THEME_KIND_CUSTOM = 5,
} theme_kind_t;

#define THEME_KIND_COUNT 5 // built-in themes; Custom is a separate SD-card choice

// Fixed semantic accents used by Tokyo Night's function-matched glyphs.
// Amber on Black and OLED map every icon role to their accent (signal amber,
// white); Dark and Light continue to use their existing single accent.
typedef enum {
	THEME_SEMANTIC_BLUE,
	THEME_SEMANTIC_CYAN,
	THEME_SEMANTIC_PURPLE,
	THEME_SEMANTIC_GREEN,
	THEME_SEMANTIC_AMBER,
	THEME_SEMANTIC_RED,
	THEME_SEMANTIC_MUTED,
} theme_semantic_t;

// Role-specific colors from the reference Now Playing screen. The Amber theme
// uses the exact cream, amber and dark-orange steps, OLED the matching steps
// of grey; other themes map these to their ordinary text/accent colors.
typedef enum {
	THEME_PLAYER_CHROME_PRIMARY,
	THEME_PLAYER_CHROME_SECONDARY,
	THEME_PLAYER_CHROME_MUTED,
	THEME_PLAYER_CHROME_PLAYBACK,
	THEME_PLAYER_CHROME_PROGRESS_REST,
	THEME_PLAYER_CHROME_CHEVRON,
	THEME_PLAYER_CHROME_SHUFFLE_IDLE,
	THEME_PLAYER_CHROME_OVERFLOW,
} theme_player_chrome_t;

// The handful of colours the whole UI is built from. Everything on screen
// pulls from the active one; Tokyo Night, Amber on Black and OLED remain dark
// palettes for the existing dark/light-specific layout decisions.
typedef struct {
	lv_color_t screen_bg;	   // page background
	lv_color_t surface;		   // list rows, cards
	lv_color_t surface_pressed; // the same, while a finger is on it
	lv_color_t panel;		   // status bar and the player's control block
	lv_color_t cover_bg;	   // behind the artwork, when a track has none
	lv_color_t text_primary;
	lv_color_t text_secondary;
	lv_color_t accent;		   // the selected theme's active colour
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

// The knob of a switch that is on. It sits on the accent-coloured track, so it
// takes theme_on_accent(): the white it has always been, and black on a white
// accent (OLED), where a white knob would vanish into the track.
// theme_apply_switch() adds it with LV_PART_KNOB | LV_STATE_CHECKED.
extern lv_style_t theme_style_switch_knob_checked;

// Two states of the track that only OLED draws differently; for every other
// theme both styles are empty. While the switch is on, the ring OLED draws
// around an off track takes the accent, so the track is one solid pill. While
// it is disabled, the whole switch is faded rather than washed with the
// default theme's light grey, which on a black track would be a grey fill.
extern lv_style_t theme_style_switch_track_checked;
extern lv_style_t theme_style_switch_disabled;

// Gives an lv_switch all of the switch styles, each on its part and state.
// Call it once, when the switch is built.
void theme_apply_switch(lv_obj_t *sw);

// A plain accent-coloured fill. Anything that has to follow the accent colour
// without the page repainting it adds this instead of setting the colour by
// hand: the sliders' filled part, the pill buttons. A hand-set colour is a
// snapshot -- it keeps whichever accent was live when the page was built, and
// does not follow a later change. Its text colour is theme_on_accent(), which
// a label inside the filled widget inherits.
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

// A single editable palette at <SD root>/Themes/custom.ini. Set the root
// before theme_init(); reload on entering Appearance or when selecting Custom.
// A missing/invalid file never changes the active palette. Custom saved in
// config falls back to Dark at startup if the card is unavailable.
void theme_set_card_root(const char *root);
bool theme_reload_custom(void);

// Palette resolved without any of the styles, for pages that run before the
// interface exists -- the charging screen, which the board reaches before
// gui_init(). The config is the only input, so a Custom palette saved with no
// card behind it falls back to Dark, exactly what happens today at startup;
// the full theme_init() runs later and applies the Custom palette to the
// pages then. Idempotent: theme_init() runs the same selection itself.
void theme_resolve_boot_palette(void);

// Builds the shared styles. Call once, before any screen is created.
void theme_init(void);

// Call with the page about to be put on screen. A theme, accent or tint change
// walks only the page the user is looking at; this is what walks a page that
// was hidden when the palette moved, and does nothing when it was not.
void theme_notify_screen_shown(lv_obj_t *screen);

const theme_palette_t *theme(void);
// The subtle accent-tinted surface shared by the dark themes' mini-player
// and Music browse tiles. OLED's is plain black, like the rest of its cards.
lv_color_t theme_now_playing_fill(void);
bool theme_is_dark(void);
bool theme_is_tokyo_night(void);
bool theme_is_amber_on_black(void);
bool theme_is_oled(void);
// The single-hue palettes, Amber on Black and OLED: every glyph and status
// mark wears the accent, and nothing borrows a second hue (no Bluetooth blue,
// codec colours or balanced-output gold). The roles still come from the
// active palette, so each keeps its own colours.
bool theme_is_monochrome(void);
// Tokyo Night, Amber on Black and OLED use the same function-matched icon and
// compact V1 layouts. Their palettes and semantic hues remain distinct.
bool theme_uses_function_glyphs(void);
theme_kind_t theme_get_kind(void);

// Tokyo Night's blue, cyan, violet, green, amber, red and muted roles. Amber
// on Black and OLED map every icon role to their accent; in Dark/Light they
// resolve to the chosen accent (or secondary text for muted).
lv_color_t theme_semantic_color(theme_semantic_t role);
lv_color_t theme_player_chrome_color(theme_player_chrome_t role);

// What text and marks drawn on an accent-coloured fill wear: white, as they
// always have, unless the accent is so light that white would not read on it
// -- OLED's white accent, where they turn black. Read at paint time, like
// every colour that follows the accent.
lv_color_t theme_on_accent(void);
// The same rule for any other fill: white, or black on a fill that light.
lv_color_t theme_ink_on(lv_color_t fill);
// Whether a colour is that light: white type would not read on it.
bool theme_color_is_light(lv_color_t c);

// What the file browser recolours its folder glyphs to, and nothing else on
// the page. Tokyo Night answers with the violet of its own palette; every
// other theme with the shared icon colour it has always used, so the page is
// unchanged outside Tokyo Night.
//
// Read at paint time: a hand-set colour is a snapshot, so the page asks again
// whenever the palette moves.
lv_color_t theme_browser_folder_color(void);

// Switches between Dark and Light, and repaints the UI. The added palettes
// are treated as dark by the legacy toggle, so one toggle from any goes to Light.
void theme_toggle(void);

// The accent colour, chosen from a fixed set of presets (Appearance > Accent
// colour): 0 blue (default), 1 red, 2 orange, 3 yellow, 4 lime, 5 green,
// 6 cyan, 7 indigo, 8 purple, 9 pink, 10 brown, 11 slate. Dark and Light
// retain the same presets; Tokyo Night defaults to its own blue (#7aa2f7),
// Amber on Black to amber (#ffad2e), OLED to white (#ffffff), and all three
// honor any explicitly selected non-default accent.
// Persists in the config.
#define THEME_ACCENT_COUNT 12
lv_color_t theme_accent_preset(int index);
int theme_get_accent(void);
void theme_set_accent(int index);
bool theme_has_custom_accent(void);
lv_color_t theme_get_custom_accent(void);
void theme_set_custom_accent(lv_color_t color);

// Picks the legacy Dark or Light preset rather than flipping. Asking for Dark
// while any added palette is selected switches back to the original Dark
// palette rather than treating the dark palette as Dark.
void theme_set_dark(bool dark);

// Picks a specific built-in theme, saves it, and repaints the visible UI.
void theme_set_kind(theme_kind_t kind);

// The picture the next power-on shows (Appearance > Boot screen):
// BOOTLOGO_STOCK, the firmware's own light/dark pair, one of the three added
// pictures -- BOOTLOGO_RETROSPACE, BOOTLOGO_SPACE, BOOTLOGO_TRAVELLING -- or
// BOOTLOGO_CUSTOM, a picture of the user's own from the Bootlogo folder on the
// card (see bootpic.h). The choice is kept in the config like every other
// setting, and pushed to the flash marker the boot script reads -- see
// bootlogo.h for why it cannot be a file. Retrospace is the default.
//
// Custom is only an answer while there is a custom picture to draw: asked for
// it with none behind it, theme_boot_screen() answers Retrospace, which is
// what the boot script would draw anyway.
int theme_boot_screen(void);
void theme_set_boot_screen(int choice);

// Which file in the Bootlogo folder the custom picture was last made from, or
// "" when none has been. The name only, no path: it is a caption for the row
// on the Appearance page and a tick in the picker's list, and the card it
// came from may not be the card that is in.
const char *theme_boot_screen_custom(void);
void theme_set_boot_screen_custom(const char *name);

// The dynamic tint (Appearance > Dynamic tint): the neutral colours --
// backgrounds, cards, the status bar -- borrow the accent's hue, the way
// Material You colours a system from one seed. Each colour keeps its brightest
// channel, so a tinted surface stays at the level the preset put it at instead
// of drifting towards the accent's own; text and the monochrome icons are left
// alone entirely. Off by default, persists in the config.
bool theme_dynamic_tint(void);
void theme_set_dynamic_tint(bool on);

#endif // THEME_H
