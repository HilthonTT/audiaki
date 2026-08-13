/* SPDX-License-Identifier: MIT */
#include "cli/cli.h"

#include "audio/click.h"
#include "audio/format.h"
#include "audio/spectrum.h"
#include "audio/tuner.h"
#include "backend/device.h"
#include "backend/monitor.h"
#include "media/visualize.h"
#include "take/latency.h"
#include "take/meta.h"
#include "take/preroll.h"
#include "util/config.h"
#include "util/parse.h"
#include "util/path.h"
#include "version.h"

#include <getopt.h>
#include <stdlib.h>
#include <string.h>

/* sanity bounds - anything outside these is a typo rather than a request.
 * The rate and channel bounds live in parse.h, which the desktop app's own
 * argument handling shares. */
#define PERIOD_MIN 32u
#define PERIOD_MAX 1048576u
#define PERIODS_MIN 2u
#define PERIODS_MAX 64u
#define VIZ_DIM_MIN 64u
#define VIZ_DIM_MAX 7680u
#define VIZ_FPS_MIN 1u
#define VIZ_FPS_MAX 240u
/* silence to +6 dB, the range the desktop app's monitoring slider covers */
#define MONITOR_GAIN_MIN 0.0
#define MONITOR_GAIN_MAX 2.0

/*
 * 30 days. Not a limit anyone will meet: the 4 GB WAV ceiling stops even
 * 4 kHz mono - the narrowest stream this accepts - after about six days, so
 * the recorder always runs out of file before it runs out of clock.
 *
 * It is here because --duration reaches cmd/record.c as duration * rate cast to
 * a frame count, and a double too large for that conversion is undefined
 * behaviour rather than a long recording. Unbounded, '-t 1e308' does not
 * record for a very long time; it converts to zero and records one frame.
 */
#define DURATION_MAX 2592000.0

/* The capture defaults are device.h's own: it carries no audio system with it,
 * so both this and usage.c quote it directly rather than through an alias. */

enum
{
  OPT_NO_METER = 1000,
  OPT_SPECTRUM,
  OPT_VISUALIZE,
  OPT_SIZE,
  OPT_FPS,
  OPT_BARS,
  OPT_STYLE,
  OPT_INFO,
  OPT_TAKE,
  OPT_JSON,
  OPT_TUNE,
  OPT_CALIBRATE,
  OPT_A4,
  OPT_TUNE_MIN,
  OPT_TUNE_MAX,
  OPT_BACKEND,
  OPT_PREROLL,
  OPT_PLAY,
  OPT_SHUFFLE,
  OPT_REPEAT,
  OPT_REPEAT_ONE,
  OPT_MONITOR_DEVICE,
  OPT_MONITOR_GAIN,
  OPT_GAIN,
  OPT_NOTE,
  OPT_NO_METADATA,
  OPT_CLICK,
  OPT_CLICK_BEATS,
  OPT_CLICK_SUBDIV,
  OPT_CLICK_GAIN,
  OPT_LATENCY,
  OPT_CHANNEL,
  OPT_DIR,
  OPT_PROMPT,
  OPT_NO_PROMPT,
  OPT_RENDER,
  OPT_BITS,
  OPT_STEMS,
};

