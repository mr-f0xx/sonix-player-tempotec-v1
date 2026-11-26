// USB DAC's opt-in low-latency profile: defaults, persistence and the
// existing explicit output-buffer override. The profile is kept off unless a
// user enables it; its buffer request is 20 ms instead of the normal 80 ms.
#define _GNU_SOURCE 1
#include "../src/system/audio/usbdac.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int buffer_requests[4];
static int buffer_request_count;
static int refused_buffer_ms = -1;

bool audio_external_begin_latency(int sample_rate, int channels, int bits, int buffer_ms) {
	assert(sample_rate == 48000 && channels == 2 && bits == 32);
	assert(buffer_request_count < (int)(sizeof(buffer_requests) / sizeof(buffer_requests[0])));
	buffer_requests[buffer_request_count++] = buffer_ms;
	return buffer_ms != refused_buffer_ms;
}

static void reset_buffer_requests(int refuse_ms) {
	memset(buffer_requests, 0, sizeof(buffer_requests));
	buffer_request_count = 0;
	refused_buffer_ms = refuse_ms;
}

static void test_open_and_fallback(void) {
	int active_ms = 0;
	bool fallback = false;

	reset_buffer_requests(OUTPUT_MS_LOW_LATENCY);
	assert(open_output_pcm(48000, 32, false, true, OUTPUT_MS_LOW_LATENCY, &active_ms, &fallback));
	assert(buffer_request_count == 2);
	assert(buffer_requests[0] == OUTPUT_MS_LOW_LATENCY);
	assert(buffer_requests[1] == OUTPUT_MS_DEFAULT);
	assert(active_ms == OUTPUT_MS_DEFAULT && fallback);

	reset_buffer_requests(-1);
	assert(open_output_pcm(48000, 32, false, true, OUTPUT_MS_LOW_LATENCY, &active_ms, &fallback));
	assert(buffer_request_count == 1 && buffer_requests[0] == OUTPUT_MS_LOW_LATENCY);
	assert(active_ms == OUTPUT_MS_LOW_LATENCY && !fallback);

	// A manually requested smaller buffer in standard mode retains the old
	// behavior: it does not silently switch profiles when that request fails.
	reset_buffer_requests(OUTPUT_MS_LOW_LATENCY);
	assert(!open_output_pcm(48000, 32, false, false, OUTPUT_MS_LOW_LATENCY, &active_ms, &fallback));
	assert(buffer_request_count == 1 && buffer_requests[0] == OUTPUT_MS_LOW_LATENCY);
	assert(active_ms == OUTPUT_MS_LOW_LATENCY && !fallback);
}

static void reload_latency_settings(const char *path) {
	// The production module loads these once, at first use. Resetting the
	// private cache here models a process restart while keeping the fixture
	// in one small test binary.
	latency_settings_loaded = false;
	low_latency_enabled = false;
	output_ms_override_present = false;
	output_ms_override = 0;
	config_init(path);
}

static bool file_contains(const char *path, const char *needle) {
	FILE *file = fopen(path, "r");
	if (!file) {
		return false;
	}
	char contents[4096];
	size_t size = fread(contents, 1, sizeof(contents) - 1, file);
	contents[size] = '\0';
	fclose(file);
	return strstr(contents, needle) != NULL;
}

int main(void) {
	test_open_and_fallback();

	char dir[] = "/tmp/sonix-usbdac-latency-XXXXXX";
	assert(mkdtemp(dir));
	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/device_config.ini", dir);

	// No saved key means the existing standard 80 ms profile, so existing
	// users do not opt into different buffering or thread scheduling.
	assert(!config_init(config_path));
	assert(!usbdac_low_latency_enabled());
	assert(output_ms_for_mode(NULL) == OUTPUT_MS_DEFAULT);

	unsigned before = usbdac_serial();
	usbdac_set_low_latency(true);
	assert(usbdac_serial() != before);
	assert(usbdac_low_latency_enabled());
	assert(output_ms_for_mode(NULL) == OUTPUT_MS_LOW_LATENCY);
	assert(file_contains(config_path, "dac_low_latency = 1"));

	// Reload the file as a fresh process would. The mode remains enabled and
	// chooses the smaller request without changing the hidden manual override.
	reload_latency_settings(config_path);
	assert(usbdac_low_latency_enabled());
	assert(output_ms_for_mode(NULL) == OUTPUT_MS_LOW_LATENCY);

	usbdac_set_low_latency(false);
	assert(!usbdac_low_latency_enabled());
	assert(output_ms_for_mode(NULL) == OUTPUT_MS_DEFAULT);

	// Preserve the existing explicit [usb] dac_output_ms tuning knob, including
	// its bounds, and let it take precedence over the two built-in profiles.
	config_set_int("usb", "dac_output_ms", 120);
	assert(config_save());
	reload_latency_settings(config_path);
	assert(output_ms_for_mode(NULL) == 120);
	usbdac_set_low_latency(true);
	assert(output_ms_for_mode(NULL) == 120);

	config_set_int("usb", "dac_output_ms", 1);
	assert(config_save());
	reload_latency_settings(config_path);
	assert(output_ms_for_mode(NULL) == OUTPUT_MS_MIN);
	config_set_int("usb", "dac_output_ms", 900);
	assert(config_save());
	reload_latency_settings(config_path);
	assert(output_ms_for_mode(NULL) == OUTPUT_MS_MAX);

	puts("usbdac: low-latency profile defaults off, persists, falls back safely and respects buffer overrides");
	return 0;
}
