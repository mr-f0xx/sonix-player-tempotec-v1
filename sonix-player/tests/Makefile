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
       $(ROOT)/src/system/image/png_scaled.c \
       $(ROOT)/src/system/net/http.c $(ROOT)/src/system/net/tls.c \
       $(ROOT)/src/system/core/json.c $(ROOT)/src/system/core/md5.c \
       $(ROOT)/src/system/core/config.c $(ROOT)/src/system/playback/playlist.c

.PHONY: test sanitize clean extractkeys qobuz

test: $(BUILD)/test_subsonic
	python3 run_subsonic_tests.py $(BUILD)/test_subsonic

# Pseudo-files (procfs/configfs) can have zero size or reject seeking.
.PHONY: readfile
readfile: $(BUILD)/test_read_file_content
	$(BUILD)/test_read_file_content

$(BUILD)/test_read_file_content: test_read_file_content.c $(ROOT)/src/system/core/utils.c $(ROOT)/src/system/core/utils.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_read_file_content.c $(ROOT)/src/system/core/utils.c $(LDFLAGS) $(LDLIBS)

# The stock-firmware credential extractor and its ini/seal round-trips. Plain
# Python, no compiler: the fixtures reproduce hiby_player's string layout.
extractkeys: $(BUILD)/test_stockkeys
	python3 test_extract_streamkeys.py
	$(BUILD)/test_stockkeys

$(ROOT)/build_gen/streamkeys_key.h:
	@mkdir -p $(dir $@)
	@printf '// Generated for host tests.\n#define STREAMKEYS_KEY_LEN 0\n' > $@

STOCKKEYS_SRC = test_stockkeys.c \
                $(ROOT)/src/system/streaming/stockkeys.c \
                $(ROOT)/src/system/streaming/streamkeys.c \
                $(ROOT)/src/system/image/png_scaled.c \
                $(ROOT)/src/system/net/http.c \
                $(ROOT)/src/system/net/tls.c \
                $(ROOT)/src/system/core/config.c \
                $(ROOT)/src/system/core/sha1.c \
                $(ROOT)/src/system/core/sha256.c \
                $(ROOT)/src/system/core/utils.c

$(BUILD)/test_stockkeys: $(STOCKKEYS_SRC) $(ROOT)/build_gen/streamkeys_key.h \
                         $(ROOT)/src/system/streaming/stockkeys.h $(ROOT)/src/system/streaming/streamkeys.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHOST_BUILD -o $@ $(STOCKKEYS_SRC) $(LDFLAGS) $(LDLIBS)

QOBUZ_SRC = test_qobuz.c \
            $(ROOT)/src/system/streaming/qobuz.c \
            $(ROOT)/src/system/streaming/streamkeys.c \
            $(ROOT)/src/system/image/png_scaled.c \
            $(ROOT)/src/system/net/http.c \
            $(ROOT)/src/system/net/tls.c \
            $(ROOT)/src/system/core/json.c \
            $(ROOT)/src/system/core/md5.c \
            $(ROOT)/src/system/core/config.c \
            $(ROOT)/src/system/core/sha256.c

qobuz: $(BUILD)/test_qobuz
	$(BUILD)/test_qobuz

$(BUILD)/test_qobuz: $(QOBUZ_SRC) $(ROOT)/build_gen/streamkeys_key.h $(ROOT)/src/system/streaming/qobuz.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHOST_BUILD -o $@ $(QOBUZ_SRC) $(LDFLAGS) $(LDLIBS)

# Download worker with fake Qobuz and HTTP transport. Requires the same LVGL
# headers as the player (run make in the parent directory to fetch them).
.PHONY: qobuzdl
qobuzdl: $(BUILD)/test_qobuzdl
	$(BUILD)/test_qobuzdl

$(BUILD)/test_qobuzdl: test_qobuzdl.c $(ROOT)/src/system/streaming/qobuzdl.c $(ROOT)/src/system/streaming/qobuzdl.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -I$(ROOT)/lvgl $(CFLAGS) -Wno-error=format-truncation -DLV_CONF_INCLUDE_SIMPLE=1 -DHOST_BUILD -o $@ test_qobuzdl.c $(ROOT)/src/system/streaming/qobuzdl.c $(ROOT)/src/system/streaming/podcastdl.c $(LDFLAGS) $(LDLIBS)

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
EQ_SRC = test_eq.c $(ROOT)/src/system/audio/eq.c $(ROOT)/src/system/core/config.c $(ROOT)/src/system/addons/addons.c
.PHONY: eq
eq: $(BUILD)/test_eq
	$(BUILD)/test_eq

$(BUILD)/test_eq: $(EQ_SRC) $(ROOT)/src/system/audio/eq.h $(ROOT)/src/system/core/config.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(EQ_SRC) $(LDFLAGS) $(LDLIBS)

