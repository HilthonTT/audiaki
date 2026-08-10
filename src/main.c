/* SPDX-License-Identifier: MIT */
/*
 * audiaki - a small capture-to-WAV recorder.
 *
 * Written for a Sonicake Smart Box (QME-20) guitar interface, but it works with
 * any capture device, through either ALSA or PipeWire.
 *
 * Host byte order does not matter. WAV and every format a backend delivers are
 * little-endian, and util/bytes.h reads and writes them a byte at a time, so
 * the same source is correct on a big-endian machine as on x86.
 *
 * This file picks a command and nothing else. src/cli turns argv into an
 * aud_options, src/cmd carries one out; DESIGN.md has the layers under those.
 */
#include "backend/backend.h"
#include "cli/cli.h"
#include "cmd/cmd.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * Whether a command has to know which audio system it is talking to. --info and
 * --visualize read a file off disk, and connecting to a sound server to do that
 * would make a machine without one fail at a job that never needed it.
 */
static int needs_a_backend(aud_command command)
{
  switch (command)
  {
  case AUD_CMD_LIST:
  case AUD_CMD_PROBE:
  case AUD_CMD_TUNE:
  case AUD_CMD_PLAY:
  case AUD_CMD_RECORD:
    return 1;
  default:
    return 0;
  }
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

  if (needs_a_backend(opts.command) && aud_backend_select(opts.backend) != 0)
  {
    return EXIT_FAILURE;
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
    return aud_cmd_list(&opts);
  case AUD_CMD_PROBE:
    return aud_cmd_probe(&opts);
  case AUD_CMD_VISUALIZE:
    return aud_cmd_visualize(&opts);
  case AUD_CMD_RENDER:
    return aud_cmd_render(&opts);
  case AUD_CMD_INFO:
    return aud_cmd_info(&opts);
  case AUD_CMD_PLAY:
    return aud_cmd_play(&opts);
  case AUD_CMD_TUNE:
    return aud_cmd_tune(&opts);
  case AUD_CMD_RECORD:
  default:
    return aud_cmd_record(&opts);
  }
}
