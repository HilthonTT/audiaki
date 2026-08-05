<p align="center">
  <img src="assets/logo.png" alt="audiaki logo" width="140">
</p>

<h1 align="center">audiaki</h1>

<p align="center">
  Minimal ALSA capture-to-WAV recorder for Linux, with live metering, a
  spectrum visualiser and a desktop app.
</p>

<p align="center">
  <a href="https://github.com/HilthonTT/audiaki/actions/workflows/ci.yml"><img src="https://github.com/HilthonTT/audiaki/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"></a>
</p>

A small ALSA capture-to-WAV recorder for Linux. It opens a capture device,
picks the best sample format the hardware actually supports, and streams it
straight into a PCM WAV file with a live meter. It can also turn a finished
take into a spectrum visualiser video.

It ships as two binaries: `audiaki`, the command line recorder, and
`audiaki-gui`, a desktop window with a transport, playback monitoring and a
live glowing spectrum. Both are built from the same capture and analysis core.

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
- Renders a take to a visualiser video with `--visualize`, in the
  spirit of [musializer](https://github.com/tsoding/musializer).
- Measures a finished take with `--info`: peak, RMS, noise floor, DC offset and
  how many samples clipped.
- Stops on an exact frame count for `--duration`, so a 30 second take is
  30.000 seconds.
- Numbers takes for you with `--take`, so tracking a part does not mean
  inventing a filename every time.
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

The script handles apt, dnf, pacman, zypper and apk, and takes `--dry-run` to
show what it would install. By default it also pulls in `ffmpeg` and the
desktop app's OpenGL and X11 headers; `--no-ffmpeg` and `--no-gui` skip those.

Install somewhere else with `PREFIX`:

```sh
make install PREFIX=~/.local
```

### Building the desktop app

`audiaki-gui` needs [raylib](https://www.raylib.com/), which is not packaged by
Debian or Ubuntu, so it is vendored as a submodule and pinned to the version the
visualiser was drawn against. It also needs the OpenGL and X11 development
headers, which `install-deps.sh` installs unless you pass `--no-gui`.

```sh
./scripts/install-deps.sh              # OpenGL and X11 headers included
git submodule update --init --depth 1  # fetch raylib
make
```

`make` then builds both binaries; raylib itself is compiled once and reused.
Installing the headers by hand instead, on Debian or Ubuntu:

```sh
sudo apt install libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev \
                 libxcursor-dev libxinerama-dev libxkbcommon-dev
```

None of this is required for the command line recorder. If the submodule is
not initialised, `make` quietly builds `audiaki` alone and skips the window —
which is what you want on a headless machine.

`sudo make install` also installs a `.desktop` entry, so audiaki appears in the
application menu alongside everything else.

## Usage

```sh
audiaki --list                       # which capture devices exist
audiaki --probe -D hw:CARD=Box,DEV=0 # what that device supports
audiaki take01.wav                   # record until Ctrl+C
audiaki --spectrum take01.wav        # record, watching the spectrum
audiaki -t 1:30 take02.wav           # record 90 seconds
audiaki --take session               # record the next free session-NNN.wav
audiaki --info take01.wav            # how did that take come out?
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
| `--take PREFIX` | Write the next free `PREFIX-001.wav` |
| `--spectrum` | Live spectrum bars instead of the peak bar |
| `--no-meter` | Do not draw anything while recording |
| `--visualize FILE` | Render a WAV to a visualiser video and exit |
| `-o, --output FILE` | Output file (video default: input with `.mp4`) |
| `--style NAME` | `bars`, `scope` or `waveform` (default `bars`) |
| `--size SPEC` | `WxH`, or `480p`/`720p`/`1080p`/`1440p`/`2160p` (default `1280x720`) |
| `--fps N` | Video frame rate (default 60) |
| `--bars N` | Spectrum bar count (default 64) |
| `--info FILE` | Report levels and clipping for a WAV and exit |
| `--json` | Machine-readable `--list`, `--probe` and `--info` |
| `-q, --quiet` / `-v, --verbose` | Less / more diagnostic output |
| `-l, --list` / `-P, --probe` | Inspect devices and exit |

Full details: `man audiaki` after installing, or `audiaki --help`.

Set a default device once instead of typing `-D` every time:

```sh
export AUDIAKI_DEVICE=hw:CARD=Box,DEV=0
```

## The desktop app

<p align="center">
  <img src="screenshots/desktop.png" alt="the audiaki desktop app" width="820">
</p>

```sh
audiaki-gui                          # open the window on the default device
audiaki-gui -D plughw:CARD=Box,DEV=0 # ...on a particular interface
audiaki-gui -o session               # name takes session-001.wav and up
audiaki-gui -s waterfall             # start on a particular visualiser
audiaki-gui -V                       # also render an MP4 of each take
audiaki-gui -V --video-size 1080p    # ...at a particular size
audiaki-gui -V --video-silent        # ...with no audio track in it
audiaki-gui -M                       # come up already monitoring
```

The capture stream opens with the window and stays open, so the spectrum moves
and the meter reads before you press anything — setting an input level should
not mean starting a take you are going to throw away.

| Control | Does |
| --- | --- |
| **Record** | Starts the next numbered take |
| **Pause** / **Resume** | Stops and continues writing, without closing the file |
| **Stop** | Patches the WAV header and closes the take |
| **Video** | Also render an MP4 of the visualiser when the take stops |
| **Audio** | Whether that MP4 carries the take's audio; off renders it silent |
| **Monitor** | Plays the input back through the default output |
| Volume slider | Monitoring level, from silent to +6 dB |
| Device dropdown | Switches capture device, `default` plus every capture PCM |

The device dropdown is disabled while a take is open: switching means closing
the capture stream, and doing that mid-take would truncate the recording. Stop
first. If the new device will not open, audiaki falls back to the previous one
rather than leaving the window with no audio.

| Key | Does |
| --- | --- |
| `space` | Record, or pause and resume once a take is running |
| `S` | Stop |
| `M` | Toggle monitoring |
| `V` | Next visualiser style |
| `F` | Fullscreen |

Takes are always numbered from the prefix, so there is no overwrite prompt and
no `--force` to get wrong: pressing record cannot destroy an earlier take.

### Recording video

**Video** off is audio only — `take-003.wav` and nothing else. Video on writes
that same WAV and then `take-003.mp4` alongside it, showing whichever
visualiser was selected, with the take's own audio muxed in. It needs `ffmpeg`
on `PATH`; without it the WAV is still written and the window says why the
video was not.

**Audio**, beside it, decides whether that MP4 gets an audio track at all. On
by default — a take and its visualiser belong together. Turn it off, or start
with `--video-silent`, and the video is encoded with no audio stream in it, for
a clip going into an edit that already has the sound, or somewhere it should
not play. The WAV is written either way, so nothing is lost by choosing wrong;
it is also what the picture is drawn from, silent video or not. Like **Video**,
it is only settable between takes: the render is a single pass over the
finished take, so there is no half of it to change your mind about.

The video is rendered **after** the take stops, not captured live off the
screen. Recording is the job that must not miss a deadline, and grabbing the
framebuffer sixty times a second on the same machine that is holding a capture
stream open is how takes end up with xruns in them. Rendering afterwards costs
the wait, but it is frame-accurate, independent of the window's size and
refresh rate, and cannot drop a frame.

While it runs, **Stop** becomes **Cancel** and the status line shows progress.
Cancelling removes the partial video and keeps the WAV. On this machine a
4-second take renders in about 5 seconds at 720p60 — roughly real time, so
budget about as long as the take itself.

The `.mp4` only appears once it is finished. An MP4 is not playable until its
final index is written, so while the render runs the frames go to a hidden
`.take-003.partial.mp4` and the real name is created by a rename at the end.
If the render is cancelled, fails, or the app is killed, you are left with the
take and no video, rather than a file that looks like a video and will not
open.

`--video-size` takes `WxH` or `720p`/`1080p`/`1440p`/`2160p`, and `--video-fps`
the frame rate; the defaults are 1280x720 at 60.

The CLI's `--visualize` renders video too, from a WAV you already have. It has
its own three styles and does not need a display, so it is the one to use on a
headless box or in a script.

**Monitoring feeds your input back to your speakers**, which will howl if you
are recording a microphone in the same room. It starts off for that reason.
Headphones, or an instrument rather than a mic, and it is fine.

### The visualisers

<p align="center">
  <img src="screenshots/styles.png" alt="the five visualiser styles" width="820">
</p>

Five styles, switchable from the strip on the visualiser or with `V`:

| Style | Shows |
| --- | --- |
| `bars` | A stem per band with a glowing cap, growing from the floor |
| `mirror` | The same bars, opening from the centre line |
| `radial` | The spectrum wrapped into a ring, bass at the top |
| `scope` | An oscilloscope trace of the last few milliseconds |
| `waterfall` | A scrolling spectrogram, newest at the right |

All five read the same analysis the CLI's `--spectrum` and `--visualize` use:
a 2048 point window folded into log-spaced bands, with a fast attack and a slow
decay. `bars` is the default, after
[musializer](https://github.com/tsoding/musializer) — the glow is one radial
gradient texture drawn additively, so overlapping halos sum towards white and
loud clusters bloom. `mirror` and `radial` reuse the same caps.

`scope` is the odd one out: it draws raw samples rather than the spectrum, and
starts each sweep at a rising zero crossing so a steady note stands still
instead of scrolling. It shows true amplitude, so a quiet input is a quiet
trace — that is the meter's job to explain, not the scope's.

`waterfall` is the only one with a memory. It keeps about eight seconds of
history as a ring of texture columns, one written per frame, so a hum or a
dropout is still on screen after it has happened.

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

## Numbering takes

Tracking a part means playing it several times. `--take` names each attempt for
you instead of making you invent one:

```sh
audiaki --take session      # session-001.wav
audiaki --take session      # session-002.wav, because 001 is there
```

It picks the first number that is free, so it never overwrites anything and
`--force` never comes into it. A prefix that already carries an extension keeps
it — `--take session.wav` also writes `session-001.wav` — and a path works as
well as a bare name: `--take takes/riff`.

## Checking a take

```
$ audiaki --info take01.wav
file:        take01.wav
format:      24 bit PCM
channels:    2
rate:        48000 Hz
duration:    01:12.34  (3472320 frames)
peak:        -4.4 dBFS
rms:         -10.7 dBFS
noise floor: -68.2 dBFS
clipped:     0 sample(s)
channels:    ch 1: peak   -4.4 dBFS  rms   -8.7 dBFS  dc +0.00000
             ch 2: peak  -10.5 dBFS  rms  -14.7 dBFS  dc -0.00029
```

Peak is the loudest single sample; if `clipped` is not zero, that many samples
sat at full scale and the take is distorted. Each channel is measured on its own,
so a clipped left channel cannot hide behind a quiet right one.

The noise floor is the tenth percentile of the level of 50 ms windows — in
practice, the room and the cable with nothing being played. Compare it with the
peak to see how much of the range the instrument is actually using. A DC offset
much past ±0.001 usually means the interface has a bias worth fixing.

`--info` reads whatever the reader accepts, including files other tools wrote:
8/16/24/32-bit PCM and 32/64-bit float. A take that was interrupted before its
header was patched is reported as truncated rather than refused.

## Scripting

`--list`, `--probe` and `--info` take `--json` and write a single object or
array to stdout, with diagnostics staying on stderr:

```sh
audiaki --list --json | jq -r '.[].device'
audiaki --info take01.wav --json | jq '.peak_dbfs, .clipped_samples'
```

## Rendering a video

```sh
audiaki --visualize take01.wav                       # -> take01.mp4, 1280x720
audiaki --visualize take01.wav -o clip.mp4 --size 1080p --fps 30 --bars 96
audiaki --visualize take01.wav --style waveform
```

audiaki does the analysis and the drawing itself, then pipes raw RGBA frames to
`ffmpeg`, which encodes them and muxes in the original audio untouched. There is
no graphics library involved: everything is rasterised into a plain pixel buffer.

`--style` picks what a frame shows:

| Style | Shows |
| --- | --- |
| `bars` (default) | The spectrum at this instant, as log-spaced bars |
| `scope` | An oscilloscope trace of the last 40 ms |
| `waveform` | The whole take's envelope, with a playhead crossing it |

For `bars`, bands come from a 2048-point Hann-windowed FFT, spaced
logarithmically so an octave takes the same width wherever it falls. Each band
shows the strongest bin it covers and rises fast but falls slowly, so a plucked
note reads as an attack rather than a flicker. Colour runs cyan at the bass end
to pink at the treble end, and the marker above each bar holds its recent peak.

`scope` draws the sample values themselves, so you see the shape of the wave
rather than its content — useful for spotting asymmetry or a clipped flat top.
Its colour follows level, not frequency, because its horizontal axis is time.

`waveform` reads the file once up front to reduce it to one min/max pair per
column, then sweeps a playhead across. The cost of that pass depends on the
width of the video, not the length of the take, so an hour is no more expensive
than a minute. `--bars` applies only to `bars`.

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
  info.c/.h     measure a finished take: levels, clipping, noise floor
  take.c/.h     numbered take filenames for --take
  jsonout.c/.h  the little JSON --json needs
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
  monitor.c/.h  ALSA playback, for hearing the input while it records
  ringbuf.c/.h  lock-free SPSC ring, capture thread -> drawing thread
  log.c/.h      stderr diagnostics
  gui/
    app.c       the desktop window: layout, transport, keys
    engine.c/.h the capture thread and its idle/recording/paused transport
    viz.c/.h    the glowing spectrum, drawn with raylib
    ui.c/.h     immediate-mode buttons, slider and meter
tests/          unit tests for the ALSA-free modules
docs/           man page
vendor/raylib/  submodule, only needed for the desktop app
```

ALSA lives behind `device.c` and `monitor.c`, so `format`, `wav`, `parse`,
`fft`, `spectrum`, `canvas`, `info`, `take`, `ringbuf` and `jsonout` are plain C
that can be built and tested anywhere — which is what CI does. The analysis is
shared three ways: the terminal display, the video renderer and the desktop
app all run the same `spectrum` module.

`src/gui/` is the only code that knows raylib exists, and nothing in `src/`
depends on it, which is what keeps the CLI buildable with the submodule absent.

## Limitations

- Little-endian hosts only; WAV is little-endian and no byte swapping is done.
- Plain 44-byte PCM WAV, so recordings stop at the 4 GB RIFF limit
  (about 3.5 hours of 24-bit stereo at 48 kHz).
- Linux/ALSA only. No PipeWire, JACK or CoreAudio backend.
- Rendering shells out to `ffmpeg`, so the codecs and their licensing are its
  business, not audiaki's.
- The desktop app records to numbered takes in the working directory. There is
  no file dialog, and no playback of a finished take.
- Video is rendered after the take, not captured live, so recording a long take
  with video on means waiting roughly its own length again before the next one.
  Cancelling is always available, and the audio is safe on disk regardless.
- Monitoring needs an output that accepts the capture rate directly. audiaki
  does not resample, so it declines to monitor rather than play back at the
  wrong pitch.
- The desktop app's device list is built at startup, so an interface plugged in
  afterwards needs a restart to appear. Rate and channels are fixed for the
  session; only the device can be changed from the window.

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
