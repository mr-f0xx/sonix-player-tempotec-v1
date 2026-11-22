#ifndef ICONSET_H
#define ICONSET_H

#include <stdbool.h>

#include "src/gui/shell/theme.h"

// SD icon sets: whole-interface artwork living in Icons/<set>/ on the card,
// chosen from Appearance instead of being baked into the firmware (see
// docs/icon-sets.md). Each set is a folder of <id>.png files - the id of a
// built-in glyph (src/gui/shell/icons.h) - optionally with a palette.ini that
// colours the function roles. Loading a set replaces the pixels behind those
// descriptors in place, so every screen that already points at them changes
// with it; the recolor the UI applies on top is dropped for those icons (see
// sonix_iconset_recolor_bypass and patches/lvgl-iconset-recolor.patch) so the
// artwork's own colours show through, while accent/state recolors still paint.

// Where the card is mounted; theme_set_card_root's twin. Empty until called.
void iconset_set_card_root(const char *root);

// Reads the saved choice (config "ui"/"icon_set") and publishes the set to the
// theme. Call once from gui_init(), right after theme_init().
void iconset_init(void);

// Folder names under Icons/, written to names (up to max). The count is how
// many were written. "" (Default) is not among them: it is the absence of a
// set and always offered by the settings row itself.
int iconset_choices(const char **names, int max);

// The active set's folder name, or "" when the built-in glyphs show.
const char *iconset_active_name(void);

// True when a set is loaded and showing its artwork.
bool iconset_active(void);

// Switches sets: name "" unloads (back to the built-in glyphs); anything else
// must be a folder under Icons/. On failure nothing changes and false comes
// back so the caller can say so.
bool iconset_select(const char *name);

// The set's colour for a function role, for theme_semantic_color(); false
// when no set is loaded or the palette doesn't define the role, leaving the
// theme's own colour in charge.
bool iconset_semantic_color(theme_semantic_t role, lv_color_t *out);

// The hook the patched lv_obj_draw.c calls: drops the idle recolor for
// swapped artwork so its baked colours draw instead. Weak there, so builds
// without the app simply keep the tint.
lv_opa_t sonix_iconset_recolor_bypass(lv_obj_t *obj, lv_color_t recolor, lv_opa_t opa);

#endif // ICONSET_H