# The explicit V1 external-DAC host trial against a private fake sysfs tree.
# Exercises the real role writes, bounded wait, rollback and crash recovery.
.PHONY: usbhost
usbhost: $(BUILD)/test_usbhostprobe $(BUILD)/test_usbhost_reset $(BUILD)/test_usb_ownership $(BUILD)/test_usbexport $(BUILD)/test_usb_storage $(BUILD)/test_usbaudio_arbitration $(BUILD)/test_usbaudio_volume $(BUILD)/test_usbhostmode $(BUILD)/test_card_release
	$(BUILD)/test_usbhostprobe
	$(BUILD)/test_usbhost_reset
	$(BUILD)/test_usb_ownership
	$(BUILD)/test_usbexport
	$(BUILD)/test_usb_storage
	$(BUILD)/test_usbaudio_arbitration
	$(BUILD)/test_usbaudio_volume
	$(BUILD)/test_usbhostmode
	$(BUILD)/test_card_release

CARD_RELEASE_SRC = test_card_release.c $(ROOT)/src/system/core/logging.c $(ROOT)/src/system/audio/waveform.c
$(BUILD)/test_card_release: $(CARD_RELEASE_SRC) $(ROOT)/src/system/core/logging.h \
                            $(ROOT)/src/system/audio/waveform.h $(ROOT)/src/system/decode/decode.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(CARD_RELEASE_SRC) $(LDFLAGS) $(LDLIBS)

# The external USB DAC setting: the hold around the role trial, its retry after
# another owner of the controller, and what the row is told. The sysfs writes
# are test_usbhostprobe's; this one replaces them and the config.
$(BUILD)/test_usbhostmode: test_usbhostmode.c $(ROOT)/src/system/audio/usbhostmode.c \
                           $(ROOT)/src/system/audio/usbhostmode.h $(ROOT)/src/system/audio/usbhostprobe.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_usbhostmode.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_usbhost_reset: test_usbhost_reset.c $(ROOT)/src/system/device/factoryreset.c \
                             $(ROOT)/src/system/audio/usbhostprobe.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_usbhost_reset.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_usbaudio_arbitration: test_usbaudio_arbitration.c $(ROOT)/src/system/audio/usbaudio.c \
                                   stubs/lvgl/lvgl.h stubs/alsa/asoundlib.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Istubs $(CFLAGS) -o $@ test_usbaudio_arbitration.c $(LDFLAGS) $(LDLIBS)

# Where the level physically happens on a USB DAC: the device's own control by
# default, the samples the remainder, and the samples the whole of it when the
# control cannot be believed or will not move. The mixer is a fake table; the
# shape matching, the split arithmetic and the read-back are the real ones.
$(BUILD)/test_usbaudio_volume: test_usbaudio_volume.c $(ROOT)/src/system/audio/usbaudio.c \
                               stubs/lvgl/lvgl.h stubs/alsa/asoundlib.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Istubs $(CFLAGS) -o $@ test_usbaudio_volume.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_usb_storage: test_usb_storage.c $(ROOT)/src/system/device/usb.c \
                           $(ROOT)/src/system/device/usbexport.h stubs/lvgl/lvgl.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Istubs $(CFLAGS) -o $@ test_usb_storage.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_usbexport: test_usbexport.c $(ROOT)/src/system/device/usbexport.c \
                         $(ROOT)/src/system/device/usbexport.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_usbexport.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_usb_ownership: test_usb_ownership.c $(ROOT)/src/system/device/usb.c \
                             $(ROOT)/src/system/device/usb.h stubs/lvgl/lvgl.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Istubs $(CFLAGS) -o $@ test_usb_ownership.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_usbhostprobe: test_usbhostprobe.c $(ROOT)/src/system/audio/usbhostprobe.c \
                            $(ROOT)/src/system/audio/usbhostprobe.h $(ROOT)/src/system/device/usb.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_usbhostprobe.c $(LDFLAGS) $(LDLIBS)

# USB DAC PCM uses the same DSP chain while the DSD stream remains untouched.
# The test includes usbdac.c to exercise its real format gate without building
# the gadget or opening an audio device.
.PHONY: usbdac-eq
usbdac-eq: $(BUILD)/test_usbdac_eq
	$(BUILD)/test_usbdac_eq

$(BUILD)/test_usbdac_eq: test_usbdac_eq.c $(ROOT)/src/system/audio/usbdac.c \
                         $(ROOT)/src/system/audio/eq.c $(ROOT)/src/system/audio/eq.h \
                         $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/config.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Istubs $(CFLAGS) -o $@ test_usbdac_eq.c \
		$(ROOT)/src/system/audio/eq.c $(ROOT)/src/system/core/config.c $(ROOT)/src/system/addons/addons.c $(LDFLAGS) $(LDLIBS)

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

