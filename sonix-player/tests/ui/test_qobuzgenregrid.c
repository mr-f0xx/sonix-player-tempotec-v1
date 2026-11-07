#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/streaming/qobuzgenregrid.h"
#include "src/gui/streaming/qobuzgenre_icons.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/gui/fonts/fonts.h"

lv_style_t theme_style_card, theme_style_card_pressed, theme_style_text, theme_style_icon;
lv_font_t font_ui_18, font_ui_14;
const lv_image_dsc_t icon_music2 = {0};
static theme_palette_t palette;
const theme_palette_t *theme(void) { return &palette; }
int bp_pick(int compact, int regular) { (void)regular; return compact; }
int bp_tile_radius(void) { return 10; }
static int clicked = -1;
static void click_cb(lv_event_t *e) { clicked = (int)(intptr_t)lv_event_get_user_data(e); }

int main(void) {
    lv_init();
    font_ui_14 = lv_font_montserrat_14;
    lv_display_create(240, 320);
    lv_style_init(&theme_style_card);
    lv_style_init(&theme_style_card_pressed);
    lv_style_init(&theme_style_text);
    lv_style_init(&theme_style_icon);
    font_ui_18 = lv_font_montserrat_14;
    palette.accent = lv_color_hex(0xff9944);

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, 240, 320);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_set_size(list, 240, 248);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_hor(list, 6, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    qobuzgenre_grid_layout(list, true);
    const long ids[] = {112, 80, 10, 133, 64};
    const char *names[] = {"Pop/Rock", "Jazz", "Classique", "Hip-Hop/Rap", "Electronic"};
    for (int i = 0; i < 4; i++) qobuzgenre_grid_add(list, 228, ids[i], names[i], i, 1234, click_cb);
    lv_obj_update_layout(screen);

    lv_obj_t *a = lv_obj_get_child(list, 0), *b = lv_obj_get_child(list, 1);
    lv_obj_t *c = lv_obj_get_child(list, 2), *d = lv_obj_get_child(list, 3);
    assert(lv_obj_get_width(a) == 111 && lv_obj_get_height(a) == 92);
    assert(lv_obj_get_x(a) < lv_obj_get_x(b) && lv_obj_get_y(a) == lv_obj_get_y(b));
    assert(lv_obj_get_y(c) > lv_obj_get_y(a) && lv_obj_get_y(c) == lv_obj_get_y(d));
    assert(lv_obj_get_x(a) == lv_obj_get_x(c) && lv_obj_get_x(b) == lv_obj_get_x(d));
    assert(lv_obj_get_y(d) + lv_obj_get_height(d) <= lv_obj_get_content_height(list));
    assert(lv_image_get_src(lv_obj_get_child(b, 0)) == &qobuz_genre_jazz);
    assert(lv_image_get_src(lv_obj_get_child(c, 0)) == &qobuz_genre_classical);
    assert(lv_image_get_src(lv_obj_get_child(d, 0)) == &qobuz_genre_hiphop);
    assert(qobuzgenre_icon(99999, 1234) == &icon_music2);
    assert(qobuzgenre_icon(1234, 1234) == &qobuz_genre_reggae);
    lv_obj_send_event(c, LV_EVENT_CLICKED, NULL);
    assert(clicked == 2);

    // Appending a later page retains the first four and their click indices.
    qobuzgenre_grid_add(list, 228, ids[4], names[4], 4, 1234, click_cb);
    lv_obj_update_layout(screen);
    assert(lv_obj_get_child_count(list) == 5);
    lv_obj_t *fifth = lv_obj_get_child(list, 4);
    assert(lv_obj_get_y(fifth) > lv_obj_get_y(c));
    lv_obj_send_event(fifth, LV_EVENT_CLICKED, NULL);
    assert(clicked == 4);
    palette.accent = lv_color_hex(0x11bb77);
    qobuzgenre_grid_refresh(list);
    assert(lv_color_eq(lv_obj_get_style_image_recolor(lv_obj_get_child(fifth, 0), 0), palette.accent));

    // A long localised genre name is confined even with larger font metrics.
    font_ui_18 = lv_font_montserrat_20;
    lv_obj_t *long_tile = qobuzgenre_grid_add(list, 228, 59,
        "Very long translated genre and subgenre name", 5, 1234, click_cb);
    lv_obj_update_layout(screen);
    lv_obj_t *label = lv_obj_get_child(long_tile, 1);
    assert(lv_obj_get_width(label) <= lv_obj_get_content_width(long_tile));
    assert(lv_obj_get_height(label) <= lv_obj_get_content_height(long_tile) - 32);

    lv_obj_clean(list);
    qobuzgenre_grid_layout(list, false);
    lv_obj_t *row1 = lv_obj_create(list), *row2 = lv_obj_create(list);
    lv_obj_set_size(row1, lv_pct(100), 40);
    lv_obj_set_size(row2, lv_pct(100), 40);
    lv_obj_update_layout(screen);
    assert(lv_obj_get_y(row2) > lv_obj_get_y(row1));
    assert(lv_obj_get_x(row1) == lv_obj_get_x(row2));
    lv_obj_delete(screen);
    puts("PASS Qobuz genre grid: geometry, glyphs, clicks, append, theme and list restore");
    return 0;
}
