#include "videoplayer_ui.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/panel.h"
#include "src/system/video/videoplayer.h"

lv_obj_t *videoplayer_screen;

static gui_config_t *ui_cfg;
static lv_timer_t *tick_timer;
static bool overlay_visible;
static uint32_t overlay_hide_ms;
static bool ui_suspended;
static bool was_playing_before_suspend;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static uint32_t now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static void show_overlay(void) {
	overlay_visible = true;
	overlay_hide_ms = now_ms() + 3000;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline void blend_pixel(uint16_t *dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	if (a == 255) { *dst = rgb565(r, g, b); return; }
	if (a == 0) return;
	uint16_t c = *dst;
	uint8_t dr = (c >> 11) & 0x1F;
	uint8_t dg = (c >> 5) & 0x3F;
	uint8_t db = c & 0x1F;
	// expand to 8-bit approx
	uint8_t dr8 = (dr << 3) | (dr >> 2);
	uint8_t dg8 = (dg << 2) | (dg >> 4);
	uint8_t db8 = (db << 3) | (db >> 2);
	uint16_t ia = 255 - a;
	uint8_t or_ = (uint8_t)((r * a + dr8 * ia) / 255);
	uint8_t og_ = (uint8_t)((g * a + dg8 * ia) / 255);
	uint8_t ob_ = (uint8_t)((b * a + db8 * ia) / 255);
	*dst = rgb565(or_, og_, ob_);
}

static void fill_rect_alpha(uint16_t *buf, int pw, int ph, int stride,
			    int x, int y, int w, int h,
			    uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + w; if (x1 > pw) x1 = pw;
	int y1 = y + h; if (y1 > ph) y1 = ph;
	for (int yy = y0; yy < y1; yy++) {
		uint16_t *row = buf + yy * stride;
		for (int xx = x0; xx < x1; xx++) {
			blend_pixel(&row[xx], r, g, b, a);
		}
	}
}

// Tiny 5x7 font for digits: 0-9, ':', '/', '-', ' '
static const uint8_t FONT5X7[][5] = {
	{0x3E,0x51,0x49,0x45,0x3E}, // 0
	{0x00,0x42,0x7F,0x40,0x00}, // 1
	{0x42,0x61,0x51,0x49,0x46}, // 2
	{0x21,0x41,0x45,0x4B,0x31}, // 3
	{0x18,0x14,0x12,0x7F,0x10}, // 4
	{0x27,0x45,0x45,0x45,0x39}, // 5
	{0x3C,0x4A,0x49,0x49,0x30}, // 6
	{0x01,0x71,0x09,0x05,0x03}, // 7
	{0x36,0x49,0x49,0x49,0x36}, // 8
	{0x06,0x49,0x49,0x29,0x1E}, // 9
	{0x00,0x36,0x36,0x00,0x00}, // :
	{0x20,0x10,0x08,0x04,0x02}, // /
	{0x08,0x08,0x08,0x08,0x08}, // -
	{0x00,0x00,0x00,0x00,0x00}, // space
};

static int glyph_index(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c == ':') return 10;
	if (c == '/') return 11;
	if (c == '-') return 12;
	return 13;
}

static void draw_char(uint16_t *buf, int pw, int ph, int stride, int x, int y, char c, uint8_t r, uint8_t g, uint8_t b) {
	int gi = glyph_index(c);
	for (int col = 0; col < 5; col++) {
		uint8_t bits = FONT5X7[gi][col];
		for (int row = 0; row < 7; row++) {
			if (bits & (1 << row)) {
				int px = x + col;
				int py = y + row;
				if (px >= 0 && px < pw && py >= 0 && py < ph) {
					buf[py * stride + px] = rgb565(r, g, b);
				}
			}
		}
	}
}

static void draw_text(uint16_t *buf, int pw, int ph, int stride, int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b) {
	int cx = x;
	for (const char *p = text; *p; p++) {
		draw_char(buf, pw, ph, stride, cx, y, *p, r, g, b);
		cx += 6;
	}
}

static void format_time(char *out, size_t outsz, double sec) {
	if (sec < 0) sec = 0;
	int s = (int)(sec + 0.5);
	int m = s / 60;
	s = s % 60;
	if (m > 99) m = 99;
	snprintf(out, outsz, "%02d:%02d", m, s);
}

// ---------------------------------------------------------------------------
// Overlay drawing (panel coordinates, 240x320 portrait)
// ---------------------------------------------------------------------------

