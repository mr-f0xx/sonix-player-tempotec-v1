#if defined(HOST_WEB_EMU) && defined(__EMSCRIPTEN__)

#include "webemu.h"

#include <emscripten.h>
#include <errno.h>
#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Emscripten's libc omits settimeofday(); clock.c falls back cleanly on EPERM. */
int settimeofday(const struct timeval *tv, const struct timezone *tz) {
	(void)tv;
	(void)tz;
	errno = EPERM;
	return -1;
}

#include "src/system/device/system.h"

#define PTR_QUEUE_CAP 64
#define KEY_QUEUE_CAP 64
#define FB_RING_SLOTS 4

typedef struct {
	int x;
	int y;
	bool pressed;
} ptr_event_t;

typedef struct {
	int code;
	int value;
} key_event_t;

static int fb_width;
static int fb_height;
static uint16_t *fb_pixels;
static uint16_t *fb_draw_buf;
static uint16_t *fb_ring[FB_RING_SLOTS];
static uint32_t fb_ring_idx;
static uint16_t *fb_capture_buf;
static atomic_bool fb_rotated;
static pthread_mutex_t fb_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t ptr_lock = PTHREAD_MUTEX_INITIALIZER;
static ptr_event_t ptr_queue[PTR_QUEUE_CAP];
static int ptr_head;
static int ptr_tail;
static int ptr_count;
static int ptr_last_x;
static int ptr_last_y;
static bool ptr_last_pressed;

static pthread_mutex_t key_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t key_cond = PTHREAD_COND_INITIALIZER;
static key_event_t key_queue[KEY_QUEUE_CAP];
static int key_head;
static int key_tail;
static int key_count;

static atomic_int shot_requested;
static atomic_int quit_requested;
static atomic_int charge_power_pressed;
static atomic_int charge_power_released;
static atomic_int current_volume_pct = 50;
static atomic_int initialized_flag;

static void send_event_json(const char *json) {
	if (!json) {
		return;
	}
	char *copy = strdup(json);
	if (!copy) {
		return;
	}
	MAIN_THREAD_ASYNC_EM_ASM(
		{
			var s = UTF8ToString($0);
			_free($0);
			if (Module && Module._sonixBridge && Module._sonixBridge.onEvent) {
				Module._sonixBridge.onEvent(s);
			}
		},
		copy);
}

static void send_event_json_sync(const char *json) {
	if (!json) {
		return;
	}
	MAIN_THREAD_EM_ASM(
		{
			var s = UTF8ToString($0);
			if (Module && Module._sonixBridge && Module._sonixBridge.onEvent) {
				Module._sonixBridge.onEvent(s);
			}
		},
		json);
}

static void send_current_frame_locked(void) {
	if (!fb_pixels || fb_width <= 0 || fb_height <= 0) {
		return;
	}
	uint16_t *slot = fb_ring[fb_ring_idx++ & (FB_RING_SLOTS - 1)];
	if (!slot) {
		return;
	}
	const size_t px_count = (size_t)fb_width * (size_t)fb_height;
	if (atomic_load(&fb_rotated)) {
		for (size_t i = 0; i < px_count; i++) {
			slot[i] = fb_pixels[px_count - 1 - i];
		}
	} else {
		memcpy(slot, fb_pixels, px_count * sizeof(uint16_t));
	}
	MAIN_THREAD_ASYNC_EM_ASM(
		{
			if (Module && Module._sonixBridge && Module._sonixBridge.onFrame) {
				var byteLen = $1 * $2 * 2;
				var view = HEAPU8.subarray($0, $0 + byteLen);
				Module._sonixBridge.onFrame(view, $1, $2);
			}
		},
		(int)(uintptr_t)slot, fb_width, fb_height);
}

