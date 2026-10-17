CC ?= cc
PKG_CONFIG ?= pkg-config
ROOT := ..
BUILD := .build
CPPFLAGS += -I$(ROOT)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 -ffunction-sections -fdata-sections
# The player is built with the Game Boy core (GB_CORE=1 in the main Makefile),
# which is what gives the More menu its seventh entry. The headless suites use
# the same flag so the page they lay out is the page the firmware builds.
CFLAGS += -DGB_CORE=1
LDLIBS += -lpthread -ldl -lm
LDFLAGS += -Wl,--gc-sections

CORE = $(ROOT)/src/system/streaming/subsonic.c \
       $(ROOT)/src/system/streaming/streambridge.c \
       $(ROOT)/src/system/streaming/subsonicstream.c \
       $(ROOT)/src/system/net/http.c $(ROOT)/src/system/net/tls.c \
       $(ROOT)/src/system/core/json.c $(ROOT)/src/system/core/md5.c \
       $(ROOT)/src/system/core/config.c $(ROOT)/src/system/playback/playlist.c

.PHONY: test sanitize clean spotify

test: $(BUILD)/test_subsonic
	python3 run_subsonic_tests.py $(BUILD)/test_subsonic

$(BUILD)/test_subsonic: test_subsonic.c support.c $(CORE) $(shell find $(ROOT)/src/system -name '*.h')
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_subsonic.c support.c $(CORE) $(LDFLAGS) $(LDLIBS)

sanitize:
	$(MAKE) BUILD=.build/sanitize CFLAGS='-O1 -g -std=gnu11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 -ffunction-sections -fdata-sections -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-Wl,--gc-sections -fsanitize=address,undefined' test

# The Screensaver folder's rules: which files are pictures, judged from their
# JPEG headers, and the random pick between them. Plain C, no LVGL.
PICS_SRC = test_screensaverpics.c $(ROOT)/src/system/device/screensaverpics.c \
           $(ROOT)/src/system/library/jpeginfo.c
.PHONY: pictures
pictures: $(BUILD)/test_screensaverpics
	$(BUILD)/test_screensaverpics

$(BUILD)/test_screensaverpics: $(PICS_SRC) $(ROOT)/src/system/device/screensaverpics.h \
                               $(ROOT)/src/system/library/jpeginfo.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(PICS_SRC) $(LDFLAGS) $(LDLIBS)

# The software volume's arithmetic -- the whole level law for a USB DAC with
# no control of its own. Plain C, no ALSA: the three routing answers
# (Bluetooth, a card on the port, that card's control) are stubbed.
SWVOL_SRC = test_swvolume.c $(ROOT)/src/system/audio/swvolume.c
.PHONY: swvolume
swvolume: $(BUILD)/test_swvolume
	$(BUILD)/test_swvolume

$(BUILD)/test_swvolume: $(SWVOL_SRC) $(ROOT)/src/system/audio/swvolume.h \
                        $(ROOT)/src/system/audio/usbaudio.h $(ROOT)/src/system/audio/audio.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SWVOL_SRC) $(LDFLAGS) $(LDLIBS)

# The Calculator page's arithmetic and the notes folder's rules: the two new
# pages' plain-C halves, with no LVGL and no screen. The calculator model and
# the notes store (with the shared text layer under it) are linked as they are
# built into the player.
CALC_SRC = test_calculator.c $(ROOT)/src/gui/calculator/calcmodel.c
.PHONY: calculator
calculator: $(BUILD)/test_calculator
	$(BUILD)/test_calculator

$(BUILD)/test_calculator: $(CALC_SRC) $(ROOT)/src/gui/calculator/calcmodel.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(CALC_SRC) $(LDFLAGS) $(LDLIBS)

# Spotify: the Q2SPOT/1 protocol, the receiver's status and control sockets,
# and the sink that carries its PCM to the DAC. The real spotify.c, spotifysink.c
# and spotifybrowse.c, with a stand-in receiver on a temporary card and
# audio_external_* stubbed -- no librespot, no account, no network.
# core/utils.c is here for deadline_in_ms(), and it is here for real rather than
# stubbed: the sink waits on statically initialized condvars, so the clock that
# helper computes its deadlines on is the difference between threads that sleep
# and threads that spin -- which, on the device, is the difference between a
# device that works and a device that freezes. A stub of it in this file would
# hide exactly the bug this suite is meant to hold. thread_be_realtime() comes
# with it, so the playback thread really does ask to be real time.
SPOTIFY_SRC = test_spotify.c $(ROOT)/src/system/streaming/spotify.c \
              $(ROOT)/src/system/streaming/spotifysink.c \
              $(ROOT)/src/system/streaming/spotifybrowse.c \
              $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/utils.c
