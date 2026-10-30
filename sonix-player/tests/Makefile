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

.PHONY: test sanitize clean extractkeys

test: $(BUILD)/test_subsonic
	python3 run_subsonic_tests.py $(BUILD)/test_subsonic

# The stock-firmware credential extractor and its ini/seal round-trips. Plain
# Python, no compiler: the fixtures reproduce hiby_player's string layout.
extractkeys:
	python3 test_extract_streamkeys.py

$(BUILD)/test_subsonic: test_subsonic.c support.c $(CORE) $(shell find $(ROOT)/src/system -name '*.h')
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_subsonic.c support.c $(CORE) $(LDFLAGS) $(LDLIBS)

sanitize:
	$(MAKE) BUILD=.build/sanitize CFLAGS='-O1 -g -std=gnu11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 -ffunction-sections -fdata-sections -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-Wl,--gc-sections -fsanitize=address,undefined' test

# The battery guard: when the player warns at five percent, when it switches
# itself off at one, and which readings leave standby alone. The rule is its own
# small module with no LVGL, no sysfs and no clock, which is exactly what makes
# it testable here: power.c reads the gauge and does the two things.
GUARD_SRC = test_batteryguard.c $(ROOT)/src/system/device/batteryguard.c
.PHONY: batteryguard
batteryguard: $(BUILD)/test_batteryguard
	$(BUILD)/test_batteryguard

$(BUILD)/test_batteryguard: $(GUARD_SRC) $(ROOT)/src/system/device/batteryguard.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(GUARD_SRC) $(LDFLAGS) $(LDLIBS)

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

# The Bootlogo folder and the copy made from it: which files the page offers,
# and that the one chosen comes out the other end as a 240x320 baseline JPEG
# the boot script can draw. The pictures are written here with the player's own
# encoder and the copy is checked by decoding it back. Plain C, no LVGL.
BOOTPIC_SRC = test_bootpic.c $(ROOT)/src/system/device/bootpic.c \
              $(ROOT)/src/system/image/jpeg_scaled.c $(ROOT)/src/system/image/png_scaled.c \
              $(ROOT)/src/system/image/stb_image_impl.c $(ROOT)/src/system/library/jpeginfo.c
.PHONY: bootpic
bootpic: $(BUILD)/test_bootpic
	$(BUILD)/test_bootpic

$(BUILD)/test_bootpic: $(BOOTPIC_SRC) $(ROOT)/src/system/device/bootpic.h \
                       $(ROOT)/src/system/image/jpeg_scaled.h $(ROOT)/src/system/image/png_scaled.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(BOOTPIC_SRC) $(LDFLAGS) $(LDLIBS)

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

# The graphic EQ, PEQ and MSEB on stereo PCM -- the same eq_process() radio
# stations go through. Plain C, no ALSA and no LVGL.
EQ_SRC = test_eq.c $(ROOT)/src/system/audio/eq.c $(ROOT)/src/system/core/config.c
.PHONY: eq
eq: $(BUILD)/test_eq
	$(BUILD)/test_eq

$(BUILD)/test_eq: $(EQ_SRC) $(ROOT)/src/system/audio/eq.h $(ROOT)/src/system/core/config.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(EQ_SRC) $(LDFLAGS) $(LDLIBS)

# The Wi-Fi worker's hidden-network join: the real wifi.c against a fake
# wpa_cli on PATH (see test_wifi.c). Plain C, no LVGL and no radio.
WIFI_SRC = test_wifi.c $(ROOT)/src/system/net/wifi.c
.PHONY: wifi
wifi: $(BUILD)/test_wifi
	$(BUILD)/test_wifi

$(BUILD)/test_wifi: $(WIFI_SRC) $(ROOT)/src/system/net/wifi.h \
                    $(ROOT)/src/system/device/sysserver.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Istubs $(CFLAGS) -o $@ test_wifi.c $(ROOT)/src/system/net/wifi.c $(LDFLAGS) $(LDLIBS)

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

