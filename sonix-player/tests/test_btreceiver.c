/* Exercise the actual receiver session with a fake nonblocking ALSA plug. */
#include "../src/system/bluetooth/btreceiver.c"
#include <assert.h>

static snd_pcm_t fake_pcm;
static snd_pcm_format_t requested_format;
static unsigned requested_rate, requested_channels;
static int nonblock_enabled, capture_closed, output_closed, output_bits, writes, reads;
static bool test_fail_nonblock;
static bt_stream_t fake_info;
static unsigned fake_serial;
static int answer_after, answer_calls;
static int32_t written[4];
static int fake_fd = -1;
static unsigned short fake_revents;

int snd_pcm_open(snd_pcm_t **p, const char *name, int stream, int mode) {
	assert(strstr(name, "PROFILE=a2dp") && stream == SND_PCM_STREAM_CAPTURE && mode == 0);
	*p = &fake_pcm; return 0;
}
int snd_pcm_close(snd_pcm_t *p) { assert(p == &fake_pcm); capture_closed++; return 0; }
int snd_pcm_set_params(snd_pcm_t *p, snd_pcm_format_t f, int access, unsigned ch, unsigned rate, int resample, unsigned latency) {
	assert(p == &fake_pcm && access == SND_PCM_ACCESS_RW_INTERLEAVED && resample == 0 && latency > 0);
	requested_format = f; requested_channels = ch; requested_rate = rate; return 0;
}
int snd_pcm_nonblock(snd_pcm_t *p, int on) { assert(p == &fake_pcm && on == 1); nonblock_enabled++; return test_fail_nonblock ? -EIO : 0; }
int snd_pcm_poll_descriptors_count(snd_pcm_t *p) { assert(p == &fake_pcm); return fake_fd < 0 ? 0 : 1; }
int snd_pcm_poll_descriptors(snd_pcm_t *p, struct pollfd *fds, unsigned n) {
	assert(p == &fake_pcm && n == 1); fds[0] = (struct pollfd){.fd = fake_fd, .events = POLLIN}; return 1;
}
int snd_pcm_poll_descriptors_revents(snd_pcm_t *p, struct pollfd *fds, unsigned n, unsigned short *events) {
	assert(p == &fake_pcm && fds && n == 1); *events = fake_revents; return 0;
}
int snd_pcm_recover(snd_pcm_t *p, int err, int silent) { (void)p; (void)err; (void)silent; return -1; }
const char *snd_strerror(int err) { (void)err; return "fake error"; }
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *p, void *buf, snd_pcm_uframes_t frames) {
	assert(p == &fake_pcm && frames == CHUNK_FRAMES);
	reads++;
	if (reads == 1) return -EAGAIN; /* exercise bounded wait, not recovery */
	if (requested_format == SND_PCM_FORMAT_S32_LE) {
		int32_t sample[4] = {0x12345600, (int32_t)0x80000000u, 0, 0x7fffff00};
		memcpy(buf, sample, sizeof(sample));
	} else {
		int16_t sample[4] = {0x1234, -32768, 0, 32767};
		memcpy(buf, sample, sizeof(sample));
	}
	return 2;
}
long config_get_int(const char *section, const char *key, long fallback) { (void)section; (void)key; return fallback; }
const char *tr(const char *key) { return key; }
unsigned bluetooth_receiver_stream_serial(void) { return fake_serial; }
unsigned bluetooth_refresh_receiver(void) { return ++fake_serial; }
unsigned bluetooth_receiver_answered(void) { return ++answer_calls >= answer_after ? fake_serial : fake_serial - 1; }
bool bluetooth_receiver_stream(bt_stream_t *out) { *out = fake_info; return true; }
bool bluetooth_receiver_playing(void) { return false; }
bool audio_external_begin_latency(int rate, int ch, int bits, int latency) {
	assert(rate == 48000 && ch == 2 && latency > 0); output_bits = bits; return true;
}
int audio_external_write(const void *data, int frames) {
	assert(frames == 2 && output_bits == 32);
	memcpy(written, data, sizeof(written)); writes++;
	pthread_mutex_lock(&lock); running = false; pthread_mutex_unlock(&lock);
	return frames;
}
void audio_external_end(void) { output_closed++; }

static void run_format(unsigned bits, snd_pcm_format_t expected) {
	memset(&fake_info, 0, sizeof(fake_info));
	strcpy(fake_info.codec, bits == 24 ? "aptX-HD" : "SBC");
	fake_info.rate = 48000; fake_info.channels = 2; fake_info.bits = bits;
	requested_format = 0; nonblock_enabled = capture_closed = output_closed = output_bits = writes = reads = 0;
	pthread_mutex_lock(&lock); running = true; session_id = 7; pthread_mutex_unlock(&lock);
	bool heard = false;
	assert(capture_session("AA:BB:CC:DD:EE:FF", "source", false, true, 7, &heard) == SESSION_OVER);
	assert(heard && requested_format == expected && requested_rate == 48000 && requested_channels == 2);
	assert(nonblock_enabled == 1 && reads == 2 && writes == 1 && capture_closed == 1 && output_closed == 1);
	assert(written[0] == (bits == 16 ? 0x12340000 : 0x12345600));
	assert(written[1] == (int32_t)0x80000000u && written[2] == 0);
	assert(written[3] == (bits == 16 ? 0x7fff0000 : 0x7fffff00));
}

int main(void) {
	// An old in-flight response must not satisfy the newly issued ticket.
	pthread_mutex_lock(&lock); running = true; session_id = 7; pthread_mutex_unlock(&lock);
	fake_info.rate = 48000; fake_info.channels = 2; fake_info.bits = 24; strcpy(fake_info.codec, "aptX-HD");
	answer_after = 3; answer_calls = 0;
	bt_stream_t fresh; unsigned serial;
	assert(read_stream_info(&fresh, &serial, 7));
	assert(answer_calls == 3 && fresh.bits == 24);
	answer_after = 0;
	run_format(16, SND_PCM_FORMAT_S16_LE);
	run_format(24, SND_PCM_FORMAT_S32_LE);
	run_format(32, SND_PCM_FORMAT_S32_LE);
	int fds[2]; assert(pipe(fds) == 0); assert(write(fds[1], "x", 1) == 1);
	fake_fd = fds[0]; fake_revents = POLLIN;
	assert(wait_capture(&fake_pcm) == 1);
	fake_revents = 0; assert(wait_capture(&fake_pcm) == 0);
	fake_revents = POLLERR; assert(wait_capture(&fake_pcm) == -EIO);
	close(fds[0]); close(fds[1]); fake_fd = -1;
	// A worker from an old visit cannot overwrite the new session's display.
	pthread_mutex_lock(&lock); running = true; session_id = 8; state.error[0] = '\0'; pthread_mutex_unlock(&lock);
	set_error(7, "stale"); assert(!state.error[0]);
	publish_format(7, "stale device", &fake_info); assert(strcmp(state.device, "stale device") != 0);
	test_fail_nonblock = true;
	assert(!open_capture("AA:BB:CC:DD:EE:FF", 48000, 2, 32));
	assert(!open_capture("AA:BB:CC:DD:EE:FF", 48000, 2, 8));
	puts("PASS Bluetooth RX (16/24/32-bit frames, nonblocking reads and cleanup)");
	return 0;
}
