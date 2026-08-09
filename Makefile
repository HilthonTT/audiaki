# audiaki - ALSA capture to WAV
# SPDX-License-Identifier: MIT
#
# Common targets:
#   make            build build/audiaki
#   make debug      build with -O0, debug info and ASan/UBSan
#   make test       build and run the unit tests (no ALSA required)
#   make check      tests, a clang-format style check and the completions
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

# Where each shell looks for completions it did not write. Separate variables
# because distributions disagree, and a packager should be able to move one
# without moving the binary.
BASHCOMPDIR ?= $(PREFIX)/share/bash-completion/completions
ZSHCOMPDIR  ?= $(PREFIX)/share/zsh/site-functions
FISHCOMPDIR ?= $(PREFIX)/share/fish/vendor_completions.d

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

# An object file remembers nothing about the flags it was built with, and one
# setting genuinely cannot be mixed: a sanitized object refers to runtime
# symbols an unsanitized link does not pull in. So `make debug` followed by
# `make` used to fail at the link with a page of undefined __asan_* references -
# a message about the linker, for a problem that is entirely about stale
# objects.
#
# The setting is stamped into the build directory and compared on every run.
# Changing it throws the objects away, which is the only sound answer and is
# what whoever changed it meant anyway.
SANITIZE_STAMP := $(BUILD_DIR)/.sanitize
SANITIZE_WAS   := $(shell cat $(SANITIZE_STAMP) 2>/dev/null)

ifneq ($(strip $(SANITIZE)),$(strip $(SANITIZE_WAS)))
$(info audiaki: sanitizer setting changed, rebuilding)
$(shell rm -rf $(OBJ_DIR) $(TEST_DIR))
$(shell mkdir -p $(BUILD_DIR) && printf '%s' '$(strip $(SANITIZE))' > $(SANITIZE_STAMP))
endif

ALSA_CFLAGS := $(shell $(PKG_CONFIG) --cflags alsa 2>/dev/null)
ALSA_LIBS   := $(shell $(PKG_CONFIG) --libs alsa 2>/dev/null || echo -lasound)
ifeq ($(strip $(ALSA_LIBS)),)
ALSA_LIBS := -lasound
endif

# PipeWire is optional, the way raylib is: with the headers absent the second
# backend is simply not compiled and audiaki talks to ALSA as it always has.
# There is no fallback to hand-written paths here - a machine either has the
# development package or it does not.
PIPEWIRE_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null)
PIPEWIRE_LIBS   := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null)
HAVE_PIPEWIRE   := $(if $(strip $(PIPEWIRE_LIBS)),1,)

ifneq ($(HAVE_PIPEWIRE),)
CPPFLAGS  += -DAUDIAKI_HAVE_PIPEWIRE
BACKEND_STATUS := enabled
else
BACKEND_STATUS := alsa only - install libpipewire-0.3-dev to add it
endif

LDLIBS    += $(ALSA_LIBS) $(PIPEWIRE_LIBS) -lm

# -- sources -----------------------------------------------------------------

# One directory per layer; see DESIGN.md. src/gui is built separately, against
# raylib, and is the only part that may be absent.
SRC_DIRS  := src src/cli src/cmd src/backend src/audio src/take src/media \
             src/edit src/term src/util

