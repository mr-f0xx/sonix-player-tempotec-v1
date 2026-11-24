#ifndef TAPEPADS_H
#define TAPEPADS_H

#include "tapemix.h"
#include "tapewav.h"

#define TAPE_PAD_COUNT 16
#define TAPE_PAD_STEPS 16
#define TAPE_PAD_RATE 44100
#define TAPE_PAD_MIN_BPM 60
#define TAPE_PAD_MAX_BPM 200
#define TAPE_CHOKE_MAX 8
#define TAPE_LEVELS_ROOT 7 // pad 08 is the unison note by default

// Chromatic uses one semitone per pad; spread uses whole tones. Both centre
// unison on pad 08 unless a caller chooses a different root.
typedef enum { TAPE_LEVELS_CHROMATIC = 1, TAPE_LEVELS_SPREAD = 2 } tape_levels_mode_t;

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
	uint32_t frames, rate; // complete source WAV, for the trim editor
	uint32_t trim_start, trim_end; // exclusive source-frame markers
	uint32_t pitch_q16; // Q16 playback-rate multiplier
	int semitone; // display value for a 16-Levels pad
	int choke; // +1..8 cuts the group, -1..8 is cut by it, 0 is off
	tape_interp_t interpolation;
	bool levels, trimmed; // levels marks a WAV mapped by 16 Levels
} tape_pad_info_t;

// A tap plays immediately on the next mix block, without starting the loop.
// One voice per pad; one long clip at once (re-tap the pad to cut it).
bool tapepads_hit(int pad);
bool tapepads_start(void); // run the one-bar sixteenth-note loop
void tapepads_stop_loop(void); // leave manual pads and any recording running
void tapepads_stop(void); // stop the loop and voices, and save any take
void tapepads_finish(void); // join before shutdown
void tapepads_release_card(void); // finalize a take and clear card-backed pads
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

// Leave the running transport alone when flipping effects or ending a take.
typedef enum { TAPE_FX_FILTER = 1, TAPE_FX_ECHO = 2, TAPE_FX_DRIVE = 4 } tape_pad_effect_t;
unsigned tapepads_effects(void);
void tapepads_toggle_effect(tape_pad_effect_t effect);
tape_mix_state_t tapepads_record(void); // start a new WAV, also opens silent PCM if needed

typedef enum {
	TAPE_SEQ_OK, TAPE_SEQ_NO_CARD, TAPE_SEQ_INVALID, TAPE_SEQ_SAMPLE_MISSING, TAPE_SEQ_SAVE_ERROR,
} tape_seq_result_t;
tape_seq_result_t tapepads_save_sequence(void);
tape_seq_result_t tapepads_load_sequence(const char *name); // validate before replacing the beat

// These operations stop performance and make
// a new mapping available only after it is valid; they never copy whole WAVs.
unsigned tapepads_revision(void);
void tapepads_info(tape_pad_info_t out[TAPE_PAD_COUNT]);
void tapepads_factory(void);
void tapepads_set_preset(int pad, int preset); // -1 clears the pad
bool tapepads_assign(int pad, const char *path); // card-relative WAV
int tapepads_load_kit(const char *name); // single folder under TAPE_KIT_DIR, positive count on success

// Trim/slice and 16 Levels. Frame markers are in the WAV's own rate; end is
// exclusive and zero means the end of the file. Every operation validates the
// WAV and stops the old PCM before publishing a new frozen pad map.
void tapepads_trim(int pad, uint32_t *start, uint32_t *end);
bool tapepads_set_trim(int pad, uint32_t start, uint32_t end);
bool tapepads_set_trim_ms(int pad, unsigned start_ms, unsigned end_ms);
bool tapepads_set_pitch(int pad, int semitones); // any non-empty pad, -24..+24
bool tapepads_slice(const char *path, int slices, tape_interp_t interpolation);
bool tapepads_set_levels(const char *path, int root_pad, int semitone_step, tape_interp_t interpolation);
bool tapepads_levels(const char *path, tape_levels_mode_t mode, tape_interp_t interpolation);
bool tapepads_enable_levels(const char *path, int root_pad, tape_interp_t interpolation);
bool tapepads_set_16levels(const char *path, int root_pad, tape_interp_t interpolation);
#define tapepads_16levels(path, root, interpolation) tapepads_set_16levels(path, root, interpolation)
bool tapepads_levels_active(void);
void tapepads_set_interpolation(tape_interp_t interpolation);
tape_interp_t tapepads_interpolation(void);

// A trigger from a positive choke group cuts every sounding pad whose group
// has the same absolute number. Negative groups can be cut but never cut
// others: closed hats use +1, open hats -1. Group 0 leaves a pad alone.
bool tapepads_set_choke_group(int pad, int group);
int tapepads_choke_group(int pad);
bool tapepads_set_choke(int pad, int group); // alias
int tapepads_choke(int pad); // alias

#endif
