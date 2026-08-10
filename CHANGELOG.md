# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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

### Fixed

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

### Changed

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
