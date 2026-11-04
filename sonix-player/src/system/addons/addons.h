#ifndef ADDONS_H
#define ADDONS_H

#include <stdbool.h>

// The optional modules of the player, as a list rather than a heap of #ifdefs.
//
// The player is one binary: a module's code is always linked in. What "off"
// means is that nothing of that module is *built* -- its pages are never
// created, its rows are not in the menus -- so the RAM those pages would hold
// stays free for the music. That is the memory that matters on a player with a
// fixed heap; flash is trimmed with the build instead (see docs/ADDONS.md).
//
// Each module is identified by a short id, which is also the key its switch is
// stored under in the config's [addons] section, so a switch survives updates
// and a module added later starts on.

typedef struct {
	const char *id;      // "tidal", "calculator": the config key and the gate
	const char *label;   // the row's name, a tag or a literal
	const char *note;    // one line on what it adds, a tag
	const char *section; // the Plugins page heading it belongs under, a tag
} addon_t;

// The modules this build carries, in the order the Plugins page lists them.
// Entries that share a section are drawn under one heading, so the list is
// ordered by section first.
int addons_count(void);
const addon_t *addons_at(int index);
const addon_t *addons_find(const char *id);

// Whether a module is switched on. Unknown ids answer true: a gate is a way to
// leave something out, and a misspelt one must not quietly remove a feature.
bool addons_enabled(const char *id);

// Switches one on or off and stores it. The caller says it needs a restart --
// a page that has already been built cannot give its memory back.
// False if the setting could not be stored, or the id is not a module.
bool addons_set(const char *id, bool on);

// Called once at start-up, before any page is built. A fresh install takes the
// defaults below (a lean set); a device that was already in use keeps every
// module it had, because before this switch existed they were all there. Once
// seeded, the choice is only ever changed from the Plugins page.
void addons_seed(bool fresh_install);

#endif /* ADDONS_H */
