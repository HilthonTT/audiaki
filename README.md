<p align="center">
  <img src="assets/logo.png" alt="audiaki logo" width="140">
</p>

<h1 align="center">audiaki</h1>

<p align="center">
  Capture-to-WAV recorder for Linux with a multi-track editor, live
  metering and a spectrum visualiser. Talks to PipeWire or ALSA.
</p>

<p align="center">
  <a href="https://github.com/HilthonTT/audiaki/actions/workflows/ci.yml"><img src="https://github.com/HilthonTT/audiaki/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"></a>
</p>

audiaki opens a capture device, picks the best sample format available, and
streams it straight into a PCM WAV file with a live meter. It can also turn a
finished take into a spectrum visualiser video.

It talks to PipeWire where there is one and to ALSA where there is not, picking
whichever answers without being told. Through PipeWire it coexists with whatever
else has the interface open, names devices the way the rest of the desktop does,
and can record another application's output; through ALSA it opens the card with
nothing in between.

Two binaries share one capture and analysis core: `audiaki`, the command line
recorder, and `audiaki-gui`, a multi-track recorder and editor — record onto a
timeline with the waveform growing as you play, overdub onto it, cut and paste
it about, save the session and export a mix. Written for a Sonicake Smart Box
(QME-20) guitar interface, but it works with any ALSA capture device — USB
interfaces, built-in codecs, `plughw` plugins.

```
 00:12 [##################|          ]  -8.4 dBFS  xruns:0
 00:12 ▁▂▄█▆▃▂▁▁▂▃▅▇▆▄▂▁▁▁▂▁▁  -8.4 dBFS  xruns:0
```

## Why not `arecord`?

`arecord` is more general. `audiaki` is a single small binary aimed at one job:
plug in an instrument, get a clean take, see the level while you play. It speaks
PipeWire as well as ALSA so it need not be told to get out of the way of the
browser holding the same interface, negotiates the widest format the device
offers (S32 → S24 → S16), shows a peak meter or a live spectrum, tunes the
instrument, plays the input back so you hear the take as you make it, counts you
in and keeps time with a metronome only you can hear, keeps the
seconds before you pressed record, stamps each take with what made it and when,
numbers takes so it
cannot overwrite one, stops on an exact frame count so a 30 second take is
30.000 seconds, and patches the WAV header on exit — including on Ctrl+C, so
interrupted recordings are still valid files.

## Install

Requires a C11 compiler, `make`, and — on Linux — the ALSA development headers.
The PipeWire (`libpipewire-0.3-dev`) and JACK (`libjack-jackd2-dev`) headers are
optional: with them present those backends are compiled in, without them `make`
quietly builds without them and everything else works unchanged. On macOS the
CoreAudio backend is built instead, out of the system frameworks, with nothing
to install. `make help` reports which backends a build has. Rendering a video
also needs `ffmpeg` on `PATH` at run time — not to build or to record.

```sh
./scripts/install-deps.sh   # or install libasound2-dev / alsa-lib-devel yourself
make
sudo make install           # installs to /usr/local; override with PREFIX=~/.local
```

