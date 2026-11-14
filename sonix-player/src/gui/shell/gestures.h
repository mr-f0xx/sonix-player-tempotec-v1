#ifndef GESTURES_H
#define GESTURES_H

#include <stdbool.h>

// ---------------------------------------------------------------------------
// Which edge each of the two sheets is pulled from
//
// The interface has two of them: the control centre and Cover Flow. There are
// two ways round, and they are the two that have shipped:
//
//   classic -- what this fork did first. The control centre comes down from the
//              status bar, and Cover Flow is pulled up from the bottom band of
//              the main menu.
//   stock   -- what the TempoTec firmware does. Cover Flow comes down from the
//              top edge, and the control centre rises from the bottom.
//
// The classic one is the default: a player that has been updated keeps the
// gesture it knows, and nothing moves under anybody's thumb because of an
// update. The stock arrangement is the option (Settings > Appearance).
//
// Everything that draws one of the sheets asks this at the moment the sheet
// opens -- see the notes over the two *_prepare() functions -- so switching it
// takes effect at the next pull and not at the next boot.
// ---------------------------------------------------------------------------

bool gestures_stock(void);
void gestures_set_stock(bool on);

bool gestures_swipe_up_home(void);
void gestures_set_swipe_up_home(bool on);

#endif /* GESTURES_H */
