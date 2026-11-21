#include "led.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "led_i2c.h"
#include "src/system/audio/visualizer.h"
#include "src/system/core/config.h"
#include "src/system/device/sysinfo.h"

#define PATTERN_NODE "/sys/class/leds/sgm31324-leds/led_pattern"
#define RED_TRIGGER_NODE "/sys/class/leds/red/trigger"
#define RED_BRIGHTNESS_NODE "/sys/class/leds/red/brightness"

// The R1's blue LED (leds_pwm_add.ko, PC02, max_brightness 100). Its stock
// player writes 50 for every lit state and 0 for off.
#define BLUE_BRIGHTNESS_NODE "/sys/class/leds/blue/brightness"
#define BLUE_ON "50"

// Pattern numbers, as registered by leds_sgm31324_add.sh and used by the
// stock binary (FUN_0047ce00's logical->pattern table). See led.h for the
// full story. They are the fallback path: on the SGM31324 the controller is
// taken over and driven directly (see led_i2c.h), and these numbers are only
// written when that fails or on the R1, which has no such controller.
#define PATTERN_IDLE_AQUA 1
#define PATTERN_CHARGING_RED 6
#define PATTERN_PCM_48K 2
#define PATTERN_PCM_192K 3
#define PATTERN_PCM_HI 4
#define PATTERN_DSD_WHITE 11
#define PATTERN_OFF 5 // the logical-0 state: all registers zero
// Podcast purple: pattern 13, added by hand to the driver script by the user,
// like 12 for transfers. (The first attempt used 10, the stock MQA rendering
// pattern, which lit up magenta rather than purple.)
#define PATTERN_PODCAST_PURPLE 13
// Wi-Fi transfer: pattern 12, added by hand to the driver script
// (leds_sgm31324_add.sh, regs="000E7D00686601010706"). Lit while a transfer is
// active, and never turned off by the standby option: with the screen dark it
// is the only sign the transfer server is running.
#define PATTERN_WIFI_TRANSFER 12
// Bluetooth receiver: pattern 14, blue at full brightness, added by hand to the
// driver script like 12 and 13. Lit for as long as the mode is on, screen off
// included -- it is the only sign the player is being used as a pair of
// headphones.
#define PATTERN_BT_RECEIVER 14

// With the "LED off in standby" option on, this long without anything playing
// turns the LED off entirely.
#define IDLE_OFF_SECONDS 20

// The controller's 11-step current ladder (docs/sgm31324-led-control.md). Level
// 0 is off and each step scales a channel's 0-255 value down to 0-79.
static const int LADDER[11] = {0, 1, 3, 7, 15, 23, 31, 39, 47, 63, 79};

// The brightness slider runs 1-10 and is the level index itself, exactly as
// the doc's `led LEVEL ...` takes it: brightness 10 is `led 10`. Level 0 is
// off and is not offered -- Stealth and the LED switch are the ways to turn
// the light out. The default stays at 5 (half the ladder) so an update does
// not suddenly make an existing player's light brighter.
#define BRIGHTNESS_MIN 1
#define BRIGHTNESS_MAX 10
#define BRIGHTNESS_DEFAULT 5

// The transition speed slider runs 20 ms to 2 s in 10 ms steps; this is the
// length of a fade-in, fade-out or cross-fade.
#define TRANSITION_MIN_MS 20
#define TRANSITION_MAX_MS 2000
#define TRANSITION_DEFAULT_MS 500

// Software fades step every 10 ms, so a transition is its length in steps.
#define LED_STEP_MS 10
// A blinking state (the Wi-Fi transfer) is on and off for this long each.
#define LED_BLINK_MS 500
#define LED_VISUALIZER_MS 40 // at most 25 brightness updates per second
#define RAINBOW_SECTOR_FRAMES 40 // 9.6 s for all six hue sectors
#define RAINBOW_HUE_FRAMES (6 * RAINBOW_SECTOR_FRAMES)
#define RAINBOW_HALF_BREATH_FRAMES 48 // 3.84 s for a rise and fall

// The logical states the LED can be in. Each is rendered either as a driver
// pattern (the fallback) or as a colour and an effect (the direct path).
typedef enum {
	LS_OFF,
	LS_STEALTH, // cut to black immediately, even during a charge
	LS_IDLE,
	LS_PCM_48K, // PCM at or below 48 kHz
	LS_PCM_192K, // PCM above 48 through 192 kHz
	LS_PCM_HI, // PCM above 192 kHz
	LS_DSD,
	LS_PODCAST,
	LS_WIFI,
	LS_BT,
	LS_CHARGING,
	LS_CHARGED,
} led_state_t;

// How a state is played on the controller.
typedef enum {
	LM_STEADY, // a single colour, reached by a cross-fade
	LM_BREATHE, // the controller's own breathing engine
	LM_BLINK, // software on/off blink
	LM_OFF, // dark, reached by a fade-out
} led_mode_t;

