/* SPDX-License-Identifier: MIT */
/*
 * audiaki - a small ALSA capture-to-WAV recorder.
 *
 * Written for a Sonicake Smart Box (QME-20) guitar interface, but it works
 * with any ALSA capture device.
 *
 * Assumes a little-endian host (x86, ARM LE). WAV is little-endian, so a
 * big-endian build would need the sample bytes swapped before writing.
 */
#include "cli.h"
#include "device.h"
#include "log.h"
#include "recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int run_record(const aud_options *opts)
{
  aud_device_config cfg;
  aud_device dev;
  aud_recorder_options rec_opts;
  int rc;

  /*
   * Fail before claiming the device, so a mistyped filename does not leave the
   * user staring at device warnings. wav_open() still does the authoritative,
   * race-free check when it creates the file.
   */
  if (!opts->overwrite && access(opts->output_path, F_OK) == 0)
  {
    aud_error("%s already exists (pass --force to overwrite)", opts->output_path);
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
    return EXIT_FAILURE;

  rec_opts.output_path = opts->output_path;
  rec_opts.duration = opts->duration;
  rec_opts.overwrite = opts->overwrite;
  rec_opts.show_meter = opts->show_meter;

  rc = aud_recorder_run(&dev, &rec_opts, NULL);
  aud_device_close(&dev);

  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
  aud_options opts;
  int rc = cli_parse(argc, argv, &opts);

  if (rc != 0)
    return rc;

  aud_log_set_level(opts.log_level);

  switch (opts.command)
  {
  case AUD_CMD_HELP:
    cli_print_usage(stdout);
    return EXIT_SUCCESS;
  case AUD_CMD_VERSION:
    cli_print_version(stdout);
    return EXIT_SUCCESS;
  case AUD_CMD_LIST:
    return aud_device_list() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  case AUD_CMD_PROBE:
    return aud_device_probe(opts.device) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  case AUD_CMD_RECORD:
  default:
    return run_record(&opts);
  }
}
