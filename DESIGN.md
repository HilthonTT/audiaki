# Design notes

Why audiaki is built the way it is. [docs/USAGE.md](docs/USAGE.md) and
[docs/DESKTOP.md](docs/DESKTOP.md) cover what it does and how to drive it; this
covers the decisions behind it, and is aimed at anyone changing the code.

- [Layout](#layout)
- [Backends](#backends)
- [The metronome](#the-metronome)
- [Analysis](#analysis)
- [The visualisers](#the-visualisers)
- [Rendering video](#rendering-video)
- [Pitch detection](#pitch-detection)
- [Pre-roll](#pre-roll)
- [Where takes go](#where-takes-go)
- [The editor](#the-editor)
- [Measuring a take](#measuring-a-take)
- [What a take carries](#what-a-take-carries)
- [Playing one back](#playing-one-back)

## Layout

One directory per layer, and each one only reaches downwards.

```
src/
  main.c        picks a command; that is all it does
  options.h     the request an invocation describes - cli fills it, cmd reads it
  cli/          argv in, an aud_options out
    cli.c/.h      the option table and the parse loop
    usage.c       the help text and the version line
  cmd/          one file per command: options in, an exit code out
    cmd.h         the eight entry points, and the shared capture config
    record.c      capture loop: device -> repack -> WAV
    playback.c/.h what you hear while recording: the monitor and the metronome
    play.c        the --play loop: WAV -> monitor -> terminal
    tune.c        the --tune loop: device -> tuner -> terminal
    render.c      mixing a saved session down, with no device opened
    info.c        how --info lays out one take, or a session of them
    visualize.c   naming and guarding the video the renderer writes
    devices.c     the --list table and the --probe hand-off
  backend/      the only directory that reaches an audio system
    backend.c/.h  which one to talk to, and the tables it is reached by
    device.c/.h   the capture interface, dispatching to the chosen backend
    device_alsa.c       ...over libasound
    device_pipewire.c   ...over libpipewire, when it was compiled in
    monitor.c/.h  the playback interface, dispatching the same way
    monitor_alsa.c      ...over libasound
    monitor_pipewire.c  ...over libpipewire
  audio/        samples, and what can be computed from them
    format.c/.h   sample formats, peak detection, repacking
    fft.c/.h      radix-2 FFT and the Hann window
    spectrum.c/.h streaming analyser: samples in, bar heights out
    tuner.c/.h    pitch detection: samples in, a note and its offset out
    click.c/.h    the metronome: a beat grid, mixed into what you hear
  edit/         the project the window edits: tracks, clips, undo
    samples.c/.h  refcounted blocks of audio, shared and never changed
    track.c/.h    one lane: a sorted list of clips over those blocks
    doc.c/.h      the tracks, the selection, and the undo stack
    edit.c/.h     cut, copy, paste, delete, silence, trim, split, fade
    mix.c/.h      what the project sounds like; playback and export share it
    project.c/.h  a session written down: which parts of which files sit where
    load.c/.h     a WAV becomes a track
    export.c/.h   the mix, back out as a WAV
  take/         what a recording is, and what surrounds it
    take.c/.h     numbered take filenames for --take
    meta.c/.h     the LIST/INFO and bext chunks a take is stamped with
    info.c/.h     measure a finished take: levels, clipping, noise floor
    preroll.c/.h  the seconds held before a take starts, kept bit for bit
    latency.c/.h  where an overdub belongs, given how late it was heard
  media/        bytes on disk, and pipes to other programs
    wav.c/.h      streaming WAV writer, and a tolerant WAV reader
    canvas.c/.h   RGBA framebuffer and the shapes the visualiser draws
    visualize.c/.h render a WAV to a video
    ffmpeg.h      pipe frames to an ffmpeg child
    ffmpeg_posix.c  its fork/exec/pipe implementation
  term/
    meter.c/.h    the terminal peak and spectrum displays
    prompt.c/.h   the one question the recorder asks back
  util/         no domain of its own
    log.c/.h      stderr diagnostics
    parse.c/.h    strict value parsing, shared with the window
    path.c/.h     joining, creating and moving, without ever clobbering
    config.c/.h   the preferences that outlive an invocation
    jsonout.c/.h  the little JSON --json needs
    ringbuf.c/.h  lock-free SPSC ring, capture thread -> drawing thread
    signals.c/.h  the shared Ctrl+C flag
  gui/          the desktop window; the only code that knows raylib exists
    app.h         the state its four halves share
    main.c        the run loop, the engine's lifecycle, the transport actions
    args.c        argv and the help text
    devices.c     the dropdown's list, kept level with the hardware
    save.c        where a take goes, and the browser that asks - also import
                  and export, which are the same question turned round
    timeline.c/.h the tracks: ruler, panels, waveforms, selection, zoom
    player.c/.h   hearing the timeline, fed from the drawing loop
    screen.c      every pixel of the chrome
    engine.c/.h   the capture thread and its idle/recording/paused transport
    viz.c/.h      the glowing spectrum, drawn with raylib
    ui.c/.h       immediate-mode buttons, slider and meter
tests/          mirrors src/, so an untested layer is visible from the tree
docs/           man page
vendor/raylib/  submodule, only needed for the desktop app
```

No audio system appears outside `backend/`: `device.c` and `monitor.c` are
dispatchers, and only the four `*_alsa.c` / `*_pipewire.c` files include
`<alsa/asoundlib.h>` or `<pipewire/pipewire.h>`. `cmd/` drives that directory
and `cli/` names it, so everything under those three — `audio`, `take`, `media`,
`term` and `util` — is plain C that builds and tests anywhere, which is what CI
does.

That is a claim the build enforces rather than one this file merely makes. The
Makefile's `PORTABLE_SRCS`, which the tests link against, is
`$(filter-out src/backend/% src/cmd/% src/cli/% src/main.c,$(SRCS))` — a
directory rule, not a list of files. A new module in any lower layer is tested
without anyone remembering to add it, and a lower layer that starts reaching for
a sound server stops linking.

The analysis is shared three ways: the terminal display, the video renderer and
the desktop app all run the same `spectrum` module, the terminal and desktop
tuners run the same `tuner`, and the CLI's armed wait and the window's idle
capture fill the same `preroll`. Nothing in `src/` outside `src/gui/` depends on
raylib, which keeps the CLI buildable with the submodule absent.

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

It is off until asked for, in the window and on the command line alike, and says
so when it starts. The default capture device on a laptop is the built-in
microphone and the default output is the speaker beside it; monitoring one
through the other is a feedback loop that reaches full scale in a fraction of a
second, and no amount of care inside the program prevents that.

Nothing about it is allowed to end a take. An output that will not open, or that
fails halfway through a recording, is reported and dropped while the capture loop
carries on — the file is the product and the monitor is a convenience, so the
convenience is what gives way. The gain applies to the monitor alone for the same
reason: the take is written from the samples the device delivered, whatever is
being listened to at the time.

Under ALSA, monitoring needs an output that accepts the capture rate directly.
Resampling would mean carrying an interpolator around for a convenience feature,
so audiaki declines to monitor rather than play back at the wrong pitch. The
PipeWire backend has no such limit, because the server resamples anyway.

## The metronome

The click goes into the monitoring stream and nowhere else. That is the same
rule `--monitor-gain` follows and for the same reason: the file is written from
the samples the device delivered, so what the person recording was listening to
at the time cannot get into it. Mixing the click into the take would also be
irreversible, and a take with a click printed on it is worth less than the take
plus the tempo written down.

Because one output carries both, `recorder.c` owns a single playback path with
two optional sources rather than two streams. Two would mean two clocks, two
sets of dropped frames, and a click that drifts against the monitoring beside
it. It also means asking for a click opens the output whether or not `--monitor`
was given, which is what lets you play to a metronome without hearing yourself
through it — and why `--monitor-device` stops implying `--monitor` once there is
a click, since naming the output is then no longer a request to hear the input.

The grid is a pure function of the absolute frame index, not a counter ticked
once per period. Beat *n* is at `round(n * 60 * rate / bpm)` whatever happened to
the periods before it, so rounding cannot accumulate and a dropped buffer costs
one click rather than shifting every click after it. Since the frames counted
are the frames captured, the tempo is measured by the capture clock — the one
clock that cannot drift against the recording — and at 120 BPM and 48 kHz the
beats were generated at frames 0, 24000, 48000 and so on exactly. This is the
same reasoning as `--play`'s backpressure: pace on the audio, never on
`CLOCK_MONOTONIC`.

What that exactness does **not** buy is a sample-aligned take. The click reaches
the player through the output's buffer, tens of milliseconds under ALSA and
around a tenth of a second under PipeWire, so a performance played perfectly in
time sits that far behind the grid it was played to. Closing that gap means
measuring the round trip and compensating, which is a different feature with a
calibration step in it. A metronome keeps you at a tempo; it does not make the
result line up with a timeline, and the documentation says so rather than
letting anyone assume otherwise.

A beat is a sine burst under an exponential decay — 50 ms over an 8 ms time
constant — with the downbeat an octave above the others. No wavetable, so
there is nothing to load or to ship, and the accent is the same sound rather
than a second one. The 50 ms burst is also what keeps the mixing loop simple:
the fastest tempo accepted is 300 BPM, 200 ms apart, so two bursts can never
sound at once and the walk over a buffer only ever has one beat to think about.

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

## Where takes go

Two rules, and everything else follows from them.

**A take is never written twice.** The folder is decided before the device is
opened, so `--dir` and `take_dir` place the file rather than move it afterwards.
A forty minute session going to an external drive is written there once; a
recorder that copied it at the end would sit there for a minute doing nothing
visible, which is where people reach for Ctrl+C.

**A take is never lost to a question.** So nothing is asked before recording —
the answer to "what should this be called?" is a number, handed out by
`take.c`, and it can be changed later. Afterwards is different: the WAV is
closed and complete on disk before the first prompt is drawn, and every way out
of the question except answering it leaves the file exactly where it is. That
includes Ctrl+C, end of input, `Esc`, the window's close button, and any
failure at all.

The move itself is `aud_path_move()`, and deliberately not `rename(2)`: rename
replaces its destination without a word, and the one thing this must never do is
drop a good take on top of an older one. A hard link claims the name atomically
and the original goes only once it is held; across filesystems, where there is
no link to make, the bytes are copied under an `O_EXCL` create and the original
is removed after they all arrived. `EEXIST` comes back to the caller, which asks
again.

The terminal asks only when there is a terminal to ask at, which is the same
reason `--preroll` treats end of input as a keypress: a program waiting on a
question nobody can answer is a program that has hung. The window has no such
doubt — someone is looking at it — so its dialog opens unless it was turned off.

One consequence worth naming: with **Video** on, the render waits for the dialog
rather than starting beside it. The MP4 is made from the take, and it should be
made from wherever the take ended up.

## The editor

One decision holds the whole thing up: **audio arrives once and is never written
to again**. A take is a block of float samples; a clip is a window onto part of
one, placed at some frame of the timeline; a track is a sorted, non-overlapping
list of clips. Every edit is clip surgery over shared blocks and copies no
samples at all.

Splitting makes two clips over one block. Deleting a range moves the windows in
and shifts what follows. Pasting inserts clips that point at somebody else's
audio. Cutting an hour-long take costs the same as cutting a bar of one.

What that buys is undo. A snapshot of the whole project is a copy of its clip
lists with the reference counts bumped — a few kilobytes for a session holding
hundreds of megabytes — so the undo stack is sixty-four whole-project states and
nobody has to think about it. Undo that copied audio would be unaffordable at
these sizes, and an editor without undo is not an editor.

The one exception is the block being recorded into, which has exactly one owner
for as long as the take lasts and so can grow without moving anything anybody
else is holding. That is what lets the waveform appear while it is being played
rather than when it stops.

A fade is the same idea again. It is two lengths on the clip - frames of ramp at
the head and at the tail - applied on the way out by `aud_track_read()`, not
written into the block. Baking one in would cost the block its immutability and
undo its affordability with it. What follows from the model is that there are no
crossfades: clips on a lane do not overlap, and a crossfade needs two pieces of
audio sounding at once. A split inside a ramp truncates it, because a clip can
say "ramp up from silence" and has no way to say "carry on from half way up one
that started in the clip before".

### Writing it down

A session is a list of clips, so saving one is cheap for exactly the reason
editing is: the audio is already on disk as the takes and imports it came from,
and the project file says which parts of which files sit where. Hundreds of
megabytes of session writes out as a few kilobytes of text.

That only works if a block can be found again, so each one carries the path it
was read from - set by the importer, and by the window when a take stops and
again if the dialog moves it. A block with no such path cannot be written by
reference, and is named rather than silently dropped.

Text, line-based, for the same reasons the config file is: it can be read,
diffed, fixed by hand and put in version control. Sources under the project's
own folder are stored relative to it, so a session folder can be copied to
another disk and still open. A load builds the project up separately and swaps it
in only once every source has been found, so a failed open leaves whatever was
already on the timeline alone - and a missing take is named rather than opening
as a silent lane, because losing audio quietly is the one failure a recorder
must not have.

Because none of that needs a device, `audiaki --render` mixes a session down
with no sound server involved at all.

### Overdubbing

Playing along to what is already recorded means hearing it first, and hearing it
costs time: the output holds a buffer before a sample is audible, and the input
holds another before a captured sample arrives. So what was played in response to
timeline frame N does not arrive labelled N - it arrives a round trip late, by
the same amount every time.

The fix is not to record differently but to place the result differently: the
clip starts a round trip earlier than the button was pressed, because that is
when the sound was made. `take/latency.h` is that arithmetic and nothing else -
no device, no clock - so it is unit tested rather than tuned by ear. Near the
start of the timeline, where there is not a round trip's worth of room to shift
into, the remainder comes off the front of the take instead; those frames
describe a moment before frame zero and there is nowhere to put them.

The systematic part is what this removes. The jitter is not: playback is fed from
the drawing loop and capture runs on its own thread, so the two start within a
drawn frame of each other rather than on the same sample. Overdubs land close,
not sample locked, and that is a property of having no single clock behind both
rather than something a constant can correct. It is worth saying plainly rather
than pretending away.

### Drawing it fast

A waveform is one column of pixels per span of frames, and at a zoom that fits
an hour on screen a column covers a hundred thousand samples. Reading them all,
sixty times a second, is not on.

So each block carries a two-level peak index: the minimum, maximum and RMS of
every 256 frames, and of every 256 of those. A column reads whichever level
covers its span in a sensible number of steps, and the loose samples at either
end directly, so the answer is exact rather than rounded out to a bucket
boundary. While a take is being recorded the index only covers the buckets that
have filled; the tail is scanned, which is at most 256 frames of it.

Both readings are drawn - the peak envelope, and the RMS as a solid core inside
it - because either alone misleads. Peaks make a quiet take with occasional
transients look as loud as a compressed one; RMS hides the transient that
clipped.

### Playing it

No thread. The output says how much it will take, so the drawing loop hands it
exactly that much each frame and the output's own consumption is the clock —
the same trick `cmd/play.c` uses to play a file at the right speed without one.

That is not laziness, it is what keeps the project single-threaded. An edit made
while playback runs cannot race the mix, because the mix happens between two
edits rather than beside them. A playhead a buffer behind what has been handed
over is a much smaller problem than a data race in a cut.

Playback and export mix through the same function, deliberately: a project that
played back differently from how it exported would be a project you could not
trust, and one piece of code is the only way to be sure of that.

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

## What a take carries

A take is stamped with a `LIST`/`INFO` block and a `bext` block — the Broadcast
Wave extension every field recorder writes. Two chunk formats rather than one
because they are read by different things: `LIST`/`INFO` is what taggers and
players already understand, and `bext` is what audio tools expect, including the
time reference that lets two takes from a session line up on a timeline without
either carrying timecode.

They go **before** the audio, which costs more than appending them would. The
data chunk no longer starts at offset 44, so the two size fields are patched
separately on close rather than by rewriting one header. What that buys is a
file that is already described before a single frame is in it: a recording
killed halfway still says what made it and when, and BWF requires `bext` ahead
of `data` in any case.

Reading them back is the same walk `--info` already did, so only chunks before
the audio are seen — the reader stops at `data`, and continuing past it would
mean seeking through gigabytes of payload on every open to find tags almost no
file has.

The clock is not in `meta.c`'s builder. `aud_meta_build()` is a pure function of
the struct it is handed and `aud_meta_stamp_now()` is what asks the system for
the time, which is the whole reason the chunk layout can be unit tested at all —
a builder that called `time()` could only be tested against itself.

There is no standard RIFF tag for "the device this was captured from". The
device goes in the `bext` coding history, whose free text `T=` field is meant
for exactly this, and in `ISRC`, which in RIFF/INFO means the source of the
material rather than the music industry's recording code.

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
