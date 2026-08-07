/* SPDX-License-Identifier: MIT */
/*
 * monitor.c - dispatch to the selected playback backend.
 *
 * The handle a caller holds, and one-line forwards into backend.h's table.
 * monitor_alsa.c and monitor_pipewire.c are where playback actually happens.
 */
#include "monitor.h"

#include "backend.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MONITOR_DEFAULT_PERIOD_FRAMES 512u
#define MONITOR_DEFAULT_PERIODS 3u

struct aud_monitor
{
  const aud_monitor_ops *ops;
  void *impl;
  unsigned rate;
  unsigned channels;
};

void aud_monitor_config_defaults(aud_monitor_config *cfg, unsigned rate,
                                 unsigned channels)
{
  if (cfg == NULL)
  {
    return;
  }

  memset(cfg, 0, sizeof(*cfg));
  cfg->name = AUD_MONITOR_DEFAULT_DEVICE;
  cfg->rate = rate;
  cfg->channels = channels;
  cfg->period_frames = MONITOR_DEFAULT_PERIOD_FRAMES;
  cfg->periods = MONITOR_DEFAULT_PERIODS;
}

aud_monitor *aud_monitor_open(const aud_monitor_config *cfg)
{
  const aud_monitor_ops *ops = aud_backend_monitor();
  aud_monitor *m;

  if (cfg == NULL || cfg->rate == 0 || cfg->channels == 0)
  {
    errno = EINVAL;
    return NULL;
  }

  m = calloc(1, sizeof(*m));
  if (m == NULL)
  {
    aud_warn("monitor: out of memory");
    return NULL;
  }

  m->ops = ops;
  m->rate = cfg->rate;
  m->channels = cfg->channels;

  m->impl = ops->open(cfg, &m->rate, &m->channels);
  if (m->impl == NULL)
  {
    /* the backend has already said why; a missing monitor is not fatal */
    free(m);
    return NULL;
  }
  return m;
}

void aud_monitor_close(aud_monitor *m)
{
  if (m == NULL)
  {
    return;
  }

  m->ops->close(m->impl);
  free(m);
}

unsigned long aud_monitor_dropped(const aud_monitor *m)
{
  return m != NULL ? m->ops->dropped(m->impl) : 0;
}

int aud_monitor_write(aud_monitor *m, const float *interleaved, size_t frames, float gain)
{
  if (m == NULL)
  {
    return -1;
  }

  return m->ops->write(m->impl, interleaved, frames, gain);
}
