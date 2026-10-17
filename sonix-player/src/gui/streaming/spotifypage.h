#ifndef SPOTIFYPAGE_H
#define SPOTIFYPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Streaming -> Spotify.
//
// The page is the switch and the browser for the receiver on the card: the
// service behind it is src/system/streaming/spotify.c, and the protocol it
// speaks over the receiver's browse socket is Q2SPOT/1 (spotifybrowse.c). This
// file is only the interface -- what the switch says, which list is on screen
// and what a tap sends.
//
// It follows the two pages it was built from: the switch, the Wi-Fi notice and
// the "it stays on when you leave" behaviour are AirPlay's (the receiver is not
// tied to its page, so the page only stops polling), and the browser is
// Subsonic's (one job at a time on a worker thread, one mailbox each way, a
// history so Back walks the lists).

extern lv_obj_t *spotify_screen;

void spotifypage_init(gui_config_t *cfg);

#endif /* SPOTIFYPAGE_H */
