# The one LVGL change this firmware keeps: lv_obj_init_draw_image_dsc() asks
# the app whether an object's idle recolor should give way to the pixels of an
# SD icon set (src/gui/shell/iconset.c). Stock LVGL doesn't know about that, so
# the patch in patches/ is applied to a fresh clone on first build; grepping
# for the hook's name makes the step idempotent for every build after that.
#
# Included by this Makefile and by tests/Makefile; both set LVGL_DIR first.
# The hook itself is a weak symbol, so a tree built without the patch still
# links and runs - it just never shows icon-set artwork.
LVGL_PATCH := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))patches/lvgl-iconset-recolor.patch)
ifneq ($(wildcard $(LVGL_DIR)/src/core/lv_obj_draw.c),)
LVGL_PATCHED := $(shell grep -q sonix_iconset_recolor_bypass $(LVGL_DIR)/src/core/lv_obj_draw.c 2>/dev/null || { git -C $(LVGL_DIR) apply $(LVGL_PATCH) 2>/dev/null || patch -p1 -d $(LVGL_DIR) < $(LVGL_PATCH); } && echo yes)
ifneq ($(LVGL_PATCHED),yes)
$(error could not apply $(LVGL_PATCH) to $(LVGL_DIR); delete that folder and rebuild to re-clone LVGL)
endif
endif
