#ifndef BROWSERSTATE_H
#define BROWSERSTATE_H

#include <stdbool.h>
#include <stddef.h>

// The browser's folder and scroll offset are stored separately from the INI:
// paths can be longer than a config value and may contain newlines.
bool browser_state_save(const char *file, const char *folder, int scroll);
bool browser_state_load(const char *file, char *folder, size_t size, int *scroll);

// Accept only existing directories inside the card, including through symlinks.
bool browser_state_directory(const char *root, const char *folder, char *out, size_t size);
// Choose saved folder, local track's parent, Music, then card root. `track` is
// an ordinary local file path (CUE virtual paths must be split by the caller).
bool browser_state_choose(const char *root, const char *saved, int saved_scroll,
                          const char *track, char *out, size_t size, int *scroll);
#endif
