<p align="center">
  <img src="assets/logo.png" alt="audiaki logo" width="140">
</p>

<h1 align="center">audiaki</h1>

<p align="center">
  Minimal capture-to-WAV recorder for Linux, with live metering, a
  spectrum visualiser and a desktop app. Talks to PipeWire or ALSA.
</p>

<p align="center">
  <a href="https://github.com/HilthonTT/audiaki/actions/workflows/ci.yml"><img src="https://github.com/HilthonTT/audiaki/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"></a>
</p>

audiaki opens a capture device, picks the best sample format available, and
streams it straight into a PCM WAV file with a live meter. It can also turn a
finished take into a spectrum visualiser video.

It talks to PipeWire where there is one and to ALSA where there is not, picking
whichever answers without being told. Through PipeWire it coexists with whatever
else has the interface open, names devices the way the rest of the desktop does,
and can record another application's output; through ALSA it opens the card with
nothing in between.

Two binaries share one capture and analysis core: `audiaki`, the command line
recorder, and `audiaki-gui`, a desktop window with a transport, playback
monitoring and a live glowing spectrum. Written for a Sonicake Smart Box
(QME-20) guitar interface, but it works with any ALSA capture device — USB
interfaces, built-in codecs, `plughw` plugins.

```
 00:12 [##################|          ]  -8.4 dBFS  xruns:0
 00:12 ▁▂▄█▆▃▂▁▁▂▃▅▇▆▄▂▁▁▁▂▁▁  -8.4 dBFS  xruns:0
```

## Why not `arecord`?

`arecord` is more general. `audiaki` is a single small binary aimed at one job:
plug in an instrument, get a clean take, see the level while you play.

- Speaks PipeWire as well as ALSA, so it does not have to be told to get out of
  the way of the browser holding the same interface.
- Negotiates the widest format the device offers (S32 → S24 → S16) instead of
  defaulting to 16-bit.
- Live peak meter with peak-hold and a clipping warning, or a live spectrum
  with `--spectrum`.
- Tunes the instrument with `--tune`, so getting in tune does not mean opening
  something else first.