NOTES_SRC = test_notes.c $(ROOT)/src/system/notes/notesstore.c $(ROOT)/src/system/text/textfile.c
.PHONY: notes
notes: $(BUILD)/test_notes
	$(BUILD)/test_notes

$(BUILD)/test_notes: $(NOTES_SRC) $(ROOT)/src/system/notes/notesstore.h $(ROOT)/src/system/text/textfile.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(NOTES_SRC) $(LDFLAGS) $(LDLIBS)

# The ListenBrainz module's rules without a network: the queue file on the
# card, the JSON body a request carries and the answers the server sends. The
# module is compiled into the test (its internals are what is checked); the
# network, the card and the clock-less externals are stubbed there. The JSON
# parser and the settings store are the real ones.
LB_SRC = test_listenbrainz.c $(ROOT)/src/system/core/json.c $(ROOT)/src/system/core/config.c
.PHONY: listenbrainz
listenbrainz: $(BUILD)/test_listenbrainz
	$(BUILD)/test_listenbrainz

$(BUILD)/test_listenbrainz: $(LB_SRC) $(ROOT)/src/system/listenbrainz/listenbrainz.h \
					$(ROOT)/src/system/core/json.h $(ROOT)/src/system/core/config.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(LB_SRC) $(LDFLAGS) $(LDLIBS)

# The rows of Album Art Max: the now-playing arrangement that trades the media
# controls for the artwork. The sleeve is asked for at the size this gives
# before anything has been laid out, so the arithmetic is its own module -- and
# that module is plain C, which is what this checks: the rows add up to the
# panel, Leftie costs exactly its rail, and a panel too short or too narrow
# still answers with something drawable.
AAM_SRC = test_aamlayout.c $(ROOT)/src/gui/nowplaying/aamlayout.c
.PHONY: aamlayout
aamlayout: $(BUILD)/test_aamlayout
	$(BUILD)/test_aamlayout

$(BUILD)/test_aamlayout: $(AAM_SRC) $(ROOT)/src/gui/nowplaying/aamlayout.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(AAM_SRC) $(LDFLAGS) $(LDLIBS)

# The dates tags carry: every shape the taggers write, the precision they
# keep, and the moment they resolve to. The podcast lists order episodes and
# write "12 March 2026" from this, so a misread date is a row in the wrong
# place. Plain C, no LVGL and no files on disk.
TAGDATE_SRC = test_tagdate.c $(ROOT)/src/system/library/tagdate.c
.PHONY: tagdate
tagdate: $(BUILD)/test_tagdate
	$(BUILD)/test_tagdate

$(BUILD)/test_tagdate: $(TAGDATE_SRC) $(ROOT)/src/system/library/tagdate.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TAGDATE_SRC) $(LDFLAGS) $(LDLIBS)

# LRCLIB search ranking, retry policy, free-text fallback and atomic sidecar
# .lrc writes against a stubbed HTTP transport. Plain C, no LVGL.
LRCLIB_SRC = test_lrclib.c $(ROOT)/src/system/core/json.c
.PHONY: lrclib
lrclib: $(BUILD)/test_lrclib
	$(BUILD)/test_lrclib

$(BUILD)/test_lrclib: $(LRCLIB_SRC) $(ROOT)/src/system/library/lrclib.c \
                      $(ROOT)/src/system/library/lrclib.h $(ROOT)/src/system/core/json.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(LRCLIB_SRC) $(LDFLAGS) $(LDLIBS)