typedef struct {
	led_mode_t mode;
	int r, g, b;		   // scaled 0-79
	int period, duty, fade; // breathing timing
	int transition_ms;	   // length of the fade into this state
	led_visualizer_style_t effect;
	bool allow_notices;
} led_render_t;

// Priority order. There is no queue: a low-battery warning can replace an
// upload confirmation, but neither is replayed after a higher priority state.
typedef enum {
	LN_NONE,
	LN_UPLOAD_COMPLETE,
	LN_UPDATE_AVAILABLE,
	LN_LOW_BATTERY,
} led_notice_t;

static bool have_pattern_node;
static bool have_red_node;
static bool have_blue_node; // the R1: no pattern node, a red and a blue LED
static bool direct_mode;	// the effect worker owns the SGM31324

// Whether this player's status LED is the SGM31324 RGB controller. The model
// table knows (sysinfo.h); when system-info.json names no model we fall back to
// the driver's pattern node, which only exists where the controller does. This
// is what lets the controller be driven directly even when the firmware's
// leds_sgm31324_add.ko is never loaded at all.
static bool has_sgm31324(void) {
	const sysinfo_model_t *model = sysinfo_model();
	if (model) {
		return model->sgm31324_led;
	}
	return have_pattern_node;
}

static int current_state = -1;
static led_visualizer_style_t current_effect;
static bool current_notices;
static int red_trigger_state = -1; // -1 unknown, 0 "none", 1 "breathing"

static int s_brightness = BRIGHTNESS_DEFAULT;
static int s_transition_ms = TRANSITION_DEFAULT_MS;

// Inputs to the state resolver. The GUI/power thread owns these, and only
// the effect worker writes to the direct controller once it is started.
static bool s_charging;
static bool s_playing;
static int s_rate;
static bool s_podcast;		 // what is playing is a podcast episode
static bool s_dsd;			 // ...or a DSD file, whatever its carrier rate
static bool s_wifi_transfer; // a Wi-Fi transfer is active
static bool s_bt_receiver;	 // Bluetooth receiver mode is on
static bool s_dac_active;	 // DAC mode is on: the gadget is up
static bool s_dac_streaming; // ...and the computer is actually sending audio
static int s_dac_rate;

static bool s_led_enabled = true; // the LED master switch, with its charge exception
static bool s_stealth;
static bool s_visualizer;
static led_visualizer_style_t s_visualizer_style = LED_VISUALIZER_BASS_PULSE;
static bool s_suspended; // includes the shutdown fade, until/unless we resume
static bool s_idle_off_enabled;
static bool s_standby;		  // screen off (set by power.c)
static time_t s_standby_since; // when the screen went off
static time_t s_last_playing; // when something was last playing
static bool s_on_charger;	  // a cable is supplying, charge finished or not
static time_t s_charger_since; // when that last changed

// --- the effect thread -----------------------------------------------------
//
// The fades are CPU-driven walks of the controller's levels, so they cannot run
// on the GUI thread. One worker owns the I2C bus and plays whatever target the
// state machine last posted; a new target cuts a running fade short rather than
// queueing behind it, which is what makes a colour change feel immediate.

static pthread_t led_thread;
static pthread_mutex_t led_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t led_cond;
static clockid_t led_clock = CLOCK_REALTIME;
static pthread_cond_t led_done_cond = PTHREAD_COND_INITIALIZER;
static led_render_t led_target;
// The target, notice and their generations are all protected by led_lock.
// Base changes during a notice update the state it returns to, but do not
// restart or cut the pattern unless they disallow notices altogether.
static unsigned led_seq, led_done_seq;
static led_notice_t led_notice;
static unsigned led_notice_seq;
static int led_notice_level;

static void write_str(const char *path, const char *value) {
	FILE *f = fopen(path, "w");
	if (!f) {
		return;
	}
	fputs(value, f);
	fclose(f);
}

static int led_level(void) {
	int b = s_brightness;
	if (b < BRIGHTNESS_MIN) {
		b = BRIGHTNESS_MIN;
	}
	if (b > BRIGHTNESS_MAX) {
		b = BRIGHTNESS_MAX;
	}
	return b;
}

// Scales a 0-255 channel through the level ladder. White is run 40% dimmer --
// the white LED reads brighter than the coloured ones -- but a channel that was
// lit is never dimmed all the way to zero, which would put the light out rather
// than dim it.
static void scale_rgb(int r, int g, int b, bool white, int *out_r, int *out_g, int *out_b) {
	int c = LADDER[led_level()];
	int sr = r * c / 255;
	int sg = g * c / 255;
	int sb = b * c / 255;
	if (white) {
		if (sr > 0) {
			sr = sr * 60 / 100;
			if (sr < 1) {
				sr = 1;
			}
		}
		if (sg > 0) {
			sg = sg * 60 / 100;
			if (sg < 1) {
				sg = 1;
			}
		}
		if (sb > 0) {
			sb = sb * 60 / 100;
			if (sb < 1) {
				sb = 1;
			}
		}
	}
	*out_r = sr;
	*out_g = sg;
	*out_b = sb;
}

