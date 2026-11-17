// Regression suite for issue #325: touch_evdev's SYN_REPORT framing,
// EVIOCGKEY-backed panel_touch_flush(), and gbinput's initial/SYN_DROPPED
// BTN_TOUCH synchronisation, driven against a real 240x320 LVGL display and a
// fake Linux evdev node that reproduces Linux 4.4's input_handle_event() dedup.

#if !defined(__linux__)
#include <stdio.h>
int main(void) {
	printf("SKIP touch_evdev wake suite (requires Linux evdev headers)\n");
	return 0;
}
#else

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/indev/lv_indev_private.h"
#include "src/system/core/panel.h"
#include "src/system/device/power.h"
#include "src/system/device/touch_evdev.h"
#include "src/system/gearboy/gbinput.h"
#include "src/system/gearboy/gearboy.h"

// ── Fake Linux evdev kernel node ────────────────────────────────────────────

#define FAKE_BIT_WORDS(max_code) (((max_code) / (8 * sizeof(unsigned long))) + 1)
#define FAKE_MAX_SLOTS 4

typedef struct {
	int rfd;
	int wfd;
	char name[64];
	unsigned long rel_cap[FAKE_BIT_WORDS(REL_MAX)];
	unsigned long abs_cap[FAKE_BIT_WORDS(ABS_MAX)];
	unsigned long key_cap[FAKE_BIT_WORDS(KEY_MAX)];
	unsigned long key_state[FAKE_BIT_WORDS(KEY_MAX)];
	struct input_absinfo absinfo[ABS_CNT];
	int cur_slot;
	int slot_val[FAKE_MAX_SLOTS][ABS_CNT];
	bool has_slots;
	pthread_mutex_t lock;
} fake_evdev_t;

static fake_evdev_t g_ev = {
	.rfd = -1,
	.wfd = -1,
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

static inline void fbit_set(unsigned long *bits, unsigned int code) {
	const unsigned int bpw = (unsigned int)(8 * sizeof(unsigned long));
	bits[code / bpw] |= 1UL << (code % bpw);
}

static inline void fbit_clr(unsigned long *bits, unsigned int code) {
	const unsigned int bpw = (unsigned int)(8 * sizeof(unsigned long));
	bits[code / bpw] &= ~(1UL << (code % bpw));
}

static inline bool fbit_get(const unsigned long *bits, unsigned int code) {
	const unsigned int bpw = (unsigned int)(8 * sizeof(unsigned long));
	return (bits[code / bpw] & (1UL << (code % bpw))) != 0;
}

static void fake_evdev_close_all(void) {
	pthread_mutex_lock(&g_ev.lock);
	if (g_ev.rfd >= 0) {
		close(g_ev.rfd);
		g_ev.rfd = -1;
	}
	if (g_ev.wfd >= 0) {
		close(g_ev.wfd);
		g_ev.wfd = -1;
	}
	pthread_mutex_unlock(&g_ev.lock);
}

static void fake_evdev_reset(const char *name) {
	fake_evdev_close_all();
	pthread_mutex_lock(&g_ev.lock);
	memset(g_ev.name, 0, sizeof(g_ev.name));
	if (name) {
		snprintf(g_ev.name, sizeof(g_ev.name), "%s", name);
	}
	memset(g_ev.rel_cap, 0, sizeof(g_ev.rel_cap));
	memset(g_ev.abs_cap, 0, sizeof(g_ev.abs_cap));
	memset(g_ev.key_cap, 0, sizeof(g_ev.key_cap));
	memset(g_ev.key_state, 0, sizeof(g_ev.key_state));
	memset(g_ev.absinfo, 0, sizeof(g_ev.absinfo));
	memset(g_ev.slot_val, 0, sizeof(g_ev.slot_val));
	for (int s = 0; s < FAKE_MAX_SLOTS; s++) {
		g_ev.slot_val[s][ABS_MT_TRACKING_ID] = -1;
	}
	g_ev.absinfo[ABS_MT_TRACKING_ID].value = -1;
	g_ev.cur_slot = 0;
	g_ev.has_slots = false;

	int fds[2];
	if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
		perror("pipe2");
		exit(1);
	}
	g_ev.rfd = fds[0];
	g_ev.wfd = fds[1];
	pthread_mutex_unlock(&g_ev.lock);
}

static void fake_set_abs(unsigned int code, int min_v, int max_v, int init_v) {
	fbit_set(g_ev.abs_cap, code);
	g_ev.absinfo[code].minimum = min_v;
	g_ev.absinfo[code].maximum = max_v;
	g_ev.absinfo[code].value = init_v;
	if (code == ABS_MT_SLOT) {
		g_ev.has_slots = true;
	}
}

// Configure exact TempoTec V1 cst8xx_touch.ko ("hyn_ts") capabilities:
// Type A MT with BTN_TOUCH, no ABS_MT_SLOT, no ABS_X/Y, and the driver's real
// 0..720 x 0..1280 MT axis ranges that must NOT scale V1's 0..239 x 0..319 pixels.
static void fake_setup_v1_cst8xx(void) {
	fake_evdev_reset("hyn_ts");
	fbit_set(g_ev.key_cap, BTN_TOUCH);
	fake_set_abs(ABS_MT_TRACKING_ID, 0, 15, 0);
	fake_set_abs(ABS_MT_TOUCH_MAJOR, 0, 255, 0);
	fake_set_abs(ABS_MT_WIDTH_MAJOR, 0, 255, 0);
	fake_set_abs(ABS_MT_POSITION_X, 0, 720, 0);
	fake_set_abs(ABS_MT_POSITION_Y, 0, 1280, 0);
	fake_set_abs(ABS_MT_PRESSURE, 0, 255, 0);
}