# Tag reading, format sniffing, streaming Ogg comments and FLAC/Ogg ID3 skipping
# across real containers and V1 tagdate rules. Plain C, no external codec libs.
TAGS_DECODE_SRC = $(ROOT)/src/system/decode/decode.c \
                  $(ROOT)/src/system/decode/sndfile.c \
                  $(ROOT)/src/system/decode/stb_vorbis.c \
                  $(ROOT)/src/system/decode/mp4.c \
                  $(ROOT)/src/system/decode/apedec.c \
                  $(ROOT)/src/system/decode/ape/apecore.c \
                  $(ROOT)/src/system/decode/alacdec.c \
                  $(ROOT)/src/system/decode/alac/alac.c \
                  $(ROOT)/src/system/decode/aacdec.c \
                  $(ROOT)/src/system/decode/dsd.c \
                  $(ROOT)/src/system/decode/growfile.c \
                  $(ROOT)/src/system/streaming/streambridge.c
TAGS_SRC = test_tags.c \
           $(ROOT)/src/system/library/metadata.c \
           $(ROOT)/src/system/library/oggtags.c \
           $(ROOT)/src/system/library/vorbischap.c \
           $(ROOT)/src/system/library/tagdate.c \
           $(ROOT)/src/system/library/cue.c \
           $(ROOT)/src/system/image/png_scaled.c \
           $(ROOT)/src/system/core/config.c \
           $(ROOT)/src/system/core/utils.c
.PHONY: tags
tags: $(BUILD)/test_tags
	$(BUILD)/test_tags

$(BUILD)/test_tags: $(TAGS_SRC) $(TAGS_DECODE_SRC) \
                    $(ROOT)/src/system/library/metadata.h $(ROOT)/src/system/library/oggtags.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-error=format-truncation -o $@ $(TAGS_SRC) \
		-Wno-unused-parameter -Wno-sign-compare -Wno-unused-function -Wno-implicit-fallthrough -Wno-misleading-indentation $(TAGS_DECODE_SRC) \
		$(LDFLAGS) $(LDLIBS)

# SD-card mmap artwork (>8 MB), SIGBUS card-removal recovery, streaming Ogg and
# APEv2 embedded covers, 1000-row JPEG player_covers cache and id_worker.
COVERS_SRC = test_covers.c \
             $(ROOT)/src/system/library/albumart.c \
             $(ROOT)/src/system/library/oggtags.c \
             $(ROOT)/src/system/library/cue.c \
             $(ROOT)/src/gui/nowplaying/cover.c \
             $(ROOT)/src/gui/nowplaying/coverloader.c \
             $(ROOT)/src/system/image/art_shrink.c \
             $(ROOT)/src/system/image/jpeg_scaled.c \
             $(ROOT)/src/system/image/png_scaled.c \
             $(ROOT)/src/system/image/stb_image_impl.c \
             $(ROOT)/src/system/core/utils.c
.PHONY: covers
covers: $(BUILD)/test_covers
	$(BUILD)/test_covers

$(BUILD)/test_covers: $(COVERS_SRC) $(TAGS_DECODE_SRC) $(BUILD)/sqlite3_impl.o \
                      $(ROOT)/src/system/library/albumart.h $(ROOT)/src/gui/nowplaying/cover.h
	@mkdir -p $(BUILD)
	$(CC) $(UI_CPPFLAGS) $(CFLAGS) -Wno-error=format-truncation -o $@ $(COVERS_SRC) \
		-Wno-unused-parameter -Wno-sign-compare -Wno-unused-function -Wno-implicit-fallthrough -Wno-misleading-indentation $(TAGS_DECODE_SRC) \
		$(BUILD)/sqlite3_impl.o $(LDFLAGS) $(LDLIBS)

# FLAC radio stream decoding: native FLAC behind ID3v2, mid-stream 24-bit
# relaxed join across false sync markers, chained Ogg FLAC with song titles
# and multi-page packet assembly, and non-FLAC rejection.
RADIOFLAC_SRC = test_radioflac.c $(ROOT)/src/system/streaming/radioflac.c
.PHONY: radioflac sanitize-radioflac
radioflac: $(BUILD)/test_radioflac
	$(BUILD)/test_radioflac

$(BUILD)/test_radioflac: $(RADIOFLAC_SRC) $(BUILD)/drflac_impl.o \
                         $(ROOT)/src/system/streaming/radioflac.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(RADIOFLAC_SRC) $(BUILD)/drflac_impl.o $(LDFLAGS) $(LDLIBS)

