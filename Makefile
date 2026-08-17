# audiaki - capture to WAV
# SPDX-License-Identifier: MIT
#
# Common targets:
#   make            build build/audiaki
#   make debug      build with -O0, debug info and ASan/UBSan
#   make test       build and run the unit tests (no audio system required)
#   make check      tests, a clang-format style check and the completions
#   make install    install into $(PREFIX) (default /usr/local)
#
#   make HOTRELOAD=1 gui   build/hot/audiaki-gui, which reloads its own code
#                          on F5 without losing the session; see CONTRIBUTING.md

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

# HOTRELOAD=1 builds the window as a shell plus a library it can load again
# while it is running; see CONTRIBUTING.md and src/hotreload/plug.h. It is a
# development build - it wants a second file beside the binary, and it is not
# what `make install` installs - so it gets a build directory of its own rather
# than sharing objects that were compiled with different flags.
HOTRELOAD ?= 0

BUILD_DIR ?= $(if $(filter 1,$(HOTRELOAD)),build/hot,build)
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
ifeq ($(HOTRELOAD),1)
CPPFLAGS  += -DAUDIAKI_HOTRELOAD
endif
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

# -- backends ----------------------------------------------------------------

# Which of the four are compiled in is decided here and nowhere else: each one
# absent is a -D that is not passed, and src/backend/backend.c turns that into a
# backend that reports itself unavailable rather than one that fails to link.
#
# Two of them are decided by the platform rather than by a package. ALSA does
# not exist on macOS and CoreAudio does not exist anywhere else, so neither is
# something to probe for - asking pkg-config about them would only turn "this is
# a Mac" into a confusing message about a missing development package.
UNAME_S := $(shell uname -s)

ifneq ($(UNAME_S),Darwin)
ALSA_CFLAGS := $(shell $(PKG_CONFIG) --cflags alsa 2>/dev/null)
ALSA_LIBS   := $(shell $(PKG_CONFIG) --libs alsa 2>/dev/null || echo -lasound)
ifeq ($(strip $(ALSA_LIBS)),)
ALSA_LIBS := -lasound
endif
HAVE_ALSA := 1
endif

# The frameworks rather than a library: they are part of the system on every Mac
# that can run this, so there is nothing to install and nothing to detect.
ifeq ($(UNAME_S),Darwin)
COREAUDIO_LIBS := -framework CoreAudio -framework AudioToolbox \
                  -framework AudioUnit -framework CoreFoundation
HAVE_COREAUDIO := 1
endif

# PipeWire and JACK are optional, the way raylib is: with the headers absent the
# backend is simply not compiled and audiaki talks to whatever else is there.
# There is no fallback to hand-written paths - a machine either has the
# development package or it does not.
PIPEWIRE_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null)
PIPEWIRE_LIBS   := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null)
HAVE_PIPEWIRE   := $(if $(strip $(PIPEWIRE_LIBS)),1,)

# jack.pc is what both jackd1 and jackd2 install, and what pipewire-jack
# installs as well - which is the point: a JACK client built against any of them
# talks to whichever is running.
JACK_CFLAGS := $(shell $(PKG_CONFIG) --cflags jack 2>/dev/null)
JACK_LIBS   := $(shell $(PKG_CONFIG) --libs jack 2>/dev/null)
HAVE_JACK   := $(if $(strip $(JACK_LIBS)),1,)

ifneq ($(HAVE_ALSA),)
CPPFLAGS  += -DAUDIAKI_HAVE_ALSA
endif
ifneq ($(HAVE_PIPEWIRE),)
CPPFLAGS  += -DAUDIAKI_HAVE_PIPEWIRE
endif
ifneq ($(HAVE_JACK),)
CPPFLAGS  += -DAUDIAKI_HAVE_JACK
endif
ifneq ($(HAVE_COREAUDIO),)
CPPFLAGS  += -DAUDIAKI_HAVE_COREAUDIO
endif

BACKENDS := $(strip $(if $(HAVE_PIPEWIRE),pipewire) $(if $(HAVE_COREAUDIO),coreaudio) \
                    $(if $(HAVE_ALSA),alsa) $(if $(HAVE_JACK),jack))
BACKEND_MISSING := $(strip $(if $(HAVE_PIPEWIRE),,libpipewire-0.3-dev) \
                           $(if $(HAVE_JACK),,libjack-jackd2-dev))

