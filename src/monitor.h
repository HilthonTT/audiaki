/* SPDX-License-Identifier: MIT */
/*
 * monitor.h - play captured audio back while it is being recorded.
 *
 * A second PCM, on the playback side, fed the same frames that go to the WAV
 * file so you can hear what the interface is hearing. Independent of the
 * capture device: monitoring can fail, or be turned off mid-take, without the
 * recording noticing.
 *
 * The write path never blocks. If the playback device falls behind - which it
 * will, because the capture and playback clocks are not the same crystal - the
 * frames that do not fit are dropped rather than queued. A monitor that drifts
 * further behind the longer you record is worse than one that skips.
 *
 * Feedback warning: monitoring a microphone through speakers howls. Callers
 * should leave it off until asked.
 */
#ifndef AUDIAKI_MONITOR_H
#define AUDIAKI_MONITOR_H

#include <stddef.h>

#define AUD_MONITOR_DEFAULT_DEVICE "default"

typedef struct
{
  const char *name;       /* ALSA playback device; NULL means the default */
  unsigned rate;          /* must match the capture stream */
  unsigned channels;      /* must match the capture stream */
  unsigned period_frames; /* how much is handed over per write */
  unsigned periods;       /* periods per buffer; more latency, fewer dropouts */
} aud_monitor_config;

typedef struct aud_monitor aud_monitor;

/* Fill `cfg` with the defaults for a stream of `rate` Hz and `channels`. */
void aud_monitor_config_defaults(aud_monitor_config *cfg, unsigned rate,
                                 unsigned channels);

/*
 * Open the playback stream. Returns NULL after reporting the reason through
 * log.h - a missing or busy output device is not fatal to a recording, so
 * callers are expected to carry on without monitoring.
 */
aud_monitor *aud_monitor_open(const aud_monitor_config *cfg);

void aud_monitor_close(aud_monitor *m);

/*
 * Play `frames` of interleaved float audio, scaled by `gain`. Values outside
 * [-1.0, 1.0] are clipped rather than wrapped.
 *
 * Returns 0 when the stream is healthy and -1 once it has failed for good, at
 * which point the caller should close the monitor. Dropped frames are not an
 * error; they are counted and reported through aud_monitor_dropped().
 */
int aud_monitor_write(aud_monitor *m, const float *interleaved, size_t frames,
                      float gain);

/* Discard anything still queued, for when monitoring is switched off. */
void aud_monitor_flush(aud_monitor *m);

/* The device string the stream was opened on. */
const char *aud_monitor_device(const aud_monitor *m);

/* Frames dropped because playback could not keep up, since opening. */
unsigned long aud_monitor_dropped(const aud_monitor *m);

/* Underruns recovered from, since opening. */
unsigned aud_monitor_underruns(const aud_monitor *m);

#endif /* AUDIAKI_MONITOR_H */