static const struct option long_options[] = {
    {"device", required_argument, NULL, 'D'},
    {"rate", required_argument, NULL, 'r'},
    {"channels", required_argument, NULL, 'c'},
    {"channel", required_argument, NULL, OPT_CHANNEL},
    {"format", required_argument, NULL, 'f'},
    {"duration", required_argument, NULL, 't'},
    {"period", required_argument, NULL, 'p'},
    {"periods", required_argument, NULL, 'n'},
    {"output", required_argument, NULL, 'o'},
    {"dir", required_argument, NULL, OPT_DIR},
    {"prompt", no_argument, NULL, OPT_PROMPT},
    {"no-prompt", no_argument, NULL, OPT_NO_PROMPT},
    {"force", no_argument, NULL, 'y'},
    {"quiet", no_argument, NULL, 'q'},
    {"verbose", no_argument, NULL, 'v'},
    {"note", required_argument, NULL, OPT_NOTE},
    {"no-metadata", no_argument, NULL, OPT_NO_METADATA},
    {"monitor", no_argument, NULL, 'M'},
    {"monitor-device", required_argument, NULL, OPT_MONITOR_DEVICE},
    {"monitor-gain", required_argument, NULL, OPT_MONITOR_GAIN},
    {"gain", required_argument, NULL, OPT_GAIN},
    {"input-gain", required_argument, NULL, OPT_GAIN},
    {"click", required_argument, NULL, OPT_CLICK},
    {"metronome", required_argument, NULL, OPT_CLICK},
    {"click-beats", required_argument, NULL, OPT_CLICK_BEATS},
    {"click-subdiv", required_argument, NULL, OPT_CLICK_SUBDIV},
    {"click-gain", required_argument, NULL, OPT_CLICK_GAIN},
    {"latency", required_argument, NULL, OPT_LATENCY},
    {"no-meter", no_argument, NULL, OPT_NO_METER},
    {"spectrum", no_argument, NULL, OPT_SPECTRUM},
    {"visualize", required_argument, NULL, OPT_VISUALIZE},
    {"visualise", required_argument, NULL, OPT_VISUALIZE},
    {"size", required_argument, NULL, OPT_SIZE},
    {"fps", required_argument, NULL, OPT_FPS},
    {"bars", required_argument, NULL, OPT_BARS},
    {"style", required_argument, NULL, OPT_STYLE},
    {"info", required_argument, NULL, OPT_INFO},
    {"play", required_argument, NULL, OPT_PLAY},
    {"shuffle", no_argument, NULL, OPT_SHUFFLE},
    {"repeat", no_argument, NULL, OPT_REPEAT},
    {"repeat-one", no_argument, NULL, OPT_REPEAT_ONE},
    {"render", required_argument, NULL, OPT_RENDER},
    {"bits", required_argument, NULL, OPT_BITS},
    {"stems", no_argument, NULL, OPT_STEMS},
    {"take", required_argument, NULL, OPT_TAKE},
    {"preroll", required_argument, NULL, OPT_PREROLL},
    {"pre-roll", required_argument, NULL, OPT_PREROLL},
    {"tune", no_argument, NULL, OPT_TUNE},
    {"calibrate", no_argument, NULL, OPT_CALIBRATE},
    {"a4", required_argument, NULL, OPT_A4},
    {"tune-min", required_argument, NULL, OPT_TUNE_MIN},
    {"tune-max", required_argument, NULL, OPT_TUNE_MAX},
    {"backend", required_argument, NULL, OPT_BACKEND},
    {"json", no_argument, NULL, OPT_JSON},
    {"list", no_argument, NULL, 'l'},
    {"probe", no_argument, NULL, 'P'},
    {"help", no_argument, NULL, 'h'},
    {"version", no_argument, NULL, 'V'},
    {NULL, 0, NULL, 0},
};