# Bluetooth's real A2DP-direction decision, startup wait policy and D-Bus
# signal cache, with fake controller/process state. No radio or ALSA required.
.PHONY: bluetooth
bluetooth: $(BUILD)/test_bluetooth $(BUILD)/test_btstack $(BUILD)/test_btreceiver $(BUILD)/test_btmetadata $(BUILD)/test_btdiscovery
	$(BUILD)/test_bluetooth
	$(BUILD)/test_btstack
	$(BUILD)/test_btreceiver
	$(BUILD)/test_btmetadata
	$(BUILD)/test_btdiscovery

$(BUILD)/test_bluetooth: test_bluetooth.c $(ROOT)/src/system/bluetooth/bluetooth.c \
                         $(ROOT)/src/system/bluetooth/bluetooth.h $(ROOT)/src/system/bluetooth/btafh.c \
                         $(ROOT)/src/system/bluetooth/btafh.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Istubs $(CFLAGS) -o $@ test_bluetooth.c $(ROOT)/src/system/bluetooth/btafh.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_btstack: test_btstack.c $(ROOT)/src/system/bluetooth/btstack.c \
                       $(ROOT)/src/system/bluetooth/btstack.h $(ROOT)/src/system/bluetooth/dbuslite.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_btstack.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_btreceiver: test_btreceiver.c $(ROOT)/src/system/bluetooth/btreceiver.c btrx-stubs/alsa/asoundlib.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Ibtrx-stubs $(CFLAGS) -o $@ test_btreceiver.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_btmetadata: test_btmetadata.c $(ROOT)/src/system/bluetooth/btstack.c $(ROOT)/src/system/bluetooth/dbuslite.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_btmetadata.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_btdiscovery: test_btdiscovery.c $(ROOT)/src/system/bluetooth/btstack.c $(ROOT)/src/system/bluetooth/dbuslite.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_btdiscovery.c $(LDFLAGS) $(LDLIBS)

# The AFH map, follow policy and actual HCI packet, using fake sockets.
.PHONY: btafh
btafh: $(BUILD)/test_btafh $(BUILD)/test_btafh_socket
	$(BUILD)/test_btafh
	$(BUILD)/test_btafh_socket

$(BUILD)/test_btafh: test_btafh.c $(ROOT)/src/system/bluetooth/btafh.c $(ROOT)/src/system/bluetooth/btafh.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_btafh.c $(ROOT)/src/system/bluetooth/btafh.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_btafh_socket: test_btafh_socket.c $(ROOT)/src/system/bluetooth/btafh.c \
                            $(ROOT)/src/system/bluetooth/btafh.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_btafh_socket.c $(LDFLAGS) $(LDLIBS)

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

# The music pet's arithmetic: how the mood moves with the listening, what
# idleness costs, when the goal counts, when the creature grows, and that its
# whole state survives in device_config.ini. Plain C, no LVGL and no audio.
PET_SRC = test_pet.c $(ROOT)/src/system/pet/pet.c $(ROOT)/src/system/core/config.c
.PHONY: pet
pet: $(BUILD)/test_pet
	$(BUILD)/test_pet

$(BUILD)/test_pet: $(PET_SRC) $(ROOT)/src/system/pet/pet.h $(ROOT)/src/system/core/config.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(PET_SRC) $(LDFLAGS) $(LDLIBS)

# The listening rules: audible seconds, album runs, focus and night, still with
# no audio hardware. pet.c comes along for the volume floor and the constants.
PETLISTEN_SRC = test_petlisten.c $(ROOT)/src/system/pet/petlisten.c $(ROOT)/src/system/pet/pet.c \
	$(ROOT)/src/system/core/config.c
.PHONY: petlisten
petlisten: $(BUILD)/test_petlisten
	$(BUILD)/test_petlisten

$(BUILD)/test_petlisten: $(PETLISTEN_SRC) $(ROOT)/src/system/pet/petlisten.h $(ROOT)/src/system/pet/pet.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(PETLISTEN_SRC) $(LDFLAGS) $(LDLIBS)

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
           $(ROOT)/src/system/tagmanager/tagwrite.c \
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
             $(ROOT)/src/system/tagmanager/tagwrite.c \
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

# Radio favourites and recents against the real store and SQLite, including
# URL-only stations and reopening after the database is replaced.
.PHONY: radio-store
radio-store: $(BUILD)/test_radio_store
	$(BUILD)/test_radio_store

$(BUILD)/test_radio_store: test_radio_store.c $(ROOT)/src/system/streaming/radio.c \
                           $(ROOT)/src/system/streaming/radio.h $(BUILD)/sqlite3_impl.o
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-format-truncation -o $@ test_radio_store.c $(BUILD)/sqlite3_impl.o $(LDFLAGS) $(LDLIBS)

# The HTTP stream header-read deadline: a server that accepts a connection but
# never finishes its header block must fail http_stream_open() at the deadline
# instead of holding the radio transport thread (and the UI thread joining it)
# for ever. Plain C, loopback only -- no SDL, ALSA or external network.
.PHONY: http-stream
http-stream: $(BUILD)/test_http_stream
	$(BUILD)/test_http_stream

