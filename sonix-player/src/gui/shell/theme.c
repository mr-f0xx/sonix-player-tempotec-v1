#include "theme.h"

#include "src/system/core/config.h"
#include "src/system/device/bootlogo.h"
#include "src/system/device/bootpic.h"

#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

lv_style_t theme_style_screen;
lv_style_t theme_style_panel;
lv_style_t theme_style_card;
lv_style_t theme_style_card_pressed;
lv_style_t theme_style_text;
lv_style_t theme_style_text_dim;
lv_style_t theme_style_icon;
lv_style_t theme_style_switch;		   // switch track, off state
lv_style_t theme_style_switch_checked; // switch indicator, on state
lv_style_t theme_style_switch_knob_checked; // switch knob, on state
lv_style_t theme_style_switch_track_checked; // switch track, on state (OLED's ring)
lv_style_t theme_style_switch_disabled;      // switch, disabled (OLED only)
lv_style_t theme_style_accent_bg;	   // anything that must follow the accent
lv_style_t theme_style_slider_track;   // a slider's unfilled part

// GNOME Adwaita (libadwaita) colours, dark variant: window #242424, cards
// #303030 (white at 8%% over the window), accent blue #3584e4. Secondary text
// is white at 55%%, flattened against the card colour.
static theme_palette_t palette_dark = {
	.screen_bg = LV_COLOR_MAKE(36, 36, 36),
	.surface = LV_COLOR_MAKE(48, 48, 48),
	.surface_pressed = LV_COLOR_MAKE(64, 64, 64),
	.panel = LV_COLOR_MAKE(30, 30, 30),
	.cover_bg = LV_COLOR_MAKE(30, 30, 30),
	.text_primary = LV_COLOR_MAKE(255, 255, 255),
	.text_secondary = LV_COLOR_MAKE(160, 160, 160),
	.accent = LV_COLOR_MAKE(53, 132, 228),
	.dark = true,
};

// Adwaita light: window #fafafa, cards white, pressed = black at 8%%, text
// black at 80%%. The accent is the same #3584e4 in both variants.
static theme_palette_t palette_light = {
	// The window is pushed further from white than stock Adwaita's #fafafa:
	// on this panel white cards on a #fafafa window are indistinguishable, so
	// the window is a light grey and the pressed state a step deeper.
	.screen_bg = LV_COLOR_MAKE(236, 237, 240),
	.surface = LV_COLOR_MAKE(255, 255, 255),
	.surface_pressed = LV_COLOR_MAKE(221, 223, 228),
	.panel = LV_COLOR_MAKE(255, 255, 255),
	.cover_bg = LV_COLOR_MAKE(224, 226, 230),
	.text_primary = LV_COLOR_MAKE(50, 50, 50),
	.text_secondary = LV_COLOR_MAKE(115, 115, 115),
	.accent = LV_COLOR_MAKE(53, 132, 228),
	.dark = false,
};

// Tokyo Night, matching the approved V1 mockup. There are no extra font or
// bitmap packs: the menu glyphs reuse the existing generated icons.
static theme_palette_t palette_tokyo = {
	.screen_bg = LV_COLOR_MAKE(26, 27, 38),       // #1a1b26
	.surface = LV_COLOR_MAKE(36, 40, 59),         // #24283b
	.surface_pressed = LV_COLOR_MAKE(45, 53, 84), // #2d3554
	.panel = LV_COLOR_MAKE(22, 22, 30),           // #16161e
	.cover_bg = LV_COLOR_MAKE(22, 22, 30),
	.text_primary = LV_COLOR_MAKE(192, 202, 245), // #c0caf5
	.text_secondary = LV_COLOR_MAKE(169, 177, 214), // #a9b1d6
	.accent = LV_COLOR_MAKE(122, 162, 247),       // #7aa2f7
	.dark = true,
};

// Amber on Black follows the same V1 screen layouts as Tokyo Night, with the
// distinct cream, golden-amber and dark-orange roles sampled from the
// reference Now Playing screen.
static theme_palette_t palette_amber = {
	.screen_bg = LV_COLOR_MAKE(10, 9, 7),          // #0a0907
	.surface = LV_COLOR_MAKE(25, 19, 10),          // #19130a
	.surface_pressed = LV_COLOR_MAKE(37, 25, 7),   // #251907
	.panel = LV_COLOR_MAKE(5, 5, 5),              // #050505
	.cover_bg = LV_COLOR_MAKE(5, 5, 5),
	.text_primary = LV_COLOR_MAKE(255, 242, 207),  // #fff2cf
	.text_secondary = LV_COLOR_MAKE(243, 186, 92), // #f3ba5c
	.accent = LV_COLOR_MAKE(255, 173, 46),         // #ffad2e
	.dark = true,
};

// OLED: true black, white type and white as its accent, for panels that switch
// a black pixel off. Nothing is filled grey: the page, the status bar, the
// player's deck, the cards and the idle pills and buttons are all the same
// black, and the layout is carried by the type and the glyphs alone. Every
// grey the theme still draws -- the second line, the quiet controls, a track
// -- is a multiple of eight, which survives the panel's RGB565 exactly: a step
// in between lands on different levels in the green channel and the red/blue
// ones, and a neutral grey comes out faintly green or magenta.
static theme_palette_t palette_oled = {
	.screen_bg = LV_COLOR_MAKE(0, 0, 0),            // #000000
	.surface = LV_COLOR_MAKE(0, 0, 0),              // #000000
	.surface_pressed = LV_COLOR_MAKE(0, 0, 0),      // #000000
	.panel = LV_COLOR_MAKE(0, 0, 0),                // #000000
	.cover_bg = LV_COLOR_MAKE(0, 0, 0),
	.text_primary = LV_COLOR_MAKE(255, 255, 255),   // #ffffff
	.text_secondary = LV_COLOR_MAKE(160, 160, 160), // #a0a0a0
	.accent = LV_COLOR_MAKE(255, 255, 255),         // #ffffff
	.dark = true,
};

