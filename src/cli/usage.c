/* SPDX-License-Identifier: MIT */
/*
 * usage.c - the help text, and the version line.
 *
 * Apart from cli.c because it is a third of what that file was, changes for
 * entirely different reasons, and reads better as one continuous piece of prose
 * than as prose wrapped around a getopt loop. Every default it quotes is the
 * macro the parser uses, so the two cannot drift.
 */
#include "cli/cli.h"

#include "audio/click.h"
#include "audio/tuner.h"
#include "backend/device.h"
#include "backend/monitor.h"
#include "media/visualize.h"
#include "take/meta.h"
#include "version.h"

void cli_print_usage(FILE *out)
{
  fprintf(out,
          "usage: " AUDIAKI_NAME " [options] <output.wav>\n"
          "       " AUDIAKI_NAME " --visualize <input.wav> [-o output.mp4]\n"
          "       " AUDIAKI_NAME " --info <input.wav>\n"
          "       " AUDIAKI_NAME " --play <input.wav> [-D output]\n"
          "       " AUDIAKI_NAME " --tune [-D device]\n"
          "       " AUDIAKI_NAME " --probe [-D device]\n"
          "       " AUDIAKI_NAME " --list\n"
          "\n"
          "Record a capture device straight to a PCM WAV file, tune an\n"
          "instrument on it, measure a finished take, play one back, and turn\n"
          "one into a visualiser video.\n"
          "\n"
          "Recording options:\n"
          "  -D, --device NAME     capture device (default: %s, $AUDIAKI_DEVICE)\n"
          "  -r, --rate HZ         sample rate (default: %u)\n"
          "  -c, --channels N      channel count (default: %u)\n"
          "  -f, --format NAME     s16_le, s24_3le, s24_le or s32_le\n"
          "                        (default: best the device offers)\n"
          "  -t, --duration SPEC   stop after SS, MM:SS or HH:MM:SS\n"
          "  -p, --period FRAMES   period size (default: %u)\n"
          "  -n, --periods N       periods per buffer (default: %u)\n"
          "      --take PREFIX     write the next free PREFIX-001.wav\n"
          "      --preroll SECS    hold SECS of audio and wait for Enter, so the\n"
          "                        take starts SECS before the keypress\n"
          "      --note TEXT       stamp the take with a note (up to %u\n"
          "                        characters), readable again with --info\n"
          "      --no-metadata     write a plain 44-byte header, with nothing\n"
          "                        about the take in it\n"
          "      --spectrum        show live spectrum bars instead of the peak bar\n"
          "      --no-meter        do not draw anything while recording\n"
          "  -M, --monitor         play the input back while recording it; use\n"
          "                        headphones, speakers will feed back\n"
          "      --monitor-device NAME\n"
          "                        output to monitor through (default: %s)\n"
          "      --monitor-gain X  scale what is monitored, 0.0 to 2.0; the file\n"
          "                        is unaffected (default: 1.0)\n"
          "      --click BPM       play a metronome at BPM (20 to 300) through the\n"
          "                        monitoring output; heard, never written to the\n"
          "                        take, so use headphones\n"
          "      --click-beats N   beats to a bar, accenting the first (default:\n"
          "                        %u; 0 or 1 for a bare pulse)\n"
          "      --click-gain X    how loud the click is, 0.0 to 2.0 (default: "
          "%.2g)\n",
          AUD_DEFAULT_DEVICE, AUD_DEFAULT_RATE, AUD_DEFAULT_CHANNELS,
          AUD_DEFAULT_PERIOD_FRAMES, AUD_DEFAULT_PERIODS, AUD_META_NOTE_MAX,
          AUD_MONITOR_DEFAULT_DEVICE, AUD_CLICK_DEFAULT_BEATS, AUD_CLICK_DEFAULT_GAIN);

  /*
   * A second call rather than a longer string: C99 only guarantees 4095
   * characters in a string literal, and the help text passed that.
   */
  fprintf(out,
          "\n"
          "Visualiser options:\n"
          "      --visualize FILE  render FILE (a WAV) to a video and exit\n"
          "  -o, --output FILE     video to write (default: input with .mp4)\n"
          "      --style NAME      bars, scope or waveform (default: bars)\n"
          "      --size SPEC       WxH, or 480p/720p/1080p/1440p/2160p\n"
          "                        (default: %ux%u)\n"
          "      --fps N           video frame rate (default: %u)\n"
          "      --bars N          number of spectrum bars (default: %u)\n"
          "\n"
          "Playback options:\n"
          "      --play FILE       play FILE (a WAV) through the default output\n"
          "                        and exit; -D names another output, -t stops\n"
          "                        early, and --spectrum and --no-meter apply as\n"
          "                        they do when recording\n"
          "\n"
          "Tuner options:\n"
          "      --tune            show the pitch of what is being played, and "
          "exit\n"
          "                        on Ctrl+C\n"
          "      --a4 HZ           reference pitch (default: %.0f)\n"
          "\n"
          "Common options:\n"
          "      --backend NAME    auto, pipewire or alsa (default: auto,\n"
          "                        $AUDIAKI_BACKEND)\n"
          "      --info FILE       report levels and clipping for FILE and exit;\n"
          "                        further files may follow, and are reported as\n"
          "                        one row each\n"
          "      --json            machine readable --list, --probe and --info\n"
          "  -y, --force           overwrite the output file if it exists\n"
          "  -q, --quiet           errors only\n"
          "  -v, --verbose         report device negotiation details\n"
          "  -l, --list            list capture devices and exit\n"
          "  -P, --probe           show what a device supports and exit\n"
          "  -h, --help            show this help and exit\n"
          "  -V, --version         show the version and exit\n"
          "\n"
          "Examples:\n"
          "  " AUDIAKI_NAME " take01.wav                  record until Ctrl+C\n"
          "  " AUDIAKI_NAME " --spectrum take01.wav       record, watching the "
          "spectrum\n"
          "  " AUDIAKI_NAME " -t 1:30 take02.wav          record 90 seconds\n"
          "  " AUDIAKI_NAME " --take session              record session-001.wav\n"
          "  " AUDIAKI_NAME " --preroll 10 take01.wav     keep the 10 s before "
          "Enter\n"
          "  " AUDIAKI_NAME " -M take01.wav               hear it while it "
          "records\n"
          "  " AUDIAKI_NAME " -M --click 120 take01.wav   ...to a metronome\n"
          "  " AUDIAKI_NAME " --tune                      tune up before recording\n"
          "  " AUDIAKI_NAME " --note 'clean tone' take01.wav\n"
          "  " AUDIAKI_NAME " --info take01.wav           how did that take come "
          "out?\n"
          "  " AUDIAKI_NAME " --info session-*.wav        ...and all the others\n"
          "  " AUDIAKI_NAME " --play take01.wav           listen to it\n"
          "  " AUDIAKI_NAME " -D plughw:CARD=Box,DEV=0 -r 48000 take03.wav\n"
          "  " AUDIAKI_NAME " --visualize take01.wav --size 1080p\n"
          "  " AUDIAKI_NAME " --visualize take01.wav --style waveform\n"
          "\n"
          "Rendering a video needs ffmpeg(1) on PATH. Recording does not.\n"
          "\n"
          "Home page: " AUDIAKI_HOMEPAGE "\n",
          AUD_VIZ_DEFAULT_WIDTH, AUD_VIZ_DEFAULT_HEIGHT, AUD_VIZ_DEFAULT_FPS,
          AUD_VIZ_DEFAULT_BARS, AUD_TUNER_DEFAULT_A4);
}

void cli_print_version(FILE *out)
{
  fprintf(out, AUDIAKI_NAME " " AUDIAKI_VERSION "\n");
}
