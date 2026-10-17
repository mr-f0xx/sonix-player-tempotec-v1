#ifndef SPOTIFYSINK_H
#define SPOTIFYSINK_H

#include <stdbool.h>

// The player's end of the receiver's audio: a FIFO, a ring, the DAC.
//
// librespot's `pipe` backend writes the decoded stream to a file, and a FIFO is
// one -- so the receiver writes S16LE 44100 Hz stereo into it and this reads it
// out, exactly as the AirPlay receiver's socket is read (src/system/remote/
// airplay.c). Same shape, same reasons:
//
//   * There is no ALSA device to open for the receiver on this firmware: no
//     aplay, no alsa-utils on the V1, and a second process opening the card
//     would fight the player for it anyway.
//   * The player owns the DAC throughout, so its volume, its headphone-jack
//     routing and its mute-on-route-change apply to Spotify for free.
//   * The ACK the receiver waits for is the FIFO filling up: it blocks in
//     write() when this stops reading, so a Wi-Fi hiccup back-pressures
//     librespot instead of being papered over with silence.
//
// One thread does nothing but drain the FIFO into a ring; another feeds the
// DAC from it. The ring's size is the slack in front of the card, and its
// fill/empty conditions are the whole pacing mechanism.

// Creates the FIFO, opens it and starts both threads. `pause_receiver` is
// called when local playback asks for the DAC back and the receiver has to be
// told to stop playing into it (spotify.c passes the socket command); it runs
// on the caller's thread and must return quickly. False when the FIFO or the
// threads could not be set up, in which case the receiver must not be started.
bool spotifysink_start(const char *fifo_path, void (*pause_receiver)(void));

// Stops both threads, unlinks the FIFO and gives the DAC back. Safe to call
// when it was never started.
void spotifysink_stop(void);

// True while bytes have arrived in the last second: what makes Spotify count as
// playing for the power layer and for the player's own "is anything on" logic.
bool spotifysink_playing(void);

// Milliseconds since the last bytes arrived, or -1 if none ever has.
int spotifysink_silent_ms(void);

// Whether the DAC is currently held open for the receiver.
bool spotifysink_holds_output(void);

// The audio_set_external_release_cb() the player calls when something local
// wants the DAC: pauses the receiver, lets the ring drain and closes the PCM.
// True means the device is free, or about to be -- the hand-back happens on the
// playback thread within a poll, and the requester retries for two seconds.
bool spotifysink_release_output(void);

#endif /* SPOTIFYSINK_H */