void cli_defaults(aud_options *opts)
{
  const char *env_device = getenv("AUDIAKI_DEVICE");
  const char *env_backend = getenv("AUDIAKI_BACKEND");
  aud_config cfg;

  memset(opts, 0, sizeof(*opts));
  opts->command = AUD_CMD_RECORD;
  opts->device =
      (env_device != NULL && *env_device != '\0') ? env_device : AUD_DEFAULT_DEVICE;
  opts->output_path = NULL;
  opts->rate = AUD_DEFAULT_RATE;
  opts->channels = AUD_DEFAULT_CHANNELS;
  opts->channel = 0;
  opts->period_frames = AUD_DEFAULT_PERIOD_FRAMES;
  opts->periods = AUD_DEFAULT_PERIODS;
  opts->format = AUD_FORMAT_UNKNOWN;
  opts->duration = 0.0;
  opts->preroll = 0.0;
  opts->overwrite = 0;
  opts->show_meter = 1;
  opts->show_spectrum = 0;
  opts->monitor = 0;
  opts->monitor_device = NULL;
  opts->monitor_gain = 1.0;
  opts->input_gain = 1.0; /* the samples the device delivered, untouched */
  opts->click_bpm = 0.0;
  opts->click_beats = AUD_CLICK_DEFAULT_BEATS;
  opts->click_subdiv = AUD_CLICK_DEFAULT_SUBDIV;
  opts->click_gain = AUD_CLICK_DEFAULT_GAIN;
  opts->metadata = 1;
  opts->note = NULL;
  opts->shuffle = 0;
  opts->repeat = AUD_REPEAT_NONE;
  opts->extra_inputs = NULL;
  opts->extra_input_count = 0;
  opts->input_path = NULL;
  opts->take_prefix = NULL;
  opts->viz_width = AUD_VIZ_DEFAULT_WIDTH;
  opts->viz_height = AUD_VIZ_DEFAULT_HEIGHT;
  opts->viz_fps = AUD_VIZ_DEFAULT_FPS;
  opts->viz_bars = AUD_VIZ_DEFAULT_BARS;
  opts->viz_style = AUD_VIZ_STYLE_BARS;
  opts->a4_hz = AUD_TUNER_DEFAULT_A4;
  /* zero at either end means "whatever the tuner's own default is" */
  opts->tune_min_hz = 0.0;
  opts->tune_max_hz = 0.0;
  opts->json = 0;
  opts->log_level = AUD_LOG_NORMAL;

  /*
   * Read before argv rather than after it, so every one of these is something
   * an option on the command line can still say otherwise about. The file not
   * being there is the ordinary case and leaves all of it alone.
   */
  aud_config_load(&cfg);
  snprintf(opts->take_dir, sizeof(opts->take_dir), "%s", cfg.take_dir);
  opts->prompt = cfg.prompt;
  /* the same number the desktop app places an overdub by; --click leads by it */
  opts->latency_ms = cfg.latency_ms;
  if (cfg.input_gain >= 0.0)
  {
    opts->input_gain = cfg.input_gain;
  }

  /*
   * A bad $AUDIAKI_BACKEND is left at auto rather than rejected. An exported
   * variable with a typo in it would otherwise make every invocation fail,
   * including the --help that would explain the spelling.
   */
  opts->backend = AUD_BACKEND_AUTO;
  if (env_backend != NULL && *env_backend != '\0' &&
      aud_backend_parse(env_backend, &opts->backend) != 0)
  {
    aud_warn("ignoring $AUDIAKI_BACKEND=%s: expected %s", env_backend,
             aud_backend_list());
    opts->backend = AUD_BACKEND_AUTO;
  }
}

static void bad_value(const char *option, const char *value, const char *expected)
{
  aud_error("invalid value '%s' for %s (expected %s)", value, option, expected);
}