static void fake_write_raw(uint16_t type, uint16_t code, int32_t value) {
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	ssize_t n = write(g_ev.wfd, &ev, sizeof(ev));
	if (n != (ssize_t)sizeof(ev)) {
		perror("fake_write_raw");
		exit(1);
	}
}

// Reproduces Linux 4.4 input_handle_event():
//   - EV_KEY dedupes against key_state.
//   - Type A MT axes (ABS_MT_FIRST..ABS_MT_LAST with !has_slots) NEVER dedupe.
//   - Type B MT axes dedupe per slot; ABS_MT_SLOT dedupes against cur_slot.
//   - Single-touch EV_ABS dedupes against absinfo[code].value.
static void fake_kernel_event(uint16_t type, uint16_t code, int32_t value) {
	pthread_mutex_lock(&g_ev.lock);
	if (type == EV_KEY) {
		bool was = fbit_get(g_ev.key_state, code);
		bool now = (value != 0);
		if (was == now) {
			pthread_mutex_unlock(&g_ev.lock);
			return;
		}
		if (now) {
			fbit_set(g_ev.key_state, code);
		} else {
			fbit_clr(g_ev.key_state, code);
		}
		fake_write_raw(type, code, now ? 1 : 0);
		pthread_mutex_unlock(&g_ev.lock);
		return;
	}
	if (type == EV_ABS) {
		if (code == ABS_MT_SLOT && g_ev.has_slots) {
			if (value == g_ev.cur_slot) {
				pthread_mutex_unlock(&g_ev.lock);
				return;
			}
			g_ev.cur_slot = value;
			g_ev.absinfo[ABS_MT_SLOT].value = value;
			fake_write_raw(type, code, value);
			pthread_mutex_unlock(&g_ev.lock);
			return;
		}
		bool is_mt = (code >= ABS_MT_SLOT && code <= ABS_MT_TOOL_Y);
		if (is_mt && g_ev.has_slots) {
			int s = g_ev.cur_slot;
			if (s >= 0 && s < FAKE_MAX_SLOTS) {
				if (g_ev.slot_val[s][code] == value) {
					pthread_mutex_unlock(&g_ev.lock);
					return;
				}
				g_ev.slot_val[s][code] = value;
				g_ev.absinfo[code].value = value;
			}
			fake_write_raw(type, code, value);
			pthread_mutex_unlock(&g_ev.lock);
			return;
		}
		if (is_mt && !g_ev.has_slots) {
			g_ev.absinfo[code].value = value;
			fake_write_raw(type, code, value);
			pthread_mutex_unlock(&g_ev.lock);
			return;
		}
		if (g_ev.absinfo[code].value == value) {
			pthread_mutex_unlock(&g_ev.lock);
			return;
		}
		g_ev.absinfo[code].value = value;
		fake_write_raw(type, code, value);
		pthread_mutex_unlock(&g_ev.lock);
		return;
	}
	fake_write_raw(type, code, value);
	pthread_mutex_unlock(&g_ev.lock);
}

// Updates the kernel's live key/abs state without queueing an event — models an
// event that fell out of the client's evdev ring during SYN_DROPPED.
static void fake_set_live_key_silent(unsigned int code, bool down) {
	pthread_mutex_lock(&g_ev.lock);
	if (down) {
		fbit_set(g_ev.key_state, code);
	} else {
		fbit_clr(g_ev.key_state, code);
	}
	pthread_mutex_unlock(&g_ev.lock);
}

static int test_open(const char *path, int flags, ...) {
	(void)path;
	(void)flags;
	pthread_mutex_lock(&g_ev.lock);
	int fd = g_ev.rfd;
	g_ev.rfd = -1; // transferred to caller (closed by driver on delete/stop)
	pthread_mutex_unlock(&g_ev.lock);
	if (fd < 0) {
		errno = ENOENT;
		return -1;
	}
	return fd;
}

static int test_ioctl(int fd, unsigned long req, ...) {
	(void)fd;
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	unsigned int type = _IOC_TYPE(req);
	unsigned int nr = _IOC_NR(req);
	size_t sz = _IOC_SIZE(req);
	if (type != 'E') {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&g_ev.lock);
	int rc = 0;
	if (nr == 0x06) { // EVIOCGNAME(len)
		memset(arg, 0, sz);
		size_t copy = strlen(g_ev.name);
		if (copy >= sz && sz > 0) copy = sz - 1;
		memcpy(arg, g_ev.name, copy);
		rc = (int)copy;
	} else if (nr == 0x18) { // EVIOCGKEY(len)
		memset(arg, 0, sz);
		size_t copy = sz < sizeof(g_ev.key_state) ? sz : sizeof(g_ev.key_state);
		memcpy(arg, g_ev.key_state, copy);
	} else if (nr == 0x20 + EV_REL) {
		memset(arg, 0, sz);
		size_t copy = sz < sizeof(g_ev.rel_cap) ? sz : sizeof(g_ev.rel_cap);
		memcpy(arg, g_ev.rel_cap, copy);
	} else if (nr == 0x20 + EV_ABS) {
		memset(arg, 0, sz);
		size_t copy = sz < sizeof(g_ev.abs_cap) ? sz : sizeof(g_ev.abs_cap);
		memcpy(arg, g_ev.abs_cap, copy);
	} else if (nr == 0x20 + EV_KEY) {
		memset(arg, 0, sz);
		size_t copy = sz < sizeof(g_ev.key_cap) ? sz : sizeof(g_ev.key_cap);
		memcpy(arg, g_ev.key_cap, copy);
	} else if (nr >= 0x40 && nr < 0x40 + ABS_CNT) { // EVIOCGABS(axis)
		unsigned int axis = nr - 0x40;
		struct input_absinfo out = g_ev.absinfo[axis];
		if (g_ev.has_slots && axis >= ABS_MT_SLOT && axis <= ABS_MT_TOOL_Y &&
			axis != ABS_MT_SLOT && g_ev.cur_slot >= 0 && g_ev.cur_slot < FAKE_MAX_SLOTS) {
			out.value = g_ev.slot_val[g_ev.cur_slot][axis];
		}
		memcpy(arg, &out, sizeof(out));
	} else {
		errno = EINVAL;
		rc = -1;
	}
	pthread_mutex_unlock(&g_ev.lock);
	return rc;
}

