/* SPDX-License-Identifier: MIT */
#include "audio/spectral.h"

#include "audio/fft.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SPECTRAL_PI 3.14159265358979323846

/* Harmonics aud_spectral_find_hum() scores a candidate over. */
#define SPECTRAL_HUM_HARMONICS 8u

/*
 * Bins either side of a harmonic taken as "the surroundings" it has to stand
 * above. Skipping the three nearest keeps the peak's own skirt out of the
 * figure it is being compared with.
 */
#define SPECTRAL_HUM_NEAR 3u
#define SPECTRAL_HUM_FAR 10u

/* dB a harmonic must clear its surroundings by, averaged, to count as a hum. */
#define SPECTRAL_HUM_THRESHOLD_DB 6.0

struct aud_spectral
{
  unsigned rate;
  size_t size;
  size_t bins; /* size / 2 + 1 */
  size_t hop;

  /*
   * Magnitude scale. A full scale sine puts amplitude * sum(window) / 2 into
   * its bin and the same into the mirror of it, and a Hann window sums to
   * size / 2 - so 4 / size reads it as 1.0. DC and Nyquist have no mirror to
   * share with, so they are scaled by half that.
   */
  double norm;
  double norm_edge;

  float *window;
  float *re;
  float *im;
  float *gain; /* per bin, rebuilt for every window during a process */

  /* the reading */
  float *mean;
  float *peak;
  float *low;
  float *sum; /* running total behind `mean` while a read is open */
  size_t windows;
  int reading;
  int has_reading;

  /* the profile */
  float *noise;
  int has_noise;
  float strength;
  float floor_db;
  float floor_gain; /* floor_db as a linear gain, so the inner loop has no pow */

  float *curve;
};

static float clampf(float v, float lo, float hi)
{
  if (v < lo)
  {
    return lo;
  }
  if (v > hi)
  {
    return hi;
  }
  return v;
}

aud_spectral *aud_spectral_create(unsigned rate, size_t size)
{
  aud_spectral *s;

  if (rate < 4000u || !aud_fft_is_pow2(size) || size < AUD_SPECTRAL_OVERLAP * 2u)
  {
    errno = EINVAL;
    return NULL;
  }

  s = calloc(1, sizeof(*s));
  if (s == NULL)
  {
    errno = ENOMEM;
    return NULL;
  }

  s->rate = rate;
  s->size = size;
  s->bins = size / 2u + 1u;
  s->hop = size / AUD_SPECTRAL_OVERLAP;
  s->norm = 4.0 / (double)size;
  s->norm_edge = 2.0 / (double)size;

  s->window = calloc(size, sizeof(*s->window));
  s->re = calloc(size, sizeof(*s->re));
  s->im = calloc(size, sizeof(*s->im));
  s->gain = calloc(s->bins, sizeof(*s->gain));
  s->mean = calloc(s->bins, sizeof(*s->mean));
  s->peak = calloc(s->bins, sizeof(*s->peak));
  s->low = calloc(s->bins, sizeof(*s->low));
  s->sum = calloc(s->bins, sizeof(*s->sum));
  s->noise = calloc(s->bins, sizeof(*s->noise));
  s->curve = calloc(s->bins, sizeof(*s->curve));

  if (s->window == NULL || s->re == NULL || s->im == NULL || s->gain == NULL ||
      s->mean == NULL || s->peak == NULL || s->low == NULL || s->sum == NULL ||
      s->noise == NULL || s->curve == NULL)
  {
    aud_spectral_destroy(s);
    errno = ENOMEM;
    return NULL;
  }

  aud_fft_hann(s->window, size);
  aud_spectral_flatten(s);
  aud_spectral_set_reduction(s, AUD_SPECTRAL_DEFAULT_STRENGTH,
                             AUD_SPECTRAL_DEFAULT_FLOOR_DB);

  return s;
}

void aud_spectral_destroy(aud_spectral *s)
{
  if (s == NULL)
  {
    return;
  }

  free(s->window);
  free(s->re);
  free(s->im);
  free(s->gain);
  free(s->mean);
  free(s->peak);
  free(s->low);
  free(s->sum);
  free(s->noise);
  free(s->curve);
  free(s);
}

unsigned aud_spectral_rate(const aud_spectral *s)
{
  return s != NULL ? s->rate : 0u;
}

size_t aud_spectral_size(const aud_spectral *s)
{
  return s != NULL ? s->size : 0;
}

size_t aud_spectral_bins(const aud_spectral *s)
{
  return s != NULL ? s->bins : 0;
}

