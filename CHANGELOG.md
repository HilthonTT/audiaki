# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **A recovery file, so a window that is killed does not take the afternoon
  with it.** While there are unsaved edits the desktop app keeps one, rewritten
  every thirty seconds, and opens it again the next time it starts.

  The takes were never what was at risk: each is a closed WAV the moment it
  stopped. What only existed in memory was everything done to them since — the
  cuts, the overdub placed against the click, which lane is which — and until
  somebody pressed ctrl+S that was one segfault away from never having happened.
  Closing the window properly already wrote the edits out; a window that is
  killed, or that goes down with the machine, never reaches that.

  A session file is a few kilobytes of text saying which parts of which files
  sit where, which is what makes writing one every half minute cost nothing
  worth measuring. It goes beside what it shadows:

  ```
  song.aki                 ->  song.aki.recover
  a session never saved    ->  recovered.aki.recover, beside the takes
  ```

  **It never writes to a file you named.** Auto-saving over the session itself
  would destroy the one thing saving gives you — a state to go back to — and it
  would do it exactly when you had not saved because you were not sure yet. The
  recovery file is a sidecar and the session is untouched until ctrl+S.

  **It is removed the moment the work is safe** — a save, a save as, an undo
  back to where the file already was, or the window closing properly. That is
  what keeps finding one meaningful: a clean exit leaves none, so one at startup
  means a window died and never anything else. It is driven by the state each
  frame rather than hooked into each of those places, so a save as moves it
  without anything having had to tell it that a save as happened.

  **Starting up with one there opens it** and says so, rather than asking.
  There is nothing to lose by opening it — what is on disk is still on disk, and
  opening the file again is the way back — and the session is left marked
  unsaved, so the difference between what is on screen and what is in the file
  is not discovered at the next save. A recovery file older than the session it
  shadows is thrown away instead: that state has already been superseded, and
  offering it would be offering to undo a save.

  **Writing it waits while a take is running.** Not for the cost, but because a
  block still arriving has no file for a project to point at until the take
  stops and the WAV is named to it. What falls out of that is worth having: a
  recovery file can only ever refer to takes that finished, so it never points
  at a WAV whose header a crash left unpatched. The write comes due the moment
  the take stops.

  It is an ordinary project file with a longer name, so `audiaki --render
  song.aki.recover` reads it and renaming it to end in `.aki` makes it a session
  like any other.

- **Clip gain, and a Normalize that measures rather than guesses.** `ctrl+-` and
  `ctrl++` turn the selection down and up a decibel a press; `ctrl+N` measures
  it and puts it where you asked.

  It is a number on the clip, exactly as a fade is a length on one: applied on
  the way out by `aud_track_read()`, no sample touched, no audio copied, and an
  undo step that is a clip list like every other. The selection is what gets it
  rather than the lane — the clips at its edges are split first — so one bar out
  of the middle of a take can be brought up on its own, which the **gain**
  slider in the track's control column cannot do and was never meant to. The two
  multiply, because "this bar came in quiet" and "this guitar is too loud
  against the vocal" are different questions.

  The waveform follows it, the way it already follows a fade. A clip somebody
  has turned also says so in the corner, `+4.2 dB`, because a take recorded four
  decibels louder and one turned up four otherwise look exactly alike and only
  one of the two can be put back.

  **Normalize** is the same operation with the number measured. Two targets, and
  they answer different questions:

  ```
  ctrl+N          the loudest point to -1 dBTP, by the true peak
  ctrl+shift+N    the loudness to -18 LUFS, by ITU-R BS.1770
  ```

  Peak is about headroom and says nothing about how loud something sounds — a
  bass note and a cymbal normalized to the same peak are nowhere near equally
  loud. Loudness is the one that makes two takes sit together, and it is the
  measurement `--info` has reported all along, now pointed at a selection
  instead of a finished file. The peak target is the true peak rather than the
  sample peak for the reason it always was: the waveform between two samples
  goes higher than either, and an encoder reconstructing it clips where a sample
  peak reported headroom to spare.

  It measures through the same `aud_track_read()` the mix and the export use, so
  what is measured is what an export of that range would hold — fades, any gain
  already there, and the silence in the gaps included. Two things follow. It is
  idempotent: normalizing something already normalized computes a factor of one,
  because the gain already there was part of what it measured. And every
  selected lane is measured on its own and gets its own figure, since what makes
  two takes comparable is each of them reaching the target; one factor across
  all of them would be a master fader wearing this name.

  A lane with nothing to measure is left alone rather than multiplied by a
  guess. Silence has no peak to raise and a selection under 400 ms has no
  loudness — that is what BS.1770 says, not a shortcut here — and every lane is
  measured before any is changed, so a normalize that finds nothing measurable
  costs no undo step and says so on the status line rather than appearing to
  work. A loudness normalize is allowed to clip: bringing a quiet take up to a
  target can push its peaks past full scale, and this window has no limiter to
  hide that behind.

  Sessions carry it. The `clip` line gained a seventh field, and it is the one
  field on that line a project may leave off: every `.aki` written before this
  opens at unity, and one written after it opens in an older audiaki minus the
  gain rather than not at all.

- **Export to FLAC, Opus and MP3**, so a mix can leave audiaki as something
  other than a quarter-gigabyte WAV.

  The extension of the output name picks the format, in the window and on the
  command line alike:

  ```sh
  audiaki --render session.aki -o mix.wav    # PCM, written by audiaki itself
  audiaki --render session.aki -o mix.flac   # lossless, about half the size
  audiaki --render session.aki -o mix.opus   # lossy, the best of them per byte
  audiaki --render session.aki -o mix.mp3    # lossy, and everything opens it
  ```

  There is no option for it, deliberately. An option and a filename can
  disagree, and both ways of resolving that are bad — honour the option and you
  write FLAC into a file called `.wav`; honour the name and the option was a
  lie. One source of truth removes the question, `--visualize out.mp4` already
  worked this way, and the feature therefore costs no new CLI surface at all.
  `--stems` composes with it for free: the stem names put the base's own
  extension back on, so `-o song.flac --stems` is a set of FLACs.

  WAV keeps its own writer — it is the only one of the four that needs no
  ffmpeg, that can carry the metadata chunks, and that can pass 4 GB by becoming
  RF64. The other three go down a pipe to an `ffmpeg` child, the same way a
  video does and for the same reason: the codec licensing and the dependency
  list stay outside this program. `ffmpeg(1)` has to be on `PATH` for them and
  not for a WAV, and an export that needs it and cannot find it says so plainly
  and leaves nothing behind.

  What goes down that pipe is exactly the PCM the WAV writer would have been
  handed, at exactly the depth asked for, so the two paths cannot drift in how
  they round or clamp: a FLAC export is bit-identical to the WAV export of the
  same session, which the tests check by decoding one and comparing. A `--bits`
  that means nothing is refused rather than ignored — a lossy file holds no
  samples for a depth to describe, and FLAC holds 24 at the most. An extension
  audiaki does not write is refused before anything is opened, rather than
  handed to ffmpeg to make of what it will.

- **The mix metered in LUFS while it plays.** The right of the window's status
  bar reads `M -14.2   S -13.8   I -15.1 LUFS` whenever the transport is
  running: momentary (400 ms), short-term (3 s) and integrated since you pressed
  play, gated.

  It is the same ITU-R BS.1770 meter `--info` runs on a finished take, fed the
  mixed buffer in the playback loop, so the figure on screen is the one an export
  of that range would get. The metronome is not counted, for the same reason it
  is not recorded: it is something you hear, not something the project holds.

  The peak meter beside it answers a different question on a different scale —
  it is the input, in dBFS, and it says whether the recording fits rather than
  how loud the mix is. Having both is the point: `-14 LUFS` is roughly what the
  streaming services normalise to and `-23` is broadcast, and neither is a thing
  a dBFS peak can tell you.

  A meter wants the blocks that just went past where a report wants the loudest
  ones, so `aud_loudness_read_live()` is a second reading of the same history
  rather than a second meter. Failing to measure never stops playback: an
  unsupported rate, or no memory for the block history, costs the readout and
  nothing else.

- **Loudness and true peak in `--info`**, so two takes can be compared by how
  loud they are rather than by how large their samples are.

  ```
  peak:        -4.4 dBFS
  true peak:   -4.2 dBTP
  rms:         -10.7 dBFS
  loudness:    -18.4 LUFS
  range:       6.2 LU
  momentary:   -14.1 LUFS
  short-term:  -15.3 LUFS
  ```

  `loudness` is ITU-R BS.1770-4, the measurement EBU R 128 and every broadcaster
  and streaming service is written in terms of: two filters standing in for the
  ear, a mean square over overlapping 400 ms blocks, and a gate that throws away
  the silence between the notes so a take with long gaps in it is not rated
  quieter than the same playing without them. Neither peak nor RMS could answer
  that question — peak is headroom alone, and RMS weights 50 Hz the same as
  3 kHz, which the ear does not. `range` is how far the quiet parts sit below the
  loud ones, by EBU Tech 3342; `momentary` and `short-term` are the loudest
  400 ms and the loudest 3 s.

  `true peak` is where the waveform actually goes, which is not where the samples
  are. The signal between two samples routinely rises above both, so a take that
  reads -3.7 dBFS can be +0.2 dBTP and clip on the way out to MP3 or AAC while
  `peak` still reports headroom to spare. Four times oversampling, as BS.1770-4
  Annex 2 specifies.

  The coefficients are derived at the file's own rate rather than tabulated at
  48 kHz, so the same performance recorded at 44.1 reads the same. A take shorter
  than the window a figure is measured over does not get that figure rather than
  one measured over less — under 400 ms there is no loudness, under 3 s no range
  — and says which it was. Every channel counts at full weight, since a WAV does
  not carry the layout that would say which one is an LFE.

  The row `--info` prints for each of several files gains a `LUFS` column beside
  `PEAK`, because those answer different halves of "which take do I keep". With
  `--json` every report carries a `loudness` object and a `true_peak_dbtp`, with
  `null` where a take was too short or too quiet to measure.

  It rides on the pass that was already reading the file, so it costs a read
  rather than a second one.

