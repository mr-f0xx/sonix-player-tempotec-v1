// Linux evdev pointer reader for the touch panel.
//
// LVGL's stock lv_evdev treats every ABS_MT_TRACKING_ID != -1 as a press and
// relies on ABS_MT_TRACKING_ID == -1 or BTN_TOUCH == 0 to release it. That is
// multitouch protocol B. The touch drivers on these players (cst8xx_touch.ko,
// gt9xx_touch.ko) speak protocol A: ABS_MT_TRACKING_ID carries the contact id
// (0, 1, ...) and is never -1, while BTN_TOUCH says whether anything is on the
// glass. When an unpatched cst8xx_touch.ko emits a spurious contact on screen
// wake with point_num == 0, the input core suppresses the unchanged BTN_TOUCH 0
// and lv_evdev stays latched in LV_INDEV_STATE_PRESSED until a real touch
// presses and lifts -- swallowing the first tap or slide after every wake
// (issue #325) and refreshing LVGL's activity timestamp on every tick.
//
// Framing packets at SYN_REPORT and taking BTN_TOUCH as authoritative whenever
// the device declares it keeps protocol-A wake glitches out of LVGL, preserves
// short taps that press and lift inside one timer period, and lets a flush
// re-read the kernel's true key state via EVIOCGKEY.

#ifndef SONIX_TOUCH_EVDEV_H
#define SONIX_TOUCH_EVDEV_H

#include "lvgl/lvgl.h"
#include <stdbool.h>

// Opens `dev_path` non-blocking and registers an LV_INDEV_TYPE_POINTER indev.
// Returns NULL if the node cannot be opened or is not a pointer/touch device.
lv_indev_t *touch_evdev_create(const char *dev_path);

// Maps raw axis extents [min_x..max_x] x [min_y..max_y] onto the display.
// Inverted bounds (min > max) flip the axis for 180-degree panel rotation.
void touch_evdev_set_calibration(lv_indev_t *indev, int min_x, int min_y, int max_x, int max_y);

// Drains all queued input events, resynchronises against the kernel's live key
// state, and suppresses any finger still physically down until it lifts.
// Returns true if a real contact is still held on the glass after the drain.
bool touch_evdev_flush(lv_indev_t *indev);

#endif /* SONIX_TOUCH_EVDEV_H */
