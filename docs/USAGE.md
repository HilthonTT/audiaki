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
- [Hearing yourself](#hearing-yourself)
- [What a take says about itself](#what-a-take-says-about-itself)
- [Checking a take](#checking-a-take)
- [Playing one back](#playing-one-back)
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
audiaki --play take01.wav            # ...and what does it sound like?
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
| `--note TEXT` | Stamp the take with a note, up to 200 characters |
| `--no-metadata` | Write a plain 44-byte header with nothing about the take |
| `--preroll SECS` | Hold SECS and wait for Enter; the take starts that far back |
| `--spectrum` | Live spectrum bars instead of the peak bar |
| `--no-meter` | Do not draw anything while recording |
| `-M, --monitor` | Hear the input while it is recorded (use headphones) |
| `--monitor-device NAME` | Output to monitor through (default `default`) |
| `--monitor-gain X` | Scale what is monitored, 0.0 to 2.0 (default 1.0) |
| `--visualize FILE` | Render a WAV to a visualiser video and exit |
| `-o, --output FILE` | Output file (video default: input with `.mp4`) |
| `--style NAME` | `bars`, `scope` or `waveform` (default `bars`) |
| `--size SPEC` | `WxH`, or `480p`/`720p`/`1080p`/`1440p`/`2160p` (default `1280x720`) |
| `--fps N` | Video frame rate (default 60) |
| `--bars N` | Spectrum bar count (default 64) |
| `--tune` | Show the pitch of what is being played, until Ctrl+C |
| `--a4 HZ` | Tuner reference pitch (default 440) |
| `--info FILE` | Report levels and clipping for a WAV and exit; more files may follow |
| `--play FILE` | Play a WAV through the output and exit |
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

## Hearing yourself

`-M` plays the input back through an output while it is being captured, so you
can hear the take as you make it:

```sh
audiaki -M take01.wav                              # through the default output
audiaki -M --monitor-device plughw:CARD=Box,DEV=0 take01.wav
audiaki -M --monitor-gain 0.4 take01.wav           # quieter in the headphones
audiaki -M --preroll 10 --take riff                # audible while armed, too
```

**Use headphones.** Monitoring an open microphone through speakers is a
feedback loop, and the recorder is not going to be the thing that breaks it:
a laptop's built-in mic played out of the speaker next to it reaches full
scale in a fraction of a second and screeches. An instrument plugged into an
interface is safe; a room mic is not.

The other loop is a digital one, and headphones do not save you from it:
capturing an output's *monitor* source — the `monitor of this output` entries
in `--list` — and monitoring it back through that same output feeds the stream
directly into itself. Check what `-D` is actually pointing at before adding
`-M`.

`--monitor-gain` scales what you hear and nothing else — the file is always
written from the samples the device delivered, so turning the monitor down
does not record a quieter take, and the meter does not move. It goes from
`0.0` (silent) to `2.0` (+6 dB), with `1.0` unchanged. Naming a device or a
gain switches monitoring on by itself, so `-M` is only needed on its own.

Monitoring is a convenience, and it is never allowed to cost you a take. If
the output will not open, or fails part way through, audiaki says so and keeps
recording:

```
audiaki: warning: monitor: cannot connect the playback stream to 'no-such-output'
audiaki: warning: recording without monitoring
```

Under ALSA the output has to accept the capture rate directly — audiaki does
not resample — so monitoring a 44.1 kHz capture on a 48 kHz output declines
rather than playing back at the wrong pitch. Use `--backend pipewire`, or the
`plug` layer: `--monitor-device plughw:CARD=Box,DEV=0`.

You are hearing the input late, through a second buffer of its own — tens of
milliseconds under ALSA, around a tenth of a second under PipeWire. That is
fine for checking a level, a tone or a room; it is not latency to play in time
against. Frames the output cannot keep up with are
dropped rather than queued, so the monitor may skip on a busy machine without
that reaching the file — `-v` reports the count at the end.

## What a take says about itself

Every take is stamped with what made it, when, and from what:

```sh
audiaki --note "second chorus, clean tone" take01.wav
```

```
$ audiaki --info take01.wav
...
recorded:    2026-08-08 12:13:23
device:      hw:CARD=Box,DEV=0
software:    audiaki 1.0.0
note:        second chorus, clean tone
```

The stamp is two standard chunks written ahead of the audio: a `LIST`/`INFO`
block, which is what taggers and players read, and a `bext` block — the
Broadcast Wave extension every field recorder writes. Other tools see them
without being told to:

```sh
$ ffprobe -show_format take01.wav
TAG:comment=second chorus, clean tone
TAG:date=2026-08-08
TAG:coding_history=A=PCM,F=48000,W=24,M=stereo,T=audiaki 1.0.0; hw:CARD=Box,DEV=0
```

`bext` also carries a **time reference**: how many samples separate local
midnight from the first frame. Two takes from one session line up on a timeline
from that alone, with no timecode involved.

The time is when the take started. With `--preroll` that is when you pressed
Enter, so the pre-roll seconds are older than the timestamp by exactly the
pre-roll — the only reading of it that does not need the pre-roll length to
make sense of.

None of it touches a sample of audio: the payload is the same PCM it always
was, and the whole stamp costs under a kilobyte. `--no-metadata` writes the
plain 44-byte header instead, for a tool that wants nothing between the `fmt`
and `data` chunks. The desktop app stamps its takes the same way, minus the
note, since there is nowhere in the window to type one.

Metadata is read back from chunks that sit **before** the audio, which is where
audiaki writes them and where the BWF specification requires `bext`. A file
whose editor appended tags after the payload will read as having none.

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

Name more than one file and each gets a line instead, which is the shape the
question takes at the end of a session — which of these do I keep?

```
$ audiaki --info session-*.wav
FILE                DURATION     PEAK      RMS   CLIPPED
session-001.wav        41.20     -4.4    -10.7         0
session-002.wav      1:12.34     -0.0     -6.1      1820  CLIP
session-003.wav        38.05    -14.2    -22.8         0
```

A file that cannot be read is reported on stderr and stepped over, so one bad
take does not hide the state of the rest; the exit status is still non-zero.
With `--json` the same run writes an array of the single-file objects rather
than a table.

## Playing one back

```sh
audiaki --play take01.wav                  # through the default output
audiaki --play take01.wav --spectrum       # ...watching the spectrum
audiaki --play take01.wav -t 30            # ...the first 30 seconds only
audiaki --play take01.wav -D plughw:CARD=Box,DEV=0   # ...through that output
```

The meter is the recording meter with the clock showing a position rather than
a length, and no xrun counter — nothing is being written, so there are no
frames to lose:

```
 00:12 / 03:45 [##################|          ]  -8.4 dBFS
```

Ctrl+C stops immediately. `--play` reads whatever the reader accepts, not only
audiaki's own takes: 8/16/24/32-bit PCM and 32/64-bit float, at any rate the
output will take.

Under PipeWire that is any rate at all, because the server resamples. Under
ALSA the output has to accept the file's rate directly — audiaki does not
resample — so a 44.1 kHz take on a device running at 48 kHz reports
`output wants 48000 Hz but the audio is 44100 Hz` and plays nothing. Use
`--backend pipewire`, or the `plug` layer: `-D plughw:CARD=Box,DEV=0`.

`-D` names an **output** here rather than a capture device. `$AUDIAKI_DEVICE`
is deliberately ignored by `--play`, because what it names is an input.

## Scripting

`--list`, `--probe` and `--info` take `--json` and write a single object or
array to stdout, with diagnostics staying on stderr:

```sh
audiaki --list --json | jq -r '.[].device'
audiaki --info take01.wav --json | jq '.peak_dbfs, .clipped_samples'
audiaki --info take01.wav --json | jq -r '.metadata.note'
audiaki --info session-*.wav --json | jq -r '.[] | select(.clipped_samples > 0).file'
```

`--info` over several files writes an array; over one it writes the single
object it always did. Every report carries a `metadata` object, whose fields
are `null` when the file said nothing about itself.

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
- PCM WAV with a `LIST`/`INFO` and `bext` stamp ahead of the audio, so
  recordings stop at the 4 GB RIFF limit (about 3.5 hours of 24-bit stereo at
  48 kHz). `--no-metadata` gets the plain 44-byte header back.
- Metadata is read only from chunks before the audio. A file whose tags were
  appended after the payload reads as having none.
- Linux only, through ALSA or PipeWire. No JACK or CoreAudio backend.
- Rendering shells out to `ffmpeg`, so the codecs and their licensing are its
  business, not audiaki's.
- The desktop app records to numbered takes in the working directory. There is
  no file dialog, and no playback of a finished take.
- Video is rendered after the take, so a long take with video on means waiting
  roughly its own length again. Cancelling is always available, and the audio
  is safe on disk regardless.
- Monitoring and `--play` through ALSA need an output that accepts the stream's
  rate directly: audiaki does not resample, so it declines rather than play
  back at the wrong pitch. The PipeWire backend has no such limit.
- `-M` monitors through a buffer of its own, so what you hear is tens of
  milliseconds behind what you played. It is for checking a sound, not for
  playing along with one.
- `--play` runs start to finish. There is no seeking, no pausing and no
  playlist; `-t` is the only way to hear less than all of it.
- The tuner is monophonic: one pitch at a time, no chords, and it looks between
  40 Hz and 2 kHz — a bass low B and a guitar's top fret are inside that, a
  piccolo is not.
- Rate and channels are fixed for the session in the desktop app; only the
  device can be changed from the window. A device that disappears mid-take ends
  that take where it stopped — what was written stays on disk, but the app does
  not resume when the hardware returns.
