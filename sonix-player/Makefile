# Makefile for sonix_player

# Target executables
HOST_BIN = sonix_player_host
WEB_EMU_BIN = sonix_player_web
WASM_JS = sonix_player_wasm.js
TARGET_BIN = sonix_player
# Built from launcher/, see "sonix_launch" below.
LAUNCH_BIN = sonix_launch

# The unstripped copy of the target binary, kept beside the stripped one.
#
# It is what a backtrace, addr2line or gdb need: the stripped binary that goes
# on the device carries no symbol table and no DWARF, so an address out of a
# crash log means nothing without this file. The two are the same build, so an
# address from one resolves in the other.
TARGET_DEBUG_BIN = sonix_player_debug

LVGL_DIR = $(CURDIR)/lvgl
ROCKBOX_TOOLCHAIN_DIR = $(CURDIR)/rockbox-toolchain

CC = gcc
CXX = g++
CROSS_CC ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-gcc
CROSS_CXX ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-g++

# The interface uses the per-flag object API (lv_obj_set_hidden() and the
# rest) and lv_text_ap_strlen(), neither of which exists before 9.6. It also
# includes some of LVGL's private headers (lv_timer_private.h,
# lv_event_private.h, lv_roller_private.h and others) from src/.
LVGL_VER = 9.6.0

# Self-bootstrap: clone LVGL if it doesn't exist yet before evaluating variables
ifeq ($(wildcard $(LVGL_DIR)),)
$(info Cloning LVGL v$(LVGL_VER)...)
$(shell git clone --depth 1 -b v$(LVGL_VER) https://github.com/lvgl/lvgl.git)
endif

# Read out of the headers, so a checkout with no git history still answers.
# All three places are tried: the macros sit in lvgl.h up to 9.1, in
# lv_version.h from 9.2 and in include/lvgl/lv_version.h from 9.6.
LVGL_VER_FOUND := $(shell sed -n 's/^\#define LVGL_VERSION_\(MAJOR\|MINOR\|PATCH\) *\([0-9]*\).*/\2/p' \
					$(LVGL_DIR)/include/lvgl/lv_version.h $(LVGL_DIR)/lv_version.h $(LVGL_DIR)/lvgl.h \
					2>/dev/null | head -3 | paste -sd. -)
ifneq ($(LVGL_VER_FOUND),)
ifneq ($(LVGL_VER_FOUND),$(LVGL_VER))
$(warning lvgl/ holds $(LVGL_VER_FOUND) and this player needs $(LVGL_VER).)
$(warning Remove the lvgl directory and run make again to fetch the right one.)
$(error wrong LVGL version)
endif
endif

# Compile flags.
#
#   -DLV_CONF_INCLUDE_SIMPLE=1  lets lv_conf.h be included as "lv_conf.h".
#   -MMD -MP                    write a .d per object, pulled back in at the
#                               bottom of this file so a header edit rebuilds.
#   -D_FILE_OFFSET_BITS=64      required on the 32-bit target: without it
#                               readdir()/stat() fail with EOVERFLOW on an inode
#                               or offset wider than 32 bits, which an exFAT
#                               card produces routinely.
#   -fno-builtin-printf         keeps printf("text\n") a call to printf. The
#   -fno-builtin-__printf_chk   compiler otherwise turns it into puts() (and
#                               a one-character printf into putchar()), and on
#                               the device's glibc 2.22 those two write to
#                               libc's own stdout object, not to the `stdout`
#                               variable logging.c points at the card log
#                               (glibc bug 24051, fixed in 2.29). Every
#                               constant message was lost that way: 125 call
#                               sites, among them the verdict on how the
#                               previous run ended. The second flag is the
#                               same for a toolchain that defines
#                               _FORTIFY_SOURCE, where printf is __printf_chk
#                               by the time it is folded. The price is that
#                               the compiler stops checking printf's format
#                               string by itself; glibc_compat.h declares the
#                               format attribute, which puts the check back.
#                               tests/ `make printf-flag` fails if any of
#                               this goes.
CFLAGS = -O3 -g -Wall -fno-builtin-printf -fno-builtin-__printf_chk -MMD -MP -D_FILE_OFFSET_BITS=64 -I. -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE=1 -DGB_CORE=1
# On the host the audio libraries are the system ones (libopusfile-dev,
# libwavpack-dev); on the target they are the ones this Makefile builds below.
# opusfile.h includes <opus_multistream.h> unprefixed, so the opus/ directory
# has to be among the -I in both cases.
HOST_CFLAGS = $(CFLAGS) -DHOST_BUILD=1 $(shell sdl2-config --cflags) $(shell pkg-config --cflags freetype2) \
			  $(shell pkg-config --cflags opusfile wavpack) \
			  -include src/system/core/glibc_compat.h
# -include glibc_compat.h must enter EVERY translation unit of the target
# build: it carries the .symver lines that keep five maths functions on the
# names the device's glibc 2.22 knows, and a .symver applies only to its own
# unit. Target only.
#
# -march=mips32r2: the toolchain is configured without --with-arch, and for the
# o32 ABI GCC then defaults to MIPS I. The X1600's core runs MIPS32r2 (the
# device's kernel modules and bluealsa are built for it), which gives mul, madd,
# conditional moves and no load delay slots.
#
# -mno-madd4: no madd.s/msub.s. Every float multiply and add stays a separate,
# separately rounded instruction, as in a MIPS I build.
#
# -mplt: the player is an executable at a fixed address, so its code need not
# be position-independent. Without this GCC compiles it as if it were a shared
# library: every function that calls another sets up $gp, every call loads its
# target from the GOT and jumps through a register, $gp is reloaded after each
# call, and every global is two loads. With it a call is a plain jal and a
# global is lui+lw; calls into the shared libraries go through a PLT the linker
# builds, and their data (stdout, stderr) is copied into the executable. The
# arithmetic is untouched: only how code and data are reached. Needs a dynamic
# linker with MIPS PLT support, which glibc has had since 2.9 (the device has
# 2.22). Ignored for anything built with -fPIC.
# This fork builds for the TempoTec V1 and nothing else: the board profile is
# the compiled one (src/gui/board_profile.c), host builds included, and there is
# no BOARD value that selects another panel.
TARGET_CFLAGS = $(CFLAGS) -march=mips32r2 -mno-madd4 -mplt -I$(LVGL_DIR)/src -I$(FREETYPE_PREFIX)/include/freetype2 \
				-I$(AUDIO_PREFIX)/include -I$(AUDIO_PREFIX)/include/opus \
				-include src/system/core/glibc_compat.h

# The C++ in this project is the Game Boy core (src/gb, Gearboy) plus the facade
# that makes it callable from the player, which is C.
#
#   -DGEARBOY_DISABLE_DISASSEMBLER  drops the disassembler and the breakpoints:
#       they are for Gearboy's own debugger, which is not here.
#   -fno-exceptions -fno-rtti       the core uses neither, and without them a
#       few tables and some dead code go away.
#   The -I are needed because Gearboy's sources include each other by short name
#       ("Video.h"), as if they were in their own directory -- which is how they
#       sit upstream, and they are not touched so that the next update does not
#       have to be merged by hand.
#   -Wno-attributes                 Gearboy declares InvalidOPCode() noinline
#       first and inline afterwards. Upstream code, left alone on purpose (see
#       src/gb/README.md).
GB_CFLAGS = -DGEARBOY_DISABLE_DISASSEMBLER -fno-exceptions -fno-rtti -Wno-attributes \
			-Isrc/gb/core -Isrc/gb/miniz
HOST_CXXFLAGS = $(HOST_CFLAGS) $(GB_CFLAGS)
TARGET_CXXFLAGS = $(TARGET_CFLAGS) $(GB_CFLAGS)

# Link flags
#
# -static-libstdc++ -static-libgcc are not optional on the target: the device
# has libstdc++.so.6.0.21, that is GLIBCXX_3.4.21 (the GCC 5 ABI), and the
# toolchain is GCC 9, which emits references up to 3.4.26. Linked dynamically
# the binary does not start at all.
#
# Target only. On the host they ask for a static libstdc++ that most
# distributions do not ship, and the link fails with "cannot find -lstdc++".
GB_LDFLAGS = -static-libstdc++ -static-libgcc
HOST_LDFLAGS = $(shell sdl2-config --libs) -lpthread -lm -ldl -lasound $(shell pkg-config --libs freetype2) \
			   $(shell pkg-config --libs opusfile wavpack)
TARGET_LDFLAGS = $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS) -lpthread -lm -ldl -lasound \
				 $(GB_LDFLAGS)

