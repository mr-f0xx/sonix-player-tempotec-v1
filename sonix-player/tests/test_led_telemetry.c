// Exercise the real state machine/worker and PCM tap against an I2C recorder
// and in-memory sysfs nodes. Never open the host's LEDs or I2C bus.
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fixture_access(const char *path, int mode);
static FILE *fixture_fopen(const char *path, const char *mode);

#define access fixture_access
#define fopen fixture_fopen
#include "../src/system/device/led.c"
#undef access
#undef fopen

enum { DIRECT, PATTERN, PWM, UNAVAILABLE };
static int backend;
static char pattern[16], red_trigger[16], red_brightness[16], blue[16];

static int fixture_access(const char *path, int mode) {
	assert(mode == W_OK);
	if (backend == PATTERN && !strcmp(path, PATTERN_NODE)) return 0;
	if (backend == PWM && (!strcmp(path, RED_TRIGGER_NODE) || !strcmp(path, BLUE_BRIGHTNESS_NODE))) return 0;
	return -1;
}

static FILE *fixture_fopen(const char *path, const char *mode) {
	char *node = !strcmp(path, PATTERN_NODE) ? pattern : !strcmp(path, RED_TRIGGER_NODE) ? red_trigger :
				 !strcmp(path, RED_BRIGHTNESS_NODE) ? red_brightness : !strcmp(path, BLUE_BRIGHTNESS_NODE) ? blue : NULL;
	assert(node && backend != DIRECT && backend != UNAVAILABLE);
	return fmemopen(node, 16, mode);
}

const sysinfo_model_t *sysinfo_model(void) {
	static const sysinfo_model_t v1 = {.sgm31324_led = true};
	return backend == DIRECT || backend == PATTERN ? &v1 : NULL;
}

bool led_i2c_init(void) { return backend == DIRECT; }

enum { LEVELS, BREATHE, DARK };
typedef struct { int kind, r, g, b; } frame_t;
static frame_t frames[1024];
static size_t frame_count;
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_cond = PTHREAD_COND_INITIALIZER;

static void record(int kind, int r, int g, int b) {
	pthread_mutex_lock(&frame_lock);
	assert(frame_count < sizeof(frames) / sizeof(frames[0]));
	frames[frame_count++] = (frame_t){kind, r, g, b};
	pthread_cond_signal(&frame_cond);
	pthread_mutex_unlock(&frame_lock);
}

void led_i2c_steady(void) {}
void led_i2c_levels(int r, int g, int b, unsigned channels) {
	assert((ch_mask(r, g, b) & channels) == ch_mask(r, g, b));
	record(LEVELS, r, g, b);
}
void led_i2c_breathe(int r, int g, int b, unsigned char period, unsigned char duty, unsigned char fade) {
	assert(period == 0x1e && duty == 0x7d && fade == 0xbb);
	record(BREATHE, r, g, b);
}
void led_i2c_off(void) { record(DARK, 0, 0, 0); }

static size_t mark(void) {
	pthread_mutex_lock(&frame_lock);
	size_t count = frame_count;
	pthread_mutex_unlock(&frame_lock);
	return count;
}

static bool matches(frame_t f, int kind, int r, int g, int b) {
	return f.kind == kind && f.r == r && f.g == g && f.b == b;
}

static size_t wait_frame(size_t start, int kind, int r, int g, int b) {
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 3;
	pthread_mutex_lock(&frame_lock);
	for (;;) {
		for (; start < frame_count; start++) {
			if (matches(frames[start], kind, r, g, b)) {
				pthread_mutex_unlock(&frame_lock);
				return start;
			}
		}
		int rc = pthread_cond_timedwait(&frame_cond, &frame_lock, &deadline);
		if (rc == ETIMEDOUT) {
			fprintf(stderr, "Timed out waiting for LED frame %d (%d,%d,%d)\n", kind, r, g, b);
			abort();
		}
		assert(!rc);
	}
}

static unsigned count_frames(size_t start, size_t end, int r, int g, int b) {
	unsigned n = 0;
	pthread_mutex_lock(&frame_lock);
	for (size_t i = start; i < end; i++) n += matches(frames[i], LEVELS, r, g, b);
	pthread_mutex_unlock(&frame_lock);
	return n;
}

