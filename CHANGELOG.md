# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- A metronome. `audiaki -M --click 120 take01.wav` plays a click at 120 BPM
  through the same output monitoring uses, so a take can be played in tempo
  rather than measured for it afterwards. `--click-beats` sets the bar - four by
  default, `3` for a waltz, `0` for a bare pulse - and accents the first beat an
  octave up; `--click-gain` sets how loud it sits against the instrument,
  independent of `--monitor-gain`. With `--preroll` the click starts as soon as
  the recorder is armed, so the wait before Enter is a count-in.

- The click is never written to the take. It reaches the headphones and stops
  there, on the same rule `--monitor-gain` follows: the file is made of the
  samples the device delivered, whatever was being played to the person making
  it. Use headphones - from a speaker the click is in the room, and the input
  will record it after all.

- Asking for a click opens the output on its own, so a metronome does not
  require hearing yourself as well; `-M` adds the input to it. One output
  carries both, because two streams would mean two clocks and a click that
  drifts against the monitoring beside it.

- `click` module: the beat grid as a pure function of the absolute frame index,
  with unit tests. Beat *n* lands where `n * 60 * rate / bpm` says it does
  however the periods arrived, so rounding cannot accumulate and a dropped
  buffer costs one click rather than moving every click after it. The frames
  counted are the frames captured, which makes the tempo the capture clock: at
  120 BPM and 48 kHz the beats are generated at frames 0, 24000, 48000 and so
  on, exactly.

  What that does not do is align a take to a timeline. The click reaches the
  player through the output's buffer, so a performance played perfectly in time
  is that far behind the grid it was played to. It keeps the tempo; it is not a
  DAW's click, and [docs/USAGE.md](docs/USAGE.md#playing-to-a-click) says so.

- Takes that say what they are. Every recording is now stamped with the software
  that made it, the date and time it started, the capture device it came from
  and the signal chain, and `--note "second chorus, clean tone"` adds whatever
  else is worth remembering. `--info` prints it all back. A take that has been
  copied off the machine used to be a filename and a modification date, and both
  are lost the first time someone moves it.
- Two standard chunks rather than one invention: a `LIST`/`INFO` block, which is
  what taggers and most players already read, and `bext`, the Broadcast Wave
  extension every field recorder writes. Other tools pick them up without being
  told - `ffprobe` reports the comment, the date and the coding history. `bext`
  also carries a time reference, the samples between local midnight and the
  first frame, which is what lets two takes from one session line up on a
  timeline with no timecode involved.
- `meta` module: building both chunks and reading them back, with the clock kept
  out of the builder so the layout can be unit tested against fixed values
  rather than against itself. The chunks go **before** the audio - where BWF
  requires `bext`, and where they are already on disk if a recording is
  interrupted - which is why the writer now patches its two size fields
  separately instead of rewriting one 44 byte header.
- `--no-metadata` for the plain 44 byte header, unchanged from what audiaki
  wrote before, for anything that wants nothing between `fmt` and `data`. The
  desktop app stamps its takes the same way, without a note: there is nowhere in
  the window to type one.

- `--info` over more than one file. `audiaki --info session-*.wav` reports a row
  per take instead of a page per take, which is the shape the question actually
  has at the end of a session - which of these do I keep, and which one clipped:

  ```
  FILE                DURATION     PEAK      RMS   CLIPPED
  session-001.wav        41.20     -4.4    -10.7         0
  session-002.wav      1:12.34     -0.0     -6.1      1820  CLIP
  ```

  A file that cannot be read is reported and stepped over rather than ending the
  run, since one bad take hiding the state of the eleven after it is the
  opposite of what measuring a session is for; the exit status is still
  non-zero. With `--json` the same run writes an array of the objects `--info`
  already wrote for one file, and a single file still writes the single object.

