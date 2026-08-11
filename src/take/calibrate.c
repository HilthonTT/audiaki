/* SPDX-License-Identifier: MIT */
#include "take/calibrate.h"

#include "audio/format.h"
#include "take/latency.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* M_PI is an X/Open extension, and this project compiles with -std=c11. */
#define CAL_PI 3.14159265358979323846

/*
 * Below this the denominator of the match is rounding rather than a signal, and
 * dividing by it would turn a silent window into a perfect match with anything.
 */
#define CAL_ENERGY_EPSILON 1e-12

/* One burst, and what came back after it. */
typedef struct
{
  size_t filled;  /* frames of the window captured so far */
  int dropped;    /* the output could not play this one */
  int measured;   /* the search below has been run */
  int found;      /* ...and it produced a reading */
  uint64_t delay; /* frames between the burst going out and coming back */
  double match;   /* how well it matched, from 0 to 1 */
  double peak;    /* the loudest anything got in the window */
} cal_slot;

struct aud_calibrate
{
  unsigned rate;
  unsigned repeats;
  size_t burst_frames;
  size_t search_frames;
  uint64_t spacing; /* frames between one burst and the next */
  uint64_t lead_in;
  float *burst; /* burst_frames of sweep */
  double burst_energy;
  float *window;  /* repeats * search_frames, one window per burst */
  cal_slot *slot; /* repeats */
  uint64_t frame; /* the next capture frame to be handed over */
  unsigned long dropped_seen;
  unsigned emitting; /* slot being written this period, plus one; 0 for none */
};

/* Where burst `i` goes out, on the capture clock. */
static uint64_t burst_at(const aud_calibrate *c, unsigned i)
{
  return c->lead_in + (uint64_t)i * c->spacing;
}

/*
 * A linear sweep under a Hann envelope. The envelope matters as much as the
 * sweep: a burst that starts and stops at full amplitude has a step at each
 * end, and a step is broadband, so it correlates with every transient in the
 * room as well as with itself.
 */
static void burst_fill(float *dst, size_t n, unsigned rate, float gain)
{
  double duration = (double)n / rate;
  double sweep = (AUD_CALIBRATE_HIGH_HZ - AUD_CALIBRATE_LOW_HZ) / duration;
  size_t i;

  for (i = 0; i < n; i++)
  {
    double t = (double)i / rate;
    /* the integral of a frequency rising linearly with time, which is the phase */
    double phase = 2.0 * CAL_PI * (AUD_CALIBRATE_LOW_HZ * t + 0.5 * sweep * t * t);
    double envelope = 0.5 - 0.5 * cos(2.0 * CAL_PI * (double)i / (double)(n - 1));

    dst[i] = (float)(gain * envelope * sin(phase));
  }
}

/*
 * Slide `h` along `x` and return the best normalised match, with where it was
 * in `*best_at`.
 *
 * Normalised by the energy under each window, so the answer is how alike the
 * two shapes are rather than how loud the louder one is - which is what lets a
 * quiet return beat a loud rumble that happens to sit under it. The absolute
 * value is taken because a path that inverts polarity returns the burst upside
 * down, and upside down is still the burst.
 *
 * Direct rather than through the FFT: the window is half a second and the burst
 * twenty milliseconds, so this is a few million multiply-adds - less than the
 * capture it is measuring took to arrive, and it happens once per burst.
 */
static double correlate(const float *x, size_t n, const float *h, size_t m,
                        double h_energy, size_t *best_at)
{
  double energy = 0.0;
  double best = 0.0;
  size_t at = 0;
  size_t d;
  size_t i;

  *best_at = 0;
  if (m == 0 || n < m || h_energy <= CAL_ENERGY_EPSILON)
  {
    return 0.0;
  }

  for (i = 0; i < m; i++)
  {
    energy += (double)x[i] * x[i];
  }

  for (d = 0;; d++)
  {
    double sum = 0.0;
    double denominator;

    for (i = 0; i < m; i++)
    {
      sum += (double)x[d + i] * h[i];
    }

    denominator = sqrt(energy * h_energy);
    if (denominator > CAL_ENERGY_EPSILON)
    {
      double match = fabs(sum / denominator);

      if (match > best)
      {
        best = match;
        at = d;
      }
    }

    if (d + m >= n)
    {
      break;
    }

    /*
     * The window energy one frame along, rather than summed again: the sum is
     * over half a second and the loop runs half a second of times, so doing it
     * properly would be quadratic in the search window for no gain in accuracy.
     * Accumulated in double, and floored, because the subtraction of a large
     * term from a large sum can land a hair under zero.
     */
    energy -= (double)x[d] * x[d];
    energy += (double)x[d + m] * x[d + m];
    if (energy < 0.0)
    {
      energy = 0.0;
    }
  }

  *best_at = at;
  return best;
}

