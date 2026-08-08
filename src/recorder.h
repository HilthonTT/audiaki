/* SPDX-License-Identifier: MIT */
/*
 * recorder.h - the capture loop: device -> optional repack -> WAV file.
 */
#ifndef AUDIAKI_RECORDER_H
#define AUDIAKI_RECORDER_H

#include "device.h"

#include <stdint.h>

typedef struct
{
  const char *output_path;
  double duration;   /* seconds; 0 records until interrupted */
  int overwrite;     /* allow replacing an existing file */
  int show_meter;    /* draw a live display at all */
  int show_spectrum; /* make that display spectrum bars, not the peak bar */
  /*
   * Seconds to hold before the take starts; 0 records as soon as the device is
   * open. Anything else arms instead: nothing is written until Enter, and the
   * take then opens with the seconds leading up to it.
   */
  double preroll;
  /*
   * Play the input back through an output while it is being captured, so a
   * take can be heard as it is made. Off unless asked for, because monitoring
   * an open microphone through speakers feeds back.
   *
   * monitor_device is the output to play through; NULL means the default one.
   * monitor_gain scales what is heard and nothing else - the file is written
   * from the samples the device delivered, whatever the monitor is set to.
   */
  int monitor;
  const char *monitor_device;
  float monitor_gain;
  /*
   * A metronome mixed into that same output, so you can play to a grid the
   * take is aligned to - see click.h. Zero BPM is off, which is the default.
   *
   * It reaches the headphones and not the file: like monitor_gain, it changes
   * what the person recording hears and nothing about what is written. Asking
   * for one opens the output whether or not `monitor` is set, which is how a
   * click can be heard without also hearing the input.
   */
  double click_bpm;
  unsigned click_beats; /* beats to a bar; the first of each is accented */
  float click_gain;
  /*
   * Stamp the take with what made it, when, and from what device - see meta.h.
   * On by default; clearing it writes the plain 44 byte header instead, for
   * anyone whose tools want nothing between the fmt and data chunks.
   *
   * `note` is free text to carry along with it, or NULL for none.
   */
  int metadata;
  const char *note;
} aud_recorder_options;

typedef struct
{
  uint64_t frames;
  uint64_t bytes;
  uint64_t preroll_frames; /* of `frames`, how many came from before the start */
  unsigned xruns;
  unsigned long monitor_dropped; /* frames the monitor output could not keep up with */
  int clipped;
  int interrupted; /* stopped by SIGINT/SIGTERM rather than reaching the end */
  int cancelled;   /* interrupted while armed, so no file was created */
} aud_recorder_stats;

/*
 * Install SIGINT/SIGTERM handlers so a running capture stops cleanly and the
 * WAV header still gets patched. Returns 0 on success, -1 with errno set.
 */
int aud_recorder_install_signals(void);

/*
 * Record from `dev` into opts->output_path. Returns 0 on success and -1 on
 * failure, after reporting the reason through log.h. `stats` may be NULL.
 */
int aud_recorder_run(aud_device *dev, const aud_recorder_options *opts,
                     aud_recorder_stats *stats);

#endif /* AUDIAKI_RECORDER_H */
