/* SPDX-License-Identifier: MIT */
#include "audio/resample.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Taps either side of the output point. Sixteen puts the stopband far enough
 * down that the images and the fold-back are inaudible under music, and keeps
 * the per-sample cost at 32 multiply-adds - which at 48 kHz stereo is a couple
 * of million a second, well under what the monitor thread has spare.
 */
#define RS_TAPS 16u
#define RS_WIDTH (2u * RS_TAPS)

/*
 * Sub-sample positions the filter is precomputed at. The remaining fraction is
 * interpolated between two of them, so the error is a 1/512th of a sample of
 * timing rather than anything spectral.
 */
#define RS_PHASES 512u

/* Input frames converted per pass, which is what the scratch buffer holds. */
#define RS_CHUNK 1024u

#define RS_PI 3.14159265358979323846

struct aud_resampler
{
  unsigned in_rate;
  unsigned out_rate;
  unsigned channels;
  double step; /* input frames per output frame */
  double pos;  /* where the next output sample falls, in `hist` coordinates */

  float *table; /* RS_PHASES + 1 rows of RS_WIDTH taps */
  float *hist;  /* RS_WIDTH frames of tail from the last call */
  float *work;  /* hist followed by up to RS_CHUNK frames of new input */
  size_t work_frames;
};

static double sinc(double x)
{
  if (x > -1e-9 && x < 1e-9)
  {
    return 1.0;
  }
  return sin(RS_PI * x) / (RS_PI * x);
}

/*
 * Blackman, over the whole 2*RS_TAPS width. Chosen over a plain Hann for the
 * stopband: about -74 dB against -31, which is the difference between a fold
 * you cannot hear and one you can.
 */
static double window(double x)
{
  double t = (x + (double)RS_TAPS) / (double)RS_WIDTH;

  if (t < 0.0 || t > 1.0)
  {
    return 0.0;
  }
  return 0.42 - 0.5 * cos(2.0 * RS_PI * t) + 0.08 * cos(4.0 * RS_PI * t);
}

/*
 * Build the filter, one row per sub-sample position.
 *
 * `cutoff` is a fraction of the input Nyquist. Going up it stays at 1.0 - there
 * is nothing above the input's own Nyquist to remove - and going down it
 * follows the output rate, which is what stops the material between the two
 * Nyquists folding back into the audible band.
 *
 * Each row is normalised to sum to one, so the converter neither lifts nor
 * drops the level. Computed rather than derived from the row before it: the
 * error in a running product would show up as a slow tilt across the band.
 */
static int build_table(aud_resampler *rs)
{
  double cutoff =
      rs->out_rate < rs->in_rate ? (double)rs->out_rate / (double)rs->in_rate : 1.0;

  rs->table = malloc((size_t)(RS_PHASES + 1u) * RS_WIDTH * sizeof(*rs->table));
  if (rs->table == NULL)
  {
    return -1;
  }

  for (unsigned p = 0; p <= RS_PHASES; p++)
  {
    double frac = (double)p / (double)RS_PHASES;
    float *row = rs->table + (size_t)p * RS_WIDTH;
    double sum = 0.0;

    for (unsigned t = 0; t < RS_WIDTH; t++)
    {
      /* distance from the output point to the input sample this tap reads */
      double x = (double)t - (double)RS_TAPS + 1.0 - frac;
      double h = cutoff * sinc(cutoff * x) * window(x);

      row[t] = (float)h;
      sum += h;
    }

    if (sum > 1e-12 || sum < -1e-12)
    {
      for (unsigned t = 0; t < RS_WIDTH; t++)
      {
        row[t] = (float)((double)row[t] / sum);
      }
    }
  }
  return 0;
}

aud_resampler *aud_resample_create(unsigned in_rate, unsigned out_rate, unsigned channels)
{
  aud_resampler *rs;

  if (in_rate == 0 || out_rate == 0 || channels == 0 || channels > 64u)
  {
    errno = EINVAL;
    return NULL;
  }

  rs = calloc(1, sizeof(*rs));
  if (rs == NULL)
  {
    errno = ENOMEM;
    return NULL;
  }

  rs->in_rate = in_rate;
  rs->out_rate = out_rate;
  rs->channels = channels;
  rs->step = (double)in_rate / (double)out_rate;

  rs->hist = calloc((size_t)RS_WIDTH * channels, sizeof(*rs->hist));
  rs->work = calloc((size_t)(RS_WIDTH + RS_CHUNK) * channels, sizeof(*rs->work));
  if (rs->hist == NULL || rs->work == NULL || build_table(rs) != 0)
  {
    aud_resample_destroy(rs);
    errno = ENOMEM;
    return NULL;
  }

  aud_resample_reset(rs);
  return rs;
}

