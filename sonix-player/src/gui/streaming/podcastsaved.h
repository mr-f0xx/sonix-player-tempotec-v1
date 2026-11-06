#ifndef PODCASTSAVED_H
#define PODCASTSAVED_H

#include "lvgl.h"
#include "src/gui/shell/gui.h"

// The podcasts kept on the card (podcastdl.h): one row per folder under
// Podcast -- downloaded or copied there by hand -- and inside it the episodes
// the folder holds, newest first. Works with no network at all.
//
// A hand-copied episode is read like the music library reads a track: its
// title and date come from the file's own tags, at whatever precision the
// date tag has. A downloaded one keeps the downloader's word -- its sidecar
// title, and the publication date stamped on the file's mtime. The cover of a
// podcast is its folder's cover.jpg, else the artwork embedded in its files.
void podcastsaved_init(gui_config_t *cfg);

// The list of podcasts. Read from the card every time it is shown.
extern lv_obj_t *podcastsaved_screen;

// An episode was marked heard, or unmarked, by something other than this list --
// the player, when one ran to its end. The rows are redrawn with the mark when
// this is the list on screen behind the player's sheet; another list reads the
// mark when it is next opened. The same shape as
// podcastpage_episode_heard(), because the two lists show the same mark.
void podcastsaved_episode_heard(long long episode_id);

#endif /* PODCASTSAVED_H */
