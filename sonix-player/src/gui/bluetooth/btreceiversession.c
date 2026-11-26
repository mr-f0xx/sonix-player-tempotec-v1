#include "btreceiversession.h"

#include <stdio.h>

#include "src/gui/shell/switcher.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btreceiver.h"

// The state is three things: whether a session is live at all, whether the
// control centre has said it is about to open a page from there (only relevant
// when background reception is off), and whether the session is waiting behind
// such a page now.
static bool session;
static bool background;
static lv_obj_t *receiver_page;
static lv_obj_t *hold_target;
static bool parked;

bool btreceiversession_in_use(void) { return session; }

bool btreceiversession_background(void) { return background; }

void btreceiversession_background_set(bool on) { background = on; }

void btreceiversession_hold_for(lv_obj_t *screen) { hold_target = screen; }

void btreceiversession_end(void) {
	session = false;
	parked = false;
	hold_target = NULL;
	btreceiver_stop();
	// The page may still be on screen: it is not going to give the sink back
	// for us, so the session does it here. bluetooth_set_receiver_profile()
	// only acts when the wanted state changes, so an unload that follows is
	// harmless.
	bluetooth_set_receiver_profile(false);
}

void btreceiversession_page_loaded(void) {
	if (parked) {
		fprintf(stderr, "btreceiver: back on the receiver's page\n");
	}
	parked = false;
	hold_target = NULL;
	session = true;
}

void btreceiversession_page_unloaded(void) {
	lv_obj_t *held_for = hold_target;
	hold_target = NULL;

	// The control centre may have ended the session from its own card while
	// this page was still on screen; there is nothing left to stop here.
	if (!session) {
		return;
	}

	// Background reception is the point of the feature: the session outlives
	// every page, and the card is where the user can come back to it or stop it.
	if (background) {
		parked = false;
		fprintf(stderr, "btreceiver: continuing in the background\n");
		return;
	}

	// The page that is on screen now is the one the control centre said it was
	// opening: the receiver waits behind it. Anything else -- no request, or one
	// for a page that did not come -- is a way out like any other.
	if (held_for && held_for == lv_screen_active()) {
		parked = true;
		fprintf(stderr, "btreceiver: staying on behind a page opened from the control centre\n");
		return;
	}

	// Every other way out -- the power menu, a notification, a screen change the
	// page did not ask for -- ends the mode too.
	parked = false;
	session = false;
	btreceiver_stop();
	bluetooth_set_receiver_profile(false);
}

// Every page that is loaded, anywhere. A receiver parked behind the equaliser
// waits for as long as the chevron can still lead back to its page. A receiver
// running in the background has no such limit: it was left on purpose and ends
// only when the user stops it or the radio goes away.
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

void btreceiversession_tick(void) {
	if (session && !bluetooth_get_enabled()) {
		fprintf(stderr, "btreceiver: Bluetooth is off; ending the receiver\n");
		btreceiversession_end();
	}
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