// Redirect open() and ioctl() inside the real Linux drivers under test.
#undef HOST_BUILD
#define open test_open
#define ioctl test_ioctl
#include "src/system/device/touch_evdev.c"
#include "src/system/gearboy/gbinput.c"
#undef open
#undef ioctl

// ── Minimal stubs for gbinput's external hooks ──────────────────────────────

lv_font_t font_ui_14;

static pthread_mutex_t g_gb_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t g_gb_keys = 0;
static int g_gb_menu_hits = 0;

const char *panel_touch_device(void) { return "/dev/input/fake_touch"; }
void panel_touch_enable(bool on) { (void)on; }
void panel_touch_flush(void) {}
bool display_get_rotated(void) { return false; }
void power_note_button_activity(void) {}

void gearboy_set_keys(uint16_t mask) {
	pthread_mutex_lock(&g_gb_lock);
	g_gb_keys = mask;
	pthread_mutex_unlock(&g_gb_lock);
}

static void on_gb_menu(void) {
	pthread_mutex_lock(&g_gb_lock);
	g_gb_menu_hits++;
	pthread_mutex_unlock(&g_gb_lock);
}

static uint16_t gb_get_keys(void) {
	pthread_mutex_lock(&g_gb_lock);
	uint16_t v = g_gb_keys;
	pthread_mutex_unlock(&g_gb_lock);
	return v;
}

static int gb_get_menu_hits(void) {
	pthread_mutex_lock(&g_gb_lock);
	int v = g_gb_menu_hits;
	pthread_mutex_unlock(&g_gb_lock);
	return v;
}

// ── Production panel_touch_flush() from main.c ──────────────────────────────

static void run_panel_touch_flush(lv_indev_t *indev) {
	touch_evdev_flush(indev);
	lv_indev_reset(indev, NULL);
	indev->state = LV_INDEV_STATE_RELEASED;
	indev->wait_until_release = 0;
}

// ── Packet emitters matching stock & patched cst8xx_touch.ko ────────────────

// Exact event sequence produced by stock cst8xx_touch.ko on a point_num=0
// all-zero frame (wake IRQ replay after enable_irq(), or gesture-only IRQ).
static void emit_unpatched_wake_glitch(void) {
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, 0);
	fake_kernel_event(EV_ABS, ABS_MT_TOUCH_MAJOR, 0);
	fake_kernel_event(EV_ABS, ABS_MT_WIDTH_MAJOR, 0);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, 0);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, 0);
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_KEY, BTN_TOUCH, 0); // deduped when already 0
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
}

// Exact event sequence produced by patched cst8xx_touch.ko on the same frame.
static void emit_patched_wake_packet(void) {
	fake_kernel_event(EV_KEY, BTN_TOUCH, 0); // deduped when already 0
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
}

// Scan 0 of the first real tap on an UNPATCHED cst8xx_touch.ko whose
// events[0].cnt was left at 1 by the spurious wake frame: cnt toggles 1->0,
// skipping ABS_MT_*, and only BTN_TOUCH 1 + SYN_REPORT goes out; Scan 1 10 ms
// later sends the full contact.
static void emit_unpatched_odd_scan0(void) {
	fake_kernel_event(EV_KEY, BTN_TOUCH, 1);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
}

static void emit_mt_a_down(int id, int x, int y) {
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, id);
	fake_kernel_event(EV_ABS, ABS_MT_TOUCH_MAJOR, 63);
	fake_kernel_event(EV_ABS, ABS_MT_WIDTH_MAJOR, 9);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, x);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, y);
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_KEY, BTN_TOUCH, 1);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
}

static void emit_mt_a_up(void) {
	fake_kernel_event(EV_KEY, BTN_TOUCH, 0);
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
}

// ── Headless 240x320 LVGL harness ───────────────────────────────────────────

#define DISP_W 240
#define DISP_H 320

static uint32_t g_now_ms = 1000;
static uint16_t g_fb[DISP_W * DISP_H];
static lv_display_t *g_disp = NULL;

static uint32_t tick_cb(void) { return g_now_ms; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
	(void)a;
	(void)px;
	lv_display_flush_ready(d);
}

static void step_ms(uint32_t dt) {
	g_now_ms += dt;
	lv_timer_handler();
}

typedef struct {
	int pressed;
	int released;
	int clicked;
	int pressing;
	lv_point_t first_press_pt;
	lv_point_t last_press_pt;
} hit_log_t;

static void widget_event_cb(lv_event_t *e) {
	hit_log_t *log = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_event_get_indev(e);
	lv_point_t pt = {0, 0};
	if (indev) {
		lv_indev_get_point(indev, &pt);
	}
	if (code == LV_EVENT_PRESSED) {
		if (log->pressed == 0) {
			log->first_press_pt = pt;
		}
		log->last_press_pt = pt;
		log->pressed++;
	} else if (code == LV_EVENT_PRESSING) {
		log->last_press_pt = pt;
		log->pressing++;
	} else if (code == LV_EVENT_RELEASED) {
		log->released++;
	} else if (code == LV_EVENT_CLICKED) {
		log->clicked++;
	}
}