# FreeType for the target, built static: the rootfs has no libfreetype.so. The
# host uses the system one. The device's fonts are plain OTF/TTF, so zlib,
# bzip2, png, harfbuzz and brotli are all off.
FREETYPE_VER = 2.13.3
FREETYPE_TARBALL = freetype-$(FREETYPE_VER).tar.xz
FREETYPE_URL = https://download.savannah.gnu.org/releases/freetype/$(FREETYPE_TARBALL)
FREETYPE_URL_MIRROR = https://downloads.sourceforge.net/project/freetype/freetype2/$(FREETYPE_VER)/$(FREETYPE_TARBALL)
FREETYPE_SRC = $(CURDIR)/freetype-$(FREETYPE_VER)
FREETYPE_PREFIX = $(CURDIR)/freetype-target

# The audio libraries carried inside the binary, static like FreeType. The
# firmware's /usr/lib holds a libopus.so, but it is the raw codec with no Ogg
# demuxer, so linking against it is not an option.
#
#   libogg + libopus + opusfile -> .opus (src/system/decode/opusdec.c)
#   libwavpack                  -> .wv   (src/system/decode/wavpackdec.c)
#
# The configure options below were settled against these versions.
# ALAC is not here: its decoder is a single file under src/system/decode/alac/.
AUDIO_PREFIX = $(CURDIR)/audio-target

