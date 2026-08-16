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
  p->mix_on = 1;
}

/*
 * Build the generator from what was asked for, now that there is a rate to
 * build it at. A tempo the click will not play - or none at all - leaves it
 * off rather than half set up.
 */
static void arm_click(aud_player *p)
{
  aud_click_config cfg;

  p->click_on = 0;
  if (!(p->click_bpm > 0.0) || p->rate == 0)
  {
    return;
  }

  aud_click_config_defaults(&cfg, p->click_bpm, p->rate);
  cfg.beats_per_bar = p->click_beats;
  cfg.subdiv = p->click_subdiv;
  cfg.gain = p->click_gain;

  p->click_on = aud_click_init(&p->click, &cfg) == 0;
}

void aud_player_set_click(aud_player *p, double bpm, unsigned beats_per_bar,
                          unsigned subdiv, float gain)
{
  if (p == NULL)
  {
    return;
  }

  p->click_bpm = bpm;
  p->click_beats = beats_per_bar;
  p->click_subdiv = subdiv;
  p->click_gain = gain;

  /*
   * A pass already running picks it up on its next chunk. It cannot glitch:
   * every chunk seeks the grid to the frame it is about to mix, so the new
   * tempo lands on the beat the new grid says rather than wherever the old
   * one had got to.
   */
  arm_click(p);
}

void aud_player_set_loop(aud_player *p, int on)
{
  if (p != NULL)
  {
    p->looping = on ? 1 : 0;
  }
}

void aud_player_set_mix(aud_player *p, int on)
{
  if (p != NULL)
  {
    p->mix_on = on ? 1 : 0;
  }
}

/* Frames the pass covers, or 0 when it is open-ended and covers no fixed span. */
static uint64_t player_span(const aud_player *p)
{
  if (p->to == AUD_PLAYER_OPEN_ENDED || p->to <= p->from)
  {
    return 0;
  }
  return p->to - p->from;
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
  aud_loudness_destroy(p->loud);
  p->loud = NULL;
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
  if (to != AUD_PLAYER_OPEN_ENDED && to <= from)
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

  /*
   * Not fatal when it cannot be had. A rate BS.1770 is not defined at, or no
   * memory for the block history, costs the readout and nothing else - the
   * point of the transport is to play the project, not to measure it.
   */
  if (aud_loudness_supported(p->rate, p->channels))
  {
    p->loud = aud_loudness_create(p->rate, p->channels);
  }

  arm_click(p);
  return 0;
}

int aud_player_pump(aud_player *p, const aud_doc *d)
{
  long space;
  uint64_t span;
  int looping;

  if (p == NULL || !p->playing || p->out == NULL)
  {
    return 0;
  }

  span = player_span(p);
  looping = p->looping && span > 0;

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
    uint64_t done = span > 0 && looping ? p->written % span : p->written;
    uint64_t at = p->from + done;
    uint64_t room;
    size_t want = (size_t)space;

    if (span > 0 && !looping && p->written >= span)
    {
      break;
    }

    /*
     * Never across the end, and never across the loop point either: a chunk
     * that ran over it would mix the start of the loop as though it followed
     * the end, and the click over it would count straight through the seam.
     */
    room = span > 0 ? span - done : (uint64_t)AUD_PLAYER_CHUNK;

    if (want > AUD_PLAYER_CHUNK)
    {
      want = AUD_PLAYER_CHUNK;
    }
    if ((uint64_t)want > room)
    {
      want = (size_t)room;
    }

    if (p->mix_on)
    {
      if (aud_mix_read(&p->mix, d, at, p->buf, want, p->channels) != 0)
      {
        aud_player_stop(p);
        return 1;
      }
    }
    else
    {
      memset(p->buf, 0, want * p->channels * sizeof(float));
    }

    /*
     * Measured here, between the mix and the click, because it is the project
     * that is being metered - see the field in player.h.
     */
    if (p->loud != NULL && aud_loudness_feed(p->loud, p->buf, want) != 0)
    {
      /* the history would not grow; stop measuring rather than stop playing */
      aud_loudness_destroy(p->loud);
      p->loud = NULL;
    }

    /*
     * Over the mix rather than into it, and after it: the click is something
     * you hear, not something the project holds. Seeking first is what keeps
     * it on the ruler's grid across a loop's seam - see click.h.
     */
    if (p->click_on)
    {
      aud_click_seek(&p->click, at);
      aud_click_mix(&p->click, p->buf, want, p->channels);
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
   *
   * A loop and an open-ended pass have no end to reach, so neither gets here:
   * both are stopped by whoever started them.
   */
  if (span > 0 && !looping && p->written >= span + (uint64_t)p->latency)
  {
    aud_player_stop(p);
    return 1;
  }

  return 0;
}

uint64_t aud_player_head(const aud_player *p)
{
  uint64_t played;
  uint64_t span;

  if (p == NULL || !p->playing)
  {
    return 0;
  }

  /*
   * What has been handed over, less what the output is still holding. Not
   * exact - the output does not say how full it is - but it is out by less than
   * one buffer and in the right direction, which is what a playhead needs.
   */
  played = p->written;
  if (played > p->latency)
  {
    played -= p->latency;
  }
  else
  {
    played = 0;
  }

  /* a loop reports where round it is, not how far it has been */
  span = player_span(p);
  if (p->looping && span > 0)
  {
    played %= span;
  }
  return p->from + played;
}

void aud_player_loudness(const aud_player *p, aud_loudness_live *out)
{
  aud_loudness_read_live(p != NULL ? p->loud : NULL, out);
}

int aud_player_playing(const aud_player *p)
{
  return p != NULL && p->playing;
}
