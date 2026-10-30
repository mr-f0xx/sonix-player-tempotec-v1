// Headless checks of the touch filter, with the panel taken out.
//
// The filter exists because of one bug, and most of these cases are that bug
// seen from a different angle: a position taken from one coordinate source and
// scaled with another one's range. On the device that reads as a list that
// drifts under a resting finger, a tap that lands on the wrong row and a drag
// that never starts -- none of which points at the arithmetic.
//
// The event streams below are what a Goodix panel actually sends: protocol A
// (BTN_TOUCH plus ABS_X/ABS_Y) and protocol B (slots, tracking ids and
// ABS_MT_POSITION_X/Y), with the two coordinate pairs in different units.

#include <assert.h>
#include <stdio.h>

#include "src/system/input/touch.h"

// A 240x320 player. ABS_X/ABS_Y carry pixels; the multi-touch pair carries the
// controller's raw counts, which is the arrangement that used to be mixed up.
#define SCREEN_W 240
#define SCREEN_H 320

static touch_filter_t panel(bool have_abs, bool have_mt) {
	touch_filter_t f;
	touch_range_t abs = {0, SCREEN_W - 1, 0, SCREEN_H - 1};
	touch_range_t mt = {0, 1023, 0, 1023};
	touch_init(&f, SCREEN_W, SCREEN_H, have_abs, abs, have_mt, mt);
	return f;
}

static void abs_at(touch_filter_t *f, int x, int y) {
	touch_feed(f, &(touch_event_t){TOUCH_EV_ABS, TOUCH_ABS_X, x});
	touch_feed(f, &(touch_event_t){TOUCH_EV_ABS, TOUCH_ABS_Y, y});
}

static void mt_at(touch_filter_t *f, int x, int y) {
	touch_feed(f, &(touch_event_t){TOUCH_EV_ABS, TOUCH_MT_X, x});
	touch_feed(f, &(touch_event_t){TOUCH_EV_ABS, TOUCH_MT_Y, y});
}

static void btn(touch_filter_t *f, int down) {
	touch_feed(f, &(touch_event_t){TOUCH_EV_KEY, TOUCH_BTN_TOUCH, down});
}

static void slot(touch_filter_t *f, int n) { touch_feed(f, &(touch_event_t){TOUCH_EV_ABS, TOUCH_MT_SLOT, n}); }

static void tracking(touch_filter_t *f, int id) { touch_feed(f, &(touch_event_t){TOUCH_EV_ABS, TOUCH_MT_ID, id}); }

// The same panel, with one range counting the other way round -- a driver saying
// the axis is mirrored, which is what a panel mounted the other way up gives.
static touch_filter_t panel_with_inverted_y(bool have_abs, bool have_mt) {
	touch_filter_t f;
	touch_range_t abs = {0, SCREEN_W - 1, SCREEN_H - 1, 0};
	touch_range_t mt = {0, 1023, 0, 1023};
	touch_init(&f, SCREEN_W, SCREEN_H, have_abs, abs, have_mt, mt);
	return f;
}

static void point_is(const touch_filter_t *f, int want_x, int want_y) {
	int x = -1, y = -1;
	assert(touch_point(f, &x, &y));
	assert(x == want_x);
	assert(y == want_y);
}

// The same finger described by both lands where ABS says it is, whatever the
// multi-touch pair carries: the driver's pair is the corrected one. Scaling the
// raw count with the pixel range -- what the evdev driver did -- clamps it to
// the edge instead.
static void check_abs_positions_the_pointer_when_present(void) {
	touch_filter_t f = panel(true, true);

	abs_at(&f, 120, 160);
	point_is(&f, 120, 160);

	// The multi-touch pair arrives in its own units, possibly turned over; it
	// must neither move the pointer nor be scaled with the ABS range.
	mt_at(&f, 512, 0);
	point_is(&f, 120, 160);

	abs_at(&f, 30, 300);
	point_is(&f, 30, 300);
}

// With no ABS on the panel, the multi-touch pair is the only thing there is.
static void check_multi_touch_positions_the_pointer_when_alone(void) {
	touch_filter_t f = panel(false, true);

	mt_at(&f, 1023, 1023);
	point_is(&f, SCREEN_W - 1, SCREEN_H - 1);
}

// A press whose coordinate has not arrived yet reports nothing rather than the
// previous finger's place -- the tap that lands on the row above.
static void check_a_press_waits_for_its_own_coordinate(void) {
	touch_filter_t f = panel(true, false);

	btn(&f, 1);
	mt_at(&f, 500, 500); // the other pair: not this panel's source
	int x = -1, y = -1;
	assert(!touch_point(&f, &x, &y));

	abs_at(&f, 40, 60);
	assert(touch_pressed(&f));
	point_is(&f, 40, 60);

	btn(&f, 0);
	assert(!touch_pressed(&f));

	// The next press starts with no coordinate of its own.
	btn(&f, 1);
	assert(!touch_point(&f, &x, &y));
	abs_at(&f, 200, 300);
	point_is(&f, 200, 300);
}