$(BUILD)/drflac_impl.o: $(ROOT)/src/system/decode/dr_flac.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -DDR_FLAC_IMPLEMENTATION -x c -c -o $@ $<

sanitize-radioflac:
	$(MAKE) BUILD=.build/sanitize CFLAGS='-O1 -g -std=gnu11 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 -ffunction-sections -fdata-sections -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-Wl,--gc-sections -fsanitize=address,undefined' radioflac

# The Most played list against the real library database: the play upsert
# device_state.c runs at track start, the handle the page opens, and the
# streaming reader the queue is built from. The card is a temporary directory
# and the scan's file-type probes are stubbed; the schema, the upsert and the
# queries are the real ones. Plain C, no LVGL. See tests/README.md.
MOSTPLAYED_SRC = test_mostplayed.c $(ROOT)/src/system/library/library.c \
                 $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/utils.c
.PHONY: mostplayed
mostplayed: $(BUILD)/test_mostplayed
	$(BUILD)/test_mostplayed

$(BUILD)/test_mostplayed: $(MOSTPLAYED_SRC) $(BUILD)/sqlite3_impl.o \
                          $(ROOT)/src/system/library/library.h $(ROOT)/src/system/library/cue.h \
                          $(ROOT)/src/system/decode/mp4.h $(ROOT)/src/system/decode/apedec.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(MOSTPLAYED_SRC) $(BUILD)/sqlite3_impl.o $(LDFLAGS) $(LDLIBS)

# The vendored SQLite amalgamation keeps its own warnings to itself: third-
# party code, built separately (and quietly) so the suite around it keeps
# -Werror.
$(BUILD)/sqlite3_impl.o: $(ROOT)/src/system/db/sqlite3_impl.c $(ROOT)/src/system/db/sqlite3_amalgamation.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -c -o $@ $<

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
         $(ROOT)/src/gui/library/music.c $(ROOT)/src/gui/shell/popover.c \
         $(ROOT)/src/gui/shell/gridpage.c $(ROOT)/src/gui/settings/appearance.c \
         $(ROOT)/src/gui/settings/bootpicpage.c \
         $(ROOT)/src/gui/shell/corners.c $(ROOT)/src/gui/shell/settingsrow.c \
         $(ROOT)/src/gui/shell/theme.c \
         $(ROOT)/src/gui/shell/icons.c $(ROOT)/src/gui/shell/main_menu_icons.c \
         $(ROOT)/src/gui/shell/gestures.c \
         $(ROOT)/src/gui/board_profile.c \
         $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
UI_LVGL_SRC = $(shell find $(ROOT)/lvgl/src -name '*.c' 2>/dev/null)
UI_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_SRC)) \
         $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))
UI_CPPFLAGS = $(CPPFLAGS) -I$(ROOT)/lvgl -DLV_CONF_PATH='"$(abspath ui/lv_conf.h)"'
UI_CFLAGS = $(CFLAGS) -MMD -MP

# The screensaver's Framed style on the same renderer; shares the objects above.
# The Wi-Fi page and its manual-entry page for hidden networks, on the same
# renderer; the radio under it is a recording stub (see test_wifi_pages.c).
WIFIPAGE_SRC = $(ROOT)/tests/ui/test_wifi_pages.c $(ROOT)/src/gui/wireless/wifisettings.c \
            $(ROOT)/src/gui/shell/settingsrow.c $(ROOT)/src/gui/shell/keyboard.c \
            $(ROOT)/src/gui/shell/popover.c $(ROOT)/src/gui/shell/spinner.c \
            $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
            $(ROOT)/src/gui/board_profile.c $(ROOT)/src/system/input/kblayout.c \
            $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
WIFIPAGE_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(WIFIPAGE_SRC)) \
            $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