void aud_calibrate_config_defaults(aud_calibrate_config *cfg, unsigned rate)
{
  if (cfg == NULL)
  {
    return;
  }

  memset(cfg, 0, sizeof(*cfg));
  cfg->rate = rate;
  cfg->repeats = AUD_CALIBRATE_DEFAULT_REPEATS;
  cfg->gain = AUD_CALIBRATE_GAIN;
}

aud_calibrate *aud_calibrate_create(const aud_calibrate_config *cfg)
{
  aud_calibrate *c;

  if (cfg == NULL || cfg->rate == 0 || cfg->repeats < AUD_CALIBRATE_MIN_REPEATS ||
      cfg->repeats > AUD_CALIBRATE_MAX_REPEATS)
  {
    return NULL;
  }

  c = calloc(1, sizeof(*c));
  if (c == NULL)
  {
    return NULL;
  }

  c->rate = cfg->rate;
  c->repeats = cfg->repeats;
  c->burst_frames = (size_t)(AUD_CALIBRATE_BURST_MS * cfg->rate / 1000.0);
  c->search_frames = (size_t)(AUD_LATENCY_MAX_MS * cfg->rate / 1000.0);
  c->lead_in = (uint64_t)(AUD_CALIBRATE_LEAD_IN * cfg->rate);

  /*
   * A rate low enough to make the burst a handful of frames is a rate the
   * sweep cannot be drawn at, never mind found in. Two frames is the fewest
   * the Hann envelope divides by without dividing by zero; the real floor is
   * far above it, and AUD_RATE_MIN puts the burst at forty frames.
   */
  if (c->burst_frames < 2 || c->search_frames <= c->burst_frames)
  {
    free(c);
    return NULL;
  }

  /*
   * The next burst goes out as the previous window closes, so no burst can
   * ever be heard inside another one's search - which is the only way a match
   * could be to the wrong burst and be right about everything else.
   */
  c->spacing = (uint64_t)c->search_frames + c->burst_frames;

  c->burst = malloc(c->burst_frames * sizeof(*c->burst));
  c->window = calloc((size_t)c->repeats * c->search_frames, sizeof(*c->window));
  c->slot = calloc(c->repeats, sizeof(*c->slot));

  if (c->burst == NULL || c->window == NULL || c->slot == NULL)
  {
    aud_calibrate_destroy(c);
    return NULL;
  }

  burst_fill(c->burst, c->burst_frames, c->rate, cfg->gain);
  for (size_t i = 0; i < c->burst_frames; i++)
  {
    c->burst_energy += (double)c->burst[i] * c->burst[i];
  }

  return c;
}

void aud_calibrate_destroy(aud_calibrate *c)
{
  if (c == NULL)
  {
    return;
  }

  free(c->burst);
  free(c->window);
  free(c->slot);
  free(c);
}

/* One frame of `captured`, averaged down to one channel. */
static float mono_at(const float *captured, unsigned channels, size_t frame)
{
  double sum = 0.0;
  unsigned ch;

  if (channels <= 1)
  {
    return captured[frame];
  }

  for (ch = 0; ch < channels; ch++)
  {
    sum += captured[frame * channels + ch];
  }
  return (float)(sum / channels);
}

/* The overlap of [a0, a1) and [b0, b1), or zero length when there is none. */
static void overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1, uint64_t *from,
                    uint64_t *to)
{
  *from = a0 > b0 ? a0 : b0;
  *to = a1 < b1 ? a1 : b1;
  if (*to < *from)
  {
    *to = *from;
  }
}

int aud_calibrate_step(aud_calibrate *c, const float *captured, unsigned channels,
                       float *playback, unsigned play_channels, size_t frames)
{
  uint64_t start;
  uint64_t end;
  unsigned i;

  if (c == NULL || playback == NULL || play_channels == 0)
  {
    return 1;
  }

  start = c->frame;
  end = start + frames;
  c->emitting = 0;

  /*
   * Nothing else is meant to be coming out of the output while this runs, so
   * the period starts as silence and the burst is written into it.
   */
  memset(playback, 0, frames * play_channels * sizeof(*playback));

  for (i = 0; i < c->repeats; i++)
  {
    uint64_t at = burst_at(c, i);
    uint64_t from;
    uint64_t to;
    uint64_t f;

    overlap(at, at + c->burst_frames, start, end, &from, &to);
    if (to == from)
    {
      continue;
    }

    for (f = from; f < to; f++)
    {
      float sample = c->burst[f - at];
      size_t out = (size_t)(f - start) * play_channels;
      unsigned ch;

      /* the same burst to every channel: which ear it comes out of is nobody's
       * question here, and one channel carrying it would halve the return */
      for (ch = 0; ch < play_channels; ch++)
      {
        playback[out + ch] = sample;
      }
    }

    c->emitting = i + 1;
  }

  if (captured != NULL && channels > 0)
  {
    for (i = 0; i < c->repeats; i++)
    {
      uint64_t at = burst_at(c, i);
      float *window = c->window + (size_t)i * c->search_frames;
      uint64_t from;
      uint64_t to;
      uint64_t f;

      overlap(at, at + c->search_frames, start, end, &from, &to);
      if (to == from)
      {
        continue;
      }

      for (f = from; f < to; f++)
      {
        window[f - at] = mono_at(captured, channels, (size_t)(f - start));
      }
      c->slot[i].filled = (size_t)(to - at);
    }
  }

  c->frame = end;
  return aud_calibrate_finished(c);
}

