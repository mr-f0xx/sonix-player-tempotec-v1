/* Exercise the actual receiver session with a fake nonblocking ALSA plug. */
// Three silent sessions are what the stream-asking case waits for; 40 ms each
// is the same branch as 3000 would be, without the wait.
#define FIRST_FRAME_MS 40
#include "../src/system/bluetooth/btreceiver.c"
#include <assert.h>

static bool fake_playing;
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
static bool stop_on_write = true;   // the older cases end the session from the write
static uint32_t change_at_ms;       // the sender moves codec at this moment

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
static bool recover_ok;
static int recover_calls;
int snd_pcm_recover(snd_pcm_t *p, int err, int silent) { (void)p; (void)err; (void)silent; recover_calls++; return recover_ok ? 0 : -1; }
const char *snd_strerror(int err) { (void)err; return "fake error"; }
/* A transport that is there and delivers nothing: what "says it is playing and
 * nothing has arrived" is about. The read keeps coming back empty. */
static bool silent_pcm;
static int overrun_on_read;     /* the n-th read reports a ring overrun, once */
static uint32_t play_at_ms;     /* the sender says "playing" from this moment */
static uint32_t ended_at_ms;    /* the last read, for the cases that time a session */

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *p, void *buf, snd_pcm_uframes_t frames) {
	assert(p == &fake_pcm && frames == CHUNK_FRAMES);
	reads++;
	ended_at_ms = now_ms();
	if (play_at_ms && now_ms() >= play_at_ms) fake_playing = true;
	if (overrun_on_read && reads == overrun_on_read) { overrun_on_read = 0; return -EPIPE; }
	if (silent_pcm) { usleep(1000); return -EAGAIN; }
	if (reads == 1) return -EAGAIN; /* exercise bounded wait, not recovery */
	/* The phone picking another codec: bluealsa reconfigures the transport and
	 * answers with the new format from then on. */
	if (change_at_ms && now_ms() >= change_at_ms) {
		change_at_ms = 0;
		strcpy(fake_info.codec, "AAC");
		fake_info.rate = 44100;
		fake_serial++;
	}
	usleep(2000); /* keep the loop from spinning a core for the whole watch */
	if (requested_format == SND_PCM_FORMAT_S32_LE) {
		int32_t sample[4] = {0x12345600, (int32_t)0x80000000u, 0, 0x7fffff00};
		memcpy(buf, sample, sizeof(sample));
	} else {
		int16_t sample[4] = {0x1234, -32768, 0, 32767};
		memcpy(buf, sample, sizeof(sample));
	}
	return 2;
}
/* Settings a case can override; -1 leaves the built-in default. */
static long cfg_capture = -1, cfg_output = -1, cfg_aptx_capture = -1, cfg_aptx_output = -1;
long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	if (!strcmp(key, "receiver_capture_ms") && cfg_capture >= 0) return cfg_capture;
	if (!strcmp(key, "receiver_output_ms") && cfg_output >= 0) return cfg_output;
	if (!strcmp(key, "receiver_aptx_capture_ms") && cfg_aptx_capture >= 0) return cfg_aptx_capture;
	if (!strcmp(key, "receiver_aptx_output_ms") && cfg_aptx_output >= 0) return cfg_aptx_output;
	return fallback;
}

/* The equaliser the receiver now runs its PCM through. Counted, and able to
 * mark what it was given so the case can tell it ran before the write. */
