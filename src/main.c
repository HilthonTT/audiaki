/* SPDX-License-Identifier: MIT */
/*
 * audiaki - a small capture-to-WAV recorder.
 *
 * Written for a Sonicake Smart Box (QME-20) guitar interface, but it works with
 * any capture device, through either ALSA or PipeWire.
 *
 * Assumes a little-endian host (x86, ARM LE). WAV is little-endian, so a
 * big-endian build would need the sample bytes swapped before writing.
 */
#include "backend.h"
#include "cli.h"
#include "device.h"
#include "info.h"
#include "log.h"
#include "play.h"
#include "recorder.h"
#include "signals.h"
#include "take.h"
#include "tune.h"
#include "visualize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int run_record(const aud_options *opts)
{
  aud_device_config cfg;
  aud_device dev;
  aud_recorder_options rec_opts;
  char take_path[4096];
  const char *output = opts->output_path;
  int rc;

  if (opts->take_prefix != NULL)
  {
    if (aud_take_next(take_path, sizeof(take_path), opts->take_prefix) != 0)
    {
      aud_error("cannot pick a take name from '%s'", opts->take_prefix);
      aud_info("the prefix may be too long, or the first %u takes may all exist",
               AUD_TAKE_MAX_NUMBER);
      return EXIT_FAILURE;
    }
    output = take_path;
    aud_info("recording %s", output);
  }
  /*
   * Fail before claiming the device, so a mistyped filename does not leave the
   * user staring at device warnings. wav_open() still does the authoritative,
   * race-free check when it creates the file.
   */
  else if (!opts->overwrite && access(output, F_OK) == 0)
  {
    aud_error("%s already exists (pass --force to overwrite)", output);
    return EXIT_FAILURE;
  }

  aud_device_config_defaults(&cfg);
  cfg.name = opts->device;
  cfg.rate = opts->rate;
  cfg.channels = opts->channels;
  cfg.format = opts->format;
  cfg.period_frames = opts->period_frames;
  cfg.periods = opts->periods;

  if (aud_recorder_install_signals() != 0)
  {
    aud_perror("cannot install signal handlers");
    return EXIT_FAILURE;
  }

  if (aud_device_open_capture(&dev, &cfg) != 0)
  {
    return EXIT_FAILURE;
  }

  rec_opts.output_path = output;
  rec_opts.duration = opts->duration;
  /* a --take name is free by construction, so it never needs clobbering */
  rec_opts.overwrite = opts->take_prefix != NULL ? 0 : opts->overwrite;
  rec_opts.show_meter = opts->show_meter;
  rec_opts.show_spectrum = opts->show_spectrum;
  rec_opts.preroll = opts->preroll;
  rec_opts.monitor = opts->monitor;
  rec_opts.monitor_device = opts->monitor_device;
  rec_opts.monitor_gain = (float)opts->monitor_gain;

  rc = aud_recorder_run(&dev, &rec_opts, NULL);
  aud_device_close(&dev);

  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_tune(const aud_options *opts)
{
  aud_device_config cfg;
  aud_device dev;
  aud_tune_options tune_opts;
  int rc;

  aud_device_config_defaults(&cfg);
  cfg.name = opts->device;
  cfg.rate = opts->rate;
  cfg.channels = opts->channels;
  cfg.format = opts->format;
  cfg.period_frames = opts->period_frames;
  cfg.periods = opts->periods;

  if (aud_signals_install_stop() != 0)
  {
    aud_perror("cannot install signal handlers");
    return EXIT_FAILURE;
  }

  if (aud_device_open_capture(&dev, &cfg) != 0)
  {
    return EXIT_FAILURE;
  }

  tune_opts.a4_hz = opts->a4_hz;
  tune_opts.show_meter = opts->show_meter;

  rc = aud_tune_run(&dev, &tune_opts);
  aud_device_close(&dev);

  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_visualize(const aud_options *opts)
{
  aud_visualize_options viz;
  char derived[4096];

  aud_visualize_defaults(&viz);
  viz.input_path = opts->input_path;
  viz.output_path = opts->output_path;
  viz.width = opts->viz_width;
  viz.height = opts->viz_height;
  viz.fps = opts->viz_fps;
  viz.bars = opts->viz_bars;
  viz.style = opts->viz_style;

  /* default output: the input with its extension replaced by .mp4 */
  if (viz.output_path == NULL)
  {
    if (aud_take_with_extension(derived, sizeof(derived), opts->input_path, ".mp4") != 0)
    {
      aud_error("input path is too long to derive an output name from");
      aud_info("pass the video name with -o");
      return EXIT_FAILURE;
    }
    viz.output_path = derived;
  }

  if (strcmp(viz.input_path, viz.output_path) == 0)
  {
    aud_error("%s is both the input and the output", viz.input_path);
    return EXIT_FAILURE;
  }

  /* the renderer overwrites, so this check is the one that enforces --force */
  if (!opts->overwrite && access(viz.output_path, F_OK) == 0)
  {
    aud_error("%s already exists (pass --force to overwrite)", viz.output_path);
    return EXIT_FAILURE;
  }

  if (aud_signals_install_stop() != 0)
  {
    aud_perror("cannot install signal handlers");
    return EXIT_FAILURE;
  }

  return aud_visualize_render(&viz) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_play(const aud_options *opts)
{
  aud_play_options play;

  play.input_path = opts->input_path;
  /*
   * The default output, unless an output was actually named. -D otherwise
   * defaults to a capture device, and $AUDIAKI_DEVICE is one by definition;
   * either would be handed to the playback side as though it could play.
   */
  play.device = opts->device_explicit ? opts->device : NULL;
  play.duration = opts->duration;
  play.show_meter = opts->show_meter;
  play.show_spectrum = opts->show_spectrum;

  if (aud_signals_install_stop() != 0)
  {
    aud_perror("cannot install signal handlers");
    return EXIT_FAILURE;
  }

  return aud_play_run(&play) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_info(const aud_options *opts)
{
  aud_info_report report;

  if (aud_info_analyse(opts->input_path, &report) != 0)
  {
    return EXIT_FAILURE;
  }

  if (opts->json)
  {
    aud_info_print_json(stdout, opts->input_path, &report);
  }
  else
  {
    aud_info_print(stdout, opts->input_path, &report);
  }

  return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
  aud_options opts;
  int rc = cli_parse(argc, argv, &opts);

  if (rc != 0)
  {
    return rc;
  }

  aud_log_set_level(opts.log_level);

  /*
   * Only the commands that open something choose a backend. --info and
   * --visualize read a file off disk, and connecting to a sound server to do
   * that would make a machine without one fail at a job that never needed it.
   */
  switch (opts.command)
  {
  case AUD_CMD_LIST:
  case AUD_CMD_PROBE:
  case AUD_CMD_TUNE:
  case AUD_CMD_PLAY:
  case AUD_CMD_RECORD:
    if (aud_backend_select(opts.backend) != 0)
    {
      return EXIT_FAILURE;
    }
    break;
  default:
    break;
  }

  switch (opts.command)
  {
  case AUD_CMD_HELP:
    cli_print_usage(stdout);
    return EXIT_SUCCESS;
  case AUD_CMD_VERSION:
    cli_print_version(stdout);
    return EXIT_SUCCESS;
  case AUD_CMD_LIST:
    return aud_device_list(opts.json) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  case AUD_CMD_PROBE:
    return aud_device_probe(opts.device, opts.json) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  case AUD_CMD_VISUALIZE:
    return run_visualize(&opts);
  case AUD_CMD_INFO:
    return run_info(&opts);
  case AUD_CMD_PLAY:
    return run_play(&opts);
  case AUD_CMD_TUNE:
    return run_tune(&opts);
  case AUD_CMD_RECORD:
  default:
    return run_record(&opts);
  }
}