double aud_spectral_hz(const aud_spectral *s, size_t bin)
{
  if (s == NULL || bin >= s->bins)
  {
    return 0.0;
  }
  return (double)bin * (double)s->rate / (double)s->size;
}

size_t aud_spectral_bin_at(const aud_spectral *s, double hz)
{
  double bin;

  if (s == NULL)
  {
    return 0;
  }

  if (!(hz > 0.0))
  {
    return 0;
  }

  bin = hz * (double)s->size / (double)s->rate;
  if (bin >= (double)(s->bins - 1))
  {
    return s->bins - 1;
  }
  return (size_t)(bin + 0.5);
}

float aud_spectral_db(float magnitude)
{
  double db;

  if (!(magnitude > 0.0f))
  {
    return AUD_SPECTRAL_FLOOR_DB;
  }

  db = 20.0 * log10((double)magnitude);
  if (db < (double)AUD_SPECTRAL_FLOOR_DB)
  {
    return AUD_SPECTRAL_FLOOR_DB;
  }
  return (float)db;
}

/* -- taking a reading ------------------------------------------------------ */

void aud_spectral_read_begin(aud_spectral *s)
{
  if (s == NULL)
  {
    return;
  }

  memset(s->sum, 0, s->bins * sizeof(*s->sum));
  memset(s->peak, 0, s->bins * sizeof(*s->peak));
  for (size_t k = 0; k < s->bins; k++)
  {
    s->low[k] = FLT_MAX;
  }
  s->windows = 0;
  s->reading = 1;
}

/* The scale bin `k` is read at, which differs at the two ends. See `norm`. */
static double bin_scale(const aud_spectral *s, size_t k)
{
  return (k == 0 || k == s->size / 2u) ? s->norm_edge : s->norm;
}

void aud_spectral_read(aud_spectral *s, const float *mono, size_t frames)
{
  if (s == NULL || mono == NULL || !s->reading || frames == 0)
  {
    return;
  }

  if (frames > s->size)
  {
    frames = s->size;
  }

  for (size_t i = 0; i < s->size; i++)
  {
    s->re[i] = i < frames ? mono[i] * s->window[i] : 0.0f;
    s->im[i] = 0.0f;
  }

  aud_fft_forward(s->re, s->im, s->size);

  for (size_t k = 0; k < s->bins; k++)
  {
    float mag = (float)((double)aud_fft_magnitude(s->re[k], s->im[k]) * bin_scale(s, k));

    s->sum[k] += mag;
    if (mag > s->peak[k])
    {
      s->peak[k] = mag;
    }
    if (mag < s->low[k])
    {
      s->low[k] = mag;
    }
  }

  s->windows++;
}

void aud_spectral_read_end(aud_spectral *s)
{
  if (s == NULL || !s->reading)
  {
    return;
  }

  s->reading = 0;

  if (s->windows == 0)
  {
    memset(s->mean, 0, s->bins * sizeof(*s->mean));
    memset(s->peak, 0, s->bins * sizeof(*s->peak));
    memset(s->low, 0, s->bins * sizeof(*s->low));
    s->has_reading = 0;
    return;
  }

  for (size_t k = 0; k < s->bins; k++)
  {
    s->mean[k] = s->sum[k] / (float)s->windows;
  }

  s->has_reading = 1;
}

int aud_spectral_has_reading(const aud_spectral *s)
{
  return s != NULL && s->has_reading;
}

size_t aud_spectral_windows(const aud_spectral *s)
{
  return s != NULL ? s->windows : 0;
}

const float *aud_spectral_mean(const aud_spectral *s)
{
  return s != NULL ? s->mean : NULL;
}

const float *aud_spectral_peak(const aud_spectral *s)
{
  return s != NULL ? s->peak : NULL;
}

const float *aud_spectral_low(const aud_spectral *s)
{
  return s != NULL ? s->low : NULL;
}

/* -- the noise profile ----------------------------------------------------- */

void aud_spectral_learn_noise(aud_spectral *s)
{
  if (s == NULL || !s->has_reading)
  {
    return;
  }

  memcpy(s->noise, s->mean, s->bins * sizeof(*s->noise));
  s->has_noise = 1;
}

void aud_spectral_guess_noise(aud_spectral *s)
{
  if (s == NULL || !s->has_reading)
  {
    return;
  }

  memcpy(s->noise, s->low, s->bins * sizeof(*s->noise));
  s->has_noise = 1;
}

int aud_spectral_has_noise(const aud_spectral *s)
{
  return s != NULL && s->has_noise;
}