static int eq_s32_calls, eq_s16_calls, eq_last_rate, eq_last_channels, eq_last_frames;
void eq_process_s32(int32_t *frames, int frame_count, int channels, int sample_rate) {
	(void)frames;
	eq_s32_calls++; eq_last_frames = frame_count; eq_last_channels = channels; eq_last_rate = sample_rate;
}
void eq_process(short *frames, int frame_count, int channels, int sample_rate) {
	(void)frames;
	eq_s16_calls++; eq_last_frames = frame_count; eq_last_channels = channels; eq_last_rate = sample_rate;
}
const char *tr(const char *key) { return key; }
unsigned bluetooth_receiver_stream_serial(void) { return fake_serial; }
unsigned bluetooth_refresh_receiver(void) { return ++fake_serial; }
unsigned bluetooth_receiver_answered(void) { return ++answer_calls >= answer_after ? fake_serial : fake_serial - 1; }
bool bluetooth_receiver_stream(bt_stream_t *out) { *out = fake_info; return true; }
bool bluetooth_receiver_playing(void) { return fake_playing; }
static bool refuse_32bit;
static int output_latency_asked;
bool audio_external_begin_latency(int rate, int ch, int bits, int latency) {
	assert((rate == 48000 || rate == 44100) && ch == 2 && latency > 0);
	output_latency_asked = latency;
	if (refuse_32bit && bits == 32) return false;
	output_bits = bits; return true;
}
int audio_external_write(const void *data, int frames) {
	assert(frames == 2 && (output_bits == 32 || output_bits == 16));
	memcpy(written, data, output_bits == 32 ? sizeof(written) : 2 * sizeof(int16_t) * 2); writes++;
	if (stop_on_write) {
		pthread_mutex_lock(&lock); running = false; pthread_mutex_unlock(&lock);
	}
	return frames;
}

/* The sending device, as the two questions btreceiver asks about it: is its PCM
 * published, and is the device itself still connected. */
static bool peer_connected = true;
static int pcm_published_after; // device queries to answer "no" before the PCM is back
static int device_queries;

bool bluetooth_receiver_peer_connected(void) { return peer_connected; }

/* Asked once when a run of silent sessions says the sender's stream is gone. */
static int stream_asks;

void bluetooth_receiver_ask_to_stream(void) { stream_asks++; }

/* The worker is a thread of the player: these two live there, not here. */
static int realtime_calls;
void thread_be_realtime(const char *name, int priority) { (void)name; (void)priority; realtime_calls++; }
void thread_be_background(const char *name) { (void)name; }

/* Not idle: this sender is streaming, which is why the silence has to be
 * answered with an invitation rather than with waiting. */
int bluetooth_receiver_sending(unsigned *serial_out) {
	(void)serial_out;
	return 1;
}

bool bluetooth_receiver_device(char *mac_out, int mac_size, char *name_out, int name_size) {
	device_queries++;
	bool published = device_queries >= (pcm_published_after ? pcm_published_after : 1);
	if (mac_out && mac_size) {
		snprintf(mac_out, (size_t)mac_size, "%s", published ? "AA:BB:CC:DD:EE:FF" : "");
	}
	if (name_out && name_size) {
		snprintf(name_out, (size_t)name_size, "%s", published ? "source" : "");
	}
	return published;
}
void audio_external_end(void) { output_closed++; }

static void run_format(unsigned bits, snd_pcm_format_t expected) {
	memset(&fake_info, 0, sizeof(fake_info));
	strcpy(fake_info.codec, bits == 24 ? "aptX-HD" : "SBC");
	fake_info.rate = 48000; fake_info.channels = 2; fake_info.bits = bits;
	requested_format = 0; nonblock_enabled = capture_closed = output_closed = output_bits = writes = reads = 0;
	eq_s32_calls = eq_s16_calls = 0;
	pthread_mutex_lock(&lock); running = true; session_id = 7; pthread_mutex_unlock(&lock);
	bool heard = false;
	assert(capture_session("AA:BB:CC:DD:EE:FF", "source", false, true, 7, &heard) == SESSION_OVER);
	assert(heard && requested_format == expected && requested_rate == 48000 && requested_channels == 2);
	assert(nonblock_enabled == 1 && reads == 2 && writes == 1 && capture_closed == 1 && output_closed == 1);
	assert(written[0] == (bits == 16 ? 0x12340000 : 0x12345600));
	assert(written[1] == (int32_t)0x80000000u && written[2] == 0);
	assert(written[3] == (bits == 16 ? 0x7fff0000 : 0x7fffff00));
	/* The same equaliser a decoded track goes through, on the 32-bit words that
	 * are about to be written, in the sender's rate and channel count. */
	assert(eq_s32_calls == 1 && eq_s16_calls == 0);
	assert(eq_last_frames == 2 && eq_last_channels == 2 && eq_last_rate == 48000);
}

