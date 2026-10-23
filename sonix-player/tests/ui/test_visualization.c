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
typedef struct { uint64_t hash; unsigned lit; } pixels_t;
static pixels_t pixels(lv_obj_t *obj) {
	lv_obj_update_layout(obj);
	lv_draw_buf_t *shot = lv_snapshot_take(obj, LV_COLOR_FORMAT_RGB888);
	assert(shot);
	pixels_t result = {1469598103934665603ULL, 0};
	for (uint32_t y = 0; y < shot->header.h; y++) {
		const uint8_t *row = shot->data + y * shot->header.stride;
		for (uint32_t x = 0; x < shot->header.w * 3; x++) {
			result.hash = (result.hash ^ row[x]) * 1099511628211ULL;
			if (row[x] > 40) result.lit++;
		}
	}
	lv_draw_buf_destroy(shot);
	return result;
}

static void check_presentation(lv_obj_t *view, lv_obj_t *art, lv_color_t accent) {
	visualizer_frame_t full = {0}, quiet = {0};
	memset(full.bands, 255, sizeof(full.bands));
	visualization_update(view, VISUALIZER_SPECTRUM, NULL, accent);
	pixels_t idle = pixels(view);
	visualization_update(view, VISUALIZER_SPECTRUM, &full, accent);
	pixels_t high = pixels(view);
	assert(high.lit > idle.lit);
	lv_tick_inc(67);
	visualization_update(view, VISUALIZER_SPECTRUM, &quiet, accent);
	pixels_t falling = pixels(view);
	assert(falling.lit > idle.lit && falling.lit < high.lit); // smoothed release
	lv_tick_inc(2000);
	assert(pixels(view).hash == falling.hash); // rendering is stateless in time
	visualization_update(view, VISUALIZER_SPECTRUM, NULL, accent);
	assert(pixels(view).hash == idle.hash); // pause/Off discards all peaks immediately
	visualizer_frame_t saved = full;
	visualization_update(view, VISUALIZER_SPECTRUM, &full, accent);
	assert(memcmp(&full, &saved, sizeof(full)) == 0); // presentation never edits input
	visualization_update(view, VISUALIZER_WAVEFORM, &quiet, accent);
	pixels_t flat = pixels(view);
	for (int i = 0; i < VISUALIZER_POINTS; i++) full.wave[i] = i % 2 ? INT16_MIN : INT16_MAX;
	visualization_update(view, VISUALIZER_WAVEFORM, &full, accent);
	pixels(view); // full-scale extreme samples, including filtered -32768
	visualization_update(view, VISUALIZER_WAVEFORM, NULL, accent);
	assert(pixels(view).hash == flat.hash); // mode switch/reset cannot leave a ghost trace
	visualization_update(view, VISUALIZER_OFF, NULL, accent);
	assert(pixels(view).lit == 0);
	lv_obj_set_size(art, 64, 16);
	visualization_update(view, VISUALIZER_SPECTRUM, &full, accent);
	pixels(view); // no reversed rectangles on undersized panels
	lv_obj_set_size(art, 96, 32);
	visualization_update(view, VISUALIZER_WAVEFORM, &full, accent);
	pixels(view); // more waveform points than horizontal pixels
	lv_obj_set_size(art, 480, 360);
	pixels(view);
	visualization_update(view, VISUALIZER_WAVEFORM, &full, accent);
	pixels(view);
	lv_obj_set_size(art, 240, 166);
	visualization_update(view, VISUALIZER_OFF, NULL, accent);
}

