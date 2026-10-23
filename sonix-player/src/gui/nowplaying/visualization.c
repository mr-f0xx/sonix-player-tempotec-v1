#include "visualization.h"
#include <stdlib.h>
#include <string.h>

// Presentation only. No timer, blur, extra FFT or work on the audio thread.
// Animation advances only when the player supplies a fresh PCM-derived frame.
typedef struct {
	visualizer_mode_t mode;
	visualizer_frame_t frame;
	lv_color_t accent;
	uint16_t level[VISUALIZER_BANDS], peak[VISUALIZER_BANDS]; // Q8 display levels
	uint16_t hold_ms[VISUALIZER_BANDS];
	uint32_t last_tick;
	bool seeded;
} view_t;

static lv_color_t background(void) { return lv_color_hex(0x090c12); }

static void gradient(lv_draw_rect_dsc_t *rect, lv_color_t top, lv_color_t bottom,
                     lv_opa_t top_opa, lv_opa_t bottom_opa) {
	rect->bg_grad.dir = LV_GRAD_DIR_VER;
	rect->bg_grad.stops_count = 2;
	rect->bg_grad.stops[0] = (lv_grad_stop_t){top, top_opa, 0};
	rect->bg_grad.stops[1] = (lv_grad_stop_t){bottom, bottom_opa, 255};
}

static void draw_spectrum(lv_layer_t *layer, const lv_area_t *area, const view_t *view) {
	int w = lv_area_get_width(area), h = lv_area_get_height(area);
	int left = area->x1 + w / 10, width = w - 2 * (w / 10);
	int baseline = area->y1 + h * 3 / 4, span = h * 9 / 16;
	int gap = LV_MAX(3, w / 60), radius = LV_MAX(2, w / 100);
	lv_color_t tip = lv_color_mix(lv_color_white(), view->accent, 105);
	lv_color_t foot = lv_color_mix(view->accent, background(), 95);
	lv_draw_rect_dsc_t bar, marker, reflection;
	lv_draw_rect_dsc_init(&bar);
	lv_draw_rect_dsc_init(&marker);
	lv_draw_rect_dsc_init(&reflection);
	bar.radius = radius;
	gradient(&bar, tip, foot, LV_OPA_COVER, LV_OPA_COVER);
	marker.bg_color = lv_color_mix(lv_color_white(), view->accent, 160);
	marker.bg_opa = 190;
	marker.radius = 1;
	gradient(&reflection, view->accent, background(), 35, 0);
	for (int i = 0; i < VISUALIZER_BANDS; i++) {
		int x = left + width * i / VISUALIZER_BANDS;
		int end = left + width * (i + 1) / VISUALIZER_BANDS - gap;
		int height = span * view->level[i] / (255 * 256);
		// Tiny quiet dots instead of a graph axis or a full-height empty rail.
		if (height < 2) {
			lv_draw_rect_dsc_t dot = marker;
			dot.bg_color = view->accent;
			dot.bg_opa = 40;
			lv_area_t box = {x, baseline - 1, end, baseline};
			lv_draw_rect(layer, &dot, &box);
		} else {
			lv_area_t box = {x, baseline - height, end, baseline};
			lv_draw_rect(layer, &bar, &box);
			// A short, faint reflection, not a blur/shadow or another render layer.
			box = (lv_area_t){x, baseline + 4, end, baseline + 4 + LV_MIN(height / 4, h / 10)};
			lv_draw_rect(layer, &reflection, &box);
		}
		if (view->peak[i] > 4 * 256) {
			int peak_y = baseline - span * view->peak[i] / (255 * 256) - 5;
			lv_area_t box = {x, peak_y, end, peak_y + 1};
			lv_draw_rect(layer, &marker, &box);
		}
	}
}

static int32_t waveform_sample(const view_t *view, int point) {
	static const int weight[] = {1, 4, 6, 4, 1};
	int32_t sample = 0;
	// A five-point binomial display filter softens decimation jaggies; it
	// never blends separate PCM windows or writes back to the audio samples.
	for (int j = -2; j <= 2; j++) {
		int at = LV_CLAMP(0, point + j, VISUALIZER_POINTS - 1);
		sample += (int32_t)view->frame.wave[at] * weight[j + 2];
	}
	return sample / 16;
}

