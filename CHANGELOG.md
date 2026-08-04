# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
