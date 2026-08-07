/* SPDX-License-Identifier: MIT */
/*
 * play.h - playing a take back: WAV file -> playback device -> terminal.
 *
 * What tune.c is to tuning up, this is to listening: it owns its own control
 * flow, blocks until the file ends or Ctrl+C, and writes nothing. The point of
 * it is hearing the take that --info has just measured, without leaving the
 * shell for a media player.
 *
 * No capture device is opened. The output is aud_monitor, which is the playback
 * PCM the desktop app monitors through; see monitor.h for why a file has to ask
 * how much will fit rather than write as fast as it can read.
 */
#ifndef AUDIAKI_PLAY_H
#define AUDIAKI_PLAY_H

typedef struct
{
  const char *input_path; /* the WAV to play */
  const char *device;     /* playback device; NULL means the default output */
  double duration;        /* seconds; 0 = to the end of the file */
  int show_meter;         /* draw the level line at all */
  int show_spectrum;      /* spectrum bars instead of the peak bar */
} aud_play_options;

/*
 * Play `opts->input_path` until it ends, `opts->duration` elapses or the stop
 * flag is set. Returns 0 on success and -1 on failure, after reporting the
 * reason through log.h.
 *
 * The caller is expected to have installed the signal handlers already, the
 * same way run_record() does.
 */
int aud_play_run(const aud_play_options *opts);

#endif /* AUDIAKI_PLAY_H */