void aud_spectral_forget_noise(aud_spectral *s)
{
  if (s == NULL)
  {
    return;
  }

  memset(s->noise, 0, s->bins * sizeof(*s->noise));
  s->has_noise = 0;
}

const float *aud_spectral_noise(const aud_spectral *s)
{
  if (s == NULL || !s->has_noise)
  {
    return NULL;
  }
  return s->noise;
}

void aud_spectral_set_reduction(aud_spectral *s, float strength, float floor_db)
{
  if (s == NULL)
  {
    return;
  }

  s->strength = clampf(strength, 0.0f, AUD_SPECTRAL_STRENGTH_MAX);
  s->floor_db = clampf(floor_db, AUD_SPECTRAL_FLOOR_MIN_DB, AUD_SPECTRAL_FLOOR_MAX_DB);
  s->floor_gain = (float)pow(10.0, (double)s->floor_db / 20.0);
}

float aud_spectral_strength(const aud_spectral *s)
{
  return s != NULL ? s->strength : 0.0f;
}

float aud_spectral_floor_db(const aud_spectral *s)
{
  return s != NULL ? s->floor_db : 0.0f;
}

/* -- the curve ------------------------------------------------------------- */

const float *aud_spectral_curve(const aud_spectral *s)
{
  return s != NULL ? s->curve : NULL;
}

void aud_spectral_flatten(aud_spectral *s)
{
  if (s == NULL)
  {
    return;
  }

  for (size_t k = 0; k < s->bins; k++)
  {
    s->curve[k] = 1.0f;
  }
}

int aud_spectral_would_change(const aud_spectral *s)
{
  if (s == NULL)
  {
    return 0;
  }

  if (s->has_noise && s->strength > 0.0f)
  {
    return 1;
  }

  for (size_t k = 0; k < s->bins; k++)
  {
    if (s->curve[k] != 1.0f)
    {
      return 1;
    }
  }
  return 0;
}

void aud_spectral_paint(aud_spectral *s, double lo_hz, double hi_hz, float gain)
{
  size_t lo;
  size_t hi;

  if (s == NULL)
  {
    return;
  }

  if (hi_hz < lo_hz)
  {
    double swap = lo_hz;
    lo_hz = hi_hz;
    hi_hz = swap;
  }

  lo = aud_spectral_bin_at(s, lo_hz);
  hi = aud_spectral_bin_at(s, hi_hz);
  gain = clampf(gain, 0.0f, 4.0f);

  for (size_t k = lo; k <= hi; k++)
  {
    s->curve[k] = gain;
  }

  /*
   * Ease out into whatever the curve already said either side, rather than
   * stepping to it. See the note in the header: a step in frequency is a ring
   * in time, and the ring is audible where the step is not.
   */
  for (size_t e = 1; e <= AUD_SPECTRAL_EDGE_BINS; e++)
  {
    float t = (float)(0.5 * (1.0 + cos(SPECTRAL_PI * (double)e /
                                       (double)(AUD_SPECTRAL_EDGE_BINS + 1u))));

    if (lo >= e)
    {
      size_t k = lo - e;
      s->curve[k] += (gain - s->curve[k]) * t;
    }
    if (hi + e < s->bins)
    {
      size_t k = hi + e;
      s->curve[k] += (gain - s->curve[k]) * t;
    }
  }
}

/* The gain bin `k` needs to read as `magnitude`, or unity if it already does. */
static float pull_gain(const aud_spectral *s, size_t k, float magnitude)
{
  float have = s->mean[k];

  if (!(have > magnitude) || !(have > 0.0f))
  {
    return 1.0f;
  }
  return magnitude / have;
}

void aud_spectral_pull_down(aud_spectral *s, double lo_hz, double hi_hz, float magnitude)
{
  size_t lo;
  size_t hi;

  if (s == NULL || !s->has_reading)
  {
    return;
  }

  if (hi_hz < lo_hz)
  {
    double swap = lo_hz;
    lo_hz = hi_hz;
    hi_hz = swap;
  }

  lo = aud_spectral_bin_at(s, lo_hz);
  hi = aud_spectral_bin_at(s, hi_hz);
  if (magnitude < 0.0f)
  {
    magnitude = 0.0f;
  }

  for (size_t k = lo; k <= hi; k++)
  {
    float g = pull_gain(s, k, magnitude);

    if (g < s->curve[k])
    {
      s->curve[k] = g;
    }
  }

  for (size_t e = 1; e <= AUD_SPECTRAL_EDGE_BINS; e++)
  {
    float t = (float)(0.5 * (1.0 + cos(SPECTRAL_PI * (double)e /
                                       (double)(AUD_SPECTRAL_EDGE_BINS + 1u))));

    if (lo >= e)
    {
      size_t k = lo - e;
      float want = s->curve[k] + (pull_gain(s, k, magnitude) - s->curve[k]) * t;

      if (want < s->curve[k])
      {
        s->curve[k] = want;
      }
    }
    if (hi + e < s->bins)
    {
      size_t k = hi + e;
      float want = s->curve[k] + (pull_gain(s, k, magnitude) - s->curve[k]) * t;

      if (want < s->curve[k])
      {
        s->curve[k] = want;
      }
    }
  }
}