void webemu_init(void) {
	if (atomic_exchange(&initialized_flag, 1) != 0) {
		return;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	fprintf(stderr, "webemu: Emscripten WebAssembly bridge ready\n");
	send_event_json("{\"type\":\"ready\",\"board\":\"TempoTec V1\",\"runtime\":\"wasm\"}");
	webemu_notify_screen(true);
	webemu_notify_volume(atomic_load(&current_volume_pct));
}

void webemu_shutdown(void) {
	atomic_store(&quit_requested, 1);
	pthread_mutex_lock(&key_lock);
	pthread_cond_broadcast(&key_cond);
	pthread_mutex_unlock(&key_lock);
}

static void webemu_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
	if (fb_pixels && area) {
		int x1 = area->x1 < 0 ? 0 : area->x1;
		int y1 = area->y1 < 0 ? 0 : area->y1;
		int x2 = area->x2 >= fb_width ? fb_width - 1 : area->x2;
		int y2 = area->y2 >= fb_height ? fb_height - 1 : area->y2;
		int area_w = area->x2 - area->x1 + 1;
		int copy_w = x2 - x1 + 1;

		if (copy_w > 0 && y2 >= y1) {
			const uint16_t *src = (const uint16_t *)px_map;
			pthread_mutex_lock(&fb_lock);
			for (int y = y1; y <= y2; y++) {
				const uint16_t *src_row = src + (size_t)(y - area->y1) * (size_t)area_w + (size_t)(x1 - area->x1);
				uint16_t *dst_row = fb_pixels + (size_t)y * (size_t)fb_width + (size_t)x1;
				memcpy(dst_row, src_row, (size_t)copy_w * sizeof(uint16_t));
			}
			if (lv_display_flush_is_last(disp)) {
				send_current_frame_locked();
			}
			pthread_mutex_unlock(&fb_lock);
		}
	}
	lv_display_flush_ready(disp);
}

lv_display_t *webemu_init_display(int width, int height) {
	webemu_init();

	fb_width = width;
	fb_height = height;
	size_t bytes = (size_t)width * (size_t)height * sizeof(uint16_t);

	fb_pixels = calloc(1, bytes);
	fb_draw_buf = calloc(1, bytes);
	fb_capture_buf = calloc(1, bytes);
	for (int i = 0; i < FB_RING_SLOTS; i++) {
		fb_ring[i] = calloc(1, bytes);
	}
	if (!fb_pixels || !fb_draw_buf || !fb_capture_buf || !fb_ring[0]) {
		fprintf(stderr, "webemu: failed to allocate %zu-byte framebuffer\n", bytes);
		return NULL;
	}

	lv_display_t *disp = lv_display_create(width, height);
	if (!disp) {
		fprintf(stderr, "webemu: lv_display_create failed\n");
		return NULL;
	}

	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, fb_draw_buf, NULL, (uint32_t)bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, webemu_flush_cb);

	fprintf(stderr, "webemu: display ready (%dx%d RGB565)\n", width, height);
	return disp;
}

static void webemu_pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
	(void)indev;
	pthread_mutex_lock(&ptr_lock);
	if (ptr_count > 0) {
		ptr_event_t ev = ptr_queue[ptr_head];
		ptr_head = (ptr_head + 1) % PTR_QUEUE_CAP;
		ptr_count--;
		ptr_last_x = ev.x;
		ptr_last_y = ev.y;
		ptr_last_pressed = ev.pressed;
		data->continue_reading = (ptr_count > 0);
	} else {
		data->continue_reading = false;
	}
	int x = ptr_last_x;
	int y = ptr_last_y;
	if (atomic_load(&fb_rotated) && fb_width > 0 && fb_height > 0) {
		x = fb_width - 1 - x;
		y = fb_height - 1 - y;
	}
	data->point.x = x;
	data->point.y = y;
	data->state = ptr_last_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	pthread_mutex_unlock(&ptr_lock);
}

lv_indev_t *webemu_init_pointer(lv_display_t *disp) {
	lv_indev_t *indev = lv_indev_create();
	if (!indev) {
		return NULL;
	}
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, webemu_pointer_read_cb);
	lv_indev_set_display(indev, disp);
	return indev;
}

uint16_t *webemu_capture_frame(int *out_w, int *out_h) {
	if (!fb_pixels || !fb_capture_buf || fb_width <= 0 || fb_height <= 0) {
		return NULL;
	}
	size_t px_count = (size_t)fb_width * (size_t)fb_height;
	pthread_mutex_lock(&fb_lock);
	if (atomic_load(&fb_rotated)) {
		for (size_t i = 0; i < px_count; i++) {
			fb_capture_buf[i] = fb_pixels[px_count - 1 - i];
		}
	} else {
		memcpy(fb_capture_buf, fb_pixels, px_count * sizeof(uint16_t));
	}
	pthread_mutex_unlock(&fb_lock);

	if (out_w) *out_w = fb_width;
	if (out_h) *out_h = fb_height;
	return fb_capture_buf;
}

void webemu_set_rotated(bool rotated) {
	atomic_store(&fb_rotated, rotated);
	char msg[96];
	snprintf(msg, sizeof(msg), "{\"type\":\"rotated\",\"rotated\":%s}", rotated ? "true" : "false");
	send_event_json(msg);
	pthread_mutex_lock(&fb_lock);
	send_current_frame_locked();
	pthread_mutex_unlock(&fb_lock);
}

bool webemu_take_screenshot_request(void) { return atomic_exchange(&shot_requested, 0) != 0; }

