#include "src/system/device/touch_evdev.h"

#if defined(__linux__) && !defined(HOST_BUILD)

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef SYN_DROPPED
#define SYN_DROPPED 3
#endif

#define BIT_WORDS(max_bit) (((max_bit) / (8 * sizeof(unsigned long))) + 1)

static bool test_bit(const unsigned long *bits, unsigned int bit) {
	const unsigned int bpw = (unsigned int)(8 * sizeof(unsigned long));
	return (bits[bit / bpw] & (1UL << (bit % bpw))) != 0;
}

typedef struct {
	bool has_rel_xy;
	bool has_abs_xy;
	bool has_mt_xy;
	bool has_mt_slot;
	bool has_btn_touch;
	bool has_btn_mouse;
	int min_x;
	int min_y;
	int max_x;
	int max_y;
} touch_evdev_caps_t;

typedef struct {
	int fd;
	touch_evdev_caps_t caps;

	int min_x;
	int min_y;
	int max_x;
	int max_y;

	// Last committed pointer position and LVGL state.
	int root_x;
	int root_y;
	lv_indev_state_t state;

	// Authoritative contact tracking across packets.
	bool btn_down;
	bool have_pos;          // MT-A: a full (x, y) arrived during this press
	bool suppress_until_up; // ignore a finger already down when flushed
	bool deferred_release;  // deliver final drag point before lifting
	bool in_syn_dropped;    // discard torn tail until the next SYN_REPORT
	int active_id;          // MT-A: contact id being followed, -1 for any
	int mt_slot;            // MT-B: slot selected by ABS_MT_SLOT
	bool mt_slot0_down;     // MT-B: slot 0 has a non-negative tracking id

	// In-flight contact / packet accumulators (cleared at SYN_REPORT).
	int cur_x;
	int cur_y;
	int cur_id;
	bool cur_has_x;
	bool cur_has_y;

	int pkt_x;
	int pkt_y;
	int pkt_id;
	bool pkt_has_x;
	bool pkt_has_y;
	bool pkt_has_contact;
	bool pkt_matched_active;

	int st_x;
	int st_y;
	bool st_has_x;
	bool st_has_y;
} touch_evdev_t;

static bool uses_button(const touch_evdev_t *dsc) {
	return dsc->caps.has_btn_touch || dsc->caps.has_btn_mouse;
}

static void reset_contact(touch_evdev_t *dsc) {
	dsc->cur_x = 0;
	dsc->cur_y = 0;
	dsc->cur_id = -1;
	dsc->cur_has_x = false;
	dsc->cur_has_y = false;
}

static void reset_packet(touch_evdev_t *dsc) {
	reset_contact(dsc);
	dsc->pkt_x = 0;
	dsc->pkt_y = 0;
	dsc->pkt_id = -1;
	dsc->pkt_has_x = false;
	dsc->pkt_has_y = false;
	dsc->pkt_has_contact = false;
	dsc->pkt_matched_active = false;
	dsc->st_x = 0;
	dsc->st_y = 0;
	dsc->st_has_x = false;
	dsc->st_has_y = false;
}

static int calibrate_axis(int v, int in_min, int in_max, int out_min, int out_max) {
	if (in_min != in_max) {
		v = (int)((int64_t)(v - in_min) * (out_max - out_min) / (in_max - in_min)) + out_min;
	}
	if (v < out_min) {
		return out_min;
	}
	if (v > out_max) {
		return out_max;
	}
	return v;
}

static lv_point_t map_point(lv_indev_t *indev, const touch_evdev_t *dsc) {
	lv_point_t p = {0, 0};
	lv_display_t *disp = lv_indev_get_display(indev);
	if (!disp) {
		return p;
	}
	int ox = lv_display_get_offset_x(disp);
	int oy = lv_display_get_offset_y(disp);
	int w = lv_display_get_horizontal_resolution(disp);
	int h = lv_display_get_vertical_resolution(disp);
	if (w <= 0 || h <= 0) {
		return p;
	}
	p.x = calibrate_axis(dsc->root_x, dsc->min_x, dsc->max_x, ox, ox + w - 1);
	p.y = calibrate_axis(dsc->root_y, dsc->min_y, dsc->max_y, oy, oy + h - 1);
	return p;
}