void aud_spectral_notch(aud_spectral *s, double hz, double width_hz, unsigned harmonics,
                        float gain)
{
  double nyquist;

  if (s == NULL || !(hz > 0.0))
  {
    return;
  }

  if (!(width_hz > 0.0))
  {
    width_hz = (double)s->rate / (double)s->size; /* one bin */
  }
  if (harmonics == 0u)
  {
    harmonics = 1u;
  }

  nyquist = (double)s->rate / 2.0;

  for (unsigned h = 1u; h <= harmonics; h++)
  {
    double centre = hz * (double)h;

    if (centre >= nyquist)
    {
      break; /* the rest of the series is off the end, not folded back onto it */
    }
    aud_spectral_paint(s, centre - width_hz / 2.0, centre + width_hz / 2.0, gain);
  }
}

/*
 * The level around bin `k`, skipping the peak's own skirt: the mean of the
 * bins from SPECTRAL_HUM_NEAR to SPECTRAL_HUM_FAR away on both sides.
 */
static double surroundings(const float *v, size_t bins, size_t k)
{
  double total = 0.0;
  size_t count = 0;

  for (size_t d = SPECTRAL_HUM_NEAR; d <= SPECTRAL_HUM_FAR; d++)
  {
    if (k >= d)
    {
      total += (double)v[k - d];
      count++;
    }
    if (k + d < bins)
    {
      total += (double)v[k + d];
      count++;
    }
  }

  return count > 0 ? total / (double)count : 0.0;
}

double aud_spectral_find_hum(const aud_spectral *s)
{
  size_t first;
  size_t last;
  double best_score = 0.0;
  double best_hz = 0.0;

  if (s == NULL || !s->has_reading)
  {
    return 0.0;
  }

  first = aud_spectral_bin_at(s, AUD_SPECTRAL_HUM_MIN_HZ);
  last = aud_spectral_bin_at(s, AUD_SPECTRAL_HUM_MAX_HZ);
  if (first < 1)
  {
    first = 1;
  }

  for (size_t k = first; k <= last && k < s->bins; k++)
  {
    double score = 0.0;
    size_t counted = 0;

    for (unsigned h = 1u; h <= SPECTRAL_HUM_HARMONICS; h++)
    {
      size_t at = k * h;
      double here;
      double around;

      if (at >= s->bins)
      {
        break;
      }

      /*
       * The `low` reading, which is where this works at all: a hum is in every
       * window and so cannot fall below itself, while a bass note is in some
       * of them and falls to the noise floor in the rest.
       */
      here = (double)s->low[at];
      if (at > 0 && (double)s->low[at - 1] > here)
      {
        here = (double)s->low[at - 1];
      }
      if (at + 1 < s->bins && (double)s->low[at + 1] > here)
      {
        here = (double)s->low[at + 1];
      }

      around = surroundings(s->low, s->bins, at);
      score +=
          (double)aud_spectral_db((float)here) - (double)aud_spectral_db((float)around);
      counted++;
    }

    if (counted < 2)
    {
      continue;
    }

    score /= (double)counted;
    if (score > best_score)
    {
      best_score = score;
      best_hz = aud_spectral_hz(s, k);
    }
  }

  return best_score >= SPECTRAL_HUM_THRESHOLD_DB ? best_hz : 0.0;
}

/* -- what it would come to ------------------------------------------------- */

/*
 * How far the profile pulls bin `k` down, given that the bin currently holds
 * `mag`. Shared by the resynthesis and by the predicted curve the graph draws,
 * so what is on screen is what will happen.
 */
static double reduce(const aud_spectral *s, size_t k, double mag)
{
  double excess;
  double g;

  if (!s->has_noise || !(s->strength > 0.0f) || !(mag > 0.0))
  {
    return 1.0;
  }

  excess = mag - (double)s->strength * (double)s->noise[k];
  g = excess > 0.0 ? excess / mag : 0.0;

  return g < (double)s->floor_gain ? (double)s->floor_gain : g;
}