static void overlay_draw(uint16_t *panel, int pw, int ph, int stride) {
	if (!overlay_visible) return;
	if (now_ms() > overlay_hide_ms) {
		overlay_visible = false;
		return;
	}

	const int bar_h = 44;
	const int y0 = ph - bar_h;
	// Semi-transparent background
	fill_rect_alpha(panel, pw, ph, stride, 0, y0, pw, bar_h, 0, 0, 0, 140);

	// Progress bar
	double pos = videoplayer_get_position();
	double dur = videoplayer_get_duration();
	double frac = (dur > 0.0) ? (pos / dur) : 0.0;
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	int pad = 8;
	int bar_y = y0 + 8;
	int bar_w = pw - pad * 2;
	int bar_h2 = 4;
	fill_rect_alpha(panel, pw, ph, stride, pad, bar_y, bar_w, bar_h2, 80, 80, 80, 200);
	int fill_w = (int)(bar_w * frac);
	if (fill_w > 0) {
		fill_rect_alpha(panel, pw, ph, stride, pad, bar_y, fill_w, bar_h2, 255, 255, 255, 230);
	}
	// Scrub knob
	int kx = pad + fill_w - 2;
	if (kx < pad) kx = pad;
	if (kx > pad + bar_w - 4) kx = pad + bar_w - 4;
	fill_rect_alpha(panel, pw, ph, stride, kx, bar_y - 2, 4, bar_h2 + 4, 255, 255, 255, 255);

	// Time text
	char tcur[16], tdur[16], tbuf[40];
	format_time(tcur, sizeof(tcur), pos);
	format_time(tdur, sizeof(tdur), dur);
	snprintf(tbuf, sizeof(tbuf), "%s/%s", tcur, tdur);
	draw_text(panel, pw, ph, stride, pad, y0 + 20, tbuf, 220, 220, 220);

	// Play/Pause indicator on right
	vp_state_t st = videoplayer_get_state();
	const char *icon = (st == VP_STATE_PAUSED) ? ">" : "||";
	int icon_w = (int)strlen(icon) * 6;
	draw_text(panel, pw, ph, stride, pw - pad - icon_w, y0 + 20, icon, 220, 220, 220);
}

// ---------------------------------------------------------------------------
// Screen / input
// ---------------------------------------------------------------------------

static void stop_and_exit(void) {
	ui_suspended = false;
	was_playing_before_suspend = false;
	if (tick_timer) {
		lv_timer_delete(tick_timer);
		tick_timer = NULL;
	}
	videoplayer_set_overlay_callback(NULL);
	videoplayer_stop();
	display_video_leave();
	back_btn_force_hidden(false);
	topbar_set_hidden(false);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	stop_and_exit();
}

static void screen_clicked_cb(lv_event_t *e) {
	(void)e;
	if (!overlay_visible) {
		show_overlay();
		return;
	}
	// When overlay visible, a tap toggles play/pause and keeps overlay up
	videoplayer_toggle_pause();
	show_overlay();
}

static void tick_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != videoplayer_screen) return;
	if (ui_suspended) return;

	videoplayer_tick();

	// Auto-exit when playback ends
	vp_state_t st = videoplayer_get_state();
	if (st == VP_STATE_STOPPED || st == VP_STATE_ERROR) {
		// If we reached end naturally (not user stop), exit
		if (!videoplayer_is_active()) {
			stop_and_exit();
			back_btn_cb(NULL);
			return;
		}
	}
}

void videoplayer_ui_suspend(void) {
	if (ui_suspended) return;
	if (lv_screen_active() != videoplayer_screen) return;
	if (!videoplayer_is_active() && !display_video_supported()) return;

	was_playing_before_suspend = (videoplayer_get_state() == VP_STATE_PLAYING);
	if (was_playing_before_suspend) {
		videoplayer_pause();
	}
	videoplayer_set_overlay_callback(NULL);
	display_video_leave();
	ui_suspended = true;
}

void videoplayer_ui_resume(void) {
	if (!ui_suspended) return;
	if (lv_screen_active() != videoplayer_screen) {
		ui_suspended = false;
		was_playing_before_suspend = false;
		return;
	}
	ui_suspended = false;
	if (display_video_enter()) {
		videoplayer_set_overlay_callback(overlay_draw);
		show_overlay();
		if (was_playing_before_suspend) {
			videoplayer_resume();
		}
	}
	was_playing_before_suspend = false;
}

bool videoplayer_ui_is_suspended(void) { return ui_suspended; }

bool videoplayer_ui_open(const char *path) {
	if (!path || !path[0]) return false;
	if (!display_video_supported()) {
		fprintf(stderr, "videoplayer_ui: video not supported on this display\n");
		return false;
	}

	// Stop any existing video
	stop_and_exit();

	// Hide shell chrome
	topbar_set_hidden(true);
	back_btn_force_hidden(true);

	// Load black screen
	lv_obj_clean(videoplayer_screen);
	lv_obj_set_style_bg_color(videoplayer_screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(videoplayer_screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(videoplayer_screen, false);
	lv_obj_add_event_cb(videoplayer_screen, screen_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(videoplayer_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(videoplayer_screen);

	switch_screen(videoplayer_screen);

	// Enter direct fb video mode and start playback
	if (!display_video_enter()) {
		back_btn_force_hidden(false);
		topbar_set_hidden(false);
		return false;
	}

	videoplayer_set_overlay_callback(overlay_draw);
	show_overlay();

	if (!videoplayer_play(path)) {
		videoplayer_set_overlay_callback(NULL);
		display_video_leave();
		back_btn_force_hidden(false);
		topbar_set_hidden(false);
		return false;
	}

	if (tick_timer) lv_timer_delete(tick_timer);
	tick_timer = lv_timer_create(tick_timer_cb, 33, NULL);
	return true;
}

void videoplayer_ui_init(gui_config_t *cfg) {
	ui_cfg = cfg;
	if (!videoplayer_screen) {
		videoplayer_screen = lv_obj_create(NULL);
	}
	lv_obj_set_style_bg_color(videoplayer_screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(videoplayer_screen, LV_OPA_COVER, 0);
}
