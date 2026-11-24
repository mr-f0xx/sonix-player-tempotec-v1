#ifndef SDHEALTHPAGE_H
#define SDHEALTHPAGE_H

#include "src/gui/shell/gui.h"

// The read-only microSD health and 4K read-speed page under Settings -> System.
// The benchmark itself is a background worker (src/system/device/sdbench.c);
// this page only polls its result and paints it.
lv_obj_t *sdhealthpage_init(gui_config_t *cfg);

#endif /* SDHEALTHPAGE_H */