static void clamp_rel(lv_indev_t *indev, touch_evdev_t *dsc) {
	lv_display_t *disp = lv_indev_get_display(indev);
	if (!disp) {
		return;
	}
	int w = lv_display_get_horizontal_resolution(disp);
	int h = lv_display_get_vertical_resolution(disp);
	if (w > 0) {
		if (dsc->root_x < 0) dsc->root_x = 0;
		if (dsc->root_x >= w) dsc->root_x = w - 1;
	}
	if (h > 0) {
		if (dsc->root_y < 0) dsc->root_y = 0;
		if (dsc->root_y >= h) dsc->root_y = h - 1;
	}
}

// Re-reads the kernel's live button and slot state.
static void sync_device_state(touch_evdev_t *dsc) {
	if (uses_button(dsc)) {
		unsigned long key_bits[BIT_WORDS(KEY_MAX)];
		memset(key_bits, 0, sizeof(key_bits));
		if (ioctl(dsc->fd, EVIOCGKEY(sizeof(key_bits)), key_bits) >= 0) {
			bool down = (dsc->caps.has_btn_touch && test_bit(key_bits, BTN_TOUCH)) ||
						(dsc->caps.has_btn_mouse && test_bit(key_bits, BTN_MOUSE));
			dsc->btn_down = down;
			if (!down) {
				dsc->suppress_until_up = false;
				dsc->have_pos = false;
				dsc->active_id = -1;
				dsc->mt_slot0_down = false;
				dsc->state = LV_INDEV_STATE_RELEASED;
			}
		}
	}

	struct input_absinfo abs;
	if (dsc->caps.has_mt_slot) {
		if (ioctl(dsc->fd, EVIOCGABS(ABS_MT_SLOT), &abs) == 0) {
			dsc->mt_slot = abs.value;
		}
		if (dsc->mt_slot == 0 && ioctl(dsc->fd, EVIOCGABS(ABS_MT_TRACKING_ID), &abs) == 0) {
			dsc->mt_slot0_down = abs.value >= 0;
			if (!uses_button(dsc) && !dsc->mt_slot0_down) {
				dsc->suppress_until_up = false;
				dsc->state = LV_INDEV_STATE_RELEASED;
			}
		}
		if (dsc->mt_slot == 0) {
			if (ioctl(dsc->fd, EVIOCGABS(ABS_MT_POSITION_X), &abs) == 0) {
				dsc->root_x = abs.value;
			}
			if (ioctl(dsc->fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs) == 0) {
				dsc->root_y = abs.value;
			}
		}
	} else if (dsc->caps.has_abs_xy && !dsc->caps.has_mt_xy) {
		if (ioctl(dsc->fd, EVIOCGABS(ABS_X), &abs) == 0) {
			dsc->root_x = abs.value;
		}
		if (ioctl(dsc->fd, EVIOCGABS(ABS_Y), &abs) == 0) {
			dsc->root_y = abs.value;
		}
	}
}

static void finish_mt_a_contact(touch_evdev_t *dsc) {
	if (dsc->cur_has_x || dsc->cur_has_y) {
		bool match = (dsc->active_id >= 0 && dsc->cur_id == dsc->active_id);
		if (match || !dsc->pkt_has_contact) {
			dsc->pkt_x = dsc->cur_x;
			dsc->pkt_y = dsc->cur_y;
			dsc->pkt_id = dsc->cur_id;
			dsc->pkt_has_x = dsc->cur_has_x;
			dsc->pkt_has_y = dsc->cur_has_y;
			dsc->pkt_has_contact = true;
			if (match) {
				dsc->pkt_matched_active = true;
			}
		}
	}
	reset_contact(dsc);
}

