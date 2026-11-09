#define _GNU_SOURCE 1

#include "btreceiver.h"

#include "src/system/audio/audio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// How long to wait for the worker's answer about what the stream carries before
// opening with the defaults below. It is one D-Bus round trip on a thread that
// is otherwise idle, so it arrives in milliseconds; the wait is a bound, not a
// delay.
#define INFO_WAIT_MS 1200
#define INFO_POLL_MS 40

// What A2DP is when nobody has said otherwise. SBC at 44.1 is the one codec
// every source must implement, so a sender that answers nothing is far more
// likely to be this than anything else.
#define FALLBACK_RATE 44100
#define FALLBACK_CHANNELS 2
#define FALLBACK_BITS 16

// A read at a time. Small enough that stopping is quick -- the loop notices
// `running` between reads -- and large enough that the syscall is not the work.
#define CHUNK_FRAMES 512

// The two buffers between the phone and the ear, and together they are most of
// the delay: bluealsa decodes into the first, this thread reads from it and
// writes into the second, which is the DAC's.
//
// Both are deliberately short. The player's own playback opens a deep output
// buffer because nothing is waiting on it; here something is -- a video on the
// phone whose picture is not going through the player -- and a delay that does
// not matter for music is plainly wrong against moving lips.
//
// Short buffers are also where dropouts come from, so both are settings. Raise
// them if the audio breaks up:
//
//   [bluetooth]
//   receiver_capture_ms = 80    bluealsa's side
//   receiver_output_ms  = 120   the DAC's side
#define CAPTURE_MS_DEFAULT 80
#define OUTPUT_MS_DEFAULT 120

// Below this neither side has room for a late turn of this thread, whatever the
// config says.
#define BUFFER_MS_MIN 30
#define BUFFER_MS_MAX 750

static int capture_ms(void) {
	int ms = (int)config_get_int("bluetooth", "receiver_capture_ms", CAPTURE_MS_DEFAULT);
	return ms < BUFFER_MS_MIN ? BUFFER_MS_MIN : (ms > BUFFER_MS_MAX ? BUFFER_MS_MAX : ms);
}

static int output_ms(void) {
	int ms = (int)config_get_int("bluetooth", "receiver_output_ms", OUTPUT_MS_DEFAULT);
	return ms < BUFFER_MS_MIN ? BUFFER_MS_MIN : (ms > BUFFER_MS_MAX ? BUFFER_MS_MAX : ms);
}

// How long after the last frame the stream still counts as live. A sender that
// pauses simply stops sending: nothing fails, and snd_pcm_readi waits. So
// "streaming" cannot be a flag something turns off -- it has to be a question
// about how long ago the last frame was.
#define SILENCE_MS 900

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
// A replacement session may start while the detached old worker is closing.
// Only one may own the shared external PCM; never wait for this on the GUI.
static pthread_mutex_t capture_owner = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static bool running;

static unsigned session_id;
static btreceiver_state_t state;
static unsigned serial;
static uint32_t last_frame_ms;

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

// Under `lock`.
static void touch(void) { serial++; }

// Under `lock`.
static bool sounding(void) { return last_frame_ms != 0 && (now_ms() - last_frame_ms) < SILENCE_MS; }

static void set_error(unsigned session, const char *text) {
	pthread_mutex_lock(&lock);
	if (running && session == session_id) {
		snprintf(state.error, sizeof(state.error), "%s", text ? text : "");
		last_frame_ms = 0;
		touch();
	}
	pthread_mutex_unlock(&lock);
}

bool btreceiver_available(void) { return bluetooth_receiver_device(NULL, 0, NULL, 0); }

bool btreceiver_is_active(void) {
	pthread_mutex_lock(&lock);
	bool on = state.active;
	pthread_mutex_unlock(&lock);
	return on;
}

bool btreceiver_is_streaming(void) {
	pthread_mutex_lock(&lock);
	bool live = state.active && sounding();
	pthread_mutex_unlock(&lock);
	return live;
}

void btreceiver_get_state(btreceiver_state_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&lock);
	*out = state;
	out->streaming = state.active && sounding();
	pthread_mutex_unlock(&lock);
}

unsigned btreceiver_serial(void) {
	pthread_mutex_lock(&lock);
	unsigned now = serial;
	pthread_mutex_unlock(&lock);
	return now;
}