$(BUILD)/test_http_stream: test_http_stream.c $(ROOT)/src/system/net/http.c \
                           $(ROOT)/src/system/net/tls.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_http_stream.c $(ROOT)/src/system/net/http.c $(ROOT)/src/system/net/tls.c $(ROOT)/src/system/core/config.c $(LDFLAGS) $(LDLIBS)

# The playback LED's stock PCM sample-rate bands and their color/pattern map.
.PHONY: led-rate
led-rate: $(BUILD)/test_led_rate
	$(BUILD)/test_led_rate

$(BUILD)/test_led_rate: test_led_rate.c $(ROOT)/src/system/device/led.c \
                        $(ROOT)/src/system/device/led.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_led_rate.c $(LDFLAGS) $(LDLIBS)

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

# The boot-time playback record: ordinary and CUE-virtual paths, the saved album
# queue across a real database reopen, and no automatic card worker with Detect
# changes off. Real library, CUE parser and SQLite; unrelated codecs are stubbed.
PLAYBACKSTATE_SRC = test_playbackstate.c $(ROOT)/src/system/library/library.c \
                    $(ROOT)/src/system/library/cue.c $(ROOT)/src/system/core/config.c \
                    $(ROOT)/src/system/core/utils.c
.PHONY: playback-state
playback-state: $(BUILD)/test_playbackstate
	$(BUILD)/test_playbackstate

$(BUILD)/test_playbackstate: $(PLAYBACKSTATE_SRC) $(BUILD)/sqlite3_impl.o \
                             $(ROOT)/src/system/library/library.h $(ROOT)/src/system/library/cue.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(PLAYBACKSTATE_SRC) $(BUILD)/sqlite3_impl.o $(LDFLAGS) $(LDLIBS)

# Heard podcast episodes against the real podcast.db: the marks themselves, the
# sorted set the binary search reads, what survives a restart, and a card whose
# database is deleted under the player. The card is a temporary directory; the
# only stub is utils.c's file identity, which is reproduced with stat().
.PHONY: podcastplayed
podcastplayed: $(BUILD)/test_podcastplayed
	$(BUILD)/test_podcastplayed

$(BUILD)/test_podcastplayed: test_podcastplayed.c $(ROOT)/src/system/streaming/podcastplayed.c \
                             $(ROOT)/src/system/streaming/podcastplayed.h $(BUILD)/sqlite3_impl.o
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_podcastplayed.c \
		$(ROOT)/src/system/streaming/podcastplayed.c $(BUILD)/sqlite3_impl.o $(LDFLAGS) $(LDLIBS)

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
         $(ROOT)/src/gui/shell/gridpage.c $(ROOT)/src/gui/pet/peticon.c \
         $(ROOT)/src/gui/settings/appearance.c \
         $(ROOT)/src/gui/settings/bootpicpage.c \
         $(ROOT)/src/gui/shell/corners.c $(ROOT)/src/gui/shell/settingsrow.c \
         $(ROOT)/src/gui/shell/theme.c \
         $(ROOT)/src/gui/shell/icons.c $(ROOT)/src/gui/shell/main_menu_icons.c \
         $(ROOT)/src/gui/shell/gestures.c \
         $(ROOT)/src/system/addons/addons.c \
         $(ROOT)/src/gui/board_profile.c \
         $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
UI_LVGL_SRC = $(shell find $(ROOT)/lvgl/src -name '*.c' 2>/dev/null)
UI_LVGL_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

# The real power/charger display transitions with fake sysfs and time. Covers
# saved 100% brightness, both wake paths, true-zero off and the PWM endpoint.
.PHONY: power-display
power-display: $(BUILD)/test_power_display
	$(BUILD)/test_power_display

$(BUILD)/test_power_display: test_power_display.c $(ROOT)/src/system/device/power.c \
                             $(ROOT)/src/system/device/power.h $(ROOT)/src/system/device/backlight.h \
                             $(ROOT)/src/gui/shell/chargescreen.c $(ROOT)/src/gui/shell/chargescreen.h $(UI_LVGL_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(UI_CPPFLAGS) $(CFLAGS) -o $@ test_power_display.c $(UI_LVGL_OBJ) $(LDFLAGS) $(LDLIBS)

# Curated order across API page boundaries, using a fake genre provider.
.PHONY: qobuzgenre-order
qobuzgenre-order: $(BUILD)/test_qobuzgenreorder
	$(BUILD)/test_qobuzgenreorder

$(BUILD)/test_qobuzgenreorder: test_qobuzgenreorder.c $(ROOT)/src/gui/streaming/qobuzgenreorder.c $(ROOT)/src/gui/streaming/qobuzgenreorder.h $(ROOT)/src/system/streaming/qobuz.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_qobuzgenreorder.c $(ROOT)/src/gui/streaming/qobuzgenreorder.c $(LDFLAGS) $(LDLIBS)

# The paged Qobuz genre picker at actual V1 dimensions.
QOBUZGENRE_UI_SRC = $(ROOT)/tests/ui/test_qobuzgenregrid.c \
                    $(ROOT)/src/gui/streaming/qobuzgenregrid.c \
                    $(ROOT)/src/gui/streaming/qobuzgenre_icons.c
QOBUZGENRE_UI_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(QOBUZGENRE_UI_SRC)) $(UI_LVGL_OBJ)
.PHONY: qobuzgenre-ui
qobuzgenre-ui: $(BUILD)/test_qobuzgenregrid
	$(BUILD)/test_qobuzgenregrid

