#include "chargescreen.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

#ifdef HOST_BUILD
#include <SDL2/SDL.h>
#endif

#include "src/gui/fonts/fonts.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/device/backlight.h"
#include "src/system/device/usb.h"

// ---------------------------------------------------------------------------
// The screen
//
// Dark, and nothing else on it: a headphone player charging with its screen on
// is a lamp, and this one is going to be looked at across a room and then left
// alone. Three lines under the battery carry the rest.
// ---------------------------------------------------------------------------

// How long the panel stays up after the screen is shown or the key is pressed.
// [power] charge_screen_seconds overrides it; below the floor it would be up so
// briefly that the press that woke it would look like it did nothing.
#define SCREEN_ON_DEFAULT_S 10
#define SCREEN_ON_MIN_S 5

// The hold that starts the player: the same 1.2 s the power menu's long press
// waits for (POWER_LONG_PRESS_MS in system.c), so the gesture is the same one
// the player uses everywhere else.
#define POWER_HOLD_MS 1200

// The gauge and the cable. The gauge once a second while the screen is up and
// every two while it is dark -- charging is a slow process, and a percentage
// that moves while you watch is worth the reads.
#define GAUGE_POLL_MS 1000
#define GAUGE_POLL_DARK_MS 2000

// How long the cable has to stay out before the player switches itself off. A
// plug moved in its socket breaks the contact for a moment, and switching off
// in that moment is not what anybody asked for.
#define UNPLUG_GRACE_MS 3000

// The panel, as power.c writes it. The wake's own settle is repeated here (see
// the long note over power_screen_on() in power.h): this panel leaves its
// memory white on reset and loses any frame pushed before it is listening, so
// the backlight is held at zero until a frame drawn after the unblank has been
// scanned out.
#define BRIGHTNESS_PATH "/sys/class/backlight/backlight_pwm0/brightness"
#define MAX_BRIGHTNESS_PATH "/sys/class/backlight/backlight_pwm0/max_brightness"
#define BLANK_PATH "/sys/class/graphics/fb0/blank"
#define FB_BLANK_UNBLANK 0
#define FB_BLANK_POWERDOWN 4
#define WAKE_SETTLE_MS 180

// The battery's own colours: the green a charge in progress has on the status
// bar, the amber that means nearly, and the red that means now.
#define CHARGE_GREEN lv_color_make(0x2e, 0xc2, 0x7e)
#define CHARGE_AMBER lv_color_make(0xe5, 0xa5, 0x14)
#define CHARGE_RED lv_color_make(0xe0, 0x1b, 0x24)
#define CHARGE_DIM lv_color_make(0x9a, 0x9a, 0xa0)

static lv_display_t *display;
static lv_obj_t *root;
static lv_obj_t *fill;
static lv_obj_t *percent_label;
static lv_obj_t *status_label;
static int fill_width; // the inside of the battery, where the fill travels
static long on_level = -1; // the backlight the screen is meant to be seen at
static bool screen_lit = true;

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000) + (uint32_t)(ts.tv_nsec / 1000000);
}

// --- the backlight and the blank node ---------------------------------------

static long read_long_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	long value = -1;
	if (fscanf(f, "%ld", &value) != 1) {
		value = -1;
	}
	fclose(f);
	return value;
}

#ifndef HOST_BUILD
static void write_long_file(const char *path, long value) {
	FILE *f = fopen(path, "w");
	if (!f) {
		return;
	}
	fprintf(f, "%ld", value);
	fclose(f);
}
#endif

// The level the screen comes up at: the brightness chosen in Settings, or
// whatever the panel was left at, and never a level too dim to read (the same
// floor power.c adopts -- a previous session that ended with the screen off
// leaves the backlight at zero).
static long screen_brightness(void) {
	long max = read_long_file(MAX_BRIGHTNESS_PATH);
	if (max <= 0) {
		max = BACKLIGHT_MAX_DEFAULT;
	}
	long wanted = config_get_int("screen", "brightness", -1);
	if (wanted <= 0) {
		long current = read_long_file(BRIGHTNESS_PATH);
		wanted = current >= 20 ? current : max;
	}
	// This screen runs before power_init(), so it must apply the same guard
	// itself, including to a saved 100% level and the adopt/fallback paths.
	return backlight_safe_level(wanted, max);
}

