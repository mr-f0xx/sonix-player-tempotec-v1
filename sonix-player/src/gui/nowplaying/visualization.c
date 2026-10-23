#include "visualization.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
	visualizer_mode_t mode;
	visualizer_frame_t frame;
	lv_color_t accent;
} view_t;

static void draw_cb(lv_event_t *e) {
	view_t *view = lv_event_get_user_data(e);
	lv_obj_t *obj = lv_event_get_target(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_content_coords(obj, &a);
	int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
	int left = a.x1 + w / 12, right = a.x2 - w / 12;
	int top = a.y1 + h / 5, bottom = a.y2 - h / 5;
	lv_draw_line_dsc_t line;
	lv_draw_line_dsc_init(&line);
	line.color = view->accent;
	line.width = 1;
	line.opa = LV_OPA_20;
	for (int i = 0; i < 4; i++) {
		int y = top + (bottom - top) * i / 3;
		line.p1 = (lv_point_precise_t){left, y};
		line.p2 = (lv_point_precise_t){right, y};
		lv_draw_line(layer, &line);
	}
	if (view->mode == VISUALIZER_SPECTRUM) {
		lv_draw_rect_dsc_t bar;
		lv_draw_rect_dsc_init(&bar);
		bar.bg_color = view->accent;
		bar.bg_opa = LV_OPA_COVER;
		bar.radius = 2;
		for (int i = 0; i < VISUALIZER_BANDS; i++) {
			int x = left + (right - left + 1) * i / VISUALIZER_BANDS;
			int end = left + (right - left + 1) * (i + 1) / VISUALIZER_BANDS - 3;
			int height = (bottom - top) * view->frame.bands[i] / 255;
			lv_area_t box = {x, bottom - height, end, bottom};
			lv_draw_rect(layer, &bar, &box);
		}
	} else {
		line.width = 2;
		line.opa = LV_OPA_COVER;
		line.round_start = line.round_end = true;
		int mid = (top + bottom) / 2;
		for (int i = 1; i < VISUALIZER_POINTS; i++) {
			line.p1 = (lv_point_precise_t){left + (right - left) * (i - 1) / (VISUALIZER_POINTS - 1),
				mid - (int32_t)view->frame.wave[i - 1] * (bottom - top) / 65536};
			line.p2 = (lv_point_precise_t){left + (right - left) * i / (VISUALIZER_POINTS - 1),
				mid - (int32_t)view->frame.wave[i] * (bottom - top) / 65536};
			lv_draw_line(layer, &line);
		}
	}
}

static void delete_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

lv_obj_t *visualization_create(lv_obj_t *parent) {
	view_t *view = calloc(1, sizeof(*view));
	if (!view) return NULL; // playback and ordinary artwork still work
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(obj, lv_color_hex(0x080c16), 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_clickable(obj, false);
	lv_obj_set_scrollable(obj, false);
	lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, view);
	lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, view);
	lv_obj_set_hidden(obj, true);
	return obj;
}

void visualization_update(lv_obj_t *obj, visualizer_mode_t mode,
                          const visualizer_frame_t *frame, lv_color_t accent) {
	if (!obj) return;
	// Event-owned state, separate from obj user_data (reserved for callers).
	lv_event_dsc_t *dsc = lv_obj_get_event_dsc(obj, 0);
	view_t *view = lv_event_dsc_get_user_data(dsc);
	view->mode = mode;
	view->accent = accent;
	if (frame) view->frame = *frame;
	else memset(&view->frame, 0, sizeof(view->frame));
	lv_obj_invalidate(obj);
}
