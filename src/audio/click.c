/* SPDX-License-Identifier: MIT */
#include "audio/click.h"

#include <math.h>
#include <string.h>

/*
 * A beat is a short sine burst under an exponential decay - the shape a struck
 * block makes, and about as much as can be said with no wavetable to read from.
 *
 * The two pitches are an octave apart so the accent is unmistakable without
 * being a different sound, and both sit where the ear is most sensitive, which
 * is what lets the click stay quiet enough to play under.
 */
#define CLICK_ACCENT_HZ 1600.0
#define CLICK_BEAT_HZ 800.0

/*
 * A subdivision is the beat pitch struck softer rather than a third tone. Two
 * pitches already carry the bar; a third competes with the accent for the same
 * attention, and what the ear wants from an off-beat tick is to know it is not
 * the beat, which quieter says on its own.
 */
#define CLICK_SUBDIV_GAIN 0.45

/*
 * 50 ms of burst over an 8 ms decay, so the tail is down to about 0.2% of the
 * peak - some 54 dB - by the time it is cut off. Loud enough to hear, short
 * enough that the fastest tempo click.h accepts (300 BPM, 200 ms apart) never
 * has two bursts sounding at once, which is what lets aud_click_mix() work one
 * beat at a time.
 */
#define CLICK_BURST_SECONDS 0.050
#define CLICK_DECAY_SECONDS 0.008

#define CLICK_TWO_PI 6.283185307179586

static double clamp_gain(double gain)
{
  if (!(gain >= AUD_CLICK_GAIN_MIN))
  {
    return AUD_CLICK_GAIN_MIN; /* also catches NaN */
  }
  if (gain > AUD_CLICK_GAIN_MAX)
  {
    return AUD_CLICK_GAIN_MAX;
  }
  return gain;
}

void aud_click_config_defaults(aud_click_config *cfg, double bpm, unsigned rate)
{
  if (cfg == NULL)
  {
    return;
  }

  memset(cfg, 0, sizeof(*cfg));
  cfg->bpm = bpm;
  cfg->beats_per_bar = AUD_CLICK_DEFAULT_BEATS;
  cfg->subdiv = AUD_CLICK_DEFAULT_SUBDIV;
  cfg->rate = rate;
  cfg->gain = (float)AUD_CLICK_DEFAULT_GAIN;
}

int aud_click_init(aud_click *c, const aud_click_config *cfg)
{
  double spacing;

  if (c == NULL || cfg == NULL)
  {
    return -1;
  }

  /* written the way round that rejects NaN rather than letting it through */
  if (!(cfg->bpm >= AUD_CLICK_BPM_MIN && cfg->bpm <= AUD_CLICK_BPM_MAX))
  {
    return -1;
  }
  if (cfg->rate == 0)
  {
    return -1;
  }

  spacing = 60.0 * (double)cfg->rate / cfg->bpm;

  memset(c, 0, sizeof(*c));
  c->inv_rate = 1.0 / (double)cfg->rate;
  c->omega_accent = CLICK_TWO_PI * CLICK_ACCENT_HZ * c->inv_rate;
  c->omega_beat = CLICK_TWO_PI * CLICK_BEAT_HZ * c->inv_rate;
  c->decay = c->inv_rate / CLICK_DECAY_SECONDS;
  c->spacing = spacing;
  c->beats_per_bar =
      cfg->beats_per_bar > AUD_CLICK_BEATS_MAX ? AUD_CLICK_BEATS_MAX : cfg->beats_per_bar;
  /* 0 and 1 are the same request - the beat undivided - so both become 1 */
  c->subdiv = cfg->subdiv == 0u ? 1u : cfg->subdiv;
  if (c->subdiv > AUD_CLICK_SUBDIV_MAX)
  {
    c->subdiv = AUD_CLICK_SUBDIV_MAX;
  }
  c->tick_spacing = spacing / (double)c->subdiv;
  c->gain = (float)clamp_gain((double)cfg->gain);
  c->subdiv_gain = (float)(c->gain * CLICK_SUBDIV_GAIN);

  c->burst_frames = (unsigned)(CLICK_BURST_SECONDS * (double)cfg->rate);
  if (c->burst_frames == 0)
  {
    c->burst_frames = 1;
  }
  /*
   * A burst that outlasts the gap to the next strike would break the
   * one-tick-at-a-time walk below rather than merely sound wrong. Undivided
   * that only happens at rates far below what any device offers; divided it is
   * reachable at the top of the tempo range, where eight ticks to a beat at
   * 300 BPM are 25 ms apart and the 50 ms burst has to be cut to fit.
   */
  if ((double)c->burst_frames > c->tick_spacing)
  {
    c->burst_frames = (unsigned)c->tick_spacing;
    if (c->burst_frames == 0)
    {
      c->burst_frames = 1;
    }
  }

  return 0;
}