LDLIBS    += $(ALSA_LIBS) $(PIPEWIRE_LIBS) $(JACK_LIBS) $(COREAUDIO_LIBS) -lm

# -- sources -----------------------------------------------------------------

# One directory per layer; see DESIGN.md. src/gui is built separately, against
# raylib, and is the only part that may be absent.
SRC_DIRS  := src src/cli src/cmd src/backend src/audio src/take src/media \
             src/edit src/term src/util

SRCS      := $(sort $(foreach d,$(SRC_DIRS),$(wildcard $(d)/*.c)))

# A backend that is not compiled in does not have its sources compiled either.
# The naming is the rule: src/backend/{device,monitor}_<backend>.c, so adding a
# fifth audio system means one more line here and nothing else in this file.
ifeq ($(HAVE_ALSA),)
SRCS      := $(filter-out src/backend/%_alsa.c,$(SRCS))
endif
ifeq ($(HAVE_PIPEWIRE),)
SRCS      := $(filter-out src/backend/%_pipewire.c,$(SRCS))
endif
ifeq ($(HAVE_JACK),)
SRCS      := $(filter-out src/backend/%_jack.c,$(SRCS))
endif
ifeq ($(HAVE_COREAUDIO),)
SRCS      := $(filter-out src/backend/%_coreaudio.c,$(SRCS))
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

# tests/gui and tests/cli are held back: one links against the window and so
# needs raylib, the other against the backend table and so needs a backend, and
# everything else here deliberately builds with neither. Both are added back
# below, each only where the thing it needs is there.
TEST_SRCS := $(filter-out tests/gui/% tests/cli/%,$(sort $(wildcard tests/*/test_*.c)))
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

# The shell, when there is one to be apart from: the window and the run loop,
# and the capture thread - unloading the code a running thread is executing is
# not something to find out about at 3 am. Everything else in src/gui goes into
# the library the shell loads. See src/hotreload/plug.h.
GUI_SHELL_SRCS := src/gui/main.c src/gui/engine.c src/hotreload/hotreload.c
GUI_PLUG_SRCS  := $(filter-out $(GUI_SHELL_SRCS),$(GUI_SRCS))
GUI_SHELL_OBJS := $(GUI_SHELL_SRCS:src/%.c=$(OBJ_DIR)/%.o)

# The library is dlopen()ed, so its objects want -fPIC and therefore a tree of
# their own; the shell's do not, and are built by the ordinary rules.
PIC_DIR   := $(OBJ_DIR)/pic
GUI_PLUG_OBJS := $(GUI_PLUG_SRCS:src/gui/%.c=$(PIC_DIR)/gui/%.o)
GUI_PLUG_LIB  := $(BUILD_DIR)/lib$(PROJECT)-gui.so

# The core the window shares with the CLI: all of src/backend, and the layers
# under it that the window actually reaches. Not src/term (the window draws its
# own meter), src/cli or src/cmd (it parses its own, much smaller, argv and
# owns its own transport).
#
# jsonout is here only because the backends print --probe tables through it;
# the window never asks for one.
GUI_CORE_SRCS := $(filter src/backend/%,$(SRCS)) \
                 src/audio/format.c src/audio/fft.c src/audio/spectrum.c \
                 src/audio/spectral.c \
                 src/audio/tuner.c src/audio/click.c src/audio/resample.c \
                 src/audio/loudness.c \
                 src/take/take.c src/take/meta.c src/take/preroll.c \
                 src/take/latency.c \
                 src/edit/samples.c src/edit/track.c src/edit/doc.c \
                 src/edit/edit.c src/edit/load.c src/edit/mix.c \
                 src/edit/export.c src/edit/project.c src/edit/repair.c \
                 src/media/wav.c src/media/ffmpeg_posix.c \
                 src/util/log.c src/util/jsonout.c src/util/ringbuf.c \
                 src/util/parse.c src/util/path.c src/util/config.c
GUI_CORE_OBJS := $(GUI_CORE_SRCS:src/%.c=$(OBJ_DIR)/%.o)

GUI_CPPFLAGS := -I$(RAYLIB_SRC)

