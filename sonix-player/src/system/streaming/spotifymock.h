#ifndef SPOTIFYMOCK_H
#define SPOTIFYMOCK_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/spotify.h"

// The demo library: what the Spotify pages show when no daemon answers.
//
// The pages are the point of this module, and they must be viewable while the
// firmware is built on a machine that has no Spotify account and no librespot
// cross-build -- which is every build a contributor makes. So when there is no
// socket and no configured command, the client is answered by this: a fixed
// library, a working queue, transport that changes state, and a clock that
// runs while it "plays". No network, no credentials, no audio (the DAC is left
// alone), and the page carries a DEMO badge so nobody mistakes it for Spotify.
//
// Everything is deterministic: the same build and the same presses give the
// same lists, which is what lets the headless UI tests assert on them.

void spotifymock_init(void);
bool spotifymock_available(void);

// A status snapshot in the daemon's shape. No locking of the caller's state:
// the mock has its own, and the caller copies this under its own lock.
void spotifymock_status(spotify_status_t *out);

// One command line, answering in the daemon's reply shape. `out` receives the
// reply's data lines, newline-separated; false on a command it does not know.
bool spotifymock_command(const char *line, char *out, size_t out_size, bool whole_reply);

// Moves the clock on, and the queue with it when a track ends.
void spotifymock_tick(void);

// True while the demo considers itself playing. The caller's PCM thread opens
// and closes its channel from this, so the interface treats the demo exactly
// as it treats the daemon.
bool spotifymock_playing(void);

#endif /* SPOTIFYMOCK_H */
