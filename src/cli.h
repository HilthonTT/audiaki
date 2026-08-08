/* SPDX-License-Identifier: MIT */
/*
 * cli.h - command line parsing.
 *
 * Kept free of any audio system's types so the option handling can be tested on
 * its own; main.c maps the result onto aud_device_config.
 */
#ifndef AUDIAKI_CLI_H
#define AUDIAKI_CLI_H

#include "backend.h"
#include "format.h"
#include "log.h"
#include "visualize.h"

#include <stdio.h>

/* exit code used for malformed invocations, matching common CLI convention */
#define CLI_EXIT_USAGE 2

typedef enum
{
  AUD_CMD_RECORD = 0,
  AUD_CMD_PROBE,
  AUD_CMD_LIST,
  AUD_CMD_VISUALIZE,
  AUD_CMD_INFO,
  AUD_CMD_TUNE,
  AUD_CMD_PLAY,
  AUD_CMD_HELP,
  AUD_CMD_VERSION,
} aud_command;

typedef struct
{
  aud_command command;
  const char *device;
  /*
   * Whether -D was typed, as opposed to defaulted or taken from the
   * environment. --play needs to know: it opens an output, and $AUDIAKI_DEVICE
   * names a capture device, which is not one.
   */
  int device_explicit;
  const char *output_path;
  const char *input_path;  /* --visualize, --info and --play source WAV */
  const char *take_prefix; /* --take; output_path is derived from it */
  /*
   * Files named after the first one, which only --info accepts: measuring a
   * session means measuring every take in it. Points into argv.
   */
  char **extra_inputs;
  int extra_input_count;
  unsigned rate;
  unsigned channels;
  unsigned period_frames;
  unsigned periods;
  aud_format format; /* AUD_FORMAT_UNKNOWN = negotiate */
  double duration;   /* seconds; 0 = until interrupted */
  double preroll;    /* seconds held before the take; 0 = record immediately */
  int overwrite;
  int show_meter;
  int show_spectrum;          /* live spectrum bars instead of the peak bar */
  int monitor;                /* hear the input while it is being recorded */
  const char *monitor_device; /* output to monitor through; NULL = the default */
  double monitor_gain;        /* what the monitor is scaled by, not the file */
  double click_bpm;           /* metronome tempo; 0 = no metronome */
  unsigned click_beats;       /* beats to a bar; the first of each is accented */
  double click_gain;          /* how loud the click is, on the same scale as above */
  int metadata;               /* stamp the take with what made it; see meta.h */
  const char *note;           /* free text to stamp along with it */
  unsigned viz_width;
  unsigned viz_height;
  unsigned viz_fps;
  unsigned viz_bars;
  aud_viz_style viz_style;
  double a4_hz;             /* --tune's reference pitch */
  int json;                 /* machine readable output for --list, --probe and --info */
  aud_backend_kind backend; /* which audio system to talk to */
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
