/* SPDX-License-Identifier: MIT */
/*
 * args.c - audiaki-gui's argv, and the help that describes it.
 *
 * Much smaller than the CLI's: the window is driven by the window, and these
 * are only the things that have to be settled before it opens. The value
 * parsing is util/parse.h, shared with the CLI so that a rate one of them
 * rejects is not one the other passes down to the driver.
 */
#include "gui/app.h"

#include "gui/viz.h"

#include "backend/backend.h"
#include "take/preroll.h"
#include "util/log.h"
#include "util/parse.h"
#include "util/path.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

void app_usage(FILE *out, const app *a)
{
  fprintf(out,
          "usage: " AUDIAKI_NAME "-gui [options]\n"
          "\n"
          "  -D, --device NAME    capture device (default: %s)\n"
          "  -b, --backend NAME   auto, pipewire or alsa (default: auto)\n"
          "  -r, --rate HZ        sample rate (default: %u)\n"
          "  -c, --channels N     channel count (default: %u)\n"
          "  -o, --take PREFIX    take name prefix (default: %s)\n"
          "      --dir FOLDER     keep takes in FOLDER (default: the config\n"
          "                       file's take_dir, or here)\n"
          "      --no-dialog      do not ask where a take should be kept when\n"
          "                       it stops; leave it where it was recorded\n"
          "  -s, --style NAME     visualiser style (default: %s)\n"
          "  -V, --video          also render an MP4 of the visualiser\n"
          "      --video-silent   render that MP4 without the take's audio\n"
          "      --video-size WxH video size (default: %ux%u, or 720p/1080p/...)\n"
          "      --video-fps N    video frame rate (default: %u)\n"
          "      --preroll SECS   keep SECS of audio while idle, so a take\n"
          "                       starts that far before Record was pressed\n"
          "  -M, --monitor        start with playback monitoring on\n"
          "  -v, --verbose        log device negotiation to the terminal\n"
          "  -h, --help           show this and exit\n"
          "\n"
          "styles: bars, mirror, radial, scope, waterfall, tuner\n"
          "\n"
          "Takes are numbered from the prefix, so recording never overwrites\n"
          "an existing file and there is no --force to get wrong.\n"
          "\n"
          "Video is rendered from the finished take when recording stops, so it\n"
          "needs ffmpeg on PATH. The audio WAV is written either way.\n"
          "\n"
          "keys: space record or pause, S stop, M monitor, V style, 1-6 a style\n"
          "      outright, F fullscreen, ? the list of them in the window\n",
          a->cfg.device, a->cfg.rate, a->cfg.channels, a->prefix,
          aud_viz_mode_name((aud_viz_mode)a->style_selected), a->video_width,
          a->video_height, a->video_fps);
}

/* Returns 0 to carry on, or a process exit code to stop with. */
int app_parse_args(app *a, int argc, char **argv)
{
  for (int i = 1; i < argc; i++)
  {
    const char *arg = argv[i];
    const char *value = NULL;

    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
    {
      app_usage(stdout, a);
      return -1;
    }
    if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0)
    {
      aud_log_set_level(AUD_LOG_VERBOSE);
      continue;
    }
    if (strcmp(arg, "-M") == 0 || strcmp(arg, "--monitor") == 0)
    {
      a->start_monitor = 1;
      continue;
    }
    if (strcmp(arg, "-V") == 0 || strcmp(arg, "--video") == 0)
    {
      a->want_video = 1;
      continue;
    }
    if (strcmp(arg, "--video-silent") == 0)
    {
      a->want_video_audio = 0;
      continue;
    }
    if (strcmp(arg, "--no-dialog") == 0)
    {
      a->want_dialog = 0;
      continue;
    }

    /* everything below takes a value */
    if (i + 1 >= argc)
    {
      aud_error("%s needs a value", arg);
      return 2;
    }
    value = argv[++i];

    if (strcmp(arg, "-D") == 0 || strcmp(arg, "--device") == 0)
    {
      a->cfg.device = value;
    }
    else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--rate") == 0)
    {
      if (parse_uint(value, AUD_RATE_MIN, AUD_RATE_MAX, &a->cfg.rate) != 0)
      {
        aud_error("bad sample rate '%s' (%u to %u Hz)", value, AUD_RATE_MIN,
                  AUD_RATE_MAX);
        return 2;
      }
    }
    else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--channels") == 0)
    {
      if (parse_uint(value, AUD_CHANNELS_MIN, AUD_CHANNELS_MAX, &a->cfg.channels) != 0)
      {
        aud_error("bad channel count '%s' (%u to %u)", value, AUD_CHANNELS_MIN,
                  AUD_CHANNELS_MAX);
        return 2;
      }
    }
    else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--take") == 0)
    {
      snprintf(a->prefix, sizeof(a->prefix), "%s", value);
    }
    /* expanded here for the same reason the CLI expands it: a quoted '~' never
     * reached the shell, and one out of a config file never went near it */
    else if (strcmp(arg, "--dir") == 0)
    {
      if (aud_path_expand(a->take_dir, sizeof(a->take_dir), value) != 0)
      {
        aud_error("cannot work out where '%s' is", value);
        return 2;
      }
    }
    else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--style") == 0)
    {
      aud_viz_mode mode;

      if (aud_viz_mode_from_name(value, &mode) != 0)
      {
        aud_error("unknown style '%s'", value);
        aud_info("styles: bars, mirror, radial, scope, waterfall, tuner");
        return 2;
      }
      a->style_selected = (int)mode;
    }
    else if (strcmp(arg, "--video-size") == 0)
    {
      if (parse_size(value, AUD_RENDER_MIN_SIZE, AUD_RENDER_MAX_SIZE, &a->video_width,
                     &a->video_height) != 0)
      {
        aud_error("bad video size '%s'", value);
        aud_info("give it as WxH, or as 720p, 1080p, 1440p or 2160p");
        return 2;
      }
    }
    else if (strcmp(arg, "--preroll") == 0 || strcmp(arg, "--pre-roll") == 0)
    {
      if (parse_duration(value, &a->cfg.preroll) != 0 || a->cfg.preroll < 0.0 ||
          a->cfg.preroll > AUD_PREROLL_MAX_SECONDS)
      {
        aud_error("bad pre-roll '%s' (seconds, up to %.0f)", value,
                  AUD_PREROLL_MAX_SECONDS);
        return 2;
      }
    }
    else if (strcmp(arg, "--video-fps") == 0)
    {
      if (parse_uint(value, 1u, 240u, &a->video_fps) != 0)
      {
        aud_error("bad video frame rate '%s' (1 to 240)", value);
        return 2;
      }
    }
    else if (strcmp(arg, "-b") == 0 || strcmp(arg, "--backend") == 0)
    {
      if (aud_backend_parse(value, &a->backend) != 0)
      {
        aud_error("unknown backend '%s'", value);
        aud_info("backends: auto, pipewire, alsa");
        return 2;
      }
    }
    else
    {
      aud_error("unknown option '%s'", arg);
      app_usage(stderr, a);
      return 2;
    }
  }

  return 0;
}