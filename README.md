<p align="center">
  <img src="assets/logo.png" alt="audiaki logo" width="140">
</p>

<h1 align="center">audiaki</h1>

<p align="center">
  Minimal ALSA capture-to-WAV recorder for Linux, with live metering and a
  spectrum visualiser.
</p>

<p align="center">
  <a href="https://github.com/HilthonTT/audiaki/actions/workflows/ci.yml"><img src="https://github.com/HilthonTT/audiaki/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"></a>
</p>

A small ALSA capture-to-WAV recorder for Linux. It opens a capture device,
picks the best sample format the hardware actually supports, and streams it
straight into a PCM WAV file with a live meter. It can also turn a finished
take into a spectrum visualiser video.

Written for a Sonicake Smart Box (QME-20) guitar interface, but it works with
any ALSA capture device — USB interfaces, built-in codecs, `plughw` plugins.

```
 00:12 [##################|          ]  -8.4 dBFS  xruns:0
 00:12 ▁▂▄█▆▃▂▁▁▂▃▅▇▆▄▂▁▁▁▂▁▁  -8.4 dBFS  xruns:0
```

## Why not `arecord`?

`arecord` is more general. `audiaki` is a single small binary aimed at one job:
plug in an instrument, get a clean take, see the level while you play.

- Negotiates the widest format the device offers (S32 → S24 → S16) instead of
  defaulting to 16-bit.
- Live peak meter with a peak-hold marker and clipping warning, or a live
  spectrum with `--spectrum`.
- Renders a take to a spectrum visualiser video with `--visualize`, in the
  spirit of [musializer](https://github.com/tsoding/musializer).
- Stops on an exact frame count for `--duration`, so a 30 second take is
  30.000 seconds.
- Refuses to overwrite an existing take unless you pass `--force`.
- Patches the WAV header on exit, including on Ctrl+C, so interrupted
  recordings are still valid files.

## Install

Requires a C11 compiler, `make`, and the ALSA development headers. Rendering a
video also needs `ffmpeg` on `PATH` at run time — it is not needed to build
audiaki or to record with it.

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
audiaki --spectrum take01.wav        # record, watching the spectrum
audiaki -t 1:30 take02.wav           # record 90 seconds
audiaki -D plughw:CARD=Box,DEV=0 -r 48000 -c 2 take03.wav
audiaki --visualize take01.wav       # render take01.mp4
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
| `--spectrum` | Live spectrum bars instead of the peak bar |
| `--no-meter` | Do not draw anything while recording |
| `--visualize FILE` | Render a WAV to a visualiser video and exit |
| `-o, --output FILE` | Output file (video default: input with `.mp4`) |
| `--size SPEC` | `WxH`, or `480p`/`720p`/`1080p`/`1440p`/`2160p` (default `1280x720`) |
| `--fps N` | Video frame rate (default 60) |
| `--bars N` | Spectrum bar count (default 64) |
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

With `--spectrum` the bar is replaced by one column per frequency band, low
frequencies on the left, log-spaced from 40 Hz to 12 kHz:

```
 00:12 ▁▂▄█▆▃▂▁▁▂▃▅▇▆▄▂▁▁▁▂▁▁  -8.4 dBFS  xruns:0
       ^ 40 Hz                    ^ 12 kHz
```

Block characters are used when the locale is UTF-8, and an ASCII ramp
otherwise.

## Rendering a video

```sh
audiaki --visualize take01.wav                       # -> take01.mp4, 1280x720
audiaki --visualize take01.wav -o clip.mp4 --size 1080p --fps 30 --bars 96
```

audiaki does the analysis and the drawing itself, then pipes raw RGBA frames to
`ffmpeg`, which encodes them and muxes in the original audio untouched. There is
no graphics library involved: the bars are rasterised into a plain pixel buffer.

Bands come from a 2048-point Hann-windowed FFT, spaced logarithmically so an
octave takes the same width wherever it falls. Each band shows the strongest bin
it covers and rises fast but falls slowly, so a plucked note reads as an attack
rather than a flicker. Colour runs cyan at the bass end to pink at the treble
end, and the marker above each bar holds its recent peak.

Ctrl+C during a render stops ffmpeg and removes the partial video.

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

**`could not run ffmpeg`**
`ffmpeg` is not on `PATH`. Install it (`apt install ffmpeg`, `dnf install
ffmpeg` or `pacman -S ffmpeg`) and try again. Only `--visualize` needs it.

**`--size 1365x768: both dimensions must be even`**
The H.264 encoder subsamples chroma by two, so odd frame sizes cannot be
encoded. Round to an even number, or use a `720p`-style shorthand.

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
  wav.c/.h      streaming WAV writer, and a tolerant WAV reader
  format.c/.h   sample formats, peak detection, repacking
  meter.c/.h    the terminal peak and spectrum displays
  fft.c/.h      radix-2 FFT and the Hann window
  spectrum.c/.h streaming analyser: samples in, bar heights out
  canvas.c/.h   RGBA framebuffer and the shapes the visualiser draws
  visualize.c/.h render a WAV to a video
  ffmpeg.h      pipe frames to an ffmpeg child
  ffmpeg_posix.c  its fork/exec/pipe implementation
  signals.c/.h  the shared Ctrl+C flag
  parse.c/.h    strict CLI value parsing
  log.c/.h      stderr diagnostics
tests/          unit tests for the ALSA-free modules
docs/           man page
```

ALSA lives behind `device.c` alone, so `format`, `wav`, `parse`, `fft`,
`spectrum` and `canvas` are plain C that can be built and tested anywhere —
which is what CI does. The analysis and drawing code is shared: the live
terminal display and the video renderer run the same `spectrum` module.

## Limitations

- Little-endian hosts only; WAV is little-endian and no byte swapping is done.
- Plain 44-byte PCM WAV, so recordings stop at the 4 GB RIFF limit
  (about 3.5 hours of 24-bit stereo at 48 kHz).
- Linux/ALSA only. No PipeWire, JACK or CoreAudio backend.
- The visualiser renders offline from a file. There is no windowed real-time
  view; live feedback is the terminal spectrum.
- Rendering shells out to `ffmpeg`, so the codecs and their licensing are its
  business, not audiaki's.

## Contributing

Bug reports and patches are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

The logo is the
[teriyaki icon](https://www.flaticon.com/free-icon/teriyaki_6632525) by
**AUTHOR** from [Flaticon](https://www.flaticon.com/), used under the
[Flaticon Free License](https://www.freepikcompany.com/legal#nav-flaticon-agreement)
with attribution.

## License

MIT. See [LICENSE](LICENSE) — this covers the source code. The logo in
`assets/` is licensed separately, as noted under [Credits](#credits).