- **Stems**, so a session can leave audiaki without becoming a mixdown.

  **Stems** in the window, `ctrl+shift+E`, or `audiaki --render session.aki
  --stems` writes one WAV a track instead of one mix. A mixdown is the end of
  the road: it is the one thing you cannot take back apart. A folder of WAVs
  that line up is the opposite — every other program can open it, and whoever
  gets it can still change their mind about the balance.

  `-o` names the set rather than a file. Each stem is that name without its
  extension, then the track's number and name, then `.wav`, so `-o song.wav`
  gives `song-01-Rhythm.wav`, `song-02-Lead.wav` and so on. The number is the
  lane's place in the session, so a gap in the numbering is a muted track rather
  than a miscount, and two tracks called the same thing are still two files. A
  name is reduced to letters, digits, dashes and underscores on the way into a
  filename — a track called `Gtr / DI` exports as `-02-Gtr-DI.wav` rather than
  into a folder called `Gtr`.

  They add back up to the mixdown, sample for sample. Every stem covers the same
  range at the same rate, depth and channel count, and carries the gain and pan
  that track sits in the mix with, so laying them side by side and starting them
  together gives exactly what Export would have written. That is not a promise
  kept by care: the mixer that adds every track together and the one that adds a
  single track reach the same code, and there is nowhere for a gain or a pan law
  to be applied differently to one than to the other.

  What the mix cannot hear is not written — a muted track, one silenced by
  another's solo, an empty lane — because a stem of silence is a file to notice
  and delete rather than information. A session where that leaves nothing at all
  says so instead of writing no files. The overwrite question is asked about the
  whole set before any of it is written, and a failure part way through takes
  back the stems already written: half a set looks like a whole one in a
  directory listing.

- **Audio can be moved along the timeline**, by dragging it or with `,` and `.`.

  Until now the only way to put a take somewhere else was to cut it and paste it
  back, which is three gestures for one intention and gets the position wrong
  unless the cursor was already exactly right. Select some audio and drag it:
  the pointer takes hold of the selection wherever it is inside one, and lets go
  of it where you let go. The keys do the same without the pointer, by one grid
  line at a time with the grid on and by a nudge without it.

  With the grid on it is the landing edge that snaps rather than the pointer,
  which is the difference between dragging a take *onto* the beat and dragging
  it a whole beat further on while it stays exactly as late as it was. `alt`
  steps off the grid here as it does everywhere else.

  Nothing moves until the button comes up. What follows the pointer is an
  outline of where it would land, so a drag across a session is one step of undo
  rather than one per drawn frame, and so a drag that turns out to have nowhere
  to go leaves nothing to put back.

  It stops against what is already on the lane rather than writing over it:
  clips do not overlap, and that invariant is what the whole editor is built on.
  A take dragged towards its neighbour slides up against it and stops, the
  outline turning amber to say so. The consequence is worth knowing rather than
  discovering — a selection with audio hard against both of its edges, a bar out
  of the middle of a take, cannot move at all, because the rest of the take is
  the thing in the way.

  A move across several selected lanes travels the same distance on all of them,
  and stops where the least roomy of them stops. An overdub that moved further
  than the take it was played against would come back out of time with it, which
  is the one thing this must not be able to do quietly.

  Like every other edit here it is clip surgery over shared blocks -
  `aud_track_move()` lifts the run of clips out of the list, offsets their
  starts and puts it back - so moving a forty minute take costs what moving a
  bar does, and the undo step it takes is a clip list rather than any audio.

- **`--calibrate` measures the round trip**, instead of telling you to go and
  measure it somewhere else.

  Everything that places a take against a grid corrects for the same number: the
  click is struck that far ahead of the beat, and the desktop app places an
  overdub that far earlier than Record was pressed. Both read it from
  `latency_ms`, and without one it is estimated from the two buffer sizes - a
  starting point and not a measurement, since the converters, the driver and the
  interface all add delay that no buffer size describes. The documentation has
  always said so, and has always followed it with an instruction to play a click
  into a loopback and look at where it lands in the file. That is the right
  instruction and a poor thing to ask anybody to carry out by hand in another
  program.

  Connect the output to the input and audiaki does it: a short sweep, five
  times, and the frames between each one going out and coming back. `-D` names
  the input and `--monitor-device` the output; the answer is reported with the
  spread of the readings beside it, and against what the buffers alone would
  have guessed, which is usually most of the point.

  It offers to write the number to the config file when there is a terminal to
  ask at, because `latency_ms` is a property of the interface and the machine
  rather than of today's session - measured once and true from then on.
  Everything else in the file survives exactly as it was, comments included, and
  an existing `latency_ms` is replaced rather than joined by a second one. A run
  with no terminal, or with `--no-prompt`, prints the line instead of writing
  anything.

  The measurement counts both directions on one clock - `aud_calibrate_step()`
  takes the captured period and fills the playback period in the same call - so
  what comes out is the correction the click already wanted rather than a
  separate fact about the hardware. It is a sweep rather than a click because
  the search is a correlation and a tone correlates with itself once per cycle,
  so a reading could land a whole cycle out with nothing looking wrong. The
  match is normalised, so a quiet return is as findable as a loud one, and taken
  on the absolute value, so a cable that returns it upside down is still a
  match. Five bursts rather than one because playback is fed from the capture
  loop and the two clocks are not the same crystal: the median carries it, a
  reading far from the rest is discarded rather than averaged in, and when too
  few agree the run says it could not tell rather than picking one.

  A run that produces nothing says which of the four things went wrong - nothing
  came back, something came back that was not the burst, the output could not
  play them, or the readings disagreed - because they want different responses
  and "calibration failed" wants none.

  All of the arithmetic is in `take/calibrate.c`, which carries no audio system,
  so it is unit tested against a loopback made of arithmetic rather than of
  cable: a run is fed its own output back, delayed by a number the test chose.

- **JACK and CoreAudio backends**, joining ALSA and PipeWire. `--backend jack`,
  `--backend coreaudio`, and both in `$AUDIAKI_BACKEND` and the desktop app's
  `-b`. Which ones a build has depends on where it was compiled - ALSA and
  PipeWire on Linux, CoreAudio on macOS, JACK on either where its headers were
  present - so `--help` now lists the backends you actually have rather than a
  fixed three, and `make help` reports them at build time.

  Under JACK a device is a client on the graph rather than a card, which is the
  thing worth knowing about it: `-D system` is the interface and `-D ardour` is
  whatever Ardour is putting out, so recording another program is a matter of
  naming it. Without `-D`, audiaki connects to the graph's physical capture
  ports. The ports it registers are ordinary JACK ports - `audiaki:in_1` and up
  - so anything that patches a graph can rewire a take while it is running.

  The server owns the rate and the period and does not resample. Asking for a
  rate it is not running at gets a warning and the server's rate, because the
  alternative is restarting `jackd` and that is not audiaki's to do. Playback of
  a take at another rate is converted on the way out instead, so `--play` and
  `-M` work regardless.

  CoreAudio is the macOS audio system and there the only one there is - both the
  driver layer and the mixer - so it shares devices the way PipeWire does while
  opening them as directly as ALSA does. `-D` takes a device UID, the stable
  string `--list` prints, or the device's name. The one surprise is that a rate
  belongs to the *device* on macOS rather than to a recording: `-r 48000` moves
  it for everything else using that device, which audiaki does rather than
  silently recording at whatever the last application left it at. The monitor
  output is left where it is, and a take at another rate is converted on the way
  out.

  Both are callback backends, like PipeWire, and both work in float. So both go
  through the same wait-free ring the desktop app already used between its
  capture and draw threads, and both encode into the capture format in the
  reader rather than in the real-time callback - which keeps a JACK process
  callback down to an interleave and a copy, where a mutex held a moment too
  long is an xrun in every other client on the machine.

  The build follows: `backend.c` now holds the four in one table, so parsing a
  name, reporting availability, choosing one and listing what a build has are
  four readings of one row. A backend that was not compiled in is a row with
  null ops, which is the only difference between "PipeWire is not installed
  here" and "ALSA does not exist on macOS" - and the second of those is new,
  because ALSA is no longer assumed to be present.

- The desktop app asks before four things, and deliberately not before anything
  else. A confirmation that appears constantly is one nobody reads, at which
  point the dangerous case is worse off than it was - so the rule is narrow: an
  action has to be large, or it has to lose something no undo will bring back.

  Large is an edit over ten seconds of audio, counted across every lane it
  touches rather than off the ruler - two seconds across six selected tracks is
  twelve seconds of audio, not two. An edit that would leave the project empty
  asks whatever its length, because emptying a session is a different act from
  taking a bit out of one and `ctrl+A` on a short session is still `ctrl+A`. So
  does closing a track, which takes every clip on the lane from a small button
  next to a name - exactly the shape of click somebody makes by accident.

  Lossy is the one worth having, because it is invisible without it: once you
  have undone a few steps, making any new edit silently discards everything you
  could have redone. The dialog names the number. Applying a spectral repair
  asks too, being the one operation that rewrites audio rather than moving clips
  over it, and it says that it writes a file.

  **Ctrl+Z is not guarded.** Undo is the way back out of a mistake and making it
  harder would be the wrong thing to do; the only undo that asks is undoing a
  repair, which says that the `cleaned-NNN.wav` stays on disk with nothing
  pointing at it.

  Closing the window with unsaved edits asks as well, and says where they go
  rather than only that they exist - a file you did not know was written is a
  file you will not go looking for. The window-manager close is handed to the
  app rather than acted on by the run loop, so the answer can be "not yet".

  The close button on a lane and Apply in the spectrum panel now ask for the
  action rather than carrying it out, so the one place that gates an edit is the
  one every route goes through.

