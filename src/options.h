/* SPDX-License-Identifier: MIT */
/*
 * options.h - the request a single invocation describes.
 *
 * src/cli fills this in from argv; src/cmd reads it and does the work. Neither
 * includes the other, so a command can be read without the parser in front of
 * you, and an option can be added without touching the command that consumes
 * it. It carries no audio system's types, only the enums the option values are
 * drawn from.
 */
#ifndef AUDIAKI_OPTIONS_H
#define AUDIAKI_OPTIONS_H

#include "audio/format.h"
#include "backend/backend.h"
#include "media/visualize.h"
#include "util/config.h"
#include "util/log.h"

typedef enum
{
  AUD_CMD_RECORD = 0,
  AUD_CMD_PROBE,
  AUD_CMD_LIST,
  AUD_CMD_VISUALIZE,
  AUD_CMD_INFO,
  AUD_CMD_TUNE,
  AUD_CMD_PLAY,
  AUD_CMD_RENDER, /* mix a saved project down to a WAV; see edit/project.h */
  AUD_CMD_HELP,
  AUD_CMD_VERSION,
} aud_command;

/* What --play does when it reaches the end of what it was given. */
typedef enum
{
  AUD_REPEAT_NONE = 0, /* stop, which is what a playlist has always done */
  AUD_REPEAT_ALL,      /* start the list again */
  AUD_REPEAT_ONE,      /* start the current file again, and stay on it */
} aud_repeat_mode;

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
   * Where takes are kept, from --dir or the config file's take_dir. Empty means
   * the working directory, which is what a bare filename has always meant.
   *
   * An array rather than a pointer into argv, because most of the time it does
   * not come from argv at all - see util/config.h.
   */
  char take_dir[AUD_PATH_MAX];
  /* whether to ask where a finished take should be kept; see term/prompt.h */
  aud_prompt_mode prompt;
  /*
   * Files named after the first one, which only --info accepts: measuring a
   * session means measuring every take in it. Points into argv.
   */
  char **extra_inputs;
  int extra_input_count;
  unsigned rate;
  unsigned channels;
  /*
   * Which single capture channel to write, counting from 1, or 0 for all of
   * them, or AUD_CHANNEL_MIX for every channel averaged down to one.
   * `channels` is still what the device is asked to capture: an interface that
   * only does stereo is still opened as stereo, and this decides what reaches
   * the file.
   */
  unsigned channel;
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
  /*
   * What the capture is scaled by on the way in - and unlike the one above,
   * this one does reach the file. 1.0 leaves the samples exactly as the device
   * delivered them, which is what audiaki does unless asked otherwise.
   */
  double input_gain;
  double click_bpm;      /* metronome tempo; 0 = no metronome */
  unsigned click_beats;  /* beats to a bar; the first of each is accented */
  unsigned click_subdiv; /* ticks to a beat; 1 is the beat undivided */
  double click_gain;     /* how loud the click is, on the same scale as above */
  /*
   * Round trip to strike --click ahead of the grid by, so it is heard on the
   * beat rather than a buffer after it. Negative means nothing was said and it
   * is worked out from the buffers; zero turns the correction off. Shared with
   * the desktop app's overdub placement through the config file.
   */
  double latency_ms;
  int metadata;     /* stamp the take with what made it; see meta.h */
  const char *note; /* free text to stamp along with it */
  unsigned viz_width;
  unsigned viz_height;
  unsigned viz_fps;
  unsigned viz_bars;
  aud_viz_style viz_style;
  /*
   * How --play walks the files it was given. Shuffling picks a fresh order for
   * each pass over the list rather than one order for the whole run, so a
   * repeating playlist does not cycle through the same permutation forever.
   */
  int shuffle;
  aud_repeat_mode repeat;
  double a4_hz; /* --tune's reference pitch */
  /*
   * The pitch range --tune searches, or 0.0 for the default at each end. The
   * low end is what costs: see aud_tuner_analyse().
   */
  double tune_min_hz;
  double tune_max_hz;
  unsigned export_bits;     /* bit depth --render writes; 0 takes the default */
  int json;                 /* machine readable output for --list, --probe and --info */
  aud_backend_kind backend; /* which audio system to talk to */
  aud_log_level log_level;
} aud_options;

#endif /* AUDIAKI_OPTIONS_H */
