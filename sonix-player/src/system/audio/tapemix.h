#ifndef TAPEMIX_H
#define TAPEMIX_H

#include "tapestore.h"

typedef enum {
	TAPE_MIX_IDLE,
	TAPE_MIX_OPENING,
	TAPE_MIX_RECORDING,
	TAPE_MIX_SAVING,
	TAPE_MIX_SAVED,
	TAPE_MIX_PARTIAL,
	TAPE_MIX_ERROR,
	TAPE_MIX_NO_CARD, // immediate action result only
	TAPE_MIX_BUSY,    // immediate action result only
} tape_mix_state_t;

typedef struct {
	tape_mix_state_t state;
	uint32_t frames;
	char name[TAPE_SAVE_NAME_SIZE]; // after publication
} tape_mix_info_t;

void tapemix_info(tape_mix_info_t *out);
bool tapemix_active(void);
void tapemix_stop(void); // save what was captured; does not stop playback

// Sampler internals. Call start/finish with the sampler's worker lock held.
// On OPENING the writer owns the reserved card fd; otherwise the caller does.
tape_mix_state_t tapemix_start(int card);
void tapemix_finish(void);
bool tapemix_accepting(void);
void tapemix_interrupt(void); // preserve a continuous prefix if playback failed
void tapemix_push(const int16_t *stereo, unsigned frames);

#endif