OGG_VER = 1.3.5
OGG_TARBALL = libogg-$(OGG_VER).tar.gz
OGG_URL = https://downloads.xiph.org/releases/ogg/$(OGG_TARBALL)
OGG_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/ogg/$(OGG_TARBALL)
OGG_SRC = $(CURDIR)/libogg-$(OGG_VER)
OGG_LIB = $(AUDIO_PREFIX)/lib/libogg.a

OPUS_VER = 1.4
OPUS_TARBALL = opus-$(OPUS_VER).tar.gz
OPUS_URL = https://downloads.xiph.org/releases/opus/$(OPUS_TARBALL)
OPUS_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/opus/$(OPUS_TARBALL)
OPUS_SRC = $(CURDIR)/opus-$(OPUS_VER)
OPUS_LIB = $(AUDIO_PREFIX)/lib/libopus.a

OPUSFILE_VER = 0.12
OPUSFILE_TARBALL = opusfile-$(OPUSFILE_VER).tar.gz
OPUSFILE_URL = https://downloads.xiph.org/releases/opus/$(OPUSFILE_TARBALL)
OPUSFILE_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/opus/$(OPUSFILE_TARBALL)
OPUSFILE_SRC = $(CURDIR)/opusfile-$(OPUSFILE_VER)
OPUSFILE_LIB = $(AUDIO_PREFIX)/lib/libopusfile.a

WAVPACK_VER = 5.6.0
WAVPACK_TARBALL = wavpack-$(WAVPACK_VER).tar.bz2
WAVPACK_URL = https://www.wavpack.com/$(WAVPACK_TARBALL)
WAVPACK_URL_MIRROR = https://github.com/dbry/WavPack/releases/download/$(WAVPACK_VER)/$(WAVPACK_TARBALL)
WAVPACK_SRC = $(CURDIR)/wavpack-$(WAVPACK_VER)
WAVPACK_LIB = $(AUDIO_PREFIX)/lib/libwavpack.a

# The order matters: with static libraries the linker resolves left to right,
# and opusfile calls opus which calls ogg.
AUDIO_LIBS = $(OPUSFILE_LIB) $(OPUS_LIB) $(OGG_LIB) $(WAVPACK_LIB)


# Source files. src/gb holds a .c as well (the miniz shell) alongside the C++
# core, so both searches reach into it.
APP_SRCS := $(shell find src -name '*.c')
LVGL_SRCS = $(shell find $(LVGL_DIR)/src -type f -name '*.c')

# The C++ here is the Game Boy core under src/gb/core plus its facade. Separate
# list because it takes different flags, and the link goes through the C++
# compiler so that libstdc++ comes along.
APP_CXX_SRCS := $(shell find src/gb -name '*.cpp')
HOST_LD = $(CXX)
TARGET_LD = $(CROSS_CXX)

# Object files
HOST_OBJS = $(APP_SRCS:src/%.c=build_host/%.o) $(APP_CXX_SRCS:src/%.cpp=build_host/%.opp) \
			$(LVGL_SRCS:$(LVGL_DIR)/%.c=build_host/lvgl/%.o)
WEB_EMU_BRIDGE_SRCS = webemu/webemu.c webemu/webemu_alsa.c
WEB_EMU_OBJS = $(APP_SRCS:src/%.c=build_web/%.o) $(APP_CXX_SRCS:src/%.cpp=build_web/%.opp) \
			   $(LVGL_SRCS:$(LVGL_DIR)/%.c=build_web/lvgl/%.o) \
			   $(WEB_EMU_BRIDGE_SRCS:webemu/%.c=build_web/webemu/%.o)
TARGET_OBJS = $(APP_SRCS:src/%.c=build_target/%.o) $(APP_CXX_SRCS:src/%.cpp=build_target/%.opp) \
			  $(LVGL_SRCS:$(LVGL_DIR)/%.c=build_target/lvgl/%.o)

# Browser-based TempoTec V1 emulator (headless LVGL framebuffer + virtual ALSA
# PCM stream, zero SDL2 dependency). Uses system FreeType/Opus/WavPack when
# available, or bootstraps static FreeType from GitHub automatically.
WEB_HAVE_FT := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --exists freetype2 && echo yes)
WEB_HAVE_OPUS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --exists opusfile && echo yes)
WEB_HAVE_WV := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --exists wavpack && echo yes)

FREETYPE_HOST_SRC = $(CURDIR)/freetype-host-src
FREETYPE_HOST_LIB = $(FREETYPE_HOST_SRC)/objs/libfreetype.a

ifeq ($(WEB_HAVE_FT),yes)
WEB_FT_CFLAGS := $(shell pkg-config --cflags freetype2)
WEB_FT_LIBS := $(shell pkg-config --libs freetype2)
WEB_FT_DEP :=
else
WEB_FT_CFLAGS := -I$(FREETYPE_HOST_SRC)/include
WEB_FT_LIBS := $(FREETYPE_HOST_LIB)
WEB_FT_DEP := $(FREETYPE_HOST_LIB)
endif

