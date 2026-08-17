# Contributing to audiaki

Thanks for taking the time. This is a small, single-purpose tool, so the bar is
simple: keep it a program that records an ALSA device to a WAV file and does
that part well.

## Getting set up

```sh
./scripts/install-deps.sh   # or install the ALSA headers yourself
make                        # -> build/audiaki
make check                  # unit tests, the fuzz corpus, clang-format
```

To work on the desktop app as well, fetch its vendored raylib. The headers it
needs are already covered by `install-deps.sh`, unless you passed `--no-gui`:

```sh
git submodule update --init --depth 1
make                        # -> build/audiaki and build/audiaki-gui
```

Without the submodule, `make` builds the command line recorder alone and says
nothing about it — that is the supported headless setup, not a broken one.

Useful targets:

| Target | What it does |
| --- | --- |
| `make` | Build `build/audiaki`, and `build/audiaki-gui` when raylib is available |
| `make gui` | Build only the desktop app, failing loudly if it cannot |
| `make test` | Run the unit tests (needs no ALSA device, or even ALSA headers) |
| `make debug` | Build with `-O0`, AddressSanitizer and UBSan |
| `make fuzz-replay` | Every fuzz corpus entry through its parser, sanitized |
| `make fuzz-run` | Go looking for new ones; needs clang. See `fuzz/README.md` |
| `make check` | `test` and `fuzz-replay`, plus a `clang-format` check |
| `make format` | Reformat the sources in place |
| `make clean` | Remove `build/`, keeping the compiled raylib |
| `make clean-raylib` | Rebuild raylib from scratch next time; takes about a minute |
| `make STRICT=1 ...` | Warnings become errors, as in CI |
| `make HOTRELOAD=1 gui` | The window, rebuildable while it is running — below |

## Hot reloading the window

Working on the drawing means looking at it, and restarting the app to see a
change costs the session: the device closes, the timeline empties, and whatever
you had set up to look at has to be recorded again.

`HOTRELOAD=1` splits the window into a shell and a library it loads:

```sh
make HOTRELOAD=1 gui        # -> build/hot/audiaki-gui and libaudiaki-gui.so
./build/hot/audiaki-gui     # leave it running

# in another terminal, after every edit
make HOTRELOAD=1 gui
```

Then press <kbd>F5</kbd> in the window. It picks up the library you have just
built without closing anything: the device stays open, the tracks stay where
they are, and a take that is recording keeps recording while the code drawing
it is replaced underneath.

A build that does not compile costs a line on the terminal rather than the
session — the library that is running is only let go once its replacement has
loaded. What does need a restart is a change to the shape of the state itself:
edit `app.h`, `timeline.h`, `player.h` or `viz.h` and the next F5 will tell you
so and stop, rather than read the session you have through the wrong map of it.

Two files are never reloaded, and changing them needs a restart too:
`src/gui/main.c`, which is the shell, and `src/gui/engine.c`, whose capture
thread would be executing code that had just been unmapped.

This is a development build. `make` and `make install` are unchanged and
produce a single binary with no library beside it and no `dlopen` in it; the
design and its rules are in [DESIGN.md](DESIGN.md#reloading-the-window) and
`src/hotreload/plug.h`.

## Before opening a pull request

```sh
make format
make STRICT=1 check
```

If your change touches capture, also run it against real hardware and say what
you tested in the PR description — `make test` cannot cover the device path.

If it touches a parser — `src/media/wav.c`, `src/edit/project.c`,
`src/take/meta.c` — spend a few minutes fuzzing it as well:

```sh
make fuzz-run FUZZ_SECONDS=300
```

Those three read files audiaki did not write, and all three decide how far to
reach into one by reading a number out of it. `fuzz/README.md` says what each
target covers; anything the fuzzer finds belongs in `fuzz/corpus/` with the fix,
so the input is replayed by every build afterwards.

## Code layout and conventions

The module map and the reasoning behind the structure live in
[DESIGN.md](DESIGN.md#layout). Read that first; the conventions below are what
a patch is checked against.

- **Keep the audio systems inside `src/backend/`.** Only the `*_alsa.c` and
  `*_pipewire.c` files may include `<alsa/asoundlib.h>` or
  `<pipewire/pipewire.h>`; everything else works on `aud_format` values and
  plain buffers. That is what lets the test suite build without either library,
  and CI has a job that would fail if such an include leaked elsewhere.
- **Put a new file in the layer it belongs to, and only reach downwards.**
  `cli/` and `cmd/` may use `backend/`; nothing below them may. The Makefile
  derives what the tests link from exactly that rule, so a module placed in
  `audio/`, `take/`, `media/`, `term/` or `util/` is tested automatically — and
  stops linking if it starts reaching for a device.
- **Includes are written from `src/`**: `#include "audio/format.h"`, never
  `"format.h"`. The layer a header comes from should be readable at the
  include site.
- **C11, no dependencies beyond libasound, libpipewire, raylib and libm.** No
  new third-party libraries without a discussion first.
- **Formatting is mechanical**: `.clang-format` decides, `make format` applies.
  Allman braces, two-space indent, 90 columns.
- **Naming**: public functions are prefixed by their module (`wav_`, `meter_`,
  `aud_device_`, `aud_format_`). File-local helpers are `static`. A function
  nothing calls does not stay: there is no library ABI here, so unused API is
  just weight.
- **Errors go through `log.h`**, never bare `fprintf(stderr, ...)`. Use
  `aud_perror()` when `errno` is meaningful. Keep stdout free of status text so
  `--list` and `--probe` stay pipeable.
- **Comments explain why**, not what. The existing comments are the reference
  for the density expected.

## Tests

`tests/` holds a small hand-rolled harness (`test_util.h`) — no framework, and
it mirrors `src/`, so which layers are covered is visible from the tree. To add
a test file, drop `tests/<layer>/test_<thing>.c` in place; the Makefile picks it
up automatically and links it against the sound-server-free objects.

New behaviour in `audio/`, `take/`, `media/` or `util/` should come with tests.
Behaviour in `backend/` or `cmd/` mostly cannot be unit tested — that is the
same boundary the test link uses — so describe your manual test instead.

## Commit messages

Conventional-commit style, matching the existing history:

```
feat: add --split to start a new file every N minutes
fix: patch the WAV header when the disk fills mid-take
docs: document AUDIAKI_DEVICE
ci: pin actions/checkout to v4
```

Keep the subject under ~72 characters and explain the reasoning in the body if
it is not obvious.

## Releases

1. Update `AUDIAKI_VERSION` in `src/version.h` and the `.TH` line in
   `docs/audiaki.1`.
2. Move the "Unreleased" entries in `CHANGELOG.md` under the new version.
3. Tag `vX.Y.Z` and push it — the release workflow builds and publishes the
   tarball, and fails if the tag and `src/version.h` disagree.

## Code of conduct

Participation is covered by the [Code of Conduct](CODE_OF_CONDUCT.md).
