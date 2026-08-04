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
PORTABLE_SRCS := src/format.c src/wav.c src/parse.c src/log.c
PORTABLE_OBJS := $(PORTABLE_SRCS:src/%.c=$(OBJ_DIR)/%.o)

TEST_SRCS := $(sort $(wildcard tests/test_*.c))
TEST_BINS := $(TEST_SRCS:tests/%.c=$(TEST_DIR)/%)

DEPS      := $(OBJS:.o=.d) $(TEST_BINS:=.d)

.PHONY: all release debug test check format format-check install uninstall \
        clean help

all: $(BIN)

release: clean
	$(MAKE) OPTFLAGS="-O2 -DNDEBUG" all

debug:
	$(MAKE) OPTFLAGS="-O0 -g3 -fno-omit-frame-pointer" \
	        SANITIZE="-fsanitize=address,undefined -fno-sanitize-recover=all" all

$(BIN): $(OBJS) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(ALSA_CFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR) $(OBJ_DIR) $(TEST_DIR):
	@mkdir -p $@

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

STYLE_FILES := $(wildcard src/*.c src/*.h tests/*.c tests/*.h)

format:
	$(CLANG_FORMAT) -i $(STYLE_FILES)

format-check:
	@$(CLANG_FORMAT) --dry-run --Werror $(STYLE_FILES) && echo "style OK"

# -- install -----------------------------------------------------------------

install: $(BIN)
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(PROJECT)
	$(INSTALL) -m 0644 docs/$(PROJECT).1 $(DESTDIR)$(MANDIR)/$(PROJECT).1

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(PROJECT)
	$(RM) $(DESTDIR)$(MANDIR)/$(PROJECT).1

# -- housekeeping ------------------------------------------------------------

clean:
	$(RM) -r $(BUILD_DIR)

help:
	@echo "$(PROJECT) $(VERSION)"
	@echo "targets: all debug release test check format format-check install uninstall clean"
	@echo "vars:    PREFIX=$(PREFIX) CC=$(CC) STRICT=0|1 BUILD_DIR=$(BUILD_DIR)"

-include $(DEPS)