static void commit_packet(touch_evdev_t *dsc) {
	if (dsc->caps.has_mt_slot) {
		bool down = uses_button(dsc) ? dsc->btn_down : dsc->mt_slot0_down;
		if (!down) {
			dsc->suppress_until_up = false;
			dsc->mt_slot0_down = false;
			dsc->state = LV_INDEV_STATE_RELEASED;
		} else if (dsc->suppress_until_up) {
			dsc->state = LV_INDEV_STATE_RELEASED;
		} else {
			if (dsc->mt_slot0_down && (dsc->cur_has_x || dsc->cur_has_y)) {
				if (dsc->cur_has_x) dsc->root_x = dsc->cur_x;
				if (dsc->cur_has_y) dsc->root_y = dsc->cur_y;
			} else if (dsc->st_has_x || dsc->st_has_y) {
				if (dsc->st_has_x) dsc->root_x = dsc->st_x;
				if (dsc->st_has_y) dsc->root_y = dsc->st_y;
			}
			dsc->state = LV_INDEV_STATE_PRESSED;
		}
		reset_packet(dsc);
		return;
	}

	if (dsc->caps.has_mt_xy) {
		// Protocol A: fold in a contact whose driver closed the frame with a
		// bare SYN_REPORT rather than SYN_MT_REPORT + SYN_REPORT.
		finish_mt_a_contact(dsc);
		if (!dsc->pkt_has_contact && (dsc->st_has_x || dsc->st_has_y)) {
			dsc->pkt_x = dsc->st_x;
			dsc->pkt_y = dsc->st_y;
			dsc->pkt_id = -1;
			dsc->pkt_has_x = dsc->st_has_x;
			dsc->pkt_has_y = dsc->st_has_y;
			dsc->pkt_has_contact = true;
		}

		if (uses_button(dsc)) {
			if (dsc->btn_down && !dsc->suppress_until_up) {
				if (dsc->pkt_has_contact) {
					// Wait for a complete (x, y) before opening the press: an
					// unpatched cst8xx_touch.ko whose per-contact toggle was
					// left at 1 emits a bare BTN_TOUCH 1 on scan 0 and sends
					// the real coordinates 10 ms later on scan 1.
					if (dsc->have_pos || (dsc->pkt_has_x && dsc->pkt_has_y)) {
						if (dsc->pkt_has_x) dsc->root_x = dsc->pkt_x;
						if (dsc->pkt_has_y) dsc->root_y = dsc->pkt_y;
						dsc->have_pos = true;
					}
					if (dsc->active_id < 0 || !dsc->pkt_matched_active) {
						dsc->active_id = dsc->pkt_id;
					}
				}
				dsc->state = dsc->have_pos ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
			} else {
				// BTN_TOUCH is 0 (or a pre-flush grip is still being ignored):
				// discard any spurious MT coordinates so neither the pressed
				// state nor the last release point moves.
				dsc->state = LV_INDEV_STATE_RELEASED;
			}
		} else if (dsc->pkt_has_contact) {
			if (!dsc->suppress_until_up && (dsc->have_pos || (dsc->pkt_has_x && dsc->pkt_has_y))) {
				if (dsc->pkt_has_x) dsc->root_x = dsc->pkt_x;
				if (dsc->pkt_has_y) dsc->root_y = dsc->pkt_y;
				dsc->have_pos = true;
				if (dsc->active_id < 0 || !dsc->pkt_matched_active) {
					dsc->active_id = dsc->pkt_id;
				}
				dsc->state = LV_INDEV_STATE_PRESSED;
			} else {
				dsc->state = LV_INDEV_STATE_RELEASED;
			}
		} else {
			dsc->suppress_until_up = false;
			dsc->have_pos = false;
			dsc->active_id = -1;
			dsc->state = LV_INDEV_STATE_RELEASED;
		}
		reset_packet(dsc);
		return;
	}

	if (dsc->caps.has_abs_xy) {
		if (!dsc->suppress_until_up) {
			if (dsc->st_has_x) dsc->root_x = dsc->st_x;
			if (dsc->st_has_y) dsc->root_y = dsc->st_y;
		}
		dsc->state = (dsc->btn_down && !dsc->suppress_until_up) ? LV_INDEV_STATE_PRESSED
																: LV_INDEV_STATE_RELEASED;
		reset_packet(dsc);
		return;
	}

	// Relative pointer.
	dsc->state = (dsc->btn_down && !dsc->suppress_until_up) ? LV_INDEV_STATE_PRESSED
															: LV_INDEV_STATE_RELEASED;
	reset_packet(dsc);
}

