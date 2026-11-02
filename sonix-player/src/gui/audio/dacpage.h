#ifndef DACPAGE_H
#define DACPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// USB DAC mode: the page that owns the device while a computer is playing
// through it.
//
// It is deliberately an ownership screen. Once DAC mode is on the player is
// not a player any more -- the audio comes from the cable and local transport
// buttons would act on nothing. The Control Centre may overlay it for audio
// controls, but substitutes DAC status for the local transport. The back
// gesture and hardware transport keys are held (volume and power excepted, which
// still do what they say), and leaving asks first.
extern lv_obj_t *dacpage_screen;

void dacpage_init(gui_config_t *cfg);

// True while the page owns the device, for the key handling and the gesture
// guards elsewhere to ask.
bool dacpage_is_holding(void);

#endif /* DACPAGE_H */
