#ifndef CORNERS_H
#define CORNERS_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

// Rounded screen corners: four black quarter-circle masks, one in each corner
// of the display, on the system layer so they sit above every page, sheet,
// dialog and the screensaver alike. They take no touches.
//
// Two settings on the Appearance page, both under "ui": whether they are drawn
// at all ("rounded_corners", off by default) and how far they cut in
// ("corner_radius"). The radius starts at bp_screen_corner_radius() -- 16 px on
// the TempoTec V1's 240×320 panel, which is the 2 mm of glass the 2" IPS
// actually rounds by -- so the corners of anyone who already had them on are
// exactly as they were. The V1's shell is squarer than that glass reads in the
// hand, so the page lets the arc be drawn tighter, down to a hint of one.

// What the Appearance slider offers: every CORNERS_RADIUS_STEP pixels from the
// smallest to the largest. The largest stays under the 24 px status bar: an arc
// taller than the bar would start cutting into its clock and battery.
#define CORNERS_RADIUS_MIN 2
#define CORNERS_RADIUS_MAX 20
#define CORNERS_RADIUS_STEP 2

void corners_init(gui_config_t *cfg);

bool corners_enabled(void);
void corners_set_enabled(bool enabled);

// The arc, in pixels, as it is drawn or will be the next time the corners are
// on. Always within CORNERS_RADIUS_MIN..CORNERS_RADIUS_MAX: a value from a
// hand-edited config is pulled into range rather than refused. Changing it
// redraws the corners at once, even while they are on screen behind the page
// that moved the slider.
int corners_radius(void);
void corners_set_radius(int radius);

#endif