// A fixed filename keeps the card interface simple and avoids user-supplied
// paths. Parsing is transactional: a partial or malformed edit is never shown.
static char custom_root[512];
static theme_palette_t palette_custom;
static lv_color_t custom_accent;

void theme_set_card_root(const char *root) {
	if (!root || strlen(root) >= sizeof(custom_root)) {
		custom_root[0] = '\0';
	} else {
		snprintf(custom_root, sizeof(custom_root), "%s", root);
	}
}

static char *trim(char *s) {
	while (isspace((unsigned char)*s)) s++;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
	return s;
}

static bool hex_color(const char *s, lv_color_t *out) {
	if (strlen(s) != 7 || s[0] != '#') return false;
	unsigned value = 0;
	for (int i = 1; i < 7; i++) {
		char c = s[i];
		unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
			 c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
			 c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16;
		if (digit == 16) return false;
		value = (value << 4) | digit;
	}
	*out = lv_color_make((value >> 16) & 255, (value >> 8) & 255, value & 255);
	return true;
}

static bool load_custom(theme_palette_t *result) {
	char path[600];
	if (!custom_root[0] || snprintf(path, sizeof(path), "%s/Themes/custom.ini", custom_root) >= (int)sizeof(path))
		return false;
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 4096) return false;
	FILE *f = fopen(path, "r");
	if (!f) return false;
	static const char *keys[] = {"screen_bg", "surface", "surface_pressed", "panel", "cover_bg",
		"text_primary", "text_secondary", "accent"};
	lv_color_t colors[8];
	unsigned seen = 0;
	bool section = false, valid = true;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		if (!strchr(line, '\n') && !feof(f)) { valid = false; break; }
		char *key = trim(line);
		if (!*key || *key == ';' || *key == '#') continue;
		if (strcmp(key, "[theme]") == 0 && !section) { section = true; continue; }
		char *eq = strchr(key, '=');
		if (!section || !eq) { valid = false; break; }
		*eq++ = '\0';
		key = trim(key);
		int i = 0;
		for (; i < 8 && strcmp(key, keys[i]) != 0; i++);
		if (i == 8 || (seen & (1u << i)) || !hex_color(trim(eq), &colors[i])) { valid = false; break; }
		seen |= 1u << i;
	}
	if (ferror(f)) valid = false;
	fclose(f);
	if (!valid || seen != 0xff) return false;
	*result = (theme_palette_t){.screen_bg = colors[0], .surface = colors[1],
		.surface_pressed = colors[2], .panel = colors[3], .cover_bg = colors[4],
		.text_primary = colors[5], .text_secondary = colors[6], .accent = colors[7], .dark = true};
	return true;
}

// With no fill to darken, OLED still has to show a press: a row or a button
// under a finger flashes this grey, the only grey that is ever a fill. A quiet
// control that would vanish on the black -- an off switch's track, a dialog's
// Cancel, an idle quick-panel circle -- is drawn as a ring instead.
#define OLED_PRESSED_LEVEL 48 // #303030
#define OLED_RING_LEVEL 96    // #606060
#define OLED_RING_WIDTH 2

// Whether white type stops reading on a fill. Every accent the presets had
// before OLED stays below the line -- the lightest of them, Amber's #ffad2e,
// weighs about 183 -- and OLED's white is far above it.
#define ON_ACCENT_DARK_INK_LUMA 200

bool theme_color_is_light(lv_color_t c) {
	return (299u * c.red + 587u * c.green + 114u * c.blue) / 1000u > ON_ACCENT_DARK_INK_LUMA;
}

// The selected preset is kept apart from `active` because dynamic tint points
// `active` at a derived copy rather than at any of the five presets.
static theme_kind_t selected_kind = THEME_KIND_DARK;

static theme_palette_t *selected_palette(void) {
	switch (selected_kind) {
	case THEME_KIND_LIGHT:
		return &palette_light;
	case THEME_KIND_TOKYO_NIGHT:
		return &palette_tokyo;
	case THEME_KIND_AMBER_ON_BLACK:
		return &palette_amber;
	case THEME_KIND_OLED:
		return &palette_oled;
	case THEME_KIND_CUSTOM:
		return &palette_custom;
	case THEME_KIND_DARK:
	default:
		return &palette_dark;
	}
}

// The dynamic tint: the neutral colours -- backgrounds, cards, the status bar
// -- borrow the accent's hue, the way Material You colours a system from one
// seed. Off by default, remembered in the config.
static bool dynamic_tint;

// The tinted copy of whichever preset is chosen. Rebuilt on every theme,
// accent or tint change; `active` points here while the tint is on.
static theme_palette_t tinted;

static theme_palette_t *active = &palette_dark;

// The accent presets, Adwaita's named accents plus four more to fill the
// gaps between them -- lime between the yellow and the green, indigo between
// the blue and the purple -- and the two neutrals, brown and slate, at the
// end. Blue (the default) comes first, then warm to cool: red, orange,
// yellow, lime, green, cyan, indigo, purple, pink.
static const lv_color_t ACCENT_COLORS[THEME_ACCENT_COUNT] = {
	LV_COLOR_MAKE(53, 132, 228),  // blue (#3584e4, the default)
	LV_COLOR_MAKE(224, 27, 36),   // red (#e01b24)
	LV_COLOR_MAKE(255, 120, 0),   // orange (#ff7800)
	LV_COLOR_MAKE(229, 165, 10),  // yellow (#e5a50a)
	LV_COLOR_MAKE(153, 197, 0),   // lime (#99c500)
	LV_COLOR_MAKE(46, 194, 126),  // green (#2ec27e)
	LV_COLOR_MAKE(0, 176, 214),   // cyan (#00b0d6)
	LV_COLOR_MAKE(102, 91, 224),  // indigo (#665be0)
	LV_COLOR_MAKE(145, 65, 172),  // purple (#9141ac)
	LV_COLOR_MAKE(213, 97, 153),  // pink (#d56199)
	LV_COLOR_MAKE(168, 101, 48),  // brown (#a86530)
	LV_COLOR_MAKE(139, 142, 148), // slate (#8b8e94)
};
static int accent_index;
static lv_color_t custom_accent_color = LV_COLOR_MAKE(53, 132, 228);

