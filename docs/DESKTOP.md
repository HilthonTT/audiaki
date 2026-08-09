# The desktop app

`audiaki-gui` is a multi-track recorder and editor over the same capture and
analysis core as the command line recorder: a transport, a timeline you can cut
about, playback monitoring and a live glowing spectrum. The
[README](../README.md) is the short version, [USAGE.md](USAGE.md) covers the CLI,
and [DESIGN.md](../DESIGN.md) covers why it is built this way.

<p align="center">
  <img src="../screenshots/desktop.png" alt="the audiaki desktop app" width="820">
</p>

- [Building it](#building-it)
- [Options](#options)
- [The layout](#the-layout)
- [Recording onto the timeline](#recording-onto-the-timeline)
- [Playing it back](#playing-it-back)
- [Editing](#editing)
- [Exporting](#exporting)
- [Controls](#controls)
- [Keeping a take](#keeping-a-take)
- [Recording video](#recording-video)
- [The visualisers](#the-visualisers)

## Building it

`audiaki-gui` needs [raylib](https://www.raylib.com/), which Debian and Ubuntu
do not package, so it is vendored as a submodule pinned to the version the
visualiser was drawn against.

```sh
./scripts/install-deps.sh              # OpenGL and X11 headers included
git submodule update --init --depth 1  # fetch raylib
make                                   # both binaries; raylib compiled once
```

Installing those headers by hand instead, on Debian or Ubuntu:

```sh
sudo apt install libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev \
                 libxcursor-dev libxinerama-dev libxkbcommon-dev
```

None of this is required for the command line recorder. With the submodule
uninitialised, `make` quietly builds `audiaki` alone and skips the window —
which is what you want on a headless machine. `sudo make install` also installs
a `.desktop` entry.

## Options

```sh
audiaki-gui                          # open the window on the default device
audiaki-gui -D plughw:CARD=Box,DEV=0 # ...on a particular interface
audiaki-gui -b alsa                  # ...through a particular backend
audiaki-gui -o session               # name takes session-001.wav and up
audiaki-gui --dir ~/Takes            # ...written in that folder
audiaki-gui --no-dialog              # never ask where a take should go
audiaki-gui -s waterfall             # start on a particular visualiser
audiaki-gui -s tuner                 # ...come up as a tuner
audiaki-gui -V                       # also render an MP4 of each take
audiaki-gui -V --video-size 1080p    # ...at a particular size
audiaki-gui -V --video-silent        # ...with no audio track in it
audiaki-gui -M                       # come up already monitoring
audiaki-gui --preroll 10             # start each take 10 s before Record
audiaki-gui --no-overdub             # do not play the project while recording
audiaki-gui --latency 14             # place overdubs by a measured round trip
audiaki-gui take01.wav take02.wav    # open takes as tracks straight away
audiaki-gui yesterday.aki            # ...or open a saved session
```

`--video-size` takes `WxH` or `720p`/`1080p`/`1440p`/`2160p`, and `--video-fps`
the frame rate; the defaults are 1280x720 at 60.

The capture stream opens with the window and stays open, so the spectrum moves
and the meter reads before you press anything — setting an input level should
not mean starting a take you are going to throw away. With `--preroll` those
seconds are kept rather than discarded, and Record starts the take that far
back.

## The layout

Top to bottom: the title and the capture device, a transport bar, an edit bar,
the visualiser panel, the time ruler, the tracks, and a status bar.

The visualiser is a drawer rather than the window. `B`, or the arrow beside its
name, shuts it and gives the room to the tracks; it keeps running either way, so
opening it shows what is happening now rather than an empty panel filling up.

Each track has a control column of its own — its name, **Mute**, **Solo**, a
gain slider and a pan slider, with `x` to close it and `^` to fold it down to a
strip. Dragging the line under a track makes it taller. Beside it is a strip of
amplitude labels, and then the waveform: the peak envelope in deep blue with the
RMS as a lighter core inside it, so how loud it got and how loud it is are both
legible at a glance.

## Recording onto the timeline

Click where you want the take to start and press **Record**. The waveform grows
along the timeline as you play, the way it does in Audacity — what you are
seeing is the same audio going into the WAV, frame for frame, not a preview of
it.

Where it lands: at the cursor, on the selected track if that track is free from
there on, and on a new track otherwise. So clicking further along a take and
pressing record again adds to that take; clicking over audio that is already
there gives the new one a lane of its own rather than refusing or overwriting.

The take is a real WAV on disk the whole time — numbered from the prefix, in
`take_dir` — so an interrupted session is not a lost one. It is also on the
timeline the moment it stops, ready to cut about, without a dialog in between.

## Playing it back

`space` plays: the selection if there is one, otherwise from the cursor to the
end. `space` again, or **Stop**, or `S`, stops it. The playhead runs along the
tracks and the view follows it.

What you hear is the mix — every track that is not muted, at its gain and pan,
with solo taking over the moment any track has it. It is the same mix **Export**
writes, which is the point of it being one piece of code.

## Editing

Drag across a track to select time; `ctrl`+click another track to take it into
the selection as well; `ctrl+A` selects everything. Then:

| | |
| --- | --- |
| **Cut** / **Copy** / **Paste** | `ctrl+X` / `ctrl+C` / `ctrl+V` |
| **Delete** | `del` — removes the selection and closes the gap |
| **Silence** | empties it and leaves the timing alone |
| **Trim** | throws away everything outside the selection |
| **Split** | cuts the clips at the selection's edges without removing anything |
| **Copy to** | the selection onto a new track of its own, at the same position |
| **Fade in** / **Fade out** | `[` / `]` — ramps the selection out of silence, or into it |
| **Undo** / **Redo** | `ctrl+Z` / `ctrl+shift+Z`, 64 steps deep |

Pasting over a selection replaces it, the way typing over selected text does.
None of this copies audio about — see [DESIGN.md](../DESIGN.md#the-editor) — so
cutting an hour-long take is as quick as cutting a bar of one, and so is undoing
it.

`ctrl`+wheel zooms about the pointer, `shift`+wheel scrolls, the wheel alone
walks up and down the tracks, and `F` fits the selection or the whole project to
the window.

The arrow keys drive the same thing without the pointer. `←` and `→` move the
cursor by about eight pixels' worth — so a nudge is the same gesture whatever
the zoom, coarse when a whole session is on screen and sample-accurate when one
transient is. `ctrl` steps clip edge to clip edge instead, which is where a trim
usually wants to land; `home` and `end` go to either end of the project. Holding
`shift` moves the far end of the selection rather than the cursor, growing it
from wherever it started rather than from the left, and `↑`/`↓` walk the track
selection up and down the stack, scrolling it into view.

The cursor is where the next edit goes. The playhead — where the audio has
actually got to — is drawn separately while something is playing or recording,
so moving the cursor during playback moves one and not the other.

A fade is a length on the clip rather than something written into the audio, so
it costs nothing, undoes like everything else, and can be taken off again by
fading a zero-length selection. The waveform follows it, so what you see is what
you will hear. A cut that lands inside an existing ramp truncates it: a clip can
say "ramp up from silence", and there is no way for one to say "carry on from
half way up a fade that began in the clip before".

There are no crossfades. Clips on a lane do not overlap — that is the invariant
the whole editor is built on — and a crossfade needs two pieces of audio
sounding at once. Fading one out and the next in gives a dip, not a crossfade,
and calling it one would be a lie.

## Sessions

The takes are files the moment they stop. What you have *done* to them — the
cuts, the levels, which clip sits where — is the session, and **Save** or
`ctrl+S` writes that to a `.aki` file. `ctrl+shift+S` asks where; **Open** or
`ctrl+O` opens one; `audiaki-gui yesterday.aki` opens one at startup.

A session refers to its takes rather than containing them, so it is a few
kilobytes of readable text whatever it holds, and it can be opened in an editor
and fixed by hand. Takes kept beside the session file are referred to relatively,
so a session folder can be copied to another disk and still open. Move a take
somewhere else and the session says which one is missing rather than opening
with a silent lane.

Closing the window with unsaved edits writes them out rather than dropping them:
back to the session file if there is one, and to `recovered.aki` beside the
takes if there is not.

`audiaki --render session.aki -o mix.wav` mixes a session down without opening a
window at all, which is what makes a session something a script can use.

## Overdubbing

Press **Record** with something already on the timeline and it plays while you
record over it. That is what **Overdub** is, and it is on by default; turn it off
to record a second take in silence.

Playing along to something means hearing it first, and hearing it costs time —
the output holds a buffer, then the input holds another. So what you played in
response to a beat does not arrive labelled with that beat; it arrives a round
trip late. The window corrects for it by placing the take a round trip earlier
than you pressed the button, because that is when the sound was actually made.

The correction is estimated from the two buffers, which is a starting point and
not a measurement: the converters, the driver and the interface all add delay
that nothing here can see. If your overdubs land consistently late or early,
measure it — play a click through the output into the input and see how far off
it is — and say so:

```
audiaki-gui --latency 14
```

or put it in the config file, where it belongs, since it is a property of the
machine rather than of the session:

```
latency_ms = 14
```

What none of this fixes is jitter. Playback here is fed from the drawing loop
and capture runs on its own thread, so the two start within a drawn frame of
each other rather than on the same sample. Overdubs land close, not sample
locked. Use headphones: monitoring plus playback through speakers is two ways
for the room to get into the take.

## Exporting

**Export**, or `ctrl+E`, mixes down to a WAV: the selection if there is one,
the whole project if not. 24-bit by default, at the project's own sample rate,
and stereo unless every track in it is mono.

## Controls

| Control | Does |
| --- | --- |
| **Play** | Plays the selection, or from the cursor |
| **Record** | Records onto the timeline at the cursor |
| **Pause** / **Resume** | Stops and continues writing, without closing the file |
| **Stop** | Closes the take; it is already on the timeline |
| **Import** | Opens a WAV as a new track |
| **Export** | Mixes down to a WAV |
| **Open** / **Save** | Opens a session, or writes this one out |
| **Overdub** | Plays the project while recording over it |
| **Video** | Also render an MP4 of the visualiser when the take stops |
| **Audio** | Whether that MP4 carries the take's audio; off renders it silent |
| **Monitor** | Plays the input back through the default output |
| Volume slider | Monitoring level, from silent to +6 dB |
| Device dropdown | Switches capture device, `default` plus every capture PCM |

| Key | Does |
| --- | --- |
| `space` | Play, or pause and resume once a take is running |
| `R`, `ctrl+space` | Record from the cursor |
| `S` | Stop the take or playback, or cancel a video render |
| `I` / `ctrl+E` | Import a WAV / export a mix |
| `ctrl+S` / `ctrl+O` | Save the session / open one; `ctrl+shift+S` saves it as |
| `[` / `]` | Fade the selection in, or out |
| `←` / `→` | Move the cursor; `ctrl` steps clip edge to clip edge |
| `shift+←` / `→` | Extend the selection instead of moving the cursor |
| `↑` / `↓` | Select the track above or below; `shift` adds it |
| `home` / `end` | Cursor to the start or the end of the project |
| `M` | Toggle monitoring |
| `B` | Show or hide the visualiser panel |
| `V` / `1`–`6` | The next visualiser style / one outright |
| `F` | Fit the selection, or the whole project |
| `F11` | Fullscreen |
| `?` | The list of keys, over the window; `Esc` closes it |

While the save dialog is up it has the keyboard and the rest of the window is
disabled behind it — including Record, so the take being named cannot be lost
to a keypress meant for the dialog.

The same list is behind the `?` in the header, because a shortcut nobody can
find is a shortcut nobody has. Resting the pointer on any control says what it
does and which key does it too — including the greyed-out ones, which is the
point: a disabled button is a question, and the answer should not be in here.

The window title carries the transport, so a window behind another one still
answers "is it still recording?" — `audiaki - recording 00:12 - take-003.wav`.

The device dropdown is disabled while a take is open: switching closes the
capture stream, which would truncate the recording. Stop first. The list
follows the hardware — plug an interface in and it appears a moment later, and
a window that came up with no device at all opens the first one plugged in.
Takes are always numbered from the prefix, so pressing record cannot destroy an
earlier take.

**Monitoring feeds your input back to your speakers**, which will howl if you
are recording a microphone in the same room. It starts off for that reason.

## Keeping a take

Off by default in the window, and deliberately the other way round from the
terminal recorder: there a take is a file and the question is where to keep it,
here it lands on the timeline the moment it stops and a dialog between playing
something and editing it would be a dialog in the way. Where the WAV goes is
`--dir`'s job, and **Export** is how a finished mix leaves.

Turn it on with `prompt = yes` in the config file, and stopping opens a small
dialog over the window: the name the take was given, the folder it was written
in, and the sub-folders of that folder to click through.

Nothing has happened to the file when it opens. It is a complete, closed WAV
sitting exactly where it was recorded, and it stays there unless **Save** is
pressed — **Keep here**, `Esc` and closing the window all leave it alone. Save
moves it, creating the folder if it has to, and never lands on a file that is
already there: a name that is taken comes back as a line under the fields
rather than as a take you no longer have.

`Tab` moves between the two fields, `Enter` saves, `Esc` keeps it where it is.
Clicking a folder goes into it; `..` goes up. A path can also be typed or
pasted into the folder field, and the list follows it.

Where the take was written in the first place is `--dir`, or `take_dir` in the
config file — the same file the CLI reads, described in
[USAGE.md](USAGE.md#the-config-file). `--no-dialog` turns the question off again
for one run.

The same browser is what **Import** and **Export** open, asking their own
question: it lists the WAVs in a folder as well as its sub-folders.

With **Video** on, the render starts once the dialog is answered rather than
beside it: the MP4 is made from the take, and it should be made from wherever
the take ended up.

## Recording video

**Video** off is audio only — `take-003.wav` and nothing else. Video on writes
that same WAV and then `take-003.mp4` alongside it, showing whichever
visualiser was selected. It needs `ffmpeg` on `PATH`; without it the WAV is
still written and the window says why the video was not. **Audio** decides
whether that MP4 gets an audio track — on by default, off for a clip going into
an edit that already has the sound. Both are only settable between takes.

The video is rendered after the take stops rather than captured live, so it is
frame-accurate and cannot cost the recording an xrun — see
[DESIGN.md](../DESIGN.md#rendering-video). While it runs, **Stop** becomes
**Cancel** and the status line shows progress; cancelling removes the partial
video and keeps the WAV.

## The visualisers

<p align="center">
  <img src="../screenshots/styles.png" alt="the visualiser styles" width="820">
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
use. How each is drawn, and why: [DESIGN.md](../DESIGN.md#the-visualisers).