// ---------------------------------------------------------------------------
// what the link is carrying
// ---------------------------------------------------------------------------

// Asks the Bluetooth worker and waits for the answer, but only up to a bound:
// the capture cannot be opened without a rate, and guessing wrong is better
// than a mode that never starts. True when bluealsa actually answered AFTER
// being asked; false when the numbers below are a guess.
//
// The ticket excludes an earlier query already in flight: a cached answer from
// the transport just torn down can open successfully but never carry a frame.
static bool session_live(unsigned session);

static bool read_stream_info(bt_stream_t *out, unsigned *serial_out, unsigned session) {
	memset(out, 0, sizeof(*out));
	unsigned ticket = bluetooth_refresh_receiver();

	for (int waited = 0; waited < INFO_WAIT_MS && session_live(session); waited += INFO_POLL_MS) {
		usleep(INFO_POLL_MS * 1000);
		unsigned answered = bluetooth_receiver_stream_serial();
		unsigned completed = bluetooth_receiver_answered();
		if (ticket && (int32_t)(completed - ticket) >= 0 && bluetooth_receiver_stream(out) && out->rate > 0) {
			if (serial_out) {
				*serial_out = answered;
			}
			return true;
		}
	}

	fprintf(stderr, "btreceiver: bluealsa did not say what it is decoding; assuming SBC %d Hz\n", FALLBACK_RATE);
	memset(out, 0, sizeof(*out));
	out->rate = FALLBACK_RATE;
	out->channels = FALLBACK_CHANNELS;
	out->bits = FALLBACK_BITS;
	if (serial_out) {
		*serial_out = bluetooth_receiver_stream_serial();
	}
	return false;
}