static lv_color_t default_accent(theme_kind_t kind) {
	switch (kind) {
	case THEME_KIND_TOKYO_NIGHT:
		return lv_color_make(122, 162, 247); // Tokyo Night blue
	case THEME_KIND_AMBER_ON_BLACK:
		return lv_color_make(255, 173, 46); // Amber on Black signal
	case THEME_KIND_OLED:
		return lv_color_make(255, 255, 255); // OLED white
	case THEME_KIND_CUSTOM:
		return custom_accent;
	case THEME_KIND_DARK:
	case THEME_KIND_LIGHT:
	default:
		return ACCENT_COLORS[0];
	}
}

// Where the six presets of the old palette sit in this one: a config written
// by an older firmware names an old slot, so the first boot on this build
// moves the choice to the matching new slot and leaves the marker that the
// init reads below, so the remap runs once rather than on every boot.
static const int LEGACY_ACCENT_MAP[6] = {0, 2, 3, 5, 8, 9};

lv_color_t theme_accent_preset(int index) {
	if (index < 0 || index >= THEME_ACCENT_COUNT) {
		index = 0;
	}
	// The added palettes have their own defaults. An explicitly saved
	// non-default accent still carries across themes like Dark and Light.
	return index == 0 ? default_accent(selected_kind) : ACCENT_COLORS[index];
}

int theme_get_accent(void) { return accent_index; }

// Callers asking to be called back after a theme or accent change. The list
// grows on demand and has no fixed limit: a dropped registration shows up only
// as a page that keeps the colours it was built with, which is invisible until
// somebody changes the accent. A failed allocation is logged for the same
// reason.
static theme_refresh_cb_t *refresh_cbs;
static int refresh_cb_count;
static int refresh_cb_capacity;

// How much of the accent's own saturation a neutral borrows. Dark surfaces
// take more before the hue stops reading as a shade and starts reading as a
// colour; the light ones are nearly white, where the same amount would turn
// the page pastel.
#define TINT_STRENGTH_DARK 0.30f
#define TINT_STRENGTH_LIGHT 0.15f

// Gives a neutral the accent's hue while keeping exactly how light it is: the
// brightest channel of the result is the brightest channel of the input. The
// colour is never pulled towards the accent's own brightness, which is what
// would turn a dark background light under a pale accent; the two channels
// below the brightest do come down, so a tinted surface is fractionally darker
// than the plain one, and that is what the two strengths above are set from.
static lv_color_t tint_with(lv_color_t c, lv_color_t accent, float strength) {
	uint8_t amax = accent.red > accent.green ? accent.red : accent.green;
	if (accent.blue > amax) {
		amax = accent.blue;
	}
	uint8_t amin = accent.red < accent.green ? accent.red : accent.green;
	if (accent.blue < amin) {
		amin = accent.blue;
	}
	uint8_t cmax = c.red > c.green ? c.red : c.green;
	if (c.blue > cmax) {
		cmax = c.blue;
	}
	if (amax == amin || cmax == 0) {
		return c; // a grey accent has no hue to lend, and black takes none
	}

	// The accent's shape: 1 on its brightest channel, 0 on its darkest. Applied
	// to the neutral's own brightness it reproduces that hue at the neutral's
	// level, at a fraction of the accent's saturation.
	float span = (float)(amax - amin);
	float sat = span / (float)amax * strength;
	float shape[3] = {
		((float)accent.red - (float)amin) / span,
		((float)accent.green - (float)amin) / span,
		((float)accent.blue - (float)amin) / span,
	};

	uint8_t out[3];
	for (int i = 0; i < 3; i++) {
		out[i] = (uint8_t)((float)cmax * (1.0f - sat * (1.0f - shape[i])) + 0.5f);
	}
	return lv_color_make(out[0], out[1], out[2]);
}

// Points `active` at the chosen preset, or at a tinted copy of it.
static void rebuild_active(void) {
	theme_palette_t *base = selected_palette();

	if (!dynamic_tint) {
		active = base;
		return;
	}

	float strength = base->dark ? TINT_STRENGTH_DARK : TINT_STRENGTH_LIGHT;
	tinted = *base;
	tinted.screen_bg = tint_with(base->screen_bg, base->accent, strength);
	tinted.surface = tint_with(base->surface, base->accent, strength);
	tinted.surface_pressed = tint_with(base->surface_pressed, base->accent, strength);
	tinted.panel = tint_with(base->panel, base->accent, strength);
	tinted.cover_bg = tint_with(base->cover_bg, base->accent, strength);
	// Text keeps the plain colours: the monochrome icons are recoloured with
	// text_primary, and they are meant to come through the tint untouched.
	active = &tinted;
}

