// Run: gcc -DGB_CORE=1 -I. -pthread tests/test_gearboy_audio.c src/system/gearboy/gearboy.c -o /tmp/test_gearboy_audio && /tmp/test_gearboy_audio
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "src/gb/gbcore.h"
#include "src/system/audio/audio.h"
#include "src/system/gearboy/gearboy.h"

// Exercise the real worker against a fast fake core and fake audio device.
static int muted_config, status, external, begins, ends, pauses, freezes, resumes, frames, fail_open, fail_rom, fail_freeze, restarts;
#define GET(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define SET(x,v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define INC(x) __atomic_add_fetch(&(x), 1, __ATOMIC_SEQ_CST)
long config_get_int(const char *s, const char *k, long fallback) { (void)s; return !strcmp(k, "mute") ? GET(muted_config) : fallback; }
const char *gbdb_bios_dir(void) { return ""; }
const char *gbdb_saves_dir(void) { return ""; }
audio_status_t audio_get_status(void) { return GET(status); }
void audio_pause(void) { INC(pauses); SET(status, AUDIO_STATUS_PAUSED); }
void audio_resume(void) { INC(resumes); SET(status, AUDIO_STATUS_PLAYING); }
bool audio_suspend_freeze(int timeout) { (void)timeout; INC(freezes); return !GET(fail_freeze); }
void audio_get_current_file(char *out, size_t size) { snprintf(out, size, "song.flac"); }
void audio_get_progress(double *current, double *total) { *current = 12; *total = 60; }
int audio_play_at(const char *path, double pos) { assert(!strcmp(path, "song.flac") && pos == 12); SET(status, AUDIO_STATUS_PLAYING); INC(restarts); return 0; }
int audio_play_paused(const char *path, double pos) { assert(!strcmp(path, "song.flac") && pos == 12); SET(status, AUDIO_STATUS_PAUSED); INC(restarts); return 0; }
bool audio_external_is_active(void) { return GET(external); }
bool audio_external_begin_exclusive_latency(int rate, int ch, int bits, int ms) {
    assert(rate == 44100 && ch == 2 && bits == 16 && ms == 46); INC(begins);
    if (GET(fail_open) || GET(external)) return false;
    SET(external, 1); return true;
}
void audio_external_end(void) { assert(GET(external)); INC(ends); SET(external, 0); }
int audio_external_write(const void *data, int count) { (void)data; usleep(16000); return count; }
struct gb_core { int unused; };
static struct gb_core core;
gb_core_t *gb_core_create(void) { return &core; }
void gb_core_destroy(gb_core_t *c) { (void)c; }
bool gb_core_load_file(gb_core_t *c, const char *p, bool d) { (void)c; (void)p; (void)d; return !GET(fail_rom); }
const char *gb_core_rom_name(gb_core_t *c) { (void)c; return "test"; }
bool gb_core_is_color(gb_core_t *c) { (void)c; return false; }
void gb_core_load_ram(gb_core_t *c, const char *p) { (void)c; (void)p; }
void gb_core_save_ram(gb_core_t *c, const char *p) { (void)c; (void)p; }
bool gb_core_save_state(gb_core_t *c, const char *p, int n) { (void)c; (void)p; (void)n; return true; }
bool gb_core_load_state(gb_core_t *c, const char *p, int n) { (void)c; (void)p; (void)n; return true; }
void gb_core_set_keys(gb_core_t *c, uint16_t mask) { (void)c; (void)mask; }
void gb_core_run_frame(gb_core_t *c, uint16_t *pixels, int16_t *audio, int *samples) {
    (void)c; (void)pixels; (void)audio; *samples = 1476; INC(frames);
}
void gb_core_set_sample_rate(gb_core_t *c, int rate) { (void)c; (void)rate; }
void gb_core_set_dmg_palette(gb_core_t *c, const uint8_t colors[4][3]) { (void)c; (void)colors; }
void gb_core_set_color_correction(gb_core_t *c, bool enabled) { (void)c; (void)enabled; }
bool gb_core_set_bootroms(gb_core_t *c, const char *a, const char *b) { (void)c; (void)a; (void)b; return true; }
static void wait_at_least(int *value, int target) {
    for (int i = 0; i < 200 && __atomic_load_n(value, __ATOMIC_SEQ_CST) < target; i++) usleep(10000);
    assert(__atomic_load_n(value, __ATOMIC_SEQ_CST) >= target);
}
static void reset(void) {
    SET(muted_config, 0); SET(status, AUDIO_STATUS_PLAYING); SET(external, 0);
    SET(begins, 0); SET(ends, 0); SET(pauses, 0); SET(freezes, 0); SET(resumes, 0);
    SET(frames, 0); SET(fail_open, 0); SET(fail_rom, 0); SET(fail_freeze, 0); SET(restarts, 0);
}
int main(void) {
    // Muted at start leaves music untouched; independent clock runs near 60 Hz.
    reset(); SET(muted_config, 1); assert(gearboy_start("test.gb", "test"));
    usleep(250000); assert(GET(frames) >= 10 && GET(frames) <= 18);
    assert(!GET(begins) && !GET(pauses) && !GET(freezes));
    gearboy_set_muted(false); wait_at_least(&begins, 1);
    assert(GET(pauses) == 1 && GET(freezes) == 1);
    gearboy_set_paused(true); gearboy_set_muted(true); wait_at_least(&resumes, 1);
    assert(GET(ends) == 1 && !GET(external));
    gearboy_request_savestate(); int result = 0;
    for (int i = 0; i < 100 && !result; i++) { usleep(10000); result = gearboy_take_state_result(); }
    assert(result == 1); gearboy_stop(); assert(GET(resumes) == 1 && GET(ends) == 1);
    // Unmuted start pauses music and stop restores it exactly once.
    reset(); assert(gearboy_start("test.gb", "test")); wait_at_least(&begins, 1);
    gearboy_stop(); assert(GET(ends) == 1 && GET(resumes) == 1);
    // An already paused track stays paused.
    reset(); SET(status, AUDIO_STATUS_PAUSED); assert(gearboy_start("test.gb", "test"));
    wait_at_least(&begins, 1); gearboy_set_muted(true); wait_at_least(&ends, 1); gearboy_stop();
    assert(!GET(pauses) && !GET(resumes));
    // Do not take or close another external source.
    reset(); SET(external, 1); assert(gearboy_start("test.gb", "test")); wait_at_least(&frames, 3);
    gearboy_stop(); assert(GET(external) && !GET(begins) && !GET(ends) && !GET(freezes));
    // A failed open restores playing music; an invalid ROM leaves it alone.
    reset(); SET(fail_open, 1); assert(gearboy_start("test.gb", "test")); wait_at_least(&resumes, 1);
    gearboy_stop(); assert(GET(resumes) == 1 && !GET(ends));
    reset(); SET(fail_freeze, 1); assert(gearboy_start("test.gb", "test")); wait_at_least(&restarts, 1);
    gearboy_stop(); assert(GET(status) == AUDIO_STATUS_PLAYING && !GET(begins));
    reset(); SET(status, AUDIO_STATUS_PAUSED); SET(fail_freeze, 1);
    assert(gearboy_start("test.gb", "test")); wait_at_least(&restarts, 1);
    gearboy_stop(); assert(GET(status) == AUDIO_STATUS_PAUSED && !GET(begins));
    reset(); SET(fail_rom, 1); assert(gearboy_start("bad.gb", "test"));
    for (int i = 0; i < 100 && !gearboy_failed(); i++) usleep(10000);
    assert(gearboy_failed()); gearboy_stop(); assert(!GET(pauses) && !GET(freezes) && !GET(begins));
    puts("gearboy audio tests passed"); return 0;
}
