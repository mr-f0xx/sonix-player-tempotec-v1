#ifndef GEARBOY_H
#define GEARBOY_H

#include <stdbool.h>
#include <stdint.h>

// The running emulator: a thread producing frames, a framebuffer to look at,
// eight buttons to press.
//
// Pacing comes from the audio, not from a clock. Each emulated frame produces
// 738 samples per channel, and writing them to an ALSA PCM opened with a short
// buffer blocks until the card has consumed them, so the rate is the DAC
// crystal's -- exactly the Game Boy's 59.73 frames per second -- and video and
// sound cannot drift apart because one loop produces both. A computed usleep
// in place of that would slip a frame every few seconds.
//
// The framebuffer is scaled from the core's 160x144: three times on the
// 480-wide HiBy panels, and to 240x216 on the TempoTec V1 so the picture fills
// its narrower display without distorting its aspect ratio. The V1 fit uses
// nearest-neighbour sampling; the integer 3x path remains pixel-perfect.

#define GEARBOY_SCALE_MAX 3

#define GEARBOY_TITLE_MAX 128

// Starts a game. `rom_path` is the file on the card, `title` the name to show
// (usually the database's). False when the ROM does not load or memory runs
// out.
//
// Stops music playback: a PCM has one writer, and a game over music is not
// what anyone wants anyway.
bool gearboy_start(const char *rom_path, const char *title);

// Stops the game and saves battery-backed RAM. Returns once the thread has
// finished, the .sav is on the card and the buffers are freed -- half a
// megabyte, not worth leaving around on 64 MB for a page nobody is looking at.
//
// Anyone holding the framebuffer (gearboy_screen()) must drop it before
// calling this.
void gearboy_stop(void);

bool gearboy_running(void);

// The game did not start: the ROM does not load, or memory ran out. False
// until that is known, since the real startup happens on the thread and
// gearboy_start() can return true with the game failing an instant later.
// Without this the page would sit on a black rectangle that explains nothing.
bool gearboy_failed(void);

// The running game's title, for the page.
const char *gearboy_title(void);

// The framebuffer, gearboy_frame_w() x gearboy_frame_h() RGB565 (480x432 on
// the HiBy players, 240x216 on the V1). NULL until a game is running.
//
// Only the UI thread writes it, inside gearboy_present(); the emulator thread
// never touches it. That single-writer rule is what keeps a mid-screen seam
// out of the picture.
const uint16_t *gearboy_screen(void);

// ---------------------------------------------------------------------------
// How much the picture is scaled up for the panel in use
// ---------------------------------------------------------------------------
//
// Three times gives 480x432, the full width of the HiBy panels. The V1 uses a
// custom 240x216 nearest-neighbour fit: a deliberate trade-off that makes the
// picture larger while preserving its aspect ratio. `gearboy_scale()` returns
// zero when this custom size is active.
//
// Set the scale or custom frame size before the first gearboy_start(): the
// framebuffer is sized from it and the page sizes its image from the same
// getters. The default is 3, so a caller that never asks gets the HiBy picture.
void gearboy_set_scale(int scale);
void gearboy_set_frame_size(int width, int height);
int gearboy_scale(void);

// The framebuffer's output size, also the size of the image the page draws.
int gearboy_frame_w(void);
int gearboy_frame_h(void);

// Takes the core's last finished frame and scales it into the framebuffer
// above. Call from the UI thread, immediately before invalidating: that is
// what makes a mid-screen seam impossible.
//
// False when there is nothing to show yet.
bool gearboy_present(void);

// Frames produced so far. The UI watches it to redraw only when there is
// something new instead of on every tick.
uint32_t gearboy_frame_count(void);

// All touch/fallback button states at once, as a mask of GB_KEY_*
// (src/gb/gbcore.h). Written by the touch reader, read by the emulator thread.
void gearboy_set_keys(uint16_t mask);

// Extra momentary buttons from outside the touch reader (the device's physical
// Play/Pause and skip keys), ORed with the main key mask by the emulator thread.
void gearboy_set_key_overlay(uint16_t mask);

// ---------------------------------------------------------------------------
// Pausing, and what can be done while paused
// ---------------------------------------------------------------------------

// Stops emulation without tearing anything down: the thread stays alive, the
// core stays in memory, the ROM stays loaded. Used by the in-game menu, so
// that a tap in the middle of the screen freezes the picture instead of
// letting the game run on behind a menu.
//
// Audio is not closed: the PCM stays open and is fed silence, one frame at a
// time. Closing it would mean reopening on resume, and reopening a PCM here
// takes half a second; keeping it open and fed is also what paces the loop,
// which without the audio metronome would have to fall back to usleep.
void gearboy_set_paused(bool paused);
bool gearboy_paused(void);

// Saves or reloads machine state. Neither call does the work: they post a
// request that the emulator thread services between frames. The core is not
// shareable, and reading its state from here while it runs can produce a
// corrupt file.
//
// Call these with emulation paused: that is where the thread looks at them.
void gearboy_request_savestate(void);
void gearboy_request_loadstate(void);

// The outcome of the last request, once: reading it consumes it.
//   0  nothing new
//  +1  saved    +2  loaded
//  -1  save failed    -2  load failed
// The page polls it each tick and turns it into a notice.
int gearboy_take_state_result(void);

// Rereads palette, shader and colour correction from config and applies them,
// including mid-game: the core thread picks up palette and correction between
// frames, and the scaler uses the shader on the next frame. Boot ROMs only
// take effect from the next game.
void gearboy_refresh_video_settings(void);

#endif /* GEARBOY_H */
