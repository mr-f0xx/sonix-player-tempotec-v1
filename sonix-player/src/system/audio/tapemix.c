#include "tapemix.h"
#include "tapepads.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#ifndef TAPE_MIX_LIMIT_SECONDS
#define TAPE_MIX_LIMIT_SECONDS 3600u
#endif
#define MAX_FRAMES (TAPE_PAD_RATE * TAPE_MIX_LIMIT_SECONDS)
#define QUEUE_FRAMES TAPE_PAD_RATE // one second, ~173 KiB, only used while a take runs
#define BATCH_FRAMES 2048u
#define FRAME_BYTES (2 * sizeof(int16_t))

// The PCM worker only copies into this bounded queue. Opening, writing and
// flushing the card never happen on it, or while the queue lock is held.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int16_t ring[QUEUE_FRAMES * 2];
static unsigned first, used;
static atomic_bool accepting;
static bool interrupted, joinable;
static tape_mix_info_t status;
static int card_fd;
static pthread_t worker;

static bool active(tape_mix_state_t state) {
	return state == TAPE_MIX_OPENING || state == TAPE_MIX_RECORDING || state == TAPE_MIX_SAVING;
}

void tapemix_info(tape_mix_info_t *out) {
	pthread_mutex_lock(&lock);
	*out = status;
	pthread_mutex_unlock(&lock);
}

bool tapemix_active(void) {
	tape_mix_info_t info;
	tapemix_info(&info);
	return active(info.state);
}

bool tapemix_accepting(void) { return accepting; }

static void stop_locked(void) {
	if (!accepting) return;
	accepting = false;
	status.state = TAPE_MIX_SAVING;
	pthread_cond_signal(&ready);
}

void tapemix_stop(void) {
	pthread_mutex_lock(&lock);
	stop_locked();
	pthread_mutex_unlock(&lock);
}

void tapemix_interrupt(void) {
	pthread_mutex_lock(&lock);
	if (accepting) {
		interrupted = true;
		stop_locked();
	}
	pthread_mutex_unlock(&lock);
}

void tapemix_push(const int16_t *stereo, unsigned frames) {
	if (!accepting || !frames) return;
	pthread_mutex_lock(&lock);
	if (!accepting) goto done;
	if (frames > MAX_FRAMES - status.frames) frames = MAX_FRAMES - status.frames;
	if (frames > QUEUE_FRAMES - used) {
		// Never silently leave a hole in a take. Retain the continuous prefix
		// and tell the user that the card couldn't keep up.
		interrupted = true;
		stop_locked();
		goto done;
	}
	unsigned at = (first + used) % QUEUE_FRAMES;
	unsigned contiguous = QUEUE_FRAMES - at;
	if (contiguous > frames) contiguous = frames;
	memcpy(ring + at * 2, stereo, (size_t)contiguous * FRAME_BYTES);
	memcpy(ring, stereo + contiguous * 2, (size_t)(frames - contiguous) * FRAME_BYTES);
	used += frames;
	status.frames += frames;
	if (status.frames == MAX_FRAMES) stop_locked();
	else if (used >= BATCH_FRAMES) pthread_cond_signal(&ready);
done:
	pthread_mutex_unlock(&lock);
}

static void *write_take(void *unused) {
	(void)unused;
	tape_savefile_t file;
	bool opened = tape_store_begin(card_fd, TAPE_STORE_TAKE, &file);
	close(card_fd);
	bool ok = opened && tape_store_wav_header(file.fd, TAPE_PAD_RATE, 0);
	uint32_t written = 0;
	int16_t pcm[BATCH_FRAMES * 2];
	pthread_mutex_lock(&lock);
	if (ok && accepting) status.state = TAPE_MIX_RECORDING;
	while (ok) {
		while (accepting && used < BATCH_FRAMES) pthread_cond_wait(&ready, &lock);
		if (!used) break;
		unsigned frames = used < BATCH_FRAMES ? used : BATCH_FRAMES;
		unsigned contiguous = QUEUE_FRAMES - first;
		if (contiguous > frames) contiguous = frames;
		memcpy(pcm, ring + first * 2, (size_t)contiguous * FRAME_BYTES);
		memcpy(pcm + contiguous * 2, ring, (size_t)(frames - contiguous) * FRAME_BYTES);
		first = (first + frames) % QUEUE_FRAMES;
		used -= frames;
		pthread_mutex_unlock(&lock);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		for (unsigned i = 0; i < frames * 2; i++) {
			unsigned v = (uint16_t)pcm[i];
			pcm[i] = (int16_t)(v >> 8 | v << 8);
		}
#endif
		ok = tape_store_write(file.fd, pcm, (size_t)frames * FRAME_BYTES);
		if (ok) written += frames;
		pthread_mutex_lock(&lock);
	}
	accepting = false;
	status.state = TAPE_MIX_SAVING;
	bool partial = interrupted;
	pthread_mutex_unlock(&lock);

	bool saved = false;
	if (opened) {
		bool keep = ok && written && tape_store_wav_header(file.fd, TAPE_PAD_RATE, written);
		saved = tape_store_end(&file, keep);
	}
	pthread_mutex_lock(&lock);
	status = (tape_mix_info_t){.state = saved ? (partial ? TAPE_MIX_PARTIAL : TAPE_MIX_SAVED) :
		ok && !written && !partial ? TAPE_MIX_IDLE : TAPE_MIX_ERROR, .frames = saved ? written : 0};
	if (saved) strcpy(status.name, file.name);
	pthread_mutex_unlock(&lock);
	return NULL;
}

void tapemix_finish(void) {
	if (!joinable) return;
	tapemix_stop();
	pthread_join(worker, NULL);
	joinable = false;
}

tape_mix_state_t tapemix_start(int card) {
	if (tapemix_active()) return TAPE_MIX_BUSY;
	tapemix_finish(); // reap a completed writer before reusing its mailbox
	pthread_mutex_lock(&lock);
	first = used = 0;
	interrupted = false;
	accepting = true;
	card_fd = card;
	status = (tape_mix_info_t){.state = TAPE_MIX_OPENING};
	if (pthread_create(&worker, NULL, write_take, NULL) == 0) joinable = true;
	else {
		accepting = false;
		status.state = TAPE_MIX_ERROR;
	}
	tape_mix_state_t result = status.state;
	pthread_mutex_unlock(&lock);
	return result;
}