static lv_obj_t *make_target(lv_obj_t *parent, int x, int y, int w, int h, hit_log_t *log) {
	memset(log, 0, sizeof(*log));
	lv_obj_t *btn = lv_obj_create(parent);
	lv_obj_remove_style_all(btn);
	lv_obj_set_pos(btn, x, y);
	lv_obj_set_size(btn, w, h);
	lv_obj_set_clickable(btn, true);
	lv_obj_set_scrollable(btn, false);
	lv_obj_add_event_cb(btn, widget_event_cb, LV_EVENT_PRESSED, log);
	lv_obj_add_event_cb(btn, widget_event_cb, LV_EVENT_PRESSING, log);
	lv_obj_add_event_cb(btn, widget_event_cb, LV_EVENT_RELEASED, log);
	lv_obj_add_event_cb(btn, widget_event_cb, LV_EVENT_CLICKED, log);
	return btn;
}

typedef struct {
	lv_obj_t *scr;
	hit_log_t tl;     // (0, 0) 48x48 — catches spurious (0, 0) wake touch
	hit_log_t center; // (80, 120) 80x80 — Play button at (120, 160)
	hit_log_t br;     // (192, 272) 48x48 — bottom-right corner (239, 319)
	hit_log_t bg;     // full 240x320 background — drag/swipe sensor
} scene_t;

static void scene_init(scene_t *s) {
	s->scr = lv_obj_create(NULL);
	lv_obj_remove_style_all(s->scr);
	lv_obj_set_size(s->scr, DISP_W, DISP_H);
	lv_obj_set_scrollable(s->scr, false);
	make_target(s->scr, 0, 0, DISP_W, DISP_H, &s->bg);
	make_target(s->scr, 0, 0, 48, 48, &s->tl);
	make_target(s->scr, 80, 120, 80, 80, &s->center);
	make_target(s->scr, 192, 272, 48, 48, &s->br);
	lv_screen_load(s->scr);
	step_ms(20);
}

static void scene_clear(scene_t *s) {
	lv_obj_t *old = s->scr;
	lv_obj_t *blank = lv_obj_create(NULL);
	lv_screen_load(blank);
	lv_obj_delete(old);
	step_ms(20);
}

#define CHECK(cond, ...)                                                                           \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
			fprintf(stderr, __VA_ARGS__);                                                          \
			fputc('\n', stderr);                                                                   \
			exit(1);                                                                               \
		}                                                                                          \
	} while (0)

// ── Test cases ──────────────────────────────────────────────────────────────

// 1. Unpatched cst8xx_touch.ko: panel_touch_flush() runs on wake, then 200 ms
//    later hyn_resume_work enables PA16 IRQ and emits the spurious (0, 0)
//    contact with BTN_TOUCH 0 deduped. Must NOT stay PRESSED, must let display
//    idle time advance for auto-screen-off, and must click the Play button on
//    the VERY FIRST tap despite scan 0's bare BTN_TOUCH 1.
static void test_unpatched_wake_after_flush_first_tap_and_idle(void) {
	scene_t s;
	scene_init(&s);
	fake_setup_v1_cst8xx();
	lv_indev_t *indev = touch_evdev_create(panel_touch_device());
	CHECK(indev != NULL, "touch_evdev_create failed");
	lv_indev_set_display(indev, g_disp);

	// Touch bottom-right before sleep so last release coords are (220, 300).
	emit_mt_a_down(0, 220, 300);
	step_ms(20);
	emit_mt_a_up();
	step_ms(20);
	CHECK(s.br.clicked == 1, "pre-sleep tap failed (%d)", s.br.clicked);

	// power_screen_off() -> power_screen_on(): flush runs immediately on wake.
	run_panel_touch_flush(indev);
	lv_display_trigger_activity(g_disp);
	step_ms(200);

	// hyn_resume_work fires after panel wakeup and replays the latched IRQ.
	emit_unpatched_wake_glitch();
	step_ms(20);

	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED,
		  "spurious wake frame left indev PRESSED");
	CHECK(s.tl.pressed == 0 && s.tl.clicked == 0,
		  "spurious wake frame hit (0, 0) corner (pressed=%d clicked=%d)",
		  s.tl.pressed, s.tl.clicked);

	// Auto-screen-off idle timer must NOT be pinned to 0 after wake!
	uint32_t idle0 = lv_display_get_inactive_time(g_disp);
	for (int i = 0; i < 30; i++) {
		step_ms(1000);
	}
	uint32_t idle1 = lv_display_get_inactive_time(g_disp);
	CHECK(idle1 >= idle0 + 29900,
		  "inactive time stuck after wake: idle0=%u idle1=%u", idle0, idle1);

	// User's FIRST touch after wake at (120, 160): scan 0 carries bare
	// BTN_TOUCH 1 (because stock .ko left events[0].cnt == 1), scan 1 carries
	// the real coordinates 10 ms later, then finger lifts.
	emit_unpatched_odd_scan0();
	step_ms(10);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED,
		  "bare BTN_TOUCH 1 opened premature press at stale coords");
	CHECK(s.br.clicked == 1 && s.br.pressed == 1,
		  "bare BTN_TOUCH 1 re-pressed prior release target (220, 300)");
	CHECK(s.tl.pressed == 0, "bare BTN_TOUCH 1 pressed (0, 0)");

	emit_mt_a_down(0, 120, 160);
	step_ms(20);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED,
		  "scan 1 with coords did not enter PRESSED");
	CHECK(s.center.pressed == 1, "Play button not pressed on scan 1");
	CHECK(s.center.first_press_pt.x == 120 && s.center.first_press_pt.y == 160,
		  "wrong press point (%d,%d)", (int)s.center.first_press_pt.x,
		  (int)s.center.first_press_pt.y);

	emit_mt_a_up();
	step_ms(20);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED, "did not release");
	CHECK(s.center.clicked == 1, "FIRST tap after wake swallowed (clicked=%d)", s.center.clicked);
	CHECK(s.tl.pressed == 0 && s.tl.clicked == 0, "corner ghost hit");
	CHECK(s.br.clicked == 1, "stale target ghost hit");

	lv_indev_delete(indev);
	fake_evdev_close_all();
	scene_clear(&s);
}

