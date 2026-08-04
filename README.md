# audiaki

[![CI](https://github.com/HilthonTT/audiaki/actions/workflows/ci.yml/badge.svg)](https://github.com/HilthonTT/audiaki/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A small ALSA capture-to-WAV recorder for Linux. It opens a capture device,
picks the best sample format the hardware actually supports, and streams it
straight into a PCM WAV file with a live peak meter.

Written for a Sonicake Smart Box (QME-20) guitar interface, but it works with
any ALSA capture device — USB interfaces, built-in codecs, `plughw` plugins.

```
 00:12 [##################|          ]  -8.4 dBFS  xruns:0
```

## Why not `arecord`?

`arecord` is more general. `audiaki` is a single small binary aimed at one job:
plug in an instrument, get a clean take, see the level while you play.

- Negotiates the widest format the device offers (S32 → S24 → S16) instead of
  defaulting to 16-bit.
- Live peak meter with a peak-hold marker and clipping warning.
- Stops on an exact frame count for `--duration`, so a 30 second take is
  30.000 seconds.
- Refuses to overwrite an existing take unless you pass `--force`.
- Patches the WAV header on exit, including on Ctrl+C, so interrupted
  recordings are still valid files.

## Install

Requires a C11 compiler, `make`, and the ALSA development headers.

```sh
./scripts/install-deps.sh   # or install libasound2-dev / alsa-lib-devel yourself
make
sudo make install           # installs to /usr/local by default
```

Install somewhere else with `PREFIX`:

```sh
make install PREFIX=~/.local
```

## Usage

```sh
audiaki --list                       # which capture devices exist
audiaki --probe -D hw:CARD=Box,DEV=0 # what that device supports
audiaki take01.wav                   # record until Ctrl+C
audiaki -t 1:30 take02.wav           # record 90 seconds
audiaki -D plughw:CARD=Box,DEV=0 -r 48000 -c 2 take03.wav
```

| Option | Description |
| --- | --- |
| `-D, --device NAME` | ALSA device (default `default`, or `$AUDIAKI_DEVICE`) |
| `-r, --rate HZ` | Sample rate (default 44100) |
| `-c, --channels N` | Channel count (default 2) |
| `-f, --format NAME` | Force `s16_le`, `s24_3le`, `s24_le` or `s32_le` |
| `-t, --duration SPEC` | Stop after `SS`, `MM:SS` or `HH:MM:SS` |
| `-p, --period FRAMES` | Period size (default 1024) |
| `-n, --periods N` | Periods per buffer (default 4) |
| `-y, --force` | Overwrite an existing output file |
| `--no-meter` | Do not draw the peak meter |
| `-q, --quiet` / `-v, --verbose` | Less / more diagnostic output |
| `-l, --list` / `-P, --probe` | Inspect devices and exit |

Full details: `man audiaki` after installing, or `audiaki --help`.

Set a default device once instead of typing `-D` every time:

```sh
export AUDIAKI_DEVICE=hw:CARD=Box,DEV=0
```

## Reading the meter

```
 00:12 [##################|          ]  -8.4 dBFS  xruns:0
       ^ current level    ^ peak hold              ^ buffer overruns
```

Aim for peaks around −6 dBFS. If `CLIP` appears, the signal hit full scale and
the take is distorted — turn the level down on the device, not in software.
`xruns` counting up means the machine could not keep up; try a larger
`--period` or more `--periods`.

## Troubleshooting

**`cannot open capture device 'default': Device or resource busy`**
PipeWire or PulseAudio holds the card exclusively. Record through the plug
layer instead: `-D plughw:CARD=Box,DEV=0`.

**`cannot set 2 channel(s)`**
The device is mono-only, or wants a different count. Run `--probe` to see the
supported range, then pass `-c`.

**Rate warning: `requested 44100 Hz, device negotiated 48000 Hz`**
The hardware does not support the rate you asked for and ALSA picked the
nearest one. The file is written at the rate that was actually used.

**Constant `xruns`**
Increase `--period` (for example `-p 2048`) or `--periods`, and avoid running
the recording from a heavily loaded terminal.

## Development

```sh
make            # build build/audiaki
make test       # unit tests (no ALSA device or headers needed)
make debug      # -O0 with AddressSanitizer and UBSan
make check      # tests plus clang-format check
make format     # apply clang-format
make STRICT=1   # warnings become errors, as in CI
```

### Layout

```
src/
  main.c        entry point; dispatches the parsed command
  cli.c/.h      argument parsing and help text
  device.c/.h   the only code that touches libasound
  recorder.c/.h capture loop: device -> repack -> WAV
  wav.c/.h      streaming WAV writer
  format.c/.h   sample formats, peak detection, repacking
  meter.c/.h    the terminal peak meter
  parse.c/.h    strict CLI value parsing
  log.c/.h      stderr diagnostics
tests/          unit tests for the ALSA-free modules
docs/           man page
```

ALSA lives behind `device.c` alone, so `format`, `wav` and `parse` are plain C
that can be built and tested anywhere — which is what CI does.

## Limitations

- Little-endian hosts only; WAV is little-endian and no byte swapping is done.
- Plain 44-byte PCM WAV, so recordings stop at the 4 GB RIFF limit
  (about 3.5 hours of 24-bit stereo at 48 kHz).
- Linux/ALSA only. No PipeWire, JACK or CoreAudio backend.

## Contributing

Bug reports and patches are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT. See [LICENSE](LICENSE).
