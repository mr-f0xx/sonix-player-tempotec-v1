#ifndef GUI_PET_PETALARM_PAGE_H
#define GUI_PET_PETALARM_PAGE_H

#include "lvgl/lvgl.h"
#include "src/gui/shell/gui.h"

// A lazily built sheet over the pet's settings, and its local-playlist picker.
void petalarm_page_init(lv_obj_t *parent, gui_config_t *cfg, void (*changed)(void));
void petalarm_page_open(void);
// Close the topmost alarm sheet for the shell's back button; false if none.
bool petalarm_page_back(void);
void petalarm_page_hide(void);

#endif /* GUI_PET_PETALARM_PAGE_H */