#define PCM_RATE 44100
#define PCM_FRAMES 4096
static int16_t bass_pcm[PCM_FRAMES], treble_pcm[PCM_FRAMES];

static void feed(const int16_t *pcm) { visualizer_feed(pcm, PCM_FRAMES, 1, 16, PCM_RATE); }

static void test_pcm_consumers(void) {
	for (int i = 0; i < PCM_FRAMES; i++) {
		bass_pcm[i] = (int16_t)(16000 * sin(2 * M_PI * 65 * i / PCM_RATE));
		treble_pcm[i] = (int16_t)(16000 * sin(2 * M_PI * 3000 * i / PCM_RATE));
	}
	feed(bass_pcm);
	visualizer_enable(VISUALIZER_STATUS_LED, true);
	assert(visualizer_bass_level() == 0); // off meant no PCM was captured
	feed(treble_pcm);
	assert(visualizer_bass_level() < 32);
	feed(bass_pcm);
	assert(visualizer_bass_level() > 180);

	// The screen can turn its reader on or off without touching the LED's
	// ring. Conversely, blackout must not kill the screensaver analyzer.
	uint8_t screen[VISUALIZER_BANDS];
	visualizer_enable(VISUALIZER_SCREENSAVER, true);
	visualizer_enable(VISUALIZER_SCREENSAVER, false);
	assert(visualizer_bass_level() > 180);
	visualizer_enable(VISUALIZER_SCREENSAVER, true);
	visualizer_enable(VISUALIZER_STATUS_LED, false);
	assert(visualizer_bass_level() == 0);
	visualizer_levels(screen);
	assert(screen[0] > 100);
	visualizer_enable(VISUALIZER_STATUS_LED, true);
	usleep(300000);
	assert(visualizer_bass_level() == 0); // stale (paused) signal cannot drive a pulse
	visualizer_enable(VISUALIZER_SCREENSAVER, false);
	visualizer_enable(VISUALIZER_STATUS_LED, false);
	puts("ok  bass vs treble, stale data, independent screensaver and LED readers");
}