# What raylib itself needs, which is the one part of the link that is about the
# window rather than about audio. macOS has no X11 and no librt; it draws
# through Cocoa and the system's own OpenGL.
ifeq ($(UNAME_S),Darwin)
GUI_PLATFORM_LIBS := -framework Cocoa -framework IOKit -framework CoreVideo \
                     -framework OpenGL
else
GUI_PLATFORM_LIBS := -lGL -lX11 -ldl -lrt
endif

GUI_SYS_LIBS := $(ALSA_LIBS) $(PIPEWIRE_LIBS) $(JACK_LIBS) $(COREAUDIO_LIBS) \
                $(GUI_PLATFORM_LIBS) -lm -lpthread
GUI_LDLIBS   := $(RAYLIB_LIB) $(GUI_SYS_LIBS)

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
ifeq ($(UNAME_S),Darwin)
# Nothing to probe: raylib draws through frameworks that are part of the system.
HAVE_GLX := yes
else
HAVE_GLX := $(shell echo 'int main(void){return 0;}' \
              | $(CC) -x c - -include X11/Xlib.h -include GL/gl.h \
                      -o /dev/null -lX11 -lGL >/dev/null 2>&1 && echo yes)
endif
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

DEPS      := $(OBJS:.o=.d) $(GUI_OBJS:.o=.d) $(GUI_SHELL_OBJS:.o=.d) \
             $(GUI_PLUG_OBJS:.o=.d) $(TEST_BINS:=.d) $(CLI_TEST_BINS:=.d) \
             $(GUI_TEST_BINS:=.d)

.PHONY: all gui gui-skipped release debug test check check-completions format \
        format-check fuzz fuzz-replay fuzz-run install uninstall clean \
        clean-raylib help

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

# The PipeWire and SPA headers use GCC statement expressions, and Apple's
# framework headers are not strict ISO either, so -Wpedantic is dropped for
# those four objects - the same exception raylib already gets. Everything
# audiaki itself writes, including the rest of these files, still gets the full
# warning set; the JACK headers are clean enough not to need the exception.
$(OBJ_DIR)/backend/device_pipewire.o $(OBJ_DIR)/backend/monitor_pipewire.o \
$(OBJ_DIR)/backend/device_coreaudio.o $(OBJ_DIR)/backend/monitor_coreaudio.o: \
  CFLAGS := $(filter-out -Wpedantic,$(CFLAGS))

# $(@D) rather than a fixed list of order-only prerequisites: the object tree
# mirrors src/, so adding a layer must not mean adding a rule here too.
$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALSA_CFLAGS) $(PIPEWIRE_CFLAGS) $(JACK_CFLAGS) $(CFLAGS) \
	      -c -o $@ $<

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

ifneq ($(HOTRELOAD),1)

$(GUI_BIN): $(GUI_OBJS) $(GUI_CORE_OBJS) $(RAYLIB_LIB) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -o $@ $(GUI_OBJS) $(GUI_CORE_OBJS) $(GUI_LDLIBS)

else

# The reloadable build.
#
# raylib and the core come in whole, and -rdynamic exports the lot: the library
# is linked against nothing and resolves every call it makes - DrawRectangle,
# aud_engine_read_take, malloc - against this binary when it is loaded. That is
# what keeps there being exactly one of everything that matters. Two copies of
# raylib would mean two copies of the window handle, and two copies of the
# engine would mean two capture threads fighting over one device.
#
# --whole-archive because libraylib.a is an archive: without it the linker
# would take only the parts the shell itself calls, and the library would come
# up missing the several hundred it draws with.
$(GUI_BIN): $(GUI_SHELL_OBJS) $(GUI_CORE_OBJS) $(RAYLIB_LIB) $(GUI_PLUG_LIB) \
            | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -rdynamic -o $@ $(GUI_SHELL_OBJS) $(GUI_CORE_OBJS) \
	      -Wl,--whole-archive $(RAYLIB_LIB) -Wl,--no-whole-archive $(GUI_SYS_LIBS)

$(GUI_PLUG_LIB): $(GUI_PLUG_OBJS) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -shared -o $@ $(GUI_PLUG_OBJS) -lm

$(PIC_DIR)/gui/%.o: src/gui/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(GUI_CPPFLAGS) $(ALSA_CFLAGS) $(PIPEWIRE_CFLAGS) -fPIC \
	      $(filter-out -Wpedantic,$(CFLAGS)) -c -o $@ $<

endif

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