// 2. Unpatched wake glitch queued BEFORE panel_touch_flush(), followed by a
//    downward swipe as the very first gesture after wake: no jump from stale
//    pre-sleep coordinates and full vertical travel preserved.
static void test_unpatched_wake_before_flush_and_first_slide(void) {
	scene_t s;
	scene_init(&s);
	fake_setup_v1_cst8xx();
	lv_indev_t *indev = touch_evdev_create(panel_touch_device());
	lv_indev_set_display(indev, g_disp);

	// Pre-sleep tap at bottom-right.
	emit_mt_a_down(0, 210, 295);
	step_ms(20);
	emit_mt_a_up();
	step_ms(20);

	// Spurious wake frame lands in the evdev ring before panel_touch_flush().
	emit_unpatched_wake_glitch();
	run_panel_touch_flush(indev);
	step_ms(20);
	CHECK(s.tl.pressed == 0, "flush leaked queued wake glitch");

	// First post-wake gesture: top-edge downward swipe (120, 8) -> (120, 110).
	emit_unpatched_odd_scan0();
	step_ms(10);
	CHECK(s.bg.pressed == 0, "premature press on bare BTN_TOUCH 1");

	const int ys[] = {8, 28, 56, 84, 110};
	for (size_t i = 0; i < sizeof(ys) / sizeof(ys[0]); i++) {
		emit_mt_a_down(0, 120, ys[i]);
		step_ms(16);
	}
	CHECK(s.bg.pressed == 1, "swipe press missing (%d)", s.bg.pressed);
	CHECK(s.bg.first_press_pt.x == 120 && s.bg.first_press_pt.y == 8,
		  "swipe jumped to stale origin (%d,%d)",
		  (int)s.bg.first_press_pt.x, (int)s.bg.first_press_pt.y);
	CHECK(s.bg.last_press_pt.x == 120 && s.bg.last_press_pt.y == 110,
		  "swipe endpoint wrong (%d,%d)",
		  (int)s.bg.last_press_pt.x, (int)s.bg.last_press_pt.y);

	emit_mt_a_up();
	step_ms(20);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED, "swipe did not release");

	lv_indev_delete(indev);
	fake_evdev_close_all();
	scene_clear(&s);
}

// 3. Patched cst8xx_touch.ko: first tap fires on scan 0, exact corner pixels
//    (0, 0) and (239, 319) work, and 180-deg display rotation calibrates cleanly.
static void test_patched_wake_corners_and_180_rotation(void) {
	scene_t s;
	scene_init(&s);
	fake_setup_v1_cst8xx();
	lv_indev_t *indev = touch_evdev_create(panel_touch_device());
	lv_indev_set_display(indev, g_disp);

	run_panel_touch_flush(indev);
	emit_patched_wake_packet();
	step_ms(20);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED, "not released after patched wake");

	// Immediate scan-0 first tap on Play button.
	emit_mt_a_down(0, 120, 160);
	step_ms(20);
	emit_mt_a_up();
	step_ms(20);
	CHECK(s.center.clicked == 1, "patched first tap missed (%d)", s.center.clicked);

	// Real (0, 0) corner tap MUST work!
	emit_mt_a_down(0, 0, 0);
	step_ms(20);
	emit_mt_a_up();
	step_ms(20);
	CHECK(s.tl.clicked == 1 && s.tl.first_press_pt.x == 0 && s.tl.first_press_pt.y == 0,
		  "real (0, 0) tap broken (clicked=%d pt=%d,%d)",
		  s.tl.clicked, (int)s.tl.first_press_pt.x, (int)s.tl.first_press_pt.y);

	// Real (239, 319) corner tap MUST work (and NOT be mis-scaled by 720x1280)!
	emit_mt_a_down(0, 239, 319);
	step_ms(20);
	emit_mt_a_up();
	step_ms(20);
	CHECK(s.br.clicked == 1 && s.br.first_press_pt.x == 239 && s.br.first_press_pt.y == 319,
		  "real (239, 319) tap broken (clicked=%d pt=%d,%d)",
		  s.br.clicked, (int)s.br.first_press_pt.x, (int)s.br.first_press_pt.y);

	// 180-deg flip (main.c apply_display_rotation): raw (0, 0) -> (239, 319),
	// raw (239, 319) -> (0, 0).
	touch_evdev_set_calibration(indev, DISP_W - 1, DISP_H - 1, 0, 0);
	run_panel_touch_flush(indev);
	emit_patched_wake_packet();
	step_ms(20);

	emit_mt_a_down(0, 0, 0);
	step_ms(20);
	emit_mt_a_up();
	step_ms(20);
	CHECK(s.br.clicked == 2, "180-deg raw (0,0) did not click bottom-right (%d)", s.br.clicked);

	emit_mt_a_down(0, 239, 319);
	step_ms(20);
	emit_mt_a_up();
	step_ms(20);
	CHECK(s.tl.clicked == 2, "180-deg raw (239,319) did not click top-left (%d)", s.tl.clicked);

	// Restore 0-deg calibration.
	touch_evdev_set_calibration(indev, 0, 0, DISP_W - 1, DISP_H - 1);
	emit_mt_a_down(0, 0, 0);
	step_ms(20);
	emit_mt_a_up();
	step_ms(20);
	CHECK(s.tl.clicked == 3, "restored 0-deg raw (0,0) failed (%d)", s.tl.clicked);

	lv_indev_delete(indev);
	fake_evdev_close_all();
	scene_clear(&s);
}

