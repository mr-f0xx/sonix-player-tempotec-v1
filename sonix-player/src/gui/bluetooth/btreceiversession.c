#include "btreceiversession.h"

#include <stdio.h>

#include "src/gui/shell/switcher.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btreceiver.h"

// See the header for what this is for. The state is three things: which page
// is the receiver's, which page the control centre has said it is about to open
// from there, and whether the receiver is waiting behind such a page now.
static lv_obj_t *receiver_page;
static lv_obj_t *hold_target;
static bool parked;

bool btreceiversession_in_use(void) { return parked || (receiver_page && lv_screen_active() == receiver_page); }

void btreceiversession_hold_for(lv_obj_t *screen) { hold_target = screen; }

void btreceiversession_end(void) {
	hold_target = NULL;
	btreceiver_stop();
	if (parked) {
		// The page is not on screen to do it: leaving it is what gives the sink
		// back, and that happened when the receiver was parked.
		parked = false;
		bluetooth_set_receiver_profile(false);
	}
}

void btreceiversession_page_loaded(void) {
	if (parked) {
		fprintf(stderr, "btreceiver: back on the receiver's page\n");
	}
	parked = false;
	hold_target = NULL;
}

void btreceiversession_page_unloaded(void) {
	lv_obj_t *held_for = hold_target;
	hold_target = NULL;

	// The page that is on screen now is the one the control centre said it was
	// opening: the receiver waits behind it. Anything else -- no request, or one
	// for a page that did not come -- is a way out like any other.
	if (held_for && held_for == lv_screen_active()) {
		parked = true;
		fprintf(stderr, "btreceiver: staying on behind a page opened from the control centre\n");
		return;
	}

	// Every other way out -- the power menu, a notification, a screen change the
	// page did not ask for -- ends the mode too. The page's own back button asks
	// first; this catches the rest, so the device is never left holding a stream
	// nobody can see.
	parked = false;
	btreceiver_stop();
	bluetooth_set_receiver_profile(false);
}

// Every page that is loaded, anywhere. While the receiver is parked it waits for
// as long as the chevron can still lead back to its page, and not a step
// longer: the page that was loaded is either the receiver's own (back where it
// was; its own event says so), or one the receiver can still be reached from, or
// neither -- a swipe home, a jump to another section -- and then there is no
// page left on which it could be seen or stopped.
static void screen_loaded_cb(lv_event_t *e) {
	if (!parked) {
		return;
	}
	lv_obj_t *screen = lv_event_get_param(e);
	if (screen == receiver_page || switcher_history_contains(receiver_page)) {
		return;
	}
	fprintf(stderr, "btreceiver: the way back to the receiver's page is gone; ending the receiver\n");
	btreceiversession_end();
}

void btreceiversession_init(lv_obj_t *page) {
	receiver_page = page;
	hold_target = NULL;
	parked = false;

	// Once per display: initialising twice must not make every page load count
	// twice.
	lv_display_t *display = lv_obj_get_display(page);
	lv_display_remove_event_cb_with_user_data(display, screen_loaded_cb, NULL);
	lv_display_add_event_cb(display, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