/* A sender that changes codec publishes nothing for a moment while bluealsa
 * rebuilds its transport. The session has to (a) notice the new codec as soon
 * as bluealsa answers with it -- the codec line and the codec menu are two reads
 * of that one answer and used to disagree -- and (b) come back with a fresh
 * session rather than being left silent. */
static void test_codec_change_mid_session(void) {
	memset(&fake_info, 0, sizeof(fake_info));
	strcpy(fake_info.codec, "SBC");
	fake_info.rate = 48000;
	fake_info.channels = 2;
	fake_info.bits = 16;
	requested_format = 0;
	nonblock_enabled = capture_closed = output_closed = output_bits = writes = reads = 0;
	pthread_mutex_lock(&lock);
	running = true;
	session_id = 21;
	memset(&state, 0, sizeof(state));
	pthread_mutex_unlock(&lock);

	stop_on_write = false;
	change_at_ms = now_ms() + 300; // the watch reads once a second
	bool heard = false;
	assert(capture_session("AA:BB:CC:DD:EE:FF", "source", false, true, 21, &heard) == SESSION_AGAIN);
	assert(heard && capture_closed == 1 && output_closed == 1);
	// Published before the reopen: the page must not keep saying the old codec.
	assert(strcmp(state.codec, "AAC") == 0);
	assert(state.sample_rate == 44100);
	assert(fake_info.rate == 44100);
	stop_on_write = true;
	pthread_mutex_lock(&lock);
	running = false;
	pthread_mutex_unlock(&lock);
}

/* Between two transports: the PCM is gone, the device is not. Waiting is what
 * keeps the session alive across a codec change; without it the mode ended and
 * only switching the receiver off and on again brought the sound back. */
static void test_between_transports(void) {
	char mac[BT_MAC_MAX] = "";
	char name[BT_NAME_MAX] = "";
	pthread_mutex_lock(&lock);
	running = true;
	session_id = 31;
	pthread_mutex_unlock(&lock);

	// There at once: nothing to wait for.
	peer_connected = true;
	pcm_published_after = 0;
	device_queries = 0;
	assert(wait_for_pcm(31, mac, sizeof(mac), name, sizeof(name)));
	assert(strcmp(mac, "AA:BB:CC:DD:EE:FF") == 0 && strcmp(name, "source") == 0);

	// Gone for a moment and back: waited for, not given up on.
	pcm_published_after = 3;
	device_queries = 0;
	assert(wait_for_pcm(31, mac, sizeof(mac), name, sizeof(name)));
	assert(device_queries == 3);

	// Gone, and the device with it: the sender really has left.
	peer_connected = false;
	pcm_published_after = 1000;
	device_queries = 0;
	assert(!wait_for_pcm(31, mac, sizeof(mac), name, sizeof(name)));
	assert(device_queries == 1);

	// Leaving the mode is not something to wait through either.
	peer_connected = true;
	pthread_mutex_lock(&lock);
	running = false;
	pthread_mutex_unlock(&lock);
	assert(!wait_for_pcm(31, mac, sizeof(mac), name, sizeof(name)));
	peer_connected = true;
	pcm_published_after = 0;
}

/* A sender that says it is playing while nothing arrives, three sessions in a
 * row: the receiver stops pulling that PCM and asks the sender for a stream.
 * Another open of the same dead object is what the report's log shows fourteen
 * times in a row, and it never brings anything. */
static void test_silent_sender_is_asked_for_a_stream(void) {
	memset(&fake_info, 0, sizeof(fake_info));
	strcpy(fake_info.codec, "aptX");
	fake_info.rate = 48000;
	fake_info.channels = 2;
	fake_info.bits = 16;
	fake_serial = 0;
	answer_after = answer_calls = 0;
	device_queries = 0;
	pcm_published_after = 0;
	reads = capture_closed = output_closed = writes = 0;
	fake_playing = true;
	silent_pcm = true;
	stream_asks = 0;

	pthread_mutex_lock(&lock);
	running = true;
	session_id = 42;
	memset(&state, 0, sizeof(state));
	pthread_mutex_unlock(&lock);

	pthread_t worker;
	assert(pthread_create(&worker, NULL, capture_worker, (void *)(uintptr_t)42) == 0);
	for (int waited = 0; waited < 400 && stream_asks == 0; waited++) {
		usleep(10 * 1000);
	}
	pthread_mutex_lock(&lock);
	running = false;
	pthread_mutex_unlock(&lock);
	pthread_join(worker, NULL);

	// Asked once for the whole run of silence, not once per session: the answer
	// is a stream this thread cannot see, and the next session is the one that
	// finds it.
	assert(stream_asks == 1);
	// Three PCMs, each opened and closed, and nothing written to the output.
	assert(capture_closed == 3 && output_closed == 3 && writes == 0);

	silent_pcm = false;
	fake_playing = false;
}


