#ifndef GUI_GEARBOYPLAY_H
#define GUI_GEARBOYPLAY_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The play screen: the picture at the top, the controls under it.
//
// HiBy R3 Pro II and R1: 160x144 at three times is 480x432, exactly the panel
// width and 432 of its rows. What is left is the button pad: 288 rows on the
// R3 Pro II, which happens to be almost exactly the screen-to-buttons
// proportion of a real Game Boy, and 368 on the R1, where the extra 80 hold
// Select and Start.
//
// TempoTec V1: the panel is 240x320, so three times 160 does not fit it and
// there is no scale in between that keeps a Game Boy pixel a square of whole
// panel pixels. The picture is shown at 160x144 -- one panel pixel per Game Boy
// pixel -- centred in the upper half, and the 176 rows below it hold the same
// controls at about half the linear size, with Select and Start drawn under
// them. See the layout comment at the top of gearboyplay.c for the numbers.

extern lv_obj_t *gearboyplay_screen;

void gearboyplay_init(gui_config_t *cfg);

// Starts a game and opens the screen. The title comes from the database.
void gearboyplay_open(const char *rom_path, const char *title);

#endif /* GUI_GEARBOYPLAY_H */
