// Regression coverage for the status LED's sample-rate color bands. Include the
// implementation so the classifier and the driver's fallback patterns can be
// checked without an LED controller or the rest of the player.

#include "../src/system/device/led.c"

static int failures;

#define CHECK(cond)                                                                            \
	do {                                                                                        \
		if (!(cond)) {                                                                          \
			failures++;                                                                     \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
		}                                                                                       \
	} while (0)

static void check_rate(int rate, bool dsd, led_state_t expected_state, int expected_pattern,
					   int expected_r, int expected_g, int expected_b) {
	led_state_t state = rate_state(rate, dsd);
	CHECK(state == expected_state);
	CHECK(state_pattern(state) == expected_pattern);

	led_render_t render;
	build_render(state, &render);
	CHECK(render.mode == LM_STEADY);
	CHECK(render.r == expected_r);
	CHECK(render.g == expected_g);
	CHECK(render.b == expected_b);
}

int main(void) {
	// The stock PCM bands are inclusive at 48 kHz and 192 kHz, with the next
	// integer rate entering the next color. At the default LED level the channel
	// values are yellow (23,23,0), aqua (0,23,23) and orange (23,11,0).
	check_rate(32000, false, LS_PCM_48K, PATTERN_PCM_48K, 23, 23, 0);
	check_rate(48000, false, LS_PCM_48K, PATTERN_PCM_48K, 23, 23, 0);
	check_rate(48001, false, LS_PCM_192K, PATTERN_PCM_192K, 0, 23, 23);
	check_rate(96000, false, LS_PCM_192K, PATTERN_PCM_192K, 0, 23, 23);
	check_rate(192000, false, LS_PCM_192K, PATTERN_PCM_192K, 0, 23, 23);
	check_rate(192001, false, LS_PCM_HI, PATTERN_PCM_HI, 23, 11, 0);

	// DoP's 176.4 kHz carrier remains in the normal PCM band unless the caller
	// identifies it as DSD; a DSD source gets white regardless of its carrier.
	check_rate(176400, false, LS_PCM_192K, PATTERN_PCM_192K, 0, 23, 23);
	led_state_t dsd = rate_state(176400, true);
	CHECK(dsd == LS_DSD);
	CHECK(state_pattern(dsd) == PATTERN_DSD_WHITE);
	CHECK(rate_state(2822400, false) == LS_DSD);

	if (failures) {
		fprintf(stderr, "%d LED rate check(s) failed\n", failures);
		return 1;
	}
	puts("ok  LED color follows the stock PCM sample-rate bands and keeps DSD white");
	return 0;
}