SAVER_SRC = $(ROOT)/tests/ui/test_screensaver.c $(ROOT)/src/gui/settings/screensaver.c $(ROOT)/src/system/audio/visualizer.c \
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
            $(ROOT)/src/gui/metronome/metronome.c \
            $(ROOT)/src/system/notes/notesstore.c $(ROOT)/src/system/text/textfile.c \
            $(ROOT)/src/gui/library/textview.c $(ROOT)/src/gui/library/pictureview.c \
            $(ROOT)/src/gui/shell/morepage.c $(ROOT)/src/gui/shell/gridpage.c \
            $(ROOT)/src/gui/shell/settingsrow.c $(ROOT)/src/gui/shell/keyboard.c \
            $(ROOT)/src/gui/shell/popover.c $(ROOT)/src/system/input/kblayout.c \
            $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
            $(ROOT)/src/gui/board_profile.c \
            $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
TOOLS_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(TOOLS_SRC)) \
            $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

# The battery screen a charger boot opens on: its layout at both panel sizes,
# the fill being the reading it was given, and the three state lines in ten
# languages. The power key and the boot it belongs to need a device; this needs
# only the glass.
CHARGE_SRC = $(ROOT)/tests/ui/test_chargescreen.c $(ROOT)/src/gui/shell/chargescreen.c \
             $(ROOT)/src/gui/board_profile.c \
             $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
CHARGE_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(CHARGE_SRC)) \
             $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

# The LRCLIB results list, preview and Apply flow on the 240x320 panel across
# all eleven shipped languages and all five V1 palettes.
LYRICSEARCH_SRC = $(ROOT)/tests/ui/test_lyricsearch.c \
                  $(ROOT)/src/gui/nowplaying/lyricsearch.c \
                  $(ROOT)/src/gui/shell/settingsrow.c \
                  $(ROOT)/src/gui/shell/theme.c \
                  $(ROOT)/src/gui/shell/icons.c \
                  $(ROOT)/src/gui/board_profile.c \
                  $(ROOT)/src/system/library/lrclib.c \
                  $(ROOT)/src/system/core/json.c \
                  $(ROOT)/src/system/core/config.c \
                  $(ROOT)/src/system/core/lang.c
LYRICSEARCH_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(LYRICSEARCH_SRC)) \
                  $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

.PHONY: ui
ui: $(BUILD)/test_main_menu $(BUILD)/test_screensaver $(BUILD)/test_tools_pages $(BUILD)/test_wifi_pages $(BUILD)/test_chargescreen $(BUILD)/test_lyricsearch
	$(BUILD)/test_main_menu ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_main_menu ../../sonix-packer/assets/V1/usr/resource/sonix/language saved
	$(BUILD)/test_screensaver ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_screensaver ../../sonix-packer/assets/V1/usr/resource/sonix/language saved
	$(BUILD)/test_tools_pages ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_wifi_pages ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_chargescreen ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_lyricsearch ../../sonix-packer/assets/V1/usr/resource/sonix/language

$(BUILD)/test_main_menu: $(UI_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_screensaver: $(SAVER_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_chargescreen: $(CHARGE_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_tools_pages: $(TOOLS_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_wifi_pages: $(WIFIPAGE_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_lyricsearch: $(LYRICSEARCH_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/ui/%.o: $(ROOT)/%.c ui/lv_conf.h $(ROOT)/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CPPFLAGS) $(UI_CFLAGS) -c $< -o $@

# Upstream LVGL is not maintained here; keep its warnings nonfatal.
$(BUILD)/ui/lvgl/%.o: $(ROOT)/lvgl/%.c ui/lv_conf.h $(ROOT)/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CPPFLAGS) $(filter-out -Werror,$(UI_CFLAGS)) -c $< -o $@

-include $(sort $(UI_OBJ:.o=.d) $(SAVER_OBJ:.o=.d) $(WIFIPAGE_OBJ:.o=.d) $(LYRICSEARCH_OBJ:.o=.d))
