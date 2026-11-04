#ifndef SONIX_TEST_STUB_LVGL_H
#define SONIX_TEST_STUB_LVGL_H

/* Minimal declarations for plain-C tests of workers that include GUI
 * headers but never manipulate LVGL. Wi-Fi and USB ownership/storage tests
 * put this directory on their include path; the player and UI tests always
 * use real LVGL. */
typedef struct lv_display lv_display_t;
typedef struct lv_timer lv_timer_t;
typedef struct lv_obj lv_obj_t;
/* USB ownership tests include GUI declarations but do not manipulate LVGL. */
typedef struct { unsigned char dummy; } lv_color_t;
typedef struct { unsigned char dummy; } lv_image_dsc_t;

#endif /* SONIX_TEST_STUB_LVGL_H */