static void publish_format(unsigned session, const char *device, const bt_stream_t *info) {
	pthread_mutex_lock(&lock);
	if (!running || session != session_id) {
		pthread_mutex_unlock(&lock);
		return;
	}
	snprintf(state.device, sizeof(state.device), "%s", device ? device : "");
	snprintf(state.codec, sizeof(state.codec), "%s", info->codec);
	state.sample_rate = info->rate;
	state.channels = info->channels;
	state.bits = info->bits;
	touch();
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// the capture loop
// ---------------------------------------------------------------------------

static snd_pcm_t *open_capture(const char *mac, unsigned rate, unsigned channels, unsigned bits) {
	char device[96];
	snprintf(device, sizeof(device), "bluealsa:DEV=%s,PROFILE=a2dp", mac);

	// Opened for capture, and that one argument is the whole of the direction:
	// the bluealsa plugin picks the A2DP sink PCM for a capture stream and the
	// source PCM for a playback one, so the same name means the headphones in
	// one place and the phone in the other.
	snd_pcm_t *pcm = NULL;
	int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		fprintf(stderr, "btreceiver: %s would not open for capture: %s\n", device, snd_strerror(err));
		return NULL;
	}

	// The plugin is a `plug`: request an explicit client format. BlueALSA's
	// 24-bit formats may be packed or in four-byte containers; asking ALSA to
	// convert either to left-aligned S32 avoids assuming their physical width.
	if (bits != 16 && bits != 32) {
		snd_pcm_close(pcm);
		return NULL;
	}
	snd_pcm_format_t format = (bits == 32) ? SND_PCM_FORMAT_S32_LE : SND_PCM_FORMAT_S16_LE;
	err = snd_pcm_set_params(pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED, (unsigned)channels, (unsigned)rate,
							 0, // no resampling: the rate is the sender's and has to stay it
							 (unsigned)capture_ms() * 1000u);
	if (err < 0) {
		fprintf(stderr, "btreceiver: %u Hz %u ch %u bit refused: %s\n", rate, channels, bits, snd_strerror(err));
		snd_pcm_close(pcm);
		return NULL;
	}
	// Never block in ALSA's bluealsa plugin on this SCHED_FIFO thread. A
	// disconnected or idle source can otherwise trap the reader indefinitely
	// (or spin on a control fd), starving the interface and preventing stop.
	err = snd_pcm_nonblock(pcm, 1);
	if (err < 0) {
		fprintf(stderr, "btreceiver: capture nonblocking mode refused: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		return NULL;
	}
	return pcm;
}

#define CAPTURE_WAIT_MS 100
#define CAPTURE_POLL_FDS_MAX 8

// One bounded poll, rather than snd_pcm_wait() which can spin indefinitely on
// a bluealsa control fd even when no audio is ready. Match the TX-side fix.
static int wait_capture(snd_pcm_t *pcm) {
	struct pollfd fds[CAPTURE_POLL_FDS_MAX];
	int count = snd_pcm_poll_descriptors_count(pcm);
	if (count <= 0 || count > CAPTURE_POLL_FDS_MAX) {
		usleep(5000);
		return 0;
	}
	count = snd_pcm_poll_descriptors(pcm, fds, (unsigned)count);
	if (count <= 0 || count > CAPTURE_POLL_FDS_MAX) {
		usleep(5000);
		return 0;
	}
	int ready = poll(fds, (nfds_t)count, CAPTURE_WAIT_MS);
	if (ready < 0) return errno == EINTR ? 0 : -errno;
	if (!ready) return 0;
	unsigned short events = 0;
	int err = snd_pcm_poll_descriptors_revents(pcm, fds, (unsigned)count, &events);
	if (err < 0) return err;
	if (events & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
	if (events & POLLIN) return 1;
	usleep(5000);
	return 0;
}

// How often the loop asks bluealsa what it is decoding, to notice a change the
// ALSA handle has not reported yet.
#define WATCH_MS 1000

// A read that fails, recovers and still brings nothing, this many times in a
// row, is not a sender pausing between tracks: it is a PCM that is not there
// any more.
#define EMPTY_RECOVERIES 40

// How long a PCM that has opened is given to carry its first frame while the
// sender says it is playing. Past that it is not a quiet stream, it is a handle
// on something that is not there: an open on the wrong transport does not fail,
// it goes silent.
// Overridable so a host test does not have to sit out three real seconds per
// silent session to reach the branch below it.
#ifndef FIRST_FRAME_MS
#define FIRST_FRAME_MS 3000
#endif

typedef enum {
	SESSION_OVER,	// nothing more to try: the mode ends
	SESSION_AGAIN,	// the configuration moved; open whatever replaced it
	SESSION_NO_PCM, // there is nothing to open right now, which may be temporary
} session_t;

// One session on one PCM. `may_guess` is for the last attempt only: half way
// through a codec change the fallback numbers would describe the transport that
// has just been torn down, and an open that succeeded on those would be worse
// than one that failed. On the way out, a guess is better than nothing.
//
// `watchdog` arms the first-frame check. It is off once a few sessions in a row
// have opened and stayed silent, so a sender that is simply not playing -- or
// one whose AVRCP status lies -- does not get the PCM pulled out from under it
// every three seconds for ever.
static session_t capture_session(const char *mac, const char *name, bool may_guess, bool watchdog, unsigned session,
								 bool *heard_out) {
	*heard_out = false;

	bt_stream_t info;
	unsigned info_serial = 0;
	bool told = read_stream_info(&info, &info_serial, session);
	if (!session_live(session)) return SESSION_OVER;
	if (!told && !may_guess) {
		return SESSION_NO_PCM;
	}
	unsigned rate = info.rate ? info.rate : FALLBACK_RATE;
	unsigned channels = info.channels ? info.channels : FALLBACK_CHANNELS;
	unsigned in_bits = info.bits ? info.bits : FALLBACK_BITS;
	if (in_bits != 16 && in_bits != 24 && in_bits != 32) {
		fprintf(stderr, "btreceiver: unsupported %u-bit capture format\n", in_bits);
		return SESSION_NO_PCM;
	}
	// 24-bit source samples are requested from the ALSA plug as S32_LE.
	// The output receives that actual representation, not the source's bit
	// depth; using 24 to size S16 capture data corrupts frames and channels.
	unsigned capture_bits = in_bits == 24 ? 32 : in_bits;
	publish_format(session, name, &info);

	snd_pcm_t *pcm = open_capture(mac, rate, channels, capture_bits);
	if (!pcm) {
		return SESSION_NO_PCM;
	}
	if (!session_live(session)) {
		snd_pcm_close(pcm);
		return SESSION_OVER;
	}

	// The output, and the same widening the AirPlay receiver needs: this DAC
	// takes 32-bit words, so 16-bit frames are left-justified into the top half
	// of one rather than handed over as they came. Asked in that order because
	// 32 is the path the hardware is known to take.
	bool widen = false;
	bool opened = false;
	int buffer_ms = output_ms();
	if (capture_bits == 16) {
		opened = audio_external_begin_latency((int)rate, (int)channels, 32, buffer_ms);
		widen = opened;
	}
	if (!opened) {
		opened = audio_external_begin_latency((int)rate, (int)channels, (int)capture_bits, buffer_ms);
	}
	if (!opened) {
		fprintf(stderr, "btreceiver: the output would not take %u Hz %u bit\n", rate, in_bits);
		set_error(session, tr("btreceiver_output_refused"));
		snd_pcm_close(pcm);
		return SESSION_OVER;
	}

	if (!session_live(session)) {
		audio_external_end();
		snd_pcm_close(pcm);
		return SESSION_OVER;
	}

	size_t in_frame = (size_t)channels * (capture_bits / 8);
	void *chunk = malloc(CHUNK_FRAMES * in_frame);
	int32_t *wide = widen ? malloc(CHUNK_FRAMES * (size_t)channels * sizeof(int32_t)) : NULL;
	if (!chunk || (widen && !wide)) {
		free(chunk);
		free(wide);
		audio_external_end();
		snd_pcm_close(pcm);
		set_error(session, tr("btreceiver_not_enough_memory"));
		return SESSION_OVER;
	}

	fprintf(stderr, "btreceiver: %s is sending %s %u Hz %u ch %u bit\n", mac, info.codec[0] ? info.codec : "?", rate,
			channels, in_bits);

	// The PCM is open: whatever the last attempt complained about is over.
	set_error(session, NULL);

	bool again = false;
	bool heard = false;
	int empty = 0;
	uint32_t began = now_ms();
	uint32_t watched = now_ms();

	for (;;) {
		pthread_mutex_lock(&lock);
		bool go = running && session == session_id;
		pthread_mutex_unlock(&lock);
		if (!go) {
			break;
		}

		// What the other end is sending, asked of bluealsa rather than of the
		// handle. Changing codec on the phone is a new transport and a new PCM
		// object: the old handle is left pointing at something that is not
		// there, and whether it says so or simply waits is not a thing to rely
		// on. So the question is asked outright, once a second.
		//
		// Only an answer NEWER than the one this session opened with counts:
		// one second after a reopen the cache still holds the format from
		// before the change, and comparing against that would tear the
		// session down a second after it started, for ever, in silence.
		if (now_ms() - watched >= WATCH_MS) {
			watched = now_ms();
			bt_stream_t now;
			bluetooth_refresh_receiver();
			unsigned answered = bluetooth_receiver_stream_serial();
			if (answered != info_serial && bluetooth_receiver_stream(&now) && now.rate > 0 &&
				(now.rate != rate || now.channels != channels || now.bits != in_bits ||
				 strcmp(now.codec, info.codec) != 0)) {
				// Published before the reopen, not after it. The codec line on
				// the page and the tick in its codec menu are two reads of this
				// one answer, and leaving this one until the new session opened
				// is what made them disagree for a moment after every change:
				// the menu said AAC because bluealsa had already said so, while
				// the line above still said the codec the old session opened
				// with. Both now follow bluealsa as soon as it has moved on.
				bt_stream_t seen = now;
				if (!seen.codec[0]) {
					snprintf(seen.codec, sizeof(seen.codec), "%s", info.codec);
				}
				publish_format(session, name, &seen);
				fprintf(stderr, "btreceiver: the sender moved to %s %u Hz %u bit; opening it again\n",
						now.codec[0] ? now.codec : "?", now.rate, now.bits);
				again = true;
				break;
			}
		}

		// A PCM that opened on a transport that is no longer there does not fail
		// and does not error: it simply never delivers. The sender's own AVRCP
		// status is what separates that from a sender that is paused.
		if (watchdog && !heard && now_ms() - began >= FIRST_FRAME_MS && bluetooth_receiver_playing()) {
			fprintf(stderr, "btreceiver: %s says it is playing and nothing has arrived in %d ms; "
							"this PCM is not the live one\n",
					mac, FIRST_FRAME_MS);
			again = true;
			break;
		}

		snd_pcm_sframes_t got = snd_pcm_readi(pcm, chunk, CHUNK_FRAMES);
		if (got == -EAGAIN || got == 0) {
			int ready = wait_capture(pcm);
			if (ready > 0) usleep(1000); // a lying readiness cannot spin FIFO
			if (ready >= 0) continue;
			got = ready;
		}
		if (got < 0) {
			// An overrun or a suspend is recoverable and common: the sender
			// pauses between tracks and nothing arrives for a moment.
			if (snd_pcm_recover(pcm, (int)got, 1) == 0 && ++empty < EMPTY_RECOVERIES) {
				pthread_mutex_lock(&lock);
				if (running && session == session_id) {
					last_frame_ms = 0;
					touch();
				}
				pthread_mutex_unlock(&lock);
				continue;
			}
			// Either it could not be recovered, or recovering it has stopped
			// meaning anything. Both are answered the same way: let go of this
			// PCM and look for the one that replaced it.
			fprintf(stderr, "btreceiver: the stream stopped: %s\n", snd_strerror((int)got));
			again = true;
			break;
		}
		if (!session_live(session)) break;
		empty = 0;
		heard = true;

		pthread_mutex_lock(&lock);
		if (!running || session != session_id) {
			pthread_mutex_unlock(&lock);
			break;
		}
		bool was = sounding();
		last_frame_ms = now_ms();
		if (!was) {
			state.error[0] = '\0';
			touch(); // silence to sound is a change worth redrawing for
		}
		pthread_mutex_unlock(&lock);

		if (widen) {
			const int16_t *src = chunk;
			for (snd_pcm_sframes_t i = 0; i < got * (snd_pcm_sframes_t)channels; i++) {
				wide[i] = (int32_t)((uint32_t)(uint16_t)src[i] << 16);
			}
			audio_external_write(wide, (int)got);
		} else {
			audio_external_write(chunk, (int)got);
		}
	}

	free(chunk);
	free(wide);
	audio_external_end();
	snd_pcm_close(pcm);
	*heard_out = heard;
	return again ? SESSION_AGAIN : SESSION_OVER;
}

// How long the sender is given to come back with a new PCM before the mode
// gives up. A codec change is a teardown and a rebuild: the object goes away
// and reappears, and in between there is nothing to open.
#define REOPEN_WAIT_MS 5000
#define REOPEN_POLL_MS 200

// How many sessions in a row may open and carry nothing before the first-frame
// watchdog is put away. It exists to catch a handle on a dead transport; a
// sender that is simply quiet must not be reopened for ever because of it.
#define SILENT_RUNS_MAX 3

typedef enum {
	REOPEN_TRY,		// ask bluealsa again and open what it names
	REOPEN_GUESS,	// the window is up: one attempt on the fallback numbers
	REOPEN_GIVE_UP, // and then the mode ends
} reopen_t;

// Kept apart from the loop so it can be exercised without a Bluetooth stack.
// `blind` is false while the last open worked.
//
// The guess is counted rather than given a stretch of clock of its own: with a
// window, one slow round could step over it and the mode would end without ever
// having tried the fallback numbers. Counted, there is always exactly one last
// attempt, whatever the round took.
static reopen_t reopen_decision(bool blind, uint32_t blind_for_ms, bool guessed) {
	if (!blind || blind_for_ms < REOPEN_WAIT_MS) {
		return REOPEN_TRY;
	}
	return guessed ? REOPEN_GIVE_UP : REOPEN_GUESS;
}

// The device this session is for, looked up without destroying what the caller
// already has.
//
// bluetooth_receiver_device() empties its output buffers first thing, so passing
// `mac` straight to it and ignoring a false answer blanks the address, and every
// open after that becomes "bluealsa:DEV=,PROFILE=a2dp", which the plugin refuses
// as an invalid address. The momentary gap between one transport going and the
// next arriving -- which is what a codec change is -- is exactly when that false
// answer comes.
static bool still_there(char *mac, size_t mac_size, char *name, size_t name_size) {
	char found_mac[BT_MAC_MAX];
	char found_name[BT_NAME_MAX];
	if (!bluetooth_receiver_device(found_mac, sizeof(found_mac), found_name, sizeof(found_name))) {
		return false;
	}
	snprintf(mac, mac_size, "%s", found_mac);
	snprintf(name, name_size, "%s", found_name);
	return true;
}

// True while this visit to the mode is still the current one.
static bool session_live(unsigned session) {
	pthread_mutex_lock(&lock);
	bool go = running && session == session_id;
	pthread_mutex_unlock(&lock);
	return go;
}

#define SENDER_POLL_MS 200

// Waits while the sender is connected with its stream idle: a computer with
// nothing playing. There is no PCM to open until it starts, and bluez says
// when it does. True when it has started or gone, false when the mode ended.
static bool wait_while_idle(unsigned session) {
	while (session_live(session)) {
		if (bluetooth_receiver_sending(NULL) != 0) {
			return true;
		}
		usleep(SENDER_POLL_MS * 1000);
	}
	return false;
}

// After the sender's PCM has gone: waits for it to come back. True when it is
// there again, false when there is nothing left to wait for.
//
// A codec change is a teardown and a rebuild, and in between bluealsa publishes
// no PCM for the sender at all while the sender itself is connected and playing
// throughout. Treating that gap as "the sender has gone" is what used to end the
// mode -- and once it had ended, only switching the receiver off and on again
// brought the sound back. What separates the two is the device, not the PCM:
// bluez still says the phone or computer is connected.
static bool wait_for_pcm(unsigned session, char *mac, size_t mac_size, char *name, size_t name_size) {
	for (int waited = 0; session_live(session); waited += SENDER_POLL_MS) {
		if (still_there(mac, mac_size, name, name_size)) {
			return true;
		}
		if (!bluetooth_receiver_peer_connected()) {
			return false; // the sender really has gone
		}
		if (waited == 0) {
			fprintf(stderr, "btreceiver: the sender is between transports; waiting for its PCM\n");
		}
		usleep(SENDER_POLL_MS * 1000);
	}
	return false;
}

static void *capture_worker(void *arg) {
	// The number this visit to the mode was given. Anything this thread does to
	// the shared state is conditional on it still being the current one.
	const unsigned session = (unsigned)(uintptr_t)arg;
	pthread_mutex_lock(&capture_owner);
	if (!session_live(session)) {
		pthread_mutex_unlock(&capture_owner);
		return NULL;
	}
	// Not background: this thread IS the playback path while the mode is on, and
	// a capture PCM that is not drained on time overruns, which is heard as
	// stuttering rather than as a delay. SCHED_IDLE yields to anything else on
	// the core, while bluealsa's own decoder runs at FIFO 12 on the other side of
	// the same buffer, so frames decoded while the interface draws land in a ring
	// nobody is emptying ("bluealsa: W: Dropping PCM frames: PCM overrun").
	//
	// 10 is the same policy and priority as the player's own playback thread,
	// because in this mode it is the same job. It is a setting because whether
	// the decoder should outrank the thread that empties after it is a question
	// a listen answers better than an argument does:
	//
	//   [bluetooth]
	//   receiver_rt_priority = 10   below bluealsa's decoder (FIFO 12)
	//                        = 13   above it
	//                        = 0    no real-time at all
	int rt = (int)config_get_int("bluetooth", "receiver_rt_priority", 10);
	if (rt > 0) {
		thread_be_realtime("btreceive", rt);
	} else {
		thread_be_background("btreceive");
	}

	char mac[BT_MAC_MAX];
	char name[BT_NAME_MAX];
	if (!still_there(mac, sizeof(mac), name, sizeof(name)) &&
		!wait_for_pcm(session, mac, sizeof(mac), name, sizeof(name))) {
		if (session_live(session)) {
			set_error(session, tr("btreceiver_nothing_streaming"));
		}
		goto done;
	}

	// One session per configuration, and the next one opens whatever took the
	// place of the last.
	//
	// The test is the open itself, repeated until it works or the window runs
	// out, rather than the device coming back: the phone stays connected right
	// through a codec change, so it answers yes again within a couple of hundred
	// milliseconds while the transport underneath is still being rebuilt and has
	// nothing to open. A device that has actually gone counts the same as a PCM
	// that will not open: both are answered by trying again for a few seconds.
	//
	// Two things do not use up the window. A sender whose stream is idle has
	// nothing to open yet, and is waited for. And a sender still connected when
	// the window runs out keeps the mode on: the error is shown, and it is tried
	// again -- a few seconds apart, for as long as the device is linked. Only a
	// sender that has gone ends the mode.
	//
	// The retry is a timer and not an event on purpose. Waiting for the stream
	// to change was the earlier arrangement, and the change that matters -- a
	// sender that moved from one codec to another and rebuilt its transport --
	// does not always produce one this side can see: bluealsa publishes no PCM
	// while it is between the two, so there is often no transport state to move
	// at all. A phone left in that state stayed silent until the mode was
	// switched off and on again, which is exactly what the report described.
	bool blind = false;
	bool guessed = false;
	bool logged = false;
	uint32_t blind_since = 0;
	int silent_runs = 0;
	for (;;) {
		reopen_t what = reopen_decision(blind, blind ? now_ms() - blind_since : 0, guessed);
		if (what == REOPEN_GIVE_UP) {
			if (!bluetooth_receiver_peer_connected()) {
				set_error(session, tr("btreceiver_stream_failed"));
				fprintf(stderr, "btreceiver: nothing to open in %d ms and the sender has gone; the mode is over\n",
						REOPEN_WAIT_MS);
				break;
			}
			if (!logged) {
				// Said once per episode: this loop is a few seconds apart and
				// every turn of it redraws the page otherwise.
				fprintf(stderr, "btreceiver: nothing to open in %d ms; %s is still linked, so trying again\n",
						REOPEN_WAIT_MS, mac);
				set_error(session, tr("btreceiver_stream_failed"));
				logged = true;
			}
			blind = false;
			guessed = false;
			usleep(REOPEN_POLL_MS * 1000);
			continue;
		}
		guessed = guessed || what == REOPEN_GUESS;

		// The first-frame watchdog is armed until it has been wrong three times.
		// Past that the silence is the sender's, not a stale handle's, and pulling
		// the PCM every three seconds would only keep it silent.
		bool heard = false;
		bool watchdog = silent_runs < SILENT_RUNS_MAX;
		session_t result = capture_session(mac, name, what == REOPEN_GUESS, watchdog, session, &heard);
		if (heard) {
			silent_runs = 0;
			logged = false;
		} else if (watchdog && result != SESSION_NO_PCM) {
			silent_runs++;
			if (silent_runs >= SILENT_RUNS_MAX) {
				fprintf(stderr, "btreceiver: %d sessions in a row with no audio; leaving the next one alone\n",
						SILENT_RUNS_MAX);
				// Three PCMs opened and none of them delivered while the
				// sender said it was playing: this is not a handle on the
				// wrong transport, it is a sender whose stream died with the
				// link and never came back. Reopening will keep finding the
				// same dead object -- the log from the report has fourteen of
				// these in a row, ending with the mode switched off by hand.
				// Ask the sender for a stream instead; that is what the one
				// recovery in that log did.
				bluetooth_receiver_ask_to_stream();
			}
		}
		if (result == SESSION_OVER) {
			break;
		}

		pthread_mutex_lock(&lock);
		bool go = running && session == session_id;
		if (go) {
			last_frame_ms = 0;
			touch();
		}
		pthread_mutex_unlock(&lock);
		if (!go) {
			break;
		}

		bool nothing_there =
			result == SESSION_NO_PCM || !still_there(mac, sizeof(mac), name, sizeof(name));
		if (!nothing_there) {
			blind = false;
			guessed = false;
			logged = false;
		} else if (bluetooth_receiver_sending(NULL) == 0) {
			fprintf(stderr, "btreceiver: %s is connected and sending nothing; waiting for it to start\n", mac);
			if (!wait_while_idle(session)) {
				break;
			}
			fprintf(stderr, "btreceiver: %s has started sending\n", mac);
			blind = false;
			guessed = false;
			continue;
		} else if (!blind) {
			blind = true;
			blind_since = now_ms();
		}

		// Also between two good sessions: a stream that stops and restarts in a
		// tight loop would otherwise spin this thread.
		usleep(REOPEN_POLL_MS * 1000);
	}

done:
	pthread_mutex_lock(&lock);
	// Only the thread that is still the current session speaks for the mode. An
	// older one finishing must not take down the session that replaced it.
	if (session == session_id) {
		last_frame_ms = 0;
		// A loop that ended on its own -- the sender went away, the stream would
		// not open -- takes the mode down with it, so the page does not show a
		// receiver that is not receiving.
		state.active = false;
		running = false;
		touch();
	}
	pthread_mutex_unlock(&lock);
	fprintf(stderr, "btreceiver: the capture thread has finished\n");
	pthread_mutex_unlock(&capture_owner);
	return NULL;
}

bool btreceiver_start(void) {
	pthread_mutex_lock(&lock);
	// A session that is up, rather than a thread that exists: a thread still
	// closing its handles from the previous visit is not a running receiver,
	// and treating it as one is what left the mode dead on a quick re-entry.
	if (state.active && running) {
		pthread_mutex_unlock(&lock);
		return true;
	}
	pthread_mutex_unlock(&lock);

	if (!btreceiver_available()) {
		return false;
	}

	// One PCM takes one writer, and in this mode the player is not playing
	// anything of its own.
	audio_stop();

	pthread_mutex_lock(&lock);
	memset(&state, 0, sizeof(state));
	state.active = true;
	running = true;
	unsigned mine = ++session_id;
	touch();
	pthread_mutex_unlock(&lock);

	if (pthread_create(&thread, NULL, capture_worker, (void *)(uintptr_t)mine) != 0) {
		pthread_mutex_lock(&lock);
		state.active = false;
		running = false;
		snprintf(state.error, sizeof(state.error), "%s", tr("btreceiver_not_enough_memory"));
		touch();
		pthread_mutex_unlock(&lock);
		return false;
	}
	pthread_detach(thread);
	return true;
}

void btreceiver_stop(void) {
	pthread_mutex_lock(&lock);
	// Whether a session was actually live, and not merely a thread still
	// winding down: leaving the page stops the mode twice -- once from the
	// confirmation's own handler, once from the page unloading a turn later --
	// and the second call used to send the sender another Pause and print
	// another "receiver mode off" a millisecond after the first.
	bool was = state.active;
	running = false;
	state.active = false;
	// Nothing of the sender survives the mode: what is left here is read by the
	// page the next time it opens, and would name a phone that has gone.
	state.device[0] = '\0';
	state.codec[0] = '\0';
	state.sample_rate = 0;
	state.channels = 0;
	state.bits = 0;
	state.error[0] = '\0';
	touch();
	pthread_mutex_unlock(&lock);

	if (!was) {
		return;
	}

	// The sender is told to stop. Leaving the mode takes its output away, and a
	// phone that carries on playing into nothing is one the user has to go and
	// pause by hand -- having just been listening to it on this device.
	bluetooth_receiver_command("Pause");

	// The thread is detached and closes the output itself; waiting on it here
	// would hold the interface for as long as one read takes. What matters is
	// that `running` is already false, so it cannot write another frame after
	// the caller has moved on.
	fprintf(stderr, "btreceiver: receiver mode off\n");
}

bool btreceiver_key(btreceiver_key_t key) {
	if (!btreceiver_is_active()) {
		return false;
	}

	switch (key) {
	case BTRECEIVER_NEXT:
		bluetooth_receiver_command("Next");
		break;
	case BTRECEIVER_PREV:
		bluetooth_receiver_command("Previous");
		break;
	case BTRECEIVER_PLAY_PAUSE:
	default: {
		// One key for two commands, and what is playing is on the other
		// machine, so the other machine is asked. Its AVRCP status is what
		// bluez publishes and keeps current.
		//
		// The sender's AVRCP status is the only reliable answer. The stream
		// itself is not one: a sender that pauses simply stops sending, and a
		// PCM that is not being fed does not fail -- snd_pcm_readi waits -- so
		// nothing would ever move the flag back.
		bool playing = bluetooth_receiver_playing();
		bluetooth_receiver_command(playing ? "Pause" : "Play");
		// Said now rather than waited for: the command is queued on the worker
		// and the confirming signal is a round trip behind it, and a second
		// press inside that window would otherwise read the old answer.
		bluetooth_receiver_note_playing(!playing);
		break;
	}
	}
	return true;
}
