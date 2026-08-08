/* SPDX-License-Identifier: MIT */
/*
 * monitor.h - the playback side: a PCM to hear audio through.
 *
 * Two callers, wanting the same stream on opposite terms. Monitoring feeds it
 * the frames going to the WAV file so you can hear what the interface is
 * hearing, and the source sets the pace. Playing a take back (cmd/play.c) feeds it
 * from a file, which has no pace of its own at all.
 *
 * The write path never blocks. If the playback device falls behind - which it
 * will while monitoring, because the capture and playback clocks are not the
 * same crystal - the frames that do not fit are dropped rather than queued. A
 * monitor that drifts further behind the longer you record is worse than one
 * that skips.
 *
 * That rule is why aud_monitor_space() exists. A file read at disk speed would
 * be almost entirely dropped by it, so playback asks how much will fit and
 * hands over exactly that, and the output's own consumption becomes the clock.
 *
 * Feedback warning: monitoring a microphone through speakers howls. Callers
 * should leave it off until asked.
 */
#ifndef AUDIAKI_MONITOR_H
#define AUDIAKI_MONITOR_H

#include "backend/backend.h"

#include <stddef.h>

#define AUD_MONITOR_DEFAULT_DEVICE "default"

struct aud_monitor_config
{
  const char *name;       /* playback device; NULL means the default */
  unsigned rate;          /* must match the capture stream */
  unsigned channels;      /* must match the capture stream */
  unsigned period_frames; /* how much is handed over per write */
  unsigned periods;       /* periods per buffer; more latency, fewer dropouts */
};

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

/* Frames dropped because playback could not keep up, since opening. */
unsigned long aud_monitor_dropped(const aud_monitor *m);

/*
 * Frames aud_monitor_write() would accept right now without dropping any.
 * Returns -1 once the stream has failed for good.
 *
 * Zero is the normal answer, not an error: it means the output is full and the
 * caller should wait rather than read more of its source.
 */
long aud_monitor_space(aud_monitor *m);

/*
 * Wait for the frames already handed over to finish playing, then return.
 * Bounded by a couple of seconds, so a stalled output cannot hang the caller.
 *
 * Monitoring has no use for this - the input is still arriving when the take
 * ends - but closing the stream at the end of a file otherwise cuts off the
 * last of it, which is the buffer's worth of audio.
 */
void aud_monitor_drain(aud_monitor *m);

#endif /* AUDIAKI_MONITOR_H */