ifeq ($(WEB_HAVE_OPUS),yes)
WEB_OPUS_CFLAGS := $(shell pkg-config --cflags opusfile)
WEB_OPUS_LIBS := $(shell pkg-config --libs opusfile)
else
WEB_OPUS_CFLAGS := -DSONIX_NO_OPUSFILE=1
WEB_OPUS_LIBS :=
endif

ifeq ($(WEB_HAVE_WV),yes)
WEB_WV_CFLAGS := $(shell pkg-config --cflags wavpack)
WEB_WV_LIBS := $(shell pkg-config --libs wavpack)
else
WEB_WV_CFLAGS := -DSONIX_NO_WAVPACK=1
WEB_WV_LIBS :=
endif

WEB_EMU_CFLAGS = $(CFLAGS) -DHOST_BUILD=1 -DHOST_WEB_EMU=1 -Iwebemu $(WEB_FT_CFLAGS) $(WEB_OPUS_CFLAGS) $(WEB_WV_CFLAGS) \
				 -include src/system/core/glibc_compat.h
WEB_EMU_CXXFLAGS = $(WEB_EMU_CFLAGS) $(GB_CFLAGS)
WEB_EMU_LDFLAGS = $(WEB_FT_LIBS) $(WEB_OPUS_LIBS) $(WEB_WV_LIBS) -lpthread -lm -ldl

# Emscripten WebAssembly build for static browser / GitHub Pages hosting.
EMCC ?= emcc
EMCXX ?= em++
EMAR ?= emar

WASM_BRIDGE_SRCS = webemu/webemu_wasm.c webemu/webemu_alsa.c
WASM_OBJS = $(APP_SRCS:src/%.c=build_wasm/%.o) $(APP_CXX_SRCS:src/%.cpp=build_wasm/%.opp) \
			$(LVGL_SRCS:$(LVGL_DIR)/%.c=build_wasm/lvgl/%.o) \
			$(WASM_BRIDGE_SRCS:webemu/%.c=build_wasm/webemu/%.o)

WASM_FT_REL_SRCS = \
	src/base/ftsystem.c src/base/ftdebug.c src/base/ftinit.c src/base/ftbase.c \
	src/base/ftbbox.c src/base/ftbdf.c src/base/ftbitmap.c src/base/ftcid.c \
	src/base/ftfstype.c src/base/ftgasp.c src/base/ftglyph.c src/base/ftgxval.c \
	src/base/ftmm.c src/base/ftotval.c src/base/ftpatent.c src/base/ftpfr.c \
	src/base/ftstroke.c src/base/ftsynth.c src/base/fttype1.c src/base/ftwinfnt.c \
	src/truetype/truetype.c src/type1/type1.c src/cff/cff.c src/cid/type1cid.c \
	src/pfr/pfr.c src/type42/type42.c src/winfonts/winfnt.c src/pcf/pcf.c \
	src/bdf/bdf.c src/sfnt/sfnt.c src/autofit/autofit.c src/pshinter/pshinter.c \
	src/smooth/smooth.c src/raster/raster.c src/svg/svg.c src/sdf/sdf.c \
	src/cache/ftcache.c src/gzip/ftgzip.c src/lzw/ftlzw.c src/bzip2/ftbzip2.c \
	src/psaux/psaux.c src/psnames/psnames.c
WASM_FT_OBJS = $(WASM_FT_REL_SRCS:src/%.c=build_wasm/freetype/%.o)
WASM_FT_LIB = build_wasm/freetype/libfreetype.a
WASM_FT_STAMP = $(FREETYPE_HOST_SRC)/include/ft2build.h