.PHONY: spotify
spotify: $(BUILD)/test_spotify
	$(BUILD)/test_spotify

$(BUILD)/test_spotify: $(SPOTIFY_SRC) $(ROOT)/src/system/streaming/spotify.h \
                       $(ROOT)/src/system/streaming/spotifysink.h \
                       $(ROOT)/src/system/streaming/spotifybrowse.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SPOTIFY_SRC) $(LDFLAGS) $(LDLIBS)

NOTES_SRC = test_notes.c $(ROOT)/src/system/notes/notesstore.c $(ROOT)/src/system/text/textfile.c
.PHONY: notes
notes: $(BUILD)/test_notes
	$(BUILD)/test_notes

$(BUILD)/test_notes: $(NOTES_SRC) $(ROOT)/src/system/notes/notesstore.h $(ROOT)/src/system/text/textfile.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(NOTES_SRC) $(LDFLAGS) $(LDLIBS)

# Optional real-codec integration: the usual host development packages are
# sufficient. No fake MP3/FLAC/Opus decoder is used. See tests/README.md.
CODEC = $(filter-out %/wavpackdec.c %/opusdec.c,$(wildcard $(ROOT)/src/system/decode/*.c)) \
        $(ROOT)/src/system/decode/opusdec.c $(ROOT)/src/system/decode/wavpackdec.c \
        $(ROOT)/src/system/decode/alac/alac.c $(ROOT)/src/system/decode/ape/apecore.c \
        $(ROOT)/src/system/library/cue.c
.PHONY: codecs sanitize-codecs
codecs: $(BUILD)/test_codecs
	python3 run_subsonic_tests.py $(BUILD)/test_codecs

$(BUILD)/test_codecs: test_codecs.c support.c $(CORE) $(CODEC)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTEST_REAL_CODECS=1 $(shell $(PKG_CONFIG) --cflags opusfile wavpack) \
		-o $@ test_codecs.c support.c $(CORE) $(CODEC) $(LDFLAGS) $(LDLIBS) $(shell $(PKG_CONFIG) --static --libs opusfile wavpack)

# Existing MP4 tag copies intentionally truncate display strings; GCC -O1
# diagnoses those inlined bounded copies. Keep all other warnings fatal.
sanitize-codecs:
	$(MAKE) BUILD=.build/sanitize-codecs CFLAGS='-O1 -g -std=gnu11 -Wall -Wextra -Werror -Wno-error=format-truncation -D_FILE_OFFSET_BITS=64 -ffunction-sections -fdata-sections -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-Wl,--gc-sections -fsanitize=address,undefined' codecs

clean:
	rm -rf $(BUILD)

# Optional headless LVGL layout regression tests. Reuse the production config,
# disabling SDL, FreeType and device drivers; no audio service is linked.
UI_SRC = $(ROOT)/tests/ui/test_main_menu.c $(ROOT)/src/gui/shell/main_menu.c \
         $(ROOT)/src/gui/shell/gridpage.c $(ROOT)/src/gui/settings/appearance.c \
         $(ROOT)/src/gui/shell/settingsrow.c $(ROOT)/src/gui/shell/theme.c \
         $(ROOT)/src/gui/shell/icons.c $(ROOT)/src/gui/shell/main_menu_icons.c \
         $(ROOT)/src/gui/board_profile.c \
         $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
UI_LVGL_SRC = $(shell find $(ROOT)/lvgl/src -name '*.c' 2>/dev/null)
UI_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_SRC)) \
         $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))
UI_CPPFLAGS = $(CPPFLAGS) -I$(ROOT)/lvgl -DLV_CONF_PATH='"$(abspath ui/lv_conf.h)"'
UI_CFLAGS = $(CFLAGS) -MMD -MP

# The screensaver's Framed style on the same renderer; shares the objects above.
SAVER_SRC = $(ROOT)/tests/ui/test_screensaver.c $(ROOT)/src/gui/settings/screensaver.c \
            $(ROOT)/src/gui/shell/scrolltext.c $(ROOT)/src/gui/shell/settingsrow.c \
            $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
            $(ROOT)/src/gui/board_profile.c \
            $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
SAVER_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(SAVER_SRC)) \
            $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

# The Calculator and Notes pages on the same renderer, at both panel sizes:
# every key, every row and every note inside the panel they are drawn on.
TOOLS_SRC = $(ROOT)/tests/ui/test_tools_pages.c \
            $(ROOT)/src/gui/calculator/calculator.c $(ROOT)/src/gui/calculator/calcmodel.c \
            $(ROOT)/src/gui/notes/notes.c $(ROOT)/src/gui/notes/noteeditor.c \
            $(ROOT)/src/system/notes/notesstore.c $(ROOT)/src/system/text/textfile.c \
            $(ROOT)/src/gui/library/textview.c \
            $(ROOT)/src/gui/shell/morepage.c $(ROOT)/src/gui/shell/gridpage.c \
            $(ROOT)/src/gui/shell/settingsrow.c $(ROOT)/src/gui/shell/keyboard.c \
            $(ROOT)/src/gui/shell/popover.c $(ROOT)/src/system/input/kblayout.c \
            $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
            $(ROOT)/src/gui/board_profile.c \
            $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
TOOLS_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(TOOLS_SRC)) \
            $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

# The Spotify page on the same renderer: the switch, the browser and the
# settings, with the receiver stubbed and the Q2SPOT/1 client with it.
SPOTPAGE_SRC = $(ROOT)/tests/ui/test_spotify_page.c \
               $(ROOT)/src/gui/streaming/spotifypage.c \
               $(ROOT)/src/gui/shell/settingsrow.c $(ROOT)/src/gui/shell/scrolltext.c \
               $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
               $(ROOT)/src/gui/board_profile.c \
               $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
SPOTPAGE_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(SPOTPAGE_SRC)) \
               $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

.PHONY: ui
ui: $(BUILD)/test_main_menu $(BUILD)/test_screensaver $(BUILD)/test_tools_pages $(BUILD)/test_spotify_page
	BOARD=tempotec_v1 $(BUILD)/test_main_menu ../../sonix-packer/assets/R1/usr/resource/sonix/language
	BOARD=tempotec_v1 $(BUILD)/test_main_menu ../../sonix-packer/assets/R1/usr/resource/sonix/language saved
	BOARD=hiby_r1 $(BUILD)/test_main_menu ../../sonix-packer/assets/R3PII/usr/resource/sonix/language
	BOARD=hiby_r1 $(BUILD)/test_main_menu ../../sonix-packer/assets/R3PII/usr/resource/sonix/language saved
	BOARD=tempotec_v1 $(BUILD)/test_screensaver ../../sonix-packer/assets/R1/usr/resource/sonix/language
	BOARD=tempotec_v1 $(BUILD)/test_screensaver ../../sonix-packer/assets/R1/usr/resource/sonix/language saved
	BOARD=hiby_r1 $(BUILD)/test_screensaver ../../sonix-packer/assets/R3PII/usr/resource/sonix/language
	BOARD=hiby_r1 $(BUILD)/test_screensaver ../../sonix-packer/assets/R3PII/usr/resource/sonix/language saved
	BOARD=tempotec_v1 $(BUILD)/test_tools_pages ../../sonix-packer/assets/R1/usr/resource/sonix/language
	BOARD=hiby_r1 $(BUILD)/test_tools_pages ../../sonix-packer/assets/R3PII/usr/resource/sonix/language
	BOARD=tempotec_v1 $(BUILD)/test_spotify_page ../../sonix-packer/assets/R1/usr/resource/sonix/language
	BOARD=hiby_r1 $(BUILD)/test_spotify_page ../../sonix-packer/assets/R3PII/usr/resource/sonix/language

$(BUILD)/test_main_menu: $(UI_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_screensaver: $(SAVER_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_tools_pages: $(TOOLS_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_spotify_page: $(SPOTPAGE_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/ui/%.o: $(ROOT)/%.c ui/lv_conf.h $(ROOT)/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CPPFLAGS) $(UI_CFLAGS) -c $< -o $@

# Upstream LVGL is not maintained here; keep its warnings nonfatal.
$(BUILD)/ui/lvgl/%.o: $(ROOT)/lvgl/%.c ui/lv_conf.h $(ROOT)/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CPPFLAGS) $(filter-out -Werror,$(UI_CFLAGS)) -c $< -o $@

-include $(sort $(UI_OBJ:.o=.d) $(SAVER_OBJ:.o=.d) $(TOOLS_OBJ:.o=.d) $(SPOTPAGE_OBJ:.o=.d))