- Hearing the take while it is recorded. `audiaki -M take01.wav` plays the
  capture stream through an output as it is written, so the level, the tone and
  the room can be checked while playing rather than afterwards. The desktop app
  has monitored since it had a transport; the CLI opened the same stream only to
  play finished files back. `--monitor-device` names the output and
  `--monitor-gain` scales what is heard, both switching monitoring on by
  themselves. Audible while `--preroll` is armed too, which is when the level is
  usually being set.
- Monitoring never costs a take. An output that will not open, or one that fails
  part way through, is reported and dropped, and the recording carries on: what
  is going to disk is the point, and what is coming out of the headphones is
  not. Frames the output cannot keep up with are dropped rather than queued, so
  the monitor may skip on a busy machine without any of that reaching the file.
  `--monitor-gain` is the monitoring level alone - the file is written from the
  samples the device delivered, so a quiet monitor is not a quiet take.
- A feedback warning, printed before recording starts. The default capture on a
  laptop is the built-in microphone and the default output is the speaker beside
  it, which is a loop that reaches full scale in a fraction of a second. So is
  capturing an output's monitor source and playing it back through that same
  output, which headphones do not save you from.

- Playing a take back. `audiaki --play take01.wav` sends it to an output and
  draws the recording meter against the file's length, so the take `--info`
  has just measured can be listened to without leaving the shell for a media
  player. `-t` plays the first part only, `--spectrum` and `--no-meter` behave
  as they do when recording, and Ctrl+C stops at once. It plays whatever the
  WAV reader accepts, not only audiaki's own takes.
- `play` module: the WAV reader, the playback stream and the meter wired
  together. No new audio system code - the two monitor backends already turned
  interleaved floats into sound - but two new operations on them:
  `aud_monitor_space()` and `aud_monitor_drain()`. Monitoring drops whatever
  does not fit, which is right when the input sets the pace and useless when a
  file does: read at disk speed, a take would be consumed in a moment and all
  but the first buffer of it thrown away. Playback asks how much will fit and
  hands over exactly that, which makes the output's own consumption the clock,
  and waits for the queue to empty before closing so the end is not cut off.
- `-D` names the **output** under `--play`. `$AUDIAKI_DEVICE` is deliberately
  ignored there: what it names is a capture device, and handing one to the
  playback side would fail for a reason nobody would guess from the message.

- Pre-roll. `--preroll SECS` holds the last few seconds and waits, and the take
  starts that far before you press Enter; `audiaki-gui --preroll SECS` keeps the
  same seconds while the window is idle, so Record does. The take everyone loses
  is the one played to check the sound, and the audio was already being captured
  for the meters - it was only being thrown away. Interrupting the wait writes
  no file, and with `--take` does not use up the number.
- `preroll` module: a circular buffer of captured frames in the hardware format,
  drained through its one or two contiguous segments rather than a second copy
  of itself. Not `ringbuf`, which carries floats for the visualiser and the
  monitor: pre-roll seconds are part of the recording and have to reach the file
  bit for bit, which a round trip through float would not do for a 24 or 32 bit
  take.

- A PipeWire backend. audiaki now talks to the sound server that owns the card
  on most current desktops, rather than only to the card. It is chosen without
  being asked: if a daemon answers, audiaki uses it; if none does, ALSA, exactly
  as before. `--backend auto|pipewire|alsa` and `$AUDIAKI_BACKEND` override the
  choice, and `audiaki-gui` takes `-b` for the same. Asking for a backend that
  is not there is an error rather than a silent downgrade - someone who typed
  `--backend pipewire` wants to hear it was missing, not to find out later that
  their device names came from somewhere else.
- Recording another application's output. A PipeWire sink appears in `--list`
  described as a monitor, and capturing it records what is being played to it -
  a browser, a synth, a call. Opening the card cannot do this at any setting.
- Device names that match the rest of the desktop. Under PipeWire `--list`
  reports `alsa_input.pci-0000_00_1f.3.analog-stereo` and "Built-in Audio
  Analog Stereo" - the strings the system settings shows - instead of the
  `hw:CARD=x,DEV=n` that only ALSA uses. The DEVICE column widens to fit them;
  under ALSA it stays at the 32 it always was, so that listing is unchanged.