- Renders a take to a visualiser video with `--visualize`, in the spirit of
  [musializer](https://github.com/tsoding/musializer).
- Measures a finished take with `--info`: peak, RMS, noise floor, DC offset and
  clipped sample count.
- Keeps the seconds before you pressed record with `--preroll`, so the take you
  played while deciding whether to record is still there.
- Stops on an exact frame count for `--duration`, so a 30 second take is
  30.000 seconds.
- Numbers takes with `--take`, so tracking a part does not mean inventing a
  filename every time.
- Refuses to overwrite an existing take unless you pass `--force`.
- Patches the WAV header on exit, including on Ctrl+C, so interrupted
  recordings are still valid files.

## Install

Requires a C11 compiler, `make`, and the ALSA development headers. The PipeWire
headers (`libpipewire-0.3-dev`) are optional: with them present the PipeWire
backend is compiled in, without them `make` quietly builds the ALSA-only binary
and everything else works unchanged. Rendering a video also needs `ffmpeg` on
`PATH` at run time — not to build or to record.

```sh
./scripts/install-deps.sh   # or install libasound2-dev / alsa-lib-devel yourself
make
sudo make install           # installs to /usr/local; override with PREFIX=~/.local
```

The script handles apt, dnf, pacman, zypper and apk, and takes `--dry-run` to
show what it would install. By default it also pulls in the PipeWire headers,
`ffmpeg`, and the desktop app's OpenGL and X11 headers; `--no-pipewire`,
`--no-ffmpeg` and `--no-gui` skip those.

### Building the desktop app

`audiaki-gui` needs [raylib](https://www.raylib.com/), which Debian and Ubuntu
do not package, so it is vendored as a submodule pinned to the version the
visualiser was drawn against. It also needs the OpenGL and X11 headers that
`install-deps.sh` installs unless you pass `--no-gui`.

```sh
./scripts/install-deps.sh              # OpenGL and X11 headers included
git submodule update --init --depth 1  # fetch raylib
make                                   # both binaries; raylib compiled once
```

Installing the headers by hand instead, on Debian or Ubuntu:

```sh
sudo apt install libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev \
                 libxcursor-dev libxinerama-dev libxkbcommon-dev
```

None of this is required for the command line recorder. With the submodule
uninitialised, `make` quietly builds `audiaki` alone and skips the window —
which is what you want on a headless machine. `sudo make install` also installs
a `.desktop` entry.

## Usage

```sh
audiaki --list                       # which capture devices exist
audiaki --probe -D hw:CARD=Box,DEV=0 # what that device supports
audiaki --tune                       # tune up before playing anything
audiaki take01.wav                   # record until Ctrl+C
audiaki --spectrum take01.wav        # record, watching the spectrum
audiaki -t 1:30 take02.wav           # record 90 seconds
audiaki --take session               # record the next free session-NNN.wav
audiaki --preroll 10 take04.wav      # keep the 10 seconds before Enter
audiaki --info take01.wav            # how did that take come out?
audiaki -D plughw:CARD=Box,DEV=0 -r 48000 -c 2 take03.wav
audiaki --visualize take01.wav       # render take01.mp4
```

| Option | Description |
| --- | --- |
| `-D, --device NAME` | Capture device (default `default`, or `$AUDIAKI_DEVICE`) |
| `--backend NAME` | `auto`, `pipewire` or `alsa` (default `auto`, or `$AUDIAKI_BACKEND`) |
| `-r, --rate HZ` | Sample rate (default 44100) |
| `-c, --channels N` | Channel count (default 2) |
| `-f, --format NAME` | Force `s16_le`, `s24_3le`, `s24_le` or `s32_le` |
| `-t, --duration SPEC` | Stop after `SS`, `MM:SS` or `HH:MM:SS` |
| `-p, --period FRAMES` | Period size (default 1024) |
| `-n, --periods N` | Periods per buffer (default 4) |
| `-y, --force` | Overwrite an existing output file |
| `--take PREFIX` | Write the next free `PREFIX-001.wav` |
| `--preroll SECS` | Hold SECS and wait for Enter; the take starts that far back |
| `--spectrum` | Live spectrum bars instead of the peak bar |
| `--no-meter` | Do not draw anything while recording |
| `--visualize FILE` | Render a WAV to a visualiser video and exit |
| `-o, --output FILE` | Output file (video default: input with `.mp4`) |
| `--style NAME` | `bars`, `scope` or `waveform` (default `bars`) |
| `--size SPEC` | `WxH`, or `480p`/`720p`/`1080p`/`1440p`/`2160p` (default `1280x720`) |
| `--fps N` | Video frame rate (default 60) |
| `--bars N` | Spectrum bar count (default 64) |
| `--tune` | Show the pitch of what is being played, until Ctrl+C |
| `--a4 HZ` | Tuner reference pitch (default 440) |
| `--info FILE` | Report levels and clipping for a WAV and exit |
| `--json` | Machine-readable `--list`, `--probe` and `--info` |
| `-q, --quiet` / `-v, --verbose` | Less / more diagnostic output |
| `-l, --list` / `-P, --probe` | Inspect devices and exit |

Full details: `man audiaki` after installing, or `audiaki --help`. Set
`AUDIAKI_DEVICE=hw:CARD=Box,DEV=0` to stop typing `-D` every time.

## Backends

audiaki talks to one of two audio systems, and picks without being asked: if a
PipeWire daemon answers, it uses PipeWire; otherwise ALSA. `--backend` overrides
that, and `$AUDIAKI_BACKEND` sets a default.

```sh
audiaki --list                   # whichever answers
audiaki --backend alsa --list    # the cards, whatever else is running
audiaki --backend pipewire -t 30 take01.wav
```

|  | `pipewire` | `alsa` |
| --- | --- | --- |
| Shares the interface | Yes, with anything else | No, one program at a time |
| Device names | `alsa_input.usb-...` — as the desktop shows them | `hw:CARD=Box,DEV=0` |
| Devices appearing | The server says so, at once | Watches `/dev/snd`, and sweeps |
| Format | Anything asked for, by conversion | What the hardware offers |
| Records other apps | Yes, through a sink's monitor | No |
| Monitoring | Any rate; the server resamples | Only if the output takes the capture rate |
| Needs | A running daemon | Nothing |

Asking for a backend that is not there is an error rather than a quiet
downgrade: `--backend pipewire` on a machine with no daemon says so, instead of
recording through ALSA and leaving you to wonder why the device names changed.

### Recording another application

A PipeWire sink appears in `--list` alongside the inputs, described as a monitor.
Recording one captures what is being *played* to it — a browser, a synth, a
video call — which is not something opening the card can do.

```sh
$ audiaki --list
DEVICE                                     DESCRIPTION
alsa_output.pci-0000_00_1f.3.analog-stereo Built-in Audio Analog Stereo: monitor of this output
alsa_input.pci-0000_00_1f.3.analog-stereo  Built-in Audio Analog Stereo: Audio/Source

$ audiaki -D alsa_output.pci-0000_00_1f.3.analog-stereo -t 30 desktop.wav
```

### What `--probe` means on each

Under ALSA, `--probe` asks the hardware what it supports, and the answer decides
what a recording can be. Under PipeWire the server converts, so anything audiaki
asks for is what it gets and the hardware's own list no longer governs — the
probe says so rather than printing a capability table that decides nothing. For
the question "what can this card actually do", use `--backend alsa --probe`.

## The desktop app

<p align="center">
  <img src="screenshots/desktop.png" alt="the audiaki desktop app" width="820">
</p>

```sh
audiaki-gui                          # open the window on the default device
audiaki-gui -D plughw:CARD=Box,DEV=0 # ...on a particular interface
audiaki-gui -b alsa                  # ...through a particular backend
audiaki-gui -o session               # name takes session-001.wav and up
audiaki-gui -s waterfall             # start on a particular visualiser
audiaki-gui -s tuner                 # ...come up as a tuner
audiaki-gui -V                       # also render an MP4 of each take
audiaki-gui -V --video-size 1080p    # ...at a particular size
audiaki-gui -V --video-silent        # ...with no audio track in it
audiaki-gui -M                       # come up already monitoring
audiaki-gui --preroll 10             # start each take 10 s before Record
```

The capture stream opens with the window and stays open, so the spectrum moves
and the meter reads before you press anything — setting an input level should
not mean starting a take you are going to throw away. With `--preroll` those
seconds are kept rather than discarded, and Record starts the take that far
back.

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

| Key | Does |
| --- | --- |
| `space` | Record, or pause and resume once a take is running |
| `S` | Stop |
| `M` | Toggle monitoring |
| `V` | Next visualiser style |
| `F` | Fullscreen |

The device dropdown is disabled while a take is open: switching closes the
capture stream, which would truncate the recording. Stop first. If the new
device will not open, audiaki falls back to the previous one rather than
leaving the window with no audio.

The list follows the hardware: plug an interface in and it appears a moment
later, unplug one and it goes. Nothing needs restarting, and a window that came
up with no device at all opens the first one plugged in.

Takes are always numbered from the prefix, so there is no overwrite prompt and
no `--force` to get wrong: pressing record cannot destroy an earlier take.

### Recording video

**Video** off is audio only — `take-003.wav` and nothing else. Video on writes
that same WAV and then `take-003.mp4` alongside it, showing whichever
visualiser was selected. It needs `ffmpeg` on `PATH`; without it the WAV is
still written and the window says why the video was not.

**Audio** decides whether that MP4 gets an audio track. On by default — a take
and its visualiser belong together. Turn it off, or start with
`--video-silent`, for a clip going into an edit that already has the sound. The
WAV is written either way, and is what the picture is drawn from regardless.
Like **Video**, it is only settable between takes: the render is a single pass
over the finished take.

The video is rendered **after** the take stops, not captured live off the
screen. Grabbing the framebuffer sixty times a second on the machine holding a
capture stream open is how takes end up with xruns in them. Rendering
afterwards costs the wait, but it is frame-accurate, independent of the
window's size and refresh rate, and cannot drop a frame. On this machine a
4-second take renders in about 5 seconds at 720p60 — roughly real time.

While it runs, **Stop** becomes **Cancel** and the status line shows progress.
Cancelling removes the partial video and keeps the WAV. The `.mp4` only appears
once finished: an MP4 is not playable until its final index is written, so
frames go to a hidden `.take-003.partial.mp4` and the real name is created by a
rename at the end. A cancelled, failed or killed render leaves you with the
take and no video, rather than a file that looks like a video and will not open.

`--video-size` takes `WxH` or `720p`/`1080p`/`1440p`/`2160p`, and `--video-fps`
the frame rate; the defaults are 1280x720 at 60. The CLI's `--visualize`
renders video too, from a WAV you already have, with its own three styles and
no display needed — that is the one for a headless box or a script.

**Monitoring feeds your input back to your speakers**, which will howl if you
are recording a microphone in the same room. It starts off for that reason.
Headphones, or an instrument rather than a mic, and it is fine.

### The visualisers

<p align="center">
  <img src="screenshots/styles.png" alt="the visualiser styles" width="820">
</p>

Six styles, switchable from the strip on the visualiser or with `V`:

| Style | Shows |
| --- | --- |
| `bars` | A stem per band with a glowing cap, growing from the floor |
| `mirror` | The same bars, opening from the centre line |
| `radial` | The spectrum wrapped into a ring, bass at the top |
| `scope` | An oscilloscope trace of the last few milliseconds |
| `waterfall` | A scrolling spectrogram, newest at the right |
| `tuner` | The note being played, and how far off it is |

The first five read the same analysis the CLI's `--spectrum` and `--visualize`
use: a 2048 point window folded into log-spaced bands, fast attack and slow
decay. `bars` is the default, after
[musializer](https://github.com/tsoding/musializer) — the glow is one radial
gradient texture drawn additively, so overlapping halos sum towards white and
loud clusters bloom. `mirror` and `radial` reuse the same caps.

`scope` draws raw samples rather than the spectrum, and starts each sweep at a
rising zero crossing so a steady note stands still instead of scrolling. It
shows true amplitude, so a quiet input is a quiet trace — that is the meter's
job to explain, not the scope's.

`waterfall` is the only one with a memory: about eight seconds of history as a
ring of texture columns, one written per frame, so a hum or a dropout is still
on screen after it has happened.

`tuner` is not a picture of the sound at all. It shows the note, a needle on a
scale of half a semitone either side, and the frequency against what that note
should be — green within 5 cents, amber outside it. It runs the same detection
as the CLI's `--tune`, and only while it is the visible style: the detection
costs millions of operations a go, and paying for it behind a style nobody is
looking at would slow every video render down. It sits with the visualisers
because tuning up is what you do immediately before pressing record, and it
wants the same place on the screen.

## Reading the meter

```
 00:12 [##################|          ]  -8.4 dBFS  xruns:0
       ^ current level    ^ peak hold              ^ buffer overruns
```

Aim for peaks around −6 dBFS. If `CLIP` appears, the signal hit full scale and
the take is distorted — turn the level down on the device, not in software.
`xruns` counting up means the machine could not keep up; try a larger
`--period` or more `--periods`.

With `--spectrum` the bar becomes one column per frequency band, log-spaced
from 40 Hz on the left to 12 kHz on the right:

```
 00:12 ▁▂▄█▆▃▂▁▁▂▃▅▇▆▄▂▁▁▁▂▁▁  -8.4 dBFS  xruns:0
       ^ 40 Hz                    ^ 12 kHz
```

Block characters are used when the locale is UTF-8, an ASCII ramp otherwise.

## Tuning up

```
$ audiaki --tune
 E2  [.................#.................]  in tune      82.4 Hz  -18.3 dBFS
 A2  [...#.............|.................]  -40 cents   107.5 Hz  -22.0 dBFS
 --  [.................|.................]  listening         --  -71.2 dBFS
```

The scale runs from half a semitone flat to half a semitone sharp. `#` is what
you are playing and `|` is where it should be; past 50 cents the note name
changes instead, so the needle never points at the wrong note. Within 5 cents
it says `in tune`, well inside what a string drifts on its own as it warms up.
`--a4` moves the reference pitch: `audiaki --tune --a4 432`.

The pitch comes from the [YIN][yin] difference function, not from looking for
the loudest frequency. A plucked low E often has more energy at 165 Hz than at
82 Hz, and a tuner following the loudest frequency would say the string is an
octave up — worse than no tuner. The tradeoff is one pitch at a time: it tunes
strings, it does not name chords. Anything quieter than about −52 dBFS is not
treated as a note, so the room does not read as a pitch, and a reading is held
briefly after the note decays so the display does not blink between strums.

[yin]: https://audition.ens.fr/adc/pdf/2002_JASA_YIN.pdf

Nothing is written and no file is named — `--tune` is a display, not a take. If
stderr is not a terminal there is no line to redraw in place, so it reports
each note once as it settles, which makes it something you can log:

```sh
$ audiaki --tune 2> notes.log
audiaki: E2  -3 cents  82.2 Hz
audiaki: A2  +1 cents  110.1 Hz
```

## Numbering takes

Tracking a part means playing it several times. `--take` names each attempt
instead of making you invent one:

```sh
audiaki --take session      # session-001.wav
audiaki --take session      # session-002.wav, because 001 is there
```

It picks the first free number, so it never overwrites anything and `--force`
never comes into it. A prefix that already carries an extension keeps it —
`--take session.wav` also writes `session-001.wav` — and a path works as well
as a bare name: `--take takes/riff`.

## Keeping what you played before you pressed record

The take you lose is the one you played to check the sound. `--preroll` opens
the device and waits, holding the last few seconds, and starts the file that
far back when you press Enter:

```sh
$ audiaki --preroll 10 --take riff
audiaki: recording riff-001.wav
audiaki: armed: holding the last 10.0 s - press Enter to record, Ctrl+C to quit
 ARM 00:10 [##################|          ]  -8.4 dBFS  press Enter
```

The clock counts what is being held, not how long you have been waiting: it
rises to the pre-roll size and stops there, at the point a take started now
would begin. Press Enter and those seconds are written to the front of the
file, followed by everything after it.

```
audiaki: prepended 10.0 s of pre-roll
 00:14 [##################|          ]  -8.4 dBFS  xruns:0
```

Ctrl+C while it is armed writes nothing at all — no empty file, and with
`--take` the number is not used up. `--duration` is measured from the keypress,
so `--preroll 10 -t 30` is a 40 second file: ten seconds of lead and the thirty
you asked to record.

The audio is held exactly as the device delivered it, so the pre-roll seconds
are the same samples the rest of the take is made of, not a converted copy. The
cost is memory: ten seconds of 24-bit stereo at 48 kHz is about 3 MiB, a minute
about 17 MiB, and a wider or faster stream proportionally more. The ceiling is
300 seconds.

In the desktop app there is nothing to arm — the capture stream is already open
before you press anything, which is what makes the meters live — so `--preroll`
there just means every take begins that far back:

```sh
audiaki-gui --preroll 10
```

The status line shows how much is held while the window is idle, and Record
writes it to the front of the take. Nothing is kept while a take is recording
or paused: audio already in the file is not held to be written twice, and audio
you paused out is not smuggled back in by resuming.

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
sat at full scale and the take is distorted. Each channel is measured on its
own, so a clipped left channel cannot hide behind a quiet right one.

The noise floor is the tenth percentile of the level of 50 ms windows — in
practice, the room and the cable with nothing being played. Compare it with the
peak to see how much of the range the instrument is using. A DC offset much
past ±0.001 usually means the interface has a bias worth fixing.

`--info` reads whatever the reader accepts, including files other tools wrote:
8/16/24/32-bit PCM and 32/64-bit float. A take interrupted before its header
was patched is reported as truncated rather than refused.

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
`ffmpeg`, which encodes them and muxes in the original audio untouched. No
graphics library is involved: everything is rasterised into a plain pixel
buffer. Ctrl+C during a render stops ffmpeg and removes the partial video.

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
column, then sweeps a playhead across. That pass costs by the width of the
video, not the length of the take, so an hour is no more expensive than a
minute. `--bars` applies only to `bars`.

## Troubleshooting

**`cannot open capture device 'default': Device or resource busy`**
Something else holds the card exclusively. This is what the PipeWire backend is
for — `--backend pipewire` shares the interface instead of competing for it. If
you need ALSA specifically, record through the plug layer instead:
`-D plughw:CARD=Box,DEV=0`.

**`the pipewire backend is not answering; is the daemon running?`**
`--backend pipewire` was asked for on a machine where no daemon replied. Either
start one or use `--backend alsa`. `auto` never produces this: it falls back on
its own.

**`this build has no pipewire backend`**
It was compiled without `libpipewire-0.3-dev`. Install it and rebuild;
`make help` reports which backends are in.

**`cannot set 2 channel(s)`**
The device is mono-only, or wants a different count. Run `--probe` to see the
supported range, then pass `-c`.

**Rate warning: `requested 44100 Hz, device negotiated 48000 Hz`**
The hardware does not support the rate you asked for and ALSA picked the
nearest one. The file is written at the rate actually used.

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
  backend.c/.h  which audio system to talk to, and the tables it is reached by
  device.c/.h   the capture interface, dispatching to the chosen backend
  device_alsa.c   ...over libasound
  device_pipewire.c ...over libpipewire, when it was compiled in
  recorder.c/.h capture loop: device -> repack -> WAV
  wav.c/.h      streaming WAV writer, and a tolerant WAV reader
  info.c/.h     measure a finished take: levels, clipping, noise floor
  preroll.c/.h  the seconds held before a take starts, kept bit for bit
  take.c/.h     numbered take filenames for --take
  jsonout.c/.h  the little JSON --json needs
  format.c/.h   sample formats, peak detection, repacking
  meter.c/.h    the terminal peak and spectrum displays
  fft.c/.h      radix-2 FFT and the Hann window
  spectrum.c/.h streaming analyser: samples in, bar heights out
  tuner.c/.h    pitch detection: samples in, a note and its offset out
  tune.c/.h     the --tune loop: device -> tuner -> terminal
  canvas.c/.h   RGBA framebuffer and the shapes the visualiser draws
  visualize.c/.h render a WAV to a video
  ffmpeg.h      pipe frames to an ffmpeg child
  ffmpeg_posix.c  its fork/exec/pipe implementation
  signals.c/.h  the shared Ctrl+C flag
  parse.c/.h    strict CLI value parsing
  monitor.c/.h  playback, for hearing the input while it records
  monitor_alsa.c     ...over libasound
  monitor_pipewire.c ...over libpipewire
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

No audio system appears above `backend.h`: `device.c` and `monitor.c` are
dispatchers, and only the four `*_alsa.c` / `*_pipewire.c` files include
`<alsa/asoundlib.h>` or `<pipewire/pipewire.h>`. So `format`, `wav`, `parse`,
`fft`, `spectrum`, `tuner`, `canvas`, `info`, `take`, `ringbuf`, `preroll` and
`jsonout` are plain C that builds and tests anywhere — which is what CI does.
The analysis is shared three ways: the terminal display, the video renderer and
the desktop app all run the same `spectrum` module, the terminal and desktop
tuners run the same `tuner`, and the CLI's armed wait and the window's idle
capture fill the same `preroll`. `src/gui/` is the only code that knows raylib
exists, and nothing in `src/` depends on it, which keeps the CLI buildable with
the submodule absent.

## Limitations

- Little-endian hosts only; WAV is little-endian and no byte swapping is done.
- Plain 44-byte PCM WAV, so recordings stop at the 4 GB RIFF limit
  (about 3.5 hours of 24-bit stereo at 48 kHz).
- Linux only, through ALSA or PipeWire. No JACK or CoreAudio backend.
- Rendering shells out to `ffmpeg`, so the codecs and their licensing are its
  business, not audiaki's.
- The desktop app records to numbered takes in the working directory. There is
  no file dialog, and no playback of a finished take.
- Video is rendered after the take, so a long take with video on means waiting
  roughly its own length again. Cancelling is always available, and the audio
  is safe on disk regardless.
- Monitoring through ALSA needs an output that accepts the capture rate
  directly: audiaki does not resample, so it declines to monitor rather than
  play back at the wrong pitch. The PipeWire backend has no such limit, because
  the server resamples.
- The tuner is monophonic: one pitch at a time, no chords, and it looks between
  40 Hz and 2 kHz — a bass low B and a guitar's top fret are inside that, a
  piccolo is not.
- Rate and channels are fixed for the session in the desktop app; only the
  device can be changed from the window. A device that disappears mid-take ends
  that take where it stopped — what was written stays on disk, but the app does
  not resume when the hardware returns.

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
