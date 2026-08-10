# Using audiaki

The command line recorder, option by option. The [README](../README.md) is the
short version, [DESKTOP.md](DESKTOP.md) covers the window, and
[DESIGN.md](../DESIGN.md) covers why any of it is built the way it is. Installed
systems also have `man audiaki`.

- [Options](#options)
- [Backends](#backends)
- [Reading the meter](#reading-the-meter)
- [Turning a quiet input up](#turning-a-quiet-input-up)
- [Tuning up](#tuning-up)
- [Numbering takes](#numbering-takes)
- [Where takes are kept](#where-takes-are-kept)
- [Recording one channel](#recording-one-channel)
- [Pre-roll](#pre-roll)
- [Hearing yourself](#hearing-yourself)
- [Playing to a click](#playing-to-a-click)
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
audiaki --dir ~/Takes --take session # ...in that folder
audiaki --preroll 10 take04.wav      # keep the 10 seconds before Enter
audiaki -M --click 120 take05.wav    # play to a metronome (headphones!)
audiaki --info take01.wav            # how did that take come out?
audiaki --play take01.wav            # ...and what does it sound like?
audiaki --play session-*.wav         # ...all of them, space to pause
audiaki -D plughw:CARD=Box,DEV=0 -r 48000 -c 2 take03.wav
audiaki --visualize take01.wav       # render take01.mp4
```

| Option | Description |
| --- | --- |
| `-D, --device NAME` | Capture device (default `default`, or `$AUDIAKI_DEVICE`) |
| `--backend NAME` | `auto`, `pipewire` or `alsa` (default `auto`, or `$AUDIAKI_BACKEND`) |
| `-r, --rate HZ` | Sample rate (default 44100) |
| `-c, --channels N` | How many channels to capture (default 2) |
| `--channel N\|mix` | Write only capture channel `N`, counting from 1, as a mono take — or `mix` for every channel averaged into one |
| `-f, --format NAME` | Force `s16_le`, `s24_3le`, `s24_le` or `s32_le` |
| `-t, --duration SPEC` | Stop after `SS`, `MM:SS` or `HH:MM:SS` |
| `-p, --period FRAMES` | Period size (default 1024) |
| `-n, --periods N` | Periods per buffer (default 4) |
| `-y, --force` | Overwrite an existing output file |
| `--take PREFIX` | Write the next free `PREFIX-001.wav` |
| `--dir FOLDER` | Keep takes in `FOLDER`, so a bare filename is written there |
| `--prompt` | After the take, ask which folder to keep it in and what to call it |
| `--no-prompt` | Never ask, whatever the config file says |
| `--note TEXT` | Stamp the take with a note, up to 200 characters |
| `--no-metadata` | Write a plain 44-byte header with nothing about the take |
| `--preroll SECS` | Hold SECS and wait for Enter; the take starts that far back |
| `--spectrum` | Live spectrum bars instead of the peak bar |
| `--no-meter` | Do not draw anything while recording |
| `-M, --monitor` | Hear the input while it is recorded (use headphones) |
| `--monitor-device NAME` | Output to monitor through (default `default`) |
| `--monitor-gain X` | Scale what is monitored, 0.0 to 2.0 (default 1.0) |
| `--gain X` | Scale the capture on the way in, 0.0 to 16.0; this one reaches the file (default 1.0) |
| `--click BPM` | Play a metronome at BPM (20 to 300) while recording |
| `--click-beats N` | Beats to a bar, accenting the first (default 4; 0 or 1 for a bare pulse) |
| `--click-subdiv N` | Ticks to a beat, struck softer than the beat: 2 for eighths, 3 for triplets, 4 for sixteenths (1 to 8, default 1) |
| `--click-gain X` | How loud the click is, 0.0 to 2.0 (default 0.5) |
| `--visualize FILE` | Render a WAV to a visualiser video and exit |
| `-o, --output FILE` | Output file (video default: input with `.mp4`) |
| `--style NAME` | `bars`, `scope` or `waveform` (default `bars`) |
| `--size SPEC` | `WxH`, or `480p`/`720p`/`1080p`/`1440p`/`2160p` (default `1280x720`) |
| `--fps N` | Video frame rate (default 60) |
| `--bars N` | Spectrum bar count (default 64) |
| `--tune` | Show the pitch of what is being played, until Ctrl+C |
| `--a4 HZ` | Tuner reference pitch (default 440) |
| `--tune-min HZ` | Lowest pitch the tuner looks for (default 30, a five-string bass's low B) |
| `--tune-max HZ` | Highest pitch the tuner looks for (default 4500, past a piccolo's top C) |
| `--info FILE` | Report levels and clipping for a WAV and exit; more files may follow |
| `--play FILE` | Play a WAV through the output and exit; more files may follow, and a terminal gets transport keys |
| `--shuffle` | Play the files in a random order, picked afresh each time through a repeating list |
| `--repeat` | Start the playlist again when it ends |
| `--repeat-one` | Start the current file again when it ends; `n` still moves on |
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
the take is distorted — turn the level down on the device if it has a knob.
`xruns` counting up means the machine could not keep up; try a larger
`--period` or more `--periods`.

With `--spectrum` the bar becomes one column per frequency band, log-spaced
from 40 Hz on the left to 12 kHz on the right:

```
 00:12 ▁▂▄█▆▃▂▁▁▂▃▅▇▆▄▂▁▁▁▂▁▁  -8.4 dBFS  xruns:0
       ^ 40 Hz                    ^ 12 kHz
```

Block characters are used when the locale is UTF-8, an ASCII ramp otherwise.

## Turning a quiet input up

The level to fix first is the one on the hardware. A gain knob on an interface
sits ahead of its converter, so turning it up captures more of the converter's
range; turning it up afterwards in software only scales the range you already
captured, noise and all. If the interface has a knob, or ALSA exposes a capture
volume for it (`alsamixer`, F4), that is the one to reach for. Check that a bass
is going into an instrument or Hi-Z input rather than a line input while you are
there — that alone is often the whole difference.

Some inputs have no knob to turn. A line input, a cheap USB box, a card whose
capture volume ALSA does not expose: for those, `--gain` scales the capture on
the way in.

```sh
audiaki --gain 2.0 riff.wav       # +6 dB
audiaki --gain 4.0 riff.wav       # +12 dB
audiaki --gain 8.0 riff.wav       # +24 dB, about as far as this is worth taking
```

A multiplier from `0.0` to `16.0`, where `1.0` is unchanged. Unlike
`--monitor-gain`, **this one reaches the file.** Set it once in the config file
if it is a property of your interface rather than of today:

```ini
gain = 2.0
```

The gain goes on each period as it arrives, before anything measures it — so the
meter, the `--spectrum` bars, the pre-roll and what you hear through `-M` are all
describing the recording rather than the device. Watch the meter and aim for the
same −6 dBFS peaks as ever. If you overshoot, the take says so and names the
cause:

```
audiaki: warning: --gain 4.00 clipped 40538 sample(s) - lower it and record again
```

Clipping is why this is the last resort rather than the first. Samples held at
full scale cannot be un-held, so a gain set too high damages the take in a way
no amount of editing afterwards will undo — where a take that came in quiet is
merely quiet, and audiaki records 24-bit by default, so there is a great deal of
room to bring one up later with the track gain in the window or in any editor.
Software gain buys nothing over doing it afterwards except the convenience of
not having to; what it costs is a take you can ruin. Turn the hardware up first.

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

By default it searches from 30 Hz to 4500 Hz, which runs from a five-string
bass's low B up past a piccolo's top C. `--tune-min` and `--tune-max` move
either end:

```sh
audiaki --tune --tune-min 27.5      # down to a piano's bottom A
audiaki --tune --tune-min 70        # only a guitar, and cheaper for it
```

The two ends do not cost the same. The ceiling sets the shortest wave the
detector looks for, so raising it is nearly free. The floor sets the longest,
and the work is quadratic in it: at 48 kHz a reading takes about 3.5 ms with the
floor at 40 Hz and about 7.6 ms at 27.5. That is still cheap twenty times a
second, but it is why the default stops at a bass's low B rather than going down
to a piano's for the sake of it — and why narrowing the range is worth doing if
you are running the tuner beside something else.

Accuracy is not flat across the range either. Down low it lands within a
hundredth of a cent; at the very top a wave is only about ten samples long at
44.1 kHz, so the detector is interpolating between whole samples and comes
within a few cents. Inaudible, but not the same number.

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

## Where takes are kept

By default a take is written where you are standing, which is fine until you
notice a year of recordings scattered across every folder you have ever run a
shell in. `--dir` says where they go instead:

```sh
audiaki --dir ~/Takes --take session   # ~/Takes/session-001.wav
```

The folder is created if it is not there. It only applies to a name that is
only a name: `--dir ~/Takes -o riff.wav` writes `~/Takes/riff.wav`, but
`-o sub/riff.wav` already says where it goes and is left alone, as is any
absolute path.

Typing `--dir` every time would be no better than typing the folder, so it has
a config file — see [below](#the-config-file).

### Being asked afterwards

Once the take is over, audiaki offers it a folder and a name:

```
audiaki: wrote ~/Takes/session-001.wav: 42.10 s, 14.2 MiB, 0 xrun(s)
folder [~/Takes]:
name [session-001.wav]: riff-clean-take.wav
audiaki: stored /home/you/Takes/riff-clean-take.wav
```

Enter accepts what is offered, so keeping it where it is costs two keystrokes.
Anything else moves it there, creating the folder if it has to and never
landing on a file that is already there — a name that is taken is asked again
rather than overwritten. Ctrl+C or Ctrl+D at either question keeps the take
where it is.

This only happens when there is a terminal to ask at. A run whose input is a
pipe, or a file, or `/dev/null` is never asked, and neither is `--quiet` — a
script that suddenly waited for a folder name would look like a script that had
hung. `--prompt` asks anyway, and `--no-prompt` never does.

Nothing here is asked before the recording. Where a take is going is a question
for afterwards; where it is being written is settled before the first frame
arrives, so there is no moment when audio is playing and nothing is catching it.

### The config file

`~/.config/audiaki/config`, or `$XDG_CONFIG_HOME/audiaki/config`, or whatever
`$AUDIAKI_CONFIG` names. It holds the answers that are the same every session:

```ini
# where a take named without a folder is written
take_dir = ~/Takes

# ask where to keep it afterwards: auto, yes or no
prompt = auto

# round-trip latency the window places an overdub by, in milliseconds;
# left out, it is estimated from the buffers. See DESKTOP.md.
latency_ms = 14

# gain added to the recording itself, as a multiplier. For an interface
# with no usable knob on it - see "Turning a quiet input up" above.
gain = 2.0
```

`auto` is the default described above. Everything in the file is a default that
the command line still overrides, `#` and `;` start a comment, and a line that
makes no sense is reported and skipped rather than stopping the recording.

The window reads the same file: `take_dir` is where its numbered takes are
written, and `prompt = no` is the same as starting it with `--no-dialog`. See
[DESKTOP.md](DESKTOP.md).

## Recording one channel

Plenty of interfaces only offer stereo. Plug one instrument into the first input
and the take is half silence at twice the size, and every tool downstream has to
be told which side the music is on. `--channel` writes one capture channel and
nothing else:

```sh
audiaki --channel 1 riff.wav      # the left input, as a mono file
audiaki --channel 2 riff.wav      # the right one
audiaki --channel mix riff.wav    # both of them averaged into one
```

`mix` is for the other shape of the same problem: two inputs that both have
something on them — a pair of room mics, or an instrument and a vocal going to
one file — where dropping either one loses half the take. It averages rather
than sums, so a mixdown can never clip; the cost is up to 3 dB against a summed
mix when the channels carry the same signal, which is the right way round for a
capture path where a clipped take cannot be undone.

It does not change what the device is asked for. `-c` still decides how many
channels are captured — the interface is opened as stereo either way, because
that is all it does — and `--channel` decides which one reaches the file. The
result is a genuine mono WAV: one channel in the header, half the bytes, and no
silent track for an editor to strip out later.

Not sure which input you are in? Record a few seconds of both and ask:

```sh
$ audiaki -t 5 -y check.wav && audiaki --info check.wav
channels:    ch 1: peak   -8.4 dBFS  rms  -18.1 dBFS  dc +0.00000
             ch 2: peak  -68.2 dBFS  rms  -74.9 dBFS  dc +0.00000
```

Channel 2 is an unplugged input, so `--channel 1` is the one to keep.

The meter follows the channel you picked, which is the point of picking it
before you play rather than after: the level you are setting is the level that
lands in the file, and a channel you are not recording can clip without it
meaning anything. Monitoring is the exception — `-M` plays back what the device
delivered, both channels, because the output was opened to match the device and
what you hear is a convenience rather than the product.

Numbering is from 1, matching what `--info` prints. Asking for a channel the
device did not give is an error rather than a silent fallback, and it is checked
against what the device actually negotiated: if a card was asked for four
channels and settled on two, `--channel 3` stops rather than recording something
you did not ask for. `--channel mix` never has that problem — it takes whatever
the device gave, however many that turned out to be.

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

`--monitor-gain` scales what you hear and nothing else — turning the monitor
down does not record a quieter take, and the meter does not move. `--gain` is
the one that does reach the file; see [Turning a quiet input
up](#turning-a-quiet-input-up). The two multiply, so an input brought up by
`--gain 4.0` is already four times louder in the headphones before
`--monitor-gain` is touched. It goes from
`0.0` (silent) to `2.0` (+6 dB), with `1.0` unchanged. Naming a gain switches
monitoring on by itself, so `-M` is only needed on its own. So does naming a
device — unless `--click` is also given, since then the output has something to
play without the input going anywhere near it, and `--monitor-device` is how
you say which output the click comes out of.

Monitoring is a convenience, and it is never allowed to cost you a take. If
the output will not open, or fails part way through, audiaki says so and keeps
recording:

```
audiaki: warning: monitor: cannot connect the playback stream to 'no-such-output'
audiaki: warning: recording without monitoring
```

An output that will not run at the capture rate is converted to on the way out,
so monitoring a 44.1 kHz capture through a 48 kHz output works rather than
declining:

```
audiaki: monitor: output wants 48000 Hz and the audio is 44100 Hz; converting
         on the way out
```

The conversion is a windowed sinc, and it is on the playback path only — the
file is always the samples the device delivered, at the rate it delivered them.
Going down as well as up matters here: dropping 96 kHz to 48 without filtering
first would fold everything above 24 kHz back into the audible band as tones
nobody played, so the cutoff follows the lower of the two rates. It costs a
couple of million multiply-adds a second, which is nothing next to the drawing.

You are still hearing the input late, through a second buffer of its own — tens
of milliseconds under ALSA, around a tenth of a second under PipeWire. That is
the output queue, not something a correction can remove. It is fine for checking
a level, a tone or a room; it is not latency to play in time against.

Frames the output cannot keep up with are dropped rather than queued, so the
monitor may skip on a busy machine without that reaching the file — `-v` reports
the count at the end.

What you hear follows `--channel`: a take being written as one channel is
monitored as one channel, so the headphones carry the take being made rather
than the pair it was taken out of.

## Playing to a click

`--click` runs a metronome at the tempo you give it, from 20 to 300 BPM,
through the same output monitoring uses:

```sh
audiaki -M --click 120 take01.wav              # play along, hearing yourself
audiaki --click 96 take02.wav                  # just the click, not the input
audiaki --click 140 --click-beats 3 waltz.wav  # three to the bar
audiaki --click 120 --click-beats 0 take03.wav # a bare pulse, no accent
audiaki --click 90 --click-subdiv 2 slow.wav   # eighths under a slow tempo
audiaki --click 100 --click-subdiv 3 shuffle.wav     # triplets
audiaki -M --click 120 --click-gain 0.8 take04.wav   # louder in the headphones
audiaki --preroll 8 --click 120 --take riff    # count yourself in, then record
```

The first beat of each bar is a tone an octave above the others, so you can
hear where the bar starts. `--click-beats` sets how many beats that is — 4 by
default, `3` for a waltz, `0` or `1` for a bare pulse with no accent at all.
`--click-gain` sets how loud it is against the instrument, on the same 0.0 to
2.0 scale as `--monitor-gain`, and the two are independent: turning the monitor
down does not take the click with it.

`--click-subdiv` divides the beat: `2` puts an eighth between each pair of
beats, `3` gives triplets, `4` sixteenths, up to 8. The ticks are the beat tone
struck softer, so the pulse still reads as the pulse and the subdivisions sit
under it. This is what a slow tempo usually wants — at 60 BPM there is a second
of nothing between beats to drift about in, and eighths give you something to
hold onto.

**The click is never written to the take.** Like `--monitor-gain`, it changes
what the person recording hears and nothing about the file — which is also why
you want headphones. Played out of a speaker, the click is in the room, and the
microphone will put it in the take after all.

What *is* written is what the click was set to. A take recorded to one carries
the tempo in its metadata, so `--info` can tell you a month later what a file
was played against — which matters exactly because the click itself is not in
the audio to be worked out from:

```sh
$ audiaki --click 96 --click-beats 3 --click-subdiv 2 -t 30 waltz.wav
$ audiaki --info waltz.wav | grep metronome
metronome:   96 BPM, 3 to the bar, 2 per beat
```

It does not need `-M`. Asking for a click opens the output on its own, so you
can play to a metronome without also hearing yourself through it; add `-M` when
you want both, and name the output with `--monitor-device` either way. With
`--preroll` the click starts as soon as the recorder is armed, so the seconds
before you press Enter are a count-in.

The grid is counted in captured frames rather than off the clock, so it cannot
drift against the recording: at 120 BPM and 48 kHz the clicks were generated at
frames 0, 24000, 48000 and so on, exactly. Without `--preroll` frame 0 is the
first frame of the file, so the beats fall on round numbers you can line a
session up on later.

Generating a beat at a frame is not the same as anybody hearing it there. You
hear the click through the output's buffer, and what you play in response comes
back through the input's — the same round trip an overdub is placed by. Left
alone, a take played perfectly in time lands that whole round trip behind the
grid the clicks were counted on.

So the click is struck early by exactly that much, and arrives at your ears
where the grid says it should be. The grid itself does not move: beat n is
still frame n × 24000 of the file. `--latency` sets the correction, and
`latency_ms` in the config file sets it for both this and the desktop app's
overdubs, since it is the same measurement:

```sh
audiaki -M --click 120 --latency 12 take01.wav   # a measured round trip
audiaki -M --click 120 --latency 0 take01.wav    # no correction, as it was
```

The default is worked out from the buffer sizes, which is a good estimate and
not a measurement — the converters, the driver and the interface all add delay
that nothing here can see. Play a click into a loopback, look at where it lands
in the file, and put that number in `latency_ms`.

What is left after that is jitter rather than offset: playback is fed from one
loop and capture runs on another, so the two agree to within a few milliseconds
rather than to the sample. The systematic part is gone; the rest is the price of
not having one clock behind both.

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
metronome:   120 BPM, 4 to the bar
note:        second chorus, clean tone
```

The `metronome` line is there only when the take was recorded to a click, and
it is written under `ITMP` — a tag of audiaki's own, because RIFF/INFO has no
registered one for a tempo. An unknown four-character tag is the one thing every
reader of the format has to step over, so a tool that does not know it ignores
it. The alternative was an `acid` chunk, which does carry a tempo and also tells
a DAW the file may be stretched to the project's — the last thing a raw take
should be saying about itself.

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

A stamped take also reserves 36 bytes for a `ds64` chunk, which is what lets it
pass 4 GB. RIFF counts in 32 bits and stops there — about three and a half
hours of 24-bit stereo at 48 kHz — and RF64 is the standard way past it: the
sizes are written again in 64 bits and the 32-bit fields are set to `0xFFFFFFFF`
to say so. The room is reserved as a `JUNK` chunk, which every reader of the
format already skips, and only filled in on close if the take actually needed
it. So a take that stayed under 4 GB is an ordinary WAV, a take that did not is
an RF64 one, and no audio has to be moved to make the difference:

```sh
$ ffprobe -hide_banner long-take.wav
Input #0, wav, from 'long-take.wav':
  Duration: 05:12:41.30, bitrate: 2304 kb/s
```

`--no-metadata` has nowhere to put the reservation, so that one still stops at
4 GB — it asked for the plain header and the plain header is what it gets.

Metadata is written **before** the audio, which is where the BWF specification
requires `bext` and what means an interrupted take still carries its stamp. It
is read back from wherever it is, though: an editor that retags a file in place
often appends its tags after the payload instead, and `--info` walks on past the
audio to find them when there were none in front of it.

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
audiaki --play session-*.wav               # ...a whole session, in order
audiaki --play session-*.wav --shuffle     # ...in some other order
audiaki --play session-*.wav --repeat      # ...round and round
audiaki --play riff.wav --repeat-one       # one take, over and over
```

The meter is the recording meter with the clock showing a position rather than
a length, and no xrun counter — nothing is being written, so there are no
frames to lose:

```
 00:12 / 03:45 [##################|          ]  -8.4 dBFS
```

Files named after the first are played after it, in the order given, so a shell
glob is a playlist. `-t` applies to each of them rather than to the run, and a
file that cannot be read is stepped over the way `--info` steps over one — the
rest still plays, and the exit status still says something was wrong.

Seeking and pausing drop whatever is queued in the output, so both land where
they were asked for rather than a buffer's worth of audio later. An output that
will not take the file's rate is converted to one it will, rather than refused —
see [Hearing yourself](#hearing-yourself).

`--shuffle` plays them in a random order instead, and picks a fresh one each
time through a repeating list rather than cycling the same permutation forever.
`--repeat` starts the list again when it ends; `--repeat-one` stays on the
current file, which is what you want to play along to something. Neither runs
away with itself when nothing can be played: a whole pass over the list with
every file unreadable stops rather than spinning. `n` and `p` still move
through the list under any of them, and `q` or Ctrl+C still ends the run.

`--repeat` and `--repeat-one` are the last one typed rather than an error
together, and both are refused outside `--play` — there is no other playlist for
them to shape.

### The keys

At a terminal — both stdin and stderr have to be one — playback takes single
keys while it runs:

| | |
| --- | --- |
| `space` | Pause, or carry on |
| `←` / `→` | Back or forward five seconds |
| `↑` / `↓` | Forward or back thirty seconds |
| `home` | Back to the start of the file |
| `end` | On to the end of it |
| `n` / `p` | The next file, or the previous one |
| `q` | Stop, whatever is left of the playlist |

None of it is required. Over a pipe, from a script or under a service manager
there is no terminal to take keys from, and each file plays start to finish the
way it always did.

Two things follow from where the audio is when a key is pressed. A pause is
heard a buffer's worth of audio late, because what the output already holds is
played out before it falls silent; and the position the seeks move around in is
the position of what has been handed to the output, which is that same buffer
ahead of what is coming out of it — about a tenth of a second on the defaults.
Skipping with `n`, `p` or `q` deliberately does not wait for the queue, so it
is heard at once.

Ctrl+C stops immediately, as everywhere else. `--play` reads whatever the reader accepts, not only
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

## Mixing a session down

The window saves what was done to a set of takes as a `.aki` session file — see
[DESKTOP.md](DESKTOP.md#sessions). `--render` mixes one down with no window
involved, which is what makes a session something a script can use:

```sh
audiaki --render session.aki                  # -> session.wav, 24-bit
audiaki --render session.aki -o mix.wav --bits 16
for s in */*.aki; do audiaki --render "$s" -y; done
```

It opens no device, so it runs over ssh, in a build, or on a machine with no
sound server at all. A session refers to its takes rather than containing them,
so they have to be where it says they are; if one has moved, `--render` names it
and stops rather than writing a mix with a hole in it.

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

- Linux only, through ALSA or PipeWire. No JACK or CoreAudio backend.
- Rendering shells out to `ffmpeg`, so the codecs and their licensing are its
  business, not audiaki's.
- Video is rendered after the take, so a long take with video on means waiting
  roughly its own length again. Cancelling is always available, and the audio
  is safe on disk regardless.
- `--no-metadata` writes the plain 44-byte header, and a plain header has
  nowhere to put a 64-bit size — so that one still stops at the 4 GB RIFF limit,
  about 3.5 hours of 24-bit stereo at 48 kHz. A stamped take reserves the room
  and becomes an RF64 file instead.
- `-M` monitors through a buffer of its own, so what you hear is tens of
  milliseconds behind what you played. That is the output queue and not
  something a correction can remove: it is for checking a sound, not for
  playing along with one. What *is* corrected is where the click falls against
  it — see `--latency`.
- The click is one tempo for the whole take: no tempo changes part way through.
  The desktop app keeps its tempo in the session rather than in the take, and it
  is one tempo there too.
- The `metronome` stamp is written under `ITMP`, which is audiaki's own tag
  rather than a registered one, so other tools ignore it rather than showing it.
- `--play` takes its keys from a terminal, so a pipe or a service manager gets
  the old behaviour of playing each file start to finish.
- The tuner is monophonic: one pitch at a time, and no chords. The range it
  searches runs from a five-string bass's low B to a piccolo's top C, and
  `--tune-min`/`--tune-max` move either end — but the low end is quadratic, so
  reaching further down costs real work per reading.
- The desktop app's grid does not move audio that is already on the timeline:
  turning it on, or dividing it more finely, changes where the *next* edit
  lands and leaves every existing clip where it was.
- The system file chooser is used when `zenity` or `kdialog` is installed, and
  the built-in folder browser when neither is. That browser has no bookmarks, no
  recent places and no search — it is the fallback rather than the intent, and
  `AUDIAKI_FILE_CHOOSER=none` keeps it deliberately.
- Rate and channels are fixed for the session in the desktop app; only the
  device can be changed from the window. A device that disappears mid-take ends
  that take where it stopped — what was written stays on disk and on the
  timeline — and the same device coming back within thirty seconds carries the
  take on in the same file and the same clip. A device back at a different rate
  or channel count is not carried on at all, a file something else has appended
  to falls back to a second take on the same lane, and after the thirty seconds
  the stream still reopens but the take does not resume.
