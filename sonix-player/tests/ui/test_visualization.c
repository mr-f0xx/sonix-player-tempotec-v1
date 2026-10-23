/* Actual production drawing widget + PCM analyzer in a representative 240x320
 * player shell. The shell/text are a preview fixture, not a full player capture.
 * SONIX_VISUALIZATION_FRAMES writes optional PPM frames; no images needed to test.
 */
#include "src/gui/nowplaying/visualization.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lv_font_t font_ui_14;

static lv_obj_t *label(lv_obj_t *parent, const char *text, int y, lv_color_t color) {
	lv_obj_t *obj = lv_label_create(parent);
	lv_label_set_text(obj, text);
	lv_obj_set_style_text_color(obj, color, 0);
	lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
	return obj;
}
static void snapshot(lv_obj_t *screen, int mode, int n) {
	lv_obj_update_layout(screen);
	lv_draw_buf_t *shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
	assert(shot && shot->header.w == 240 && shot->header.h == 320);
	const char *dir = getenv("SONIX_VISUALIZATION_FRAMES");
	if (dir) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s-%03d.ppm", dir, mode == VISUALIZER_SPECTRUM ? "spectrum" : "waveform", n);
		FILE *f = fopen(path, "wb");
		assert(f);
		fprintf(f, "P6\n240 320\n255\n");
		for (unsigned y = 0; y < 320; y++) {
			const uint8_t *row = shot->data + y * shot->header.stride;
			for (unsigned x = 0; x < 240; x++) {
				uint8_t rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
				assert(fwrite(rgb, 3, 1, f) == 1);
			}
		}
		assert(fclose(f) == 0);
	}
	lv_draw_buf_destroy(shot);
}
int main(void) {
	font_ui_14 = lv_font_montserrat_14;
	lv_init();
	lv_display_create(240, 320);
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_remove_style_all(screen);
	lv_obj_set_size(screen, 240, 320);
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x101522), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_screen_load(screen);
	lv_obj_t *art = lv_obj_create(screen);
	lv_obj_remove_style_all(art);
	lv_obj_set_size(art, 240, 166);
	lv_obj_t *view = visualization_create(art);
	assert(view);
	lv_obj_set_hidden(view, false);
	lv_color_t accent = lv_color_hex(0x7aa2f7);
	lv_obj_t *heading = label(screen, "SPECTRUM", 12, accent);
	label(screen, "Synthetic audio preview", 139, lv_color_hex(0x8994af));
	label(screen, "Sonix Player", 181, lv_color_hex(0xc0caf5));
	label(screen, "TempoTec V1", 203, lv_color_hex(0x8994af));
	lv_obj_t *bar = lv_bar_create(screen);
	lv_obj_set_size(bar, 204, 4);
	lv_obj_set_pos(bar, 18, 235);
	lv_bar_set_value(bar, 38, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(bar, accent, LV_PART_INDICATOR);
	label(screen, "1:24                    3:42", 247, lv_color_hex(0x8994af));
	label(screen, LV_SYMBOL_PREV "      " LV_SYMBOL_PAUSE "      " LV_SYMBOL_NEXT, 283, accent);
	int16_t pcm[VISUALIZER_SAMPLES];
	for (int mode = VISUALIZER_SPECTRUM; mode <= VISUALIZER_WAVEFORM; mode++) {
		lv_label_set_text(heading, mode == VISUALIZER_SPECTRUM ? "SPECTRUM" : "LIVE WAVEFORM");
		for (int n = 0; n < 45; n++) {
			// Re-enable to inject a deterministic fresh window, independent of
			// host wall-clock speed and the production 15 Hz capture budget.
			visualizer_enable(false);
			visualizer_enable(true);
			double t = n / 15.0;
			for (int i = 0; i < VISUALIZER_SAMPLES; i++) {
				double v = 0;
				for (int k = 0; k < 9; k++) {
					double hz = 187.5 * pow(1.5, k);
					double gain = (0.5 + 0.5 * sin(t * (2 + k * 0.3) + k)) / 12;
					v += gain * sin(2 * M_PI * hz * (t + i / 48000.0));
				}
				pcm[i] = (int16_t)(v * 32767);
			}
			visualizer_feed(pcm, VISUALIZER_SAMPLES, 1, 48000, 16);
			visualizer_frame_t frame;
			assert(visualizer_read((visualizer_mode_t)mode, &frame));
			visualization_update(view, (visualizer_mode_t)mode, &frame, accent);
			snapshot(screen, mode, n);
		}
	}
	visualization_update(view, VISUALIZER_WAVEFORM, NULL, accent);
	snapshot(screen, VISUALIZER_WAVEFORM, 45); // idle, no fabricated motion
	lv_obj_set_hidden(view, true);
	assert(lv_obj_is_hidden(view));
	lv_screen_load(lv_obj_create(NULL));
	lv_obj_delete(screen); // exercises widget-owned state cleanup
	visualizer_enable(false);
	puts("PASS live visualization: 240x320, spectrum/waveform rendering, idle, hidden, cleanup");
	return 0;
}