void aud_spectral_result(const aud_spectral *s, float *out)
{
  if (s == NULL || out == NULL)
  {
    return;
  }

  for (size_t k = 0; k < s->bins; k++)
  {
    double mag = (double)s->mean[k];

    out[k] = (float)(mag * (double)s->curve[k] * reduce(s, k, mag));
  }
}

/* -- putting the audio back together --------------------------------------- */

size_t aud_spectral_context(const aud_spectral *s)
{
  return s != NULL ? s->size : 0;
}

/*
 * Smooth the gains across neighbouring bins, in place.
 *
 * Subtraction decides each bin on its own, so a bin that happened to fall just
 * under the profile is silenced between two that did not - and a lone silent
 * bin among loud ones is a tone that fades in and out, which is the warbling
 * that spectral subtraction is known for. Three taps is enough to take the
 * worst of it off. `prev` carries the unsmoothed left neighbour so the pass can
 * be done in place without a second array.
 */
static void smooth_gains(aud_spectral *s)
{
  size_t half = s->size / 2u;
  float prev = s->gain[0];

  for (size_t k = 1; k < half; k++)
  {
    float here = s->gain[k];

    s->gain[k] = (prev + here + s->gain[k + 1]) / 3.0f;
    prev = here;
  }
}

/* Scale the bins of the window now in re[]/im[] by the curve and the profile. */
static void apply_gains(aud_spectral *s)
{
  size_t n = s->size;
  size_t half = n / 2u;

  for (size_t k = 0; k <= half; k++)
  {
    double mag = (double)aud_fft_magnitude(s->re[k], s->im[k]) * bin_scale(s, k);

    s->gain[k] = (float)((double)s->curve[k] * reduce(s, k, mag));
  }

  smooth_gains(s);

  for (size_t k = 0; k <= half; k++)
  {
    float g = s->gain[k];

    s->re[k] *= g;
    s->im[k] *= g;

    /* the mirror bin, which carries the other half of a real signal's energy */
    if (k > 0 && k < half)
    {
      s->re[n - k] *= g;
      s->im[n - k] *= g;
    }
  }
}

int aud_spectral_process(aud_spectral *s, const float *in, float *out, size_t frames,
                         unsigned channels)
{
  float *acc;
  float *cover;
  size_t n;
  size_t hop;

  if (s == NULL || in == NULL || out == NULL || frames == 0 || channels == 0)
  {
    errno = EINVAL;
    return -1;
  }

  n = s->size;
  hop = s->hop;

  acc = calloc(frames, sizeof(*acc));
  cover = calloc(frames, sizeof(*cover));
  if (acc == NULL || cover == NULL)
  {
    free(acc);
    free(cover);
    errno = ENOMEM;
    return -1;
  }

  for (unsigned c = 0; c < channels; c++)
  {
    memset(acc, 0, frames * sizeof(*acc));
    memset(cover, 0, frames * sizeof(*cover));

    /*
     * The first window starts before the buffer does, so that sample 0 is
     * covered by as many windows as any sample in the middle. What lands
     * outside is read as silence and thrown away again below.
     */
    for (ptrdiff_t start = -(ptrdiff_t)(n - hop); start < (ptrdiff_t)frames;
         start += (ptrdiff_t)hop)
    {
      for (size_t i = 0; i < n; i++)
      {
        ptrdiff_t at = start + (ptrdiff_t)i;
        float x = 0.0f;

        if (at >= 0 && at < (ptrdiff_t)frames)
        {
          x = in[(size_t)at * channels + c];
        }
        s->re[i] = x * s->window[i];
        s->im[i] = 0.0f;
      }

      aud_fft_forward(s->re, s->im, n);
      apply_gains(s);
      aud_fft_inverse(s->re, s->im, n);

      for (size_t i = 0; i < n; i++)
      {
        ptrdiff_t at = start + (ptrdiff_t)i;

        if (at < 0 || at >= (ptrdiff_t)frames)
        {
          continue;
        }
        acc[at] += s->re[i] * s->window[i];
        cover[at] += s->window[i] * s->window[i];
      }
    }

    /*
     * Divided by the window energy that actually landed here rather than by
     * the constant it comes to in the middle, so the two ends come back at
     * full level instead of fading in and out of the edit.
     *
     * Written after every window of this channel has been read, which is what
     * makes `in == out` safe: a channel's output frames are exactly its input
     * frames, and no channel touches another's.
     */
    for (size_t i = 0; i < frames; i++)
    {
      out[i * channels + c] = cover[i] > 1e-6f ? acc[i] / cover[i] : in[i * channels + c];
    }
  }

  free(acc);
  free(cover);
  return 0;
}
