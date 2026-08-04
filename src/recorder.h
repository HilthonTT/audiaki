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
  double duration; /* seconds; 0 records until interrupted */
  int overwrite;   /* allow replacing an existing file */
  int show_meter;  /* draw the live peak meter */
} aud_recorder_options;

typedef struct
{
  uint64_t frames;
  uint64_t bytes;
  unsigned xruns;
  int clipped;
  int interrupted; /* stopped by SIGINT/SIGTERM rather than reaching the end */
} aud_recorder_stats;

/*
 * Install SIGINT/SIGTERM handlers so a running capture stops cleanly and the
 * WAV header still gets patched. Returns 0 on success, -1 with errno set.
 */
int aud_recorder_install_signals(void);

/* Non-zero once a stop has been requested. */
int aud_recorder_stop_requested(void);

/*
 * Record from `dev` into opts->output_path. Returns 0 on success and -1 on
 * failure, after reporting the reason through log.h. `stats` may be NULL.
 */
int aud_recorder_run(aud_device *dev, const aud_recorder_options *opts,
                     aud_recorder_stats *stats);

#endif /* AUDIAKI_RECORDER_H */