- `backend` module: the two op tables an audio system is reached through, and
  the selection between them. `device.c` and `monitor.c` became dispatchers
  over it, with the implementations moving to `device_alsa.c`, `monitor_alsa.c`
  and their PipeWire counterparts.

### Changed

- `src/` is one directory per layer — `cli`, `cmd`, `backend`, `audio`, `take`,
  `media`, `term`, `util` — where it was 62 files in a flat directory. No
  behaviour changes; local includes are now written from `src/`
  (`#include "audio/format.h"`), so the layer a header comes from is readable at
  the include site. `tests/` mirrors the same tree.

- The layering is enforced by the build rather than described in a comment. The
  Makefile's `PORTABLE_SRCS` — the objects the tests link against, and so the
  set that must build with no sound server present — was a hand-maintained list
  of fifteen files that silently went stale whenever a module was added; it is
  now `$(filter-out src/backend/% src/cmd/% src/cli/% src/main.c,$(SRCS))`. It
  turned out the old list had already drifted: `visualize` and `meter` were
  portable and untested by omission, and are now linked into the suite.

- `main.c` dispatches and nothing else. The five `run_*` functions that mapped
  options onto modules moved into `src/cmd/`, one file per command, and the
  option struct they share with the parser moved to `src/options.h` — so a
  command no longer includes the argument parser to find out what it was asked
  for. `aud_recorder_options`, `aud_play_options` and `aud_tune_options` had one
  caller each and are now internal to the command that uses them.

- Three files that had grown past what one unit should hold were split along
  seams they already had: the CLI's help text out of `cli.c` into `cli/usage.c`,
  the monitor-and-metronome output out of the capture loop into
  `cmd/playback.c`, and the desktop window's 1,300 lines into `gui/main.c`,
  `args.c`, `devices.c` and `screen.c` over a shared `gui/app.h`. The `--list`
  table moved out of `backend/device.c`, which no longer decides how anything is
  laid out.

- `--monitor-device` no longer implies `--monitor` when `--click` is also given.
  Naming an output used to mean "play the input through this one", because there
  was nothing else it could mean; with a metronome there is, and it now names
  where the click comes out without switching monitoring on. Unchanged without
  `--click`, as is `--monitor-gain`, which only ever scaled the input.
- The terminal meter takes a total length, and draws `00:12 / 03:45` with no
  xrun counter when it has one. The recording line is unchanged to the byte.
- `device.h` no longer includes `<alsa/asoundlib.h>`, and the stream handle in
  `aud_device` is opaque. The header comment claimed ALSA was confined to
  `device.c` already; it was not, because every file including the header was
  compiled against libasound whether it used it or not, and `recorder.c` reached
  through the handle to call `snd_pcm_drop()` directly. That call is now
  `aud_device_drop()`.
- Monitoring through PipeWire works at any capture rate, because the server
  resamples. The ALSA monitor still declines rather than carry an interpolator
  for a convenience feature, and now says which backend does not have the limit.
- `--probe` under PipeWire reports what a stream will actually be given and says
  where that comes from, rather than a hardware capability table. Through a
  server that converts, the card's own format list no longer decides what a
  recording can be, and printing it as though it did would be a lie of exactly
  the kind `--probe` exists to prevent. `--backend alsa --probe` still asks the
  hardware.
- `--list --json` and `--probe --json` gained a `backend` field. Additive, so
  existing filters keep working.
- The README was split: how to drive audiaki stays there, why it is built the
  way it is moved to `DESIGN.md`. The two audiences were reading past each
  other, and the module map in `CONTRIBUTING.md` had drifted a backend behind.
