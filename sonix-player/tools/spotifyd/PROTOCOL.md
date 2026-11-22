# Checked in as documentation and as the contract the player's client is
# written against (sonix-player/src/system/streaming/spotify.c): the two ends
# are built by different toolchains, so the agreement between them is a file,
# not a header.
#
# spotifyd -- Spotify Connect for Sonix, after SpotUI
# ===================================================
#
# SpotUI for the HiBy R3 Pro II (github.com/noisetta/spotui-hiby-r3proii) splits
# Spotify into a UI and a librespot daemon that talk over a text command socket.
# This is the same daemon for the TempoTec Variations V1, with the same
# library-based browsing (Liked Songs, playlists, search), the same queue
# controls, and one addition: the decoded audio goes out over the same socket
# family as signed 16-bit PCM instead of being played by the daemon, so the
# player's own DAC path, output routing, equaliser and volume stay in charge.
#
# Build: see docs/spotify.md. It is a Rust program built outside the firmware's
# Makefile, exactly as librespot itself is, and deployed next to the player:
#
#   /usr/bin/spotifyd                  the daemon
#   /usr/data/librespot-cache/         credentials, written by:
#     librespot --cache /usr/data/librespot-cache \
#               --cache-tool login --username <you>
#   /usr/data/spotifyd.conf            optional key=value settings
#   /etc/init.d/S99spotifyd            optional, start it at boot
#
# spotifyd.conf (all optional):
#
#   socket=0.0.0.0:6600                    what the player connects to: a
#                                          host:port for TCP, or a path for a
#                                          UNIX socket on an on-device rig
#   cache=/usr/data/librespot-cache        librespot credentials and volume
#   device_name=Sonix Player               what the phone's Spotify app shows
#   bitrate=320                            librespot's 96/160/320
#   sink=pipe                              pipe is the default below
#   autoplay=1                             1 (default): playing from the
#                                          phone is heard on the player
#
# Transport
# ---------
# A TCP socket at `socket` when it looks like host:port (the default,
# 0.0.0.0:6600 -- the daemon is on a computer, the player is on the Wi-Fi), or
# a UNIX SOCK_STREAM socket when it is a path. The listener side is the same
# either way, and the player picks its side by the same rule.
#
# Several clients may be connected at once; each connection is a
# request/response stream:
#
#   client -> one line,   COMMAND [arg]...    (space-separated, \n-terminated)
#   daemon -> zero or more data lines         (tab-separated fields)
#             END                            the command finished
#             ERR <reason>                   it did not
#
# A command with nothing to say answers "END" alone. Unknown commands answer
# "ERR unknown command <name>". Lines are UTF-8 and are never longer than 4096
# bytes; a field never contains a tab or a newline.
#
# What the player's parser actually accepts, exactly:
#
#   * the command word ends at the first space OR tab, so a daemon may use
#     either after it; the player sends spaces;
#   * STATE is key=value pairs separated by spaces or tabs, in any order;
#   * TRACK, T, P, Q and AUTH fields are separated by TABS only, because a
#     cover URL or an account name may be empty and a space-splitter would
#     fold an empty field into its neighbour;
#   * the player stops reading a reply after 128 lines or 24576 bytes,
#     whichever comes first, so a long list is asked for in pages instead;
#   * a reply that does not arrive within the timeout is a failure, not a
#     retry: 4 s for a command, 15 s for a list, 2 s to connect.
#
# Status
# ------
#   STATUS
#     STATE state=<0..5> pos=<ms> dur=<ms> shuffle=<0|1> repeat=<0|1|2>
#           volume=<0..100> index=<1..N> count=<N> account=<name>
#     TRACK\t<id>\t<title>\t<artist>\t<album>\t<cover_url>\t<duration_ms>
#     END
#
#   state: 0 unavailable (no session), 1 ready (logged in, nothing loaded),
#          2 loading, 3 playing, 4 paused, 5 stopped.
#   TRACK is sent only while something is loaded (2..5). <id> is the track's
#   base62 id, or "local:<hash>" for a local file. <cover_url> is a Spotify
#   image URL or empty -- the field is still there, holding nothing, because
#   it sits in the middle of the line. Missing STATE keys are simply absent;
#   the player must not depend on the order of the pairs, and ignores unknown
#   keys. `dur` doubles as the count of the queue the track came from.
#
#   The player polls STATUS about twice a second while a page is open, and
#   bumps its redraw serial only when something a page draws has changed:
#   position alone does not, and is read from the same status the poll left.
#
# Browsing
# --------
#   LIKED <offset> <limit>
#   PLAYLIST <playlist_id> <offset> <limit>
#   SEARCH <query> <offset> <limit>        query spaces are sent as spaces;
#                                          the player strips tabs/newlines
#     N\t<total>                          the list's own length
#     T\t<id>\t<title>\t<artist>\t<album>\t<cover_url>\t<duration_ms>
#     ...                                  one per track, in order
#     END
#
#   A track the daemon could not read still takes its place in the numbering:
#     ?\t<reason>
#   The player draws that row as an explanation rather than a track: its id
#   is empty, so tapping it does nothing instead of playing the wrong song.
#
#   PLAYLISTS
#     P\t<playlist_id>\t<name>\t<owner>\t<count>
#     ...
#     END
#
#   QUEUE
#     Q\t<current_index>\t<count>        both 0-based / 0 when empty
#     END
#
#   AUTH
#     AUTH ok <account>                  signed in; <account> is the rest of
#                                        the line, spaces and all, because a
#                                        display name has spaces in it
#     AUTH none                          not signed in
#     END
#
# Playback
# --------
#   PLAY <track_id>              load and play one track, queue dropped
#   PLAYCTX liked - <track_id>   start Liked Songs at that track
#   PLAYCTX playlist <id> <track_id>
#   PLAYCTX search - <track_id>  start the last search's results at that track
#   TOGGLE                       play if paused, pause if playing
#   RESUME                       play
#   PAUSE                        pause (the stream goes quiet, not away)
#   NEXT / PREV                  step through the active queue
#   SEEK <ms>
#   SHUFFLE <0|1>
#   REPEAT <0|1|2>               0 off, 1 the whole queue, 2 the one track
#   VOLUME <0..100>              the session's software volume
#   PING                         END, for a liveness check
#   END                          close this daemon
#
#   PLAYCTX exists because the daemon, not the player, owns the queue: the
#   player only ever knows which page a track was tapped on. "liked" is the
#   identity of the heart list; "search" refers to the query the daemon last
#   answered for this connection; "playlist" names the playlist by id.
#
# Audio
# -----
#   PCM
#     The connection switches to binary for the rest of its life. First:
#
#       32 bytes   "SNXPCM1" in a zero-filled field
#       line       "<rate> <channels> <bits>\n"     e.g. "44100 2 16\n"
#
#     then nothing but interleaved little-endian samples, frame after frame,
#     for as long as any track plays. 44100 Hz stereo 16-bit by default;
#     24 (S24_3LE, three bytes per sample) when the player asks for 24 in
#     spotifyd.conf. A new track does NOT restart the channel: silence while a
#     track loads, then its frames -- the player keeps one DAC open.
#
#     Pausing stops the bytes (the player reads nothing and the daemon does
#     not block forever: it drops what it decodes while paused). Seeking
#     continues the stream from the new position. If the daemon dies, the
#     connection closes and the player reports the track stopped -- that is
#     the whole error protocol for audio.
#
#     One consumer per channel is expected. A second PCM connection replaces
#     the first, which is how the player restarts its own stream after a stop.
#
#     Frames may arrive split at any byte, and the player carries the tail of
#     a frame into the next read; a daemon is free to write in any sizes.
#
#   Audio is decoded by librespot in its own process and written here as
#   signed 16-bit (or 24-bit) PCM. The daemon never opens an ALSA device: on
#   the V1 there is exactly one, and the player owns it.
#
# Errors
# ------
#   ERR <reason> is an answer, not a failure of the connection: the player
#   returns it to the caller with the error text set and keeps the socket.
#   The one case that tears the socket down is EOF, which the player treats
#   as the daemon having gone away; it then reports the backend disconnected
#   and the home page shows its offline card.
#
# The player's side of all of this is sonix-player/src/system/streaming/spotify.c
# and spotifymock.c (the demo library used when no daemon is present). The
# mock implements the same replies in-process, so the pages can be built and
# tested with no daemon, no credentials and no network.
