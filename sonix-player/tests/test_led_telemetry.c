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

static frame_t last_frame(void) {
	pthread_mutex_lock(&frame_lock);
	assert(frame_count);
	frame_t frame = frames[frame_count - 1];
	pthread_mutex_unlock(&frame_lock);
	return frame;
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
static int16_t quiet_bass_pcm[PCM_FRAMES], beat_pcm[PCM_FRAMES], quiet_treble_pcm[PCM_FRAMES];
static const int16_t silence_pcm[PCM_FRAMES];

static void feed(const int16_t *pcm) { visualizer_feed(pcm, PCM_FRAMES, 1, 16, PCM_RATE); }

static void feed_for(const int16_t *pcm, int ms) {
	// Repeat the feed in case the worker briefly has the ring locked.
	for (int elapsed = 0; elapsed < ms; elapsed += 20) {
		feed(pcm);
		usleep(20000);
	}
}

static void init_pcm(void) {
	for (int i = 0; i < PCM_FRAMES; i++) {
		double bass = sin(2 * M_PI * 65 * i / PCM_RATE);
		double treble = sin(2 * M_PI * 3000 * i / PCM_RATE);
		bass_pcm[i] = (int16_t)(16000 * bass);
		quiet_bass_pcm[i] = (int16_t)(2400 * bass);
		beat_pcm[i] = (int16_t)(3400 * bass); // about 3 dB above the quiet bass
		treble_pcm[i] = (int16_t)(16000 * treble);
		quiet_treble_pcm[i] = (int16_t)(1400 * treble);
	}
}

static void test_pcm_consumers(void) {
	feed(bass_pcm);
	visualizer_enable(VISUALIZER_STATUS_LED_BASS, true);
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
	visualizer_enable(VISUALIZER_STATUS_LED_BASS, false);
	assert(visualizer_bass_level() == 0);
	visualizer_levels(screen);
	assert(screen[0] > 100);
	visualizer_enable(VISUALIZER_STATUS_LED_BASS, true);
	usleep(300000);
	assert(visualizer_bass_level() == 0); // stale (paused) signal cannot drive a pulse
	visualizer_enable(VISUALIZER_SCREENSAVER, false);
	visualizer_enable(VISUALIZER_STATUS_LED_BASS, false);
	puts("ok  bass vs treble, stale data, independent screensaver and LED readers");
}

static void test_pulse_envelope(void) {
	led_pulse_t pulse = {0};
	assert(pulse_step(&pulse, 20) == 0); // noise floor
	for (int i = 0; i < 50; i++) pulse_step(&pulse, 180);
	assert(pulse_step(&pulse, 183) == 0); // small variations are not beats
	assert(pulse_step(&pulse, 189) == 255); // modest rise, full-strength pulse
	assert(pulse_step(&pulse, 189) == 255); // hold the peak for two frames
	int previous = pulse_step(&pulse, 189);
	assert(previous > 0 && previous < 255);
	for (int i = 0; i < 50; i++) {
		int level = pulse_step(&pulse, 189);
		assert(level <= previous); // decay without retriggering the held tone
		previous = level;
	}
	assert(previous == 0);
	assert(pulse_step(&pulse, 180) == 0);
	assert(pulse_step(&pulse, 197) == 255); // a fresh beat re-arms the pulse

	pulse = (led_pulse_t){0};
	assert(pulse_step(&pulse, 180) == 255);
	assert(pulse_step(&pulse, 0) == 255);
	assert(pulse_step(&pulse, 180) < 255); // the next attack arrived too soon
	pulse_step(&pulse, 0);
	int dip = pulse_step(&pulse, 0);
	assert(dip > 0 && dip < 128);
	assert(pulse_step(&pulse, 180) == 255); // the next peak follows a visible dip
	puts("ok  bass attacks reach the peak, hold, and leave a dip between pulses");
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
	assert(led_visualizer_style() == LED_VISUALIZER_BASS_PULSE); // existing configs
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
	const int floor = 5;
	wait_frame(start, LEVELS, floor, floor, 0);
	led_set_standby(true);
	feed_for(quiet_bass_pcm, 1000); // let the detector settle to a nonzero baseline
	start = mark();
	feed_for(beat_pcm, 420);
	size_t peak = wait_frame(start, LEVELS, 23, 23, 0);
	size_t rest = wait_frame(peak + 1, LEVELS, floor, floor, 0);
	assert(count_frames(start, rest, 23, 23, 0) == 1);
	led_set_standby(false);

	// At the minimum brightness there is only one lit step. The actual
	// controller output should go dark between beats, not disable the effect
	// or exceed that step to create the contrast.
	led_set_visualizer(false);
	start = mark();
	led_set_brightness(1);
	wait_frame(start, LEVELS, 1, 1, 0);
	start = mark();
	led_set_visualizer(true);
	wait_frame(start, LEVELS, 0, 0, 0);
	start = mark();
	feed_for(bass_pcm, 120);
	peak = wait_frame(start, LEVELS, 1, 1, 0);
	wait_frame(peak + 1, LEVELS, 0, 0, 0);
	start = mark();
	led_set_brightness(5);
	wait_frame(start, LEVELS, floor, floor, 0);

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
	assert(current_effect == LED_VISUALIZER_NONE);
	led_set_dac(true, true, 96000);
	start = mark();
	led_update_playback(false, 0, false, false);
	wait_frame(start, LEVELS, 0, 23, 23);
	assert(current_effect == LED_VISUALIZER_NONE);
	start = mark();
	led_update_playback(true, 96000, false, false);
	wait_frame(start, LEVELS, 0, floor, floor);
	assert(current_effect == LED_VISUALIZER_BASS_PULSE);

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

static void test_energy_capture(void) {
	visualizer_enable(VISUALIZER_SCREENSAVER, true);
	feed(treble_pcm);
	assert(visualizer_energy_level() == 0); // spectrum alone doesn't collect energy
	visualizer_enable(VISUALIZER_STATUS_LED_ENERGY, true);
	assert(visualizer_energy_level() == 0); // a fresh window is needed
	feed(silence_pcm);
	assert(visualizer_energy_level() == 0);
	feed(quiet_treble_pcm);
	int quiet = visualizer_energy_level();
	assert(quiet > 0 && quiet < 255);
	feed(treble_pcm);
	assert(visualizer_energy_level() == 255);

	// Opposite-phase channels cancel in the mono spectrum, not in RMS.
	int16_t stereo[PCM_FRAMES * 2];
	for (int i = 0; i < PCM_FRAMES; i++) {
		stereo[i * 2] = quiet_treble_pcm[i];
		stereo[i * 2 + 1] = -quiet_treble_pcm[i];
	}
	visualizer_feed(stereo, PCM_FRAMES, 2, 16, PCM_RATE);
	assert(visualizer_energy_level() == quiet);
	uint8_t bands[VISUALIZER_BANDS];
	visualizer_levels(bands);
	for (int i = 0; i < VISUALIZER_BANDS; i++) assert(bands[i] == 0);

	visualizer_enable(VISUALIZER_SCREENSAVER, false);
	assert(visualizer_energy_level() == quiet);
	usleep(300000);
	assert(visualizer_energy_level() == 0);
	feed(treble_pcm);
	assert(visualizer_energy_level() > quiet);
	visualizer_feed(treble_pcm, 1, 1, 16, 48000);
	assert(visualizer_energy_level() == 0); // don't reuse a previous rate's samples
	visualizer_enable(VISUALIZER_STATUS_LED_ENERGY, false);
	puts("ok  overall PCM level includes treble and out-of-phase stereo, ignores stale PCM");
}

static void test_glow_envelope(void) {
	int level_q8 = 0;
	int level = glow_step(&level_q8, 255);
	assert(level > 0 && level < 128); // no flash on the initial attack
	for (int i = 0; i < 80; i++) {
		int next = glow_step(&level_q8, 255);
		assert(next >= level && next <= 255);
		level = next;
	}
	assert(level == 255);
	level = glow_step(&level_q8, 0);
	assert(level > 200 && level < 255); // silence doesn't cut straight to dim
	for (int i = 0; i < 100; i++) {
		int next = glow_step(&level_q8, 0);
		assert(next >= 0 && next <= level);
		level = next;
	}
	assert(level == 0);
}

static void init_saved_style(led_visualizer_style_t style) {
	char cfg[] = "/tmp/sonix-led-style-XXXXXX";
	int fd = mkstemp(cfg);
	assert(fd >= 0);
	close(fd);
	config_init(cfg);
	config_set_bool("power", "led_visualizer", true);
	config_set_int("power", "led_visualizer_style", style);
	config_set_int("power", "led_transition_ms", 20);
	assert(config_save());
	config_init(cfg);
	unlink(cfg);
	led_init();
	assert(led_visualizer_style() == style);
}

static void test_glow(void) {
	init_saved_style(LED_VISUALIZER_ENERGY_GLOW);
	assert(current_effect == LED_VISUALIZER_NONE); // not yet playing
	const int floor = 5;
	led_update_playback(true, PCM_RATE, false, false);
	assert(current_effect == LED_VISUALIZER_ENERGY_GLOW);
	wait_frame(0, LEVELS, floor, floor, 0);
	led_set_standby(true);

	feed_for(quiet_treble_pcm, 700);
	frame_t mid = last_frame();
	assert(mid.kind == LEVELS && mid.r == mid.g && mid.r > floor && mid.r < 23 && !mid.b);
	size_t start = mark();
	feed_for(treble_pcm, 1000);
	size_t peak = wait_frame(start, LEVELS, 23, 23, 0);
	pthread_mutex_lock(&frame_lock);
	assert(peak > start); // there are intermediate steps before the bright peak
	int previous = mid.r;
	for (size_t i = start; i <= peak; i++) {
		frame_t f = frames[i];
		assert(f.kind == LEVELS && f.r == f.g && !f.b && f.r >= previous && f.r <= 23);
		previous = f.r;
	}
	pthread_mutex_unlock(&frame_lock);
	assert(matches(last_frame(), LEVELS, 23, 23, 0)); // the tone sustains the glow
	wait_frame(peak + 1, LEVELS, floor, floor, 0); // fades out on a stale feed

	led_set_visualizer_style(LED_VISUALIZER_BASS_PULSE);
	assert(current_effect == LED_VISUALIZER_BASS_PULSE && !visualizer_energy_level());
	led_set_visualizer_style(LED_VISUALIZER_ENERGY_GLOW);
	assert(current_effect == LED_VISUALIZER_ENERGY_GLOW);
	start = mark();
	led_set_brightness(1);
	wait_frame(start, LEVELS, 0, 0, 0);
	start = mark();
	feed_for(treble_pcm, 400);
	peak = wait_frame(start, LEVELS, 1, 1, 0);
	wait_frame(peak + 1, LEVELS, 0, 0, 0);

	start = mark();
	feed_for(treble_pcm, 240);
	wait_frame(start, LEVELS, 1, 1, 0);
	// A charging cable takes over an actively lit PCM effect right away.
	start = mark();
	led_set_charge_state(true, true);
	wait_frame(start, BREATHE, 1, 0, 0);
	assert(current_effect == LED_VISUALIZER_NONE);
	feed(treble_pcm);
	assert(visualizer_energy_level() == 0);
	start = mark();
	led_set_stealth(true);
	wait_frame(start, DARK, 0, 0, 0);
	assert(led_visualizer_style() == LED_VISUALIZER_ENERGY_GLOW);
	led_shutdown();
	puts("ok  saved Energy Glow, smooth PCM brightness on the RGB worker, style switch and overrides");
}

static void test_rainbow_frames(void) {
	// Pure hues are joined by interpolated frames, including magenta -> red.
	static const int colours[][3] = {{1,0,0}, {1,1,0}, {0,1,0}, {0,1,1}, {0,0,1}, {1,0,1}};
	const int half = RAINBOW_HALF_BREATH_FRAMES;
	const int max = 23;
	for (int i = 0; i < 6; i++) {
		led_levels_t f = rainbow_frame(i * RAINBOW_SECTOR_FRAMES, half, max);
		assert(f.r == max * colours[i][0] && f.g == max * colours[i][1] && f.b == max * colours[i][2]);
	}
	led_levels_t previous = rainbow_frame(0, half, max);
	for (int hue = 1; hue <= RAINBOW_HUE_FRAMES; hue++) {
		led_levels_t f = rainbow_frame(hue % RAINBOW_HUE_FRAMES, half, max);
		assert(abs(f.r - previous.r) <= 1 && abs(f.g - previous.g) <= 1 && abs(f.b - previous.b) <= 1);
		previous = f;
	}

	// Value rises and falls smoothly even as hue keeps moving. Across all
	// settings at least one channel reaches the selected peak, never more.
	for (int brightness = BRIGHTNESS_MIN; brightness <= BRIGHTNESS_MAX; brightness++) {
		int peak = LADDER[brightness];
		int floor = brightness == BRIGHTNESS_MIN ? 0 : (peak + 4) / 5;
		int last = floor;
		for (int breath = 0; breath <= 2 * half; breath++) {
			led_levels_t f = rainbow_frame(breath * 2, breath, peak);
			assert(f.r >= 0 && f.r <= peak && f.g >= 0 && f.g <= peak && f.b >= 0 && f.b <= peak);
			int level = f.r > f.g ? f.r : f.g;
			if (f.b > level) level = f.b;
			assert(level >= floor);
			assert(breath <= half ? level >= last : level <= last);
			if (breath == half) assert(level == peak);
			if (breath == half / 2 && peak == max) assert(level > floor && level < peak);
			last = level;
		}
		assert(last == floor);
	}
	puts("ok  Rainbow Breath covers all six hues with a smooth fade and brightness limit");
}

static void test_rainbow(void) {
	init_saved_style(LED_VISUALIZER_RAINBOW_BREATH);
	assert(current_effect == LED_VISUALIZER_NONE);
	led_set_standby(true);
	led_update_playback(true, PCM_RATE, false, false);
	assert(current_effect == LED_VISUALIZER_RAINBOW_BREATH);
	size_t start = wait_frame(0, LEVELS, 5, 0, 0);
	const int half = RAINBOW_HALF_BREATH_FRAMES;
	led_levels_t crest = rainbow_frame(half, half, 23);
	// No audio samples at all: this is a timed effect, not an analyzer.
	size_t bright = wait_frame(start + 1, LEVELS, crest.r, crest.g, crest.b);
	int fall = 3 * half / 2;
	led_levels_t fading = rainbow_frame(fall, fall, 23);
	wait_frame(bright + 1, LEVELS, fading.r, fading.g, fading.b);

	start = mark();
	led_notify_low_battery();
	size_t restored = wait_frame(start, LEVELS, 5, 0, 0);
	assert(count_frames(start, restored, 23, 0, 0) == 3);

	// Switching from an audio-reactive style to rainbow releases its tap.
	led_set_visualizer_style(LED_VISUALIZER_ENERGY_GLOW);
	feed_for(treble_pcm, 120);
	assert(visualizer_energy_level() > 0);
	start = mark();
	led_set_visualizer_style(LED_VISUALIZER_RAINBOW_BREATH);
	feed(treble_pcm);
	assert(!visualizer_energy_level() && !visualizer_bass_level());
	wait_frame(start, LEVELS, 5, 0, 0);

	start = mark();
	led_set_charge_state(true, true);
	wait_frame(start, BREATHE, 23, 0, 0);
	assert(current_effect == LED_VISUALIZER_NONE);
	start = mark();
	led_set_stealth(true);
	wait_frame(start, DARK, 0, 0, 0);
	led_set_charge_state(false, false);
	assert(mark() == start + 1);
	start = mark();
	led_set_stealth(false);
	wait_frame(start, LEVELS, 5, 0, 0);
	assert(led_visualizer_style() == LED_VISUALIZER_RAINBOW_BREATH);

	start = mark();
	led_update_playback(true, 176400, false, true);
	wait_frame(start, LEVELS, 13, 13, 13);
	assert(current_effect == LED_VISUALIZER_NONE);
	led_set_visualizer(false);
	start = mark();
	led_update_playback(true, PCM_RATE, false, false);
	wait_frame(start, LEVELS, 23, 23, 0);
	assert(current_effect == LED_VISUALIZER_NONE);
	led_shutdown();
	puts("ok  saved Rainbow Breath without PCM capture, correct alerts, charging, Stealth and format fallback");
}

static void test_fallback(void) {
	config_init(NULL);
	led_init();
	assert(!led_effects_available());
	led_set_visualizer_style(LED_VISUALIZER_RAINBOW_BREATH);
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
	if (argc == 1) {
		init_pcm();
		test_pcm_consumers();
		test_pulse_envelope();
		test_direct();
	} else if (!strcmp(argv[1], "glow")) {
		init_pcm();
		test_energy_capture();
		test_glow_envelope();
		test_glow();
	} else if (!strcmp(argv[1], "rainbow")) {
		init_pcm();
		test_rainbow_frames();
		test_rainbow();
	} else {
		backend = !strcmp(argv[1], "pattern") ? PATTERN : !strcmp(argv[1], "pwm") ? PWM : UNAVAILABLE;
		test_fallback();
	}
	return 0;
}