void aud_click_reset(aud_click *c)
{
  if (c != NULL)
  {
    c->frame = 0;
    c->tick = 0;
  }
}

void aud_click_seek(aud_click *c, uint64_t frame)
{
  if (c == NULL)
  {
    return;
  }

  c->frame = frame;

  if (!(c->tick_spacing > 0.0))
  {
    c->tick = 0; /* never initialised; the mix will find nothing to do anyway */
    return;
  }

  /*
   * Worked out rather than walked to, so seeking an hour in costs the same as
   * seeking a bar in. Landing a tick either side of the right one is harmless:
   * aud_click_mix() steps the tick forward until the burst it holds reaches
   * the frame being mixed, and a tick not yet due is simply silence until it
   * is - which is what makes this safe to call before every pass.
   */
  c->tick = (uint64_t)((double)frame / c->tick_spacing);
}

/*
 * Where tick `n` starts, computed from n rather than accumulated, so rounding
 * cannot build up: at 48 kHz the product stays exact in a double for longer
 * than a WAV file can be.
 */
static uint64_t onset_of(const aud_click *c, uint64_t tick)
{
  return (uint64_t)((double)tick * c->tick_spacing + 0.5);
}

/*
 * What tick `n` sounds like: the bar's first beat is the accent, every other
 * whole beat is the plain tone, and what falls between beats is that tone
 * softened. Returns the amplitude and sets `*omega` to the pitch.
 */
static float strike_of(const aud_click *c, uint64_t tick, double *omega)
{
  uint64_t beat;

  if ((tick % c->subdiv) != 0u)
  {
    *omega = c->omega_beat;
    return c->subdiv_gain;
  }

  beat = tick / c->subdiv;
  *omega = (c->beats_per_bar > 1u && (beat % c->beats_per_bar) == 0u) ? c->omega_accent
                                                                      : c->omega_beat;
  return c->gain;
}

void aud_click_mix(aud_click *c, float *interleaved, size_t frames, unsigned channels)
{
  size_t i = 0;

  if (c == NULL || interleaved == NULL || channels == 0 || c->gain <= 0.0f)
  {
    if (c != NULL)
    {
      c->frame += frames; /* silent, but still on the grid when it comes back */
    }
    return;
  }

  while (i < frames)
  {
    uint64_t here = c->frame + i;
    uint64_t onset = onset_of(c, c->tick);
    uint64_t end = onset + c->burst_frames;
    double omega;
    float strike;
    size_t stop;

    if (here >= end)
    {
      c->tick++;
      continue;
    }

    /* silence up to the next tick: left as it was found, since this mixes */
    if (here < onset)
    {
      uint64_t gap = onset - here;
      size_t room = frames - i;

      i += (gap < (uint64_t)room) ? (size_t)gap : room;
      continue;
    }

    strike = strike_of(c, c->tick, &omega);

    stop = frames;
    if (end - c->frame < (uint64_t)stop)
    {
      stop = (size_t)(end - c->frame);
    }

    for (; i < stop; i++)
    {
      double k = (double)(c->frame + i - onset); /* frames since the strike */
      float v = (float)(sin(omega * k) * exp(-c->decay * k)) * strike;

      for (unsigned ch = 0; ch < channels; ch++)
      {
        interleaved[i * channels + ch] += v;
      }
    }
  }

  c->frame += frames;
}

uint64_t aud_click_beat_frames(const aud_click *c)
{
  if (c == NULL)
  {
    return 0;
  }
  return (uint64_t)(c->spacing + 0.5);
}
