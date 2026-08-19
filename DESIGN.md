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
- [Reloading the window](#reloading-the-window)

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
    spectral.c/.h an editable spectrum, and the audio resynthesised from it
    resample.c/.h windowed-sinc rate conversion, for the playback path
    loudness.c/.h how loud a take is: BS.1770 loudness and the true peak
  edit/         the project the window edits: tracks, clips, undo
    samples.c/.h  refcounted blocks of audio, shared and never changed
    track.c/.h    one lane: a sorted list of clips over those blocks
    doc.c/.h      the tracks, the selection, and the undo stack
    edit.c/.h     cut, copy, paste, delete, silence, trim, split, fade, gain
    mix.c/.h      what the project sounds like; playback and export share it
    project.c/.h  a session written down: which parts of which files sit where
    load.c/.h     a WAV becomes a track
    export.c/.h   the mix, back out as a WAV, FLAC, Opus or MP3
    repair.c/.h   the spectral edit, applied to a range of a lane
  take/         what a recording is, and what surrounds it
    take.c/.h     numbered take filenames for --take
    meta.c/.h     the LIST/INFO and bext chunks a take is stamped with
    info.c/.h     measure a finished take: levels, clipping, noise floor
    preroll.c/.h  the seconds held before a take starts, kept bit for bit
    latency.c/.h  where an overdub belongs, given how late it was heard
    calibrate.c/.h  measuring that lateness, rather than estimating it
  media/        bytes on disk, and pipes to other programs
    wav.c/.h      streaming WAV writer, and a tolerant WAV reader
    canvas.c/.h   RGBA framebuffer and the shapes the visualiser draws
    visualize.c/.h render a WAV to a video
    ffmpeg.h      pipe frames, or samples, to an ffmpeg child
    ffmpeg_posix.c  its fork/exec/pipe implementation
  term/
    meter.c/.h    the terminal peak and spectrum displays
    prompt.c/.h   the one question the recorder asks back
    keys.c/.h     single keypresses, for the transport --play runs under
  util/         no domain of its own
    log.c/.h      stderr diagnostics
    parse.c/.h    strict value parsing, shared with the window
    path.c/.h     joining, creating and moving, without ever clobbering
    config.c/.h   the preferences that outlive an invocation
    jsonout.c/.h  the little JSON --json needs
    ringbuf.c/.h  lock-free SPSC ring, capture thread -> drawing thread
    signals.c/.h  the shared Ctrl+C flag
  gui/          the desktop window; the only code that knows raylib exists
    app.h         the state its parts share
    main.c        the shell: the window, the run loop, the hot reload key
    plug.c        the app's own lifecycle: start, frame, and the way out
    take.c        the capture device, and the take being written to it
    actions.c     what the toolbar, the keys and the timeline all mean
    keys.c/.h     which of those a frame of the keyboard was asking for
    args.c        argv and the help text
    devices.c     the dropdown's list, kept level with the hardware
    save.c        where a take goes, and the browser that asks - also import
                  and export, which are the same question turned round
    timeline.c/.h the tracks: ruler, panels, waveforms, selection, zoom
    player.c/.h   hearing the timeline, fed from the drawing loop
    preview.c/.h  hearing a file from the browser, fed the same way
    screen.c      every pixel of the chrome
    engine.c/.h   the capture thread and its idle/recording/paused transport
    viz.c/.h      the glowing spectrum, drawn with raylib
    repair.c/.h   the spectrum of what was recorded, and drawing on it
    confirm.c     the question that stops an action until it is answered
    ui.c/.h       immediate-mode buttons, slider and meter
  hotreload/    only in a development build of the window
    plug.h        the five calls the shell reaches the app through
    hotreload.h   how it reaches them: directly, or through a library
    hotreload.c   loading that library, and loading it again
tests/          mirrors src/, so an untested layer is visible from the tree
fuzz/           the three parsers that read files audiaki did not write, and a
                committed corpus of what they have to survive
docs/           man page
vendor/raylib/  submodule, only needed for the desktop app
```

No audio system appears outside `backend/`: `device.c` and `monitor.c` are
dispatchers, and only the eight `*_alsa.c` / `*_pipewire.c` / `*_jack.c` /
`*_coreaudio.c` files include `<alsa/asoundlib.h>`, `<pipewire/pipewire.h>`,
`<jack/jack.h>` or the CoreAudio frameworks. `cmd/` drives that directory
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

Four of them, behind one pair of tables: ALSA and PipeWire on Linux, CoreAudio
on macOS, JACK on either. `backend.c` holds them in a single table — name,
aliases, the two ops pointers, and how to ask whether the thing is running — so
parsing a name, reporting availability, choosing one and listing what this build
has are four readings of one row rather than four switch statements to keep in
step. A backend that was not compiled in is a row with null ops, which is the
only difference between "PipeWire is not installed here" and "ALSA does not
exist on macOS".

Asking for a backend that is not there is an error rather than a quiet
downgrade: `--backend pipewire` on a machine with no daemon says so, instead of
recording through ALSA and leaving you to wonder why the device names changed.
Only `auto` falls back, because that is what it was asked to do — and it tries
PipeWire, CoreAudio and ALSA in that order.

JACK is not in that order, and is reached only by name (or when it is the sole
backend a build has). A JACK graph is something somebody wired up on purpose,
where the right capture source is a decision rather than a default; and on a
desktop running `pipewire-jack` a JACK server answering says nothing about what
the user wanted.

### Who converts

The three callback backends — PipeWire, JACK and CoreAudio — all deliver float
and all push, where ALSA hands over the hardware's own integers and is pulled
from. Both differences are absorbed in `backend/`: a wait-free ring
(`util/ringbuf.h`) between the callback and `aud_device_read()`, holding the
float that arrived, and `aud_format_from_float()` encoding it into the capture
layout in the reader rather than in the real-time callback. That keeps the
callback down to an interleave and a copy, and keeps one encoder shared by the
three rather than one each.

### What `--probe` means on each

Under ALSA, `--probe` asks the hardware what it supports, and the answer decides
what a recording can be. Under PipeWire the server converts, so anything audiaki
asks for is what it gets and the hardware's own list no longer governs — the
probe says so rather than printing a capability table that decides nothing.
Under JACK there is nothing device-shaped to ask: the rate and the period belong
to the server and are the same for every client on it, so the probe reports the
server's terms and counts the named client's ports, which is the one number that
does vary. CoreAudio is between the two — the device has a real list of rates,
and moving it to one of them is something `-r` genuinely does. For the question
"what can this card actually do", use `--backend alsa --probe`.

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

Which side resamples differs, and only that. PipeWire converts in the server, so
the monitor there asks for the capture rate and gets it. ALSA, JACK and
CoreAudio will each only run the output at a rate of their own choosing — the
card's, the server's, the device's — so those three put `audio/resample.h` in
the write path when it does not match. Nothing resamples what is written to a
take, in any of the four: the file is the samples the device delivered, at the
rate it delivered them.

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

What that exactness does **not** buy on its own is a sample-aligned take. The
click reaches the player through the output's buffer, tens of milliseconds under
ALSA and around a tenth of a second under PipeWire, so a performance played
perfectly in time sits that far behind the grid it was played to. What closes
that gap is striking the click early rather than moving the grid: the beat is
generated `lead_frames` ahead of where it belongs so that it *arrives* where it
belongs, and beat n is still frame n × spacing of the file. The correction is
the same round trip an overdub is placed by, from the same number — see
[Overdubbing](#overdubbing) and [Measuring the round
trip](#measuring-the-round-trip).

What is left after that is jitter rather than offset, and no constant removes
it. A metronome corrected this way keeps you at a tempo and lands the take close
to the grid; it does not sample-align it, and the documentation says so rather
than letting anyone assume otherwise.

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

A clip's gain is the third of these and the plainest: one number the window is
multiplied by, applied at the same point in `aud_track_read()` and for the same
reason. Turning a bar of a take up is then a split at each edge and a float on
the clip between them — no audio copied, an undo step that is a clip list like
every other, and `aud_track_range()` scaling the waveform to match so the
display is not the one thing in the editor that lies. It is deliberately not
the track's own `gain` field: that one is a fader over a whole lane, applied by
the mixer afterwards, and the two multiply because they answer different
questions.

Normalizing is that operation with the number measured rather than typed.
`aud_edit_normalize()` reads the selection through the same `aud_track_read()`
the mix uses, feeds it the BS.1770 meter from `audio/loudness.h`, and sets the
gain that lands its true peak or its integrated loudness on a target. Measuring
what the mix reads, rather than the blocks underneath, is what makes it
idempotent — the gain already there is part of what is measured, so normalizing
twice computes a factor of one — and it is why the same code serves both
targets. Each selected lane is measured separately: what makes two takes
comparable is each of them reaching the target, and one factor across all of
them would be a master fader wearing this name. A lane with nothing to measure
is left alone rather than multiplied by a guess, and every lane is measured
before any is changed, so a normalize that finds nothing measurable costs no
undo step.

### Moving a piece of it

Moving audio along a lane is the same trick once more: the clips inside the
range keep the blocks they were reading and are laid down at a different frame.
`aud_track_move()` splits at both edges so what moves is exactly the range asked
for, lifts that run of clips out of the list without touching their references,
adds the offset to each start, and puts the run back where the sort order now
wants it. Nothing is copied and nothing is freed, so moving a forty minute take
costs what moving a bar does — and the undo step it takes is a clip list, like
every other one.

What the model decides for us is what happens at the far end. Clips on a lane do
not overlap, so a move onto occupied ground is not a move that overwrites: it is
one that cannot happen. `aud_track_move_room()` answers how far the range could
go before it meets something, by looking at the parts of clips that lie
*outside* it — those are the parts that stay put — and the move refuses any
offset that is further than that. The window asks the same question every frame
of a drag and draws the outline where the answer says, so what you see the
pointer doing is what letting go will do.

Two things fall out of that, and both are honest rather than convenient. A range
with audio hard against both edges — a bar out of the middle of a take — has
nowhere to go at all, because the rest of the take is the obstacle. And the
distance is shared across every selected lane: `aud_edit_move_room()` threads
one offset through all of them and takes the least, because an overdub that
travelled further than the take it was played against would come back out of
time with it. A move that went as far as each lane individually allowed would be
the one edit here that could silently ruin a session.

The drag itself changes nothing until the button comes up. That is not
squeamishness about mutation — the project is only ever touched by the drawing
thread — but about undo: a move applied per frame would be a hundred steps deep
by the time it arrived, and one that turned out to have nowhere to go would have
to be unwound rather than simply never made.

### Taking the hum out

Every edit above is a window moving over audio that never changes. Spectral
repair is the one that is not, and it is worth being explicit about why: there
is no arrangement of clips that means *this take, without the 50 Hz buzz*. So
this one produces audio that did not exist before — the range is read out,
filtered, and put back as a new block.

The graph **is** the edit. `aud_spectral` holds a gain per FFT bin; the panel
draws that array and drawing on it writes to that array; applying multiplies the
audio by it. There is no separate list of filters that could drift out of step
with what is on screen, and the obvious gesture is the right one: a hum is a
spike standing out of the floor around it, so you drag the spike down to the
floor and it is gone.

Three ways in, because a spike you can point at is not the only kind of noise:

- **drag** — pull any part of the spectrum down to where the pointer is
- **Find hum** — score every candidate fundamental between 30 and 130 Hz on how
  far its harmonics stand above their surroundings, then notch the whole series
  at once. Mains hum is never the fundamental alone, and hunting down 100, 150
  and 200 Hz by hand is the tedium this removes
- **the noise profile** — an estimate of the noise on its own, subtracted from
  every frame, for hiss that is everywhere and has no spike to aim at

The profile is the interesting one, because the honest way to get it is a
stretch of recording with nothing played on it — and people do not always have
one. So there are two: `Learn` averages the current selection, for when you *do*
have silence, and `Guess` takes the per-bin **minimum over time** of the whole
selection, which needs none. A steady hum or hiss is in every window and so
cannot fall below itself; a note comes and goes and falls to the floor in the
windows between. The minimum is therefore what was underneath all of it, which
is a remarkably good noise floor for free.

The filtering is short-time Fourier work — overlapping Hann windows, 4096 points
at 75% overlap, scaled bin by bin and added back up. Two details are worth
keeping:

The overlap-add divides by the window energy that actually landed on each output
sample rather than by the constant it comes to in the middle. So a flat curve
returns the input **sample for sample**, the two ends included, instead of
fading in and out of the edit. "Changed nothing" means it, and the test asserts
it.

Bands are eased over a few bins at each edge rather than stepped. A wall in the
frequency domain is a long ring in the time domain: a brick-wall notch under a
bass note does not remove a hum, it removes a hum and adds a chirp after every
string it was under.

Two costs follow, and both are deliberate. It costs the range — a minute of
stereo is about twenty megabytes and a second of work, where a cut costs
neither. And it writes a WAV as it goes, because a project is a list of which
parts of which files sit where and a block from nowhere cannot be written down
(see below). Undoing a repair leaves that file behind rather than deleting it:
the redo stack still points at the block, and a file removed under it would turn
redo into silence.

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

### Measuring the round trip

The estimate above is made of the two buffer sizes, and it is honest about being
an estimate: the converters, the driver and the interface all add delay that no
buffer size describes, and on real hardware the estimate and the truth differ by
more than half. So the instruction has always been to measure it — and an
instruction to go and do something in another program is a feature that has not
been finished. `--calibrate` is the rest of it.

`take/calibrate.h` plays a burst, listens for it, and reports the frames between
the two. Three decisions carry it.

**One call advances the clock.** The number wanted is the distance between the
frame a burst was written to the output and the frame it arrived back on the
input, and that distance only means anything if both are counted on the same
clock. So `aud_calibrate_step()` takes the captured period and fills the
playback period in one call, and nothing else may move the frame counter. That
also makes the result the *right* correction rather than merely a fact about the
hardware: the click is mixed into the output for the period just captured, so
the burst and the beat sit in the same relationship to the capture clock.

**A sweep, not a click.** What happens to the return is a correlation, and a
tone correlates with itself once per cycle — a reading can land a whole cycle out
with nothing looking wrong. A sweep correlates with itself once and sharply, and
spreading its energy from 300 Hz to 3 kHz means it survives a path that rolls
off at either end. The match is normalised, so a quiet return is as findable as
a loud one, and taken on the absolute value, so a path that inverts polarity is
a match rather than a miss. The search is direct rather than through the FFT:
half a second of window against twenty milliseconds of burst is a few million
multiply-adds, less than the capture it is measuring took to arrive.

The burst is twenty milliseconds rather than ten because length is what
separates a real return from a chance one. The best match noise can manage
against a template falls off as the square root of the template's length, and
the search looks at half a second of noise for something short — at ten
milliseconds the best fluke lands close enough to the accept threshold to be
worth avoiding.

**Five of them, and a median.** Playback is fed from the capture loop and the
two are not the same crystal, so consecutive readings differ by a millisecond or
two however good the measurement is, and one reading cannot tell that apart from
a door slamming. Five can: the median first, then everything near it, then the
mean of what is left. A false match lands wherever the noise happened to look
most like a sweep, which is nowhere in particular, so it cannot drag a median
the way it would drag a mean — and once the outliers are gone the mean of the
survivors is the better estimate of the two. When too few agree, the run says it
could not tell rather than choosing between them.

There is no sub-sample interpolation. Resolution is a frame, 0.02 ms at 44.1
kHz, and the jitter reported alongside it is three orders of magnitude larger; a
number refined past its own repeatability is a false precision.

None of this touches a device, so it is unit tested against synthesised returns
— a run fed its own output back, delayed by a number the test chose — rather
than by plugging a cable in. `cmd/calibrate.c` is the part that owns the
streams, and it is the one place in audiaki where an output that will not open
is fatal: everywhere else playback is the convenience and the take is the
product, and here the output is half of what is being measured.

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

Exporting stems extends that rather than working around it. `aud_mix_read()`
adds every track together; `aud_mix_read_track()` adds one — and both reach the
same `add_track()`, which is the only place in the program where a track becomes
sound. So the stems add back up to the mixdown by construction rather than by
care: there is nowhere for a per-track gain, a pan law or a solo rule to be
applied twice or differently, because there is only one copy of each. The
exporter settles the rate, width, depth and range once for the whole set and
writes every file to them, and `edit/export.c` picks between the two readers on
one argument instead of holding a second copy of the write loop. A set of stems
that did not add up to the mix would be a set nobody could use, and that is a
strong enough guarantee to be worth building the code around rather than
asserting afterwards — though the test asserts it too.

### What the file it lands in is

An export can be a WAV, a FLAC, an Opus or an MP3, and which one is decided by
the extension of the name it is given rather than by an option beside it.

There is no `--format`, and not only because `-f` was already the capture sample
format. An option and a filename can disagree, and the two ways of resolving
that are both bad: honour the option and you write FLAC into a file called
`.wav`, honour the name and the option was a lie. One source of truth removes
the question, `--visualize out.mp4` already worked that way, and the whole
feature costs no new CLI surface — `--stems` composes with it for free, because
the stem names are built by putting the base's own extension back on.

An extension outside the list is refused before anything is opened, rather than
passed to ffmpeg to make of what it will. The list is short on purpose: samples
as they are, samples losslessly smaller, and the two lossy formats the rest of
the world plays. Adding another is a row in one table in `edit/export.c`.

WAV keeps its own writer. It is the only one that needs no ffmpeg, the only one
that can carry the metadata chunks, and the only one that can pass 4 GB by
becoming RF64 — none of which a pipe to an encoder does. The other three write
through `ffmpeg_start_encoding()`, which is the frame pipe with a different
argv: the same fork, the same SIGPIPE care, the same reaping. Refactoring those
three shapes of invocation onto one `execvp()` was most of the work, and it
removed a copy of the child setup rather than adding one.

What goes down that pipe is exactly the PCM the WAV writer would have been
handed, at exactly the depth `--bits` asked for. So `put_sample()` stays the one
place a float becomes an integer, the two paths cannot drift in how they round
or clamp, and a FLAC export is bit-identical to the WAV export of the same
session — which is a property worth having, and one the write loop gets for free
by not caring which sink it is talking to.

A bit depth is refused where it means nothing rather than ignored: a lossy file
holds no samples for a depth to describe, and FLAC holds 24 at the most. That
distinction needs `aud_export_options::bits` to tell "not asked for" from "asked
for 24", which is why `aud_export_defaults()` leaves it at zero instead of
filling in the default it documents. Quietly accepting `--bits 16 -o mix.mp3`
would be the same silence `--bits` was already fixed for once.

### Hearing a file that is not the project

The dialog asking where a finished take should go used to ask it about a file
nobody had heard, and `gui/preview.c` is the answer: a WAV read straight off
disk into an output of its own, pumped from the drawing loop exactly the way
the player above is.

It is a separate thing from `aud_player` on purpose. The player plays the
document, and knows about mixing, looping and the metronome; what the dialog is
asking about is a *file*, quite possibly at a rate or a channel count the
session is not. Loading it into the project to hear it would cost an undo step
and a track's worth of memory to answer a question that has not been answered
yet — and answering "no, keep it where it was" would then mean taking both
back. So the preview opens at the file's own geometry and touches nothing.

One output at a time: starting a preview stops the player. The project and the
file being asked about are two things to be listening to, and hearing them over
each other tells you nothing about either.

### When the cable comes out

A capture stream that dies cannot be revived, so the question is not how to keep
it but what to do with what is already there. The engine answers the first half
on the capture thread the moment the read fails: close the WAV, patch its
header, and say `AUD_ENGINE_FAILED`. The file is a complete take from that
instant on, whatever happens next.

The window answers the second half. `app_check_capture_loss()` runs at the top
of every frame, ahead of the device watch — deliberately, because the watch is
what tears the dead engine down and stands a new one up, and the engine that was
writing the take is the only thing left that can be asked what it wrote. It
drains the last of the ring onto the track, closes the clip that was growing,
points the block at the file, and notes the frame it stopped at.

The device coming back is `app_recover_engine()`, which already existed for a
window that started without one. What is new is that it then calls
`app_resume_take()`, and what that does is start *a new take* on the same lane
at that frame. Not a continuation of the old file: that file is closed and safe,
and reopening it to append would mean rewriting a finished recording to produce
one whose middle is a moment the interface was not running. Two clips that meet
exactly is what the track model is for.

Three things have to hold, and each is a way the obvious version would be wrong.
The lane has to still be there, because the timeline is editable while the
device is gone. The new device has to deliver the same rate and channel count,
because a device that comes back is not necessarily the device that went. And it
has to be within thirty seconds, because "the hardware returned" is a good
reason to carry on recording only while somebody is still standing there holding
the cable — a window left open overnight should not start writing to disk
because of something that happened at lunchtime.

## Measuring a take

The noise floor `--info` reports is the tenth percentile of the level of 50 ms
windows — in practice, the room and the cable with nothing being played. Compare
it with the peak to see how much of the range the instrument is using. A DC
offset much past ±0.001 usually means the interface has a bias worth fixing.

Each channel is measured on its own, so a clipped left channel cannot hide
behind a quiet right one.

### How loud, as opposed to how large

Peak and RMS both answer questions the ear does not ask, which is why
`loudness.c` exists beside them. Peak is headroom and nothing else. RMS weights
every frequency equally and the ear is far less sensitive at 50 Hz than at
3 kHz, so RMS rates a boomy take above a bright one that sounds twice as loud
next to it. Neither lets you say "that take came out four decibels quieter" and
be right, which is the question you actually have with twelve takes on disk.

ITU-R BS.1770-4 is the answer everything else agrees on, so it is the one used
rather than something invented here — the point of a loudness figure is that it
means the same thing to the next program that reads it. Two biquads standing in
for the ear, a mean square over 400 ms blocks overlapping by 75%, and a two-pass
gate: everything below -70 LUFS goes, then everything more than 10 LU below what
is left. The gate is what makes it a measure of the playing rather than of how
much silence surrounds it.

The coefficients are derived at the file's own rate rather than tabulated. The
standard prints its table at 48 kHz only, and a take here is as likely to be at
44.1; a 48 kHz filter used at 44.1 puts the shelf 350 Hz out. Re-deriving through
the bilinear transform is what makes the same performance recorded at two rates
read the same, which is the property the whole figure is worth having for.

The 400 ms block and the 3 s block the range is measured over share one history
of 100 ms sub-blocks rather than keeping their own overlapping windows, because
100 ms is the step both of them move by and summing runs of it gives either.

Every channel counts at full weight. BS.1770 lifts the surround channels and
drops the LFE, but which channel is which comes from a layout, and the reader
does not parse one — a take's channels are the inputs of an interface, in the
order it delivered them. Weighting the fourth input of a four-input box as an
LFE and dropping it would be a guess that silently changes the answer, and for
the mono and stereo takes this measures in practice the two rules agree anyway.

### Between the samples

The true peak is a different measurement in similar units. Sample peak sees only
where the samples landed; the waveform between two of them routinely goes
higher, and a converter or an encoder reconstructing it clips there while `peak`
reports headroom to spare. Four times oversampling through a twelve-tap
polyphase interpolator, as BS.1770-4 Annex 2 specifies.

Four is the accuracy limit rather than the filter, and it is worth knowing which:
the oversampled grid only looks between the samples every quarter of one, so a
peak falling between two of those points is missed by up to about 0.4 dB near
Nyquist. Widening the filter does not help — a wider one was tried and moved
nothing. Oversampling further would, but four is what every other implementation
uses, and agreeing with them is worth more than the last few tenths.

Doing that filtering on every sample made `--info` seven times slower, which for
a command whose job is to answer a question about a dozen takes at once is the
wrong trade. What it costs now is roughly double the old read, and the
difference is an exact test rather than an approximation: samples are grouped
twelve at a time, a window never spans more than two groups, and the filter
cannot lift a window past a known bound times its largest sample. When that
bound falls below the peak already found there is provably nothing in the group
to find, and it is skipped whole. On a real take that is nearly every group, the
loudest moment being one moment. The figure is identical either way — the
skipping only avoids arithmetic whose answer is already known to lose.

The reader is deliberately more forgiving than the writer is strict, because it
has to cope with files other tools produced: 8/16/24/32-bit PCM and 32/64-bit
float, in any chunk order, with unknown chunks skipped. A take interrupted
before its header was patched is reported as truncated rather than refused.

### The same measurement, while it plays

`--info` measures a file that has stopped; the window measures a mix that has
not. Both run the same meter, because a loudness that meant one thing on the
status bar and another in the report would be worth less than no loudness at
all.

`aud_player` creates an `aud_loudness` per pass and feeds it the mixed buffer in
`aud_player_pump()` — between the mix and the metronome, which is where the
project ends and what you happen to be hearing begins. The click is not
recorded, so it is not counted either, and the figure on screen is the one an
export of the same range would get.

What the transport wants and what a finished take wants are different readings
of the same history, though. A report wants the *loudest* 400 ms block and the
loudest 3 s block — the maxima are the interesting part of something that is
over. A meter wants the ones that just went past, because it is showing what is
happening now. `aud_loudness_read_live()` is that second reading: the latest
momentary and short-term blocks, plus the integrated figure gated over
everything so far, which is the number the mix will be judged on.

Those two live beside each other rather than one being derived from the other,
and there is a reason the latest blocks are kept in the meter rather than read
off the end of the histories: `range_of()` sorts the short-term history to find
its percentiles, so "the last element" stops being "the most recent one" the
moment anything calls `aud_loudness_read()`. Two doubles removes that coupling
entirely, and the order the two calls are made in stops mattering.

Failing to measure never stops playback. A rate BS.1770 cannot be derived at, or
no memory for the block history, costs the readout and nothing else — the
transport's job is to play the project, and a meter is not worth a silence.

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
stopping now is exactly what was asked for. So does a skip to the next file: `n`
means now, and a buffer of the file being left over the top of the one arriving
is not what was asked for either.

### Taking keys without owning the loop

The transport — pause, seek, next — needed keys, and the loop it runs in cannot
stop to wait for one: it is the thing keeping the output fed, and a blocking
read on stdin would starve it into a dropout every time nobody typed. So
`term/keys.c` puts the terminal into a mode where a key arrives without Enter
behind it (`~ICANON`) and a read returns immediately whether or not anything did
(`VMIN` and `VTIME` both zero), and the loop asks once per pass round.

`ISIG` is deliberately left on. Ctrl+C is how everything else in audiaki stops,
and a player that had quietly taken that over would be the one command that
behaved differently.

The bytes are kept between calls rather than decoded a read at a time. A cursor
key is a three byte escape sequence and a fast pair of presses can arrive in one
read, so `aud_keys_poll()` hands out one key per call from a small buffer;
decoding a read as a single key would lose most of an arrow held down.

None of it is required. `aud_keys_open()` refuses unless both stdin and stderr
are a terminal — over a pipe there is somebody else's data on stdin, and no
terminal to take it back from — and a refusal is not an error: playback then
does what it always did and runs each file from beginning to end.

Seeking is `wav_read_seek()`, which is why the reader keeps the offset its data
chunk started at. A take stamped with its metadata has a `LIST`/`INFO` and a
`bext` chunk ahead of the audio, so frame zero is nowhere near byte 44, and a
seek that assumed the canonical header would decode the stamp as samples.

## Reloading the window

The desktop app can be rebuilt while it is running. `make HOTRELOAD=1 gui`
produces a shell and a library beside it, and F5 in the window throws the
library away and loads the one you have just built. Recording keeps recording;
the timeline, the selection and the undo stack stay exactly as they were. It is
the same trick, and largely the same shape, as tsoding's musializer, which is
also where the visualiser came from.

The split is not by subject matter but by what cannot be unmapped:

| In the shell | In the library |
| --- | --- |
| `main.c` — the window and the run loop | everything else in `src/gui` |
| `engine.c` — the capture thread | |
| all of `backend`, `edit`, `take`, `media`, `util` | |
| raylib | |

A thread executing code from a library that is about to be `dlclose`d is not a
bug that reports itself politely, so the capture thread stays in the shell. So
does raylib, and for a sharper reason: raylib's window handle, its GL context
and its input state are file-scope variables inside it, and a second copy of
them would be a second window. The library is therefore linked against nothing
at all. Every call it makes — `DrawRectangle`, `aud_engine_read_take`, `malloc`
— is left undefined and resolved against the executable when it is loaded,
which is what `-rdynamic` and `--whole-archive` in the Makefile are for. There
is exactly one of everything.

What crosses the line is five calls, listed once in `hotreload/plug.h` and
expanded three ways: into typedefs, into declarations, and into the `dlsym()`
names. `AUD_PLUG(frame)()` is a pointer through a library in a development
build and an ordinary function call in a release one, which is the point — the
shipped binary has no `dlopen` in it and no second file to find.

### What the session has to survive

The state is one heap allocation. A `static app` would be unmapped along with
the code, so `plug.c` allocates it, `aud_plug_pre_reload()` hands the pointer
back, and whichever library comes next takes it as its own.

That leaves two ways for a reload to go wrong, and both are handled where they
happen rather than left as folklore.

The first is a pointer into the code. The state holds one — `style_labels`,
which points at the visualiser's own names for its styles, string literals on
pages that a reload frees. `aud_plug_post_reload()` looks them up again. It is
the only such pointer in `app`, and the reason there is a rule that there may
not be another: the device names, the folder listings and the status line are
all arrays inside the state, not pointers into whoever wrote them.

The second is the layout of the state itself, which is what actually bites. A
build that lays `app` out differently sees every field of an old session
somewhere else — silent corruption, the worst possible outcome for a
convenience feature. So `app.self_size` is the first field of the struct, the
one place a differently built copy can still find, and `post_reload` refuses a
session it does not agree with and says which header must have changed. Editing
`app.h`, `timeline.h`, `player.h` or `viz.h` means restarting; editing what the
window draws does not.

Anything the library allocated whose shape only it knows is let go in
`pre_reload`, while the code that made it is still the code doing the letting
go: the analyser is destroyed there and built again in `post_reload`, and a
video render in flight is cancelled. That is what makes `viz.c` and `render.c`
as freely editable as the drawing they serve — including their own structs,
because nothing of theirs outlives the library that allocated it.

Finally, the reload is attempted before anything is closed. The library is
copied to a name never used before and loaded from that — `dlopen()` keys on the
name it was given, and the linker writes a rebuilt library back over the same
path, so opening that path again is as likely to hand back the code already
running as the new one — and the old library is only released once the new one
has bound every symbol it needs. A build that does not compile costs a line on
the terminal, not the take.
