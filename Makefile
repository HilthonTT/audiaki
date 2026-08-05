# audiaki - ALSA capture to WAV
# SPDX-License-Identifier: MIT
#
# Common targets:
#   make            build build/audiaki
#   make debug      build with -O0, debug info and ASan/UBSan
#   make test       build and run the unit tests (no ALSA required)
#   make check      tests plus a clang-format style check
#   make install    install into $(PREFIX) (default /usr/local)

PROJECT   := audiaki
VERSION   := $(shell sed -n 's/^\#define AUDIAKI_VERSION "\(.*\)".*/\1/p' src/version.h)

CC        ?= cc
PKG_CONFIG ?= pkg-config
INSTALL   ?= install
CLANG_FORMAT ?= clang-format

DESTDIR   ?=
PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
MANDIR    ?= $(PREFIX)/share/man/man1

BUILD_DIR ?= build
OBJ_DIR   := $(BUILD_DIR)/obj
TEST_DIR  := $(BUILD_DIR)/tests
BIN       := $(BUILD_DIR)/$(PROJECT)

# -- flags -------------------------------------------------------------------

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wpointer-arith \
            -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition \
            -Wredundant-decls -Wvla -Wformat=2

OPTFLAGS  ?= -O2
SANITIZE  ?=
# STRICT=1 turns warnings into errors; CI builds with it.
ifeq ($(STRICT),1)
WARNINGS += -Werror
endif

CPPFLAGS  += -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS    ?= $(OPTFLAGS) -g
CFLAGS    += -std=c11 $(WARNINGS) $(SANITIZE) -MMD -MP
LDFLAGS   += $(SANITIZE)

ALSA_CFLAGS := $(shell $(PKG_CONFIG) --cflags alsa 2>/dev/null)
ALSA_LIBS   := $(shell $(PKG_CONFIG) --libs alsa 2>/dev/null || echo -lasound)
ifeq ($(strip $(ALSA_LIBS)),)
ALSA_LIBS := -lasound
endif

LDLIBS    += $(ALSA_LIBS) -lm

# -- sources -----------------------------------------------------------------