int main(void) {
	font_ui_14 = lv_font_montserrat_14;
	lv_init();
	lv_display_create(240, 320);
	lv_obj_t *screen = lv_obj_create(NULL);
	lv_obj_remove_style_all(screen);
	lv_obj_set_size(screen, 240, 320);
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x090c12), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_screen_load(screen);
	lv_obj_t *art = lv_obj_create(screen);
	lv_obj_remove_style_all(art);
	lv_obj_set_size(art, 240, 166);
	lv_obj_t *view = visualization_create(art);
	assert(view);
	lv_obj_set_hidden(view, false);
	lv_color_t accent = lv_color_hex(0x7bdcc8);
	lv_color_t muted = lv_color_hex(0x717d8d);
	check_presentation(view, art, accent);
	lv_obj_t *heading = label(screen, "SPECTRUM", 14, muted);
	lv_obj_set_style_text_font(heading, &lv_font_montserrat_12, 0);
	lv_obj_set_style_text_letter_space(heading, 2, 0);
	lv_obj_t *title = label(screen, "Weightless", 176, lv_color_hex(0xe5eaf0));
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 176);
	lv_obj_t *artist = label(screen, "Marconi Union", 196, muted);
	lv_obj_align(artist, LV_ALIGN_TOP_LEFT, 20, 196);
	lv_obj_set_style_text_font(artist, &lv_font_montserrat_12, 0);
	lv_obj_t *quality = label(screen, "FLAC", 198, muted);
	lv_obj_align(quality, LV_ALIGN_TOP_RIGHT, -20, 198);
	lv_obj_set_style_text_font(quality, &lv_font_montserrat_12, 0);
	lv_obj_t *bar = lv_bar_create(screen);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, 200, 3);
	lv_obj_set_pos(bar, 20, 226);
	lv_obj_set_style_bg_color(bar, lv_color_hex(0x27303b), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(bar, 3, 0);
	lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
	lv_bar_set_value(bar, 38, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(bar, accent, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
	lv_obj_t *elapsed = label(screen, "1:24", 238, muted);
	lv_obj_set_style_text_font(elapsed, &lv_font_montserrat_12, 0);
	lv_obj_align(elapsed, LV_ALIGN_TOP_LEFT, 20, 238);
	lv_obj_t *total = label(screen, "3:42", 238, muted);
	lv_obj_set_style_text_font(total, &lv_font_montserrat_12, 0);
	lv_obj_align(total, LV_ALIGN_TOP_RIGHT, -20, 238);
	lv_obj_t *play = lv_obj_create(screen);
	lv_obj_remove_style_all(play);
	lv_obj_set_size(play, 44, 44);
	lv_obj_set_pos(play, 98, 270);
	lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(play, accent, 0);
	lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
	for (int i = 0; i < 2; i++) {
		lv_obj_t *stroke = lv_obj_create(play);
		lv_obj_remove_style_all(stroke);
		lv_obj_set_size(stroke, 4, 14);
		lv_obj_set_pos(stroke, 15 + i * 10, 15);
		lv_obj_set_style_bg_color(stroke, lv_color_hex(0x090c12), 0);
		lv_obj_set_style_bg_opa(stroke, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(stroke, 1, 0);
	}
	lv_obj_t *previous = label(screen, LV_SYMBOL_PREV, 284, lv_color_hex(0xc1cad5));
	lv_obj_align(previous, LV_ALIGN_TOP_LEFT, 57, 284);
	lv_obj_t *next = label(screen, LV_SYMBOL_NEXT, 284, lv_color_hex(0xc1cad5));
	lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -57, 284);
	int16_t pcm[VISUALIZER_SAMPLES];
	for (int mode = VISUALIZER_SPECTRUM; mode <= VISUALIZER_WAVEFORM; mode++) {
		lv_label_set_text(heading, mode == VISUALIZER_SPECTRUM ? "SPECTRUM" : "WAVEFORM");
		for (int n = 0; n < 60; n++) {
			// Re-enable to inject a deterministic fresh window, independent of
			// host wall-clock speed and the production 15 Hz capture budget.
			visualizer_enable(false);
			visualizer_enable(true);
			lv_tick_inc(67);
			double t = n / 15.0;
			for (int i = 0; i < VISUALIZER_SAMPLES; i++) {
				// Bass-led musical test signal, with moving upper harmonics.
				double v = 0.30 * (0.7 + 0.3 * sin(t * 3)) *
					sin(2 * M_PI * 187.5 * (t + i / 48000.0));
				for (int k = 0; k < 9; k++) {
					double hz = 187.5 * pow(1.5, k);
					double gain = (0.5 + 0.5 * sin(t * (2 + k * 0.3) + k)) / 40;
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
	snapshot(screen, VISUALIZER_WAVEFORM, 60); // idle, no fabricated motion
	lv_obj_set_hidden(view, true);
	assert(lv_obj_is_hidden(view));
	lv_screen_load(lv_obj_create(NULL));
	lv_obj_delete(screen); // exercises widget-owned state cleanup
	visualizer_enable(false);
	puts("PASS live visualization: rendering, release/peaks, resets, no autonomous animation, bounds, cleanup");
	return 0;
}