$(BUILD)/test_qobuzgenregrid: $(QOBUZGENRE_UI_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Stock edge presses must not scroll the page beneath the sheet.
.PHONY: edge-lock
edge-lock: $(BUILD)/test_edge_lock
	$(BUILD)/test_edge_lock

$(BUILD)/test_edge_lock: test_edge_lock.c $(ROOT)/src/gui/shell/edge_lock.h $(UI_LVGL_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(ROOT)/lvgl \
		-DLV_CONF_PATH='"$(abspath ui/lv_conf.h)"' -o $@ test_edge_lock.c \
		$(UI_LVGL_OBJ) $(LDFLAGS) $(LDLIBS)

# Control Centre layout migration: the production parser and saved lists,
# without constructing the unrelated radio/streaming services.
.PHONY: quickpanel-layout
quickpanel-layout: $(BUILD)/test_quickpanel_layout
	$(BUILD)/test_quickpanel_layout

$(BUILD)/test_quickpanel_layout: test_quickpanel_layout.c $(ROOT)/src/gui/shell/quickpanel.c \
                                $(ROOT)/src/gui/shell/quickpanel.h $(ROOT)/src/gui/shell/edge_lock.h \
                                $(ROOT)/src/system/core/config.c $(UI_LVGL_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(ROOT)/lvgl \
		-DLV_CONF_PATH='"$(abspath ui/lv_conf.h)"' -o $@ test_quickpanel_layout.c \
		$(ROOT)/src/system/core/config.c $(ROOT)/src/system/addons/addons.c $(UI_LVGL_OBJ) $(LDFLAGS) $(LDLIBS)

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
            $(ROOT)/src/gui/games/games.c $(ROOT)/src/gui/games/snake.c \
            $(ROOT)/src/gui/games/game2048.c $(ROOT)/src/gui/games/dino.c \
            $(ROOT)/src/gui/notes/notes.c $(ROOT)/src/gui/notes/noteeditor.c \
            $(ROOT)/src/gui/tagmanager/tagmanager.c $(ROOT)/src/system/tagmanager/tagwrite.c \
            $(ROOT)/src/gui/metronome/metronome.c \
            $(ROOT)/src/gui/pet/petpage.c $(ROOT)/src/gui/pet/peticon.c $(ROOT)/src/gui/pet/pikachu.c \
            $(ROOT)/src/system/pet/pet.c \
            $(ROOT)/src/system/notes/notesstore.c $(ROOT)/src/system/text/textfile.c \
            $(ROOT)/src/gui/library/textview.c $(ROOT)/src/gui/library/pictureview.c \
            $(ROOT)/src/gui/shell/morepage.c $(ROOT)/src/gui/shell/gridpage.c \
            $(ROOT)/src/system/addons/addons.c \
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

# The three games in the More menu on the same renderer: built on first open,
# repainted when the palette moves while they are on screen, and caught up on
# the next show when it moved while they were not. The shell they hide is stubbed.
GAMES_SRC = $(ROOT)/tests/ui/test_games.c \
            $(ROOT)/src/gui/games/games.c $(ROOT)/src/gui/games/snake.c \
            $(ROOT)/src/gui/games/game2048.c $(ROOT)/src/gui/games/dino.c \
            $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
            $(ROOT)/src/gui/board_profile.c \
            $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
GAMES_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(GAMES_SRC)) \
            $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

# PEQ profile selection on the real page and DSP engine, with the shell and
# network AutoEq browser stubbed. No audio device or network is needed.
PEQPAGE_SRC = $(ROOT)/tests/ui/test_peq_profiles.c $(ROOT)/src/gui/audio/peqpage.c \
              $(ROOT)/src/system/audio/eq.c $(ROOT)/src/system/addons/addons.c \
              $(ROOT)/src/gui/shell/settingsrow.c \
              $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
              $(ROOT)/src/gui/board_profile.c $(ROOT)/src/system/core/config.c \
              $(ROOT)/src/system/core/lang.c
PEQPAGE_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(PEQPAGE_SRC)) \
              $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

# The rounded screen corners and their radius: the real masks drawn into a
# framebuffer the way the panel is driven (only what was invalidated is redrawn),
# so a corner that was not repainted when the arc moved is still there to find.
CORNERS_SRC = $(ROOT)/tests/ui/test_corners.c $(ROOT)/src/gui/shell/corners.c \
              $(ROOT)/src/gui/board_profile.c $(ROOT)/src/system/core/config.c
CORNERS_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(CORNERS_SRC)) \
              $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