// Protocol B: the tracking id is the press, the slots carry the coordinates.
static void check_protocol_b_tracking_id(void) {
	touch_filter_t f = panel(false, true);

	slot(&f, 0);
	tracking(&f, 42);
	assert(touch_pressed(&f));

	mt_at(&f, 0, 0);
	point_is(&f, 0, 0);
	mt_at(&f, 1023, 0);
	point_is(&f, SCREEN_W - 1, 0);

	tracking(&f, -1);
	assert(!touch_pressed(&f));
}

// The interface is single-touch: a second contact is somebody else's finger and
// must not drag the pointer away from the one being used.
static void check_a_second_finger_is_ignored(void) {
	touch_filter_t f = panel(false, true);

	slot(&f, 0);
	tracking(&f, 7);
	mt_at(&f, 512, 512);
	int x = -1, y = -1;
	assert(touch_point(&f, &x, &y));
	int first_x = x, first_y = y;

	// A second contact, and its lift, which says nothing about the first.
	slot(&f, 1);
	tracking(&f, 8);
	mt_at(&f, 0, 1023);
	assert(touch_pressed(&f));
	point_is(&f, first_x, first_y);

	tracking(&f, -1);
	assert(touch_pressed(&f)); // the first finger is still down

	// ...and the first finger still moves the pointer.
	slot(&f, 0);
	mt_at(&f, 0, 0);
	point_is(&f, 0, 0);
}

static void check_rotation_turns_the_point_over(void) {
	touch_filter_t f = panel(true, false);

	abs_at(&f, 0, 0);
	point_is(&f, 0, 0);

	touch_set_rotated(&f, true);
	point_is(&f, SCREEN_W - 1, SCREEN_H - 1);

	touch_set_rotated(&f, false);
	point_is(&f, 0, 0);
}

// A range whose maximum is below its minimum is a mirrored axis, not an
// unusable one: the filter must keep it and divide by its sign. Dropping it was
// how a finger at the bottom of the glass was reported at the top of the screen.
static void check_an_inverted_range_mirrors_the_axis(void) {
	touch_filter_t f = panel_with_inverted_y(true, true);

	// The ascending multi-touch range must not replace the valid, inverted ABS
	// range: these coordinates are the panel driver's corrected positions.
	abs_at(&f, 0, 0);
	point_is(&f, 0, SCREEN_H - 1);
	mt_at(&f, 0, 0);
	point_is(&f, 0, SCREEN_H - 1);

	abs_at(&f, SCREEN_W - 1, SCREEN_H - 1);
	point_is(&f, SCREEN_W - 1, 0);

	// With an even-sized screen the two center pixels are adjacent; reversing
	// the axis maps the upper center pixel onto the lower one.
	abs_at(&f, 120, 160);
	point_is(&f, 120, SCREEN_H / 2 - 1);
}

static void check_an_inverted_multi_touch_range_is_usable_alone(void) {
	touch_filter_t f;
	touch_range_t mt = {0, 1023, 1023, 0};
	touch_init(&f, SCREEN_W, SCREEN_H, false, (touch_range_t){0}, true, mt);

	mt_at(&f, 0, 0);
	point_is(&f, 0, SCREEN_H - 1);
	mt_at(&f, 1023, 1023);
	point_is(&f, SCREEN_W - 1, 0);
}

static void check_out_of_range_is_clamped(void) {
	touch_filter_t f = panel(true, false);

	abs_at(&f, 4000, -12);
	point_is(&f, SCREEN_W - 1, 0);
}

// A panel that reports only the multi-touch pair: the other one being absent is
// not an error, and its events are not a coordinate.
static void check_a_multi_touch_only_panel(void) {
	touch_filter_t f = panel(false, true);

	abs_at(&f, 100, 100);
	int x = -1, y = -1;
	assert(!touch_point(&f, &x, &y));

	mt_at(&f, 1023, 1023);
	point_is(&f, SCREEN_W - 1, SCREEN_H - 1);
}

// Nothing has been touched yet: no point, no press.
static void check_an_idle_panel(void) {
	touch_filter_t f = panel(true, true);

	int x = -1, y = -1;
	assert(!touch_pressed(&f));
	assert(!touch_point(&f, &x, &y));
}

static void check_touch_reset(void) {
	touch_filter_t f = panel(true, true);
	btn(&f, 1);
	abs_at(&f, 100, 150);
	assert(touch_pressed(&f));
	int x = -1, y = -1;
	assert(touch_point(&f, &x, &y));

	touch_reset(&f);
	assert(!touch_pressed(&f));
	assert(!touch_point(&f, &x, &y));
}

int main(void) {
	check_an_idle_panel();
	check_abs_positions_the_pointer_when_present();
	check_multi_touch_positions_the_pointer_when_alone();
	check_a_press_waits_for_its_own_coordinate();
	check_protocol_b_tracking_id();
	check_a_second_finger_is_ignored();
	check_rotation_turns_the_point_over();
	check_an_inverted_range_mirrors_the_axis();
	check_an_inverted_multi_touch_range_is_usable_alone();
	check_out_of_range_is_clamped();
	check_a_multi_touch_only_panel();
	check_touch_reset();
	printf("touch: all checks passed\n");
	return 0;
}
