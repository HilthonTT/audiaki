# Using audiaki

The command line recorder, option by option. The [README](../README.md) is the
short version, [DESKTOP.md](DESKTOP.md) covers the window, and
[DESIGN.md](../DESIGN.md) covers why any of it is built the way it is. Installed
systems also have `man audiaki`.

- [Options](#options)
- [Backends](#backends)
- [Reading the meter](#reading-the-meter)
- [Tuning up](#tuning-up)
- [Numbering takes](#numbering-takes)
- [Pre-roll](#pre-roll)
- [Checking a take](#checking-a-take)
- [Scripting](#scripting)
- [Rendering a video](#rendering-a-video)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)

## Options

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

Set `AUDIAKI_DEVICE=hw:CARD=Box,DEV=0` to stop typing `-D` every time.

## Backends

audiaki talks to one of two audio systems, and picks without being asked: if a
PipeWire daemon answers, it uses PipeWire; otherwise ALSA. `--backend` overrides
that, and `$AUDIAKI_BACKEND` sets a default. Asking for a backend that is not
there is an error rather than a quiet downgrade.

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

A PipeWire sink appears in `--list` alongside the inputs, described as a
monitor. Recording one captures what is being *played* to it — a browser, a
synth, a video call — which is not something opening the card can do.

```sh
$ audiaki --list
DEVICE                                     DESCRIPTION
alsa_output.pci-0000_00_1f.3.analog-stereo Built-in Audio Analog Stereo: monitor of this output
alsa_input.pci-0000_00_1f.3.analog-stereo  Built-in Audio Analog Stereo: Audio/Source

$ audiaki -D alsa_output.pci-0000_00_1f.3.analog-stereo -t 30 desktop.wav
```

`--probe` means different things on each backend; see
[DESIGN.md](../DESIGN.md#what---probe-means-on-each).

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
you are playing and `|` is where it should be; within 5 cents it says `in tune`.
`--a4` moves the reference pitch: `audiaki --tune --a4 432`.

Nothing is written and no file is named — `--tune` is a display, not a take. If
stderr is not a terminal there is no line to redraw in place, so it reports
each note once as it settles, which makes it something you can log:

```sh
$ audiaki --tune 2> notes.log
audiaki: E2  -3 cents  82.2 Hz
audiaki: A2  +1 cents  110.1 Hz
```

The detection is monophonic and uses YIN rather than the loudest frequency;
[DESIGN.md](../DESIGN.md#pitch-detection) says why.

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

## Pre-roll

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
file. Ctrl+C while it is armed writes nothing at all — no empty file, and with
`--take` the number is not used up.

In the desktop app there is nothing to arm; `--preroll` there just means every
take begins that far back, and the status line shows how much is held while the
window is idle. What it costs and how the samples are kept:
[DESIGN.md](../DESIGN.md#pre-roll).

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
sat at full scale and the take is distorted. `--info` reads whatever the reader
accepts, including files other tools wrote. What the noise floor and DC figures
mean: [DESIGN.md](../DESIGN.md#measuring-a-take).

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

| Style | Shows |
| --- | --- |
| `bars` (default) | The spectrum at this instant, as log-spaced bars |
| `scope` | An oscilloscope trace of the last 40 ms |
| `waveform` | The whole take's envelope, with a playhead crossing it |

audiaki does the analysis and the drawing itself, then pipes raw RGBA frames to
`ffmpeg`, which encodes them and muxes in the original audio untouched. No
graphics library is involved, so this works on a headless box. Ctrl+C during a
render stops ffmpeg and removes the partial video. `--bars` applies only to
`bars`.

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
  play back at the wrong pitch. The PipeWire backend has no such limit.
- The tuner is monophonic: one pitch at a time, no chords, and it looks between
  40 Hz and 2 kHz — a bass low B and a guitar's top fret are inside that, a
  piccolo is not.
- Rate and channels are fixed for the session in the desktop app; only the
  device can be changed from the window. A device that disappears mid-take ends
  that take where it stopped — what was written stays on disk, but the app does
  not resume when the hardware returns.