# The V1 player deck's real flex layout, including switching to and from Waveform.
PLAYERDECK_SRC = $(ROOT)/tests/ui/test_playerdeck.c
PLAYERDECK_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(PLAYERDECK_SRC)) \
                 $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

.PHONY: playerdeck
playerdeck: $(BUILD)/test_playerdeck
	$(BUILD)/test_playerdeck

$(BUILD)/test_playerdeck: $(PLAYERDECK_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Vinyl sleeve/record geometry and real LVGL layering, without the audio shell.
VINYLLAYOUT_SRC = $(ROOT)/tests/ui/test_vinylayout.c $(ROOT)/src/gui/nowplaying/vinylayout.c
VINYLLAYOUT_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(VINYLLAYOUT_SRC)) \
                  $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

.PHONY: vinylayout
vinylayout: $(BUILD)/test_vinylayout
	$(BUILD)/test_vinylayout

$(BUILD)/test_vinylayout: $(VINYLLAYOUT_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Waveform artwork pills and favourite, including long scrolling labels.
WAVEOVERLAY_SRC = $(ROOT)/tests/ui/test_waveform_overlay.c $(ROOT)/src/gui/shell/scrolltext.c
WAVEOVERLAY_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(WAVEOVERLAY_SRC)) \
                  $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

.PHONY: waveform-overlay
waveform-overlay: $(BUILD)/test_waveform_overlay
	$(BUILD)/test_waveform_overlay

$(BUILD)/test_waveform_overlay: $(WAVEOVERLAY_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# The four hi-res artwork badge designs, including the embedded official logo.
HIRESBADGE_UI_SRC = $(ROOT)/tests/ui/test_hiresbadge.c $(ROOT)/src/gui/nowplaying/hiresbadgeview.c \
                    $(ROOT)/src/gui/shell/icons.c
HIRESBADGE_UI_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(HIRESBADGE_UI_SRC)) \
                    $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

.PHONY: hiresbadge-ui
hiresbadge-ui: $(BUILD)/test_hiresbadge_ui
	$(BUILD)/test_hiresbadge_ui

$(BUILD)/test_hiresbadge_ui: $(HIRESBADGE_UI_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# The now-playing page's play/pause button in its plain and legacy looks: the
# real disc drawn on a stand-in for the sleeve under every palette and accent.
# The player around it is not built; see src/gui/nowplaying/playbutton.h.
PLAYBUTTON_SRC = $(ROOT)/tests/ui/test_playbutton.c $(ROOT)/src/gui/nowplaying/playbutton.c \
                 $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
                 $(ROOT)/src/gui/board_profile.c $(ROOT)/src/system/core/config.c \
                 $(ROOT)/src/system/core/lang.c
PLAYBUTTON_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(PLAYBUTTON_SRC)) \
                 $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

# Music settings: the expandable DSD card must share the icon/title columns
# and midline of ordinary toggle/navigation rows; NOS has its own glyph.
MUSICROWS_SRC = $(ROOT)/tests/ui/test_musicsettings_rows.c \
                $(ROOT)/src/gui/shell/settingsrow.c $(ROOT)/src/gui/shell/theme.c \
                $(ROOT)/src/gui/shell/icons.c $(ROOT)/src/gui/board_profile.c \
                $(ROOT)/src/system/core/config.c $(ROOT)/src/system/core/lang.c
MUSICROWS_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(MUSICROWS_SRC)) \
                $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

.PHONY: musicsettings-rows
musicsettings-rows: $(BUILD)/test_musicsettings_rows
	$(BUILD)/test_musicsettings_rows ../../sonix-packer/assets/V1/usr/resource/sonix/language

$(BUILD)/test_musicsettings_rows: $(MUSICROWS_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# The volume pill with the level read as a voltage: the production overlay and
# reading on the V1 layout, with the audio side reduced to what the test sets.
# The label has to hold one width, and one line, over every reading.
VOLUMEOVERLAY_SRC = $(ROOT)/tests/ui/test_volume_overlay.c \
                    $(ROOT)/src/gui/shell/volume_overlay.c $(ROOT)/src/gui/shell/volumereadout.c \
                    $(ROOT)/src/gui/shell/theme.c $(ROOT)/src/gui/shell/icons.c \
                    $(ROOT)/src/gui/board_profile.c $(ROOT)/src/system/core/config.c
VOLUMEOVERLAY_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(VOLUMEOVERLAY_SRC)) \
                    $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

.PHONY: volume-overlay
volume-overlay: $(BUILD)/test_volume_overlay
	$(BUILD)/test_volume_overlay

