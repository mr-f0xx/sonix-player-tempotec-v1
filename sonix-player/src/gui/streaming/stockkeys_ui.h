#ifndef STOCKKEYS_UI_H
#define STOCKKEYS_UI_H

#include "lvgl/lvgl.h"

// Populates a streaming service's home container when its API keys are not yet
// configured: renders an explanation, a "Set up from stock firmware" action
// row, and (when neither a microSD stock archive nor Wi-Fi is currently ready)
// a shortcut to Wi-Fi settings.
//
// When extraction finishes, reloads streaming-keys.ini, initializes Qobuz and
// Tidal in place without rebooting, and calls `on_ready()` so the active page
// immediately switches to Sign in.
void stockkeys_ui_populate_missing(lv_obj_t *home_container, void (*on_ready)(void));

#endif /* STOCKKEYS_UI_H */