// Pushes the active palette into the shared styles.
static void apply_palette(void) {
	lv_style_set_bg_color(&theme_style_screen, active->screen_bg);

	lv_style_set_bg_color(&theme_style_panel, active->panel);

	lv_style_set_bg_color(&theme_style_card, active->surface);
	lv_style_set_text_color(&theme_style_card, active->text_primary);

	bool oled = selected_kind == THEME_KIND_OLED;
	lv_color_t pressed = oled ? lv_color_make(OLED_PRESSED_LEVEL, OLED_PRESSED_LEVEL, OLED_PRESSED_LEVEL) : active->surface_pressed;
	lv_style_set_bg_color(&theme_style_card_pressed, pressed);

	lv_style_set_text_color(&theme_style_text, active->text_primary);
	lv_style_set_text_color(&theme_style_text_dim, active->text_secondary);

	lv_style_set_image_recolor(&theme_style_icon, active->text_primary);

	lv_style_set_bg_color(&theme_style_switch, active->surface_pressed);
	// The ring only exists on OLED. Elsewhere the property is left out of the
	// style altogether, so the controls that carry it look exactly as before.
	if (oled) {
		lv_style_set_border_width(&theme_style_switch, OLED_RING_WIDTH);
		lv_style_set_border_color(&theme_style_switch, lv_color_make(OLED_RING_LEVEL, OLED_RING_LEVEL, OLED_RING_LEVEL));
		lv_style_set_border_opa(&theme_style_switch, LV_OPA_COVER);
	} else {
		lv_style_remove_prop(&theme_style_switch, LV_STYLE_BORDER_WIDTH);
		lv_style_remove_prop(&theme_style_switch, LV_STYLE_BORDER_COLOR);
		lv_style_remove_prop(&theme_style_switch, LV_STYLE_BORDER_OPA);
	}
	// A switch that is on: the ring takes the accent and closes up with the
	// track. A disabled one: the default theme washes it 50% towards a light
	// grey, so OLED turns the wash off and fades the whole switch instead.
	if (oled) {
		// The track under the indicator goes white too, so the two rounded
		// edges have nothing dark to blend into where they meet.
		lv_style_set_bg_color(&theme_style_switch_track_checked, active->accent);
		lv_style_set_border_color(&theme_style_switch_track_checked, active->accent);
		lv_style_set_recolor_opa(&theme_style_switch_disabled, LV_OPA_TRANSP);
		lv_style_set_opa(&theme_style_switch_disabled, LV_OPA_40);
	} else {
		lv_style_remove_prop(&theme_style_switch_track_checked, LV_STYLE_BG_COLOR);
		lv_style_remove_prop(&theme_style_switch_track_checked, LV_STYLE_BORDER_COLOR);
		lv_style_remove_prop(&theme_style_switch_disabled, LV_STYLE_RECOLOR_OPA);
		lv_style_remove_prop(&theme_style_switch_disabled, LV_STYLE_OPA);
	}
	lv_style_set_bg_color(&theme_style_switch_checked, active->accent);
	lv_style_set_bg_color(&theme_style_switch_knob_checked, theme_on_accent());

	lv_style_set_bg_color(&theme_style_accent_bg, active->accent);
	lv_style_set_text_color(&theme_style_accent_bg, theme_on_accent());
	lv_style_set_bg_color(&theme_style_slider_track, active->text_secondary);
}

// Which picture the next power-on shows (Appearance > Boot screen): the
// stock light/dark pair, Retrospace, Space or Travelling in space, or the
// custom picture from the card. Retrospace is the default, and it is also what
// a device with no marker at all draws, because the stock script's fallback
// file is the one that carries it -- see bootlogo.h.
//
// Custom is answered with Retrospace while the copy is not there. The boot
// script would draw the fallback in its place anyway -- a factory reset of
// /usr/data takes the copy with it, and a card that has been through a
// computer takes the file it was made from -- and a page that showed Custom
// as chosen while the device boots something else would be lying about what
// the next power-on does.
int theme_boot_screen(void) {
	int choice = (int)config_get_int("ui", "boot_screen", BOOTLOGO_RETROSPACE);
	if (choice < BOOTLOGO_STOCK || choice > BOOTLOGO_CUSTOM) {
		choice = BOOTLOGO_RETROSPACE; // a config written by a newer firmware
	}
	if (choice == BOOTLOGO_CUSTOM && !bootpic_present()) {
		choice = BOOTLOGO_RETROSPACE;
	}
	return choice;
}

// Remembers the choice and moves the flash marker now rather than at the next
// theme switch: the setting is only worth anything from the next boot on.
//
// Custom with no copy behind it is refused rather than written, for the reason
// above. The picker makes the copy before it asks for the choice, so this is
// only ever the answer to something else having taken the copy away.
void theme_set_boot_screen(int choice) {
	if (choice < BOOTLOGO_STOCK || choice > BOOTLOGO_CUSTOM) {
		return;
	}
	if (choice == BOOTLOGO_CUSTOM && !bootpic_present()) {
		return;
	}

	config_set_int("ui", "boot_screen", choice);
	config_save();

	bootlogo_set(choice, active->dark);
}

// How bright the splash is while the boot picture is on the panel, as a
// percent of the backlight's range. 60 is the stock picture's own level, so
// that is what a player without this setting keeps. Like the boot picture it
// is read by the boot script, before any config file can be mounted.
int theme_boot_brightness(void) {
	int light = (int)config_get_int("ui", "boot_brightness", BOOTLOGO_LIGHT_DEFAULT);
	if (light < BOOTLOGO_LIGHT_MIN || light > BOOTLOGO_LIGHT_MAX) {
		return BOOTLOGO_LIGHT_DEFAULT;
	}
	return BOOTLOGO_LIGHT_MIN + ((light - BOOTLOGO_LIGHT_MIN) / BOOTLOGO_LIGHT_STEP) * BOOTLOGO_LIGHT_STEP;
}

void theme_set_boot_brightness(int light) {
	if (light < BOOTLOGO_LIGHT_MIN || light > BOOTLOGO_LIGHT_MAX) {
		return;
	}
	light = BOOTLOGO_LIGHT_MIN + ((light - BOOTLOGO_LIGHT_MIN) / BOOTLOGO_LIGHT_STEP) * BOOTLOGO_LIGHT_STEP;
	config_set_int("ui", "boot_brightness", light);
	config_save();

	// Picture and brightness share one marker in the flash, so saving either
	// writes the whole marker again.
	bootlogo_set(theme_boot_screen(), active->dark);
}

// Which file in the Bootlogo folder the custom picture was made from. The name
// and nothing else: the card it came from may not be the one in the slot, and
// the copy on the partition is what boots either way.
const char *theme_boot_screen_custom(void) {
	return config_get("ui", "boot_screen_custom", "");
}

void theme_set_boot_screen_custom(const char *name) {
	config_set("ui", "boot_screen_custom", name ? name : "");
	config_save();
}