static void panel_off(void) {
	if (!screen_lit) {
		return;
	}
	screen_lit = false;

#ifndef HOST_BUILD
	// Backlight first, so the blank that follows is not seen as a flash.
	write_long_file(BRIGHTNESS_PATH, 0);
	write_long_file(BLANK_PATH, FB_BLANK_POWERDOWN);
#endif

	// Nothing is being scanned out, so there is nothing to paint either: the
	// refresh timer is the one thing that would keep the core awake for a
	// frame nobody can see.
	lv_timer_t *refr = lv_display_get_refr_timer(display);
	if (refr) {
		lv_timer_pause(refr);
	}

#ifdef HOST_BUILD
	// The simulator has no panel to blank. Taking the screen off the window is
	// the same picture from the outside: black, and the loop still pumping SDL
	// so the power key still arrives.
	lv_obj_set_hidden(root, true);
	lv_refr_now(display);
#endif
}

static void panel_on(void) {
	if (screen_lit) {
		return;
	}
	screen_lit = true;

#ifdef HOST_BUILD
	lv_obj_set_hidden(root, false);
#else
	// Unblank, and hold the backlight dark through the panel's own init: the
	// glass is white until the frame after it, and the backlight at zero is
	// what keeps that from being a flash.
	write_long_file(BLANK_PATH, FB_BLANK_UNBLANK);
	write_long_file(BRIGHTNESS_PATH, 0);
	usleep(WAKE_SETTLE_MS * 1000);
#endif

	lv_timer_t *refr = lv_display_get_refr_timer(display);
	if (refr) {
		lv_timer_resume(refr);
	}

	// The frame goes into the framebuffer inside the display's own wake
	// bracket, then the backlight comes up over a picture that is already
	// there.
	display_wake_begin(display);
	lv_obj_invalidate(lv_screen_active());
	lv_refr_now(display);
	display_wake_end(display);

#ifndef HOST_BUILD
	if (on_level > 0) {
		write_long_file(BRIGHTNESS_PATH, on_level);
	}
#endif
}

// --- drawing ----------------------------------------------------------------

