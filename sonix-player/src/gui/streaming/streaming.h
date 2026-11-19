#ifndef STREAMING_H
#define STREAMING_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Network folders uses native libcurl; Listen Together uses LAN UDP sockets.
// Keep their screens in all menus. Only the WASM build lacks these transports;
// HOST_WEB_EMU also covers the native bridge and must not disable its controls.
#ifdef __EMSCRIPTEN__
#define STREAMING_LAN_PREVIEW_ONLY 1
#else
#define STREAMING_LAN_PREVIEW_ONLY 0
#endif

// The Streaming section, shared by the device and both emulator modes.
extern lv_obj_t *streaming_screen;

void streaming_init(gui_config_t *cfg);

// A visible but inactive network action. Do not attach its native callback.
void streaming_disable_preview_action(lv_obj_t *button, lv_obj_t *label);

#endif /* STREAMING_H */
