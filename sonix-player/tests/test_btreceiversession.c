// The Bluetooth receiver's session, on real LVGL screens.
//
// The receiver is on only while its page is, with one exception: the control
// centre may hold it for an equaliser page, and it then waits behind that page
// for as long as the chevron can lead back (btreceiversession.h). What can go
// wrong is a receiver left on after the user has gone somewhere it cannot be
// seen or stopped, or a hold that stops a receiver it should have kept -- and
// both are decided by the order LVGL sends its events in when a screen is
// loaded, so this drives the production code with the real thing: real displays,
// real screens, real lv_screen_load(). Only the stack behind it is stubbed --
// the mode, the A2DP sink and the switcher's back-stack, which is kept here the
// way switcher.c keeps it: updated before the screen is loaded.
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/gui/bluetooth/btreceiversession.c"

// LVGL's default font is the project's; nothing here is drawn, so a stand-in
// does.
lv_font_t font_ui_14;

// The mode and the A2DP sink, counted.
static int mode_stops;
static int profile_off, profile_on;
void btreceiver_stop(void) { mode_stops++; }
void bluetooth_set_receiver_profile(bool on) {
	if (on) {
		profile_on++;
	} else {
		profile_off++;
	}
}

// The back-stack, and the three ways the switcher moves along it.
static lv_obj_t *history[8];
static int history_len;

bool switcher_history_contains(lv_obj_t *screen) {
	for (int i = 0; i < history_len; i++) {
		if (history[i] == screen) {
			return true;
		}
	}
	return false;
}

// switch_screen(): the page being left is remembered, then the next is loaded.
static void go(lv_obj_t *target) {
	history[history_len++] = lv_screen_active();
	lv_screen_load(target);
}

// back_btn_cb(): the stack is popped, then what was under is loaded.
static void back(void) {
	assert(history_len > 0);
	lv_obj_t *previous = history[--history_len];
	lv_screen_load(previous);
}

// A swipe home, or any jump to the menu: the stack is emptied.
static void home(lv_obj_t *menu) {
	history_len = 0;
	lv_screen_load(menu);
}

// The page's own wiring, as btreceiverpage.c does it.
static void page_loaded_cb(lv_event_t *e) {
	(void)e;
	btreceiversession_page_loaded();
}

static void page_unloaded_cb(lv_event_t *e) {
	(void)e;
	btreceiversession_page_unloaded();
}

typedef struct {
	lv_display_t *display;
	lv_obj_t *menu, *page, *peq, *autoeq, *other;
} world_t;

