#ifndef TAPEPADS_H
#define TAPEPADS_H

#include "tapewav.h"

#define TAPE_PAD_COUNT 16
#define TAPE_PAD_STEPS 16
#define TAPE_PAD_RATE 44100
#define TAPE_PAD_MIN_BPM 60
#define TAPE_PAD_MAX_BPM 200

typedef enum {
	TAPE_PADS_IDLE,
	TAPE_PADS_OPENING,
	TAPE_PADS_PLAYING,
	TAPE_PADS_UNAVAILABLE,
	TAPE_PADS_SAMPLE_ERROR,
} tape_pads_state_t;

typedef enum { TAPE_PAD_FACTORY, TAPE_PAD_WAV, TAPE_PAD_EMPTY } tape_pad_kind_t;

typedef struct {
	tape_pad_kind_t kind;
	int preset; // 0–15 for a factory drum (four variants per instrument)
	bool long_clip;
	char name[TAPE_FILE_NAME_SIZE]; // basename, only for a WAV
} tape_pad_info_t;

// A tap plays immediately on the next mix block, without starting the loop.
// One voice per pad; one long clip at once (re-tap the pad to cut it).
bool tapepads_hit(int pad);
bool tapepads_start(void); // run the one-bar sixteenth-note loop
void tapepads_stop(void); // request stop of loop and all voices
void tapepads_finish(void); // join before shutdown
void tapepads_release_card(void); // stop card-backed voices and forget that card's pads
bool tapepads_active(void);
tape_pads_state_t tapepads_state(void);
unsigned tapepads_sounding(void);
bool tapepads_looping(void);
bool tapepads_overdub(void);
void tapepads_set_overdub(bool enabled);
int tapepads_step(void); // current loop step, or -1 before it is running
void tapepads_pattern(bool demo); // replace the loop with the demo, or clear it
int tapepads_bpm(void);
void tapepads_set_bpm(int bpm);

// Assignments are session-local. These operations stop performance and make
// a new mapping available only after it is valid; they never copy whole WAVs.
unsigned tapepads_revision(void);
void tapepads_info(tape_pad_info_t out[TAPE_PAD_COUNT]);
void tapepads_factory(void);
void tapepads_set_preset(int pad, int preset); // -1 clears the pad
bool tapepads_assign(int pad, const char *path); // card-relative WAV
int tapepads_load_kit(const char *name); // single folder under TAPE_KIT_DIR, positive count on success

#endif