- The README was cut down again, to what someone deciding whether to install
  audiaki needs. The reference material it had grown - every option, the meter,
  the tuner, pre-roll, troubleshooting - moved to `docs/USAGE.md`, and the
  desktop app's build steps, controls and visualisers to `docs/DESKTOP.md`.
  Both ship in the release tarball alongside the man page.

### Removed

- Nine functions nothing called: `aud_engine_device()`,
  `aud_engine_monitor_gain()`, `aud_monitor_device()`, `aud_monitor_flush()`,
  `aud_monitor_underruns()`, `aud_recorder_stop_requested()`, `aud_ui_panel()`,
  `aud_viz_bands()` and `aud_viz_mode_get()`. There is no library ABI here, so
  an accessor with no caller is weight rather than surface.
- With them, the `flush` and `underruns` slots on `aud_monitor_ops`, and their
  ALSA and PipeWire implementations. The engine closes the monitor when
  monitoring is switched off rather than flushing it, so the vtable entry only
  obliged every future backend to implement something nothing reached. The
  underrun counters were incremented and never read, which took the PipeWire
  monitor's `primed` and `prime_frames` bookkeeping with them.
- The monitor backends' `device_out` out-parameter, which reported the resolved
  playback device name into a field that nothing read once `aud_monitor_device()`
  was gone. `rate_out` and `channels_out` stay: the ALSA monitor refuses to open
  on a rate it cannot match, and that is a negotiation a backend may need.
- The PipeWire headers are optional at build time, the way raylib is. Without
  `libpipewire-0.3-dev` the two backend files are not compiled and the binary is
  ALSA-only, which is what keeps CI and headless machines building unchanged.
  `make help` reports which backends are in.

## [1.0.0] - 2026-08-06

### Added

- The desktop app's device list follows the hardware. `device` gained a watch
  that re-walks ALSA every couple of seconds - about 0.3 ms for two cards - and
  the window rebuilds its dropdown whenever the answer differs, so an interface
  plugged in shows up without a restart and one unplugged leaves the list. The
  watch also listens on `/dev/snd`, the directory the kernel puts a card's
  nodes in the moment it registers one, which brings a plugged-in device up in
  well under a second where those events are delivered; they are not everywhere
  - a sandbox or a container can hold its own mount of devtmpfs, where the
  nodes come and go exactly as they do outside and no watch on them ever fires
  - so the sweep is the mechanism and inotify only shortens the wait. The
  rebuilt list is only swapped in when it actually differs, so an open menu
  cannot shuffle under the pointer that is about to click a row.
- The desktop app opens the capture stream again when the device it was using
  comes back, whether the window came up without it or its stream died with the
  cable. A dead stream cannot be revived, and re-picking the device in the
  dropdown does nothing because the row is already selected, so before this a
  device that returned was unreachable without restarting the app.
- A tuner. `audiaki --tune` opens the capture device and reports the pitch of
  whatever is being played as a note, a needle on a scale of half a semitone
  either side of it, and the frequency and level, until Ctrl+C. Nothing is
  written; it is a display, not a take. `--a4` moves the reference pitch for an
  ensemble tuned somewhere other than concert pitch.
- `tuner` module: monophonic pitch detection using the YIN difference function,
  plus the note arithmetic that turns a frequency into a note name, an octave
  and an offset in cents. Time domain rather than a peak over the spectrum,
  because a plucked low string often has more energy in its second harmonic than
  in its fundamental and a spectrum peak would report the octave above. No ALSA
  and no I/O, so it is unit tested like the rest of the analysis.
- A `tuner` style in the desktop app, alongside the five visualisers and reached
  the same ways - the strip on the stage, the `V` key, or `-s tuner` up front.
  It runs the same detection as `--tune`, and only while it is the visible
  style, so a video render is not slowed by a needle nobody is looking at.
- `tune` module: the `--tune` capture loop, which is to tuning what `recorder`
  is to a take. It keeps ALSA out of `tuner`, which the desktop app also uses.
