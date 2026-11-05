#ifndef PODCASTPAGE_H
#define PODCASTPAGE_H

#include "lvgl.h"
#include "src/gui/shell/gui.h"

void podcastpage_init(gui_config_t *cfg);

// The section's screen, for the streaming grid. There is only one: the two
// entries are pills -- like the audiobooks' all / recent / finished -- and a
// pill changes what is underneath rather than navigating elsewhere, so the
// list sits on the same screen.
extern lv_obj_t *podcast_screen;

// Where leaving the player returns to after a podcast was opened from there.
// The same object as podcast_screen; both names remain because they say
// different things to the reader.
extern lv_obj_t *podcast_list_screen;

// Opens a podcast's episode list. Called by "show podcast" in the track menu,
// which read the id from the sidecar file of the episode playing. False when it
// cannot be done (section disabled, no network).
bool podcastpage_open_feed(long long feed_id, const char *title);

// The podcast settings page, which holds the podcast sleep timer: what the
// control centre's sleep button for podcasts opens on a long press.
lv_obj_t *podcastpage_settings_screen(void);

// An episode was marked heard, or unmarked, by something other than this list --
// the player, when one ran to its end. The row showing it is bound again and
// only that row: the list sits behind the player's sheet rather than being
// unloaded, so no screen-load will come to redraw it, and the tick has to be
// there by the time the sheet goes away. Does nothing when this list is not the
// one on screen (entering it rebinds every row anyway).
void podcastpage_episode_heard(long long episode_id);

// Which way round episode lists run. The catalogue answers newest first; oldest
// first is the same list read backwards. Kept in the config because the Saved
// page's list of downloaded episodes follows the same setting: the order is a
// property of the reader, not of one screen.
bool podcastpage_episodes_newest_first(void);
void podcastpage_set_episodes_newest_first(bool newest);

// How the followed podcasts are ordered. Most listened first is the default and
// the one the list was asked for: the podcasts somebody keeps up with are the
// ones at the top, rather than whichever was followed most recently. The other
// two are the alphabet, either way round.
typedef enum {
	PODCAST_SORT_PLAYED, // most episodes started first
	PODCAST_SORT_AZ,
	PODCAST_SORT_ZA,
} podcast_sort_t;

podcast_sort_t podcastpage_subs_sort(void);
void podcastpage_set_subs_sort(podcast_sort_t order);

#endif /* PODCASTPAGE_H */