void aud_calibrate_note_dropped(aud_calibrate *c, unsigned long dropped)
{
  if (c == NULL)
  {
    return;
  }

  if (dropped > c->dropped_seen && c->emitting > 0)
  {
    c->slot[c->emitting - 1].dropped = 1;
  }
  c->dropped_seen = dropped;
}

int aud_calibrate_finished(const aud_calibrate *c)
{
  uint64_t last;

  if (c == NULL)
  {
    return 1;
  }

  last = burst_at(c, c->repeats - 1) + c->search_frames;
  return c->frame >= last;
}

unsigned aud_calibrate_fired(const aud_calibrate *c)
{
  unsigned fired = 0;
  unsigned i;

  if (c == NULL)
  {
    return 0;
  }

  for (i = 0; i < c->repeats; i++)
  {
    if (c->frame >= burst_at(c, i) + c->burst_frames)
    {
      fired++;
    }
  }
  return fired;
}

int aud_calibrate_ready(const aud_calibrate *c, unsigned index)
{
  if (c == NULL || index >= c->repeats)
  {
    return 0;
  }

  return c->frame >= burst_at(c, index) + c->search_frames;
}

/*
 * Search slot `i` for the burst, once, and keep the answer.
 *
 * `final` says the run is over and this window is all there will ever be of it.
 * Without it a window still filling is left alone rather than searched, because
 * the answer is cached and a caller polling for progress would otherwise fix
 * the verdict on a burst from the half of its window that had arrived.
 */
static const cal_slot *slot_measure(aud_calibrate *c, unsigned i, int final)
{
  cal_slot *s = &c->slot[i];
  size_t at;
  size_t j;

  if (s->measured)
  {
    return s;
  }

  if (!final && s->filled < c->search_frames)
  {
    return s;
  }
  s->measured = 1;

  /* a window that never filled belongs to a burst the run did not reach */
  if (s->filled < c->burst_frames || s->dropped)
  {
    return s;
  }

  for (j = 0; j < s->filled; j++)
  {
    double magnitude = fabs((double)c->window[(size_t)i * c->search_frames + j]);

    if (magnitude > s->peak)
    {
      s->peak = magnitude;
    }
  }

  if (aud_format_dbfs(s->peak) < AUD_CALIBRATE_SILENCE_DBFS)
  {
    return s;
  }

  s->match = correlate(c->window + (size_t)i * c->search_frames, s->filled, c->burst,
                       c->burst_frames, c->burst_energy, &at);
  if (s->match < AUD_CALIBRATE_MIN_MATCH)
  {
    return s;
  }

  s->delay = at;
  s->found = 1;
  return s;
}

int aud_calibrate_reading(aud_calibrate *c, unsigned index, double *ms, double *match)
{
  const cal_slot *s;

  if (c == NULL || index >= c->repeats)
  {
    return -1;
  }

  s = slot_measure(c, index, 0);
  if (!s->found)
  {
    return -1;
  }

  if (ms != NULL)
  {
    *ms = 1000.0 * (double)s->delay / c->rate;
  }
  if (match != NULL)
  {
    *match = s->match;
  }
  return 0;
}

/* Insertion sort: at most AUD_CALIBRATE_MAX_REPEATS values, already near order. */
static void sort_doubles(double *v, unsigned n)
{
  unsigned i;

  for (i = 1; i < n; i++)
  {
    double key = v[i];
    unsigned j = i;

    while (j > 0 && v[j - 1] > key)
    {
      v[j] = v[j - 1];
      j--;
    }
    v[j] = key;
  }
}

