#ifdef HOST_WEB_EMU

#include "webemu.h"

#include <errno.h>
#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "src/system/device/system.h"

#define WEBEMU_MSG_FRAME 1
#define WEBEMU_MSG_EVENT 2
#define WEBEMU_MSG_AUDIO 3

#define PTR_QUEUE_CAP 64

typedef struct {
	int x;
	int y;
	bool pressed;
} ptr_event_t;

static int sock_fd = -1;
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t reader_thread;
static bool reader_started;
static atomic_int shutting_down;

static int fb_width;
static int fb_height;
static uint16_t *fb_pixels;
static uint16_t *fb_draw_buf;
static uint16_t *fb_send_buf;
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

static atomic_int shot_requested;
static atomic_int quit_requested;
static atomic_int charge_power_pressed;
static atomic_int charge_power_released;
static atomic_int current_volume_pct = 50;

static void write_be16(uint8_t *dst, uint16_t v) {
	dst[0] = (uint8_t)((v >> 8) & 0xFFu);
	dst[1] = (uint8_t)(v & 0xFFu);
}

static void write_be32(uint8_t *dst, uint32_t v) {
	dst[0] = (uint8_t)((v >> 24) & 0xFFu);
	dst[1] = (uint8_t)((v >> 16) & 0xFFu);
	dst[2] = (uint8_t)((v >> 8) & 0xFFu);
	dst[3] = (uint8_t)(v & 0xFFu);
}

static bool send_all(int fd, const void *buf, size_t len) {
	const uint8_t *p = (const uint8_t *)buf;
	size_t left = len;
	while (left > 0) {
		ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		if (n == 0) {
			return false;
		}
		p += (size_t)n;
		left -= (size_t)n;
	}
	return true;
}

static void send_packet_parts(uint8_t type, const void *hdr, size_t hdr_len, const void *body, size_t body_len) {
	pthread_mutex_lock(&write_lock);
	if (sock_fd < 0) {
		pthread_mutex_unlock(&write_lock);
		return;
	}
	uint8_t prefix[5];
	prefix[0] = type;
	write_be32(prefix + 1, (uint32_t)(hdr_len + body_len));
	bool ok = send_all(sock_fd, prefix, sizeof(prefix));
	if (ok && hdr && hdr_len > 0) {
		ok = send_all(sock_fd, hdr, hdr_len);
	}
	if (ok && body && body_len > 0) {
		ok = send_all(sock_fd, body, body_len);
	}
	if (!ok) {
		int dead = sock_fd;
		sock_fd = -1;
		close(dead);
	}
	pthread_mutex_unlock(&write_lock);
}

static void send_event_json(const char *json) {
	if (!json) {
		return;
	}
	send_packet_parts(WEBEMU_MSG_EVENT, json, strlen(json), NULL, 0);
}

static void send_current_frame_locked(void) {
	if (!fb_pixels || fb_width <= 0 || fb_height <= 0) {
		return;
	}
	const size_t px_count = (size_t)fb_width * (size_t)fb_height;
	const uint16_t *src = fb_pixels;
	if (atomic_load(&fb_rotated) && fb_send_buf) {
		for (size_t i = 0; i < px_count; i++) {
			fb_send_buf[i] = fb_pixels[px_count - 1 - i];
		}
		src = fb_send_buf;
	}
	uint8_t dims[4];
	write_be16(dims, (uint16_t)fb_width);
	write_be16(dims + 2, (uint16_t)fb_height);
	send_packet_parts(WEBEMU_MSG_FRAME, dims, sizeof(dims), src, px_count * sizeof(uint16_t));
}

