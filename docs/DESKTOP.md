# The desktop app

`audiaki-gui` is a window over the same capture and analysis core as the command
line recorder: a transport, playback monitoring and a live glowing spectrum. The
[README](../README.md) is the short version, [USAGE.md](USAGE.md) covers the CLI,
and [DESIGN.md](../DESIGN.md) covers why it is built this way.

<p align="center">
  <img src="../screenshots/desktop.png" alt="the audiaki desktop app" width="820">
</p>

- [Building it](#building-it)
- [Options](#options)
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
```

`--video-size` takes `WxH` or `720p`/`1080p`/`1440p`/`2160p`, and `--video-fps`
the frame rate; the defaults are 1280x720 at 60.

The capture stream opens with the window and stays open, so the spectrum moves
and the meter reads before you press anything — setting an input level should
not mean starting a take you are going to throw away. With `--preroll` those
seconds are kept rather than discarded, and Record starts the take that far
back.

## Controls

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
| `S` | Stop, or cancel a video render |
| `M` | Toggle monitoring |
| `V` | Next visualiser style |
| `1`–`6` | A visualiser style outright |
| `F` | Fullscreen |
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

Stopping opens a small dialog over the window: the name the take was given, the
folder it was written in, and the sub-folders of that folder to click through.

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
[USAGE.md](USAGE.md#the-config-file). `--no-dialog`, or `prompt = no` in that
file, turns the question off and leaves every take where it was recorded.

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