// The palette the config describes, and nothing else: kind, accent, dynamic
// tint, resolved into `active`. Shared by theme_init() and the charging
// screen's pre-GUI call (theme_resolve_boot_palette), so the two cannot
// drift apart. Idempotent, and free of every other side effect -- no styles,
// no flash marker -- which is why the boot path may run it before the
// interface exists.
static void select_active(void) {
	// Read the named theme choice when present; configs from older builds have
	// only `dark_theme`, so they keep their original Dark/Light setting.
	const char *saved_theme = config_get("ui", "theme", NULL);
	if (saved_theme && strcmp(saved_theme, "light") == 0) {
		selected_kind = THEME_KIND_LIGHT;
	} else if (saved_theme && strcmp(saved_theme, "tokyo-night") == 0) {
		selected_kind = THEME_KIND_TOKYO_NIGHT;
	} else if (saved_theme && strcmp(saved_theme, "amber-on-black") == 0) {
		selected_kind = THEME_KIND_AMBER_ON_BLACK;
	} else if (saved_theme && strcmp(saved_theme, "oled") == 0) {
		selected_kind = THEME_KIND_OLED;
	} else if (saved_theme && strcmp(saved_theme, "custom") == 0) {
		selected_kind = load_custom(&palette_custom) ? THEME_KIND_CUSTOM : THEME_KIND_DARK;
		if (selected_kind == THEME_KIND_CUSTOM) custom_accent = palette_custom.accent;
	} else if (saved_theme && strcmp(saved_theme, "dark") == 0) {
		selected_kind = THEME_KIND_DARK;
	} else {
		selected_kind = config_get_bool("ui", "dark_theme", true) ? THEME_KIND_DARK : THEME_KIND_LIGHT;
	}

	// The accent slot is shared across themes; slot zero resolves to each new
	// palette's native accent while the other slots retain their shared colors.
	// Slot -1 means a user-picked custom colour in [ui] accent_custom.
	accent_index = (int)config_get_int("ui", "accent", 0);
	if (accent_index == -1) {
		int rgb = (int)config_get_int("ui", "accent_custom", 0x3584e4);
		custom_accent_color = lv_color_make((uint8_t)((rgb >> 16) & 0xFF),
											(uint8_t)((rgb >> 8) & 0xFF),
											(uint8_t)(rgb & 0xFF));
	} else if (accent_index < 0 || accent_index >= THEME_ACCENT_COUNT) {
		accent_index = 0;
	}
	if (config_get_int("ui", "accent_scheme", 0) == 0) {
		// No marker: the value came from the six-preset palette. Move the
		// choice to its place among the twelve and write the marker, so a
		// boot after this one reads the index as it is.
		if (accent_index >= 0 && accent_index < 6) {
			accent_index = LEGACY_ACCENT_MAP[accent_index];
		}
		config_set_int("ui", "accent", accent_index);
		config_set_int("ui", "accent_scheme", 1);
		config_save();
	}
	if (accent_index == -1) {
		palette_dark.accent = custom_accent_color;
		palette_light.accent = custom_accent_color;
		palette_tokyo.accent = custom_accent_color;
		palette_amber.accent = custom_accent_color;
		palette_oled.accent = custom_accent_color;
		palette_custom.accent = custom_accent_color;
	} else {
		palette_dark.accent = ACCENT_COLORS[accent_index];
		palette_light.accent = ACCENT_COLORS[accent_index];
		palette_tokyo.accent = accent_index == 0 ? default_accent(THEME_KIND_TOKYO_NIGHT) : ACCENT_COLORS[accent_index];
		palette_amber.accent = accent_index == 0 ? default_accent(THEME_KIND_AMBER_ON_BLACK) : ACCENT_COLORS[accent_index];
		palette_oled.accent = accent_index == 0 ? default_accent(THEME_KIND_OLED) : ACCENT_COLORS[accent_index];
		palette_custom.accent = accent_index == 0 ? custom_accent : ACCENT_COLORS[accent_index];
	}

	dynamic_tint = config_get_bool("ui", "dynamic_tint", false);

	rebuild_active();
}

void theme_resolve_boot_palette(void) { select_active(); }

void theme_init(void) {
	lv_style_init(&theme_style_screen);
	lv_style_set_bg_opa(&theme_style_screen, LV_OPA_COVER);

	lv_style_init(&theme_style_panel);
	lv_style_set_bg_opa(&theme_style_panel, LV_OPA_COVER);

	lv_style_init(&theme_style_card);
	lv_style_set_bg_opa(&theme_style_card, LV_OPA_COVER);

	lv_style_init(&theme_style_card_pressed);
	lv_style_set_bg_opa(&theme_style_card_pressed, LV_OPA_COVER);
	// The default theme grows a pressed button by a few pixels; on a list of
	// cards that reads as the row jumping out of the list, so pin it flat.
	lv_style_set_transform_width(&theme_style_card_pressed, 0);
	lv_style_set_transform_height(&theme_style_card_pressed, 0);

	lv_style_init(&theme_style_text);
	lv_style_init(&theme_style_text_dim);

	lv_style_init(&theme_style_icon);
	lv_style_set_image_recolor_opa(&theme_style_icon, LV_OPA_COVER);

	lv_style_init(&theme_style_switch);
	lv_style_set_bg_opa(&theme_style_switch, LV_OPA_COVER);
	lv_style_init(&theme_style_switch_checked);
	lv_style_set_bg_opa(&theme_style_switch_checked, LV_OPA_COVER);
	lv_style_init(&theme_style_switch_knob_checked);
	lv_style_set_bg_opa(&theme_style_switch_knob_checked, LV_OPA_COVER);
	lv_style_init(&theme_style_switch_track_checked);
	lv_style_init(&theme_style_switch_disabled);

	lv_style_init(&theme_style_accent_bg);
	lv_style_set_bg_opa(&theme_style_accent_bg, LV_OPA_COVER);
	lv_style_init(&theme_style_slider_track);
	lv_style_set_bg_opa(&theme_style_slider_track, LV_OPA_40);
	lv_style_set_radius(&theme_style_slider_track, LV_RADIUS_CIRCLE);

	theme_resolve_boot_palette();

	// The marker the boot script reads has to agree with the setting too, not
	// only with the theme. The added palettes use the dark variant of that
	// legacy light/dark boot-logo switch.
	//
	// Asked for at startup, this is also what puts right a marker whose
	// picture has gone: a Custom whose copy is no longer on the partition
	// reads back as Retrospace, and the marker follows, so the boot script
	// stops waiting for a file that is not there.
	bootlogo_set(theme_boot_screen(), selected_palette()->dark);

	apply_palette();
}