SRCS      := $(sort $(foreach d,$(SRC_DIRS),$(wildcard $(d)/*.c)))
ifeq ($(HAVE_PIPEWIRE),)
SRCS      := $(filter-out src/backend/%_pipewire.c,$(SRCS))
endif
OBJS      := $(SRCS:src/%.c=$(OBJ_DIR)/%.o)

# Objects that do not touch a sound server. Tests link against these, so the
# suite runs on machines with neither ALSA nor PipeWire headers.
#
# A directory rule rather than a list of files, which is the point of the
# layout. src/backend is the only place a sound server is reached; src/cmd and
# src/cli sit on top of it (backend.c binds a capture table at file scope, so
# anything reaching it drags libasound in). Every layer below that is portable
# by construction, and is tested without anyone having to remember to say so.
PORTABLE_SRCS := $(filter-out src/backend/% src/cmd/% src/cli/% src/main.c,$(SRCS))
PORTABLE_OBJS := $(PORTABLE_SRCS:src/%.c=$(OBJ_DIR)/%.o)

TEST_SRCS := $(sort $(wildcard tests/*/test_*.c))
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

# The core the window shares with the CLI: all of src/backend, and the layers
# under it that the window actually reaches. Not src/term (the window draws its
# own meter), src/cli or src/cmd (it parses its own, much smaller, argv and
# owns its own transport).
#
# jsonout is here only because the backends print --probe tables through it;
# the window never asks for one.
GUI_CORE_SRCS := $(filter src/backend/%,$(SRCS)) \
                 src/audio/format.c src/audio/fft.c src/audio/spectrum.c \
                 src/audio/tuner.c \
                 src/take/take.c src/take/meta.c src/take/preroll.c \
                 src/edit/samples.c src/edit/track.c src/edit/doc.c \
                 src/edit/edit.c src/edit/load.c src/edit/mix.c \
                 src/edit/export.c \
                 src/media/wav.c src/media/ffmpeg_posix.c \
                 src/util/log.c src/util/jsonout.c src/util/ringbuf.c \
                 src/util/parse.c src/util/path.c src/util/config.c
GUI_CORE_OBJS := $(GUI_CORE_SRCS:src/%.c=$(OBJ_DIR)/%.o)

GUI_CPPFLAGS := -I$(RAYLIB_SRC)
GUI_LDLIBS   := $(RAYLIB_LIB) $(ALSA_LIBS) $(PIPEWIRE_LIBS) \
                -lGL -lX11 -lm -lpthread -ldl -lrt

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

.PHONY: all gui gui-skipped release debug test check check-completions format \
        format-check install uninstall clean clean-raylib help

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

# The PipeWire and SPA headers use GCC statement expressions, which -Wpedantic
# rejects in strict ISO mode, so the two backend objects are built without it -
# the same exception raylib already gets. Everything audiaki itself writes,
# including the rest of these files, still gets the full warning set.
$(OBJ_DIR)/backend/device_pipewire.o $(OBJ_DIR)/backend/monitor_pipewire.o: \
  CFLAGS := $(filter-out -Wpedantic,$(CFLAGS))

# $(@D) rather than a fixed list of order-only prerequisites: the object tree
# mirrors src/, so adding a layer must not mean adding a rule here too.
$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALSA_CFLAGS) $(PIPEWIRE_CFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
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
$(OBJ_DIR)/gui/%.o: src/gui/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(GUI_CPPFLAGS) $(ALSA_CFLAGS) $(PIPEWIRE_CFLAGS) \
	      $(filter-out -Wpedantic,$(CFLAGS)) -c -o $@ $<

# -- tests -------------------------------------------------------------------

# tests/ mirrors src/, so which layers are covered - and which are not - is
# visible from the tree rather than only from this list.
$(TEST_DIR)/%: tests/%.c $(PORTABLE_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) $(LDFLAGS) -o $@ $< $(PORTABLE_OBJS) -lm

test: $(TEST_BINS)
	@status=0; \
	for t in $(TEST_BINS); do \
	  printf '\n== %s ==\n' "$$t"; \
	  "$$t" || status=1; \
	done; \
	exit $$status

# The completions are the one part of the CLI nothing else exercises: an option
# added to cli.c works while its completion silently does not exist.
check-completions:
	@./scripts/check-completions.sh

# Everything a contributor should run before opening a pull request.
check: all test format-check check-completions

# -- style -------------------------------------------------------------------

STYLE_FILES := $(wildcard $(foreach d,$(SRC_DIRS) src/gui,$(d)/*.c $(d)/*.h) \
                          tests/*.h tests/*/*.c)

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
	$(INSTALL) -d $(DESTDIR)$(BASHCOMPDIR) $(DESTDIR)$(ZSHCOMPDIR) \
	             $(DESTDIR)$(FISHCOMPDIR)
	$(INSTALL) -m 0644 completions/$(PROJECT).bash \
	           $(DESTDIR)$(BASHCOMPDIR)/$(PROJECT)
	$(INSTALL) -m 0644 completions/_$(PROJECT) $(DESTDIR)$(ZSHCOMPDIR)/_$(PROJECT)
	$(INSTALL) -m 0644 completions/$(PROJECT).fish \
	           $(DESTDIR)$(FISHCOMPDIR)/$(PROJECT).fish
ifneq ($(BUILD_GUI),)
	$(INSTALL) -d $(DESTDIR)$(APPDIR) $(DESTDIR)$(ICONDIR)
	$(INSTALL) -m 0755 $(GUI_BIN) $(DESTDIR)$(BINDIR)/$(PROJECT)-gui
	$(INSTALL) -m 0644 assets/$(PROJECT).desktop $(DESTDIR)$(APPDIR)/$(PROJECT).desktop
	$(INSTALL) -m 0644 assets/logo.png $(DESTDIR)$(ICONDIR)/$(PROJECT).png
	# bash-completion loads by command name, and both commands are in the one
	# file, so the window gets a link to it rather than a copy of it.
	ln -sf $(PROJECT) $(DESTDIR)$(BASHCOMPDIR)/$(PROJECT)-gui
	$(INSTALL) -m 0644 completions/_$(PROJECT)-gui \
	           $(DESTDIR)$(ZSHCOMPDIR)/_$(PROJECT)-gui
	$(INSTALL) -m 0644 completions/$(PROJECT)-gui.fish \
	           $(DESTDIR)$(FISHCOMPDIR)/$(PROJECT)-gui.fish
endif

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(PROJECT)
	$(RM) $(DESTDIR)$(BINDIR)/$(PROJECT)-gui
	$(RM) $(DESTDIR)$(MANDIR)/$(PROJECT).1
	$(RM) $(DESTDIR)$(APPDIR)/$(PROJECT).desktop
	$(RM) $(DESTDIR)$(ICONDIR)/$(PROJECT).png
	$(RM) $(DESTDIR)$(BASHCOMPDIR)/$(PROJECT)
	$(RM) $(DESTDIR)$(BASHCOMPDIR)/$(PROJECT)-gui
	$(RM) $(DESTDIR)$(ZSHCOMPDIR)/_$(PROJECT)
	$(RM) $(DESTDIR)$(ZSHCOMPDIR)/_$(PROJECT)-gui
	$(RM) $(DESTDIR)$(FISHCOMPDIR)/$(PROJECT).fish
	$(RM) $(DESTDIR)$(FISHCOMPDIR)/$(PROJECT)-gui.fish

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
	@echo "targets: all gui debug release test check check-completions format"
	@echo "         format-check install uninstall clean clean-raylib"
	@echo "vars:    PREFIX=$(PREFIX) CC=$(CC) STRICT=0|1 BUILD_DIR=$(BUILD_DIR)"
	@echo "gui:     $(GUI_STATUS)"
	@echo "pipewire: $(BACKEND_STATUS)"

-include $(DEPS)
