/* SPDX-License-Identifier: MIT */
/*
 * tune.h - the interactive tuner: capture device -> tuner -> terminal.
 *
 * What recorder.c is to a take, this is to tuning up. It owns its own control
 * flow and blocks until Ctrl+C, and it writes no file: the point of it is the
 * line on the terminal, not anything left behind afterwards.
 *
 * The pitch detection itself is in tuner.c, which knows nothing about ALSA and
 * is shared with the desktop app's tuner display.
 */
#ifndef AUDIAKI_TUNE_H
#define AUDIAKI_TUNE_H

#include "device.h"

typedef struct
{
  double a4_hz;   /* what A above middle C is being called */
  int show_meter; /* draw the live needle at all */
} aud_tune_options;

/*
 * Read from `dev` and display the pitch until interrupted. Returns 0 on
 * success and -1 on failure, after reporting the reason through log.h.
 *
 * The caller is expected to have installed the signal handlers already, the
 * same way run_record() does.
 */
int aud_tune_run(aud_device *dev, const aud_tune_options *opts);

#endif /* AUDIAKI_TUNE_H */
