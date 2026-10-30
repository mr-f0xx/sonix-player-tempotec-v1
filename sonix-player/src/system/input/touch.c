#include "touch.h"

#include <stddef.h>

// One axis of one source, in screen pixels.
//
// The range belongs to the source the value came from. In an inclusive 0..239
// panel and a 240-pixel screen that is the identity; in a 0..1023 panel and a
// 240-pixel screen it is the only thing standing between a finger near the
// right edge and a pointer pinned to it.
//
// A range whose maximum is BELOW its minimum is not an error and must not be
// thrown away: it is how a driver says the axis counts the other way, which is
// what a panel mounted upside down looks like from here. Dividing by a negative
// span mirrors the axis, so the sign of the division is doing the work -- and
// refusing those ranges was how a finger at the bottom of the glass came to be
// reported at the top of the screen.
static int to_pixels(int value, int in_min, int in_max, int out_max) {
	if (in_max == in_min || out_max <= 1) {
		return 0;
	}
	int64_t span = (int64_t)in_max - in_min;
	int64_t scaled = ((int64_t)value - in_min) * (out_max - 1) / span;
	if (scaled < 0) {
		scaled = 0;
	}
	if (scaled > out_max - 1) {
		scaled = out_max - 1;
	}
	return (int)scaled;
}

// A range is usable when both axes span something. Which way they span is not
// this test's business: an inverted axis is a transformed axis, not a broken one
// (see to_pixels).
static bool usable_range(touch_range_t r) { return r.max_x != r.min_x && r.max_y != r.min_y; }

static bool has_abs_point(const touch_filter_t *f) { return f->have_abs && f->have_abs_point; }

static bool has_mt_point(const touch_filter_t *f) { return f->have_mt && f->have_mt_point; }

void touch_init(touch_filter_t *f, int screen_w, int screen_h, bool have_abs, touch_range_t abs, bool have_mt,
				touch_range_t mt) {
	if (!f) {
		return;
	}
	f->screen_w = screen_w > 1 ? screen_w : 1;
	f->screen_h = screen_h > 1 ? screen_h : 1;
	f->rotated = false;
	f->have_abs = have_abs && usable_range(abs);
	f->abs = abs;
	f->have_mt = have_mt && usable_range(mt);
	f->mt = mt;
	f->slot = 0;
	f->btn_down = false;
	f->mt_down = false;
	f->have_abs_point = false;
	f->have_mt_point = false;
	f->abs_raw_x = f->abs_raw_y = 0;
	f->mt_raw_x = f->mt_raw_y = 0;
	f->waiting = false;
}

void touch_reset(touch_filter_t *f) {
	if (!f) {
		return;
	}
	f->slot = 0;
	f->btn_down = false;
	f->mt_down = false;
	f->waiting = false;
	f->have_abs_point = false;
	f->have_mt_point = false;
	f->abs_raw_x = f->abs_raw_y = 0;
	f->mt_raw_x = f->mt_raw_y = 0;
}

void touch_set_rotated(touch_filter_t *f, bool rotated) {
	if (f) {
		f->rotated = rotated;
	}
}

bool touch_pressed(const touch_filter_t *f) { return f && (f->btn_down || f->mt_down); }

const char *touch_source_name(const touch_filter_t *f) {
	if (!f) {
		return "none";
	}
	// The source the next point will come from -- not "the one with a point
	// right now", which at startup is neither of them.
	if (f->have_abs) {
		return "ABS_X/ABS_Y";
	}
	if (f->have_mt) {
		return "ABS_MT_POSITION";
	}
	return "none";
}