- `meter_draw_tuner()` draws the terminal tuner line. With stderr redirected
  there is no line to redraw in place, so `--tune` reports each note once as it
  settles instead, which makes it something a script can log.
- `parse_double()` parses a bounded decimal as strictly as `parse_uint()` parses
  an integer; `--a4` and the duration fields both go through it.

- `audiaki-gui`, a desktop application. A window with a record / pause / resume
  / stop transport, a live level meter with peak hold, a clipping indicator and
  a real-time spectrum. The capture stream opens with the window and stays open,
  so levels and the spectrum are live before a take is started. Takes are always
  numbered from a prefix, so recording cannot overwrite an earlier one.
- A capture device dropdown in the desktop app, listing `default` alongside
  every capture PCM ALSA reports. Switching rebuilds the capture stream and the
  analyser around whatever the new device negotiates, and falls back to the
  previous device if the new one will not open. Disabled while a take is open,
  so a device change cannot truncate a recording.
- `aud_device_enumerate()` returns the capture device list as data rather than
  printing it; `--list` is now a presentation layer over it and its output is
  unchanged.
- Playback monitoring: a second ALSA stream plays the input back while it
  records, with a volume slider and a toggle. Off by default, because
  monitoring a microphone through speakers howls. It drops frames rather than
  queueing them when the output falls behind, so the monitor cannot drift
  further behind the input the longer a take runs.