int cli_parse(int argc, char **argv, aud_options *opts)
{
  int opt;
  int click_shape = 0;          /* --click-beats or --click-gain was typed */
  int monitor_device_given = 0; /* --monitor-device was typed */
  int latency_given = 0;        /* --latency was typed */
  /*
   * Typed, as opposed to read out of the config file. The config applies to
   * every invocation, so it cannot be what makes one rejected for asking about
   * takes on a command that does not make any.
   */
  int dir_given = 0;
  int prompt_given = 0;

  cli_defaults(opts);

  /* leading ':' -> report a missing argument as ':' instead of '?' */
  while ((opt = getopt_long(argc, argv, ":D:r:c:f:t:p:n:o:MyqvlPhV", long_options,
                            NULL)) != -1)
  {
    switch (opt)
    {
    case 'D':
      opts->device = optarg;
      opts->device_explicit = 1;
      break;
    case 'r':
      if (parse_uint(optarg, AUD_RATE_MIN, AUD_RATE_MAX, &opts->rate) != 0)
      {
        bad_value("--rate", optarg, "a sample rate in Hz");
        return CLI_EXIT_USAGE;
      }
      break;
    case 'c':
      if (parse_uint(optarg, AUD_CHANNELS_MIN, AUD_CHANNELS_MAX, &opts->channels) != 0)
      {
        bad_value("--channels", optarg, "1..64");
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_CHANNEL:
      /*
       * Bounded against the widest capture audiaki accepts, not against what
       * this device offers - the device has not been opened yet, and a machine
       * with the interface unplugged should still be told that 0 is not a
       * channel number. The real check is against the negotiated count, in
       * cmd/record.c, once there is something to check against.
       */
      if (strcmp(optarg, "mix") == 0)
      {
        opts->channel = AUD_CHANNEL_MIX;
      }
      else if (parse_uint(optarg, 1u, AUD_CHANNELS_MAX, &opts->channel) != 0)
      {
        bad_value("--channel", optarg, "a channel number counting from 1, or \"mix\"");
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
      if (parse_duration(optarg, &opts->duration) != 0 || opts->duration <= 0.0 ||
          opts->duration > DURATION_MAX)
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
    /*
     * Expanded here rather than carried as typed: a shell expands '~' before
     * audiaki ever sees it, but a value quoted to stop that, or one that came
     * out of a config file, has to be expanded by someone.
     */
    case OPT_DIR:
      if (aud_path_expand(opts->take_dir, sizeof(opts->take_dir), optarg) != 0)
      {
        bad_value("--dir", optarg, "a folder to keep takes in");
        return CLI_EXIT_USAGE;
      }
      dir_given = 1;
      break;
    case OPT_PROMPT:
      opts->prompt = AUD_PROMPT_ALWAYS;
      prompt_given = 1;
      break;
    case OPT_NO_PROMPT:
      opts->prompt = AUD_PROMPT_NEVER;
      prompt_given = 1;
      break;
    case 'y':
      opts->overwrite = 1;
      break;
    case OPT_NOTE:
      if (strlen(optarg) > AUD_META_NOTE_MAX)
      {
        aud_error("--note is limited to %u characters", AUD_META_NOTE_MAX);
        return CLI_EXIT_USAGE;
      }
      opts->note = optarg;
      break;
    case OPT_NO_METADATA:
      opts->metadata = 0;
      break;
    case 'M':
      opts->monitor = 1;
      break;
    /*
     * Naming a level is asking to hear the input, since scaling it is all
     * --monitor-gain does, so it does not need -M as well; the alternative is a
     * flag that silently does nothing on its own.
     *
     * Naming an output is not the same thing once there is a metronome, which
     * plays through the same stream without the input going anywhere near it.
     * So --monitor-device only turns monitoring on when nothing else would
     * have opened the output - decided after the loop, where the tempo is
     * known whichever order the two were typed in.
     */
    case OPT_MONITOR_DEVICE:
      opts->monitor_device = optarg;
      monitor_device_given = 1;
      break;
    case OPT_MONITOR_GAIN:
      if (parse_double(optarg, MONITOR_GAIN_MIN, MONITOR_GAIN_MAX, &opts->monitor_gain) !=
          0)
      {
        bad_value("--monitor-gain", optarg, "0.0 to 2.0, where 1.0 is unchanged");
        return CLI_EXIT_USAGE;
      }
      opts->monitor = 1;
      break;
    /*
     * The one that does reach the file, which is why it turns nothing else on
     * and is spelled without "monitor" in it. See --monitor-gain above for the
     * one that does not.
     */
    case OPT_GAIN:
      if (parse_double(optarg, AUD_GAIN_MIN, AUD_GAIN_MAX, &opts->input_gain) != 0)
      {
        bad_value("--gain", optarg,
                  "0.0 to 16.0, where 1.0 is unchanged and 16.0 "
                  "is +24 dB");
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_CLICK:
      if (parse_double(optarg, AUD_CLICK_BPM_MIN, AUD_CLICK_BPM_MAX, &opts->click_bpm) !=
          0)
      {
        bad_value("--click", optarg, "a tempo in BPM, 20 to 300");
        return CLI_EXIT_USAGE;
      }
      break;
    /*
     * Neither of these implies --click, unlike the monitoring pair above: what
     * they shape is the tempo, and there is no tempo to guess. Saying so beats
     * inventing one, so the check after the loop rejects them on their own.
     */
    case OPT_CLICK_BEATS:
      if (parse_uint(optarg, 0u, AUD_CLICK_BEATS_MAX, &opts->click_beats) != 0)
      {
        bad_value("--click-beats", optarg, "beats to a bar, 0..32");
        return CLI_EXIT_USAGE;
      }
      click_shape = 1;
      break;
    case OPT_CLICK_SUBDIV:
      if (parse_uint(optarg, 1u, AUD_CLICK_SUBDIV_MAX, &opts->click_subdiv) != 0)
      {
        bad_value("--click-subdiv", optarg, "ticks to a beat, 1..8");
        return CLI_EXIT_USAGE;
      }
      click_shape = 1;
      break;
    case OPT_CLICK_GAIN:
      if (parse_double(optarg, AUD_CLICK_GAIN_MIN, AUD_CLICK_GAIN_MAX,
                       &opts->click_gain) != 0)
      {
        bad_value("--click-gain", optarg, "0.0 to 2.0, where 1.0 is full scale");
        return CLI_EXIT_USAGE;
      }
      click_shape = 1;
      break;
    case OPT_LATENCY:
      if (parse_double(optarg, 0.0, AUD_LATENCY_MAX_MS, &opts->latency_ms) != 0)
      {
        bad_value("--latency", optarg, "a round trip in milliseconds, 0 to 500");
        return CLI_EXIT_USAGE;
      }
      click_shape = 1;
      latency_given = 1;
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
    case OPT_STYLE:
      if (aud_visualize_style_from_name(optarg, &opts->viz_style) != 0)
      {
        bad_value("--style", optarg, "bars, scope or waveform");
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_INFO:
      opts->command = AUD_CMD_INFO;
      opts->input_path = optarg;
      break;
    case OPT_PLAY:
      opts->command = AUD_CMD_PLAY;
      opts->input_path = optarg;
      break;
    case OPT_SHUFFLE:
      opts->shuffle = 1;
      break;
    /*
     * The last one typed wins rather than being an error: --repeat --repeat-one
     * is somebody changing their mind on the command line, and there is only one
     * reading of it.
     */
    case OPT_REPEAT:
      opts->repeat = AUD_REPEAT_ALL;
      break;
    case OPT_REPEAT_ONE:
      opts->repeat = AUD_REPEAT_ONE;
      break;
    case OPT_RENDER:
      opts->command = AUD_CMD_RENDER;
      opts->input_path = optarg;
      break;
    case OPT_BITS:
      /*
       * The three depths edit/export.c writes. Not a free number: 8 bit WAV is
       * unsigned and 64 bit float is not what a mix is handed to anyone as.
       */
      if (parse_uint(optarg, 16u, 32u, &opts->export_bits) != 0 ||
          (opts->export_bits != 16u && opts->export_bits != 24u &&
           opts->export_bits != 32u))
      {
        bad_value("--bits", optarg, "16, 24 or 32");
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_STEMS:
      opts->export_stems = 1;
      break;
    case OPT_TAKE:
      opts->take_prefix = optarg;
      break;
    case OPT_PREROLL:
      if (parse_duration(optarg, &opts->preroll) != 0 || opts->preroll < 0.0 ||
          opts->preroll > AUD_PREROLL_MAX_SECONDS)
      {
        bad_value("--preroll", optarg, "seconds, up to 300");
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_TUNE:
      opts->command = AUD_CMD_TUNE;
      break;
    case OPT_CALIBRATE:
      opts->command = AUD_CMD_CALIBRATE;
      break;
    case OPT_A4:
      if (parse_double(optarg, AUD_TUNER_A4_MIN, AUD_TUNER_A4_MAX, &opts->a4_hz) != 0)
      {
        bad_value("--a4", optarg, "a reference pitch in Hz, 390 to 500");
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_TUNE_MIN:
      if (parse_double(optarg, AUD_TUNER_MIN_HZ_FLOOR, AUD_TUNER_MAX_HZ_CEILING,
                       &opts->tune_min_hz) != 0)
      {
        bad_value("--tune-min", optarg, "the lowest pitch to look for, in Hz");
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_TUNE_MAX:
      if (parse_double(optarg, AUD_TUNER_MIN_HZ_FLOOR, AUD_TUNER_MAX_HZ_CEILING,
                       &opts->tune_max_hz) != 0)
      {
        bad_value("--tune-max", optarg, "the highest pitch to look for, in Hz");
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_BACKEND:
      if (aud_backend_parse(optarg, &opts->backend) != 0)
      {
        bad_value("--backend", optarg, aud_backend_list());
        return CLI_EXIT_USAGE;
      }
      break;
    case OPT_JSON:
      opts->json = 1;
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
      {
        aud_error("unknown option '-%c'", optopt);
      }
      else
      {
        aud_error("unknown option '%s'", argv[optind - 1]);
      }
      aud_info("run '" AUDIAKI_NAME " --help' for usage");
      return CLI_EXIT_USAGE;
    }
  }

  /*
   * See OPT_MONITOR_DEVICE: naming the output means "play the input through
   * this one" until a metronome gives the output something else to carry, and
   * then it means "play the click through this one" and leaves the input where
   * it was. Either way the flag does something, which is the point.
   *
   * --calibrate is the third of those: the output is half of what is being
   * measured, so naming it is the whole use of the flag there, and turning
   * monitoring on as well would feed the input back into the measurement.
   */
  if (monitor_device_given && opts->click_bpm <= 0.0 &&
      opts->command != AUD_CMD_CALIBRATE)
  {
    opts->monitor = 1;
  }

  /*
   * --json describes a report, not a recording. Silently ignoring it on a
   * command that has nothing to serialise would let a script believe it was
   * getting parseable output right up until it tried to parse it.
   */
  if (opts->json && opts->command != AUD_CMD_LIST && opts->command != AUD_CMD_PROBE &&
      opts->command != AUD_CMD_INFO)
  {
    aud_error("--json only applies to --list, --probe and --info");
    return CLI_EXIT_USAGE;
  }

  if (opts->take_prefix != NULL && opts->command != AUD_CMD_RECORD)
  {
    aud_error("--take only applies when recording");
    return CLI_EXIT_USAGE;
  }

  /*
   * Both are about what happens to a take once it is made, so on a command
   * that makes none they would be quietly ignored. --visualize in particular
   * writes a file and could plausibly be thought to honour --dir; it does not,
   * and -o is how that one is placed.
   */
  if (dir_given && opts->command != AUD_CMD_RECORD)
  {
    aud_error("--dir only applies when recording");
    return CLI_EXIT_USAGE;
  }

  /*
   * The pair say whether audiaki may stop and ask a person something, so they
   * apply wherever there is a question. Recording asks where to keep the take;
   * --calibrate asks whether to write what it measured to the config file.
   */
  if (prompt_given && opts->command != AUD_CMD_RECORD &&
      opts->command != AUD_CMD_CALIBRATE)
  {
    aud_error("--prompt and --no-prompt only apply when recording or calibrating");
    return CLI_EXIT_USAGE;
  }

  /*
   * Caught before the metronome check below, which would otherwise answer a
   * question about calibration by talking about a click. --latency is the one
   * number --calibrate exists to produce, so being given it is a contradiction
   * rather than a spare option.
   */
  if (latency_given && opts->command == AUD_CMD_CALIBRATE)
  {
    aud_error("--calibrate measures the round trip; --latency is what it measures");
    return CLI_EXIT_USAGE;
  }

  if (opts->preroll > 0.0 && opts->command != AUD_CMD_RECORD)
  {
    aud_error("--preroll only applies when recording");
    return CLI_EXIT_USAGE;
  }

  /*
   * Both describe a take being made. On any other command there is no take to
   * describe, and --note in particular would be quietly discarded text.
   */
  if (opts->note != NULL && opts->command != AUD_CMD_RECORD)
  {
    aud_error("--note only applies when recording");
    return CLI_EXIT_USAGE;
  }

  if (!opts->metadata && opts->command != AUD_CMD_RECORD)
  {
    aud_error("--no-metadata only applies when recording");
    return CLI_EXIT_USAGE;
  }

  /*
   * Monitoring means hearing an input while it is being captured, so it has
   * nowhere to apply outside a recording. --play gets its own message because
   * it is already playing something, and the option to say which output is -D.
   */
  if (opts->monitor && opts->command != AUD_CMD_RECORD)
  {
    if (opts->command == AUD_CMD_PLAY)
    {
      aud_error("--play already plays through an output; name it with -D");
    }
    else
    {
      aud_error("--monitor only applies when recording");
    }
    return CLI_EXIT_USAGE;
  }

  /*
   * A metronome is something to play a take to, so like monitoring it has
   * nowhere to apply outside a recording.
   */
  if (opts->click_bpm > 0.0 && opts->command != AUD_CMD_RECORD)
  {
    aud_error("--click only applies when recording");
    return CLI_EXIT_USAGE;
  }

  /*
   * Caught here rather than left to aud_tuner_create(), which would refuse an
   * inverted range with nothing but EINVAL to say about it.
   */
  if (opts->tune_min_hz > 0.0 && opts->tune_max_hz > 0.0 &&
      opts->tune_max_hz <= opts->tune_min_hz * 2.0)
  {
    aud_error("--tune-max %.4g must be at least an octave above --tune-min %.4g",
              opts->tune_max_hz, opts->tune_min_hz);
    return CLI_EXIT_USAGE;
  }

  /*
   * Both shape a playlist, and only --play has one. Refused rather than
   * ignored, because a run that quietly did not shuffle is worse than one that
   * says why.
   */
  if ((opts->shuffle || opts->repeat != AUD_REPEAT_NONE) && opts->command != AUD_CMD_PLAY)
  {
    aud_error("--shuffle and --repeat shape a playlist; only --play has one");
    return CLI_EXIT_USAGE;
  }

  if (click_shape && opts->click_bpm <= 0.0)
  {
    aud_error("--click-beats, --click-subdiv and --click-gain shape a metronome; "
              "--click sets its tempo and turns it on");
    return CLI_EXIT_USAGE;
  }

  /*
   * Up here with the other cross-command checks rather than further down among
   * the per-command ones, because every command that is not --render has to be
   * caught by them - and the ones below return before a check placed after them
   * is ever reached. --bits with --info used to be accepted in silence for
   * exactly that reason.
   */
  if (opts->export_bits != 0 && opts->command != AUD_CMD_RENDER)
  {
    aud_error("--bits only applies to --render");
    return CLI_EXIT_USAGE;
  }

  if (opts->export_stems && opts->command != AUD_CMD_RENDER)
  {
    aud_error("--stems only applies to --render");
    return CLI_EXIT_USAGE;
  }

  /*
   * --tune writes nothing, so an output file passed with it is either a
   * mistyped recording or a file the user expects to be created. Neither is
   * what will happen, and saying so beats tuning up next to a silent surprise.
   */
  if (opts->command == AUD_CMD_TUNE)
  {
    if (optind < argc)
    {
      aud_error("unexpected argument '%s' (--tune records nothing)", argv[optind]);
      return CLI_EXIT_USAGE;
    }
    if (opts->output_path != NULL)
    {
      aud_error("--tune records nothing, so there is no output file to write");
      return CLI_EXIT_USAGE;
    }
    return 0;
  }

  /*
   * Nor does --calibrate. The one file it may write is the config file, and
   * that is asked about afterwards rather than named on the command line -
   * there is only one of it, and audiaki already knows where it is.
   */
  if (opts->command == AUD_CMD_CALIBRATE)
  {
    if (optind < argc)
    {
      aud_error("unexpected argument '%s' (--calibrate records nothing)", argv[optind]);
      return CLI_EXIT_USAGE;
    }
    if (opts->output_path != NULL)
    {
      aud_error("--calibrate records nothing, so there is no output file to write");
      return CLI_EXIT_USAGE;
    }
    return 0;
  }

  /*
   * The two commands that take more than one file, for the same reason:
   * measuring a session means measuring every take in it, and playing one back
   * means hearing them in order. A shell glob is how anyone would ask either -
   * 'audiaki --info session-*.wav' reads the first from --info and the rest
   * from here, and --play reads its playlist the same way.
   */
  if (opts->command == AUD_CMD_INFO || opts->command == AUD_CMD_PLAY)
  {
    if (opts->command == AUD_CMD_PLAY && opts->output_path != NULL)
    {
      aud_error("--play writes nothing, so there is no output file to write");
      return CLI_EXIT_USAGE;
    }
    if (optind < argc)
    {
      opts->extra_inputs = argv + optind;
      opts->extra_input_count = argc - optind;
    }
    return 0;
  }

  /*
   * A mix is written, so -o names it and everything left over is a mistake.
   * --bits and --stems belong to this one alone, and are refused for every
   * other command up with the cross-command checks.
   */
  if (opts->command == AUD_CMD_RENDER)
  {
    if (optind < argc)
    {
      aud_error("unexpected argument '%s' (the project comes from --render, "
                "the mix from -o)",
                argv[optind]);
      return CLI_EXIT_USAGE;
    }
    return 0;
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

  /* --take picks the name itself, so being handed one as well is a conflict */
  if (opts->take_prefix != NULL && opts->output_path != NULL)
  {
    aud_error("--take %s and the output file '%s' both name the take", opts->take_prefix,
              opts->output_path);
    return CLI_EXIT_USAGE;
  }

  if (opts->take_prefix == NULL && opts->output_path == NULL)
  {
    aud_error("no output file given");
    aud_info("run '" AUDIAKI_NAME " --help' for usage");
    return CLI_EXIT_USAGE;
  }

  return 0;
}