// 4. Finger held on the panel across wake / panel_touch_enable(true):
//    EVIOCGKEY reports BTN_TOUCH == 1, so the ongoing grip is suppressed until
//    BTN_TOUCH == 0 — and even if the finger lifts AND re-taps inside a single
//    LVGL timer period, the fresh tap clicks cleanly.
static void test_held_finger_across_flush_and_subtick_retap(void) {
	scene_t s;
	scene_init(&s);
	fake_setup_v1_cst8xx();
	lv_indev_t *indev = touch_evdev_create(panel_touch_device());
	lv_indev_set_display(indev, g_disp);

	// Holdswitch / sleep while user's thumb is actively pressing Play button.
	lv_indev_enable(indev, false);
	emit_mt_a_down(0, 120, 160);
	emit_mt_a_down(0, 122, 162);

	// Holdswitch released / screen wakes while thumb is STILL held down.
	lv_indev_enable(indev, true);
	run_panel_touch_flush(indev);

	// Thumb drags across Play button while still held: must stay suppressed!
	emit_mt_a_down(0, 125, 165);
	step_ms(20);
	emit_mt_a_down(0, 130, 170);
	step_ms(20);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED,
		  "held pre-flush finger leaked PRESSED");
	CHECK(s.center.pressed == 0 && s.center.clicked == 0,
		  "held pre-flush finger clicked Play (%d/%d)", s.center.pressed, s.center.clicked);

	// Thumb lifts, taps Play button, and lifts all within ONE timer tick!
	emit_mt_a_up();
	emit_mt_a_down(0, 120, 160);
	emit_mt_a_up();
	step_ms(20);

	CHECK(s.center.clicked == 1,
		  "sub-tick lift + re-tap after held wake was lost (clicked=%d)", s.center.clicked);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED, "not released after retap");

	lv_indev_delete(indev);
	fake_evdev_close_all();
	scene_clear(&s);
}

// 5. Fast sub-tick tap and sub-tick flick (press + drag + release queued before
//    a single lv_timer_handler call): tap clicks, flick delivers start point,
//    end point, AND deferred release without getting stuck pressed.
static void test_subtick_fast_tap_and_flick(void) {
	scene_t s;
	scene_init(&s);
	fake_setup_v1_cst8xx();
	lv_indev_t *indev = touch_evdev_create(panel_touch_device());
	lv_indev_set_display(indev, g_disp);

	// Complete press + release queued before one tick.
	emit_mt_a_down(0, 120, 160);
	emit_mt_a_up();
	step_ms(20);
	CHECK(s.center.clicked == 1, "sub-tick fast tap missed (%d)", s.center.clicked);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED, "stuck after fast tap");

	// Complete press + multi-sample drag + release queued before one tick.
	emit_mt_a_down(0, 60, 260);
	emit_mt_a_down(0, 60, 200);
	emit_mt_a_down(0, 60, 140);
	emit_mt_a_down(0, 60, 80);
	emit_mt_a_up();
	step_ms(20);

	CHECK(s.bg.first_press_pt.x == 60 && s.bg.first_press_pt.y == 260,
		  "sub-tick flick origin wrong (%d,%d)",
		  (int)s.bg.first_press_pt.x, (int)s.bg.first_press_pt.y);
	CHECK(s.bg.last_press_pt.x == 60 && s.bg.last_press_pt.y == 80,
		  "sub-tick flick endpoint wrong (%d,%d)",
		  (int)s.bg.last_press_pt.x, (int)s.bg.last_press_pt.y);
	CHECK(s.bg.released == 1, "sub-tick flick missed release (%d)", s.bg.released);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED, "stuck after sub-tick flick");

	lv_indev_delete(indev);
	fake_evdev_close_all();
	scene_clear(&s);
}

// 6. SYN_DROPPED ring overrun dropping BTN_TOUCH 0 mid-touch: partial events
//    up to SYN_REPORT are ignored, EVIOCGKEY resync releases the pointer, and
//    the next tap clicks immediately.
static void test_syn_dropped_resync(void) {
	scene_t s;
	scene_init(&s);
	fake_setup_v1_cst8xx();
	lv_indev_t *indev = touch_evdev_create(panel_touch_device());
	lv_indev_set_display(indev, g_disp);

	emit_mt_a_down(0, 60, 250);
	step_ms(20);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED, "expected pressed");

	// Ring overruns while finger lifts: BTN_TOUCH 0 is lost from the queue,
	// partial coords sit before the closing SYN_REPORT, live key state is 0.
	fake_set_live_key_silent(BTN_TOUCH, false);
	fake_kernel_event(EV_SYN, SYN_DROPPED, 0);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, 0);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, 0);
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);

	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED,
		  "SYN_DROPPED did not release via EVIOCGKEY");
	CHECK(s.tl.pressed == 0, "partial frame after SYN_DROPPED leaked to (0, 0)");

	// Subsequent tap on Play button works cleanly.
	emit_mt_a_down(0, 120, 160);
	step_ms(20);
	emit_mt_a_up();
	step_ms(20);
	CHECK(s.center.clicked == 1, "tap after SYN_DROPPED failed (%d)", s.center.clicked);

	lv_indev_delete(indev);
	fake_evdev_close_all();
	scene_clear(&s);
}