void theme_apply_slider_knob(lv_obj_t *slider) {
	if (!slider) {
		return;
	}
	bool dark = theme()->dark;
	// The white knob sits at the end of the accent fill. On a light accent
	// (OLED's white) the hairline is not enough to tell the two apart, so the
	// knob gets a solid black ring, which on a black page reads as a gap.
	bool solid = dark && theme_color_is_light(theme()->accent);
	lv_obj_set_style_border_color(slider, dark ? lv_color_black() : lv_color_make(120, 120, 120), LV_PART_KNOB);
	lv_obj_set_style_border_opa(slider, dark && !solid ? LV_OPA_10 : LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_border_width(slider, dark && !solid ? 1 : 2, LV_PART_KNOB);
}

void theme_apply_switch(lv_obj_t *sw) {
	lv_obj_add_style(sw, &theme_style_switch, LV_PART_MAIN);
	lv_obj_add_style(sw, &theme_style_switch_track_checked, LV_PART_MAIN | LV_STATE_CHECKED);
	lv_obj_add_style(sw, &theme_style_switch_disabled, LV_PART_MAIN | LV_STATE_DISABLED);
	lv_obj_add_style(sw, &theme_style_switch_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_add_style(sw, &theme_style_switch_knob_checked, LV_PART_KNOB | LV_STATE_CHECKED);
}

void theme_register_refresh(theme_refresh_cb_t cb) {
	if (!cb) {
		return;
	}

	if (refresh_cb_count == refresh_cb_capacity) {
		int wanted = refresh_cb_capacity ? refresh_cb_capacity * 2 : 16;
		theme_refresh_cb_t *grown = realloc(refresh_cbs, (size_t)wanted * sizeof(*grown));
		if (!grown) {
			// A device that cannot find a few dozen bytes has bigger problems,
			// but a registration must never be lost unnoticed.
			fprintf(stderr, "theme: out of memory for refresh callback number %d\n",
					refresh_cb_count + 1);
			return;
		}
		refresh_cbs = grown;
		refresh_cb_capacity = wanted;
	}

	refresh_cbs[refresh_cb_count++] = cb;
}

const theme_palette_t *theme(void) { return active; }

lv_color_t theme_now_playing_fill(void) {
	// OLED keeps even this one black: a tenth of its white accent would be
	// exactly the grey card it leaves out.
	if (selected_kind == THEME_KIND_OLED) {
		return active->surface;
	}
	return lv_color_mix(active->accent, active->surface, LV_OPA_10);
}

bool theme_is_dark(void) { return active->dark; }

bool theme_is_tokyo_night(void) { return selected_kind == THEME_KIND_TOKYO_NIGHT; }

bool theme_is_amber_on_black(void) { return selected_kind == THEME_KIND_AMBER_ON_BLACK; }

bool theme_is_oled(void) { return selected_kind == THEME_KIND_OLED; }

bool theme_is_monochrome(void) {
	return selected_kind == THEME_KIND_AMBER_ON_BLACK || selected_kind == THEME_KIND_OLED;
}

bool theme_uses_function_glyphs(void) {
	return selected_kind == THEME_KIND_TOKYO_NIGHT || selected_kind == THEME_KIND_AMBER_ON_BLACK ||
		   selected_kind == THEME_KIND_OLED;
}

lv_color_t theme_ink_on(lv_color_t fill) { return theme_color_is_light(fill) ? lv_color_black() : lv_color_white(); }

lv_color_t theme_on_accent(void) { return theme_ink_on(active->accent); }

theme_kind_t theme_get_kind(void) { return selected_kind; }

lv_color_t theme_semantic_color(theme_semantic_t role) {
	if (role == THEME_SEMANTIC_MUTED) {
		// The monochrome palettes' icons all share the same signal colour;
		// reserve their secondary tone for labels and surfaces, not
		// destination glyphs.
		return theme_is_monochrome() ? active->accent : active->text_secondary;
	}
	// Amber on Black and OLED are intentionally monochrome: blue, cyan,
	// purple, green, amber and red roles all resolve to the chosen accent.
	if (selected_kind != THEME_KIND_TOKYO_NIGHT) {
		return active->accent;
	}

	switch (role) {
	case THEME_SEMANTIC_BLUE:
		return active->accent;
	case THEME_SEMANTIC_CYAN:
		return lv_color_make(125, 207, 255); // #7dcfff
	case THEME_SEMANTIC_PURPLE:
		return lv_color_make(187, 154, 247); // #bb9af7
	case THEME_SEMANTIC_GREEN:
		return lv_color_make(158, 206, 106); // #9ece6a
	case THEME_SEMANTIC_AMBER:
		return lv_color_make(224, 175, 104); // #e0af68
	case THEME_SEMANTIC_RED:
		return lv_color_make(247, 118, 142); // #f7768e
	case THEME_SEMANTIC_MUTED:
	default:
		return active->text_secondary;
	}
}

// The file browser's folders are the one thing in the interface that does not
// wear a shared colour: Tokyo Night gives them its violet -- the hue its Now
// Playing deck already uses -- and nowhere else is it spent. Every other
// palette answers with the colour it has always used here, so outside Tokyo
// Night the page is unchanged.
//
// The names on the rows are not part of this: they read in the ordinary text
// colour, the same one the library lists write theirs in.
lv_color_t theme_browser_folder_color(void) {
	if (selected_kind == THEME_KIND_TOKYO_NIGHT) {
		return theme_semantic_color(THEME_SEMANTIC_PURPLE); // #bb9af7, the palette's violet
	}
	// Unchanged from before: the signal green a folder wears on the other
	// function-glyph palettes (their accent), and the plain text colour
	// everywhere else.
	return theme_uses_function_glyphs() ? theme_semantic_color(THEME_SEMANTIC_GREEN) : active->text_primary;
}

lv_color_t theme_player_chrome_color(theme_player_chrome_t role) {
	if (selected_kind == THEME_KIND_AMBER_ON_BLACK) {
		switch (role) {
		case THEME_PLAYER_CHROME_PRIMARY:
			return lv_color_make(255, 242, 207); // #fff2cf
		case THEME_PLAYER_CHROME_SECONDARY:
			return lv_color_make(243, 186, 92); // #f3ba5c
		case THEME_PLAYER_CHROME_MUTED:
			return lv_color_make(185, 116, 36); // #b97424
		case THEME_PLAYER_CHROME_PLAYBACK:
			return lv_color_make(156, 90, 13); // #9c5a0d
		case THEME_PLAYER_CHROME_PROGRESS_REST:
			return lv_color_make(104, 64, 18); // #684012
		case THEME_PLAYER_CHROME_CHEVRON:
			return lv_color_make(208, 139, 53); // #d08b35
		case THEME_PLAYER_CHROME_SHUFFLE_IDLE:
			return lv_color_make(114, 70, 19); // #724613
		case THEME_PLAYER_CHROME_OVERFLOW:
			return lv_color_make(189, 118, 38); // #bd7626
		}
	}

	// OLED takes the same roles in steps of grey: white for the type that
	// matters, the palette's grey for the second line, and the quiet controls
	// dimmed further. The playback glyph and the progress bar wear the accent,
	// which is white unless another one has been picked.
	if (selected_kind == THEME_KIND_OLED) {
		switch (role) {
		case THEME_PLAYER_CHROME_PRIMARY:
		case THEME_PLAYER_CHROME_CHEVRON:
			return active->text_primary;
		case THEME_PLAYER_CHROME_SECONDARY:
			return active->text_secondary;
		case THEME_PLAYER_CHROME_MUTED:
		case THEME_PLAYER_CHROME_OVERFLOW:
			return lv_color_make(136, 136, 136); // #888888
		case THEME_PLAYER_CHROME_PLAYBACK:
			return active->accent;
		case THEME_PLAYER_CHROME_PROGRESS_REST:
			return lv_color_make(48, 48, 48); // #303030
		case THEME_PLAYER_CHROME_SHUFFLE_IDLE:
			return lv_color_make(96, 96, 96); // #606060
		}
	}

	// Tokyo Night's Now Playing deck is drawn from three of its own hues:
	// the track name, the format line and the playback glyph wear the violet,
	// while the idle shuffle and the overflow button take the comment grey --
	// quiet controls, the way the palette uses #565f89 elsewhere. The artist
	// name, favourite star and skip glyphs ask for the cyan semantic role
	// themselves.
	if (selected_kind == THEME_KIND_TOKYO_NIGHT) {
		switch (role) {
		case THEME_PLAYER_CHROME_PRIMARY:
		case THEME_PLAYER_CHROME_SECONDARY:
		case THEME_PLAYER_CHROME_PLAYBACK:
			return lv_color_make(187, 154, 247); // #bb9af7
		case THEME_PLAYER_CHROME_SHUFFLE_IDLE:
		case THEME_PLAYER_CHROME_OVERFLOW:
			return lv_color_make(86, 95, 137); // #565f89
		default:
			break;
		}
	}

	switch (role) {
	case THEME_PLAYER_CHROME_PRIMARY:
	case THEME_PLAYER_CHROME_CHEVRON:
		return active->text_primary;
	case THEME_PLAYER_CHROME_SECONDARY:
	case THEME_PLAYER_CHROME_MUTED:
	case THEME_PLAYER_CHROME_PROGRESS_REST:
	case THEME_PLAYER_CHROME_SHUFFLE_IDLE:
	case THEME_PLAYER_CHROME_OVERFLOW:
		return active->text_secondary;
	case THEME_PLAYER_CHROME_PLAYBACK:
	default:
		return active->accent;
	}
}

void theme_set_dark(bool dark) { theme_set_kind(dark ? THEME_KIND_DARK : THEME_KIND_LIGHT); }

// Which palette the pages on screen were last walked for.
//
// lv_obj_report_style_change(NULL) walks every object of every screen and
// refreshes each one against its whole subtree. The pages here are built at
// startup and stay built, so that is the entire interface -- thousands of
// objects -- for a change that moves nothing but colours, and it takes far
// longer than a frame: the finger is still on the switch when it starts.
//
// A colour is read out of the style at draw time, so the page in front of the
// user is the only one that has to be walked at the moment of the change. The
// pages behind it are walked the first time they are shown again, which is
// where a redraw was going to happen anyway.
static unsigned palette_generation = 1;

static struct screen_generation {
	lv_obj_t *screen;
	unsigned generation;
} *screen_gens;
static int screen_gen_count;
static int screen_gen_capacity;

// True when `screen` still carries an older palette than the one in force, and
// records it as walked for this one. A screen this has never seen is taken as
// current: it was built from the styles as they stand.
//
// An allocation that fails costs a walk per page change rather than a page
// with the wrong colours, which is why the failure is silent here.
static bool screen_is_stale(lv_obj_t *screen) {
	for (int i = 0; i < screen_gen_count; i++) {
		if (screen_gens[i].screen != screen) {
			continue;
		}
		if (screen_gens[i].generation == palette_generation) {
			return false;
		}
		screen_gens[i].generation = palette_generation;
		return true;
	}

	if (screen_gen_count == screen_gen_capacity) {
		int wanted = screen_gen_capacity ? screen_gen_capacity * 2 : 32;
		struct screen_generation *grown = realloc(screen_gens, (size_t)wanted * sizeof(*grown));
		if (!grown) {
			return true;
		}
		screen_gens = grown;
		screen_gen_capacity = wanted;
	}

	screen_gens[screen_gen_count].screen = screen;
	screen_gens[screen_gen_count].generation = palette_generation;
	screen_gen_count++;
	return false;
}

// One tree, refreshed against the styles it carries. LV_STYLE_PROP_ANY takes
// the object's own properties and recurses into its children.
static void refresh_tree(lv_obj_t *obj) {
	if (obj) {
		lv_obj_refresh_style(obj, LV_PART_ANY, LV_STYLE_PROP_ANY);
	}
}

void theme_notify_screen_shown(lv_obj_t *screen) {
	if (screen && screen_is_stale(screen)) {
		refresh_tree(screen);
	}
}

// Repaints after a palette change: the page on screen and the three layers the
// status bar, the player sheet and the popups live on, then the callbacks,
// which catch what cannot be expressed as a shared style -- an icon recoloured
// at runtime, the blurred artwork behind the controls. Those run for every
// page, on screen or not, and cost about as much as one frame in total.
static void refresh_all(void) {
	palette_generation++;

	lv_obj_t *screen = lv_screen_active();
	screen_is_stale(screen);
	refresh_tree(screen);
	refresh_tree(lv_layer_top());
	refresh_tree(lv_layer_sys());
	refresh_tree(lv_layer_bottom());

	for (int i = 0; i < refresh_cb_count; i++) {
		refresh_cbs[i]();
	}
}

static const char *theme_config_value(theme_kind_t kind) {
	switch (kind) {
	case THEME_KIND_LIGHT:
		return "light";
	case THEME_KIND_TOKYO_NIGHT:
		return "tokyo-night";
	case THEME_KIND_AMBER_ON_BLACK:
		return "amber-on-black";
	case THEME_KIND_OLED:
		return "oled";
	case THEME_KIND_CUSTOM:
		return "custom";
	case THEME_KIND_DARK:
	default:
		return "dark";
	}
}

bool theme_reload_custom(void) {
	theme_palette_t next;
	if (!load_custom(&next)) return false;
	custom_accent = next.accent;
	palette_custom = next;
	palette_custom.accent = accent_index == -1 ? custom_accent_color
						  : (accent_index == 0 ? custom_accent : ACCENT_COLORS[accent_index]);
	if (selected_kind == THEME_KIND_CUSTOM) {
		rebuild_active();
		apply_palette();
		refresh_all();
	}
	return true;
}

void theme_set_kind(theme_kind_t kind) {
	if (kind < THEME_KIND_DARK || kind > THEME_KIND_CUSTOM) {
		return;
	}
	if (kind == THEME_KIND_CUSTOM && !theme_reload_custom()) {
		return;
	}
	if (selected_kind == kind) {
		return;
	}

	selected_kind = kind;
	if (accent_index == -1) {
		palette_tokyo.accent = custom_accent_color;
		palette_amber.accent = custom_accent_color;
		palette_oled.accent = custom_accent_color;
		palette_custom.accent = custom_accent_color;
	} else {
		palette_tokyo.accent = accent_index == 0 ? default_accent(THEME_KIND_TOKYO_NIGHT) : ACCENT_COLORS[accent_index];
		palette_amber.accent = accent_index == 0 ? default_accent(THEME_KIND_AMBER_ON_BLACK) : ACCENT_COLORS[accent_index];
		palette_oled.accent = accent_index == 0 ? default_accent(THEME_KIND_OLED) : ACCENT_COLORS[accent_index];
		palette_custom.accent = accent_index == 0 ? custom_accent : ACCENT_COLORS[accent_index];
	}
	rebuild_active();
	apply_palette();

	// Keep the new string key and the old boolean in step: older builds still
	// read `dark_theme`, while new builds can distinguish the named palettes.
	config_set("ui", "theme", theme_config_value(kind));
	config_set_bool("ui", "dark_theme", active->dark);
	config_save();

	// The boot script understands only light and dark; the added palettes use
	// the dark image and marker.
	bootlogo_set(theme_boot_screen(), active->dark);
	refresh_all();
}

void theme_toggle(void) { theme_set_kind(active->dark ? THEME_KIND_LIGHT : THEME_KIND_DARK); }

void theme_set_accent(int index) {
	if (index < 0 || index >= THEME_ACCENT_COUNT) {
		index = 0;
	}
	accent_index = index;
	palette_dark.accent = ACCENT_COLORS[index];
	palette_light.accent = ACCENT_COLORS[index];
	palette_tokyo.accent = index == 0 ? default_accent(THEME_KIND_TOKYO_NIGHT) : ACCENT_COLORS[index];
	palette_amber.accent = index == 0 ? default_accent(THEME_KIND_AMBER_ON_BLACK) : ACCENT_COLORS[index];
	palette_oled.accent = index == 0 ? default_accent(THEME_KIND_OLED) : ACCENT_COLORS[index];
	palette_custom.accent = index == 0 ? custom_accent : ACCENT_COLORS[index];
	rebuild_active(); // the tint is drawn from the accent, so it moves with it
	apply_palette();

	config_set_int("ui", "accent", index);
	config_save();

	refresh_all();
}

bool theme_has_custom_accent(void) { return accent_index == -1; }

lv_color_t theme_get_custom_accent(void) { return custom_accent_color; }

void theme_set_custom_accent(lv_color_t color) {
	accent_index = -1;
	custom_accent_color = color;
	palette_dark.accent = color;
	palette_light.accent = color;
	palette_tokyo.accent = color;
	palette_amber.accent = color;
	palette_oled.accent = color;
	palette_custom.accent = color;
	rebuild_active();
	apply_palette();

	int rgb = ((int)color.red << 16) | ((int)color.green << 8) | (int)color.blue;
	config_set_int("ui", "accent", -1);
	config_set_int("ui", "accent_custom", rgb);
	config_save();

	refresh_all();
}

bool theme_dynamic_tint(void) { return dynamic_tint; }

void theme_set_dynamic_tint(bool on) {
	if (dynamic_tint == on) {
		return;
	}
	dynamic_tint = on;
	rebuild_active();
	apply_palette();

	config_set_bool("ui", "dynamic_tint", on);
	config_save();

	refresh_all();
}