SRCS      := $(sort $(wildcard src/*.c))
OBJS      := $(SRCS:src/%.c=$(OBJ_DIR)/%.o)

# Objects that do not touch libasound. Tests link against these so the suite
# runs on machines without ALSA headers.
PORTABLE_SRCS := src/format.c src/wav.c src/parse.c src/log.c src/fft.c \
                 src/spectrum.c src/canvas.c src/jsonout.c src/info.c \
                 src/take.c src/ringbuf.c
PORTABLE_OBJS := $(PORTABLE_SRCS:src/%.c=$(OBJ_DIR)/%.o)

TEST_SRCS := $(sort $(wildcard tests/test_*.c))
TEST_BINS := $(TEST_SRCS:tests/%.c=$(TEST_DIR)/%)

# -- desktop app -------------------------------------------------------------

# raylib is a submodule rather than a system package: it is not in Debian or
# Ubuntu, and pinning the version we drew the visualiser against beats asking
# every contributor to build the right one by hand.
RAYLIB_DIR := vendor/raylib
RAYLIB_SRC := $(RAYLIB_DIR)/src
RAYLIB_LIB := $(RAYLIB_SRC)/libraylib.a
RAYLIB_PLATFORM ?= PLATFORM_DESKTOP

GUI_BIN   := $(BUILD_DIR)/$(PROJECT)-gui
GUI_SRCS  := $(sort $(wildcard src/gui/*.c))
GUI_OBJS  := $(GUI_SRCS:src/gui/%.c=$(OBJ_DIR)/gui/%.o)

# The core the window shares with the CLI. Not the whole of src/: the GUI has
# no use for the terminal meter, the argument parser or the ffmpeg pipe.
# jsonout is here only because device.c's --list and --probe reference it; the
# window never calls either.
GUI_CORE_SRCS := src/device.c src/format.c src/wav.c src/log.c src/fft.c \
                 src/spectrum.c src/monitor.c src/ringbuf.c src/take.c \
                 src/jsonout.c src/parse.c src/ffmpeg_posix.c
GUI_CORE_OBJS := $(GUI_CORE_SRCS:src/%.c=$(OBJ_DIR)/%.o)

GUI_CPPFLAGS := -Isrc/gui -I$(RAYLIB_SRC)
GUI_LDLIBS   := $(RAYLIB_LIB) $(ALSA_LIBS) -lGL -lX11 -lm -lpthread -ldl -lrt

# An uninitialised submodule is not an error: the CLI still builds, and `make`
# just quietly stops shipping a window.
HAVE_RAYLIB := $(wildcard $(RAYLIB_SRC)/raylib.h)

# raylib needs OpenGL and X11 to build. Probing with the compiler rather than
# looking for known paths, so this holds on distributions that put their
# headers somewhere else. Only worth the fork when there is a raylib to build.
#
# The headers come in with -include rather than a source that #includes them:
# make escapes a literal '#' inside $(shell ...), and the backslash survives
# into the program text and stops it compiling for the wrong reason entirely.
ifneq ($(HAVE_RAYLIB),)
HAVE_GLX := $(shell echo 'int main(void){return 0;}' \
              | $(CC) -x c - -include X11/Xlib.h -include GL/gl.h \
                      -o /dev/null -lX11 -lGL >/dev/null 2>&1 && echo yes)
endif

BUILD_GUI := $(if $(HAVE_RAYLIB),$(HAVE_GLX))

GUI_DEV_PKGS := libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev \
                libxcursor-dev libxinerama-dev libxkbcommon-dev

ifneq ($(BUILD_GUI),)
GUI_STATUS := enabled
else ifneq ($(HAVE_RAYLIB),)
GUI_STATUS := missing the OpenGL and X11 headers
else
GUI_STATUS := run 'git submodule update --init --depth 1' to enable
endif

DEPS      := $(OBJS:.o=.d) $(GUI_OBJS:.o=.d) $(TEST_BINS:=.d)

.PHONY: all gui gui-skipped release debug test check format format-check \
        install uninstall clean clean-raylib help

all: $(BIN) $(if $(BUILD_GUI),$(GUI_BIN),gui-skipped)

# Silent when raylib is simply absent - that is the headless case, and it is
# not a problem. Only says anything when the app was asked for and cannot be
# built, which is the case someone needs telling about.
gui-skipped:
ifneq ($(HAVE_RAYLIB),)
	@echo "audiaki: skipping $(PROJECT)-gui, the OpenGL and X11 headers are missing"
	@echo "    sudo apt install $(GUI_DEV_PKGS)"
endif

release: clean
	$(MAKE) OPTFLAGS="-O2 -DNDEBUG" all

debug:
	$(MAKE) OPTFLAGS="-O0 -g3 -fno-omit-frame-pointer" \
	        SANITIZE="-fsanitize=address,undefined -fno-sanitize-recover=all" all

$(BIN): $(OBJS) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(ALSA_CFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR) $(OBJ_DIR) $(OBJ_DIR)/gui $(TEST_DIR):
	@mkdir -p $@

# -- desktop app -------------------------------------------------------------

gui: $(GUI_BIN)

# Both checks run before raylib is invoked: it takes about a minute to build,
# and failing at the end of that on a missing header helps nobody.
$(RAYLIB_LIB):
	@test -n "$(HAVE_RAYLIB)" || { \
	  echo "$(RAYLIB_DIR) is empty - run:"; \
	  echo "    git submodule update --init --depth 1"; \
	  exit 1; }
	@test -n "$(HAVE_GLX)" || { \
	  echo "$(PROJECT)-gui needs the OpenGL and X11 development headers:"; \
	  echo "    sudo apt install $(GUI_DEV_PKGS)"; \
	  exit 1; }
	$(MAKE) -C $(RAYLIB_SRC) PLATFORM=$(RAYLIB_PLATFORM) RAYLIB_LIBTYPE=STATIC

$(GUI_BIN): $(GUI_OBJS) $(GUI_CORE_OBJS) $(RAYLIB_LIB) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -o $@ $(GUI_OBJS) $(GUI_CORE_OBJS) $(GUI_LDLIBS)

# raylib.h trips -Wpedantic in strict ISO mode, so the GUI objects are built
# without it. Everything audiaki itself writes still gets the full set.
$(OBJ_DIR)/gui/%.o: src/gui/%.c | $(OBJ_DIR)/gui
	$(CC) $(CPPFLAGS) $(GUI_CPPFLAGS) $(ALSA_CFLAGS) \
	      $(filter-out -Wpedantic,$(CFLAGS)) -c -o $@ $<

# -- tests -------------------------------------------------------------------

$(TEST_DIR)/%: tests/%.c $(PORTABLE_OBJS) | $(TEST_DIR)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) $(LDFLAGS) -o $@ $< $(PORTABLE_OBJS) -lm

test: $(TEST_BINS)
	@status=0; \
	for t in $(TEST_BINS); do \
	  printf '\n== %s ==\n' "$$t"; \
	  "$$t" || status=1; \
	done; \
	exit $$status

# Everything a contributor should run before opening a pull request.
check: all test format-check

# -- style -------------------------------------------------------------------

STYLE_FILES := $(wildcard src/*.c src/*.h src/gui/*.c src/gui/*.h \
                          tests/*.c tests/*.h)

format:
	$(CLANG_FORMAT) -i $(STYLE_FILES)

format-check:
	@$(CLANG_FORMAT) --dry-run --Werror $(STYLE_FILES) && echo "style OK"

# -- install -----------------------------------------------------------------

APPDIR    ?= $(PREFIX)/share/applications
ICONDIR   ?= $(PREFIX)/share/icons/hicolor/256x256/apps

install: all
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(PROJECT)
	$(INSTALL) -m 0644 docs/$(PROJECT).1 $(DESTDIR)$(MANDIR)/$(PROJECT).1
ifneq ($(BUILD_GUI),)
	$(INSTALL) -d $(DESTDIR)$(APPDIR) $(DESTDIR)$(ICONDIR)
	$(INSTALL) -m 0755 $(GUI_BIN) $(DESTDIR)$(BINDIR)/$(PROJECT)-gui
	$(INSTALL) -m 0644 assets/$(PROJECT).desktop $(DESTDIR)$(APPDIR)/$(PROJECT).desktop
	$(INSTALL) -m 0644 assets/logo.png $(DESTDIR)$(ICONDIR)/$(PROJECT).png
endif

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(PROJECT)
	$(RM) $(DESTDIR)$(BINDIR)/$(PROJECT)-gui
	$(RM) $(DESTDIR)$(MANDIR)/$(PROJECT).1
	$(RM) $(DESTDIR)$(APPDIR)/$(PROJECT).desktop
	$(RM) $(DESTDIR)$(ICONDIR)/$(PROJECT).png

# -- housekeeping ------------------------------------------------------------

clean:
	$(RM) -r $(BUILD_DIR)

# Separate from clean because rebuilding raylib costs about a minute and it
# only ever changes when the submodule is moved.
clean-raylib:
	@test -f $(RAYLIB_SRC)/Makefile && \
	  $(MAKE) -C $(RAYLIB_SRC) clean || true

help:
	@echo "$(PROJECT) $(VERSION)"
	@echo "targets: all gui debug release test check format format-check install"
	@echo "         uninstall clean clean-raylib"
	@echo "vars:    PREFIX=$(PREFIX) CC=$(CC) STRICT=0|1 BUILD_DIR=$(BUILD_DIR)"
	@echo "gui:     $(GUI_STATUS)"

-include $(DEPS)