// Feeds one input_event. Returns true when SYN_REPORT closed a packet.
static bool process_event(lv_indev_t *indev, touch_evdev_t *dsc, const struct input_event *in) {
	if (in->type == EV_SYN) {
		if (in->code == SYN_DROPPED) {
			reset_packet(dsc);
			dsc->in_syn_dropped = true;
			return false;
		}
		if (in->code == SYN_MT_REPORT) {
			if (!dsc->in_syn_dropped && dsc->caps.has_mt_xy && !dsc->caps.has_mt_slot) {
				finish_mt_a_contact(dsc);
			}
			return false;
		}
		if (in->code == SYN_REPORT) {
			if (dsc->in_syn_dropped) {
				dsc->in_syn_dropped = false;
				reset_packet(dsc);
				sync_device_state(dsc);
				return true;
			}
			commit_packet(dsc);
			return true;
		}
		return false;
	}

	if (dsc->in_syn_dropped) {
		return false;
	}

	if (in->type == EV_REL) {
		if (!dsc->suppress_until_up) {
			if (in->code == REL_X) dsc->root_x += in->value;
			else if (in->code == REL_Y) dsc->root_y += in->value;
			clamp_rel(indev, dsc);
		}
		return false;
	}

	if (in->type == EV_ABS) {
		switch (in->code) {
		case ABS_X:
			dsc->st_x = in->value;
			dsc->st_has_x = true;
			break;
		case ABS_Y:
			dsc->st_y = in->value;
			dsc->st_has_y = true;
			break;
		case ABS_MT_SLOT:
			dsc->mt_slot = in->value;
			break;
		case ABS_MT_TRACKING_ID:
			if (dsc->caps.has_mt_slot) {
				if (dsc->mt_slot == 0) {
					dsc->mt_slot0_down = in->value >= 0;
				}
			} else {
				dsc->cur_id = in->value;
			}
			break;
		case ABS_MT_POSITION_X:
			if (!dsc->caps.has_mt_slot || dsc->mt_slot == 0) {
				dsc->cur_x = in->value;
				dsc->cur_has_x = true;
			}
			break;
		case ABS_MT_POSITION_Y:
			if (!dsc->caps.has_mt_slot || dsc->mt_slot == 0) {
				dsc->cur_y = in->value;
				dsc->cur_has_y = true;
			}
			break;
		default:
			break;
		}
		return false;
	}

	if (in->type == EV_KEY && (in->code == BTN_TOUCH || in->code == BTN_MOUSE)) {
		if (in->value != 0) {
			if (!dsc->btn_down) {
				dsc->btn_down = true;
				dsc->have_pos = false;
				dsc->active_id = -1;
			}
		} else {
			dsc->btn_down = false;
			dsc->suppress_until_up = false;
			dsc->have_pos = false;
			dsc->active_id = -1;
			dsc->mt_slot0_down = false;
		}
	}
	return false;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
	touch_evdev_t *dsc = lv_indev_get_driver_data(indev);
	if (!dsc) {
		return;
	}

	if (dsc->deferred_release) {
		dsc->deferred_release = false;
		dsc->state = LV_INDEV_STATE_RELEASED;
		data->point = map_point(indev, dsc);
		data->state = LV_INDEV_STATE_RELEASED;
		data->continue_reading = true;
		return;
	}

	lv_indev_state_t entry_state = dsc->state;
	int entry_x = dsc->root_x;
	int entry_y = dsc->root_y;
	bool moved_pressed = false;
	bool more = false;

	struct input_event in;
	for (;;) {
		ssize_t br = read(dsc->fd, &in, sizeof(in));
		if (br < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (br < (ssize_t)sizeof(in)) {
			break;
		}
		if (!process_event(indev, dsc, &in)) {
			continue;
		}

		// Yield on press/release transitions so a quick tap or flick that
		// both started and ended inside one timer period reaches LVGL intact,
		// while steady drags coalesce to the newest coordinate.
		if (entry_state == LV_INDEV_STATE_RELEASED && dsc->state == LV_INDEV_STATE_PRESSED) {
			more = true;
			break;
		}
		if (entry_state == LV_INDEV_STATE_PRESSED && dsc->state == LV_INDEV_STATE_PRESSED) {
			if (dsc->root_x != entry_x || dsc->root_y != entry_y) {
				moved_pressed = true;
			}
		} else if (entry_state == LV_INDEV_STATE_PRESSED && dsc->state == LV_INDEV_STATE_RELEASED) {
			if (moved_pressed) {
				dsc->state = LV_INDEV_STATE_PRESSED;
				dsc->deferred_release = true;
			}
			more = true;
			break;
		}
	}

	data->point = map_point(indev, dsc);
	data->state = dsc->state;
	data->continue_reading = more;
}

static void touch_delete_cb(lv_event_t *e) {
	lv_indev_t *indev = lv_event_get_user_data(e);
	if (!indev) {
		return;
	}
	touch_evdev_t *dsc = lv_indev_get_driver_data(indev);
	if (!dsc) {
		return;
	}
	lv_indev_set_read_cb(indev, NULL);
	lv_indev_set_driver_data(indev, NULL);
	if (dsc->fd >= 0) {
		close(dsc->fd);
	}
	lv_free(dsc);
}

static bool probe_caps(int fd, touch_evdev_caps_t *out) {
	memset(out, 0, sizeof(*out));

	unsigned long rel_bits[BIT_WORDS(REL_MAX)];
	unsigned long abs_bits[BIT_WORDS(ABS_MAX)];
	unsigned long key_bits[BIT_WORDS(KEY_MAX)];
	memset(rel_bits, 0, sizeof(rel_bits));
	memset(abs_bits, 0, sizeof(abs_bits));
	memset(key_bits, 0, sizeof(key_bits));

	if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) >= 0) {
		out->has_rel_xy = test_bit(rel_bits, REL_X) && test_bit(rel_bits, REL_Y);
	}
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0) {
		out->has_abs_xy = test_bit(abs_bits, ABS_X) && test_bit(abs_bits, ABS_Y);
		out->has_mt_xy = test_bit(abs_bits, ABS_MT_POSITION_X) && test_bit(abs_bits, ABS_MT_POSITION_Y);
		out->has_mt_slot = test_bit(abs_bits, ABS_MT_SLOT);
	}
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
		out->has_btn_touch = test_bit(key_bits, BTN_TOUCH);
		out->has_btn_mouse = test_bit(key_bits, BTN_MOUSE);
	}

	if (!out->has_mt_xy && !out->has_abs_xy && !out->has_rel_xy) {
		return false;
	}

	// Only ABS_X / ABS_Y set the default scale, matching lv_evdev and gbinput:
	// cst8xx_touch.ko advertises ABS_MT_POSITION_X/Y as 0..720 x 0..1280 while
	// delivering raw panel coordinates (0..239 x 0..319 on the V1).
	struct input_absinfo abs;
	if (out->has_abs_xy) {
		if (ioctl(fd, EVIOCGABS(ABS_X), &abs) == 0) {
			out->min_x = abs.minimum;
			out->max_x = abs.maximum;
		}
		if (ioctl(fd, EVIOCGABS(ABS_Y), &abs) == 0) {
			out->min_y = abs.minimum;
			out->max_y = abs.maximum;
		}
	}
	return true;
}