- `--gain X`, and an **in** slider on the desktop app's status bar, for an input
  with no usable level control of its own - a line input, a cheap USB box, a
  card whose capture volume ALSA does not expose. A multiplier from 0.0 to 16.0,
  or `gain = 2.0` in the config file for an interface that is quiet by the same
  amount every time it is plugged in.

  Unlike `--monitor-gain`, this one reaches the file, and everything about how
  it is built follows from that. It goes on each period the instant it arrives,
  ahead of the meter, the spectrum, the pre-roll and monitoring, so all of them
  describe the recording rather than the device and a gain set too high reads as
  CLIP while there is still time to turn it down. What the meter says is what
  lands in the file. The slider sits beside the meter rather than up in the
  transport bar for the same reason - setting an input level means watching a
  needle.

  Samples pushed past full scale are held there rather than wrapped, which would
  turn a loud note into a burst of noise, and the count of them is reported when
  the take ends and names the gain as the cause rather than blaming the device.
  Rounded rather than truncated, and the rounding happens before the bounds are
  checked, so a sample that rounds up onto full scale is held there instead of
  wrapping to the opposite polarity.

  It is a last resort and the documentation says so. A gain stage ahead of a
  converter captures more of its range; this only scales the range already
  captured, so against a take brought up afterwards it buys nothing but the
  convenience - and it can clip a recording in a way nothing downstream will
  undo, where a quiet 24-bit take is merely quiet.

- A spectrum of what you recorded, in the desktop app's drawer beside the
  visualiser, that you can draw on to take noise out of a take - `N`, or the
  **Spectrum** arrow. Frequency across the bottom on a log scale, level up the
  side, and three traces: the loudest each frequency ever got, the average, and
  what you would be left with after the edit currently drawn.

  The graph *is* the edit. There is no list of filters kept in step with a
  picture of them - the line on screen is the gain curve, drawing on it writes
  to that curve, and **Apply** multiplies the audio by it. So the obvious
  gesture is the right one: a hum is a spike standing out of the floor around
  it, so you drag the spike down to the floor and it is gone. Right-drag puts a
  band back; the brush slider, or the wheel over the graph, sets how wide a
  stroke is.

  **Find hum** looks for a steady tone between 30 and 130 Hz, and **Notch**
  takes it *and its harmonics* out in one go. The harmonics are the point: mains
  hum is never the fundamental alone, and taking 50 Hz out leaves 100, 150 and
  200 Hz buzzing away. It scores candidates on the quietest each frequency ever
  got rather than the average, which is what lets it tell a hum from a note - a
  hum is in every window and cannot fall below itself, while a bass note comes
  and goes.

  For hiss, which has no spike to point at, there is a noise profile subtracted
  from every frame. **Learn** takes a stretch you have selected as being nothing
  but noise; **Guess** works it out from whatever is selected with no silent
  stretch needed, by taking the quietest each frequency ever fell to - which for
  a steady noise is the noise itself. Two sliders decide how hard it is
  subtracted and how far down it may pull, the second because driving a bin to
  nothing between two loud ones is what makes spectral noise reduction warble.

  Overlapping Hann windows, 4096 points at 75% overlap, resynthesised by
  overlap-add. The add divides by the window energy that actually landed on each
  output sample rather than by the constant it comes to in the middle, so a flat
  curve returns the input sample for sample including at the two ends rather
  than fading in and out of the edit. Bands are eased over a few bins at each
  edge, because a wall in the frequency domain is a ring in the time domain: a
  brick-wall notch under a bass note does not remove a hum, it removes a hum and
  adds a chirp after every string it was under.

  Applying is one press of `ctrl+Z` like any other edit. Unlike every other edit
  it really does rewrite audio, so it costs a moment and some memory on a long
  range - and it writes a `cleaned-NNN.wav` beside the takes as it goes, because
  a session records which parts of which files sit where and a block that came
  from nowhere could not be saved. An undo leaves that file rather than deleting
  it: redo still points at the block, and a file removed under it would turn
  redo into silence.

- Takes are no longer capped at 4 GB. A stamped take reserves 36 bytes for a
  `ds64` chunk, and becomes an RF64 file - the format EBU Tech 3306 defines and
  the ITU calls BW64 - if the payload ever needs it. Three and a half hours of
  24-bit stereo at 48 kHz was the old ceiling; there is no longer one worth
  quoting.

  Reserved as a `JUNK` chunk rather than promoted eagerly, so a take that stayed
  small is an ordinary RIFF/WAVE file with one chunk in it that every reader of
  the format already skips - and a take that outgrew it needs no audio moved to
  make room. The reader takes RF64 and BW64 as well as RIFF. Verified against
  ffmpeg, which reads both the small and the promoted file correctly.

  `--no-metadata` still stops at 4 GB. It asks for the plain 44-byte header, and
  a plain header has nowhere to put a 64-bit size.

- Sample rate conversion on the playback path, so an ALSA output that will not
  run at the stream's rate is converted to rather than refused. Monitoring a
  44.1 kHz capture through a 48 kHz output used to decline; now it plays.

  A windowed sinc rather than a linear interpolator, because the cut runs both
  ways: going down, an unfiltered converter folds everything above the new
  Nyquist back into the audible band as tones nobody played. The cutoff follows
  the lower of the two rates, which makes the same code correct in both
  directions. Nothing resamples what is written to a take - the file is the
  samples the device delivered, at the rate it delivered them.

- `--latency MS`, and the click is struck early by it. Hearing a beat costs an
  output buffer and what gets played in response costs an input buffer, so a
  take played perfectly in time used to land that whole round trip behind the
  grid the clicks were counted on. It is now struck a round trip early and
  arrives at the ears where the grid says it should be; the grid itself does not
  move. The same number, and the same arithmetic, that already placed an
  overdub - `latency_ms` in the config file sets both.

- The desktop app hands the save, import and export questions to the system file
  chooser when `zenity` or `kdialog` is installed, through **Browse...**. That
  is the desktop's real chooser, with its bookmarks, recent places and search,
  and under a sandbox it goes through xdg-desktop-portal.

  The chooser runs beside the window and is polled once a frame rather than
  waited on, so the window keeps drawing and the take keeps recording while
  somebody browses. What comes back lands in the dialog's two fields rather than
  acting on its own, so it can still be corrected before either button is
  pressed. The built-in browser stays as the fallback, and
  `AUDIAKI_FILE_CHOOSER=none` keeps it deliberately.

- `--click-subdiv N` divides the beat: `2` for eighths, `3` for triplets, `4`
  for sixteenths, up to 8. The ticks are the beat tone struck softer, so the
  pulse still reads as the pulse. What a slow tempo usually wants - at 60 BPM
  there is a second of nothing between beats to drift about in.

  A take recorded to a click now says so. The tempo, the bar length and the
  subdivision go into the take's `LIST`/`INFO` chunk under `ITMP`, and `--info`
  prints them back as a `metronome:` line. The click is heard and never
  recorded, which is exactly why the file has to carry the number: there is
  nothing in the audio to work it out from afterwards. `ITMP` is audiaki's own -
  RIFF/INFO has no registered tag for a tempo, and an unknown four-character id
  is the one thing every reader of the format has to step over. An `acid` chunk
  would have been the standard-ish alternative and also tells a DAW the file may
  be stretched to the project's tempo, which is the last thing a raw take should
  say about itself.

- `--channel mix` averages every capture channel into one instead of dropping
  all but one. For two inputs that both have something on them - a pair of room
  mics, an instrument and a vocal - where `--channel 1` loses half the take.
  Averaged rather than summed, so a mixdown cannot clip; that costs up to 3 dB
  against a summed mix on material that is the same in every channel, which is
  the right way round for a capture path where a clipped take cannot be undone.

- `--shuffle`, `--repeat` and `--repeat-one` for `--play`. Shuffling picks a
  fresh order each time through a repeating list rather than cycling one
  permutation forever, and `n`/`p` still walk the list under any of them. A pass
  over a playlist where every file failed to open stops rather than repeating
  itself: an unreadable list would otherwise spin.

- `--tune-min` and `--tune-max` set the range the tuner searches, and the
  defaults moved: 30 Hz to 4500 Hz, from a five-string bass's low B (30.87 Hz)
  up past a piccolo's top C (4186 Hz), where it used to be 40 Hz to 2 kHz and
  reached neither.

  The two ends do not cost the same, which is why only one of them moved far on
  its own merits. The ceiling sets the shortest lag searched, so raising it is
  nearly free. The floor sets the longest and the analysis is quadratic in it -
  measured at 48 kHz, one reading takes about 3.5 ms with the floor at 40 Hz and
  about 7.6 ms at 27.5 - so the default stops at the lowest note anyone actually
  brings to a tuner rather than going down to a piano's A0 for the sake of it.

- The desktop app's grid divides. `shift+G` cycles bars, beats, halves, thirds
  and quarters of a beat; the ruler and the track lines draw whichever of those
  there is room for, thinning from subdivisions to beats to doubled bars as you
  zoom away, and every line drawn is somewhere an edit can land. The division is
  saved with the session, and a project written before it existed opens on
  beats.

  The arrow keys snap too. `←` and `→` step from one grid line to the next
  rather than by a fixed number of pixels, which is the only way a keyboard can
  put an edit where the pointer would; `alt` drops the keys off the grid the
  same way it already dropped the pointer.

- `--play` has a transport, and takes more than one file. Space pauses, the
  cursor keys seek - five seconds sideways, thirty up and down - `home` and
  `end` jump to either end of the file, `n` and `p` move through the files
  given, and `q` stops. `audiaki --play session-*.wav` is a playlist; `-t`
  applies to each file rather than to the run, and a file that will not read is
  stepped over the way `--info` steps over one.

  None of it is required and nothing changes without a terminal on the other
  end. Over a pipe, from a script or under a service manager there is nowhere to
  read keys from, and each file plays from beginning to end exactly as before.
  Ctrl+C still stops everything - the terminal is switched out of canonical
  mode, not out of its signals.

  Pausing and seeking act on what is being handed to the output rather than on
  what is coming out of it, so both land within a buffer - about a tenth of a
  second - of where they were asked for. Skipping deliberately does not wait for
  what is queued.

  `wav_read_seek()` is new alongside it, and the reader now keeps the offset its
  data chunk started at: a take carrying its metadata stamp does not begin at
  byte 44, and a seek that assumed it did would decode the stamp as audio.

- The window's save dialog can play the take back before you decide where it
  goes. **Play** reads it straight off the disk at its own rate and channel
  count, with nothing put on the timeline and no undo step spent, and follows a
  row once one is clicked - so it hears the take being filed, or the take about
  to be reported as in the way. **Import** and **Export** get the same button.

- That dialog now lists the files already in a folder as well as its
  sub-folders, with the row the name field points at drawn as the current one.
  A folder that looked empty was a folder anybody would file `take01.wav` into
  twice and be refused afterwards over a file that had never been on screen.
  **Hidden** lists dot files and dot folders, off by default, for the folders
  that were previously reachable only by typing the path.