static unsigned ch_mask(int r, int g, int b) {
	unsigned m = 0;
	if (r > 0) {
		m |= LED_CH_R;
	}
	if (g > 0) {
		m |= LED_CH_G;
	}
	if (b > 0) {
		m |= LED_CH_B;
	}
	return m;
}

static void led_post(const led_render_t *t, bool redraw) {
	pthread_mutex_lock(&led_lock);
	led_target = *t;
	if (!t->allow_notices && led_notice != LN_NONE) {
		led_notice = LN_NONE;
		led_notice_seq++;
		redraw = true;
	}
	if (redraw) {
		led_seq++;
		pthread_cond_signal(&led_cond);
	}
	pthread_mutex_unlock(&led_lock);
}

static void post_notice(led_notice_t notice) {
	if (!direct_mode) {
		return;
	}
	pthread_mutex_lock(&led_lock);
	// An update notice replaces an update notice still playing: each release
	// found is its own blink, and a fresh tag must never be swallowed by the
	// tail of its predecessor. The other kinds keep the stricter rule, where a
	// lower-priority notice cannot take a higher-priority blink over.
	bool replace = notice > led_notice
		|| (notice == LN_UPDATE_AVAILABLE && led_notice == LN_UPDATE_AVAILABLE);
	if (led_target.allow_notices && replace) {
		led_notice = notice;
		led_notice_level = LADDER[led_level()];
		led_notice_seq++;
		led_seq++;
		pthread_cond_signal(&led_cond);
	}
	pthread_mutex_unlock(&led_lock);
}

// Wait once per frame, not in short polling slices. A relevant target or
// notice change wakes the effect early, even during a long blink interval.
static bool led_wait(const unsigned *seq, unsigned seen, int ms) {
	struct timespec until;
	clock_gettime(led_clock, &until);
	until.tv_nsec += (long)ms * 1000000L;
	if (until.tv_nsec >= 1000000000L) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&led_lock);
	while (*seq == seen && pthread_cond_timedwait(&led_cond, &led_lock, &until) == 0) {}
	bool changed = *seq != seen;
	pthread_mutex_unlock(&led_lock);
	return changed;
}

typedef struct {
	int r, g, b;
	unsigned channels;
} led_levels_t;

static void set_levels(led_levels_t *cur, int r, int g, int b, unsigned ch) {
	led_i2c_levels(r, g, b, ch);
	*cur = (led_levels_t){r, g, b, ch};
}

// Zero repeats means the ongoing blue Wi-Fi Transfer pattern. Notices instead
// run a finite number of beats and then return to the latest base target.
static void play_blink(led_levels_t *cur, int r, int g, int b, int repeats, int on_ms, int off_ms,
					   const unsigned *seq, unsigned seen) {
	unsigned ch = ch_mask(r, g, b);
	led_i2c_steady();
	for (int i = 0; !repeats || i < repeats; i++) {
		set_levels(cur, r, g, b, ch);
		if (led_wait(seq, seen, on_ms)) {
			return;
		}
		set_levels(cur, 0, 0, 0, ch);
		if (led_wait(seq, seen, off_ms)) {
			return;
		}
	}
}

typedef struct {
	int average_q8;
	unsigned frames_left;
	bool above;
} led_pulse_t;

// Use the rise in bass to trigger a pulse, not to set its brightness. Bass is
// already log-compressed, so a modest attack should still reach the peak.
// Hysteresis keeps a sustained note from retriggering until the bass drops
// back toward the running average.
static int pulse_step(led_pulse_t *state, int bass) {
	// At 40 ms per frame: 80 ms at the peak, then a soft fade back to rest.
	static const int LEVELS[] = {255, 255, 192, 128, 72, 32, 12};
	const unsigned frame_count = sizeof(LEVELS) / sizeof(LEVELS[0]);
	int rise = bass - (state->average_q8 >> 8);
	state->average_q8 += (bass * 256 - state->average_q8) / 5;
	if (bass < 32 || rise <= 2) {
		state->above = false;
	} else if (rise >= 7 && !state->above) {
		state->above = true;
		// Leave a visible dip between close beats rather than holding the
		// light at full brightness when several attacks arrive at once.
		if (state->frames_left <= 3) {
			state->frames_left = frame_count;
		}
	}
	if (!state->frames_left) {
		return 0;
	}
	unsigned frame = frame_count - state->frames_left;
	state->frames_left--;
	return LEVELS[frame];
}

// An asymmetric envelope keeps the light on during sustained sound, gliding
// up over a few hundred ms and easing down more slowly between passages.
static int glow_step(int *level_q8, int target) {
	int delta = target * 256 - *level_q8;
	int step = delta / (delta > 0 ? 5 : 12);
	*level_q8 += step ? step : delta;
	return (*level_q8 + 128) >> 8;
}

static int effect_channel(int rest, int peak, int amount) {
	return rest + ((peak - rest) * amount + 127) / 255;
}

