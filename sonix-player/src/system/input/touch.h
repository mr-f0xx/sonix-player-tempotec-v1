#ifndef SONIX_PLAYER_TOUCH_H
#define SONIX_PLAYER_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

// A touch panel's event stream, turned into "where is the finger" without any
// of the surprises a generic evdev driver has.
//
// The reason this exists is a specific bug worth spelling out, because it is
// invisible in the code that had it. lv_evdev takes its pointer position from
// whichever of ABS_X and ABS_MT_POSITION_X arrives last, but takes the range it
// scales with from ABS_X alone. On a panel where those two carry different
// units -- raw sensor counts in one, screen pixels in the other, which is the
// usual arrangement on a Goodix controller -- every multi-touch event re-scales
// a raw count against a pixel range and the pointer jumps. A few counts of
// finger jitter become several pixels, a tap lands on a row that was never
// touched, and a drag never gets going because its first movement is a leap.
//
// So: each source is scaled with its own range, and the point that is reported
// comes from one source at a time. Nothing is ever scaled with somebody else's
// units.
//
// The source that one point comes from is ABS_X/ABS_Y whenever the panel sends
// it, because that pair is the driver's own presentation of the contact: a
// driver turns the panel round for how the glass is mounted, and the
// multi-touch pair is left as the controller sent it. Following the latter on a
// panel whose driver corrects only the former puts a finger at the bottom of
// the glass at the top of the screen. The multi-touch pair is used when it is
// the only one there is.
//
// Nothing here knows about the kernel: main.c translates struct input_event into
// touch_event_t, which is what lets the whole filter be tested on the host.

// The events this filter understands, as its own tokens. main.c maps the
// kernel's EV_KEY/EV_ABS and code numbers onto them.
enum {
	TOUCH_EV_KEY = 1,
	TOUCH_EV_ABS = 3,
};

enum {
	TOUCH_ABS_X = 0, // protocol A: the scaled position
	TOUCH_ABS_Y = 1,
	TOUCH_MT_X = 2, // protocol B: the per-slot position
	TOUCH_MT_Y = 3,
	TOUCH_MT_SLOT = 4,
	TOUCH_MT_ID = 5,
	TOUCH_BTN_TOUCH = 6,
};

typedef struct {
	uint16_t type;
	uint16_t code;
	int32_t value;
} touch_event_t;

// What one axis of one source spans, straight from EVIOCGABS.
typedef struct {
	int min_x, max_x;
	int min_y, max_y;
} touch_range_t;

typedef struct {
	int screen_w, screen_h;
	bool rotated;

	// Which of the two sources this panel sends, and the range of each. A
	// source is usable when neither axis has a zero-width range; descending
	// ranges are valid and mirror that axis.
	bool have_abs, have_mt;
	touch_range_t abs, mt;

	// Protocol B: the slot being followed. The interface is single-touch, so
	// that is slot 0 -- the first finger down -- and a second contact is
	// ignored rather than allowed to drag the pointer somewhere else.
	int slot;

	// The two ways a press is announced: BTN_TOUCH (protocol A) and a
	// tracking id (protocol B). Either one can be the only one a panel sends.
	bool btn_down;
	bool mt_down;

	// Where the finger is per source, in that source's own units.
	int abs_raw_x, abs_raw_y;
	bool have_abs_point;
	int mt_raw_x, mt_raw_y;
	bool have_mt_point;

	// Pressed but no coordinate for this press yet. Reporting a press at the
	// previous finger's position is how a tap lands on the row above the one
	// somebody touched, so the filter says "no point" until the coordinate for
	// the press that is happening now has arrived.
	bool waiting;
} touch_filter_t;

// Sets up the filter for one panel. `have_abs`/`have_mt` say which sources the
// device actually emits; a range with equal endpoints on either axis is ignored.
void touch_init(touch_filter_t *f, int screen_w, int screen_h, bool have_abs, touch_range_t abs, bool have_mt,
				touch_range_t mt);

// One event from the panel. Safe to call with events the filter ignores.
void touch_feed(touch_filter_t *f, const touch_event_t *ev);

// Whether a finger is down.
bool touch_pressed(const touch_filter_t *f);

// The finger in screen pixels. False when there is nothing to report: no
// coordinate has arrived at all, or a press has begun and its own coordinate is
// still to come.
bool touch_point(const touch_filter_t *f, int *x, int *y);

// The display turned over; the same finger is now somewhere else on screen.
void touch_set_rotated(touch_filter_t *f, bool rotated);

// Which source the reported point comes from, for the boot log.
const char *touch_source_name(const touch_filter_t *f);

// Clears any latched press, coordinates, or slot tracking state (e.g. on screen wake).
void touch_reset(touch_filter_t *f);

#endif