$(BUILD)/test_volume_overlay: $(VOLUMEOVERLAY_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

STOCKKEYS_UI_SRC = $(ROOT)/tests/ui/test_stockkeys_ui.c \
                   $(ROOT)/src/gui/streaming/stockkeys_ui.c \
                   $(ROOT)/src/system/streaming/stockkeys.c \
                   $(ROOT)/src/system/streaming/streamkeys.c \
                   $(ROOT)/src/system/image/png_scaled.c \
                   $(ROOT)/src/system/addons/addons.c \
                   $(ROOT)/src/system/core/sha256.c \
                   $(ROOT)/src/system/core/utils.c \
                   $(ROOT)/src/gui/shell/settingsrow.c \
                   $(ROOT)/src/gui/shell/theme.c \
                   $(ROOT)/src/gui/shell/icons.c \
                   $(ROOT)/src/gui/board_profile.c \
                   $(ROOT)/src/system/core/config.c \
                   $(ROOT)/src/system/core/lang.c
STOCKKEYS_UI_OBJ = $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(STOCKKEYS_UI_SRC)) \
                   $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))

.PHONY: ui peq-ui corners playbutton stockkeys-ui
corners: $(BUILD)/test_corners
	$(BUILD)/test_corners

playbutton: $(BUILD)/test_playbutton
	$(BUILD)/test_playbutton ../../sonix-packer/assets/V1/usr/resource/sonix/language

peq-ui: $(BUILD)/test_peq_profiles
	$(BUILD)/test_peq_profiles ../../sonix-packer/assets/V1/usr/resource/sonix/language

stockkeys-ui: $(BUILD)/test_stockkeys_ui
	$(BUILD)/test_stockkeys_ui ../../sonix-packer/assets/V1/usr/resource/sonix/language