void chargescreen_build(lv_display_t *disp) {
	if (disp) {
		display = disp;
	}
	int width = (int)lv_display_get_horizontal_resolution(display);
	int height = (int)lv_display_get_vertical_resolution(display);

	root = lv_obj_create(lv_screen_active());
	lv_obj_set_size(root, width, height);
	lv_obj_set_pos(root, 0, 0);
	lv_obj_set_style_bg_color(root, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(root, 0, 0);
	lv_obj_set_style_radius(root, 0, 0);
	lv_obj_set_style_pad_all(root, 0, 0);
	lv_obj_set_scrollable(root, false);
	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(root, 20, 0);

	// The battery: a bordered box with a nub on its right, and inside it a bar
	// whose width is the level. Sizes follow the panel, so the same drawing
	// works on the V1's 240x320 and on the wider panels of other boards.
	int battery_w = width * 55 / 100;
	int battery_h = battery_w * 45 / 100;
	int border = 3;
	int inset = 6;
	int nub_w = battery_w * 7 / 100;
	if (nub_w < 6) {
		nub_w = 6;
	}
	fill_width = battery_w - 2 * border - 2 * inset;

	lv_obj_t *row = lv_obj_create(root);
	lv_obj_set_size(row, battery_w + nub_w, battery_h);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_radius(row, 0, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *body = lv_obj_create(row);
	lv_obj_set_size(body, battery_w, battery_h);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(body, border, 0);
	lv_obj_set_style_border_color(body, lv_color_white(), 0);
	lv_obj_set_style_radius(body, 10, 0);
	lv_obj_set_style_pad_all(body, inset, 0);
	lv_obj_set_style_shadow_width(body, 0, 0);
	lv_obj_set_scrollable(body, false);

	// The fill is a child of the box, so the border and the inset are already
	// accounted for: it starts at nothing and grows to the right.
	fill = lv_obj_create(body);
	lv_obj_set_height(fill, lv_pct(100));
	lv_obj_set_width(fill, 0);
	lv_obj_set_style_bg_color(fill, CHARGE_GREEN, 0);
	lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(fill, 0, 0);
	lv_obj_set_style_radius(fill, 4, 0);
	lv_obj_set_style_pad_all(fill, 0, 0);
	lv_obj_set_style_shadow_width(fill, 0, 0);
	lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);

	lv_obj_t *nub = lv_obj_create(row);
	lv_obj_set_size(nub, nub_w, battery_h * 36 / 100);
	lv_obj_set_style_bg_color(nub, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(nub, 0, 0);
	lv_obj_set_style_radius(nub, 3, 0);
	lv_obj_set_style_pad_all(nub, 0, 0);
	lv_obj_set_style_shadow_width(nub, 0, 0);

	// Both lines are given the panel's width and told to wrap: the three
	// states are short in English and not in Ukrainian ("Зарядний пристрій
	// підключено" is wider than the glass at this font), and a label that sizes
	// itself to its text would simply leave the screen.
	int text_w = width - 2 * (width / 10);

	percent_label = lv_label_create(root);
	lv_obj_set_width(percent_label, text_w);
	lv_obj_set_style_text_font(percent_label, &font_ui_36_bold, 0);
	lv_obj_set_style_text_color(percent_label, lv_color_white(), 0);
	lv_obj_set_style_text_align(percent_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(percent_label, LV_LABEL_LONG_WRAP);
	lv_label_set_text(percent_label, "--");

	status_label = lv_label_create(root);
	lv_obj_set_width(status_label, text_w);
	lv_obj_set_style_text_font(status_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(status_label, CHARGE_DIM, 0);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_label_set_text(status_label, tr("chargescreen_charging"));

	lv_obj_t *hint = lv_label_create(root);
	lv_obj_set_style_text_font(hint, &font_ui_18, 0);
	lv_obj_set_style_text_color(hint, CHARGE_DIM, 0);
	lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(hint, text_w);
	lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
	lv_label_set_text(hint, tr("chargescreen_hold_to_start"));
}

// Paints one reading. `have` false means the gauge said nothing at all, which
// is shown as such rather than as a flat battery: a reading of nothing and an
// empty cell are different things to anybody looking at this screen.
void chargescreen_show(bool have, int percent, bool charging, bool cable) {
	static int shown_percent = -2;
	static int shown_state = -2;

	// 0 no cable, 1 a cable with nothing going into the cell (full, or held at
	// the configured charge limit), 2 a charge in progress.
	int state = !cable ? 0 : (charging ? 2 : 1);

	if (percent != shown_percent || state != shown_state) {
		shown_percent = percent;
		shown_state = state;

		char text[16];
		if (!have || percent < 0) {
			lv_label_set_text(percent_label, "--");
			lv_obj_set_width(fill, 0);
		} else {
			snprintf(text, sizeof(text), "%d%%", percent);
			lv_label_set_text(percent_label, text);

			lv_obj_set_width(fill, (int32_t)fill_width * percent / 100);
			lv_obj_set_style_bg_color(fill, percent <= 10 ? CHARGE_RED : (percent <= 20 ? CHARGE_AMBER : CHARGE_GREEN), 0);
		}

		lv_label_set_text(status_label, tr(state == 2   ? "chargescreen_charging"
										   : state == 1 ? "chargescreen_connected"
														: "chargescreen_unplugged"));
	}
}

// --- the power key -----------------------------------------------------------

#ifndef HOST_BUILD
// The node the panel's power button reports on. Its number is not fixed -- the
// drivers probe in whatever order they do -- so the node is found by what it
// can report: a key bit for KEY_POWER and a name that is not the headset's.
// The GPIO key node wins when two of them qualify, which on this device is the
// power button itself ("md-gpio-keys") rather than the touch controller's
// double-tap gesture.
static int open_power_key(void) {
	DIR *dir = opendir("/dev/input");
	if (!dir) {
		return -1;
	}

	int best = -1;
	bool best_is_gpio = false;
	struct dirent *e;
	while ((e = readdir(dir)) != NULL) {
		if (strncmp(e->d_name, "event", 5) != 0) {
			continue;
		}

		char node[32];
		if ((size_t)snprintf(node, sizeof(node), "/dev/input/%s", e->d_name) >= sizeof(node)) {
			continue;
		}

		int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}

		unsigned long keys[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
		memset(keys, 0, sizeof(keys));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
			!(keys[KEY_POWER / (8 * sizeof(unsigned long))] & (1ul << (KEY_POWER % (8 * sizeof(unsigned long)))))) {
			close(fd);
			continue;
		}

		char name[64] = "";
		ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
		if (strncmp(name, "earpods_", 8) == 0) {
			close(fd); // the cable remote's centre button is not the power key
			continue;
		}

		bool is_gpio = strstr(name, "gpio") != NULL;
		if (best < 0 || (is_gpio && !best_is_gpio)) {
			if (best >= 0) {
				close(best);
			}
			best = fd;
			best_is_gpio = is_gpio;
		} else {
			close(fd);
		}

		if (best_is_gpio) {
			break;
		}
	}

	closedir(dir);
	return best;
}
#else
// The simulator: the keyboard's P is the power button (see main.c's
// HOST_BUTTONS). SDL's event watch cannot be called from the loop directly --
// it is invoked while SDL pumps events -- so it only raises a flag, and the
// loop reads it after lv_timer_handler() has pumped them.
static volatile int host_press_seen;
static volatile int host_release_seen;

static int host_key_watch(void *userdata, SDL_Event *event) {
	(void)userdata;
	if ((event->type != SDL_KEYDOWN && event->type != SDL_KEYUP) || event->key.repeat) {
		return 1;
	}
	if (event->key.keysym.sym != SDLK_p) {
		return 1;
	}
	if (event->type == SDL_KEYDOWN) {
		host_press_seen = 1;
	} else {
		host_release_seen = 1;
	}
	return 1;
}
#endif

// --- the loop ---------------------------------------------------------------

void chargescreen_run(lv_display_t *disp, const battery_config_t *battery_cfg) {
	if (!disp) {
		return;
	}
	display = disp;

#ifndef HOST_BUILD
	int key_fd = open_power_key();
	if (key_fd < 0) {
		// No key to read means no way out of this screen but the battery going
		// flat. Handing over to the player is the only sane answer.
		fprintf(stderr, "chargescreen: no power key node; starting the player instead\n");
		return;
	}
#else
	SDL_AddEventWatch(host_key_watch, NULL);
#endif

	on_level = screen_brightness();
	long seconds = config_get_int("power", "charge_screen_seconds", SCREEN_ON_DEFAULT_S);
	if (seconds < SCREEN_ON_MIN_S) {
		seconds = SCREEN_ON_MIN_S;
	}
	const uint32_t screen_on_ms = (uint32_t)seconds * 1000u;

	chargescreen_build(NULL);
	// The panel may have come up blanked, or lit at a stale level from the
	// session that switched the player off. Starting from "dark" and running
	// the ordinary wake puts it right and draws the first frame where the
	// panel can still see it.
	screen_lit = false;
	panel_on();

	printf("chargescreen: up (screen %ld s, brightness %ld)\n", (long)seconds, on_level);
	fflush(stdout);

	bool have = false;
	int percent = -1;
	bool charging = false;
	bool cable = true;

	bool pressed = false;
	uint32_t press_at = 0;
	uint32_t last_activity = now_ms();
	uint32_t next_gauge = last_activity;
	uint32_t unplugged_since = 0;

	for (;;) {
		uint32_t now = now_ms();

		// 1. the gauge and the cable
		if ((int32_t)(now - next_gauge) >= 0) {
			have = battery_read_now(battery_cfg, &percent, &charging);
#ifdef HOST_BUILD
			cable = true; // a PC has no charger to take away
#else
			cable = usb_vbus_present();
#endif
			if (!cable) {
				if (!unplugged_since) {
					unplugged_since = now ? now : 1;
				}
			} else {
				unplugged_since = 0;
			}

			chargescreen_show(have, percent, charging, cable);

			if (screen_lit) {
				lv_refr_now(display); // the four objects that may have moved
			}

			next_gauge = now + (screen_lit ? GAUGE_POLL_MS : GAUGE_POLL_DARK_MS);
		}

		// 2. the key. A press brings a dark screen back; a release that comes
		//    before the hold has done all it is going to do. Anything at all
		//    counts as the user being there, and pushes the screen-off back.
		bool was_pressed = false;
		bool was_released = false;
		int wait_ms = 250;
#ifndef HOST_BUILD
		struct pollfd pfd = {.fd = key_fd, .events = POLLIN};
		if (pressed) {
			int32_t left = (int32_t)(press_at + POWER_HOLD_MS - now);
			if (left < wait_ms) {
				wait_ms = left > 0 ? left : 0;
			}
		}
		if ((int32_t)(next_gauge - now) < wait_ms) {
			int32_t left = (int32_t)(next_gauge - now);
			wait_ms = left > 0 ? left : 0;
		}
		if (screen_lit) {
			int32_t left = (int32_t)(last_activity + screen_on_ms - now);
			if (left < wait_ms) {
				wait_ms = left > 0 ? left : 0;
			}
		}
		if (poll(&pfd, 1, wait_ms) > 0) {
			struct input_event events[16];
			ssize_t got;
			while ((got = read(key_fd, events, sizeof(events))) > 0) {
				for (ssize_t i = 0; i < got / (ssize_t)sizeof(events[0]); i++) {
					if (events[i].type != EV_KEY || events[i].code != KEY_POWER) {
						continue;
					}
					if (events[i].value == 1) {
						was_pressed = true;
					} else if (events[i].value == 0) {
						was_released = true;
					}
				}
			}
		}
#else
		// Pumping SDL is what delivers the key: the LVGL SDL driver reads its
		// events from its own timer, which only runs inside lv_timer_handler().
		lv_timer_handler();
		wait_ms = 20;
		was_pressed = host_press_seen != 0;
		was_released = host_release_seen != 0;
		host_press_seen = 0;
		host_release_seen = 0;
#endif
		now = now_ms();

		if (was_pressed && !pressed) {
			pressed = true;
			press_at = now;
			last_activity = now;
			panel_on(); // a dark screen is what the press is for
		}
		if (was_released) {
			pressed = false;
			last_activity = now;
		}

		if (pressed && (int32_t)(now - press_at) >= POWER_HOLD_MS) {
			printf("chargescreen: power held -- starting the player\n");
			fflush(stdout);
			break;
		}

		// 3. the screen's own timeout, and the cable that went away under it
		if (screen_lit && (int32_t)(now - last_activity) >= (int32_t)screen_on_ms) {
			printf("chargescreen: screen off (%ld s without a press)\n", (long)seconds);
			panel_off();
		}

		if (!cable && !screen_lit && unplugged_since && (int32_t)(now - unplugged_since) >= UNPLUG_GRACE_MS) {
			printf("chargescreen: the charger is gone and the screen is dark -- switching off\n");
			fflush(stdout);
#ifndef HOST_BUILD
			sync();
			reboot(RB_POWER_OFF);
#endif
			break; // the host build has nothing to switch off
		}

#ifdef HOST_BUILD
		// The target waits in poll(); the simulator has the same wait to do by
		// hand, and needs it: without one this loop would spin the core.
		usleep((useconds_t)(wait_ms > 0 ? wait_ms : 1) * 1000u);
#endif
	}

	// Back to the player: the panel is left lit at its proper level, and the
	// screen comes off the display. main.c's ordinary startup puts up its own
	// first frame a moment later.
	lv_obj_delete(root);
	root = NULL;
	lv_timer_t *refr = lv_display_get_refr_timer(display);
	if (refr) {
		lv_timer_resume(refr);
	}
	lv_refr_now(display);

#ifndef HOST_BUILD
	close(key_fd);
#else
	SDL_DelEventWatch(host_key_watch, NULL);
#endif
}
