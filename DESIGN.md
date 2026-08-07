# Design notes

Why audiaki is built the way it is. [docs/USAGE.md](docs/USAGE.md) and
[docs/DESKTOP.md](docs/DESKTOP.md) cover what it does and how to drive it; this
covers the decisions behind it, and is aimed at anyone changing the code.

- [Layout](#layout)
- [Backends](#backends)
- [Analysis](#analysis)
- [The visualisers](#the-visualisers)
- [Rendering video](#rendering-video)
- [Pitch detection](#pitch-detection)
- [Pre-roll](#pre-roll)
- [Measuring a take](#measuring-a-take)
- [Playing one back](#playing-one-back)

## Layout

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
  monitor.c/.h  the playback side: monitoring an input, and --play
  monitor_alsa.c     ...over libasound
  monitor_pipewire.c ...over libpipewire
  play.c/.h     the --play loop: WAV -> monitor -> terminal
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

## Backends

Asking for a backend that is not there is an error rather than a quiet
downgrade: `--backend pipewire` on a machine with no daemon says so, instead of
recording through ALSA and leaving you to wonder why the device names changed.
Only `auto` falls back, because that is what it was asked to do.

### What `--probe` means on each

Under ALSA, `--probe` asks the hardware what it supports, and the answer decides
what a recording can be. Under PipeWire the server converts, so anything audiaki
asks for is what it gets and the hardware's own list no longer governs — the
probe says so rather than printing a capability table that decides nothing. For
the question "what can this card actually do", use `--backend alsa --probe`.

### Monitoring

The write path never blocks. If the playback device falls behind — which it
will, because capture and playback are not the same crystal — the frames that do
not fit are dropped rather than queued. A monitor that drifts further behind the
longer you record is worse than one that skips. Playing a file back needs the
opposite of that rule and gets it by asking first; see
[Playing one back](#playing-one-back).

Under ALSA, monitoring needs an output that accepts the capture rate directly.
Resampling would mean carrying an interpolator around for a convenience feature,
so audiaki declines to monitor rather than play back at the wrong pitch. The
PipeWire backend has no such limit, because the server resamples anyway.

## Analysis

Bands come from a 2048-point Hann-windowed FFT, spaced logarithmically so an
octave takes the same width wherever it falls. Each band shows the strongest bin
it covers and rises fast but falls slowly, so a plucked note reads as an attack
rather than a flicker.

The same `spectrum` module feeds the terminal's `--spectrum`, the video
renderer's `bars` style and the desktop app's first five visualisers. One
analyser means one set of constants to tune and one place for a bug to live.

## The visualisers

The desktop app's first five styles read the analysis above. `bars` is the
default, after [musializer](https://github.com/tsoding/musializer) — the glow is
one radial gradient texture drawn additively, so overlapping halos sum towards
white and loud clusters bloom. `mirror` and `radial` reuse the same caps.

`scope` draws raw samples rather than the spectrum, and starts each sweep at a
rising zero crossing so a steady note stands still instead of scrolling. It
shows true amplitude, so a quiet input is a quiet trace — that is the meter's
job to explain, not the scope's.

`waterfall` is the only one with a memory: about eight seconds of history as a
ring of texture columns, one written per frame, so a hum or a dropout is still
on screen after it has happened.

`tuner` is not a picture of the sound at all. It runs the same detection as the
CLI's `--tune`, and only while it is the visible style: the detection costs
millions of operations a go, and paying for it behind a style nobody is looking
at would slow every video render down. It sits with the visualisers because
tuning up is what you do immediately before pressing record, and it wants the
same place on the screen.

## Rendering video

audiaki does the analysis and the drawing itself, then pipes raw RGBA frames to
`ffmpeg`, which encodes them and muxes in the original audio untouched. No
graphics library is involved: everything is rasterised into a plain pixel
buffer, which is why `--visualize` works on a headless box.

In the desktop app, video is rendered **after** the take stops, not captured
live off the screen. Grabbing the framebuffer sixty times a second on the
machine holding a capture stream open is how takes end up with xruns in them.
Rendering afterwards costs the wait, but it is frame-accurate, independent of
the window's size and refresh rate, and cannot drop a frame. On a typical
machine a 4-second take renders in about 5 seconds at 720p60 — roughly real
time.

The `.mp4` only appears once finished: an MP4 is not playable until its final
index is written, so frames go to a hidden `.take-003.partial.mp4` and the real
name is created by a rename at the end. A cancelled, failed or killed render
leaves you with the take and no video, rather than a file that looks like a
video and will not open.

The CLI's `waveform` style reads the file once up front to reduce it to one
min/max pair per column, then sweeps a playhead across. That pass costs by the
width of the video, not the length of the take, so an hour is no more expensive
than a minute.

## Pitch detection

The pitch comes from the [YIN][yin] difference function, not from looking for
the loudest frequency. A plucked low E often has more energy at 165 Hz than at
82 Hz, and a tuner following the loudest frequency would say the string is an
octave up — worse than no tuner.

The tradeoff is one pitch at a time: it tunes strings, it does not name chords.
Anything quieter than about −52 dBFS is not treated as a note, so the room does
not read as a pitch, and a reading is held briefly after the note decays so the
display does not blink between strums. Past 50 cents the note name changes
rather than the needle running off the scale, so it never points at the wrong
note.

[yin]: https://audition.ens.fr/adc/pdf/2002_JASA_YIN.pdf

## Pre-roll

The audio is held exactly as the device delivered it, so the pre-roll seconds
are the same samples the rest of the take is made of, not a converted copy. This
is why `preroll` is its own module and not `ringbuf`: the ring carries floats
for the visualiser and the monitor, and a round trip through float would not
return a 24 or 32 bit take bit for bit.

The cost is memory: ten seconds of 24-bit stereo at 48 kHz is about 3 MiB, a
minute about 17 MiB, and a wider or faster stream proportionally more. The
ceiling is 300 seconds.

`--duration` is measured from the keypress, so `--preroll 10 -t 30` is a 40
second file: ten seconds of lead and the thirty you asked to record.

In the desktop app there is nothing to arm — the capture stream is already open
before you press anything, which is what makes the meters live — so `--preroll`
there just means every take begins that far back. Nothing is kept while a take
is recording or paused: audio already in the file is not held to be written
twice, and audio you paused out is not smuggled back in by resuming.

## Measuring a take

The noise floor `--info` reports is the tenth percentile of the level of 50 ms
windows — in practice, the room and the cable with nothing being played. Compare
it with the peak to see how much of the range the instrument is using. A DC
offset much past ±0.001 usually means the interface has a bias worth fixing.

Each channel is measured on its own, so a clipped left channel cannot hide
behind a quiet right one.

The reader is deliberately more forgiving than the writer is strict, because it
has to cope with files other tools produced: 8/16/24/32-bit PCM and 32/64-bit
float, in any chunk order, with unknown chunks skipped. A take interrupted
before its header was patched is reported as truncated rather than refused.

## Playing one back

`--play` is the WAV reader, the playback stream monitoring already used and the
same meter, wired together in `play.c`. No capture device is opened, and no new
audio system code was needed for it: the two `monitor_*.c` backends were already
the thing that turns interleaved floats into sound.

What it did need was backpressure, and that is the interesting part. Monitoring
drops whatever will not fit, because a monitor that queues instead of skipping
puts the sound further behind the strings every second. A file has no pace of
its own — it can be read as fast as the disk answers — so under that rule
playback would consume the whole take in a moment and drop all but the first
buffer of it.

So `aud_monitor_space()` reports how much will fit, and the play loop reads
exactly that much and sleeps when the answer is zero. The output's own
consumption becomes the clock, which is the one clock that cannot drift against
the device: pacing on `CLOCK_MONOTONIC` instead would be open loop, and a card
running a few parts per million off nominal would slowly starve or overrun.

`aud_monitor_drain()` is the other half. Closing the stream when the last frame
has been handed over cuts off everything still queued — inaudible while
monitoring, because the input is still arriving, and the last hundred
milliseconds of the take when playing a file. Ctrl+C skips the drain, since
stopping now is exactly what was asked for.