// 7. Protocol A two-finger tracking + handoff, plus Protocol A without
//    BTN_TOUCH capability (including gt9xx-style bare SYN_REPORT release).
static void test_mt_a_multitouch_and_buttonless(void) {
	scene_t s;
	scene_init(&s);
	fake_setup_v1_cst8xx();
	lv_indev_t *indev = touch_evdev_create(panel_touch_device());
	lv_indev_set_display(indev, g_disp);

	// Two contacts down simultaneously: id=1 reported first on frame 2, id=0
	// second — pointer must stay locked onto id=0 rather than bouncing to id=1!
	emit_mt_a_down(0, 110, 150);
	step_ms(20);

	// Frame 2: id=1 at (10, 10) first, id=0 at (125, 165) second.
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, 1);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, 10);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, 10);
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, 0);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, 125);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, 165);
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_KEY, BTN_TOUCH, 1);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);

	lv_point_t pt;
	lv_indev_get_point(indev, &pt);
	CHECK(pt.x == 125 && pt.y == 165, "2-finger frame bounced off active id (%d,%d)",
		  (int)pt.x, (int)pt.y);
	CHECK(s.tl.pressed == 0, "second finger falsely pressed (10, 10)");

	// Frame 3: id=0 lifts while id=1 remains down at (210, 290) (BTN_TOUCH stays 1).
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, 1);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, 210);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, 290);
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);
	lv_indev_get_point(indev, &pt);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED && pt.x == 210 && pt.y == 290,
		  "handoff to remaining contact failed (state=%d pt=%d,%d)",
		  (int)lv_indev_get_state(indev), (int)pt.x, (int)pt.y);

	emit_mt_a_up();
	step_ms(20);
	CHECK(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED, "did not release after 2-finger");
	lv_indev_delete(indev);

	// Buttonless Type A device (gt9xx-like): bare SYN_REPORT releases cleanly.
	int before_clicks = s.center.clicked;
	fake_evdev_reset("buttonless_mta");
	fake_set_abs(ABS_MT_POSITION_X, 0, 240, 0);
	fake_set_abs(ABS_MT_POSITION_Y, 0, 320, 0);
	indev = touch_evdev_create(panel_touch_device());
	lv_indev_set_display(indev, g_disp);

	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, 120);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, 160);
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0); // bare SYN_REPORT release
	step_ms(20);
	CHECK(s.center.clicked == before_clicks + 1, "buttonless MT-A tap failed (%d)",
		  s.center.clicked - before_clicks);

	lv_indev_delete(indev);
	fake_evdev_close_all();
	scene_clear(&s);
}

// 8. Protocol B (ABS_MT_SLOT, including deduped repeat-tap at identical coords
//    and ignoring slot 1), classic single-touch ABS_X/ABS_Y scaling, and
//    relative REL_X/REL_Y + BTN_MOUSE pointer.
static void test_mt_b_single_touch_and_relative(void) {
	scene_t s;
	scene_init(&s);

	// 8a. Protocol B with BTN_TOUCH.
	fake_evdev_reset("goodix_mtb");
	fbit_set(g_ev.key_cap, BTN_TOUCH);
	fake_set_abs(ABS_MT_SLOT, 0, 4, 0);
	fake_set_abs(ABS_MT_TRACKING_ID, 0, 65535, -1);
	fake_set_abs(ABS_MT_POSITION_X, 0, 240, 0);
	fake_set_abs(ABS_MT_POSITION_Y, 0, 320, 0);
	lv_indev_t *indev = touch_evdev_create(panel_touch_device());
	lv_indev_set_display(indev, g_disp);

	// Tap 1 at (120, 160), while slot 1 also touches (5, 5).
	fake_kernel_event(EV_ABS, ABS_MT_SLOT, 0);
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, 10);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, 120);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, 160);
	fake_kernel_event(EV_ABS, ABS_MT_SLOT, 1);
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, 11);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, 5);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, 5);
	fake_kernel_event(EV_KEY, BTN_TOUCH, 1);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);
	CHECK(s.tl.pressed == 0, "MT-B slot 1 overrode slot 0");

	fake_kernel_event(EV_ABS, ABS_MT_SLOT, 1);
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, -1);
	fake_kernel_event(EV_ABS, ABS_MT_SLOT, 0);
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, -1);
	fake_kernel_event(EV_KEY, BTN_TOUCH, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);
	CHECK(s.center.clicked == 1, "MT-B first tap missed (%d)", s.center.clicked);

	// Tap 2 at EXACT SAME (120, 160): kernel dedupes ABS_MT_POSITION_X/Y!
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, 12);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, 120); // deduped
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, 160); // deduped
	fake_kernel_event(EV_KEY, BTN_TOUCH, 1);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);
	fake_kernel_event(EV_ABS, ABS_MT_TRACKING_ID, -1);
	fake_kernel_event(EV_KEY, BTN_TOUCH, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);
	CHECK(s.center.clicked == 2, "MT-B deduped repeat-tap missed (%d)", s.center.clicked);
	lv_indev_delete(indev);

	// 8b. Classic single-touch ABS_X/ABS_Y calibrated from 100..900 x 200..1000.
	fake_evdev_reset("st_touch");
	fbit_set(g_ev.key_cap, BTN_TOUCH);
	fake_set_abs(ABS_X, 100, 900, 100);
	fake_set_abs(ABS_Y, 200, 1000, 200);
	indev = touch_evdev_create(panel_touch_device());
	lv_indev_set_display(indev, g_disp);

	// Midpoint (500, 600) maps to (120, 160) -> Play button.
	fake_kernel_event(EV_ABS, ABS_X, 500);
	fake_kernel_event(EV_ABS, ABS_Y, 600);
	fake_kernel_event(EV_KEY, BTN_TOUCH, 1);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);
	fake_kernel_event(EV_KEY, BTN_TOUCH, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);
	CHECK(s.center.clicked == 3, "ST calibrated tap missed (%d)", s.center.clicked);
	lv_indev_delete(indev);

	// 8c. Relative mouse (REL_X/REL_Y + BTN_MOUSE).
	fake_evdev_reset("usb_mouse");
	fbit_set(g_ev.rel_cap, REL_X);
	fbit_set(g_ev.rel_cap, REL_Y);
	fbit_set(g_ev.key_cap, BTN_MOUSE);
	indev = touch_evdev_create(panel_touch_device());
	lv_indev_set_display(indev, g_disp);

	fake_kernel_event(EV_REL, REL_X, 120);
	fake_kernel_event(EV_REL, REL_Y, 160);
	fake_kernel_event(EV_KEY, BTN_MOUSE, 1);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);
	fake_kernel_event(EV_KEY, BTN_MOUSE, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	step_ms(20);
	CHECK(s.center.clicked == 4, "relative mouse click missed (%d)", s.center.clicked);

	lv_indev_delete(indev);
	fake_evdev_close_all();
	scene_clear(&s);
}