static void test_direct(void) {
	char cfg[] = "/tmp/sonix-led-XXXXXX";
	int fd = mkstemp(cfg);
	assert(fd >= 0);
	close(fd);
	config_init(cfg);
	config_set_bool("power", "led_stealth", true);
	config_set_int("power", "led_transition_ms", 20);
	assert(config_save());
	config_init(cfg);
	unlink(cfg);

	led_init();
	assert(led_effects_available());
	assert(wait_frame(0, DARK, 0, 0, 0) == 0); // no lit startup frames
	led_set_charge_state(true, true);
	led_notify_low_battery();
	led_suspend();
	led_resume();
	assert(mark() == 1);
	led_set_stealth(false);
	wait_frame(1, BREATHE, 23, 0, 0);

	size_t start = mark();
	led_set_charge_state(true, false);
	wait_frame(start, LEVELS, 0, 23, 0);
	led_set_charge_state(false, false);
	start = mark();
	led_update_playback(true, 44100, false, false);
	wait_frame(start, LEVELS, 23, 23, 0);
	feed(bass_pcm);
	assert(visualizer_bass_level() == 0); // Smart LED is opt-in

	start = mark();
	led_set_visualizer(true);
	int floor = pulse_channel(23, 0);
	wait_frame(start, LEVELS, floor, floor, 0);
	start = mark();
	led_set_standby(true);
	// A few attempts account for the PCM tap intentionally skipping a write
	// if the worker is briefly copying the ring.
	for (int i = 0; i < 20 && visualizer_bass_level() == 0; i++) {
		feed(bass_pcm);
		usleep(1000);
	}
	wait_frame(start, LEVELS, 23, 23, 0);
	wait_frame(start, LEVELS, floor, floor, 0);

	// A held tone stops triggering once the slow average catches up.
	int average = 0, pulse = pulse_step(200, &average, 0);
	assert(pulse == 255);
	for (int i = 0; i < 100; i++) pulse = pulse_step(200, &average, pulse);
	assert(!pulse);
	assert(pulse_channel(1, 0) == 1 && pulse_channel(23, 255) == 23);
	led_set_standby(false);

	// Two green beats, then the new playback colour, not the old one.
	start = mark();
	led_notify_upload_complete();
	wait_frame(start, LEVELS, 0, 23, 0);
	led_update_playback(true, 96000, false, false);
	size_t end = wait_frame(start, LEVELS, 0, floor, floor);
	assert(count_frames(start, end, 0, 23, 0) == 2);

	start = mark();
	led_notify_upload_complete();
	wait_frame(start, LEVELS, 0, 23, 0);
	led_notify_low_battery();
	led_notify_upload_complete(); // ignored while the warning is in flight
	end = wait_frame(start, LEVELS, 0, floor, floor);
	assert(count_frames(start, end, 23, 0, 0) == 3);
	assert(count_frames(start, end, 0, 23, 0) == 1);

	// DSD and USB DAC never treat their audio as the app's PCM tap. The same
	// PCM rate when handed back to the player can still start pulsing again.
	start = mark();
	led_update_playback(true, 176400, false, true);
	wait_frame(start, LEVELS, 13, 13, 13);
	assert(!current_pulse);
	led_set_dac(true, true, 96000);
	start = mark();
	led_update_playback(false, 0, false, false);
	wait_frame(start, LEVELS, 0, 23, 23);
	assert(!current_pulse);
	start = mark();
	led_update_playback(true, 96000, false, false);
	wait_frame(start, LEVELS, 0, floor, floor);
	assert(current_pulse);

	start = mark();
	led_notify_low_battery();
	wait_frame(start, LEVELS, 23, 0, 0);
	// Stealth cuts the pattern instead of taking the configured two-second fade.
	led_set_transition_ms(2000);
	start = mark();
	led_set_stealth(true);
	assert(wait_frame(start, DARK, 0, 0, 0) == start);
	led_set_charge_state(true, true);
	led_notify_upload_complete();
	led_suspend();
	led_resume();
	assert(mark() == start + 1);
	led_set_transition_ms(20);
	led_set_stealth(false);
	wait_frame(start + 1, BREATHE, 23, 0, 0);

	start = mark();
	led_suspend();
	assert(matches(frames[mark() - 1], DARK, 0, 0, 0));
	end = mark();
	assert(end > start);
	led_set_charge_state(false, false);
	led_notify_low_battery();
	assert(mark() == end);
	led_resume();
	wait_frame(end, LEVELS, 0, floor, floor);
	led_shutdown();
	puts("ok  startup, PCM pulse, alert priority/restore, Stealth and suspend on the RGB worker");
}

static void test_fallback(void) {
	config_init(NULL);
	led_init();
	assert(!led_effects_available());
	led_set_visualizer(true);
	led_notify_upload_complete();
	led_notify_low_battery();
	assert(mark() == 0);
	led_set_enabled(false);
	led_set_charge_state(true, true);
	if (backend == PATTERN) assert(!strcmp(pattern, "6"));
	if (backend == PWM) assert(!strcmp(red_trigger, "breathing") && !strcmp(blue, "0"));
	led_set_stealth(true);
	if (backend == PATTERN) assert(!strcmp(pattern, "5"));
	if (backend == PWM) assert(!strcmp(red_trigger, "none") && !strcmp(red_brightness, "0") && !strcmp(blue, "0"));
	led_set_charge_state(false, false);
	led_set_standby(true);
	led_resume();
	led_set_stealth(false);
	if (backend == PATTERN) assert(!strcmp(pattern, "5"));
	led_set_enabled(true);
	if (backend == PATTERN) assert(!strcmp(pattern, "1"));
	if (backend == PWM) assert(!strcmp(blue, "50"));
	assert(mark() == 0);
	puts("ok  Stealth and safe static behavior without direct RGB effects");
}

int main(int argc, char **argv) {
	if (argc > 1) {
		backend = !strcmp(argv[1], "pattern") ? PATTERN : !strcmp(argv[1], "pwm") ? PWM : UNAVAILABLE;
		test_fallback();
	} else {
		test_pcm_consumers();
		test_direct();
	}
	return 0;
}