static void enqueue_pointer(int x, int y, bool pressed) {
	if (fb_width > 0 && fb_height > 0) {
		if (x < 0) x = 0;
		if (x >= fb_width) x = fb_width - 1;
		if (y < 0) y = 0;
		if (y >= fb_height) y = fb_height - 1;
	}

	pthread_mutex_lock(&ptr_lock);
	if (ptr_count > 0) {
		int last_idx = (ptr_tail + PTR_QUEUE_CAP - 1) % PTR_QUEUE_CAP;
		/* Coalesce consecutive move events while pressed/released to keep
		 * dragging ultra-low-latency without ever dropping press/release edges. */
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
	if (strcmp(name, "power") == 0) return KEY_POWER;
	if (strcmp(name, "vol_up") == 0) return KEY_VOLUMEUP;
	if (strcmp(name, "vol_down") == 0) return KEY_VOLUMEDOWN;
	/* TempoTec V1 swaps NEXTSONG / PREVIOUSSONG at the kernel layer; see
	 * HOST_BUTTONS in src/main.c and keymap_button_for_code(). */
	if (strcmp(name, "prev") == 0) return KEY_NEXTSONG;
	if (strcmp(name, "play") == 0) return KEY_PLAYPAUSE;
	if (strcmp(name, "next") == 0) return KEY_PREVIOUSSONG;
	return -1;
}

static void handle_command_line(char *line) {
	if (!line || !line[0]) {
		return;
	}
	if (line[0] == 'P' && line[1] == ' ') {
		int x = 0, y = 0, down = 0;
		if (sscanf(line + 2, "%d %d %d", &x, &y, &down) == 3) {
			enqueue_pointer(x, y, down != 0);
		}
		return;
	}
	if (line[0] == 'K' && line[1] == ' ') {
		char name[32] = {0};
		int down = 0;
		if (sscanf(line + 2, "%31s %d", name, &down) == 2) {
			int code = button_code_for_name(name);
			if (code >= 0) {
				if (code == KEY_POWER) {
					if (down) {
						atomic_store(&charge_power_pressed, 1);
					} else {
						atomic_store(&charge_power_released, 1);
					}
				}
				input_host_button(code, down != 0);
			}
		}
		return;
	}
	if (line[0] == 'S') {
		atomic_store(&shot_requested, 1);
		return;
	}
	if (line[0] == 'Q') {
		atomic_store(&quit_requested, 1);
		raise(SIGINT);
		return;
	}
}

static void *reader_main(void *arg) {
	int fd = (int)(intptr_t)arg;
	char buf[1024];
	size_t used = 0;

	while (!atomic_load(&shutting_down)) {
		ssize_t n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (n == 0) {
			break;
		}
		used += (size_t)n;
		buf[used] = '\0';

		char *start = buf;
		char *nl;
		while ((nl = strchr(start, '\n')) != NULL) {
			*nl = '\0';
			if (nl > start && *(nl - 1) == '\r') {
				*(nl - 1) = '\0';
			}
			handle_command_line(start);
			start = nl + 1;
		}
		size_t rem = used - (size_t)(start - buf);
		if (rem > 0 && start != buf) {
			memmove(buf, start, rem);
		}
		used = rem;
		if (used >= sizeof(buf) - 1) {
			used = 0; /* discard malformed overlong line */
		}
	}
	return NULL;
}

void webemu_init(void) {
	if (sock_fd >= 0) {
		return;
	}
	/* Stream stdout/stderr line-by-line immediately into the web log console. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	const char *path = getenv("SONIX_WEB_EMU_SOCK");
	if (!path || !path[0]) {
		fprintf(stderr, "webemu: SONIX_WEB_EMU_SOCK unset; running headless without bridge\n");
		return;
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("webemu: socket");
		return;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("webemu: connect");
		close(fd);
		return;
	}

	/* Prevent a stalled server from ever deadlocking the audio or UI thread. */
	struct timeval tv = {.tv_sec = 0, .tv_usec = 250000};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	pthread_mutex_lock(&write_lock);
	sock_fd = fd;
	pthread_mutex_unlock(&write_lock);

	atomic_store(&shutting_down, 0);
	if (pthread_create(&reader_thread, NULL, reader_main, (void *)(intptr_t)fd) == 0) {
		reader_started = true;
		pthread_detach(reader_thread);
	}

	fprintf(stderr, "webemu: connected to %s\n", path);
	send_event_json("{\"type\":\"ready\",\"board\":\"TempoTec V1\"}");
	webemu_notify_screen(true);
	webemu_notify_volume(atomic_load(&current_volume_pct));
}

void webemu_shutdown(void) {
	atomic_store(&shutting_down, 1);
	pthread_mutex_lock(&write_lock);
	if (sock_fd >= 0) {
		shutdown(sock_fd, SHUT_RDWR);
		close(sock_fd);
		sock_fd = -1;
	}
	pthread_mutex_unlock(&write_lock);
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
	fb_send_buf = calloc(1, bytes);
	fb_capture_buf = calloc(1, bytes);
	if (!fb_pixels || !fb_draw_buf || !fb_send_buf || !fb_capture_buf) {
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

static int sgm_step_to_srgb(int step) {
	if (step <= 0) return 0;
	if (step >= 79) return 255;
	/* Perceptual gamma expansion for the SGM31324 0..79 current ladder so
	 * default level 5 (23) displays as a crisp, visible LED color. */
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

	/* Approximate SGM31324 register timing into milliseconds for UI animation. */
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
	send_event_json(msg);
}

int webemu_get_volume_percent(void) { return atomic_load(&current_volume_pct); }

void webemu_send_audio_pcm(uint32_t sample_rate, uint16_t channels, const int16_t *samples, size_t frame_count) {
	if (!samples || frame_count == 0 || sample_rate == 0 || channels == 0 || channels > 2) {
		return;
	}
	uint8_t hdr[6];
	write_be32(hdr, sample_rate);
	write_be16(hdr + 4, channels);
	send_packet_parts(WEBEMU_MSG_AUDIO, hdr, sizeof(hdr), samples, frame_count * channels * sizeof(int16_t));
}

#endif /* HOST_WEB_EMU */