- A take the capture device is pulled out of carries on when it comes back.
  What was played is safe either way - the WAV is closed and patched the instant
  the stream dies, and the clip that was growing on the timeline becomes a
  finished one over it - and if the same device returns within thirty seconds
  the take continues on the same lane, starting at the frame the first half
  stopped at, in a take file of its own. A device that comes back at another
  rate or channel count, or after the thirty seconds, reopens the stream and
  waits for **Record** as it did before.

- A tempo, and the three things that read it. A session now counts on one -
  120 to the bar of four until told otherwise - and it is saved with the
  session, so two people opening the same project see the same bar lines.

  **Grid** (`G`) counts the ruler in bars instead of minutes and seconds, draws
  the bar lines down the tracks, and puts the pointer on them: clicking,
  scrubbing and dragging a selection land on the nearest beat, with `alt` to
  step off for the cut that has to go between two. **Click** (`C`) plays a
  metronome at that tempo - mixed into the output on its way to your
  headphones and nowhere near the file, so a take made to one carries no click
  in it. It runs under playback, under an overdub, and on its own over an empty
  timeline, which is what a count-in is. `-` and `+` move the tempo a beat at a
  time, while something is playing if that is how you want to find it.

  The ruler and the metronome count one grid from one number, both computed
  from the frame index rather than accumulated, so the click lands on the line
  however long the session runs. Playing to a click costs the same round trip
  playing to a backing track does, so recording to one places the take by the
  same correction - `--latency` and all.

  `--tempo`, `--click`, `--click-beats`, `--click-gain` and `--grid` set it up
  from the command line. A `.aki` file gains a `tempo` line, which older
  audiakis step over rather than refuse.

- **Loop** (`L`), which plays the selection round and round rather than
  stopping at the end of it. Select a bar and it repeats that bar, which is
  what learning one is; select nothing and it repeats from the cursor. It can
  be turned on and off without stopping what is playing. Recording never loops.
  `--loop` comes up with it on.