static void play_visualizer(led_levels_t *cur, const led_render_t *t, led_levels_t rest, unsigned seen) {
	led_pulse_t pulse = {0};
	int glow_q8 = 0;
	bool single_step = !(rest.r || rest.g || rest.b);
	while (!led_wait(&led_seq, seen, LED_VISUALIZER_MS)) {
		int amount;
		if (t->effect == LED_VISUALIZER_ENERGY_GLOW) {
			amount = glow_step(&glow_q8, visualizer_energy_level());
			if (single_step) {
				// With just dark/lit available, leave a gap between the on
				// and off thresholds so nearby levels don't cause flicker.
				int threshold = (cur->r || cur->g || cur->b) ? 112 : 144;
				amount = amount >= threshold ? 255 : 0;
			}
		} else {
			amount = pulse_step(&pulse, visualizer_bass_level());
		}
		int r = effect_channel(rest.r, t->r, amount);
		int g = effect_channel(rest.g, t->g, amount);
		int b = effect_channel(rest.b, t->b, amount);
		if (r != cur->r || g != cur->g || b != cur->b) {
			set_levels(cur, r, g, b, rest.channels);
		}
	}
}

// Walk the six saturated HSV sectors as brightness eases up and down on a
// separate clock. The different periods let every hue spend time near the
// brighter part of a breath. Use integer levels throughout, including at the
// lowest setting where the envelope can only switch between dark and lit.
static led_levels_t rainbow_frame(int hue_frame, int breath_frame, int peak) {
	const int half = RAINBOW_HALF_BREATH_FRAMES;
	const int full = half * half * half;
	int rise = breath_frame <= half ? breath_frame : 2 * half - breath_frame;
	int eased = rise * rise * (3 * half - 2 * rise); // smoothstep, 0 to full
	int floor = peak > 1 ? (peak + 4) / 5 : 0;
	int level = floor + ((peak - floor) * eased + full / 2) / full;
	int sector = hue_frame / RAINBOW_SECTOR_FRAMES;
	int offset = hue_frame % RAINBOW_SECTOR_FRAMES;
	int up = (level * offset + RAINBOW_SECTOR_FRAMES / 2) / RAINBOW_SECTOR_FRAMES;
	int down = level - up;

	// Keep all channels enabled; zero levels are dark, and hue transitions
	// don't need to switch channels on and off in the middle of a fade.
	const unsigned ch = LED_CH_R | LED_CH_G | LED_CH_B;
	switch (sector) {
	case 0: return (led_levels_t){level, up, 0, ch};
	case 1: return (led_levels_t){down, level, 0, ch};
	case 2: return (led_levels_t){0, level, up, ch};
	case 3: return (led_levels_t){0, down, level, ch};
	case 4: return (led_levels_t){up, 0, level, ch};
	default: return (led_levels_t){level, 0, down, ch};
	}
}

static void play_rainbow(led_levels_t *cur, int peak, unsigned seen) {
	int hue_frame = 0, breath_frame = 0;
	while (!led_wait(&led_seq, seen, LED_VISUALIZER_MS)) {
		if (++hue_frame == RAINBOW_HUE_FRAMES) hue_frame = 0;
		if (++breath_frame == 2 * RAINBOW_HALF_BREATH_FRAMES) breath_frame = 0;
		led_levels_t next = rainbow_frame(hue_frame, breath_frame, peak);
		if (next.r != cur->r || next.g != cur->g || next.b != cur->b) {
			set_levels(cur, next.r, next.g, next.b, next.channels);
		}
	}
}

static void play_steady(led_levels_t *cur, const led_render_t *t, unsigned seen) {
	bool off = t->mode == LM_OFF;
	if (off && (t->transition_ms == 0 || !(cur->r || cur->g || cur->b))) {
		led_i2c_off();
		*cur = (led_levels_t){0};
		return;
	}

	// Walk from the actual levels to the new colour (or a dimmer floor for
	// the visualizer). Both sets of channels stay enabled during the ramp.
	led_levels_t from = *cur;
	led_levels_t rest = {t->r, t->g, t->b, ch_mask(t->r, t->g, t->b)};
	bool rainbow = t->effect == LED_VISUALIZER_RAINBOW_BREATH;
	if (rainbow) {
		rest = rainbow_frame(0, 0, t->r);
	} else if (t->effect != LED_VISUALIZER_NONE) {
		// Rest at roughly a fifth of the selected brightness. At brightness
		// 1 there is only one lit step, so rest at zero instead. Keep the
		// channels enabled for the next rise in brightness.
		bool lit_floor = t->r > 1 || t->g > 1 || t->b > 1;
		rest.r = lit_floor ? (t->r + 4) / 5 : 0;
		rest.g = lit_floor ? (t->g + 4) / 5 : 0;
		rest.b = lit_floor ? (t->b + 4) / 5 : 0;
	}
	int steps = t->transition_ms / LED_STEP_MS;
	if (steps < 1) steps = 1;

	led_i2c_steady();
	for (int i = 1; i <= steps; i++) {
		int r, g, b;
		if (off) {
			// A quadratic fall eases out instead of dropping in a line.
			int k = steps - i;
			r = from.r * k * k / (steps * steps);
			g = from.g * k * k / (steps * steps);
			b = from.b * k * k / (steps * steps);
		} else {
			r = from.r + (rest.r - from.r) * i / steps;
			g = from.g + (rest.g - from.g) * i / steps;
			b = from.b + (rest.b - from.b) * i / steps;
		}
		set_levels(cur, r, g, b, i < steps ? from.channels | rest.channels : rest.channels);
		if (i < steps && led_wait(&led_seq, seen, LED_STEP_MS)) {
			return;
		}
	}
	if (off) {
		led_i2c_off();
	} else if (rainbow) {
		play_rainbow(cur, t->r, seen);
	} else if (t->effect != LED_VISUALIZER_NONE) {
		play_visualizer(cur, t, rest, seen);
	}
}

