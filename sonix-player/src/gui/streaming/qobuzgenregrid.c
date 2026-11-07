#include "qobuzgenregrid.h"

#include <stdint.h>

#include "qobuzgenre_icons.h"
#include "qobuzgenreorder.h"
#include "src/gui/board_profile.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"

#define GENRE_GAP 4

// Qobuz returns localised names, so choose glyphs by stable API ids instead
// of matching English text. Unknown/new genres still get a music glyph.
const lv_image_dsc_t *qobuzgenre_icon(long id, long reggae_id) {
    if (reggae_id > 0 && id == reggae_id) return &qobuz_genre_reggae;
    switch (id) {
    case 80: return &qobuz_genre_jazz;
    case 10: return &qobuz_genre_classical;
    case 133: return &qobuz_genre_hiphop;
    case 112: return &qobuz_genre_rock;
    case 64: return &qobuz_genre_electronic;
    case 127: return &qobuz_genre_soul;
    case 116: return &qobuz_genre_metal;
    case 5: return &qobuz_genre_folk;
    case 4: return &qobuz_genre_country;
    case 3: return &qobuz_genre_blues;
    case 149: return &qobuz_genre_latin;
    case 91: return &qobuz_genre_soundtrack;
    case 94: return &qobuz_genre_world;
    case 59: return &qobuz_genre_comedy;
    default: return &icon_music2;
    }
}

void qobuzgenre_grid_layout(lv_obj_t *container, bool grid) {
    lv_obj_set_flex_flow(container, grid ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          grid ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(container, grid ? GENRE_GAP : bp_pick(4, 8), 0);
}

lv_obj_t *qobuzgenre_grid_add(lv_obj_t *container, int content_width, long genre_id,
                              const char *name, const char *slug, int index, long reggae_id, lv_event_cb_t clicked) {
    int width = (content_width - GENRE_GAP) / 2;
    if (width < 1) width = 1;
    lv_obj_t *tile = lv_btn_create(container);
    lv_obj_set_size(tile, width, bp_pick(74, 150));
    lv_obj_add_style(tile, &theme_style_card, 0);
    lv_obj_add_style(tile, &theme_style_card_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_radius(tile, bp_tile_radius(), 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, bp_pick(4, 12), 0);
    lv_obj_set_style_pad_row(tile, bp_pick(2, 8), 0);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_event_bubble(tile, true);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(tile, clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    lv_obj_t *icon = lv_image_create(tile);
    const lv_image_dsc_t *source = qobuzgenre_is_blues_parent(name, slug)
        ? &qobuz_genre_blues : qobuzgenre_icon(genre_id, reggae_id);
    lv_image_set_src(icon, source);
    lv_obj_set_size(icon, 32, 32);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    // Unknown genres use the existing note, which is also 32x32.
    lv_obj_add_style(icon, &theme_style_icon, 0);
    lv_obj_set_style_image_recolor(icon, theme()->accent, 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_event_bubble(icon, true);

    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, name ? name : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, lv_pct(100));
    int label_height = 2 * lv_font_get_line_height(&font_ui_14);
    int available = bp_pick(32, 70);
    lv_obj_set_height(label, label_height < available ? label_height : available);
    lv_obj_add_style(label, &theme_style_text, 0);
    lv_obj_set_style_text_font(label, &font_ui_14, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_event_bubble(label, true);
    return tile;
}

void qobuzgenre_grid_refresh(lv_obj_t *container) {
    if (!container) return;
    uint32_t count = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *tile = lv_obj_get_child(container, (int32_t)i);
        lv_obj_t *icon = lv_obj_get_child(tile, 0);
        if (icon) lv_obj_set_style_image_recolor(icon, theme()->accent, 0);
    }
}