- The window can be rebuilt while it is running. `make HOTRELOAD=1 gui` builds
  it as a shell plus a library it loads, and F5 loads the library again: the
  device stays open, the timeline stays as it is, and a take that is recording
  keeps recording while the code drawing it is replaced underneath. After
  tsoding's musializer, which is also where the visualiser came from.

  A development build only — `make` and `make install` are unchanged, and ship
  one binary with no `dlopen` in it. A build that does not compile costs a line
  on the terminal rather than the session, and a change to the shape of the
  state itself is refused with a message saying to restart rather than read the
  session through the wrong map of it. `src/gui/main.c` is now the shell and
  the app's own lifecycle has moved to `src/gui/plug.c`; see
  [DESIGN.md](DESIGN.md#reloading-the-window).

- Sessions. What was done to a set of takes - the cuts, the levels, which clip
  sits where - is now something that can be written down and opened again.
  `ctrl+S` in the window saves it to a `.aki` file, `ctrl+O` opens one, and
  `audiaki-gui session.aki` opens one at startup.

  A session refers to its takes rather than containing them, so it is a few
  kilobytes of readable text whatever it holds, and it can be opened in an
  editor and fixed by hand. Takes kept beside it are referred to relatively, so
  a session folder can be copied to another disk and still open; a take that
  has moved is named rather than opening as a silent lane. A load that fails
  leaves the timeline it was going to replace alone.

  Closing the window with unsaved edits writes them out rather than dropping
  them - back to the session file, or to `recovered.aki` beside the takes.

- `audiaki --render session.aki -o mix.wav` mixes a session down with no window
  involved, at whatever `--bits` asks for. It opens no device, so it runs over
  ssh, in a build, or on a machine with no sound server - which is what makes a
  session something a script can use.

- Fades. `[` and `]`, or the two new buttons, ramp the selection up out of
  silence or down into it, so a cut across a note stops clicking. A fade is a
  length on the clip rather than something written into the audio, so it costs
  nothing, undoes like every other edit, and the waveform is drawn through it.

  No crossfades: clips on a lane do not overlap, which is the invariant the
  editor is built on, and a crossfade needs two pieces of audio sounding at
  once.

- Overdubbing. Pressing Record with something already on the timeline plays it
  while the take is recorded over it. On by default; **Overdub**, or
  `--no-overdub`, turns it off.

  Playing along to something means hearing it late - the output holds a buffer,
  then the input holds another - so the take is placed a round trip earlier than
  Record was pressed, because that is when the sound was made. The correction is
  estimated from the two buffers, which is a starting point and not a
  measurement; `--latency MS` and `latency_ms` in the config file are where a
  measured one goes. Jitter is not corrected for: playback is fed from the
  drawing loop and capture runs on its own thread, so the two start within a
  drawn frame of each other rather than on the same sample.

- `audiaki-gui` is a multi-track recorder and editor. It records onto a
  timeline, draws the waveform while you are playing rather than when you have
  stopped, plays the result back, cuts it about, and writes a mix out again.
  What was a window over one take is now a window over a session of them.

  Recording lands at the cursor: on the selected track if it is free from there
  on, and on a new one otherwise. So clicking further along a take and pressing
  record again adds to that take, and clicking over audio that is already there
  gives the new one a lane of its own rather than refusing or overwriting.

  The take goes onto the timeline as it arrives, frame for frame with the WAV
  being written - not a preview of it - and it is editable the moment it stops,
  with nothing in between. The visualiser that used to be the whole window is a
  panel you can shut.

- Editing: select by dragging across a track, `ctrl`+click to take in another,
  then cut, copy, paste, delete, silence, trim, split or copy the selection onto
  a track of its own. 64 steps of undo, and pasting over a selection replaces
  it the way typing over selected text does.

  None of it copies audio. A clip is a window onto a block of samples that is
  never written to, so an edit moves a handful of structs however long the take
  is - which is also what makes whole-project undo affordable. DESIGN.md has
  the reasoning.

- Playback: `space` plays the selection, or from the cursor to the end, with a
  playhead the view follows. Mute, solo, gain and pan are what they say, and
  what you hear is exactly what Export writes, because both go through one
  mixer.

- Import and Export. `I` opens a WAV as a track; `ctrl+E` mixes down to one -
  the selection if there is one, the whole project if not, 24-bit at the
  project's own rate. Files named on the command line are opened as tracks, so
  `audiaki-gui take-*.wav` opens a session.

- The arrow keys, which the window did not answer to at all. `left` and `right`
  move the cursor by a few pixels' worth of time, so a nudge is the same gesture
  whatever the zoom - coarse over a whole session, sample-accurate over one
  transient. `ctrl` steps clip edge to clip edge instead, which is where a trim
  wants to land; `home` and `end` go to either end of the project.

  `shift` moves the far end of the selection rather than the cursor, growing it
  from the end it started at rather than always from the left, and `up`/`down`
  walk the track selection up and down the stack and scroll it into view. The
  editor is drivable from the keyboard alone now.

- A two-level peak index behind each block of audio - minimum, maximum and RMS
  per 256 frames, and per 256 of those - so a waveform zoomed out to a whole
  session reads a few thousand buckets rather than a hundred million samples,
  and one being recorded is indexed as it arrives.

- **The three parsers that read files audiaki did not write are fuzzed.** A
  project saved by an older version, somebody else's WAV, a take a crash left
  half way through — all three are walked by reading a number out of the file
  and reaching that far into it, which is the shape every buffer overrun is
  written in.

  ```sh
  make fuzz-replay   # every corpus entry, sanitized, with any compiler
  make fuzz-run      # go looking; needs clang for libFuzzer
  ```

  Each target builds twice, and that is the point. `fuzz-run` finds new inputs
  and needs clang; `fuzz-replay` runs the ones already found and does not, so it
  is part of `make check` and of CI on every change. An input that crashed once
  and was fixed is a test from then on rather than something rediscovered by
  luck. Seeds are committed for each parser: the shapes it must accept, and the
  ones it must refuse without reading past the end of anything.

  Nothing was found. 24,000 mutated inputs through the WAV reader, the metadata
  chunk readers and the project loader under AddressSanitizer and UBSan produced
  no crash, no overrun and no leak — which is a result worth recording, since it
  is the answer the corpus now protects.

- **A project has to save to what it was loaded from.** The property nothing was
  checking, and the one the format actually needs: a session that opens is not
  enough if it opens as something slightly different, because then every round
  trip moves it further from what was recorded. Checked byte for byte, and over
  four passes — a field comparison only covers the fields somebody thought to
  list, and the ones nobody listed are where this goes wrong.

- **The option parser has tests**, which found the misleading `--latency`
  message below. `cli.c` is nine hundred lines of option table and
  cross-command checks, and until now the only thing exercising any of it was
  three smoke assertions in CI — while every rule in it exists because accepting
  the invocation would have done something quietly other than what was asked.

  28 tests over what is accepted, what is refused, and *why* it is refused: an
  invocation rejected for the wrong reason sends somebody looking in the wrong
  place. `cli.h` had always kept an audio system out of the option handling, so
  the only thing needed to test it was the backend name table, which the test
  links directly and supplies the ops for.

### Fixed

- **The unit tests build on macOS again.** `make test` there linked the option
  parser's own tests without the option parser in them and stopped at an
  undefined `cli_parse`.

  Two pattern rules matched `build/tests/cli/test_cli`: the general one that
  builds every test against the portable layers, and the one that adds
  `src/cli` and the backend name table for this suite alone. Which of two
  matching pattern rules a make picks is not something every make agrees on.
  The 3.81 that ships with macOS takes the first one written; every make since
  3.82 takes the one with the shortest stem, which is why Linux and the
  sanitizer jobs never saw it.

  The rules that need to win now name their targets. A static pattern rule is
  an explicit rule for the targets it lists, and an explicit rule beats a
  pattern in every version of make there is - so which one runs is no longer a
  tie-break at all. The window's objects had the same collision waiting, and
  are named the same way: a macOS build of the desktop app would have compiled
  them without raylib's include path.

- **A session saves even when a name has a line break in it**, instead of
  writing a project file that will not open again.

  A project is line-based text, and each value gets the rest of its line. Both a
  track name and a source path were written straight into one, so either could
  end the line early and have what followed read back as the next setting: a
  take called `guitar\nchannels 7\ngain 0.01` saved as a project whose second
  track was seven channels wide at a hundredth of its gain — or, more often,
  simply refused to reopen at all. A filename may hold a newline, so this needed
  no ill intent to reach, only an awkward file.

  A name is a label, so it is trimmed to one line and the save goes ahead: the
  lane comes back called `guitar channels 7 gain 0.01`, and the audio and every
  edit on it come back exactly. A source path is not a label — a trimmed one
  names a different file or none — so a take whose filename holds a line break
  is named and the save refused, before anything is written, rather than the
  project opening later having quietly lost it.

- **`--bits` and `--stems` are refused wherever they do not apply**, rather than
  only for some of the commands they do not apply to.

  Both belong to `--render` alone, and the parser said so — but the check sat
  below the per-command handling, and `--info`, `--play`, `--tune` and
  `--calibrate` all return before reaching it. So `audiaki --bits 16 take.wav`
  was rejected while `audiaki --info take.wav --bits 16` was accepted in
  silence, having ignored the option entirely. The two checks have moved up
  with the other cross-command ones, where every command reaches them.

- **An export is refused a channel count the mixer cannot pan**, instead of
  writing the header anyway. The depth was already checked this way; the width
  beside it was documented as mono or stereo and enforced nowhere, so a caller
  asking for six got a six-channel file the mix had no pan law for. No
  in-tree caller asked for one — this closes the gap rather than fixing a
  broken export.

- **The toolbars fit the window they are in**, instead of only fitting a
  full-screen one. The desktop app opens at 1100 by 680 and the two rows of
  buttons across the top of it were laid out in equal slots, which is a way of
  dividing a row that ignores what is written in it: "Cut" was handed the same
  width as "Fade out", and the slots were narrower than the longest labels well
  before the window was, so the edit bar ran its words into each other -
  "PasteDeleteSilence" - at the size it starts at. Widening the window to the
  whole screen was the only way to read it.

  Both bars are now measured from their labels. Each button is as wide as what
  it says plus its padding, and the size the row is lettered at is the largest
  at which the whole bar - the edits, the tempo cluster and the zoom controls,
  or the transport and the capture options - fits between the window's edges.
  One size for both rows, so they match. A button never draws a label wider than
  itself either: it steps the size down until it fits and cuts the end off with
  an ellipsis only when there is nothing left to give, which is what keeps the
  timeline's lane buttons and the dialogs honest as well.

  Widths come from the longest label a button ever carries - Play becomes
  Playing, Save grows a star - so nothing shifts out from under the pointer when
  the state changes.

- Overdub, Video, Audio, the monitoring switch and its level are on the
  transport bar at the size the window opens at. The bar had no room for them
  once its equal slots had been handed out, and rather than overlap the
  transport it drew none of them - so five controls were missing from a fresh
  window until it was made wider, with nothing to say they existed.

- A take the capture device was pulled out of now carries on in the *same*
  file, and the same clip on the lane, rather than in a second one beside it.

  Nothing already written is rewritten to do it: the frames go on the end of the
  ones that are there and the header is patched when the take finally stops, so
  a crash part way through the second half leaves the file exactly as long as it
  was after the first - the same amount lost as a second file that never got
  created. The first half is never put at risk to save the second, which was the
  objection to splicing in the first place.

  One clip rather than two touching ones, and that part is not cosmetic: a
  project stores a clip as an offset into the file it came from, and two clips
  both stamped with the one file would both claim to start at the beginning of
  it - so reloading the project would have played the first half twice.

  A file that will not take the rest - one something else has appended to, or a
  device back at a different rate - falls back to the second take on the same
  lane, which is what this always used to do.

- Seeking and pausing in `--play` land where they were asked for. Both used to
  act on what had been handed to the output while a buffer's worth - around a
  tenth of a second - was still on its way to the speaker, so a jump was heard
  late and a pause was followed by more audio. The queue is now dropped along
  with the jump.

- Monitoring follows `--channel`. A take being written as one channel is now
  monitored as one channel, rather than the headphones carrying every channel
  the device delivered while the file kept one of them.

- Host byte order no longer has to be little-endian. WAV and every format a
  backend delivers are little-endian wherever they came from, and the decoders
  were reading them with a `memcpy` into a native integer, which is only correct
  because most hosts happen to agree. `src/util/bytes.h` reads and writes the
  bytes one at a time instead; `media/wav.c`, `audio/format.c` and the RGBA
  frames going down the `ffmpeg` pipe all go through it.

  There is no conditional compilation in the audio paths on purpose - a compiler
  folds a byte-at-a-time little-endian load back into the single instruction the
  `memcpy` was, so the portable version costs nothing and there is only one path
  to test. The framebuffer is the exception, where the per-pixel cost is real
  enough to keep a whole-buffer write on hosts that are already in that order.

  Verified by construction rather than on the hardware: `tests/util/test_bytes.c`
  states what a given array of bytes means, and nothing in the helpers looks at
  a host integer's layout. Actually running it on a big-endian machine still
  wants one, or an emulator, that CI does not have.

- Metadata appended after the audio is read. The chunk walk stopped at the
  `data` chunk, so a file an editor had retagged in place - which usually means
  appending - reported having no metadata at all. It now walks on past the
  payload when it has not found any yet, seeking over the audio rather than
  reading it, and stops early as before when the tags were where audiaki puts
  them.

- A heap use-after-free in **Copy to**: the new track's name was read out of the
  track list through a pointer the same call had just reallocated. Reachable
  whenever the number of tracks crossed a capacity boundary.

- A heap use-after-free when a lane was edited while a take was recording onto
  it. The index of the clip being recorded into did not move with the clip list,
  so a split or a delete on that lane sent the next captured period through a
  stale index - and off the end of the list once it had shrunk past.

- A heap buffer overflow in the desktop app on interfaces wider than sixteen
  channels. The take drain buffer was sized for sixteen but asked for a fixed
  number of frames, so `-c 32` wrote a megabyte past the end of it. The frame
  count is now derived from what the device negotiated.

- The take ring could be left holding part of a frame when it filled, which put
  every frame drawn after that one a channel out of step. It now takes whole
  frames or none.

- A capture-thread hang: an ALSA playback write that accepted nothing was
  neither an error nor progress, and the loop went round forever with the take
  behind it.

- A PipeWire capture stream that renegotiated its format after opening could
  widen the frame while the buffers above it stayed the size the first agreement
  set. A changed geometry now ends the stream, the way an unplugged device does.

- Panning a track no longer quietly turns it down in a mono mix. `track.h` said
  pan was ignored on a mono output; the mixer applied the left leg of it anyway.

- A take no longer starts before the line it was asked to start on. The overdub
  latency correction was applied whenever the project had any audio in it at
  all, rather than when something was actually being played along to - so
  recording from past the end of the existing material, where playback has
  nothing to play and never starts, still shifted the take back by the whole
  estimated round trip. Placement now follows whether playback started.

- That estimate was also about four times too large. It took the capture side's
  whole ring as the delay, when the capture thread drains it continuously one
  period at a time and so is a period behind, not a buffer. On the defaults it
  came to 128 ms where about 58 ms is the honest guess.

- The playhead was never drawn. The timeline takes a playhead position and
  whether anything is running, and the window was handing it the cursor and a
  hard-coded zero - so playback scrolled the view along with nothing on screen
  to say where it had got to. It is now told where playback or the take actually
  is, which is a different place from the cursor.

- **A project file with a clip on no track opened as an empty session, and said
  it had worked.** Every line describing a track was stepped over when there was
  no track open — reasonable for a setting, wrong for a clip, which is audio.
  A file whose `track` lines were damaged opened as a session quietly missing
  takes; a file of nothing but clips opened as nothing at all, successfully, over
  whatever was on the timeline. It is now refused, the way every other piece of
  audio this loader cannot place already was.

- **`--latency` was refused with a message naming three other options.** It
  shapes the metronome — it is the round trip the click is struck ahead of the
  beat by — so `--latency` without `--click` is correctly rejected, but the
  message listed `--click-beats`, `--click-subdiv` and `--click-gain` and not
  the option that had actually been typed.

### Changed

- **The window's shortcuts have tests.** They previously could not: raylib was
  read and acted on in the same breath, so `IsKeyPressed(KEY_SPACE)` and "start
  a take" were one expression and there was no seam to put a test through. That
  left the rules that decide what a key means — which dialog has the keyboard,
  what a modifier turns a key into, whether `space` plays or stops a take —
  checkable only by sitting in front of the window, and every one of them fails
  silently when it is wrong.

  Reading the keyboard is now separate from deciding what it asked for.
  `app_input_read()` takes a copy of the frame's keys and is the only function
  in `src/gui/keys.c` that knows raylib exists; `app_cmd_map()` turns that copy
  plus the state of the app into the list of commands the frame should carry
  out, and calls nothing, draws nothing and changes nothing. `actions.c` carries
  them out. 20 new tests hand the mapping a keyboard and read back what the
  window would have done.

  No shortcut changed. The commands come out in the order the old if-chain ran
  them in, which with two keys down in one frame is the behaviour.

- **`src/gui/plug.c` split four ways**, from 2,404 lines holding the engine's
  lifecycle, the take's, every edit action, the transport, the export, the whole
  keyboard and the hot-reload ABI:

  ```
  plug.c     the app's own lifecycle: start, frame, and the way out
  take.c     the capture device, and the take being written to it
  actions.c  what the toolbar, the keys and the timeline all mean
  keys.c     which of those a frame of the keyboard was asking for
  ```

  The hot-reload boundary is unchanged — the same four entry points, in the same
  file — and so is everything the window does.

- In the window, `space` now plays and `R` or `ctrl+space` records. Playing back
  is the commoner of the two once there is a timeline to play, so it gets the
  bare key; `F` fits the project to the window, the way it does in every editor
  of this kind, and fullscreen has moved to `F11`.

- The window does not ask where to keep a take unless it is told to. The take is
  on the timeline the moment it stops, and a dialog between playing something
  and editing it is a dialog in the way. `prompt = yes` in the config file
  brings it back; the terminal recorder is unchanged, because there a take is a
  file and where it goes is the whole question.

- A take gets asked where it should live. The recorder offers a folder and a
  name once the file is closed - Enter twice keeps it exactly where it is - and
  the window opens a dialog with the same two questions, the folder it was
  written in, and the sub-folders of that folder to click through. Naming a take
  is a job for after it has been played, not for the moment between the count-in
  and the first note, so nothing is asked before the recording and the file is
  complete on disk before the first question appears. Every way out that is not
  an answer leaves the take where it was recorded: Ctrl+C, end of input, **Keep
  here**, `Esc`, and any failure. A name that is already taken is asked again
  rather than written over.

  In the terminal this only happens when there is a terminal to ask at - both
  standard input and standard error, and not `--quiet` - so a pipeline never
  stops for a question nobody is there to answer. `--prompt` asks anyway and
  `--no-prompt` never does; the window has `--no-dialog`.

- A config file, `~/.config/audiaki/config`, for the two answers that are the
  same every session: `take_dir`, the folder a take named without one is written
  into, and `prompt`, whether to ask about it afterwards. Both binaries read it,
  everything in it is a default the command line still overrides, and a file
  that is not there is the behaviour audiaki always had. `--dir` sets the folder
  for one invocation.

  The folder applies before the first frame is captured rather than by moving
  the take afterwards, so a long session is not copied twice, and it is created
  if it is not there. A name that already says where it goes - anything with a
  slash in it, or an absolute path - is left alone.

- The desktop app explains itself. `?` - or the button now beside the device
  picker - puts every key it answers to over the window, and `Esc` takes it
  away. The keys were real and documented and completely invisible, which for
  anyone who never opened the manual meant the window had no keyboard at all.

- `1` to `6` select a visualiser style outright, rather than pressing `V` until
  the right one comes round. Six styles is one too many to cycle through when
  you know which one you want.

- Hover help on every control, disabled ones included. A greyed-out **Pause**
  is a question - the pointer resting on it now answers "nothing to pause - no
  take is open" rather than leaving it to be worked out. The device picker says
  why it will not open mid-take, **Audio** says to turn **Video** on first, and
  the ones that are simply doing their job name their shortcut.

- The monitoring level reads out in decibels above the slider. A knob two
  thirds along is not a level you can set deliberately or come back to
  tomorrow, and the range it sits on - silent to +6 dB - was only in the manual.

- The window title carries the transport: `audiaki - recording 00:12 -
  take-003.wav`. A recorder behind a browser should be able to answer "is it
  still running?" from the taskbar rather than by being raised. Whole seconds
  rather than the status line's tenths, because the title is a window property
  and the display server does not need ten of them a second.

### Fixed

- Dragging the monitor slider no longer lets go when the pointer wanders off
  the track. It follows the mouse until the button is released, wherever that
  ends up, which is what every other slider does; stopping dead a few pixels
  above the control read as the app losing the drag rather than as precision.
  The wheel nudges it too, for a correction a drag is a clumsy way to make.

- `--channel N` records a single capture channel as a mono take. Plenty of
  interfaces only offer stereo, so an instrument in the first input costs a file
  that is half silence at twice the size, and every tool after it has to be told
  which side the music is on. The device is still opened with `--channels`
  channels; this decides which one reaches the file, and the result is a real
  mono WAV rather than a stereo one with a dead track.

- The meter and the spectrum follow the picked channel, so the level being set
  while recording - or while armed under `--preroll` - is the level that lands
  in the file. Metering the pair would report clipping on a channel that is not
  being written, which is a wrong answer rather than no answer. Monitoring is
  deliberately left alone: `-M` still plays every channel the device delivered,
  because the output was opened to match the device and what is heard is a
  convenience, not the product.

- Channels are numbered from 1, matching what `--info` prints per channel, so
  the report that shows one input is silent names the channel to keep. Asking
  for one the device did not negotiate is an error rather than a silent
  fallback, checked against the count the device actually settled on.

- A metronome. `audiaki -M --click 120 take01.wav` plays a click at 120 BPM
  through the same output monitoring uses, so a take can be played in tempo
  rather than measured for it afterwards. `--click-beats` sets the bar - four by
  default, `3` for a waltz, `0` for a bare pulse - and accents the first beat an
  octave up; `--click-gain` sets how loud it sits against the instrument,
  independent of `--monitor-gain`. With `--preroll` the click starts as soon as
  the recorder is armed, so the wait before Enter is a count-in.

- The click is never written to the take. It reaches the headphones and stops
  there, on the same rule `--monitor-gain` follows: the file is made of the
  samples the device delivered, whatever was being played to the person making
  it. Use headphones - from a speaker the click is in the room, and the input
  will record it after all.

- Asking for a click opens the output on its own, so a metronome does not
  require hearing yourself as well; `-M` adds the input to it. One output
  carries both, because two streams would mean two clocks and a click that
  drifts against the monitoring beside it.

- `click` module: the beat grid as a pure function of the absolute frame index,
  with unit tests. Beat *n* lands where `n * 60 * rate / bpm` says it does
  however the periods arrived, so rounding cannot accumulate and a dropped
  buffer costs one click rather than moving every click after it. The frames
  counted are the frames captured, which makes the tempo the capture clock: at
  120 BPM and 48 kHz the beats are generated at frames 0, 24000, 48000 and so
  on, exactly.

  What that does not do is align a take to a timeline. The click reaches the
  player through the output's buffer, so a performance played perfectly in time
  is that far behind the grid it was played to. It keeps the tempo; it is not a
  DAW's click, and [docs/USAGE.md](docs/USAGE.md#playing-to-a-click) says so.

- Takes that say what they are. Every recording is now stamped with the software
  that made it, the date and time it started, the capture device it came from
  and the signal chain, and `--note "second chorus, clean tone"` adds whatever
  else is worth remembering. `--info` prints it all back. A take that has been
  copied off the machine used to be a filename and a modification date, and both
  are lost the first time someone moves it.
- Two standard chunks rather than one invention: a `LIST`/`INFO` block, which is
  what taggers and most players already read, and `bext`, the Broadcast Wave
  extension every field recorder writes. Other tools pick them up without being
  told - `ffprobe` reports the comment, the date and the coding history. `bext`
  also carries a time reference, the samples between local midnight and the
  first frame, which is what lets two takes from one session line up on a
  timeline with no timecode involved.
- `meta` module: building both chunks and reading them back, with the clock kept
  out of the builder so the layout can be unit tested against fixed values
  rather than against itself. The chunks go **before** the audio - where BWF
  requires `bext`, and where they are already on disk if a recording is
  interrupted - which is why the writer now patches its two size fields
  separately instead of rewriting one 44 byte header.
- `--no-metadata` for the plain 44 byte header, unchanged from what audiaki
  wrote before, for anything that wants nothing between `fmt` and `data`. The
  desktop app stamps its takes the same way, without a note: there is nowhere in
  the window to type one.

- `--info` over more than one file. `audiaki --info session-*.wav` reports a row
  per take instead of a page per take, which is the shape the question actually
  has at the end of a session - which of these do I keep, and which one clipped:

  ```
  FILE                DURATION     PEAK      RMS   CLIPPED
  session-001.wav        41.20     -4.4    -10.7         0
  session-002.wav      1:12.34     -0.0     -6.1      1820  CLIP
  ```

  A file that cannot be read is reported and stepped over rather than ending the
  run, since one bad take hiding the state of the eleven after it is the
  opposite of what measuring a session is for; the exit status is still
  non-zero. With `--json` the same run writes an array of the objects `--info`
  already wrote for one file, and a single file still writes the single object.

- Hearing the take while it is recorded. `audiaki -M take01.wav` plays the
  capture stream through an output as it is written, so the level, the tone and
  the room can be checked while playing rather than afterwards. The desktop app
  has monitored since it had a transport; the CLI opened the same stream only to
  play finished files back. `--monitor-device` names the output and
  `--monitor-gain` scales what is heard, both switching monitoring on by
  themselves. Audible while `--preroll` is armed too, which is when the level is
  usually being set.
- Monitoring never costs a take. An output that will not open, or one that fails
  part way through, is reported and dropped, and the recording carries on: what
  is going to disk is the point, and what is coming out of the headphones is
  not. Frames the output cannot keep up with are dropped rather than queued, so
  the monitor may skip on a busy machine without any of that reaching the file.
  `--monitor-gain` is the monitoring level alone - the file is written from the
  samples the device delivered, so a quiet monitor is not a quiet take.
- A feedback warning, printed before recording starts. The default capture on a
  laptop is the built-in microphone and the default output is the speaker beside
  it, which is a loop that reaches full scale in a fraction of a second. So is
  capturing an output's monitor source and playing it back through that same
  output, which headphones do not save you from.

- Playing a take back. `audiaki --play take01.wav` sends it to an output and
  draws the recording meter against the file's length, so the take `--info`
  has just measured can be listened to without leaving the shell for a media
  player. `-t` plays the first part only, `--spectrum` and `--no-meter` behave
  as they do when recording, and Ctrl+C stops at once. It plays whatever the
  WAV reader accepts, not only audiaki's own takes.
- `play` module: the WAV reader, the playback stream and the meter wired
  together. No new audio system code - the two monitor backends already turned
  interleaved floats into sound - but two new operations on them:
  `aud_monitor_space()` and `aud_monitor_drain()`. Monitoring drops whatever
  does not fit, which is right when the input sets the pace and useless when a
  file does: read at disk speed, a take would be consumed in a moment and all
  but the first buffer of it thrown away. Playback asks how much will fit and
  hands over exactly that, which makes the output's own consumption the clock,
  and waits for the queue to empty before closing so the end is not cut off.
- `-D` names the **output** under `--play`. `$AUDIAKI_DEVICE` is deliberately
  ignored there: what it names is a capture device, and handing one to the
  playback side would fail for a reason nobody would guess from the message.

- Pre-roll. `--preroll SECS` holds the last few seconds and waits, and the take
  starts that far before you press Enter; `audiaki-gui --preroll SECS` keeps the
  same seconds while the window is idle, so Record does. The take everyone loses
  is the one played to check the sound, and the audio was already being captured
  for the meters - it was only being thrown away. Interrupting the wait writes
  no file, and with `--take` does not use up the number.
- `preroll` module: a circular buffer of captured frames in the hardware format,
  drained through its one or two contiguous segments rather than a second copy
  of itself. Not `ringbuf`, which carries floats for the visualiser and the
  monitor: pre-roll seconds are part of the recording and have to reach the file
  bit for bit, which a round trip through float would not do for a 24 or 32 bit
  take.

- A PipeWire backend. audiaki now talks to the sound server that owns the card
  on most current desktops, rather than only to the card. It is chosen without
  being asked: if a daemon answers, audiaki uses it; if none does, ALSA, exactly
  as before. `--backend auto|pipewire|alsa` and `$AUDIAKI_BACKEND` override the
  choice, and `audiaki-gui` takes `-b` for the same. Asking for a backend that
  is not there is an error rather than a silent downgrade - someone who typed
  `--backend pipewire` wants to hear it was missing, not to find out later that
  their device names came from somewhere else.
- Recording another application's output. A PipeWire sink appears in `--list`
  described as a monitor, and capturing it records what is being played to it -
  a browser, a synth, a call. Opening the card cannot do this at any setting.
- Device names that match the rest of the desktop. Under PipeWire `--list`
  reports `alsa_input.pci-0000_00_1f.3.analog-stereo` and "Built-in Audio
  Analog Stereo" - the strings the system settings shows - instead of the
  `hw:CARD=x,DEV=n` that only ALSA uses. The DEVICE column widens to fit them;
  under ALSA it stays at the 32 it always was, so that listing is unchanged.
- `backend` module: the two op tables an audio system is reached through, and
  the selection between them. `device.c` and `monitor.c` became dispatchers
  over it, with the implementations moving to `device_alsa.c`, `monitor_alsa.c`
  and their PipeWire counterparts.

### Changed

- `src/` is one directory per layer — `cli`, `cmd`, `backend`, `audio`, `take`,
  `media`, `term`, `util` — where it was 62 files in a flat directory. No
  behaviour changes; local includes are now written from `src/`
  (`#include "audio/format.h"`), so the layer a header comes from is readable at
  the include site. `tests/` mirrors the same tree.

- The layering is enforced by the build rather than described in a comment. The
  Makefile's `PORTABLE_SRCS` — the objects the tests link against, and so the
  set that must build with no sound server present — was a hand-maintained list
  of fifteen files that silently went stale whenever a module was added; it is
  now `$(filter-out src/backend/% src/cmd/% src/cli/% src/main.c,$(SRCS))`. It
  turned out the old list had already drifted: `visualize` and `meter` were
  portable and untested by omission, and are now linked into the suite.

- `main.c` dispatches and nothing else. The five `run_*` functions that mapped
  options onto modules moved into `src/cmd/`, one file per command, and the
  option struct they share with the parser moved to `src/options.h` — so a
  command no longer includes the argument parser to find out what it was asked
  for. `aud_recorder_options`, `aud_play_options` and `aud_tune_options` had one
  caller each and are now internal to the command that uses them.

- Three files that had grown past what one unit should hold were split along
  seams they already had: the CLI's help text out of `cli.c` into `cli/usage.c`,
  the monitor-and-metronome output out of the capture loop into
  `cmd/playback.c`, and the desktop window's 1,300 lines into `gui/main.c`,
  `args.c`, `devices.c` and `screen.c` over a shared `gui/app.h`. The `--list`
  table moved out of `backend/device.c`, which no longer decides how anything is
  laid out.

- `--monitor-device` no longer implies `--monitor` when `--click` is also given.
  Naming an output used to mean "play the input through this one", because there
  was nothing else it could mean; with a metronome there is, and it now names
  where the click comes out without switching monitoring on. Unchanged without
  `--click`, as is `--monitor-gain`, which only ever scaled the input.
- The terminal meter takes a total length, and draws `00:12 / 03:45` with no
  xrun counter when it has one. The recording line is unchanged to the byte.
- `device.h` no longer includes `<alsa/asoundlib.h>`, and the stream handle in
  `aud_device` is opaque. The header comment claimed ALSA was confined to
  `device.c` already; it was not, because every file including the header was
  compiled against libasound whether it used it or not, and `recorder.c` reached
  through the handle to call `snd_pcm_drop()` directly. That call is now
  `aud_device_drop()`.
- Monitoring through PipeWire works at any capture rate, because the server
  resamples. The ALSA monitor still declines rather than carry an interpolator
  for a convenience feature, and now says which backend does not have the limit.
- `--probe` under PipeWire reports what a stream will actually be given and says
  where that comes from, rather than a hardware capability table. Through a
  server that converts, the card's own format list no longer decides what a
  recording can be, and printing it as though it did would be a lie of exactly
  the kind `--probe` exists to prevent. `--backend alsa --probe` still asks the
  hardware.
- `--list --json` and `--probe --json` gained a `backend` field. Additive, so
  existing filters keep working.
- The README was split: how to drive audiaki stays there, why it is built the
  way it is moved to `DESIGN.md`. The two audiences were reading past each
  other, and the module map in `CONTRIBUTING.md` had drifted a backend behind.
- The README was cut down again, to what someone deciding whether to install
  audiaki needs. The reference material it had grown - every option, the meter,
  the tuner, pre-roll, troubleshooting - moved to `docs/USAGE.md`, and the
  desktop app's build steps, controls and visualisers to `docs/DESKTOP.md`.
  Both ship in the release tarball alongside the man page.

### Removed

- Nine functions nothing called: `aud_engine_device()`,
  `aud_engine_monitor_gain()`, `aud_monitor_device()`, `aud_monitor_flush()`,
  `aud_monitor_underruns()`, `aud_recorder_stop_requested()`, `aud_ui_panel()`,
  `aud_viz_bands()` and `aud_viz_mode_get()`. There is no library ABI here, so
  an accessor with no caller is weight rather than surface.
- With them, the `flush` and `underruns` slots on `aud_monitor_ops`, and their
  ALSA and PipeWire implementations. The engine closes the monitor when
  monitoring is switched off rather than flushing it, so the vtable entry only
  obliged every future backend to implement something nothing reached. The
  underrun counters were incremented and never read, which took the PipeWire
  monitor's `primed` and `prime_frames` bookkeeping with them.
- The monitor backends' `device_out` out-parameter, which reported the resolved
  playback device name into a field that nothing read once `aud_monitor_device()`
  was gone. `rate_out` and `channels_out` stay: the ALSA monitor refuses to open
  on a rate it cannot match, and that is a negotiation a backend may need.
- The PipeWire headers are optional at build time, the way raylib is. Without
  `libpipewire-0.3-dev` the two backend files are not compiled and the binary is
  ALSA-only, which is what keeps CI and headless machines building unchanged.
  `make help` reports which backends are in.

## [1.0.0] - 2026-08-06

### Added

- The desktop app's device list follows the hardware. `device` gained a watch
  that re-walks ALSA every couple of seconds - about 0.3 ms for two cards - and
  the window rebuilds its dropdown whenever the answer differs, so an interface
  plugged in shows up without a restart and one unplugged leaves the list. The
  watch also listens on `/dev/snd`, the directory the kernel puts a card's
  nodes in the moment it registers one, which brings a plugged-in device up in
  well under a second where those events are delivered; they are not everywhere
  - a sandbox or a container can hold its own mount of devtmpfs, where the
  nodes come and go exactly as they do outside and no watch on them ever fires
  - so the sweep is the mechanism and inotify only shortens the wait. The
  rebuilt list is only swapped in when it actually differs, so an open menu
  cannot shuffle under the pointer that is about to click a row.
- The desktop app opens the capture stream again when the device it was using
  comes back, whether the window came up without it or its stream died with the
  cable. A dead stream cannot be revived, and re-picking the device in the
  dropdown does nothing because the row is already selected, so before this a
  device that returned was unreachable without restarting the app.
- A tuner. `audiaki --tune` opens the capture device and reports the pitch of
  whatever is being played as a note, a needle on a scale of half a semitone
  either side of it, and the frequency and level, until Ctrl+C. Nothing is
  written; it is a display, not a take. `--a4` moves the reference pitch for an
  ensemble tuned somewhere other than concert pitch.
- `tuner` module: monophonic pitch detection using the YIN difference function,
  plus the note arithmetic that turns a frequency into a note name, an octave
  and an offset in cents. Time domain rather than a peak over the spectrum,
  because a plucked low string often has more energy in its second harmonic than
  in its fundamental and a spectrum peak would report the octave above. No ALSA
  and no I/O, so it is unit tested like the rest of the analysis.
- A `tuner` style in the desktop app, alongside the five visualisers and reached
  the same ways - the strip on the stage, the `V` key, or `-s tuner` up front.
  It runs the same detection as `--tune`, and only while it is the visible
  style, so a video render is not slowed by a needle nobody is looking at.
- `tune` module: the `--tune` capture loop, which is to tuning what `recorder`
  is to a take. It keeps ALSA out of `tuner`, which the desktop app also uses.
- `meter_draw_tuner()` draws the terminal tuner line. With stderr redirected
  there is no line to redraw in place, so `--tune` reports each note once as it
  settles instead, which makes it something a script can log.
- `parse_double()` parses a bounded decimal as strictly as `parse_uint()` parses
  an integer; `--a4` and the duration fields both go through it.

- `audiaki-gui`, a desktop application. A window with a record / pause / resume
  / stop transport, a live level meter with peak hold, a clipping indicator and
  a real-time spectrum. The capture stream opens with the window and stays open,
  so levels and the spectrum are live before a take is started. Takes are always
  numbered from a prefix, so recording cannot overwrite an earlier one.
- A capture device dropdown in the desktop app, listing `default` alongside
  every capture PCM ALSA reports. Switching rebuilds the capture stream and the
  analyser around whatever the new device negotiates, and falls back to the
  previous device if the new one will not open. Disabled while a take is open,
  so a device change cannot truncate a recording.
- `aud_device_enumerate()` returns the capture device list as data rather than
  printing it; `--list` is now a presentation layer over it and its output is
  unchanged.
- Playback monitoring: a second ALSA stream plays the input back while it
  records, with a volume slider and a toggle. Off by default, because
  monitoring a microphone through speakers howls. It drops frames rather than
  queueing them when the output falls behind, so the monitor cannot drift
  further behind the input the longer a take runs.
- The desktop visualiser draws a stem per band with an additively blended
  glowing cap, after [musializer](https://github.com/tsoding/musializer). It
  reuses the same `spectrum` analyser as the terminal meter and the video
  renderer.
- Video capture in the desktop app: a **Video** toggle, or `-V, --video`, that
  renders an MP4 of the visualiser alongside the take's WAV. It is rendered
  from the finished take rather than captured live off the screen, so it cannot
  cost the recording an xrun and is frame-accurate regardless of the window's
  size or refresh rate; progress is shown in the status line and **Stop**
  becomes **Cancel** while it runs. `--video-size` and `--video-fps` set the
  output, defaulting to 1280x720 at 60. Needs `ffmpeg` on `PATH`; without it
  the WAV is still written and the window says why the video was not.
- An **Audio** toggle beside it, or `--video-silent`, choosing whether that MP4
  carries the take's audio. On by default; off encodes the video with no audio
  stream at all, rather than a muted one, for a clip going into an edit that
  has the sound already. `ffmpeg_start_rendering()` takes a NULL sound file for
  this. The take's WAV is written either way, and is still what the picture is
  drawn from.
- `aud_take_with_extension()` derives a video name from a take name. The CLI's
  `--visualize` default output now goes through it too, so the two cannot
  disagree about what `take-003.wav`'s video is called.
- Five live visualiser styles in the desktop app - `bars`, `mirror`, `radial`,
  `scope` and `waterfall` - switchable from a strip on the visualiser, with the
  `V` key, or up front with `-s, --style`. `scope` draws raw samples, triggered
  on a rising zero crossing so a steady note stands still; `waterfall` keeps
  about eight seconds of spectrogram history as a ring of texture columns, one
  written per frame.
- `monitor` module: ALSA playback for hearing the input, independent of the
  capture stream, so monitoring can fail or be switched off without the
  recording noticing.
- `ringbuf` module: a lock-free single-producer single-consumer float ring, so
  the capture thread hands audio to the drawing thread without a mutex that
  could stall it into an xrun. ALSA-free and unit tested.
- `aud_format_to_float()` decodes interleaved PCM to interleaved floats,
  keeping the channels apart, which is what monitoring needs.
- A `.desktop` entry, installed by `make install` when the desktop app is
  built, so audiaki appears in the application menu.
- `scripts/install-deps.sh` now installs the desktop app's OpenGL and X11
  headers too, across apt, dnf, pacman, zypper and apk, and takes `--no-gui`
  to leave them out on a headless machine.
- `--visualize FILE` renders a WAV recording into a spectrum visualiser video.
  audiaki analyses and rasterises the frames itself and pipes raw RGBA to
  `ffmpeg`, which encodes them and muxes in the original audio. `--size`,
  `--fps` and `--bars` control the output; `-o` names it, defaulting to the
  input path with `.mp4`. Inspired by
  [musializer](https://github.com/tsoding/musializer).
- `--spectrum` replaces the live peak bar with spectrum bars while recording,
  using block characters on UTF-8 terminals and an ASCII ramp elsewhere.
- `-o, --output` names the output file, for recording as well as rendering.
- A WAV reader (`wav_read_*`), tolerant of the chunk layouts other tools
  produce: unknown chunks are skipped, `WAVE_FORMAT_EXTENSIBLE` is unwrapped,
  and 8/16/24/32 bit PCM plus 32/64 bit float are decoded.
- `fft`, `spectrum` and `canvas` modules, all ALSA-free and unit tested. The
  live display and the video renderer share the same analyser.

### Changed

- A device the desktop app cannot open is no longer the end of the session. The
  window used to come up on the "no device" screen and stay there until it was
  closed; it now keeps the device dropdown live on that screen and opens
  whatever is picked there or plugged in afterwards.
- `aud_device_enumerate()` reports a machine with no sound cards as an empty
  list rather than an error, because a caller watching for hardware asks over
  and over and none of those asks is a failure. `--list` prints its header and
  the usual "no capture devices found" warning, and now exits 0 rather than 1.
- `ffmpeg` is a new optional run-time dependency, needed only for
  `--visualize`. Building and recording are unaffected.
- raylib is a new optional build-time dependency, vendored as a pinned
  submodule under `vendor/raylib` and needed only for `audiaki-gui`. `make`
  builds the desktop app when the submodule is initialised and the OpenGL and
  X11 headers are present, and quietly builds the command line recorder alone
  when they are not — so headless machines and CI are unaffected.
- `make install` now installs `audiaki-gui`, its `.desktop` entry and its icon
  as well, when the desktop app was built.
- The SIGINT/SIGTERM stop flag moved out of `recorder.c` into `signals.c` so
  the renderer can be interrupted too. `aud_recorder_install_signals()` and
  `aud_recorder_stop_requested()` still work as before.

### Fixed

- The tuner analysed the oldest samples in its buffer rather than the newest.
  The analysis span is `2 * tau_max + tau_max` samples but the buffer is that
  rounded up to a power of two, and the slack sat at the new end, so the
  newest 790 samples at 44.1 kHz - nearly 18 ms - were left out of every
  reading. Both the pitch and the gate that decides whether anything is being
  played described a moment that had already passed; a note is now picked up
  about 18 ms sooner. Affects `--tune` and the desktop app's tuner style.
- The desktop app missed clipping that ran into the positive rail. Signed PCM
  is asymmetric - the most negative sample normalises to exactly -1.0 but the
  most positive is one step short, 32767/32768 at 16 bit - so the engine's
  `peak >= 1.0` test only ever fired on a take that clipped downwards. Both the
  terminal meter and the app now share one `AUD_CLIP_THRESHOLD`.
- `--duration` is bounded, so `-t 1e308` is rejected instead of overflowing the
  conversion to a frame count. Being undefined behaviour it did not record for
  a very long time: the value converted to zero and the recorder wrote a single
  frame.
- Rendering to or from a path beginning with `-` works. ffmpeg reads any
  argument starting with a dash as an option and there is no `--` to stop it,
  so `-take01.wav` was parsed as flags and the render failed with
  "Unrecognized option". Such paths are now passed as `./-take01.wav`.
- The desktop app's device dropdown can reach every device. It drew only the
  first `AUD_UI_DROPDOWN_MAX_ROWS` entries with no way to scroll, so on a
  machine with more than eight capture devices the rest could not be selected
  at all - including the one named by `-D`, which is appended last. The list
  now scrolls on the mouse wheel and opens on the current selection.
- `audiaki-gui` validates `-r` and `-c` the way the recorder does. They went
  through bare `strtoul`, so `-c -5` became 4294967291 and `-r abc` became 0
  before being handed to libasound. The bounds now live in one place.
- The ffmpeg child no longer closes its own stdin if the pipe's read end lands
  on file descriptor 0, which can happen when audiaki is started with stdin
  already closed.

## [0.2.0] - 2026-08-04

The single-file recorder grew into a structured project. Behaviour is
compatible with 0.1.0 except for the default device, noted below.

### Added

- `--list` enumerates every capture-capable PCM device.
- `--format` pins the capture format instead of negotiating one.
- `--period` and `--periods` expose the buffer geometry.
- `--force` is now required to overwrite an existing recording; without it an
  existing file is an error rather than silently truncated.
- `--quiet`, `--verbose` and `--no-meter` control the output.
- `--version`, and long forms for every previously short-only option.
- `--duration` accepts `MM:SS` and `HH:MM:SS` as well as plain seconds.
- `AUDIAKI_DEVICE` sets the default device.
- Peak-hold marker and a clipping warning in the meter.
- Man page (`docs/audiaki.1`), installed by `make install`.
- Unit tests for the WAV writer, sample-format handling and option parsing,
  runnable without ALSA or a sound card.
- GitHub Actions CI: gcc and clang builds with warnings as errors, a build
  without ALSA installed, ASan/UBSan, clang-format, cppcheck and shellcheck.

### Changed

- **Default device is now `default` instead of `hw:CARD=Box,DEV=0`.** Set
  `AUDIAKI_DEVICE=hw:CARD=Box,DEV=0` to restore the old behaviour.
- Split the single `src/main.c` into focused modules; libasound is confined to
  `src/device.c`.
- `make` replaces `build.sh`, and `scripts/install-deps.sh` replaces
  `install.sh` with support for apt, dnf, pacman, zypper and apk.
- `--duration` now stops on an exact frame count instead of overshooting by up
  to one period.
- The meter refreshes on a time interval rather than every fourth period, so
  the rate no longer depends on the period size.
- The meter and status output are suppressed when stderr is not a terminal.
- Errors are reported consistently as `audiaki: error: ...` on stderr; stdout
  carries only `--list` and `--probe` output.

### Fixed

- The desktop app's video capture no longer creates the `.mp4` until it is
  finished. An MP4 is only playable once its moov atom is written, which ffmpeg
  does as the stream ends, so writing straight to the final name put a file on
  disk the moment recording stopped that looked like a finished take and opened
  as "no playable streams" for as long as the render ran - and stayed that way
  for good if the render was cancelled or interrupted. Frames now go to a
  hidden `.NAME.partial.mp4` that is renamed into place only on success.
- `aud_take_path()` no longer leaves a truncated filename in the caller's
  buffer when the name does not fit. It documented "dst untouched" but wrote
  through `snprintf` before checking the length, so a caller that tested the
  result second would have acted on half a name.
- Odd-sized payloads (24-bit mono) now get the RIFF pad byte required by the
  specification.
- Numeric options reject trailing garbage and negative values instead of
  silently accepting `-r 44100abc` or wrapping `-c -1` to a huge count.
- Capture and output buffers are freed on every error path.
- The 32-bit peak calculation no longer overflows on the most negative sample.
- `wav_close()` reports write and close failures instead of discarding them,
  so a full disk is no longer silently ignored.

## [0.1.0] - 2026-08-04

### Added

- Initial single-file ALSA capture-to-WAV recorder with format negotiation,
  a peak meter, `--probe`, and duration-limited recording.

[Unreleased]: https://github.com/HilthonTT/audiaki/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/HilthonTT/audiaki/releases/tag/v1.0.0
[0.2.0]: https://github.com/HilthonTT/audiaki/releases/tag/v0.2.0
[0.1.0]: https://github.com/HilthonTT/audiaki/releases/tag/v0.1.0