void aud_calibrate_analyse(aud_calibrate *c, aud_calibrate_result *out)
{
  double reading[AUD_CALIBRATE_MAX_REPEATS];
  double sorted[AUD_CALIBRATE_MAX_REPEATS];
  double weakest = 1.0;
  double median;
  double total = 0.0;
  double lowest = 0.0;
  double highest = 0.0;
  unsigned readings = 0;
  unsigned kept = 0;
  unsigned silent = 0;
  unsigned dropped = 0;
  unsigned considered = 0;
  unsigned fired;
  unsigned i;

  if (out == NULL)
  {
    return;
  }

  memset(out, 0, sizeof(*out));
  out->peak_dbfs = AUD_DBFS_FLOOR;

  if (c == NULL)
  {
    out->verdict = AUD_CALIBRATE_SHORT;
    return;
  }

  fired = aud_calibrate_fired(c);
  out->fired = fired;

  for (i = 0; i < c->repeats; i++)
  {
    const cal_slot *s;

    /* a burst that never went out is not a burst that failed */
    if (c->frame < burst_at(c, i) + c->burst_frames)
    {
      continue;
    }

    s = slot_measure(c, i, 1);
    considered++;

    if (s->dropped)
    {
      dropped++;
      continue;
    }

    if (aud_format_dbfs(s->peak) > out->peak_dbfs)
    {
      out->peak_dbfs = aud_format_dbfs(s->peak);
    }

    if (s->found)
    {
      reading[readings++] = 1000.0 * (double)s->delay / c->rate;
    }
    else if (aud_format_dbfs(s->peak) < AUD_CALIBRATE_SILENCE_DBFS)
    {
      silent++;
    }
  }

  if (fired == 0 || considered == 0)
  {
    out->verdict = AUD_CALIBRATE_SHORT;
    return;
  }

  if (readings == 0)
  {
    /*
     * Three ways to have nothing, and they want different answers: the output
     * never played the bursts, the input never heard them, or it heard
     * something that was not them. Reported in that order because each is
     * upstream of the next - an output that dropped its bursts explains a
     * silent input, and a silent input explains a poor match.
     */
    if (dropped == considered)
    {
      out->verdict = AUD_CALIBRATE_DROPPED;
    }
    else if (silent > 0)
    {
      out->verdict = AUD_CALIBRATE_SILENT;
    }
    else
    {
      out->verdict = AUD_CALIBRATE_UNRECOGNISED;
    }
    return;
  }

  /*
   * The median first, and then everything near it. A false match lands wherever
   * the noise looked most like a sweep, which is nowhere in particular, so it
   * cannot drag a median the way it would drag a mean - and once the outliers
   * are gone the mean of what is left is the better estimate of the two.
   */
  memcpy(sorted, reading, readings * sizeof(*sorted));
  sort_doubles(sorted, readings);
  median = sorted[readings / 2];

  for (i = 0; i < c->repeats; i++)
  {
    const cal_slot *s = &c->slot[i];
    double ms;

    if (!s->found)
    {
      continue;
    }

    ms = 1000.0 * (double)s->delay / c->rate;
    if (fabs(ms - median) > AUD_CALIBRATE_OUTLIER_MS)
    {
      continue;
    }

    if (kept == 0 || ms < lowest)
    {
      lowest = ms;
    }
    if (kept == 0 || ms > highest)
    {
      highest = ms;
    }
    if (s->match < weakest)
    {
      weakest = s->match;
    }
    total += ms;
    kept++;
  }

  /* a minority agreeing is not a measurement, it is two of them disagreeing */
  if (kept * 2 <= readings)
  {
    out->verdict = AUD_CALIBRATE_UNSTEADY;
    out->taken = kept;
    out->spread_ms = highest - lowest;
    return;
  }

  out->verdict = AUD_CALIBRATE_OK;
  out->ms = total / kept;
  out->frames = (uint64_t)(out->ms * c->rate / 1000.0 + 0.5);
  out->spread_ms = highest - lowest;
  out->match = weakest;
  out->taken = kept;
}

const char *aud_calibrate_verdict_text(aud_calibrate_verdict verdict)
{
  switch (verdict)
  {
  case AUD_CALIBRATE_OK:
    return "measured";
  case AUD_CALIBRATE_SILENT:
    return "nothing came back - check the cable from the output to the input, and "
           "that the input is turned up";
  case AUD_CALIBRATE_UNRECOGNISED:
    return "something came back but it was not the burst - check that the output "
           "and the input are the two ends of one loop, and play nothing while it "
           "runs";
  case AUD_CALIBRATE_UNSTEADY:
    return "the readings did not agree with each other - something else was making "
           "a noise, or the machine could not keep both streams fed";
  case AUD_CALIBRATE_DROPPED:
    return "the output could not play the bursts - try a larger --period, or more "
           "--periods";
  case AUD_CALIBRATE_SHORT:
    return "the run ended before a burst had been fired";
  default:
    return "the measurement failed";
  }
}