static void draw_waveform(lv_layer_t *layer, const lv_area_t *area, const view_t *view) {
	int w = lv_area_get_width(area), h = lv_area_get_height(area);
	int left = area->x1 + w / 12, width = w - 2 * (w / 12);
	int center = area->y1 + h / 2, amplitude = h / 3;
	int stroke = LV_MAX(1, w / 200);
	int32_t wave[VISUALIZER_POINTS], range = 8192;
	for (int i = 0; i < VISUALIZER_POINTS; i++) {
		wave[i] = waveform_sample(view, i);
		int32_t magnitude = wave[i] < 0 ? -wave[i] : wave[i];
		if (magnitude > range) range = magnitude;
	}
	// Display-only gain, capped at 4x: quiet passages still get smaller and
	// digital silence remains a flat line. No clipping or audio modification.
	lv_color_t trace = lv_color_mix(lv_color_white(), view->accent, 85);
	lv_draw_line_dsc_t line;
	lv_draw_line_dsc_init(&line);
	line.round_start = line.round_end = true;
	lv_draw_rect_dsc_t fill;
	lv_draw_rect_dsc_init(&fill);
	fill.bg_color = view->accent;
	fill.bg_opa = 16;
	// Center on a rising zero crossing when one is near the start. This makes
	// periodic signals readable without inventing samples or changing audio.
	int start = 0;
	for (int i = 1; i < VISUALIZER_POINTS / 6; i++) {
		if (view->frame.wave[i - 1] <= 0 && view->frame.wave[i] > 0) { start = i; break; }
	}
	for (int i = 1; i < VISUALIZER_POINTS; i++) {
		int a = start + (i - 1) * (VISUALIZER_POINTS - 1 - start) / (VISUALIZER_POINTS - 1);
		int b = start + i * (VISUALIZER_POINTS - 1 - start) / (VISUALIZER_POINTS - 1);
		int x1 = left + width * (i - 1) / (VISUALIZER_POINTS - 1);
		int x2 = left + width * i / (VISUALIZER_POINTS - 1);
		int y1 = center - wave[a] * amplitude / range, y2 = center - wave[b] * amplitude / range;
		int fade = LV_MIN(255, LV_MIN(i, VISUALIZER_POINTS - i) * 32);
		if (y1 != center && x2 > x1) {
			lv_area_t box = {x1, LV_MIN(y1, center), x2 - 1, LV_MAX(y1, center)};
			fill.bg_opa = fade / 14;
			lv_draw_rect(layer, &fill, &box);
		}
		line.p1 = (lv_point_precise_t){x1, y1};
		line.p2 = (lv_point_precise_t){x2, y2};
		// Fixed-width translucent under-stroke: a cheap optical halo, no blur.
		line.color = view->accent;
		line.width = stroke * 5;
		line.opa = fade / 10;
		lv_draw_line(layer, &line);
		line.color = trace;
		line.width = stroke * 2;
		line.opa = fade;
		lv_draw_line(layer, &line);
	}
}

static void draw_cb(lv_event_t *e) {
	view_t *view = lv_event_get_user_data(e);
	lv_area_t area;
	lv_obj_get_content_coords(lv_event_get_target(e), &area);
	// A resized panel must never submit reversed/degenerate rectangles.
	if (lv_area_get_width(&area) < 96 || lv_area_get_height(&area) < 32) return;
	if (view->mode == VISUALIZER_SPECTRUM) draw_spectrum(lv_event_get_layer(e), &area, view);
	else if (view->mode == VISUALIZER_WAVEFORM) draw_waveform(lv_event_get_layer(e), &area, view);
}

static void delete_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

lv_obj_t *visualization_create(lv_obj_t *parent) {
	view_t *view = calloc(1, sizeof(*view));
	if (!view) return NULL;
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(obj, background(), 0);
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
	view_t *view = lv_event_dsc_get_user_data(lv_obj_get_event_dsc(obj, 0));
	if (view->mode != mode || !frame) {
		memset(view->level, 0, sizeof(view->level));
		memset(view->peak, 0, sizeof(view->peak));
		memset(view->hold_ms, 0, sizeof(view->hold_ms));
		view->seeded = false;
	}
	uint32_t dt = view->seeded ? LV_CLAMP(1, lv_tick_elaps(view->last_tick), 250) : 67;
	view->last_tick = lv_tick_get();
	view->mode = mode;
	view->accent = accent;
	if (frame) {
		view->frame = *frame;
		for (int i = 0; i < VISUALIZER_BANDS; i++) {
			uint32_t target = (uint32_t)frame->bands[i] * 256;
			uint32_t old = view->level[i];
			// Fast attack, 250 ms release; elapsed-time based at lower frame rates.
			view->level[i] = !view->seeded ? target : target > old ?
				old + (target - old) * LV_MIN(dt, 45) / 45 : old - (old - target) * dt / 250;
			if (view->level[i] >= view->peak[i]) {
				view->peak[i] = view->level[i];
				view->hold_ms[i] = 280;
			} else {
				uint32_t falling = dt > view->hold_ms[i] ? dt - view->hold_ms[i] : 0;
				view->hold_ms[i] = dt >= view->hold_ms[i] ? 0 : view->hold_ms[i] - dt;
				uint32_t drop = falling * 20; // ~78 display-level units/s, Q8
				view->peak[i] = LV_MAX(view->level[i], drop >= view->peak[i] ? 0 : view->peak[i] - drop);
			}
		}
		view->seeded = true;
	} else memset(&view->frame, 0, sizeof(view->frame));
	lv_obj_invalidate(obj);
}
