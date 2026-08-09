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
  c->gain = (float)clamp_gain((double)cfg->gain);

  c->burst_frames = (unsigned)(CLICK_BURST_SECONDS * (double)cfg->rate);
  if (c->burst_frames == 0)
  {
    c->burst_frames = 1;
  }
  /*
   * Only reachable at rates far below what any device offers, but a burst that
   * outlasts the gap between beats would break the one-beat-at-a-time walk
   * below rather than merely sound wrong.
   */
  if ((double)c->burst_frames > spacing)
  {
    c->burst_frames = (unsigned)spacing;
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
    c->beat = 0;
  }
}

void aud_click_seek(aud_click *c, uint64_t frame)
{
  if (c == NULL)
  {
    return;
  }

  c->frame = frame;

  if (!(c->spacing > 0.0))
  {
    c->beat = 0; /* never initialised; the mix will find nothing to do anyway */
    return;
  }

  /*
   * Worked out rather than walked to, so seeking an hour in costs the same as
   * seeking a bar in. Landing a beat either side of the right one is harmless:
   * aud_click_mix() steps the beat forward until the burst it holds reaches
   * the frame being mixed, and a beat not yet due is simply silence until it
   * is - which is what makes this safe to call before every pass.
   */
  c->beat = (uint64_t)((double)frame / c->spacing);
}

/*
 * Where beat `n` starts, computed from n rather than accumulated, so rounding
 * cannot build up: at 48 kHz the product stays exact in a double for longer
 * than a WAV file can be.
 */
static uint64_t onset_of(const aud_click *c, uint64_t beat)
{
  return (uint64_t)((double)beat * c->spacing + 0.5);
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
    uint64_t onset = onset_of(c, c->beat);
    uint64_t end = onset + c->burst_frames;
    double omega;
    size_t stop;

    if (here >= end)
    {
      c->beat++;
      continue;
    }

    /* silence up to the next beat: left as it was found, since this mixes */
    if (here < onset)
    {
      uint64_t gap = onset - here;
      size_t room = frames - i;

      i += (gap < (uint64_t)room) ? (size_t)gap : room;
      continue;
    }

    omega = (c->beats_per_bar > 1u && (c->beat % c->beats_per_bar) == 0) ? c->omega_accent
                                                                         : c->omega_beat;

    stop = frames;
    if (end - c->frame < (uint64_t)stop)
    {
      stop = (size_t)(end - c->frame);
    }

    for (; i < stop; i++)
    {
      double k = (double)(c->frame + i - onset); /* frames since the strike */
      float v = (float)(sin(omega * k) * exp(-c->decay * k)) * c->gain;

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
