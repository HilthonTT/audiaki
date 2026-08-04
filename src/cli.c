/* SPDX-License-Identifier: MIT */
#include "cli.h"

#include "parse.h"
#include "spectrum.h"
#include "version.h"
#include "visualize.h"

#include <getopt.h>
#include <stdlib.h>
#include <string.h>

/* sanity bounds - anything outside these is a typo rather than a request */
#define RATE_MIN 4000u
#define RATE_MAX 768000u
#define CHANNELS_MIN 1u
#define CHANNELS_MAX 64u
#define PERIOD_MIN 32u
#define PERIOD_MAX 1048576u
#define PERIODS_MIN 2u
#define PERIODS_MAX 64u
#define VIZ_DIM_MIN 64u
#define VIZ_DIM_MAX 7680u
#define VIZ_FPS_MIN 1u
#define VIZ_FPS_MAX 240u

/* defaults mirrored from device.h without pulling in <alsa/asoundlib.h> */
#define CLI_DEFAULT_DEVICE "default"
#define CLI_DEFAULT_RATE 44100u
#define CLI_DEFAULT_CHANNELS 2u
#define CLI_DEFAULT_PERIOD_FRAMES 1024u
#define CLI_DEFAULT_PERIODS 4u

enum
{
  OPT_NO_METER = 1000,
  OPT_SPECTRUM,
  OPT_VISUALIZE,
  OPT_SIZE,
  OPT_FPS,
  OPT_BARS,
};

static const struct option long_options[] = {
    {"device", required_argument, NULL, 'D'},
    {"rate", required_argument, NULL, 'r'},
    {"channels", required_argument, NULL, 'c'},
    {"format", required_argument, NULL, 'f'},
    {"duration", required_argument, NULL, 't'},
    {"period", required_argument, NULL, 'p'},
    {"periods", required_argument, NULL, 'n'},
    {"output", required_argument, NULL, 'o'},
    {"force", no_argument, NULL, 'y'},
    {"quiet", no_argument, NULL, 'q'},
    {"verbose", no_argument, NULL, 'v'},
    {"no-meter", no_argument, NULL, OPT_NO_METER},
    {"spectrum", no_argument, NULL, OPT_SPECTRUM},
    {"visualize", required_argument, NULL, OPT_VISUALIZE},
    {"visualise", required_argument, NULL, OPT_VISUALIZE},
    {"size", required_argument, NULL, OPT_SIZE},
    {"fps", required_argument, NULL, OPT_FPS},
    {"bars", required_argument, NULL, OPT_BARS},
    {"list", no_argument, NULL, 'l'},
    {"probe", no_argument, NULL, 'P'},
    {"help", no_argument, NULL, 'h'},
    {"version", no_argument, NULL, 'V'},
    {NULL, 0, NULL, 0},
};

void cli_defaults(aud_options *opts)
{
  const char *env_device = getenv("AUDIAKI_DEVICE");

  memset(opts, 0, sizeof(*opts));
  opts->command = AUD_CMD_RECORD;
  opts->device =
      (env_device != NULL && *env_device != '\0') ? env_device : CLI_DEFAULT_DEVICE;
  opts->output_path = NULL;
  opts->rate = CLI_DEFAULT_RATE;
  opts->channels = CLI_DEFAULT_CHANNELS;
  opts->period_frames = CLI_DEFAULT_PERIOD_FRAMES;
  opts->periods = CLI_DEFAULT_PERIODS;
  opts->format = AUD_FORMAT_UNKNOWN;
  opts->duration = 0.0;
  opts->overwrite = 0;
  opts->show_meter = 1;
  opts->show_spectrum = 0;
  opts->input_path = NULL;
  opts->viz_width = AUD_VIZ_DEFAULT_WIDTH;
  opts->viz_height = AUD_VIZ_DEFAULT_HEIGHT;
  opts->viz_fps = AUD_VIZ_DEFAULT_FPS;
  opts->viz_bars = AUD_VIZ_DEFAULT_BARS;
  opts->log_level = AUD_LOG_NORMAL;
}