// 9. Gearboy raw evdev reader (gbinput.c): unpatched wake (0, 0) frame BEFORE
//    any real tap must NOT trigger the top-left MENU zone, real taps on A and
//    MENU must trigger immediately, and SYN_DROPPED dropping BTN_TOUCH 0 must
//    clear held keys via EVIOCGKEY.
static void wait_until_gb(uint16_t want_keys, int want_menu) {
	for (int i = 0; i < 100; i++) {
		if (gb_get_keys() == want_keys && gb_get_menu_hits() == want_menu) {
			return;
		}
		struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
		nanosleep(&ts, NULL);
	}
	CHECK(false, "gbinput timeout: got keys=0x%x menu=%d, want keys=0x%x menu=%d",
		  gb_get_keys(), gb_get_menu_hits(), want_keys, want_menu);
}

#define GB_TEST_KEY_A 0x0001

static void test_gbinput_wake_guard_and_syn_dropped(void) {
	fake_setup_v1_cst8xx();
	g_gb_keys = 0;
	g_gb_menu_hits = 0;

	const gbinput_zone_t z[2] = {
		{.x = 0, .y = 0, .w = 48, .h = 48, .keys = GBINPUT_KEY_MENU},
		{.x = 160, .y = 220, .w = 80, .h = 100, .keys = GB_TEST_KEY_A},
	};
	gbinput_set_glass(DISP_W, DISP_H);
	CHECK(gbinput_start(z, 2, on_gb_menu), "gbinput_start failed");

	// Spurious wake frame right on the (0, 0) MENU button before any tap!
	emit_unpatched_wake_glitch();
	struct timespec settle = {.tv_sec = 0, .tv_nsec = 30 * 1000 * 1000};
	nanosleep(&settle, NULL);
	CHECK(gb_get_menu_hits() == 0 && gb_get_keys() == 0,
		  "gbinput tripped on spurious (0, 0) wake frame (menu=%d keys=0x%x)",
		  gb_get_menu_hits(), gb_get_keys());

	// Real press on A button (200, 260) — including unpatched odd scan 0.
	emit_unpatched_odd_scan0();
	emit_mt_a_down(0, 200, 260);
	wait_until_gb(GB_TEST_KEY_A, 0);

	// SYN_DROPPED drops BTN_TOUCH 0: live key bitmap is 0 -> A must release!
	fake_set_live_key_silent(BTN_TOUCH, false);
	fake_kernel_event(EV_SYN, SYN_DROPPED, 0);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_X, 200);
	fake_kernel_event(EV_ABS, ABS_MT_POSITION_Y, 260);
	fake_kernel_event(EV_SYN, SYN_MT_REPORT, 0);
	fake_kernel_event(EV_SYN, SYN_REPORT, 0);
	wait_until_gb(0, 0);

	// Real tap on (0, 0) MENU corner fires menu_cb exactly once.
	emit_mt_a_down(0, 0, 0);
	wait_until_gb(0, 1);
	emit_mt_a_up();
	nanosleep(&settle, NULL);
	CHECK(gb_get_menu_hits() == 1, "menu fired wrong count (%d)", gb_get_menu_hits());

	gbinput_stop();
	fake_evdev_close_all();
}

int main(void) {
	lv_init();
	font_ui_14 = lv_font_montserrat_14;
	lv_tick_set_cb(tick_cb);

	g_disp = lv_display_create(DISP_W, DISP_H);
	lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(g_disp, g_fb, NULL, sizeof(g_fb), LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(g_disp, flush_cb);

	test_unpatched_wake_after_flush_first_tap_and_idle();
	test_unpatched_wake_before_flush_and_first_slide();
	test_patched_wake_corners_and_180_rotation();
	test_held_finger_across_flush_and_subtick_retap();
	test_subtick_fast_tap_and_flick();
	test_syn_dropped_resync();
	test_mt_a_multitouch_and_buttonless();
	test_mt_b_single_touch_and_relative();
	test_gbinput_wake_guard_and_syn_dropped();

	printf("PASS touch_evdev wake, calibration, multitouch, SYN_DROPPED, and gbinput guard\n");
	return 0;
}

#endif /* __linux__ */