lv_indev_t *touch_evdev_create(const char *dev_path) {
	if (!dev_path) {
		return NULL;
	}
	int fd = open(dev_path, O_RDONLY | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}
	touch_evdev_caps_t caps;
	if (!probe_caps(fd, &caps)) {
		close(fd);
		return NULL;
	}
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(fd);
		return NULL;
	}

	touch_evdev_t *dsc = lv_malloc_zeroed(sizeof(*dsc));
	if (!dsc) {
		close(fd);
		return NULL;
	}
	dsc->fd = fd;
	dsc->caps = caps;
	dsc->min_x = caps.min_x;
	dsc->min_y = caps.min_y;
	dsc->max_x = caps.max_x;
	dsc->max_y = caps.max_y;
	dsc->state = LV_INDEV_STATE_RELEASED;
	dsc->active_id = -1;
	reset_packet(dsc);
	sync_device_state(dsc);
	if (dsc->btn_down || dsc->mt_slot0_down) {
		dsc->suppress_until_up = true;
	}

	lv_indev_t *indev = lv_indev_create();
	if (!indev) {
		lv_free(dsc);
		close(fd);
		return NULL;
	}
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_driver_data(indev, dsc);
	lv_indev_set_read_cb(indev, touch_read_cb);
	lv_indev_add_event_cb(indev, touch_delete_cb, LV_EVENT_DELETE, indev);
	return indev;
}