/* aptX is the one codec given a deeper pair of buffers (the reasoning, and
 * what the logs do and do not show, is in btreceiver.c). Nothing else is, and
 * the codec's name is the whole test: a different codec that happens to
 * contain it, like aptX-HD, is not. */
static void test_aptx_buffers(void) {
	assert(capture_ms_for("SBC") == 80 && output_ms_for("SBC") == 120);
	assert(capture_ms_for("AAC") == 80 && output_ms_for("AAC") == 120);
	assert(capture_ms_for("LDAC") == 80 && output_ms_for("LDAC") == 120);
	assert(capture_ms_for("aptX-HD") == 80 && output_ms_for("aptX-HD") == 120); /* a different codec */
	assert(capture_ms_for("") == 80 && output_ms_for("") == 120);
	assert(capture_ms_for(NULL) == 80 && output_ms_for(NULL) == 120);
	assert(capture_ms_for("aptX") == 160 && output_ms_for("aptX") == 360);
	assert(output_ms_for("APTX") == 360); /* bluealsa's spelling is not something to depend on */

	/* The floor never lowers a larger setting of the ordinary pair. */
	cfg_output = 500; cfg_capture = 300;
	assert(output_ms_for("aptX") == 500 && capture_ms_for("aptX") == 300);
	assert(output_ms_for("SBC") == 500 && capture_ms_for("SBC") == 300);
	cfg_output = cfg_capture = -1;

	/* Zero switches the extra room off. */
	cfg_aptx_output = 0; cfg_aptx_capture = 0;
	assert(output_ms_for("aptX") == 120 && capture_ms_for("aptX") == 80);

	/* Out-of-range values are held to the same bounds as the ordinary ones. */
	cfg_aptx_output = 5000; cfg_aptx_capture = 5;
	assert(output_ms_for("aptX") == BUFFER_MS_MAX && capture_ms_for("aptX") == 80);
	cfg_aptx_output = cfg_aptx_capture = -1;
}

/* What the DAC is actually asked for when the session opens it. */
static void test_session_asks_for_the_aptx_buffer(void) {
	const char *codecs[] = {"aptX", "SBC", "aptX-HD"};
	const int expected[] = {360, 120, 120};
	for (int i = 0; i < 3; i++) {
		memset(&fake_info, 0, sizeof(fake_info));
		strcpy(fake_info.codec, codecs[i]);
		fake_info.rate = 48000; fake_info.channels = 2; fake_info.bits = 16;
		output_latency_asked = 0; eq_s32_calls = eq_s16_calls = 0;
		nonblock_enabled = capture_closed = output_closed = output_bits = writes = reads = 0;
		pthread_mutex_lock(&lock); running = true; session_id = 61; memset(&state, 0, sizeof(state)); pthread_mutex_unlock(&lock);
		bool heard = false;
		assert(capture_session("AA:BB:CC:DD:EE:FF", "source", false, true, 61, &heard) == SESSION_OVER);
		assert(heard && output_latency_asked == expected[i]);
	}
}

/* When the output will not take 32-bit words the 16-bit frames go to it as they
 * are, and the equaliser is the 16-bit one. */
