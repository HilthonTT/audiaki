/* SPDX-License-Identifier: MIT */
#include "gui/player.h"

#include "backend/monitor.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

/*
 * A stereo mix, whatever the tracks are. It is what an output offers and what
 * anyone listening has; a project of mono takes still wants panning to mean
 * something, and that needs two sides to mean it with.
 */
#define PLAYER_CHANNELS 2u

void aud_player_init(aud_player *p)
{
  memset(p, 0, sizeof(*p));
}

void aud_player_stop(aud_player *p)
{
  if (p == NULL)
  {
    return;
  }

  if (p->out != NULL)
  {
    aud_monitor_close(p->out);
    p->out = NULL;
  }
  aud_mix_free(&p->mix);
  free(p->buf);
  p->buf = NULL;
  p->playing = 0;
}

int aud_player_start(aud_player *p, const aud_doc *d, uint64_t from, uint64_t to,
                     const char *device)
{
  aud_monitor_config cfg;

  if (p == NULL || d == NULL || d->rate == 0)
  {
    return -1;
  }

  aud_player_stop(p);

  if (to == 0)
  {
    to = aud_doc_end(d);
  }
  if (to <= from)
  {
    return -1; /* nothing to play; the caller has probably clicked past the end */
  }

  aud_monitor_config_defaults(&cfg, d->rate, PLAYER_CHANNELS);
  cfg.name = device;

  p->out = aud_monitor_open(&cfg);
  if (p->out == NULL)
  {
    return -1;
  }

  if (aud_mix_init(&p->mix, AUD_PLAYER_CHUNK) != 0)
  {
    aud_player_stop(p);
    return -1;
  }

  p->buf = malloc(AUD_PLAYER_CHUNK * PLAYER_CHANNELS * sizeof(float));
  if (p->buf == NULL)
  {
    aud_player_stop(p);
    return -1;
  }

  p->rate = d->rate;
  p->channels = PLAYER_CHANNELS;
  p->latency = cfg.period_frames * cfg.periods;
  p->from = from;
  p->to = to;
  p->written = 0;
  p->playing = 1;
  return 0;
}

int aud_player_pump(aud_player *p, const aud_doc *d)
{
  long space;

  if (p == NULL || !p->playing || p->out == NULL)
  {
    return 0;
  }

  /*
   * Only ever as much as the output will take without dropping any. Handing it
   * more would put the playhead ahead of what is being heard, and reading the
   * project faster than it plays is how a playhead ends up somewhere the sound
   * is not.
   */
  space = aud_monitor_space(p->out);
  if (space < 0)
  {
    aud_warn("playback stopped: the output stream failed");
    aud_player_stop(p);
    return 1;
  }

  while (space > 0)
  {
    uint64_t at = p->from + p->written;
    size_t want = (size_t)space;

    if (at >= p->to)
    {
      break;
    }
    if (want > AUD_PLAYER_CHUNK)
    {
      want = AUD_PLAYER_CHUNK;
    }
    if ((uint64_t)want > p->to - at)
    {
      want = (size_t)(p->to - at);
    }

    if (aud_mix_read(&p->mix, d, at, p->buf, want, p->channels) != 0)
    {
      aud_player_stop(p);
      return 1;
    }

    if (aud_monitor_write(p->out, p->buf, want, 1.0f) != 0)
    {
      aud_warn("playback stopped: the output stream failed");
      aud_player_stop(p);
      return 1;
    }

    p->written += want;
    space -= (long)want;
  }

  /*
   * Done only once what was handed over has actually been played. Stopping at
   * the last frame written would cut the last few tens of milliseconds off
   * every playback, which is exactly the part you were listening for at the end
   * of a take.
   */
  if (p->from + p->written >= p->to && aud_player_head(p) >= p->to)
  {
    aud_player_stop(p);
    return 1;
  }

  return 0;
}

uint64_t aud_player_head(const aud_player *p)
{
  uint64_t written;

  if (p == NULL || !p->playing)
  {
    return 0;
  }

  /*
   * What has been handed over, less what the output is still holding. Not
   * exact - the output does not say how full it is - but it is out by less than
   * one buffer and in the right direction, which is what a playhead needs.
   */
  written = p->written;
  if (written > p->latency)
  {
    written -= p->latency;
  }
  else
  {
    written = 0;
  }
  return p->from + written;
}

int aud_player_playing(const aud_player *p)
{
  return p != NULL && p->playing;
}