void touch_evdev_set_calibration(lv_indev_t *indev, int min_x, int min_y, int max_x, int max_y) {
	if (!indev) {
		return;
	}
	touch_evdev_t *dsc = lv_indev_get_driver_data(indev);
	if (!dsc) {
		return;
	}
	dsc->min_x = min_x;
	dsc->min_y = min_y;
	dsc->max_x = max_x;
	dsc->max_y = max_y;
}

bool touch_evdev_flush(lv_indev_t *indev) {
	if (!indev) {
		return false;
	}
	touch_evdev_t *dsc = lv_indev_get_driver_data(indev);
	if (!dsc) {
		return false;
	}

	dsc->deferred_release = false;
	struct input_event in;
	for (;;) {
		ssize_t br = read(dsc->fd, &in, sizeof(in));
		if (br < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (br < (ssize_t)sizeof(in)) {
			break;
		}
		(void)process_event(indev, dsc, &in);
	}

	dsc->in_syn_dropped = false;
	reset_packet(dsc);
	sync_device_state(dsc);

	bool held = false;
	if (uses_button(dsc)) {
		held = dsc->btn_down;
	} else if (dsc->caps.has_mt_slot) {
		held = dsc->mt_slot0_down;
	} else {
		held = (dsc->state == LV_INDEV_STATE_PRESSED);
	}

	dsc->suppress_until_up = held;
	dsc->have_pos = false;
	dsc->active_id = -1;
	dsc->state = LV_INDEV_STATE_RELEASED;
	return held;
}

#else

lv_indev_t *touch_evdev_create(const char *dev_path) {
	(void)dev_path;
	return NULL;
}

void touch_evdev_set_calibration(lv_indev_t *indev, int min_x, int min_y, int max_x, int max_y) {
	(void)indev;
	(void)min_x;
	(void)min_y;
	(void)max_x;
	(void)max_y;
}

bool touch_evdev_flush(lv_indev_t *indev) {
	(void)indev;
	return false;
}

#endif