bool touch_point(const touch_filter_t *f, int *x, int *y) {
	if (!f) {
		return false;
	}
	// A press that has not sent its own coordinate yet: report nothing rather
	// than the previous finger's position.
	if (touch_pressed(f) && f->waiting) {
		return false;
	}

	int raw_x, raw_y, min_x, max_x, min_y, max_y;
	// ABS_X/ABS_Y wins whenever the panel reports it, and the reason is not
	// arbitrary: that pair is the driver's own presentation of the contact. It is
	// where a panel driver puts whatever correction the glass needs -- how it is
	// mounted, whether the ribbon leaves the panel at the far end -- while the
	// multi-touch pair is the controller's frame as it arrived. On this device
	// the driver turns ABS round and leaves the multi-touch pair alone, so
	// following the latter puts a finger at the bottom of the glass at the top of
	// the screen. Either way the range belongs to the source the value came from.
	if (has_abs_point(f)) {
		raw_x = f->abs_raw_x;
		raw_y = f->abs_raw_y;
		min_x = f->abs.min_x;
		max_x = f->abs.max_x;
		min_y = f->abs.min_y;
		max_y = f->abs.max_y;
	} else if (has_mt_point(f)) {
		raw_x = f->mt_raw_x;
		raw_y = f->mt_raw_y;
		min_x = f->mt.min_x;
		max_x = f->mt.max_x;
		min_y = f->mt.min_y;
		max_y = f->mt.max_y;
	} else {
		return false;
	}

	int px = to_pixels(raw_x, min_x, max_x, f->screen_w);
	int py = to_pixels(raw_y, min_y, max_y, f->screen_h);
	if (f->rotated) {
		px = f->screen_w - 1 - px;
		py = f->screen_h - 1 - py;
	}
	if (x) {
		*x = px;
	}
	if (y) {
		*y = py;
	}
	return true;
}

// The lifts. Both the coordinate of the press that just ended and the next
// press's "do I have a position yet" question hang off this: with the points
// cleared, a press only reports a place once its own coordinate has arrived.
static void release(touch_filter_t *f) {
	f->btn_down = false;
	f->mt_down = false;
	f->waiting = false;
	f->have_abs_point = false;
	f->have_mt_point = false;
}

// The both-ways press: BTN_TOUCH for protocol A, a tracking id for protocol B.
// A press that begins while a coordinate from before the lift is still around
// must not reuse it, hence asking whether there is a point at all.
static void press(touch_filter_t *f) {
	f->waiting = !(has_abs_point(f) || has_mt_point(f));
}

void touch_feed(touch_filter_t *f, const touch_event_t *ev) {
	if (!f || !ev) {
		return;
	}

	if (ev->type == TOUCH_EV_KEY) {
		if (ev->code != TOUCH_BTN_TOUCH) {
			return;
		}
		if (ev->value) {
			bool was = f->btn_down;
			f->btn_down = true;
			if (!was) {
				press(f);
			}
		} else {
			// BTN_TOUCH lifting means no contact is left, whatever the slots
			// said: a panel that sends both must not stay pressed because the
			// tracking id went by unread.
			release(f);
		}
		return;
	}

	if (ev->type != TOUCH_EV_ABS) {
		return;
	}

	switch (ev->code) {
	case TOUCH_ABS_X:
		f->abs_raw_x = ev->value;
		f->have_abs_point = true;
		f->waiting = false;
		break;
	case TOUCH_ABS_Y:
		f->abs_raw_y = ev->value;
		f->have_abs_point = true;
		f->waiting = false;
		break;
	case TOUCH_MT_X:
		// Only the slot being followed moves the pointer. A second contact is
		// not the finger the interface is being used with.
		if (f->slot == 0) {
			f->mt_raw_x = ev->value;
			f->have_mt_point = true;
			f->waiting = false;
		}
		break;
	case TOUCH_MT_Y:
		if (f->slot == 0) {
			f->mt_raw_y = ev->value;
			f->have_mt_point = true;
			f->waiting = false;
		}
		break;
	case TOUCH_MT_SLOT:
		// Events for any other slot are ignored rather than followed: their
		// coordinates are a different finger's, and their tracking id says
		// nothing about the first one. That keeps the press state honest --
		// a second finger lifting leaves the pointer pressed, as it should.
		if (ev->value >= 0) {
			f->slot = ev->value;
		}
		break;
	case TOUCH_MT_ID:
		if (f->slot != 0) {
			break;
		}
		if (ev->value >= 0) {
			bool was = f->mt_down;
			f->mt_down = true;
			if (!was) {
				press(f);
			}
		} else {
			f->mt_down = false;
			f->waiting = false;
			f->have_mt_point = false;
			if (!f->btn_down) {
				// No BTN_TOUCH on this panel: the tracking id is the release.
				f->have_abs_point = false;
			}
		}
		break;
	default:
		break;
	}
}
