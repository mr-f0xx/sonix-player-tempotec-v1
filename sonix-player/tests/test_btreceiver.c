/* Exercise the actual receiver session with a fake nonblocking ALSA plug. */
// Three silent sessions are what the stream-asking case waits for; 40 ms each
// is the same branch as 3000 would be, without the wait.
#define FIRST_FRAME_MS 40
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
int snd_pcm_recover(snd_pcm_t *p, int err, int silent) { (void)p; (void)err; (void)silent; return -1; }
const char *snd_strerror(int err) { (void)err; return "fake error"; }
/* A transport that is there and delivers nothing: what "says it is playing and
 * nothing has arrived" is about. The read keeps coming back empty. */
static bool silent_pcm;

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *p, void *buf, snd_pcm_uframes_t frames) {
	// Never more than one chunk is asked for, and never more than the ring has
	// room for: both are the receiver's own promises to the phone.
	assert(p == &fake_pcm && frames > 0 && frames <= CHUNK_FRAMES);
	reads++;
	if (silent_pcm) return -EAGAIN;
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
// The ring's depth is a config key, and the defaults would have the fake
// session waiting three seconds for a target a two-frame-per-read fake takes
// four hundred reads to reach. The floor it allows is what the test asks for.
long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	if (strcmp(key, "receiver_jitter_ms") == 0) return 20;
	if (strcmp(key, "receiver_ring_ms") == 0) return 60;
	return fallback;
}
/* The codec menu, as the receiver reads it: one codec, never the one the link is
 * stuck on (the caller filters that), so a stuck decoder always has somewhere to
 * go. The recovery itself is recorded so the test can see it was asked for. */
static int recoveries;
static char recovered[BT_CODEC_MAX];
static bool no_codecs; // a link that offers nothing: nothing to recover onto
int bluetooth_receiver_codecs(char out[][BT_CODEC_MAX], int max, char *selected, int selected_size) {
	(void)selected;
	(void)selected_size;
	if (max < 1 || no_codecs) return 0;
	const char *names[] = {"aptX", "AAC"};
	int count = (int)(sizeof(names) / sizeof(names[0]));
	if (count > max) count = max;
	for (int i = 0; i < count; i++) snprintf(out[i], BT_CODEC_MAX, "%s", names[i]);
	return count;
}
bool bluetooth_receiver_recover_codec(const char *codec) {
	recoveries++;
	snprintf(recovered, sizeof(recovered), "%s", codec ? codec : "");
	return true;
}
const char *tr(const char *key) { return key; }
unsigned bluetooth_receiver_stream_serial(void) { return fake_serial; }
unsigned bluetooth_refresh_receiver(void) { return ++fake_serial; }
unsigned bluetooth_receiver_answered(void) { return ++answer_calls >= answer_after ? fake_serial : fake_serial - 1; }
bool bluetooth_receiver_stream(bt_stream_t *out) { *out = fake_info; return true; }
static bool fake_playing;
bool bluetooth_receiver_playing(void) { return fake_playing; }
bool audio_external_begin_latency(int rate, int ch, int bits, int latency) {
	assert(rate == 48000 && ch == 2 && latency > 0); output_bits = bits; return true;
}
// The output thread writes whatever it took out of the ring, which is a chunk
// rather than one read: what matters here is that the samples arriving are the
// right ones, in the right order, widened the same way. The first four are kept
// and the rest counted. Writing any of them stops the session, so the capture
// side has to notice and the ring has to be played out for this to return.
static int written_frames;
static int64_t written_samples;
int audio_external_write(const void *data, int frames) {
	assert(output_bits == 32 && frames > 0);
	if (written_frames < 4) {
		const int32_t *samples = data;
		for (int i = 0; i < frames * 2 && written_frames < 4; i++) {
			written[written_frames++] = samples[i];
		}
	}
	written_samples += frames * 2;
	writes++;
	// The sessions that read this as "the user is done" end themselves here;
	// the ones about a codec change keep producing, so that the change is what
	// ends them and not the write.
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
	written_frames = 0; written_samples = 0;
	pthread_mutex_lock(&lock); running = true; session_id = 7; pthread_mutex_unlock(&lock);
	bool heard = false;
	assert(capture_session("AA:BB:CC:DD:EE:FF", "source", false, true, 7, &heard) == SESSION_OVER);
	assert(heard && requested_format == expected && requested_rate == 48000 && requested_channels == 2);
	assert(nonblock_enabled == 1 && reads > 2 && writes >= 1 && capture_closed == 1 && output_closed == 1);
	assert(written_samples > 0);
	assert(written[0] == (bits == 16 ? 0x12340000 : 0x12345600));
	assert(written[1] == (int32_t)0x80000000u && written[2] == 0);
	assert(written[3] == (bits == 16 ? 0x7fff0000 : 0x7fffff00));
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

// The condition that moves a link off a decoder that will not run: frames have
// arrived, none has reached the DAC, and it has been long enough.
static void test_stuck_decoder(void) {
	uint32_t began = 1000;
	// Frames arriving and nothing out, after the window: stuck.
	assert(decoder_stuck(true, 2400, 0, began, began + DECODER_STUCK_MS));
	// Too early: a slow start is not a stuck decoder.
	assert(!decoder_stuck(true, 2400, 0, began, began + DECODER_STUCK_MS - 1));
	// Something reached the DAC: whatever is wrong, it is not this.
	assert(!decoder_stuck(true, 2400, 64, began, began + DECODER_STUCK_MS * 2));
	// Nothing has arrived: a silent sender, which the first-frame watchdog and
	// the AVRCP status own, not this.
	assert(!decoder_stuck(false, 0, 0, began, began + DECODER_STUCK_MS * 2));
	assert(!decoder_stuck(true, 0, 0, began, began + DECODER_STUCK_MS * 2));
}

// A link whose decoder will not run has to be moved to another codec, and never
// onto the one it is stuck on.
static void test_next_codec(void) {
	char out[BT_CODEC_MAX];
	// The first codec on the link that is not the one it is stuck on.
	assert(next_codec_offered("aptX", out, sizeof(out)));
	assert(strcmp(out, "AAC") == 0);
	assert(next_codec_offered("AAC", out, sizeof(out)));
	assert(strcmp(out, "aptX") == 0);
	// A codec the link does not offer at all: the first one it does.
	assert(next_codec_offered("LDAC", out, sizeof(out)));
	assert(strcmp(out, "aptX") == 0);
	// Nothing at all to move to is the caller's "leave the link alone".
	no_codecs = true;
	assert(!next_codec_offered("aptX", out, sizeof(out)));
	assert(out[0] == '\0');
	no_codecs = false;
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
	test_stuck_decoder();
	test_next_codec();
	puts("PASS Bluetooth RX (16/24/32-bit frames, nonblocking reads, codec change and cleanup)");
	return 0;
}
