#ifndef BTRECEIVERSESSION_H
#define BTRECEIVERSESSION_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// How long the Bluetooth receiver lasts, and the one way it may outlive its page.
//
// The receiver runs only while its page is on screen. That is deliberate: the
// page is the only place the stream can be seen and stopped, so walking out of
// it by any route -- the power menu, a notification, a swipe home -- ends the
// mode, and the device is never left holding a stream nobody can see.
//
// The control centre makes one exception necessary. It can be pulled down over
// the page, and the reason it is allowed there at all is the equaliser: the
// receiver's audio goes through the same chain a decoded track does, and its
// pages are where that chain is set. Opening one of them from the sheet takes
// the user off the receiver's page, which would end the mode in the middle of
// the song -- the opposite of what reaching them from here is for.
//
// So the control centre may *hold* the receiver for a page it is about to open
// (btreceiversession_hold_for()). If that page is what is on screen once the
// receiver's page has been left, the mode is *parked*: it stays on, behind the
// page. A parked receiver ends the moment the way back to its page is gone from
// the back-stack -- home, another section, anything that makes the chevron lead
// somewhere else -- so it never outlives the pages it was parked for, and the
// invariant above only gains the one case it was bent for.
//
// A parked receiver is served as if its page were open: a sender that connects
// while the user is in the equaliser is started the same way (the page's own
// poll keeps running for it), and the side keys, the sleep timer, the power
// handling and the Wi-Fi priority all follow the mode, not the page.
//
// Nothing else is changed: a page left in any way the control centre did not
// ask to hold the receiver for still stops it, and a request that is not
// followed by the page it named is ignored, so a hold can never leak a stream.

// The receiver's page, once it exists. Called once, from the page's own init.
void btreceiversession_init(lv_obj_t *page);

// The page's own events: it has just been loaded, or just been left. Leaving
// stops the mode and gives the A2DP sink back unless the control centre held the
// receiver for the page that was loaded in its place.
void btreceiversession_page_loaded(void);
void btreceiversession_page_unloaded(void);

// The control centre is about to open `screen` from the receiver's page, or
// from a page the receiver is already parked behind. Call it just before the
// screen change; what matters is the page that is on screen afterwards, and
// the request lapses if that is some other.
void btreceiversession_hold_for(lv_obj_t *screen);

// True while the user is in the receiver: on its page, or parked behind one.
// Whether anything is playing is btreceiver_is_active(), and a different
// question -- the page can be open and waiting for a phone to connect.
bool btreceiversession_in_use(void);

// Ends the mode wherever the user is: stops it and, when it was parked, gives
// the A2DP sink back too. On the page itself that last part is what leaving it
// does, so there it only stops the mode.
void btreceiversession_end(void);

#endif /* BTRECEIVERSESSION_H */