void cli_print_usage(FILE *out)
{
  fprintf(out,
          "usage: " AUDIAKI_NAME " [options] <output.wav>\n"
          "       " AUDIAKI_NAME " --visualize <input.wav> [-o output.mp4]\n"
          "       " AUDIAKI_NAME " --probe [-D device]\n"
          "       " AUDIAKI_NAME " --list\n"
          "\n"
          "Record an ALSA capture device straight to a PCM WAV file, and turn a\n"
          "recording into a spectrum visualiser video.\n"
          "\n"
          "Recording options:\n"
          "  -D, --device NAME     ALSA device (default: %s, $AUDIAKI_DEVICE)\n"
          "  -r, --rate HZ         sample rate (default: %u)\n"
          "  -c, --channels N      channel count (default: %u)\n"
          "  -f, --format NAME     s16_le, s24_3le, s24_le or s32_le\n"
          "                        (default: best the device offers)\n"
          "  -t, --duration SPEC   stop after SS, MM:SS or HH:MM:SS\n"
          "  -p, --period FRAMES   period size (default: %u)\n"
          "  -n, --periods N       periods per buffer (default: %u)\n"
          "      --spectrum        show live spectrum bars instead of the peak bar\n"
          "      --no-meter        do not draw anything while recording\n"
          "\n"
          "Visualiser options:\n"
          "      --visualize FILE  render FILE (a WAV) to a video and exit\n"
          "  -o, --output FILE     video to write (default: input with .mp4)\n"
          "      --size SPEC       WxH, or 480p/720p/1080p/1440p/2160p\n"
          "                        (default: %ux%u)\n"
          "      --fps N           video frame rate (default: %u)\n"
          "      --bars N          number of spectrum bars (default: %u)\n"
          "\n"
          "Common options:\n"
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
          "  " AUDIAKI_NAME " -D plughw:CARD=Box,DEV=0 -r 48000 take03.wav\n"
          "  " AUDIAKI_NAME " --visualize take01.wav --size 1080p\n"
          "\n"
          "Rendering a video needs ffmpeg(1) on PATH. Recording does not.\n"
          "\n"
          "Home page: " AUDIAKI_HOMEPAGE "\n",
          CLI_DEFAULT_DEVICE, CLI_DEFAULT_RATE, CLI_DEFAULT_CHANNELS,
          CLI_DEFAULT_PERIOD_FRAMES, CLI_DEFAULT_PERIODS, AUD_VIZ_DEFAULT_WIDTH,
          AUD_VIZ_DEFAULT_HEIGHT, AUD_VIZ_DEFAULT_FPS, AUD_VIZ_DEFAULT_BARS);
}

void cli_print_version(FILE *out)
{
  fprintf(out, AUDIAKI_NAME " " AUDIAKI_VERSION "\n");
}

static void bad_value(const char *option, const char *value, const char *expected)
{
  aud_error("invalid value '%s' for %s (expected %s)", value, option, expected);
}