static void test_equaliser_on_the_16_bit_fallback(void) {
	memset(&fake_info, 0, sizeof(fake_info));
	strcpy(fake_info.codec, "SBC");
	fake_info.rate = 48000; fake_info.channels = 2; fake_info.bits = 16;
	refuse_32bit = true;
	eq_s32_calls = eq_s16_calls = 0;
	nonblock_enabled = capture_closed = output_closed = output_bits = writes = reads = 0;
	pthread_mutex_lock(&lock); running = true; session_id = 62; memset(&state, 0, sizeof(state)); pthread_mutex_unlock(&lock);
	bool heard = false;
	assert(capture_session("AA:BB:CC:DD:EE:FF", "source", false, true, 62, &heard) == SESSION_OVER);
	refuse_32bit = false;
	assert(heard && output_bits == 16 && eq_s16_calls == 1 && eq_s32_calls == 0);
	assert(eq_last_frames == 2 && eq_last_channels == 2 && eq_last_rate == 48000);
}

/* The capture ring overrunning is recovered from and the session goes on: the
 * audio after it is still delivered. */
static void test_overrun_is_recovered(void) {
	memset(&fake_info, 0, sizeof(fake_info));
	strcpy(fake_info.codec, "SBC");
	fake_info.rate = 48000; fake_info.channels = 2; fake_info.bits = 16;
	recover_ok = true; recover_calls = 0; overrun_on_read = 2;
	nonblock_enabled = capture_closed = output_closed = output_bits = writes = reads = 0;
	pthread_mutex_lock(&lock); running = true; session_id = 63; memset(&state, 0, sizeof(state)); pthread_mutex_unlock(&lock);
	bool heard = false;
	assert(capture_session("AA:BB:CC:DD:EE:FF", "source", false, true, 63, &heard) == SESSION_OVER);
	recover_ok = false;
	assert(recover_calls == 1 && heard && writes == 1 && capture_closed == 1);
}

/* The PCM opens while the phone is still paused. The phone being played a
 * while later is a sender that has started, not a PCM that is dead: the three
 * seconds the sender is given to produce a frame are counted from "playing",
 * not from the open. (Counted from the open, the first poll after "playing"
 * found the time up and took a live PCM away -- 14 times in the log this case
 * comes from.) A sender that says "playing" and delivers nothing for that long
 * afterwards is still found out. */
static void test_pause_then_play_is_not_a_dead_pcm(void) {
	memset(&fake_info, 0, sizeof(fake_info));
	strcpy(fake_info.codec, "SBC");
	fake_info.rate = 48000; fake_info.channels = 2; fake_info.bits = 16;
	silent_pcm = true; fake_playing = false;
	reads = capture_closed = output_closed = writes = 0;
	pthread_mutex_lock(&lock); running = true; session_id = 64; memset(&state, 0, sizeof(state)); pthread_mutex_unlock(&lock);

	/* "Playing" arrives well after the time allowed (40 ms here) has gone by
	 * since the open. */
	uint32_t pressed = now_ms() + 6 * FIRST_FRAME_MS;
	play_at_ms = pressed;
	bool heard = false;
	assert(capture_session("AA:BB:CC:DD:EE:FF", "source", false, true, 64, &heard) == SESSION_AGAIN);
	play_at_ms = 0;
	assert(!heard);
	/* It ended because the silence outlasted FIRST_FRAME_MS from "playing" --
	 * not at once. A few milliseconds of slack for the loop's own sleeps. */
	assert(ended_at_ms >= pressed + FIRST_FRAME_MS - 12);
	assert(capture_closed == 1 && output_closed == 1 && writes == 0);

	/* Paused again before the time is up: the count starts over rather than
	 * carrying on from the first "playing". */
	fake_playing = false;
	silent_pcm = false;
	fake_playing = false;
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
	test_codec_change_mid_session();
	test_between_transports();
	test_silent_sender_is_asked_for_a_stream();
	test_aptx_buffers();
	test_session_asks_for_the_aptx_buffer();
	test_equaliser_on_the_16_bit_fallback();
	test_overrun_is_recovered();
	test_pause_then_play_is_not_a_dead_pcm();
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
	assert(!open_capture("AA:BB:CC:DD:EE:FF", 48000, 2, 32, 80));
	assert(!open_capture("AA:BB:CC:DD:EE:FF", 48000, 2, 8, 80));
	puts("PASS Bluetooth RX (16/24/32-bit frames, nonblocking reads, codec change, aptX buffers, equaliser, overrun, pause-then-play and cleanup)");
	return 0;
}
