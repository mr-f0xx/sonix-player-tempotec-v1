#ifndef TAPERECORDER_H
#define TAPERECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAPE_REC_RATE 48000
#define TAPE_MEMO_NAME_SIZE 29 // Memo-YYYYMMDD-HHMMSS-NNN.wav + NUL

typedef struct {
	int card, device;
	bool usb;
} tape_input_t;

typedef enum {
	TAPE_REC_IDLE,
	TAPE_REC_OPENING,
	TAPE_REC_MONITORING,
	TAPE_REC_RECORDING,
	TAPE_REC_SAVING,
	TAPE_REC_SAVED,
	TAPE_REC_INTERRUPTED, // input stopped unexpectedly; a partial memo was saved
	TAPE_REC_NO_INPUT,
	TAPE_REC_NO_CARD,
	TAPE_REC_BUSY,
	TAPE_REC_INPUT_ERROR,
	TAPE_REC_DISK_ERROR,
	TAPE_REC_START_ERROR,
} tape_rec_state_t;

typedef struct {
	tape_rec_state_t state;
	bool record; // distinguishes opening a memo from opening just the meter
	uint32_t frames;
	unsigned peak; // 0..32768 (full scale)
	bool clipped;
} tape_rec_status_t;

// Read-only enumeration; does not open a PCM. USB capture takes precedence
// over onboard capture. A playback PCM, headset keys and /dev/uac_sa don't count.
bool taperecorder_find_input(tape_input_t *out);

// Starts on a worker, or replaces a meter-only session with a new recording.
// OPENING means the request was accepted; other results are immediate errors.
tape_rec_state_t taperecorder_start(const tape_input_t *input, bool record);
void taperecorder_request_stop(void);
void taperecorder_finish(void); // stop and join before the card is unmounted
void taperecorder_status(tape_rec_status_t *out);
bool taperecorder_active(void);

// Recent memos (newest first), 0 for an empty folder, -1 for unavailable storage.
int taperecorder_list(char names[][TAPE_MEMO_NAME_SIZE], int capacity);
bool taperecorder_path(const char *name, char *out, size_t size);

#endif /* TAPERECORDER_H */
