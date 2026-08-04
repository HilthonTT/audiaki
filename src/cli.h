/* SPDX-License-Identifier: MIT */
/*
 * cli.h - command line parsing.
 *
 * Kept free of ALSA types so the option handling can be tested on its own;
 * main.c maps the result onto aud_device_config.
 */
#ifndef AUDIAKI_CLI_H
#define AUDIAKI_CLI_H

#include "format.h"
#include "log.h"

#include <stdio.h>

/* exit code used for malformed invocations, matching common CLI convention */
#define CLI_EXIT_USAGE 2

typedef enum
{
  AUD_CMD_RECORD = 0,
  AUD_CMD_PROBE,
  AUD_CMD_LIST,
  AUD_CMD_HELP,
  AUD_CMD_VERSION,
} aud_command;

typedef struct
{
  aud_command command;
  const char *device;
  const char *output_path;
  unsigned rate;
  unsigned channels;
  unsigned period_frames;
  unsigned periods;
  aud_format format; /* AUD_FORMAT_UNKNOWN = negotiate */
  double duration;   /* seconds; 0 = until interrupted */
  int overwrite;
  int show_meter;
  aud_log_level log_level;
} aud_options;

/* Populate `opts` with defaults, honouring the AUDIAKI_DEVICE environment. */
void cli_defaults(aud_options *opts);

/*
 * Parse argv into `opts`. Returns 0 when the caller should proceed, or a
 * process exit code (CLI_EXIT_USAGE) when the invocation was rejected.
 */
int cli_parse(int argc, char **argv, aud_options *opts);

void cli_print_usage(FILE *out);
void cli_print_version(FILE *out);

#endif /* AUDIAKI_CLI_H */