static void *led_worker(void *arg) {
	(void)arg;
	led_levels_t cur = {0};
	unsigned seen = 0;

	for (;;) {
		pthread_mutex_lock(&led_lock);
		while (led_seq == seen) {
			pthread_cond_wait(&led_cond, &led_lock);
		}
		if (led_notice != LN_NONE) {
			led_notice_t notice = led_notice;
			unsigned notice_seq = led_notice_seq;
			int level = led_notice_level;
			pthread_mutex_unlock(&led_lock);
			if (notice == LN_LOW_BATTERY) {
				play_blink(&cur, level, 0, 0, 3, 140, 140, &led_notice_seq, notice_seq);
			} else if (notice == LN_UPDATE_AVAILABLE) {
				play_blink(&cur, 0, level, 0, 3, 200, 130, &led_notice_seq, notice_seq);
			} else {
				play_blink(&cur, 0, level, 0, 2, 180, 120, &led_notice_seq, notice_seq);
			}
			pthread_mutex_lock(&led_lock);
			if (led_notice_seq == notice_seq) led_notice = LN_NONE;
			pthread_mutex_unlock(&led_lock);
			// Leave seen alone to pick up the newest base after the pattern.
			continue;
		}
		led_render_t t = led_target;
		seen = led_seq;
		pthread_mutex_unlock(&led_lock);

		if (t.mode == LM_BREATHE) {
			led_i2c_breathe(t.r, t.g, t.b, (unsigned char)t.period, (unsigned char)t.duty, (unsigned char)t.fade);
			cur = (led_levels_t){t.r, t.g, t.b, ch_mask(t.r, t.g, t.b)};
		} else if (t.mode == LM_BLINK) {
			play_blink(&cur, t.r, t.g, t.b, 0, LED_BLINK_MS, LED_BLINK_MS, &led_seq, seen);
		} else {
			play_steady(&cur, &t, seen);
		}

		// The target finished (or a newer one cut it short). Power transitions
		// wait for their own fade to complete, not for the nominal duration.
		pthread_mutex_lock(&led_lock);
		led_done_seq = seen;
		pthread_cond_broadcast(&led_done_cond);
		pthread_mutex_unlock(&led_lock);
	}
	return NULL;
}

static bool start_worker(void) {
	pthread_condattr_t clock_attr;
	pthread_condattr_init(&clock_attr);
	if (pthread_condattr_setclock(&clock_attr, CLOCK_MONOTONIC) == 0) led_clock = CLOCK_MONOTONIC;
	int error = pthread_cond_init(&led_cond, &clock_attr);
	pthread_condattr_destroy(&clock_attr);
	if (error != 0) return false;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	error = pthread_create(&led_thread, &attr, led_worker, NULL);
	pthread_attr_destroy(&attr);
	if (error != 0) {
		fprintf(stderr, "led: could not start the effect thread\n");
		pthread_cond_destroy(&led_cond);
	}
	return error == 0;
}

// --- state -> output -------------------------------------------------------

// PCM playback follows the sample-rate bands used by the stock LED patterns.
// DSD is identified separately because DoP presents a PCM carrier rate.
static led_state_t rate_state(int sample_rate, bool dsd) {
	if (dsd || sample_rate >= 2822400) {
		return LS_DSD;
	}
	if (sample_rate > 192000) {
		return LS_PCM_HI;
	}
	if (sample_rate > 48000) {
		return LS_PCM_192K;
	}
	return LS_PCM_48K;
}

// The driver pattern a state falls back to. There is no green pattern
// registered, so a fully charged battery shows the idle aqua here; the direct
// path below paints it green.
static int state_pattern(led_state_t s) {
	switch (s) {
	case LS_OFF:
	case LS_STEALTH:
		return PATTERN_OFF;
	case LS_IDLE:
		return PATTERN_IDLE_AQUA;
	case LS_PCM_48K:
		return PATTERN_PCM_48K;
	case LS_PCM_192K:
		return PATTERN_PCM_192K;
	case LS_PCM_HI:
		return PATTERN_PCM_HI;
	case LS_DSD:
		return PATTERN_DSD_WHITE;
	case LS_PODCAST:
		return PATTERN_PODCAST_PURPLE;
	case LS_WIFI:
		return PATTERN_WIFI_TRANSFER;
	case LS_BT:
		return PATTERN_BT_RECEIVER;
	case LS_CHARGING:
		return PATTERN_CHARGING_RED;
	case LS_CHARGED:
		return PATTERN_IDLE_AQUA;
	}
	return PATTERN_OFF;
}