$(BUILD)/test_peq_profiles: $(PEQPAGE_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_stockkeys_ui: $(ROOT)/build_gen/streamkeys_key.h $(STOCKKEYS_UI_OBJ)
	$(CC) -o $@ $(STOCKKEYS_UI_OBJ) $(LDFLAGS) $(LDLIBS)

.PHONY: ui
ui: $(BUILD)/test_qobuzgenregrid $(BUILD)/test_main_menu $(BUILD)/test_screensaver $(BUILD)/test_tools_pages $(BUILD)/test_wifi_pages $(BUILD)/test_chargescreen $(BUILD)/test_power_display $(BUILD)/test_lyricsearch $(BUILD)/test_games $(BUILD)/test_peq_profiles $(BUILD)/test_quickpanel_layout $(BUILD)/test_corners $(BUILD)/test_playbutton $(BUILD)/test_musicsettings_rows $(BUILD)/test_volume_overlay $(BUILD)/test_playerdeck $(BUILD)/test_vinylayout $(BUILD)/test_waveform_overlay $(BUILD)/test_hiresbadge_ui $(BUILD)/test_statusbar_ui $(BUILD)/test_stockkeys_ui
	$(BUILD)/test_qobuzgenregrid
	$(BUILD)/test_main_menu ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_main_menu ../../sonix-packer/assets/V1/usr/resource/sonix/language saved
	$(BUILD)/test_screensaver ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_screensaver ../../sonix-packer/assets/V1/usr/resource/sonix/language saved
	$(BUILD)/test_tools_pages ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_wifi_pages ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_chargescreen ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_power_display
	$(BUILD)/test_lyricsearch ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_games ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_peq_profiles ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_quickpanel_layout
	$(BUILD)/test_corners
	$(BUILD)/test_playbutton ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_musicsettings_rows ../../sonix-packer/assets/V1/usr/resource/sonix/language
	$(BUILD)/test_volume_overlay
	$(BUILD)/test_playerdeck
	$(BUILD)/test_vinylayout
	$(BUILD)/test_waveform_overlay
	$(BUILD)/test_hiresbadge_ui
	$(BUILD)/test_statusbar_ui
	$(BUILD)/test_stockkeys_ui ../../sonix-packer/assets/V1/usr/resource/sonix/language

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

$(BUILD)/test_games: $(GAMES_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_corners: $(CORNERS_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_playbutton: $(PLAYBUTTON_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/ui/%.o: $(ROOT)/%.c ui/lv_conf.h $(ROOT)/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CPPFLAGS) $(UI_CFLAGS) -c $< -o $@

# Upstream LVGL is not maintained here; keep its warnings nonfatal.
$(BUILD)/ui/lvgl/%.o: $(ROOT)/lvgl/%.c ui/lv_conf.h $(ROOT)/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(UI_CPPFLAGS) $(filter-out -Werror,$(UI_CFLAGS)) -c $< -o $@

-include $(sort $(QOBUZGENRE_UI_OBJ:.o=.d) $(PEQPAGE_OBJ:.o=.d) $(UI_OBJ:.o=.d) $(SAVER_OBJ:.o=.d) $(CHARGE_OBJ:.o=.d) $(WIFIPAGE_OBJ:.o=.d) $(LYRICSEARCH_OBJ:.o=.d) $(GAMES_OBJ:.o=.d) $(CORNERS_OBJ:.o=.d) $(PLAYBUTTON_OBJ:.o=.d) $(MUSICROWS_OBJ:.o=.d) $(VOLUMEOVERLAY_OBJ:.o=.d) $(PLAYERDECK_OBJ:.o=.d) $(VINYLLAYOUT_OBJ:.o=.d) $(WAVEOVERLAY_OBJ:.o=.d) $(HIRESBADGE_UI_OBJ:.o=.d) $(STOCKKEYS_UI_OBJ:.o=.d))

# Smart Mix track-count limits and traversal, with a memory-only graph fixture.
GENIUS_SRC = test_genius.c $(ROOT)/src/system/library/genius.c
.PHONY: genius
genius: $(BUILD)/test_genius
	$(BUILD)/test_genius

$(BUILD)/test_genius: $(GENIUS_SRC) $(ROOT)/src/system/library/genius.h $(ROOT)/src/system/library/geniusdb.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(GENIUS_SRC) $(LDFLAGS) $(LDLIBS)

# The stock TempoTec release version and its separate Sonix metadata field.
.PHONY: basebuild
basebuild: $(BUILD)/test_sysinfo
	python3 test_base_build.py
	$(BUILD)/test_sysinfo

# prepare_v1_stock.sh finding v1.upt in a split archive (v1.zip.part1 + .part2,
# NAME.001 + NAME.002), a folder, a plain archive or a bare v1.upt, and refusing
# a set with a piece missing. Plain Python plus 7z; skipped without 7z.
.PHONY: stock-prepare
stock-prepare:
	python3 test_stock_prepare.py

$(BUILD)/test_sysinfo: test_sysinfo.c $(ROOT)/src/system/device/sysinfo.c $(ROOT)/src/system/device/sysinfo.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHOST_BUILD -o $@ test_sysinfo.c $(ROOT)/src/system/device/sysinfo.c $(LDFLAGS) $(LDLIBS)

# Lossless metadata rewrites and malformed input safety.
.PHONY: tagwrite
tagwrite:
	python3 test_tagwrite.py

.PHONY: artsearch
artsearch: $(BUILD)/test_artsearch
	$(BUILD)/test_artsearch

$(BUILD)/test_artsearch: test_artsearch.c $(ROOT)/src/system/tagmanager/artsearch.c $(ROOT)/src/system/core/json.c $(ROOT)/src/system/library/jpeginfo.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

.PHONY: tidal
tidal: $(BUILD)/test_tidal_login
	$(BUILD)/test_tidal_login

$(BUILD)/test_tidal_login: test_tidal_login.c $(ROOT)/src/system/streaming/tidal.c $(ROOT)/src/system/core/base64.c $(ROOT)/src/system/core/json.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)


.PHONY: browserstate
browserstate: $(BUILD)/test_browserstate
	$(BUILD)/test_browserstate

$(BUILD)/test_browserstate: test_browserstate.c $(ROOT)/src/gui/library/browserstate.c $(ROOT)/src/gui/library/browserstate.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_browserstate.c $(ROOT)/src/gui/library/browserstate.c $(LDFLAGS) $(LDLIBS)

.PHONY: hiresbadge
hiresbadge: $(BUILD)/test_hiresbadge
	$(BUILD)/test_hiresbadge

$(BUILD)/test_hiresbadge: test_hiresbadge.c $(ROOT)/src/gui/nowplaying/hiresbadge.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_hiresbadge.c

# The volume read as a voltage: the half-dB table, the three-figure text, and
# the reading at every index of both gain tables. Plain C, no LVGL. The tables
# and the DSD offsets are the real ones -- alsa-controls.c is compiled into the
# test against the declarations-only ALSA header. The one routine of it that is
# run against a mixer, the write of the two channel controls, gets the test's
# own recording one; the linker drops the rest.
.PHONY: volumereadout
volumereadout: $(BUILD)/test_volumereadout
	$(BUILD)/test_volumereadout

$(BUILD)/test_volumereadout: test_volumereadout.c $(ROOT)/src/gui/shell/volumereadout.c \
                             $(ROOT)/src/gui/shell/volumereadout.h \
                             $(ROOT)/src/system/audio/alsa-controls.c $(ROOT)/src/system/audio/alsa-controls.h \
                             stubs/alsa/asoundlib.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Istubs $(CFLAGS) -o $@ test_volumereadout.c $(ROOT)/src/gui/shell/volumereadout.c $(LDFLAGS) $(LDLIBS)

# Actual status-bar styles rendered with LVGL, including a large blurred image.
STATUSBAR_UI_OBJ = $(BUILD)/ui/tests/ui/test_statusbarstyle.o \
                   $(patsubst $(ROOT)/%.c,$(BUILD)/ui/%.o,$(UI_LVGL_SRC))
.PHONY: statusbar-ui
statusbar-ui: $(BUILD)/test_statusbar_ui
	$(BUILD)/test_statusbar_ui
$(BUILD)/test_statusbar_ui: $(STATUSBAR_UI_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)
-include $(BUILD)/ui/tests/ui/test_statusbarstyle.d
