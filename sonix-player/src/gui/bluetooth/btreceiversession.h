#ifndef BTRECEIVERSESSION_H
#define BTRECEIVERSESSION_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// How long the Bluetooth receiver lasts, and the ways it may outlive its page.
//
// The receiver starts when its page is loaded. By default it may then keep
// running with the page left -- the A2DP sink stays up, a phone can connect and
// play, and the control centre's card shows what it is receiving. Leaving is
// not silent any more: the blue receiver LED, the Bluetooth icon and the card
// say that the player is still a speaker. Tap the card to come back to the
// receiver's page; hold it to stop. The switch on the page turns this off for
// someone who wants the old, page-only behaviour.
//
// Turning the radio off while a session is live is not allowed: the session
// ends first, either from the card or from the switch on its page. A radio
// going away on its own ends the session on the page's next poll.
//
// The control centre also has one narrower way to hold the receiver, for the
// equaliser pages it opens from it (btreceiversession_hold_for()). That is what
// is used when background reception is switched off: the receiver waits behind
// such a page for as long as the chevron can lead back, and ends when it cannot.
// With background reception on, every page is one the receiver can wait behind,
// so the hold is unnecessary and is simply ignored.
//
// Everything that follows the mode rather than the page -- the side keys, the
// sleep timer, the power handling and the Wi-Fi priority -- uses the session,
// not the screen that happens to be on top.

// The receiver's page, once it exists. Called once, from the page's own init.
void btreceiversession_init(lv_obj_t *page);

// The page's own events: it has just been loaded, or just been left. Loading
// starts a session; leaving keeps it when background reception is on, parks it
// behind an equaliser page when one was requested, and otherwise ends it.
void btreceiversession_page_loaded(void);
void btreceiversession_page_unloaded(void);

// The control centre is about to open `screen` from the receiver's page, or
// from a page the receiver is already parked behind. Call it just before the
// screen change; what matters is the page that is on screen afterwards, and
// the request lapses if that is some other. Only used when background
// reception is off.
void btreceiversession_hold_for(lv_obj_t *screen);

// True while the user is in the receiver: on its page, parked behind one, or
// somewhere else with reception running in the background. Whether anything
// is playing is btreceiver_is_active(), and a different question -- the page
// can be open and waiting for a phone to connect.
bool btreceiversession_in_use(void);

// Whether reception may continue off the page. The page reads this from
// config and sets it once at init; the switch on the page changes it later.
bool btreceiversession_background(void);
void btreceiversession_background_set(bool on);

// Ends the mode wherever the user is: stops it and gives the A2DP sink back.
// This is the one way out of a background session; a page being unloaded does
// not end one.
void btreceiversession_end(void);

// Called from the receiver page's poll. A radio switched off somewhere else
// cannot carry a stream, so the session ends rather than sitting on a profile
// that no longer answers.
void btreceiversession_tick(void);

#endif /* BTRECEIVERSESSION_H */