// The colour and effect a state is played with on the direct path. The colours
// are the ones the registered patterns stand for: aqua idle, yellow 48 kHz,
// aqua up to 192 kHz, orange above it, white DSD, purple podcasts, blue the
// Bluetooth receiver, red breathing while charging and green once charged.
static void build_render(led_state_t s, led_render_t *out) {
	int r = 0, g = 0, b = 0;
	bool white = false;
	led_mode_t mode = LM_STEADY;
	switch (s) {
	case LS_OFF:
	case LS_STEALTH:
		mode = LM_OFF;
		break;
	case LS_IDLE:
	case LS_PCM_192K:
		g = 255;
		b = 255;
		break;
	case LS_PCM_48K:
		r = 255;
		g = 255;
		break;
	case LS_PCM_HI:
		r = 255;
		g = 128;
		break;
	case LS_DSD:
		r = 255;
		g = 255;
		b = 255;
		white = true;
		break;
	case LS_PODCAST:
		r = 160;
		b = 255;
		break;
	case LS_WIFI:
		g = 128;
		b = 255;
		mode = LM_BLINK;
		break;
	case LS_BT:
		b = 255;
		break;
	case LS_CHARGED:
		g = 255;
		break;
	case LS_CHARGING:
		r = 255;
		mode = LM_BREATHE;
		break;
	}
	scale_rgb(r, g, b, white, &out->r, &out->g, &out->b);
	out->mode = mode;
	out->period = 0x1E; // the stock charging pulse
	out->duty = 0x7D;
	out->fade = 0xBB;
	out->transition_ms = s == LS_STEALTH ? 0 : s_transition_ms;
	out->effect = LED_VISUALIZER_NONE;
	out->allow_notices = false;
}

// Renders a state to whichever output is available: the direct controller, the
// driver's pattern node, or the R1's plain blue LED. Cheap to call repeatedly:
// it only writes when the state changes.
static void render_state(led_state_t state, led_visualizer_style_t effect, bool allow_notices) {
	bool redraw = (int)state != current_state || effect != current_effect;
	if (!redraw && allow_notices == current_notices) {
		return;
	}
	if (direct_mode) {
		led_render_t t;
		build_render(state, &t);
		if (effect == LED_VISUALIZER_RAINBOW_BREATH) {
			// Start the wheel at red. Its level is the ceiling for every hue.
			t.r = LADDER[led_level()];
			t.g = t.b = 0;
		}
		t.effect = effect;
		t.allow_notices = allow_notices;
		if (effect != current_effect) {
			visualizer_enable(VISUALIZER_STATUS_LED_BASS, effect == LED_VISUALIZER_BASS_PULSE);
			visualizer_enable(VISUALIZER_STATUS_LED_ENERGY, effect == LED_VISUALIZER_ENERGY_GLOW);
		}
		led_post(&t, redraw);
	} else if (have_pattern_node) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", state_pattern(state));
		write_str(PATTERN_NODE, buf);
	} else if (have_blue_node) {
		// On the R1 every lit colour except charging becomes blue.
		bool blue = state != LS_OFF && state != LS_STEALTH && state != LS_CHARGING;
		write_str(BLUE_BRIGHTNESS_NODE, blue ? BLUE_ON : "0");
	} else {
		return;
	}
	current_state = state;
	current_effect = effect;
	current_notices = allow_notices;
}

// When the "off in standby" countdown starts: the latest of the screen going
// dark, the last moment something was playing, and the cable going in or out.
static time_t idle_window_start(void) {
	time_t latest = s_standby_since > s_last_playing ? s_standby_since : s_last_playing;
	return s_charger_since > latest ? s_charger_since : latest;
}

