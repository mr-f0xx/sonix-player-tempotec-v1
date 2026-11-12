#ifndef SUBSONICDLPAGE_H
#define SUBSONICDLPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"
#include "src/system/streaming/subsonic.h"

// The Subsonic downloads page: the running download's progress, then every
// track already saved to the card, newest first, playable and deletable from
// there. Bytes are saved as ordinary files by subsonicdl.c, so the rows are
// local tracks and playing one is ordinary local playback.
//
// The page is built with the Subsonic page and opened from the corner arrow on
// its home tiles; starting a download from a list opens it too, so the
// transfer it started is visible.

extern lv_obj_t *subsonicdl_screen;
void subsonicdlpage_init(gui_config_t *cfg);

// Saves `count` tracks of the list the selection was made on, from `index`
// onwards. `page` is false for the one track tapped and true for every track
// from that one to the end of the list -- an album page is one album, a folder
// page is one directory. Opens the downloads page so the transfer is visible,
// and says why with a toast when nothing could start.
void subsonicdlpage_download(const subsonic_entry_t *tracks, int count, int index, bool page);

// True when `entry` is already on the card at the quality in force: the list
// draws these differently and does not offer to save them again.
bool subsonicdlpage_present(const subsonic_entry_t *entry);

// Opens the page on top of the Subsonic page (the corner arrow, and every
// started download).
void subsonicdlpage_open(void);

// Re-reads the card and redraws. Called when the page loads, when a download
// ends and after a delete; the timer only repaints the numbers in between.
void subsonicdlpage_repaint(void);

// Reports a download that ended while this page was not on screen. Called from
// the Subsonic page's own always-on tick; a job is announced exactly once, by
// whichever of the two ticks sees it first.
void subsonicdlpage_watch(void);

#endif /* SUBSONICDLPAGE_H */