The script handles apt, dnf, pacman, zypper and apk, and takes `--dry-run` to
show what it would install. By default it also pulls in the PipeWire and JACK
headers, `ffmpeg`, and the desktop app's OpenGL and X11 headers;
`--no-pipewire`, `--no-jack`, `--no-ffmpeg` and `--no-gui` skip those. The
desktop app additionally needs the vendored raylib submodule — see [docs/DESKTOP.md](docs/DESKTOP.md#building-it).

## Usage

```sh
audiaki --list                       # which capture devices exist
audiaki --tune                       # tune up before playing anything
audiaki take01.wav                   # record until Ctrl+C
audiaki --spectrum -t 1:30 take02.wav  # 90 seconds, watching the spectrum
audiaki --take session               # record the next free session-NNN.wav
audiaki --dir ~/Takes --take session # ...kept in one folder
audiaki --channel 1 take03.wav       # a stereo interface, one instrument -> mono
audiaki --preroll 10 take04.wav      # keep the 10 seconds before Enter
audiaki -M take05.wav                # hear it while it records (headphones!)
audiaki -M --click 120 take05.wav    # ...in time with a metronome
audiaki --note "clean tone" take06.wav  # ...and say what it was
audiaki --info take01.wav            # how did that take come out?
audiaki --info session-*.wav         # ...and the rest of them, a row each
audiaki --play take01.wav            # ...and what does it sound like?
audiaki --play session-*.wav         # ...all of them; space pauses, n skips
audiaki --visualize take01.wav       # render take01.mp4
audiaki --render session.aki         # mix a saved session down, no window
audiaki-gui                          # the multi-track recorder and editor
audiaki-gui take01.wav take02.wav    # ...opened on those takes
audiaki-gui session.aki              # ...or on a saved session
```

When a take finishes, audiaki asks where to keep it and what to call it — Enter
twice keeps it where it is, and the window asks the same in a dialog you can
play the take back from before answering. Where they go by default is one line
in `~/.config/audiaki/config`:

```ini
take_dir = ~/Takes
```

The window records at the cursor onto whichever track is free, draws the
waveform as it arrives, plays the project back while you record over it, counts
it out with a metronome and rules it into bars you can snap to, loops a passage
while you learn it, cuts,
copies, pastes, splits, trims and fades with 64 steps of undo, saves the session
as a `.aki` file and exports a WAV. A session refers to its takes rather than
containing them, so it is a few kilobytes of readable text that `audiaki
--render` can mix down without a window. The visualiser is a panel of it you can
shut, and beside it is a spectrum of what you recorded that you can draw on —
drag a mains hum off the graph, or subtract a hiss, and the take is saved as
though it was never there. Every option, the meter, the tuner, pre-roll, monitoring, the
metronome, take metadata, playback, `--json` output and troubleshooting:
[docs/USAGE.md](docs/USAGE.md), `man audiaki`, or
`audiaki --help`. The window, its keys and its visualisers:
[docs/DESKTOP.md](docs/DESKTOP.md).

<p align="center">
  <img src="screenshots/desktop.png" alt="the audiaki desktop app" width="820">
</p>

## Documentation

| | |
| --- | --- |
| [docs/USAGE.md](docs/USAGE.md) | The CLI in full: options, backends, meter, tuner, video, troubleshooting, limitations |
| [docs/DESKTOP.md](docs/DESKTOP.md) | `audiaki-gui`: building it, controls, keys, visualisers |
| [DESIGN.md](DESIGN.md) | Why it is built this way — module layout, backends, analysis |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Bug reports and patches |
| [CHANGELOG.md](CHANGELOG.md) | What changed, release by release |

## Development

```sh
make            # build build/audiaki
make test       # unit tests (no ALSA device or headers needed)
make debug      # -O0 with AddressSanitizer and UBSan
make check      # tests plus clang-format check
make format     # apply clang-format
make STRICT=1   # warnings become errors, as in CI

make HOTRELOAD=1 gui   # a window that reloads its own code on F5, session and all
```

The module layout and the rule that keeps most of the tree free of any audio
system are in [DESIGN.md](DESIGN.md#layout).

## Limitations

Linux and macOS: ALSA, PipeWire and JACK on the first, CoreAudio and JACK on the
second — no Windows backend. Under JACK the rate and the period are the server's
and cannot be changed from here; under CoreAudio a rate belongs to the device,
so asking for one moves it for everything else using that device. The tuner is
monophonic, and video rendering shells out to `ffmpeg` and happens after the
take rather than during it. The desktop app falls back to browsing folders
itself when neither `zenity` nor `kdialog` is installed to hand the question to.
`--no-metadata` still stops at the 4 GB RIFF limit, since a plain 44-byte header
has nowhere to put a 64-bit size; a stamped take becomes an RF64 file instead
and keeps going. Overdubs and the click are placed by a latency correction that
is estimated unless you measure it, and playback and capture start within a
drawn frame of each other rather than on the same sample, so they land close
rather than sample locked. The full list is in
[docs/USAGE.md](docs/USAGE.md#limitations).

## Credits

The logo is the
[teriyaki icon](https://www.flaticon.com/free-icon/teriyaki_6632525) by
**AUTHOR** from [Flaticon](https://www.flaticon.com/), used under the
[Flaticon Free License](https://www.freepikcompany.com/legal#nav-flaticon-agreement)
with attribution.

## License

MIT. See [LICENSE](LICENSE) — this covers the source code. The logo in
`assets/` is licensed separately, as noted under [Credits](#credits).