# Self-bootstrap FreeType source tree before evaluating WASM_FT_OBJS prerequisites
ifneq ($(filter web-wasm web-wasm-bundle,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(WASM_FT_STAMP)),)
$(info Cloning FreeType VER-2-13-3 for WebAssembly build...)
$(shell git clone --depth 1 -b VER-2-13-3 https://github.com/freetype/freetype.git "$(FREETYPE_HOST_SRC)")
endif
endif

WASM_CFLAGS = -O2 -pthread -MMD -MP -D_FILE_OFFSET_BITS=64 -DNAME_MAX=255 -I. -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE=1 -DGB_CORE=1 \
			  -DHOST_BUILD=1 -DHOST_WEB_EMU=1 -DSONIX_NO_OPUSFILE=1 -DSONIX_NO_WAVPACK=1 \
			  -Iwebemu/wasm_compat -Iwebemu -I$(FREETYPE_HOST_SRC)/include \
			  -Wno-unused-function -Wno-format-truncation -Wno-unknown-warning-option
WASM_CXXFLAGS = $(WASM_CFLAGS) $(GB_CFLAGS)
WASM_FT_CFLAGS = -O2 -pthread -DFT2_BUILD_LIBRARY -I$(FREETYPE_HOST_SRC)/include -I$(FREETYPE_HOST_SRC)/src
WASM_LDFLAGS = -O2 -pthread $(WASM_FT_LIB) -Wno-pthreads-mem-growth \
			   -sMODULARIZE=1 -sEXPORT_NAME=createSonixPlayer \
			   -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=8 \
			   -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=100663296 -sMAXIMUM_MEMORY=268435456 \
			   -sSTACK_SIZE=2097152 -sDEFAULT_PTHREAD_STACK_SIZE=524288 \
			   -sEXIT_RUNTIME=1 -sFORCE_FILESYSTEM=1 \
			   -sEXPORTED_FUNCTIONS="['_main','_malloc','_free','_webemu_wasm_pointer','_webemu_wasm_key','_webemu_wasm_screenshot','_webemu_wasm_quit']" \
			   -sEXPORTED_RUNTIME_METHODS="['FS','HEAPU8','PThread','UTF8ToString','stringToUTF8','ccall','cwrap','ENV']" \
			   -sINCOMING_MODULE_JS_API="['arguments','preRun','postRun','print','printErr','locateFile','wasmBinary','mainScriptUrlOrBlob','onExit','onAbort','noInitialRun']" \
			   -lm

.PHONY: all host web-emu run-web-emu web-wasm web-wasm-bundle target clean compile_commands.json

all: host compile_commands.json

host: $(HOST_BIN) compile_commands.json

$(HOST_BIN): $(HOST_OBJS)
	$(HOST_LD) -o $@ $(HOST_OBJS) $(HOST_LDFLAGS)
	@echo "Host build complete: Run './$(HOST_BIN)' to start the simulator."

$(FREETYPE_HOST_LIB):
	@if [ ! -d "$(FREETYPE_HOST_SRC)" ]; then \
		echo "Cloning FreeType VER-2-13-3 for host emulator..."; \
		git clone --depth 1 -b VER-2-13-3 https://github.com/freetype/freetype.git "$(FREETYPE_HOST_SRC)"; \
	fi
	@echo "Building static FreeType for host emulator..."
	@$(MAKE) -C "$(FREETYPE_HOST_SRC)" setup ansi
	@$(MAKE) -C "$(FREETYPE_HOST_SRC)" -j$$(nproc)

web-emu: $(WEB_EMU_BIN)

$(WEB_EMU_OBJS): $(WEB_FT_DEP)

$(WEB_EMU_BIN): $(WEB_EMU_OBJS) $(WEB_FT_DEP)
	$(HOST_LD) -o $@ $(WEB_EMU_OBJS) $(WEB_EMU_LDFLAGS)
	@echo "Web emulator binary built: $(WEB_EMU_BIN)"

run-web-emu:
	@node ../tools/v1-emulator/server.mjs

$(WASM_FT_STAMP):
	@if [ ! -d "$(FREETYPE_HOST_SRC)" ]; then \
		echo "Cloning FreeType VER-2-13-3 for WebAssembly build..."; \
		git clone --depth 1 -b VER-2-13-3 https://github.com/freetype/freetype.git "$(FREETYPE_HOST_SRC)"; \
	fi

build_wasm/freetype/%.o: $(FREETYPE_HOST_SRC)/src/%.c | $(WASM_FT_STAMP)
	@mkdir -p $(dir $@)
	$(EMCC) $(WASM_FT_CFLAGS) -c $< -o $@

$(WASM_FT_LIB): $(WASM_FT_OBJS)
	@mkdir -p $(dir $@)
	$(EMAR) rcs $@ $(WASM_FT_OBJS)

web-wasm: $(WASM_JS)

$(WASM_OBJS): | $(WASM_FT_STAMP)

$(WASM_JS): $(WASM_OBJS) $(WASM_FT_LIB)
	$(EMCXX) -o $@ $(WASM_OBJS) $(WASM_LDFLAGS)
	@echo "WebAssembly build complete: $(WASM_JS) + sonix_player_wasm.wasm"

web-wasm-bundle: web-wasm
	@node ../tools/v1-emulator/pack-wasm-bundle.mjs

# The key that opens streaming-keys.bin (src/system/streaming/streamkeys.h),
# kept outside git and compiled into streamkeys.c. The header is rewritten only
# when the key changes. Without the key file the build still works and the
# player reads only a plain streaming-keys.ini.
STREAMKEYS_KEY ?= streaming-keys.key
STREAMKEYS_KEY_H = build_gen/streamkeys_key.h
$(STREAMKEYS_KEY_H): FORCE
	@mkdir -p $(dir $@)
	@{ echo '// Generated by the Makefile from $(STREAMKEYS_KEY). Not in git.'; \
	  if [ -f "$(STREAMKEYS_KEY)" ]; then \
		hex=$$(tr -dc '0-9a-fA-F' < "$(STREAMKEYS_KEY)"); \
		[ $${#hex} -eq 64 ] || { echo "$(STREAMKEYS_KEY) is not 32 bytes of hex" >&2; exit 1; }; \
		echo '#define STREAMKEYS_KEY_LEN 32'; \
		echo "static const unsigned char streamkeys_key[STREAMKEYS_KEY_LEN] = {$$(echo $$hex | sed 's/../0x&,/g')};"; \
	  else \
		echo '#define STREAMKEYS_KEY_LEN 0'; \
	  fi; } > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; \
		if [ -f "$(STREAMKEYS_KEY)" ]; then echo "streamkeys: key from $(STREAMKEYS_KEY)"; \
		else echo "streamkeys: no $(STREAMKEYS_KEY), this build reads only a plain streaming-keys.ini"; fi; fi
build_host/system/streaming/streamkeys.o build_web/system/streaming/streamkeys.o build_wasm/system/streaming/streamkeys.o build_target/system/streaming/streamkeys.o: $(STREAMKEYS_KEY_H)

build_host/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

build_host/%.opp: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_CXXFLAGS) -c $< -o $@

build_host/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

build_web/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(WEB_EMU_CFLAGS) -c $< -o $@

build_web/%.opp: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(WEB_EMU_CXXFLAGS) -c $< -o $@

build_web/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(WEB_EMU_CFLAGS) -c $< -o $@

build_web/webemu/%.o: webemu/%.c
	@mkdir -p $(dir $@)
	$(CC) $(WEB_EMU_CFLAGS) -c $< -o $@

build_wasm/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(EMCC) $(WASM_CFLAGS) -c $< -o $@

build_wasm/%.opp: src/%.cpp
	@mkdir -p $(dir $@)
	$(EMCXX) $(WASM_CXXFLAGS) -c $< -o $@

build_wasm/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(EMCC) $(WASM_CFLAGS) -c $< -o $@

build_wasm/webemu/%.o: webemu/%.c
	@mkdir -p $(dir $@)
	$(EMCC) $(WASM_CFLAGS) -c $< -o $@

target: $(TARGET_BIN) $(LAUNCH_BIN) compile_commands.json

$(TARGET_BIN): $(TARGET_OBJS)
	$(TARGET_LD) -o $@ $(TARGET_OBJS) $(TARGET_LDFLAGS)
	@$(MAKE) --no-print-directory check-abi
	@$(MAKE) --no-print-directory strip-target
	@echo "Target build complete: File ready at '$(TARGET_BIN)'"

# ---------------------------------------------------------------------------
# sonix_launch
#
# The process sonix_player.sh hands itself over to: it starts the player and
# reboots the device when the player exits (launcher/sonix_launch.c). No libc
# and no loader, so it is built on its own: static, freestanding, non-PIC, and
# with the loop idioms GCC would turn into memset or strlen calls left as loops.
# The packer takes it from next to itself, like sonix_player.
# ---------------------------------------------------------------------------

LAUNCH_CFLAGS = -Os -Wall -Wextra -march=mips32r2 -mno-abicalls -fno-pic -G0 -static -nostdlib \
				-ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector \
				-fno-asynchronous-unwind-tables -Wl,-e,__start

$(LAUNCH_BIN): launcher/sonix_launch.c $(CROSS_CC)
	$(CROSS_CC) $(LAUNCH_CFLAGS) -o $@ $<
	@if [ -x "$(CROSS_STRIP)" ]; then $(CROSS_STRIP) $@; fi
	@echo "sonix_launch: $$(wc -c < $@) bytes"

# ---------------------------------------------------------------------------
# Splitting the link output in two
#
# Runs after check-abi, which deletes both files when it fails: there is then
# nothing to copy, and no debug binary left claiming to match a build that was
# refused.
#
# The cross strip and not the host one. /usr/bin/strip is built for x86-64 and
# answers "file format not recognized" on a MIPS binary -- and it says so on
# stderr while exiting non-zero, which would take the whole build down.
# ---------------------------------------------------------------------------

CROSS_STRIP ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-strip

.PHONY: strip-target
strip-target:
	@if [ ! -f "$(TARGET_BIN)" ]; then \
		exit 0; \
	fi; \
	if [ ! -x "$(CROSS_STRIP)" ]; then \
		echo "strip-target: $(CROSS_STRIP) is not there, '$(TARGET_BIN)' left unstripped"; \
		exit 0; \
	fi; \
	cp -f $(TARGET_BIN) $(TARGET_DEBUG_BIN); \
	$(CROSS_STRIP) $(TARGET_BIN); \
	echo "strip-target: symbols in '$(TARGET_DEBUG_BIN)' ($$(du -h $(TARGET_DEBUG_BIN) | cut -f1)), stripped '$(TARGET_BIN)' ($$(du -h $(TARGET_BIN) | cut -f1))"

# ---------------------------------------------------------------------------
# The check the linker does not do
#
# The toolchain compiles against glibc 2.27, the device has 2.22. A binary
# asking for a newer versioned symbol links without a word and then refuses to
# start, and on this device that means a boot loop: sonix_launch runs
# `sleep 1; reboot` as soon as the player exits.
#
# glibc_compat.h holds the known functions down; this catches the next ones.
# ---------------------------------------------------------------------------

CROSS_READELF ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-readelf

# The device's glibc, as major.minor.
DEVICE_GLIBC_MAJOR = 2
DEVICE_GLIBC_MINOR = 22

.PHONY: check-abi
check-abi:
	@if [ ! -x "$(CROSS_READELF)" ]; then \
		echo "check-abi: $(CROSS_READELF) is not there, check skipped"; \
		exit 0; \
	fi; \
	bad=$$($(CROSS_READELF) --dyn-syms -W $(TARGET_BIN) 2>/dev/null \
		| awk '{ for (i = 1; i <= NF; i++) if (match($$i, /@GLIBC_[0-9.]+$$/)) { \
			ver = substr($$i, RSTART + 7); n = split(ver, v, "."); \
			if (v[1] > $(DEVICE_GLIBC_MAJOR) || (v[1] == $(DEVICE_GLIBC_MAJOR) && v[2] > $(DEVICE_GLIBC_MINOR))) print "  " $$i; \
		} }' | sort -u); \
	if [ -n "$$bad" ]; then \
		echo ""; \
		echo "*** ERROR: the binary asks for symbols the device does not have."; \
		echo "*** The device has glibc $(DEVICE_GLIBC_MAJOR).$(DEVICE_GLIBC_MINOR); these want more:"; \
		echo "$$bad"; \
		echo ""; \
		echo "*** As it stands the binary DOES NOT START, and the device boot loops."; \
		echo "*** Add a .symver in src/system/core/glibc_compat.h, which explains how."; \
		echo ""; \
		rm -f $(TARGET_BIN) $(TARGET_DEBUG_BIN); \
		exit 1; \
	fi; \
	echo "check-abi: no symbol beyond glibc $(DEVICE_GLIBC_MAJOR).$(DEVICE_GLIBC_MINOR)."

# The flags every target object was compiled with. Rewritten only when they
# differ, so a change of flags rebuilds everything and nothing else does.
TARGET_FLAGS_STAMP = build_target/.flags
$(TARGET_FLAGS_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo '$(TARGET_CFLAGS) | $(TARGET_CXXFLAGS)' | cmp -s - $@ || \
		echo '$(TARGET_CFLAGS) | $(TARGET_CXXFLAGS)' > $@
FORCE:

build_target/%.o: src/%.c $(TARGET_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

build_target/%.opp: src/%.cpp $(TARGET_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CXX) $(TARGET_CXXFLAGS) -c $< -o $@

build_target/lvgl/%.o: $(LVGL_DIR)/%.c $(TARGET_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

# For the Zed/clangd LSP: autofill and hover popups.
compile_commands.json:
	@python3 generate_compile_commands.py

clean:
	rm -rf build_host build_web build_wasm build_target build_gen $(HOST_BIN) $(WEB_EMU_BIN) sonix_player_wasm.js sonix_player_wasm.wasm sonix_player_wasm.worker.js $(TARGET_BIN) $(TARGET_DEBUG_BIN) $(LAUNCH_BIN) compile_commands.json compile_flags.txt

# The bundled libraries and their extracted sources. The downloaded tarballs stay.
.PHONY: libs-clean
libs-clean:
	rm -rf $(FREETYPE_PREFIX) $(AUDIO_PREFIX) $(FREETYPE_SRC) $(OGG_SRC) $(OPUS_SRC) $(OPUSFILE_SRC) $(WAVPACK_SRC)

# Make sure the cross compiler is built before trying to make any of the target
# objects with it.
$(TARGET_OBJS): $(CROSS_CC) $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS)
$(TARGET_BIN): $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS)

$(CROSS_CC):
	@echo "Building Rockbox toolchain..."
	RBDEV_SONIX_MINIMAL=1 ./rockboxdev/rockboxdev.sh

# The flags FreeType and the audio libraries are built with. -march and -mplt as
# in TARGET_CFLAGS: they are static, so they end up in the same executable. They
# are built once: `make libs-clean` removes them, with their extracted sources,
# so the next `make target` builds them again.
LIB_CFLAGS = -O2 -march=mips32r2 -mplt

# Cross-build FreeType, once. configure needs the toolchain on PATH for
# ranlib/ar to be found under their host-triplet names.
$(FREETYPE_PREFIX)/lib/libfreetype.a: $(CROSS_CC)
	@echo "Building static FreeType $(FREETYPE_VER) for the target..."
	@if [ ! -f $(FREETYPE_TARBALL) ]; then \
		wget -O $(FREETYPE_TARBALL) $(FREETYPE_URL) || \
		wget -O $(FREETYPE_TARBALL) $(FREETYPE_URL_MIRROR); \
	fi
	rm -rf $(FREETYPE_SRC) && tar xf $(FREETYPE_TARBALL)
	cd $(FREETYPE_SRC) && PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" ./configure \
		--host=mipsel-rockbox-linux-gnu \
		--prefix=$(FREETYPE_PREFIX) \
		--disable-shared --enable-static \
		--with-zlib=no --with-bzip2=no --with-png=no \
		--with-harfbuzz=no --with-brotli=no \
		CC=$(CROSS_CC) CFLAGS="$(LIB_CFLAGS)" \
		AR=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ar \
		RANLIB=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ranlib
	PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) -C $(FREETYPE_SRC)
	PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) -C $(FREETYPE_SRC) install

# ---------------------------------------------------------------------------
# Downloaded, configured with the toolchain on PATH (configure looks for ar and
# ranlib under the triplet name) and installed into AUDIO_PREFIX. The .a is the
# make target, so each is built once.
# ---------------------------------------------------------------------------

CROSS_PATH = PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH"
CROSS_TOOLS = CC=$(CROSS_CC) CFLAGS="$(LIB_CFLAGS)" \
			  AR=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ar \
			  RANLIB=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ranlib
CROSS_COMMON = --host=mipsel-rockbox-linux-gnu --prefix=$(AUDIO_PREFIX) --disable-shared --enable-static

$(OGG_LIB): $(CROSS_CC)
	@echo "Building static libogg $(OGG_VER) for the target..."
	@if [ ! -f $(OGG_TARBALL) ]; then \
		wget -O $(OGG_TARBALL) $(OGG_URL) || wget -O $(OGG_TARBALL) $(OGG_URL_MIRROR); \
	fi
	rm -rf $(OGG_SRC) && tar xf $(OGG_TARBALL)
	cd $(OGG_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) $(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OGG_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OGG_SRC) install

# --enable-fixed-point: integer decoder, bit-exact across platforms. The float
# API stays compiled in -- removing it would take --disable-float-api.
$(OPUS_LIB): $(CROSS_CC)
	@echo "Building static libopus $(OPUS_VER) for the target..."
	@if [ ! -f $(OPUS_TARBALL) ]; then \
		wget -O $(OPUS_TARBALL) $(OPUS_URL) || wget -O $(OPUS_TARBALL) $(OPUS_URL_MIRROR); \
	fi
	rm -rf $(OPUS_SRC) && tar xf $(OPUS_TARBALL)
	cd $(OPUS_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--enable-fixed-point --disable-doc --disable-extra-programs \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OPUS_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OPUS_SRC) install

# DEPS_CFLAGS/DEPS_LIBS by hand: opusfile's configure finds ogg and opus with
# PKG_CHECK_MODULES, and no pkg-config here knows the cross sysroot. Passing
# them skips the search. --disable-http drops the stream support that would
# pull in OpenSSL.
$(OPUSFILE_LIB): $(CROSS_CC) $(OGG_LIB) $(OPUS_LIB)
	@echo "Building static opusfile $(OPUSFILE_VER) for the target..."
	@if [ ! -f $(OPUSFILE_TARBALL) ]; then \
		wget -O $(OPUSFILE_TARBALL) $(OPUSFILE_URL) || wget -O $(OPUSFILE_TARBALL) $(OPUSFILE_URL_MIRROR); \
	fi
	rm -rf $(OPUSFILE_SRC) && tar xf $(OPUSFILE_TARBALL)
	cd $(OPUSFILE_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--disable-http --disable-examples --disable-doc \
		DEPS_CFLAGS="-I$(AUDIO_PREFIX)/include -I$(AUDIO_PREFIX)/include/opus" \
		DEPS_LIBS="-L$(AUDIO_PREFIX)/lib -lopus -logg" \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OPUSFILE_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OPUSFILE_SRC) install

# --enable-legacy: also reads the .wv written by WavPack 4.0 and earlier, which
# costs a few kilobytes. --enable-dsd keeps DSD inside WavPack, which the
# wrapper opens with OPEN_DSD_AS_PCM. --disable-asm because the assembly wavpack
# ships is for x86 and ARM.
$(WAVPACK_LIB): $(CROSS_CC)
	@echo "Building static libwavpack $(WAVPACK_VER) for the target..."
	@if [ ! -f $(WAVPACK_TARBALL) ]; then \
		wget -O $(WAVPACK_TARBALL) $(WAVPACK_URL) || wget -O $(WAVPACK_TARBALL) $(WAVPACK_URL_MIRROR); \
	fi
	rm -rf $(WAVPACK_SRC) && tar xf $(WAVPACK_TARBALL)
	cd $(WAVPACK_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--disable-apps --disable-man --disable-asm \
		--enable-dsd --enable-legacy --without-libiconv-prefix \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(WAVPACK_SRC)
	$(CROSS_PATH) $(MAKE) -C $(WAVPACK_SRC) install

# Header dependencies recorded by -MMD (see CFLAGS). The dashes keep make quiet
# on the first build, when no .d files exist yet. The C++ objects end in .opp
# so the two compile rules cannot overlap, which is why both suffixes are
# substituted -- otherwise make reads an object file as a makefile.
-include $(patsubst %.opp,%.d,$(patsubst %.o,%.d,$(HOST_OBJS)))
-include $(patsubst %.opp,%.d,$(patsubst %.o,%.d,$(WEB_EMU_OBJS)))
-include $(patsubst %.opp,%.d,$(patsubst %.o,%.d,$(WASM_OBJS)))
-include $(patsubst %.opp,%.d,$(patsubst %.o,%.d,$(TARGET_OBJS)))