void aud_resample_destroy(aud_resampler *rs)
{
  if (rs == NULL)
  {
    return;
  }
  free(rs->table);
  free(rs->hist);
  free(rs->work);
  free(rs);
}

void aud_resample_reset(aud_resampler *rs)
{
  if (rs == NULL)
  {
    return;
  }

  memset(rs->hist, 0, (size_t)RS_WIDTH * rs->channels * sizeof(*rs->hist));
  rs->work_frames = 0;
  /*
   * Chosen so the delay through the filter is exactly RS_TAPS frames, which is
   * what aud_resample_latency() reports. The history is RS_WIDTH frames of
   * zeros ahead of the first real input, the peak tap sits RS_TAPS - 1 in from
   * the left of the window, and starting here puts the two together: output n
   * comes out carrying input n - RS_TAPS.
   *
   * One less than this also satisfies the taps, and was what this did first -
   * at the price of a delay of RS_TAPS + 1, which made the number the header
   * promises wrong by a sample.
   */
  rs->pos = (double)RS_TAPS;
}

size_t aud_resample_latency(const aud_resampler *rs)
{
  return rs != NULL ? RS_TAPS : 0;
}

size_t aud_resample_out_max(const aud_resampler *rs, size_t in_frames)
{
  double exact;

  if (rs == NULL)
  {
    return 0;
  }

  /*
   * Where the output grid falls carries over between calls, so a given call
   * can produce one more frame than the ratio alone suggests. The spare frame
   * is what keeps a caller's buffer from being one short exactly once in a
   * while.
   */
  exact = (double)in_frames * (double)rs->out_rate / (double)rs->in_rate;
  return (size_t)exact + 2u;
}

/* One output frame, from the taps around `at` in `src`. */
static void tap(const aud_resampler *rs, const float *src, size_t base, double frac,
                float *out)
{
  unsigned channels = rs->channels;
  /* between two precomputed rows, so the timing error is a fraction of a step */
  double scaled = frac * (double)RS_PHASES;
  unsigned p = (unsigned)scaled;
  float blend = (float)(scaled - (double)p);
  const float *row = rs->table + (size_t)p * RS_WIDTH;
  const float *next = row + RS_WIDTH;

  for (unsigned c = 0; c < channels; c++)
  {
    float sum = 0.0f;
    const float *x = src + base * channels + c;

    for (unsigned t = 0; t < RS_WIDTH; t++)
    {
      float h = row[t] + (next[t] - row[t]) * blend;

      sum += h * x[(size_t)t * channels];
    }
    out[c] = sum;
  }
}

size_t aud_resample_run(aud_resampler *rs, const float *in, size_t in_frames, float *out,
                        size_t out_cap)
{
  size_t done_in = 0;
  size_t done_out = 0;
  unsigned channels;

  if (rs == NULL || out == NULL || (in == NULL && in_frames > 0))
  {
    return 0;
  }
  channels = rs->channels;

  while (done_in < in_frames)
  {
    size_t take = in_frames - done_in;
    size_t have;
    size_t keep_from;

    if (take > RS_CHUNK)
    {
      take = RS_CHUNK;
    }

    /* the tail of the last pass, then this slice of new input */
    memcpy(rs->work, rs->hist, (size_t)RS_WIDTH * channels * sizeof(*rs->work));
    memcpy(rs->work + (size_t)RS_WIDTH * channels, in + done_in * channels,
           take * channels * sizeof(*rs->work));
    have = RS_WIDTH + take;

    /*
     * Produce while the rightmost tap still has real input under it. The
     * leftmost is guaranteed by pos never dropping below RS_TAPS - 1.
     */
    for (;;)
    {
      double whole = floor(rs->pos);
      size_t base;

      if (whole < (double)(RS_TAPS - 1u))
      {
        break; /* cannot happen; the history is exactly this deep */
      }
      base = (size_t)whole - (RS_TAPS - 1u);
      if (base + RS_WIDTH > have || done_out >= out_cap)
      {
        break;
      }

      tap(rs, rs->work, base, rs->pos - whole, out + done_out * channels);
      done_out++;
      rs->pos += rs->step;
    }

    /* carry the last RS_WIDTH frames over, and move pos into their coordinates */
    keep_from = have - RS_WIDTH;
    memcpy(rs->hist, rs->work + keep_from * channels,
           (size_t)RS_WIDTH * channels * sizeof(*rs->hist));
    rs->pos -= (double)keep_from;

    done_in += take;
  }

  return done_out;
}