int cli_parse(int argc, char **argv, aud_options *opts)
{
  int opt;

  cli_defaults(opts);

  /* leading ':' -> report a missing argument as ':' instead of '?' */
  while ((opt = getopt_long(argc, argv, ":D:r:c:f:t:p:n:o:yqvlPhV", long_options,
                            NULL)) != -1)
  {
    switch (opt)
    {
    case 'D':
      opts->device = optarg;
      break;
    case 'r':
      if (parse_uint(optarg, RATE_MIN, RATE_MAX, &opts->rate) != 0)
      {
        bad_value("--rate", optarg, "a sample rate in Hz");
        return CLI_EXIT_USAGE;
      }
      break;
    case 'c':
      if (parse_uint(optarg, CHANNELS_MIN, CHANNELS_MAX, &opts->channels) != 0)
      {
        bad_value("--channels", optarg, "1..64");
        return CLI_EXIT_USAGE;
      }
      break;
    case 'f':
      opts->format = aud_format_from_name(optarg);
      if (opts->format == AUD_FORMAT_UNKNOWN)
      {
        bad_value("--format", optarg, "s16_le, s24_3le, s24_le or s32_le");
        return CLI_EXIT_USAGE;
      }
      break;
    case 't':
      if (parse_duration(optarg, &opts->duration) != 0 || opts->duration <= 0.0)
      {
        bad_value("--duration", optarg, "SS, MM:SS or HH:MM:SS");
        return CLI_EXIT_USAGE;
      }
      break;
    case 'p':
      if (parse_uint(optarg, PERIOD_MIN, PERIOD_MAX, &opts->period_frames) != 0)
      {
        bad_value("--period", optarg, "32..1048576 frames");
        return CLI_EXIT_USAGE;
      }
      break;
    case 'n':
      if (parse_uint(optarg, PERIODS_MIN, PERIODS_MAX, &opts->periods) != 0)
      {
        bad_value("--periods", optarg, "2..64");
        return CLI_EXIT_USAGE;
      }
      break;
    case 'o':
      opts->output_path = optarg;
      break;
    case 'y':
      opts->overwrite = 1;
      break;
    case 'q':
      opts->log_level = AUD_LOG_QUIET;
      opts->show_meter = 0;
      break;
    case 'v':
      opts->log_level = AUD_LOG_VERBOSE;
      break;
    case OPT_NO_METER:
      opts->show_meter = 0;
      break;
    case OPT_SPECTRUM:
      opts->show_spectrum = 1;
      break;
    case OPT_VISUALIZE:
      opts->command = AUD_CMD_VISUALIZE;
      opts->input_path = optarg;
      break;
    case OPT_SIZE:
      if (parse_size(optarg, VIZ_DIM_MIN, VIZ_DIM_MAX, &opts->viz_width,
                     &opts->viz_height) != 0)
      {
        bad_value("--size", optarg, "WxH in 64..7680, or 720p/1080p/1440p/2160p");
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_FPS:
      if (parse_uint(optarg, VIZ_FPS_MIN, VIZ_FPS_MAX, &opts->viz_fps) != 0)
      {
        bad_value("--fps", optarg, "1..240");
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_BARS:
      if (parse_uint(optarg, AUD_SPECTRUM_MIN_BANDS, AUD_SPECTRUM_MAX_BANDS,
                     &opts->viz_bars) != 0)
      {
        bad_value("--bars", optarg, "4..512");
        return CLI_EXIT_USAGE;
      }
      break;
    case 'l':
      opts->command = AUD_CMD_LIST;
      break;
    case 'P':
      opts->command = AUD_CMD_PROBE;
      break;
    case 'h':
      opts->command = AUD_CMD_HELP;
      return 0;
    case 'V':
      opts->command = AUD_CMD_VERSION;
      return 0;
    case ':':
      aud_error("option '%s' requires an argument", argv[optind - 1]);
      return CLI_EXIT_USAGE;
    case '?':
    default:
      if (optopt != 0)
        aud_error("unknown option '-%c'", optopt);
      else
        aud_error("unknown option '%s'", argv[optind - 1]);
      aud_info("run '" AUDIAKI_NAME " --help' for usage");
      return CLI_EXIT_USAGE;
    }
  }

  if (opts->command == AUD_CMD_VISUALIZE)
  {
    if (optind < argc)
    {
      aud_error("unexpected argument '%s' (the input comes from --visualize, "
                "the output from -o)",
                argv[optind]);
      return CLI_EXIT_USAGE;
    }
    /*
     * libx264 with yuv420p subsamples by two in both directions, so an odd
     * dimension fails inside ffmpeg with a message about the pixel format.
     * Catching it here says what is actually wrong.
     */
    if ((opts->viz_width % 2u) != 0 || (opts->viz_height % 2u) != 0)
    {
      aud_error("--size %ux%u: both dimensions must be even", opts->viz_width,
                opts->viz_height);
      return CLI_EXIT_USAGE;
    }
    return 0;
  }

  if (opts->command != AUD_CMD_RECORD)
  {
    if (optind < argc)
    {
      aud_error("unexpected argument '%s'", argv[optind]);
      return CLI_EXIT_USAGE;
    }
    return 0;
  }

  if (optind < argc)
  {
    /* -o is accepted here too, but naming the file twice is surely a mistake */
    if (opts->output_path != NULL)
    {
      aud_error("output file given twice ('-o %s' and '%s')", opts->output_path,
                argv[optind]);
      return CLI_EXIT_USAGE;
    }
    opts->output_path = argv[optind++];

    if (optind < argc)
    {
      aud_error("unexpected argument '%s' (only one output file is supported)",
                argv[optind]);
      return CLI_EXIT_USAGE;
    }
  }
  else if (opts->output_path == NULL)
  {
    aud_error("no output file given");
    aud_info("run '" AUDIAKI_NAME " --help' for usage");
    return CLI_EXIT_USAGE;
  }

  return 0;
}