- The desktop visualiser draws a stem per band with an additively blended
  glowing cap, after [musializer](https://github.com/tsoding/musializer). It
  reuses the same `spectrum` analyser as the terminal meter and the video
  renderer.
- Video capture in the desktop app: a **Video** toggle, or `-V, --video`, that
  renders an MP4 of the visualiser alongside the take's WAV. It is rendered
  from the finished take rather than captured live off the screen, so it cannot
  cost the recording an xrun and is frame-accurate regardless of the window's
  size or refresh rate; progress is shown in the status line and **Stop**
  becomes **Cancel** while it runs. `--video-size` and `--video-fps` set the
  output, defaulting to 1280x720 at 60. Needs `ffmpeg` on `PATH`; without it
  the WAV is still written and the window says why the video was not.
- An **Audio** toggle beside it, or `--video-silent`, choosing whether that MP4
  carries the take's audio. On by default; off encodes the video with no audio
  stream at all, rather than a muted one, for a clip going into an edit that
  has the sound already. `ffmpeg_start_rendering()` takes a NULL sound file for
  this. The take's WAV is written either way, and is still what the picture is
  drawn from.
- `aud_take_with_extension()` derives a video name from a take name. The CLI's
  `--visualize` default output now goes through it too, so the two cannot
  disagree about what `take-003.wav`'s video is called.
- Five live visualiser styles in the desktop app - `bars`, `mirror`, `radial`,
  `scope` and `waterfall` - switchable from a strip on the visualiser, with the
  `V` key, or up front with `-s, --style`. `scope` draws raw samples, triggered
  on a rising zero crossing so a steady note stands still; `waterfall` keeps
  about eight seconds of spectrogram history as a ring of texture columns, one
  written per frame.
- `monitor` module: ALSA playback for hearing the input, independent of the
  capture stream, so monitoring can fail or be switched off without the
  recording noticing.
- `ringbuf` module: a lock-free single-producer single-consumer float ring, so
  the capture thread hands audio to the drawing thread without a mutex that
  could stall it into an xrun. ALSA-free and unit tested.
- `aud_format_to_float()` decodes interleaved PCM to interleaved floats,
  keeping the channels apart, which is what monitoring needs.
- A `.desktop` entry, installed by `make install` when the desktop app is
  built, so audiaki appears in the application menu.
- `scripts/install-deps.sh` now installs the desktop app's OpenGL and X11
  headers too, across apt, dnf, pacman, zypper and apk, and takes `--no-gui`
  to leave them out on a headless machine.
- `--visualize FILE` renders a WAV recording into a spectrum visualiser video.
  audiaki analyses and rasterises the frames itself and pipes raw RGBA to
  `ffmpeg`, which encodes them and muxes in the original audio. `--size`,
  `--fps` and `--bars` control the output; `-o` names it, defaulting to the
  input path with `.mp4`. Inspired by
  [musializer](https://github.com/tsoding/musializer).
- `--spectrum` replaces the live peak bar with spectrum bars while recording,
  using block characters on UTF-8 terminals and an ASCII ramp elsewhere.
- `-o, --output` names the output file, for recording as well as rendering.
- A WAV reader (`wav_read_*`), tolerant of the chunk layouts other tools
  produce: unknown chunks are skipped, `WAVE_FORMAT_EXTENSIBLE` is unwrapped,
  and 8/16/24/32 bit PCM plus 32/64 bit float are decoded.
- `fft`, `spectrum` and `canvas` modules, all ALSA-free and unit tested. The
  live display and the video renderer share the same analyser.

### Changed

- A device the desktop app cannot open is no longer the end of the session. The
  window used to come up on the "no device" screen and stay there until it was
  closed; it now keeps the device dropdown live on that screen and opens
  whatever is picked there or plugged in afterwards.
- `aud_device_enumerate()` reports a machine with no sound cards as an empty
  list rather than an error, because a caller watching for hardware asks over
  and over and none of those asks is a failure. `--list` prints its header and
  the usual "no capture devices found" warning, and now exits 0 rather than 1.
- `ffmpeg` is a new optional run-time dependency, needed only for
  `--visualize`. Building and recording are unaffected.
- raylib is a new optional build-time dependency, vendored as a pinned
  submodule under `vendor/raylib` and needed only for `audiaki-gui`. `make`
  builds the desktop app when the submodule is initialised and the OpenGL and
  X11 headers are present, and quietly builds the command line recorder alone
  when they are not — so headless machines and CI are unaffected.
- `make install` now installs `audiaki-gui`, its `.desktop` entry and its icon
  as well, when the desktop app was built.
- The SIGINT/SIGTERM stop flag moved out of `recorder.c` into `signals.c` so
  the renderer can be interrupted too. `aud_recorder_install_signals()` and
  `aud_recorder_stop_requested()` still work as before.

### Fixed

- The tuner analysed the oldest samples in its buffer rather than the newest.
  The analysis span is `2 * tau_max + tau_max` samples but the buffer is that
  rounded up to a power of two, and the slack sat at the new end, so the
  newest 790 samples at 44.1 kHz - nearly 18 ms - were left out of every
  reading. Both the pitch and the gate that decides whether anything is being
  played described a moment that had already passed; a note is now picked up
  about 18 ms sooner. Affects `--tune` and the desktop app's tuner style.
- The desktop app missed clipping that ran into the positive rail. Signed PCM
  is asymmetric - the most negative sample normalises to exactly -1.0 but the
  most positive is one step short, 32767/32768 at 16 bit - so the engine's
  `peak >= 1.0` test only ever fired on a take that clipped downwards. Both the
  terminal meter and the app now share one `AUD_CLIP_THRESHOLD`.
- `--duration` is bounded, so `-t 1e308` is rejected instead of overflowing the
  conversion to a frame count. Being undefined behaviour it did not record for
  a very long time: the value converted to zero and the recorder wrote a single
  frame.
- Rendering to or from a path beginning with `-` works. ffmpeg reads any
  argument starting with a dash as an option and there is no `--` to stop it,
  so `-take01.wav` was parsed as flags and the render failed with
  "Unrecognized option". Such paths are now passed as `./-take01.wav`.
- The desktop app's device dropdown can reach every device. It drew only the
  first `AUD_UI_DROPDOWN_MAX_ROWS` entries with no way to scroll, so on a
  machine with more than eight capture devices the rest could not be selected
  at all - including the one named by `-D`, which is appended last. The list
  now scrolls on the mouse wheel and opens on the current selection.
- `audiaki-gui` validates `-r` and `-c` the way the recorder does. They went
  through bare `strtoul`, so `-c -5` became 4294967291 and `-r abc` became 0
  before being handed to libasound. The bounds now live in one place.
- The ffmpeg child no longer closes its own stdin if the pipe's read end lands
  on file descriptor 0, which can happen when audiaki is started with stdin
  already closed.

## [0.2.0] - 2026-08-04

The single-file recorder grew into a structured project. Behaviour is
compatible with 0.1.0 except for the default device, noted below.

### Added

- `--list` enumerates every capture-capable PCM device.
- `--format` pins the capture format instead of negotiating one.
- `--period` and `--periods` expose the buffer geometry.
- `--force` is now required to overwrite an existing recording; without it an
  existing file is an error rather than silently truncated.
- `--quiet`, `--verbose` and `--no-meter` control the output.
- `--version`, and long forms for every previously short-only option.
- `--duration` accepts `MM:SS` and `HH:MM:SS` as well as plain seconds.
- `AUDIAKI_DEVICE` sets the default device.
- Peak-hold marker and a clipping warning in the meter.
- Man page (`docs/audiaki.1`), installed by `make install`.
- Unit tests for the WAV writer, sample-format handling and option parsing,
  runnable without ALSA or a sound card.
- GitHub Actions CI: gcc and clang builds with warnings as errors, a build
  without ALSA installed, ASan/UBSan, clang-format, cppcheck and shellcheck.

### Changed

- **Default device is now `default` instead of `hw:CARD=Box,DEV=0`.** Set
  `AUDIAKI_DEVICE=hw:CARD=Box,DEV=0` to restore the old behaviour.
- Split the single `src/main.c` into focused modules; libasound is confined to
  `src/device.c`.
- `make` replaces `build.sh`, and `scripts/install-deps.sh` replaces
  `install.sh` with support for apt, dnf, pacman, zypper and apk.
- `--duration` now stops on an exact frame count instead of overshooting by up
  to one period.
- The meter refreshes on a time interval rather than every fourth period, so
  the rate no longer depends on the period size.
- The meter and status output are suppressed when stderr is not a terminal.
- Errors are reported consistently as `audiaki: error: ...` on stderr; stdout
  carries only `--list` and `--probe` output.

### Fixed

- The desktop app's video capture no longer creates the `.mp4` until it is
  finished. An MP4 is only playable once its moov atom is written, which ffmpeg
  does as the stream ends, so writing straight to the final name put a file on
  disk the moment recording stopped that looked like a finished take and opened
  as "no playable streams" for as long as the render ran - and stayed that way
  for good if the render was cancelled or interrupted. Frames now go to a
  hidden `.NAME.partial.mp4` that is renamed into place only on success.
- `aud_take_path()` no longer leaves a truncated filename in the caller's
  buffer when the name does not fit. It documented "dst untouched" but wrote
  through `snprintf` before checking the length, so a caller that tested the
  result second would have acted on half a name.
- Odd-sized payloads (24-bit mono) now get the RIFF pad byte required by the
  specification.
- Numeric options reject trailing garbage and negative values instead of
  silently accepting `-r 44100abc` or wrapping `-c -1` to a huge count.
- Capture and output buffers are freed on every error path.
- The 32-bit peak calculation no longer overflows on the most negative sample.
- `wav_close()` reports write and close failures instead of discarding them,
  so a full disk is no longer silently ignored.

## [0.1.0] - 2026-08-04

### Added

- Initial single-file ALSA capture-to-WAV recorder with format negotiation,
  a peak meter, `--probe`, and duration-limited recording.

[Unreleased]: https://github.com/HilthonTT/audiaki/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/HilthonTT/audiaki/releases/tag/v1.0.0
[0.2.0]: https://github.com/HilthonTT/audiaki/releases/tag/v0.2.0
[0.1.0]: https://github.com/HilthonTT/audiaki/releases/tag/v0.1.0