bool webemu_quit_requested(void) { return atomic_load(&quit_requested) != 0; }

void webemu_take_power_edges(bool *pressed, bool *released) {
	if (pressed) *pressed = atomic_exchange(&charge_power_pressed, 0) != 0;
	if (released) *released = atomic_exchange(&charge_power_released, 0) != 0;
}

void webemu_wasm_push_key_event(int code, bool down) {
	pthread_mutex_lock(&key_lock);
	if (key_count >= KEY_QUEUE_CAP) {
		key_head = (key_head + 1) % KEY_QUEUE_CAP;
		key_count--;
	}
	key_queue[key_tail].code = code;
	key_queue[key_tail].value = down ? 1 : 0;
	key_tail = (key_tail + 1) % KEY_QUEUE_CAP;
	key_count++;
	pthread_cond_signal(&key_cond);
	pthread_mutex_unlock(&key_lock);
}

int webemu_wasm_wait_key_event(int *out_code, int *out_value, int timeout_ms) {
	pthread_mutex_lock(&key_lock);
	while (key_count == 0 && !atomic_load(&quit_requested)) {
		if (timeout_ms < 0) {
			pthread_cond_wait(&key_cond, &key_lock);
		} else if (timeout_ms == 0) {
			pthread_mutex_unlock(&key_lock);
			return 0;
		} else {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += timeout_ms / 1000;
			ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			int rc = pthread_cond_timedwait(&key_cond, &key_lock, &ts);
			if (rc == ETIMEDOUT) {
				break;
			}
		}
	}
	if (atomic_load(&quit_requested)) {
		pthread_mutex_unlock(&key_lock);
		return -1;
	}
	if (key_count == 0) {
		pthread_mutex_unlock(&key_lock);
		return 0;
	}
	key_event_t ev = key_queue[key_head];
	key_head = (key_head + 1) % KEY_QUEUE_CAP;
	key_count--;
	pthread_mutex_unlock(&key_lock);
	if (out_code) *out_code = ev.code;
	if (out_value) *out_value = ev.value;
	return 1;
}

static int sgm_step_to_srgb(int step) {
	if (step <= 0) return 0;
	if (step >= 79) return 255;
	double norm = (double)step / 79.0;
	int val = (int)(pow(norm, 0.42) * 255.0 + 0.5);
	if (val < 28) val = 28;
	if (val > 255) val = 255;
	return val;
}

void webemu_notify_led(const char *mode, int r, int g, int b, int period, int duty, int fade) {
	const char *norm_mode = "off";
	if (mode) {
		if (strcmp(mode, "breathe") == 0 || strcmp(mode, "breathing") == 0) {
			norm_mode = "breathing";
		} else if (strcmp(mode, "steady") == 0) {
			norm_mode = "steady";
		}
	}

	int sr = sgm_step_to_srgb(r);
	int sg = sgm_step_to_srgb(g);
	int sb = sgm_step_to_srgb(b);

	int period_ms = 0;
	int rise_ms = 0;
	int fall_ms = 0;
	if (strcmp(norm_mode, "breathing") == 0) {
		period_ms = period > 0 ? period * 128 : 3200;
		if (period_ms < 800) period_ms = 800;
		int rise_code = fade & 0x0F;
		int fall_code = (fade >> 4) & 0x0F;
		rise_ms = rise_code > 0 ? rise_code * 110 : period_ms / 3;
		fall_ms = fall_code > 0 ? fall_code * 110 : period_ms / 3;
		if (rise_ms + fall_ms > period_ms) {
			rise_ms = period_ms / 2;
			fall_ms = period_ms / 2;
		}
	}

	char msg[320];
	snprintf(msg, sizeof(msg),
			 "{\"type\":\"led\",\"mode\":\"%s\",\"r\":%d,\"g\":%d,\"b\":%d,"
			 "\"r_step\":%d,\"g_step\":%d,\"b_step\":%d,"
			 "\"rise_ms\":%d,\"fall_ms\":%d,\"period_ms\":%d,\"duty\":%d}",
			 norm_mode, sr, sg, sb, r, g, b, rise_ms, fall_ms, period_ms, duty);
	send_event_json(msg);
}

void webemu_notify_backlight(long value, long max_val) {
	long m = max_val > 0 ? max_val : 255;
	long pct = (value * 100 + m / 2) / m;
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	char msg[160];
	snprintf(msg, sizeof(msg), "{\"type\":\"backlight\",\"value\":%ld,\"max\":%ld,\"percent\":%ld}",
			 value, m, pct);
	send_event_json(msg);
}