// The menu, the receiver's page reached from it, and the pages around: the
// equaliser's, a child of it (the Auto EQ browser), and one the receiver cannot
// follow. Counters are zeroed once the user is on the receiver's page.
static world_t make_world(void) {
	world_t w;
	w.display = lv_display_create(240, 320);
	w.menu = lv_obj_create(NULL);
	w.page = lv_obj_create(NULL);
	w.peq = lv_obj_create(NULL);
	w.autoeq = lv_obj_create(NULL);
	w.other = lv_obj_create(NULL);
	lv_obj_add_event_cb(w.page, page_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(w.page, page_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	btreceiversession_init(w.page);

	history_len = 0;
	lv_screen_load(w.menu);
	assert(!btreceiversession_in_use());
	go(w.page);
	assert(btreceiversession_in_use());
	mode_stops = profile_off = profile_on = 0;
	return w;
}

static void free_world(world_t *w) { lv_display_delete(w->display); }

static bool is_parked(void) { return parked; }

// The control centre opens the equaliser from the receiver's page, and the
// user backs out of it: the receiver was never touched, and the page is back.
static void test_parks_behind_the_equaliser_and_comes_back(void) {
	world_t w = make_world();

	btreceiversession_hold_for(w.peq);
	go(w.peq);
	assert(is_parked() && btreceiversession_in_use());
	assert(mode_stops == 0 && profile_off == 0);
	assert(hold_target == NULL); // consumed

	back();
	assert(!is_parked() && btreceiversession_in_use());
	assert(lv_screen_active() == w.page);
	assert(mode_stops == 0 && profile_off == 0);

	// And again: the second trip is no different from the first.
	btreceiversession_hold_for(w.peq);
	go(w.peq);
	assert(is_parked());
	back();
	assert(!is_parked() && mode_stops == 0 && profile_off == 0);

	// Leaving the page for good afterwards still ends the mode as it always did.
	go(w.other);
	assert(!btreceiversession_in_use());
	assert(mode_stops == 1 && profile_off == 1);
	free_world(&w);
}

// The user pulls the sheet down over the equaliser and swipes home: the way back
// to the receiver's page is gone, and so is the receiver. Nothing is left
// holding a stream nobody can see.
static void test_a_swipe_home_from_the_equaliser_ends_it(void) {
	world_t w = make_world();

	btreceiversession_hold_for(w.peq);
	go(w.peq);
	assert(is_parked());

	home(w.menu);
	assert(!is_parked() && !btreceiversession_in_use());
	assert(mode_stops == 1 && profile_off == 1 && profile_on == 0);

	// Nothing further happens on the next pages.
	go(w.other);
	back();
	assert(mode_stops == 1 && profile_off == 1);
	free_world(&w);
}

// The equaliser has pages of its own (the Auto EQ browser, opened from PEQ).
// The receiver waits behind all of them as long as the chevron leads back, and
// ends when it no longer does.
static void test_child_pages_of_the_equaliser_keep_it(void) {
	world_t w = make_world();

	btreceiversession_hold_for(w.peq);
	go(w.peq);
	go(w.autoeq);
	assert(is_parked() && mode_stops == 0);
	back(); // Auto EQ -> PEQ
	assert(is_parked() && mode_stops == 0);
	go(w.autoeq);
	assert(is_parked() && mode_stops == 0);

	home(w.menu);
	assert(!is_parked() && mode_stops == 1 && profile_off == 1);
	free_world(&w);

	// ...and the whole way back through them lands on the receiver's page.
	w = make_world();
	btreceiversession_hold_for(w.peq);
	go(w.peq);
	go(w.autoeq);
	back();
	back();
	assert(lv_screen_active() == w.page && !is_parked());
	assert(mode_stops == 0 && profile_off == 0);
	free_world(&w);
}

// Something else loaded over a parked receiver -- a notification, the charge
// screen -- leaves the way back where it was, so it stays.
static void test_a_page_over_a_parked_receiver_does_not_end_it(void) {
	world_t w = make_world();

	btreceiversession_hold_for(w.peq);
	go(w.peq);
	go(w.other);
	assert(is_parked() && mode_stops == 0);
	back();
	back();
	assert(lv_screen_active() == w.page && !is_parked() && mode_stops == 0);
	free_world(&w);
}

// No hold: leaving the page is what it always was.
static void test_leaving_without_a_hold_stops_it(void) {
	world_t w = make_world();

	go(w.other); // the power menu, a notification, a swipe
	assert(!is_parked() && !btreceiversession_in_use());
	assert(mode_stops == 1 && profile_off == 1);
	free_world(&w);

	// The equaliser page is not special on its own: only the control centre's
	// request makes it a place the receiver waits.
	w = make_world();
	go(w.peq);
	assert(!is_parked() && mode_stops == 1 && profile_off == 1);
	free_world(&w);
}

// A hold names a page, and it is honoured only if that page is the one that
// comes. A request that is followed by anything else, or by nothing, must not
// outlive its moment and keep a receiver for a page loaded later.
static void test_a_hold_for_another_page_is_not_honoured(void) {
	world_t w = make_world();

	btreceiversession_hold_for(w.peq);
	go(w.other);
	assert(!is_parked() && mode_stops == 1 && profile_off == 1);
	assert(hold_target == NULL);

	// Back on the page, and on to the equaliser *without* a request now: the
	// old one is gone, so this is a way out like any other.
	back();
	assert(lv_screen_active() == w.page);
	mode_stops = profile_off = 0;
	go(w.peq);
	assert(!is_parked() && mode_stops == 1 && profile_off == 1);
	free_world(&w);

	// A request followed by the user leaving some other way is gone as well: the
	// page's own unload drops it, and so does arriving on the page again, so it
	// cannot wait for a screen change that comes much later.
	w = make_world();
	btreceiversession_hold_for(w.peq); // the screen change never happened
	back();							   // out to the menu by some other road
	assert(!btreceiversession_in_use() && mode_stops == 1 && hold_target == NULL);
	btreceiversession_hold_for(w.peq); // and one that was left lying
	go(w.page);
	assert(hold_target == NULL && !is_parked());
	mode_stops = profile_off = 0;
	go(w.peq);
	assert(!is_parked() && mode_stops == 1 && profile_off == 1);
	free_world(&w);
}

// The control centre ends the session itself when the user says yes to leaving
// it, from wherever the receiver is.
static void test_ending_the_session(void) {
	// Parked behind the equaliser: nothing else will give the sink back.
	world_t w = make_world();
	btreceiversession_hold_for(w.peq);
	go(w.peq);
	btreceiversession_end();
	assert(!is_parked() && !btreceiversession_in_use());
	assert(mode_stops == 1 && profile_off == 1);
	// Where the user goes next does not do it a second time.
	home(w.menu);
	assert(mode_stops == 1 && profile_off == 1);
	free_world(&w);

	// On the page: only the mode stops. Leaving the page is what gives the sink
	// back, and it is about to.
	w = make_world();
	btreceiversession_end();
	assert(mode_stops == 1 && profile_off == 0 && !is_parked());
	go(w.other);
	assert(profile_off == 1);
	free_world(&w);

	// A request waiting for a page is dropped with the session.
	w = make_world();
	btreceiversession_hold_for(w.peq);
	btreceiversession_end();
	assert(hold_target == NULL);
	go(w.peq);
	assert(!is_parked());
	free_world(&w);
}

// Not in the receiver at all: pages come and go and the session does nothing.
static void test_nothing_happens_outside_the_session(void) {
	world_t w = make_world();
	back(); // to the menu: the page is left on purpose, stop and sink as ever
	assert(mode_stops == 1 && profile_off == 1);
	mode_stops = profile_off = 0;

	go(w.other);
	go(w.peq);
	go(w.autoeq);
	home(w.menu);
	assert(mode_stops == 0 && profile_off == 0 && profile_on == 0);
	assert(!btreceiversession_in_use());
	free_world(&w);
}

// Initialising again, as a test harness or a rebuild of the pages might, must
// not leave a second copy of the watcher on the display.
static void test_init_is_idempotent(void) {
	world_t w = make_world();
	uint32_t once = lv_display_get_event_count(w.display);
	btreceiversession_init(w.page);
	btreceiversession_init(w.page);
	assert(lv_display_get_event_count(w.display) == once);

	// And it still does its work afterwards.
	btreceiversession_hold_for(w.peq);
	go(w.peq);
	assert(is_parked());
	home(w.menu);
	assert(mode_stops == 1 && profile_off == 1);
	free_world(&w);
}

int main(void) {
	lv_init();

	test_parks_behind_the_equaliser_and_comes_back();
	test_a_swipe_home_from_the_equaliser_ends_it();
	test_child_pages_of_the_equaliser_keep_it();
	test_a_page_over_a_parked_receiver_does_not_end_it();
	test_leaving_without_a_hold_stops_it();
	test_a_hold_for_another_page_is_not_honoured();
	test_ending_the_session();
	test_nothing_happens_outside_the_session();
	test_init_is_idempotent();

	puts("btreceiversession: the receiver parks behind the equaliser, comes back, and ends when the way back is gone");
	return 0;
}