static void apply(void) {
	// s_charging is "a charge is going in", not "a cable is in": the caller
	// takes the battery being full, or held at its limit, out of it, and hands
	// this a settled answer rather than a reading that is still wobbling. See
	// led.h.
	bool charging_now = s_charging && !s_stealth && !s_suspended;

	// The separate red classdev: the R1's red LED. Breathing while a charge goes
	// in, released otherwise. The R3 Pro II has no such node, and its charging
	// red comes from the state below.
	int want_red = charging_now ? 1 : 0;
	if (have_red_node && want_red != red_trigger_state) {
		red_trigger_state = want_red;
		write_str(RED_TRIGGER_NODE, charging_now ? "breathing" : "none");
		if (!charging_now) write_str(RED_BRIGHTNESS_NODE, "0");
	}

	bool smart = direct_mode && s_visualizer && s_led_enabled && !s_stealth && !s_suspended &&
				 !charging_now && !s_on_charger;
	led_visualizer_style_t effect = LED_VISUALIZER_NONE;
	led_state_t state;
	if (s_stealth) {
		state = LS_STEALTH;
	} else if (s_suspended) {
		state = LS_OFF;
	} else if (!s_led_enabled) {
		// The master switch is off: the LED stays dark, except that a charge in
		// progress still shows -- a device that looks dead while it is charging
		// is worse than a lit LED.
		state = charging_now ? LS_CHARGING : LS_OFF;
	} else if (charging_now) {
		// Charging wins over everything, the stock priority: the red breathes
		// while a charge goes in.
		state = LS_CHARGING;
	} else if (s_on_charger) {
		// A cable is supplying and the charge is done -- full, or held at the
		// configured limit. Green says so, and like the red it is never darkened
		// by the standby option: a player left plugged in overnight stays lit.
		state = LS_CHARGED;
	} else if (s_playing && s_rate > 0) {
		// Choose the playback colour (purple for podcasts). Rainbow Breath
		// can replace it for PCM, but DSD and podcast indicators stay steady.
		state = s_podcast ? LS_PODCAST : rate_state(s_rate, s_dsd);
		// DoP is not PCM and there is no PCM feed for the USB DAC mode below.
		if (smart && state != LS_DSD && state != LS_PODCAST) effect = s_visualizer_style;
	} else if (s_dac_streaming && s_dac_rate > 0) {
		// In DAC mode nothing plays through audio.c, so s_playing is false and
		// the LED went dark twenty seconds after the screen did -- with the
		// computer still sending music. The rate the host chose gets the same
		// colour a local PCM track of that rate would.
		state = rate_state(s_dac_rate, false);
	} else if (s_wifi_transfer) {
		// A Wi-Fi transfer is running: its light stays lit even with the screen
		// off, since it is the only sign the server is running, and it
		// deliberately overrides the off-in-standby option.
		state = LS_WIFI;
	} else if (s_bt_receiver) {
		// Bluetooth receiver mode, lit whether or not the sender is sending
		// right now: the mode is what the LED reports, the way the transfer
		// above reports its server. Nothing plays through audio.c here, so the
		// branch at the top never fires and without this one the LED went dark
		// twenty seconds after the screen did -- with music still coming out of
		// the jack.
		state = LS_BT;
	} else if (s_dac_active) {
		// The mode is up but the computer is not sending anything yet. Same
		// argument as the transfer above: with the screen dark the LED is the
		// only sign the player is a sound card, so the standby option does not
		// reach it.
		state = LS_IDLE;
	} else if (s_idle_off_enabled && s_standby && time(NULL) - idle_window_start() >= IDLE_OFF_SECONDS) {
		// LED off in standby: the screen has been off this long with nothing
		// playing and nothing else going on. The window runs from whichever came
		// last -- the screen going dark, playback stopping, the cable going in or
		// out. Counted from the screen alone, a track paused with the screen
		// already off would find the window long since elapsed and put the LED
		// out the instant the music stopped, with no aqua in between. With the
		// screen on the aqua stays: the device is visibly in use.
		//
		// A cable is handled above (LS_CHARGED), which is the point: a battery
		// that reaches 100%, or its configured limit, has nothing left to say in
		// red, and a player left plugged in overnight would otherwise answer the
		// end of its charge by going dark.
		state = LS_OFF;
	} else {
		state = LS_IDLE;
	}
	render_state(state, effect, smart);
}

static led_visualizer_style_t normalize_style(long style) {
	switch (style) {
	case LED_VISUALIZER_ENERGY_GLOW:
		return LED_VISUALIZER_ENERGY_GLOW;
	case LED_VISUALIZER_RAINBOW_BREATH:
		return LED_VISUALIZER_RAINBOW_BREATH;
	default:
		return LED_VISUALIZER_BASS_PULSE;
	}
}

void led_init(void) {
	static bool initialized;
	if (initialized) {
		return;
	}
	initialized = true;
#ifndef HOST_BUILD
	have_pattern_node = access(PATTERN_NODE, W_OK) == 0;
	have_red_node = access(RED_TRIGGER_NODE, W_OK) == 0;
	have_blue_node = !have_pattern_node && access(BLUE_BRIGHTNESS_NODE, W_OK) == 0;
#endif

	bool sgm = has_sgm31324();
	if (!sgm && !have_red_node && !have_blue_node) {
		return; // no supported LED
	}

	// Read every setting before posting the first target: a saved Stealth Mode
	// never fades in so much as one step of aqua on its way to going dark.
	s_led_enabled = config_get_bool("power", "led_on", true);
	s_stealth = config_get_bool("power", "led_stealth", false);
	s_visualizer = config_get_bool("power", "led_visualizer", false);
	s_visualizer_style = normalize_style(config_get_int("power", "led_visualizer_style", LED_VISUALIZER_BASS_PULSE));
	s_idle_off_enabled = config_get_bool("power", "led_off_standby", true);
	led_set_transition_ms(config_get_int("power", "led_transition_ms", TRANSITION_DEFAULT_MS));
	s_brightness = (int)config_get_int("power", "led_brightness", BRIGHTNESS_DEFAULT);

	if (sgm && led_i2c_init()) {
		direct_mode = start_worker();
		if (!direct_mode && s_stealth) led_i2c_off();
	}

	const char *mode = direct_mode ? "direct i2c"
							   : have_pattern_node ? "pattern node"
							   : have_blue_node	 ? "blue pwm"
											 : "unavailable";
	fprintf(stderr, "led: %s, red trigger %s, blue %s\n", mode, have_red_node ? "ok" : "missing",
			have_blue_node ? "ok" : "missing");

	s_last_playing = time(NULL); // the power-on colour holds for the first idle window
	apply();
}