void webemu_notify_screen(bool on) {
	char msg[96];
	snprintf(msg, sizeof(msg), "{\"type\":\"screen\",\"on\":%s}", on ? "true" : "false");
	send_event_json(msg);
}

void webemu_notify_volume(int percent) {
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	atomic_store(&current_volume_pct, percent);
	char msg[96];
	snprintf(msg, sizeof(msg), "{\"type\":\"volume\",\"percent\":%d}", percent);
	send_event_json(msg);
}

void webemu_notify_power(const char *action) {
	char msg[128];
	snprintf(msg, sizeof(msg), "{\"type\":\"power\",\"action\":\"%s\"}", action ? action : "poweroff");
	send_event_json_sync(msg);
}

int webemu_get_volume_percent(void) { return atomic_load(&current_volume_pct); }

void webemu_send_audio_pcm(uint32_t sample_rate, uint16_t channels, const int16_t *samples, size_t frame_count) {
	if (!samples || frame_count == 0 || sample_rate == 0 || channels == 0 || channels > 2) {
		return;
	}
	size_t bytes = frame_count * channels * sizeof(int16_t);
	int16_t *copy = (int16_t *)malloc(bytes);
	if (!copy) {
		return;
	}
	memcpy(copy, samples, bytes);
	MAIN_THREAD_ASYNC_EM_ASM(
		{
			if (Module && Module._sonixBridge && Module._sonixBridge.onPcm) {
				var view = HEAPU8.slice($0, $0 + $1);
				Module._sonixBridge.onPcm(view, $2, $3);
			}
			_free($0);
		},
		(int)(uintptr_t)copy, (int)bytes, (int)sample_rate, (int)channels);
}

/* -------------------------------------------------------------------------
 * Exported JS -> WASM entry points (safe to call from the browser main thread)
 * ------------------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE
void webemu_wasm_pointer(int x, int y, int down) {
	if (fb_width > 0 && fb_height > 0) {
		if (x < 0) x = 0;
		if (x >= fb_width) x = fb_width - 1;
		if (y < 0) y = 0;
		if (y >= fb_height) y = fb_height - 1;
	}
	bool pressed = down != 0;
	pthread_mutex_lock(&ptr_lock);
	if (ptr_count > 0) {
		int last_idx = (ptr_tail + PTR_QUEUE_CAP - 1) % PTR_QUEUE_CAP;
		if (ptr_queue[last_idx].pressed == pressed && ptr_count >= 2) {
			int prev_idx = (last_idx + PTR_QUEUE_CAP - 1) % PTR_QUEUE_CAP;
			if (ptr_queue[prev_idx].pressed == pressed) {
				ptr_queue[last_idx].x = x;
				ptr_queue[last_idx].y = y;
				pthread_mutex_unlock(&ptr_lock);
				return;
			}
		}
	}
	if (ptr_count >= PTR_QUEUE_CAP) {
		ptr_head = (ptr_head + 1) % PTR_QUEUE_CAP;
		ptr_count--;
	}
	ptr_queue[ptr_tail].x = x;
	ptr_queue[ptr_tail].y = y;
	ptr_queue[ptr_tail].pressed = pressed;
	ptr_tail = (ptr_tail + 1) % PTR_QUEUE_CAP;
	ptr_count++;
	pthread_mutex_unlock(&ptr_lock);
}

static int button_code_for_name(const char *name) {
	if (!name) return -1;
	if (strcmp(name, "power") == 0) return KEY_POWER;
	if (strcmp(name, "vol_up") == 0) return KEY_VOLUMEUP;
	if (strcmp(name, "vol_down") == 0) return KEY_VOLUMEDOWN;
	if (strcmp(name, "prev") == 0) return KEY_NEXTSONG;
	if (strcmp(name, "play") == 0) return KEY_PLAYPAUSE;
	if (strcmp(name, "next") == 0) return KEY_PREVIOUSSONG;
	return -1;
}

EMSCRIPTEN_KEEPALIVE
void webemu_wasm_key(const char *name, int down) {
	int code = button_code_for_name(name);
	if (code < 0) {
		return;
	}
	if (code == KEY_POWER) {
		if (down) {
			atomic_store(&charge_power_pressed, 1);
		} else {
			atomic_store(&charge_power_released, 1);
		}
	}
	input_host_button(code, down != 0);
}

EMSCRIPTEN_KEEPALIVE
void webemu_wasm_screenshot(void) { atomic_store(&shot_requested, 1); }

EMSCRIPTEN_KEEPALIVE
void webemu_wasm_quit(void) { webemu_shutdown(); }

#endif /* HOST_WEB_EMU && __EMSCRIPTEN__ */
