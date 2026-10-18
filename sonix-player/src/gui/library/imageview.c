#include "imageview.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/cover.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

// Maximum image file size we'll try to load. Larger files are refused before
// any decode attempt, to keep the device responsive.
#define IMAGEVIEW_MAX_BYTES (32 * 1024 * 1024)

lv_obj_t *imageview_screen;

static lv_obj_t *title_label;
static lv_obj_t *image_obj;  // the image widget
static lv_obj_t *error_label;
static cover_image_t held_image;
static bool image_owned;

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void cleanup_image(void) {
    if (image_owned) {
        cover_free(&held_image);
        image_owned = false;
    }
    lv_obj_set_hidden(image_obj, true);
    lv_label_set_text(error_label, "");
    lv_obj_set_hidden(error_label, true);
}

static bool back_guard_cb(void) {
    // Always allow going back from the image viewer
    cleanup_image();
    return false; // let the switcher handle the actual navigation
}

bool imageview_open(const char *path) {
    if (!path || !path[0]) {
        return false;
    }

    // Check file size first
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    if (size <= 0 || (size_t)size > IMAGEVIEW_MAX_BYTES) {
        return false;
    }

    const char *slash = strrchr(path, '/');
    const char *title = slash ? slash + 1 : path;

    // Use the public cover API to load the image
    cover_image_t img;
    memset(&img, 0, sizeof(img));

    bool ok = cover_load_image_file(path, 240, 320, COVER_FIT_CONTAIN, &img);

    if (!ok) {
        // Show error
        lv_label_set_text(title_label, title);
        cleanup_image();
        lv_label_set_text(error_label, tr("imageview_decode_failed"));
        lv_obj_set_hidden(error_label, false);
        lv_obj_scroll_to_y(imageview_screen, 0, LV_ANIM_OFF);
        switch_screen(imageview_screen);
        return false;
    }

    // Success - show the image
    imageview_show_image(title, &img);
    cover_free(&img); // imageview_show_image copied the image data
    return true;
}

bool imageview_show_image(const char *title, cover_image_t *img) {
    if (!title || !img || !img->pixels) {
        return false;
    }

    // Clean up any previous image
    cleanup_image();

    // Set the title
    lv_label_set_text(title_label, title);

    // Take ownership of the image
    held_image.dsc = img->dsc;
    held_image.pixels = img->pixels;
    image_owned = true;

    // Show the image
    lv_obj_set_hidden(image_obj, false);
    lv_image_set_src(image_obj, &img->dsc);

    lv_obj_scroll_to_y(imageview_screen, 0, LV_ANIM_OFF);
    switch_screen(imageview_screen);
    return true;
}

void imageview_init(gui_config_t *cfg) {
    lv_obj_add_style(imageview_screen, &theme_style_screen, 0);
    title_label = settingsrow_title(imageview_screen, cfg, "");
    settingsrow_title_corner_slots(title_label, cfg, 0);

    int content_top = settingsrow_content_top(cfg);

    // Image widget
    image_obj = lv_image_create(imageview_screen);
    lv_obj_set_size(image_obj, lv_pct(100), cfg->screen_height - content_top);
    lv_obj_align(image_obj, LV_ALIGN_TOP_LEFT, 0, content_top);
    lv_obj_set_style_bg_color(image_obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(image_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(image_obj, 0, 0);
    lv_obj_set_style_radius(image_obj, 0, 0);
    lv_obj_set_style_pad_all(image_obj, cfg->padding, 0);
    lv_obj_set_hidden(image_obj, true);

    // Error label (hidden by default)
    error_label = lv_label_create(imageview_screen);
    lv_label_set_text(error_label, "");
    lv_obj_add_style(error_label, &theme_style_text_dim, 0);
    lv_obj_set_style_text_font(error_label, &font_ui_22, 0);
    lv_obj_set_width(error_label, lv_pct(100));
    lv_obj_align(error_label, LV_ALIGN_TOP_LEFT, cfg->padding, content_top + 60);
    lv_obj_set_hidden(error_label, true);

    // Back gesture
    switcher_attach_back_gesture(imageview_screen);
    switcher_set_back_guard(imageview_screen, back_guard_cb);
}