void led_set_charge_state(bool present, bool charging) {
	s_charging = present && charging;
	if (present != s_on_charger) {
		s_on_charger = present;
		// A cable going in or out gives the idle light a fresh standby window.
		s_charger_since = time(NULL);
	}
	apply();
}

void led_update_playback(bool playing, int sample_rate, bool podcast, bool dsd) {
	s_playing = playing && sample_rate > 0;
	s_rate = sample_rate;
	s_podcast = podcast;
	s_dsd = dsd;
	if (s_playing) {
		s_last_playing = time(NULL);
	}
	apply();
}

void led_set_dac(bool active, bool streaming, int sample_rate) {
	if (active == s_dac_active && streaming == s_dac_streaming && sample_rate == s_dac_rate) {
		return;
	}
	s_dac_active = active;
	s_dac_streaming = streaming;
	s_dac_rate = sample_rate;
	apply();
}

void led_set_wifi_transfer(bool active) {
	if (active == s_wifi_transfer) {
		return;
	}
	s_wifi_transfer = active;
	apply();
}

void led_set_bt_receiver(bool active) {
	if (active == s_bt_receiver) {
		return;
	}
	s_bt_receiver = active;
	apply();
}

void led_set_enabled(bool enabled) {
	s_led_enabled = enabled;
	apply();
}

void led_set_stealth(bool enabled) {
	s_stealth = enabled;
	apply();
}

void led_set_visualizer(bool enabled) {
	s_visualizer = enabled;
	apply();
}

void led_set_visualizer_style(led_visualizer_style_t style) {
	s_visualizer_style = normalize_style(style);
	apply();
}

led_visualizer_style_t led_visualizer_style(void) { return s_visualizer_style; }

void led_notify_low_battery(void) { post_notice(LN_LOW_BATTERY); }

void led_notify_upload_complete(void) { post_notice(LN_UPLOAD_COMPLETE); }

void led_notify_update_ready(void) { post_notice(LN_UPDATE_AVAILABLE); }

void led_set_idle_off(bool enabled) {
	s_idle_off_enabled = enabled;
	apply();
}

void led_set_standby(bool standby) {
	if (standby && !s_standby) {
		s_standby_since = time(NULL); // the idle window starts here
	}
	s_standby = standby;
	if (!standby) {
		// Waking counts as activity: the aqua shows for a fresh idle window
		// before the option can dim it again.
		s_last_playing = time(NULL);
		s_standby_since = time(NULL);
	}
	apply();
}

void led_set_brightness(int brightness) {
	if (brightness < BRIGHTNESS_MIN) {
		brightness = BRIGHTNESS_MIN;
	}
	if (brightness > BRIGHTNESS_MAX) {
		brightness = BRIGHTNESS_MAX;
	}
	if (brightness == s_brightness) {
		return;
	}
	s_brightness = brightness;
	if (direct_mode && current_state != LS_STEALTH && current_state != LS_OFF) {
		current_state = -1; // force a re-render at the new level
		apply();
	}
}

void led_set_transition_ms(int ms) {
	if (ms < TRANSITION_MIN_MS) {
		ms = TRANSITION_MIN_MS;
	}
	if (ms > TRANSITION_MAX_MS) {
		ms = TRANSITION_MAX_MS;
	}
	s_transition_ms = ms;
}

bool led_effects_available(void) { return direct_mode; }

static void fade_out_and_wait(void) {
	s_suspended = true;
	apply();
	if (!direct_mode) {
		return;
	}

	pthread_mutex_lock(&led_lock);
	unsigned want = led_seq;
	// A safety net, not the expected path: the longest fade is two seconds.
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 3;
	while (led_done_seq != want) {
		if (pthread_cond_timedwait(&led_done_cond, &led_lock, &deadline) != 0) {
			break;
		}
	}
	pthread_mutex_unlock(&led_lock);
}

void led_shutdown(void) { fade_out_and_wait(); }

void led_suspend(void) { fade_out_and_wait(); }

void led_resume(void) {
	s_suspended = false;
	apply();
}