# The option parser's tests, which exist only where a backend was compiled in.
#
# cli.c reaches exactly one thing outside the portable layers: the table in
# backend.c that names the backends, for --backend and for the help text. That
# file cannot join PORTABLE_SRCS - it refuses to compile when no backend was
# configured at all, which is the whole point of the job that builds this suite
# without ALSA - so the test links it directly and supplies the ops it points
# at. Naming a backend and binding one are different questions, and only the
# first is asked here.
ifneq ($(BACKENDS),)
CLI_TEST_SRCS := $(sort $(wildcard tests/cli/test_*.c))
CLI_TEST_BINS := $(CLI_TEST_SRCS:tests/%.c=$(TEST_DIR)/%)
CLI_TEST_DEPS := src/cli/cli.c src/cli/usage.c src/backend/backend.c

$(TEST_DIR)/cli/%: tests/cli/%.c $(CLI_TEST_DEPS) $(PORTABLE_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Itests $(CFLAGS) $(LDFLAGS) -o $@ \
	  $< $(CLI_TEST_DEPS) $(PORTABLE_OBJS) -lm
endif

# The window's own tests, which exist only where there is a raylib to link.
#
# Only what decides something is testable this way, and that is the point of it
# being apart from what draws: confirm.c reads the document and writes a
# question, keys.c reads the keyboard and writes a list of commands, and neither
# touches a pixel. The sources go in whole rather than as objects because the
# window's objects are built PIC or not depending on HOTRELOAD, and a test
# should not have to care which.
ifneq ($(HAVE_RAYLIB),)
GUI_TEST_SRCS := $(sort $(wildcard tests/gui/test_*.c))
GUI_TEST_BINS := $(GUI_TEST_SRCS:tests/%.c=$(TEST_DIR)/%)

# What each test needs linked beside it, a test at a time rather than one list
# for all of them. A shared list means every test has to stub out whatever the
# others dragged in, which is how a suite stops being added to.
$(TEST_DIR)/gui/test_confirm: GUI_TEST_DEPS := src/gui/confirm.c src/gui/ui.c
$(TEST_DIR)/gui/test_keys: GUI_TEST_DEPS := src/gui/keys.c

# The union of those, as prerequisites. Naming one too many here costs a rebuild
# that was not needed; naming one too few costs a test that is not rebuilt when
# the code under it changes.
GUI_TEST_SRC_DEPS := src/gui/confirm.c src/gui/keys.c src/gui/ui.c

$(TEST_DIR)/gui/%: tests/gui/%.c $(GUI_TEST_SRC_DEPS) $(PORTABLE_OBJS) $(RAYLIB_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(GUI_CPPFLAGS) -Itests $(CFLAGS) $(LDFLAGS) -o $@ \
	  $< $(GUI_TEST_DEPS) $(PORTABLE_OBJS) $(RAYLIB_LIB) $(GUI_SYS_LIBS)
endif

test: $(TEST_BINS) $(CLI_TEST_BINS) $(GUI_TEST_BINS)
	@status=0; \
	for t in $(TEST_BINS) $(CLI_TEST_BINS) $(GUI_TEST_BINS); do \
	  printf '\n== %s ==\n' "$$t"; \
	  "$$t" || status=1; \
	done; \
	exit $$status

# -- fuzzing -----------------------------------------------------------------

# Three parsers read files audiaki did not write - a project saved by an older
# version, somebody else's WAV, a take a crash left half way through - and all
# three walk a length-prefixed or index-bearing format to do it. See fuzz/.
#
# Each target builds twice. `make fuzz` builds it against libFuzzer to go
# looking for new inputs, which needs clang; `make fuzz-replay` builds it with a
# main() of its own and runs it over the corpus, which does not, and is what
# belongs in CI on every change - an input that crashed once and was fixed
# should be a test from then on rather than something rediscovered by luck.
FUZZ_TARGETS := $(sort $(wildcard fuzz/fuzz_*.c))
FUZZ_TARGETS := $(filter-out fuzz/fuzz_file.c,$(FUZZ_TARGETS))
FUZZ_NAMES   := $(FUZZ_TARGETS:fuzz/fuzz_%.c=%)
FUZZ_DIR     := $(BUILD_DIR)/fuzz
FUZZ_BINS    := $(FUZZ_NAMES:%=$(FUZZ_DIR)/fuzz_%)
FUZZ_REPLAYS := $(FUZZ_NAMES:%=$(FUZZ_DIR)/replay_%)

# Everything a parser sits on. The same list the unit tests link, for the same
# reason: no sound server is reachable from any of it.
FUZZ_SUPPORT := $(PORTABLE_SRCS) fuzz/fuzz_file.c

# clang, because libFuzzer is its runtime. The replay build below is whatever
# CC is, which is the point of having one.
FUZZ_CC       ?= clang
FUZZ_SANITIZE ?= -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all
# A fuzzer is measured in inputs a second, and -O1 with frame pointers is the
# usual compromise between running fast and saying where it stopped.
FUZZ_CFLAGS   ?= -O1 -g -fno-omit-frame-pointer

# What the replay build uses instead: the same sanitizers without the fuzzer's
# own runtime, so a corpus entry that overruns something still says so.
REPLAY_SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=all

fuzz: $(FUZZ_BINS)

$(FUZZ_DIR)/fuzz_%: fuzz/fuzz_%.c $(FUZZ_SUPPORT)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) -Ifuzz -std=c11 $(WARNINGS) $(FUZZ_CFLAGS) \
	  $(FUZZ_SANITIZE) -o $@ $< $(FUZZ_SUPPORT) -lm

$(FUZZ_DIR)/replay_%: fuzz/fuzz_%.c fuzz/replay.c $(FUZZ_SUPPORT)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Ifuzz -std=c11 $(WARNINGS) $(FUZZ_CFLAGS) \
	  $(REPLAY_SANITIZE) -o $@ $< fuzz/replay.c $(FUZZ_SUPPORT) -lm

# Every corpus entry through its target, under the sanitizers. Seconds, and it
# needs no clang - so it runs beside the unit tests rather than on a schedule.
fuzz-replay: $(FUZZ_REPLAYS)
	@status=0; \
	for name in $(FUZZ_NAMES); do \
	  printf '\n== %s ==\n' "$$name"; \
	  $(FUZZ_DIR)/replay_$$name fuzz/corpus/$$name/* || status=1; \
	done; \
	exit $$status

# Go looking. `make fuzz-run FUZZ_SECONDS=300` gives each target five minutes;
# anything it finds lands in fuzz/corpus/ and is replayed by every build after.
FUZZ_SECONDS ?= 60

fuzz-run: $(FUZZ_BINS)
	@status=0; \
	for name in $(FUZZ_NAMES); do \
	  printf '\n== %s ==\n' "$$name"; \
	  $(FUZZ_DIR)/fuzz_$$name fuzz/corpus/$$name \
	    -max_total_time=$(FUZZ_SECONDS) -print_final_stats=1 \
	    -artifact_prefix=fuzz/corpus/$$name/ || status=1; \
	done; \
	exit $$status

# The completions are the one part of the CLI nothing else exercises: an option
# added to cli.c works while its completion silently does not exist.
check-completions:
	@./scripts/check-completions.sh

# Everything a contributor should run before opening a pull request.
check: all test fuzz-replay format-check check-completions

# -- style -------------------------------------------------------------------

STYLE_FILES := $(wildcard $(foreach d,$(SRC_DIRS) src/gui src/hotreload,$(d)/*.c $(d)/*.h) \
                          tests/*.h tests/*/*.c)

format:
	$(CLANG_FORMAT) -i $(STYLE_FILES)

format-check:
	@$(CLANG_FORMAT) --dry-run --Werror $(STYLE_FILES) && echo "style OK"

# -- install -----------------------------------------------------------------

APPDIR    ?= $(PREFIX)/share/applications
ICONDIR   ?= $(PREFIX)/share/icons/hicolor/256x256/apps

install: all
ifeq ($(HOTRELOAD),1)
	@echo "audiaki: HOTRELOAD=1 builds a window that loads a library from beside"
	@echo "    itself; install a normal build instead:  make install"
	@exit 1
endif
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
	@echo "         HOTRELOAD=1 builds $(GUI_BIN) with F5 reloading its own code"
	@echo "gui:      $(GUI_STATUS)"
	@echo "backends: $(BACKENDS)"
ifneq ($(BACKEND_MISSING),)
	@echo "          install $(BACKEND_MISSING) to add the rest"
endif

-include $(DEPS)
