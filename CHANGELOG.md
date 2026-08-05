# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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

[Unreleased]: https://github.com/HilthonTT/audiaki/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/HilthonTT/audiaki/releases/tag/v0.2.0
[0.1.0]: https://github.com/HilthonTT/audiaki/releases/tag/v0.1.0
