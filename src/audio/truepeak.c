/* SPDX-License-Identifier: MIT */
#include "audio/truepeak.h"

#include <math.h>
#include <stddef.h>

#define TP_PI 3.14159265358979323846

static double sinc(double x)
{
  if (x > -1e-9 && x < 1e-9)
  {
    return 1.0;
  }
  return sin(TP_PI * x) / (TP_PI * x);
}

/* Blackman, the same window resample.c interpolates through and for the same
 * reason: the stopband is where the images of the oversampled signal land. */
static double window_at(double x)
{
  double t = (x + (double)(AUD_TRUEPEAK_TAPS / 2u)) / (double)AUD_TRUEPEAK_TAPS;

  if (t < 0.0 || t > 1.0)
  {
    return 0.0;
  }
  return 0.42 - 0.5 * cos(2.0 * TP_PI * t) + 0.08 * cos(4.0 * TP_PI * t);
}

/*
 * One row a phase, each normalised to sum to one so the interpolator neither
 * lifts nor drops the level - which matters more here than anywhere, since
 * everything downstream of it is a level.
 *
 * `bound` falls out of the same loop: the largest a row can make any window is
 * the sum of its magnitudes times the largest sample in that window.
 */
void aud_truepeak_build(aud_truepeak *f)
{
  if (f == NULL)
  {
    return;
  }

  f->bound = 0.0;

  for (unsigned p = 0; p < AUD_TRUEPEAK_PHASES; p++)
  {
    double frac = (double)p / (double)AUD_TRUEPEAK_PHASES;
    double sum = 0.0;
    double magnitude = 0.0;

    for (unsigned t = 0; t < AUD_TRUEPEAK_TAPS; t++)
    {
      /* distance from the point being interpolated to the sample this tap reads */
      double x = (double)t - (double)(AUD_TRUEPEAK_TAPS / 2u) + 1.0 - frac;
      double h = sinc(x) * window_at(x);

      f->tap[p][t] = (float)h;
      sum += h;
    }

    for (unsigned t = 0; t < AUD_TRUEPEAK_TAPS; t++)
    {
      f->tap[p][t] = (float)((double)f->tap[p][t] / sum);
      magnitude += fabs((double)f->tap[p][t]);
    }

    if (magnitude > f->bound)
    {
      f->bound = magnitude;
    }
  }
}

float aud_truepeak_between(const aud_truepeak *f, const float *window)
{
  float best = 0.0f;

  if (f == NULL || window == NULL)
  {
    return 0.0f;
  }

  for (unsigned p = 1; p < AUD_TRUEPEAK_PHASES; p++)
  {
    const float *taps = f->tap[p];
    float sum = 0.0f;

    for (unsigned t = 0; t < AUD_TRUEPEAK_TAPS; t++)
    {
      sum += taps[t] * window[t];
    }
    if (fabsf(sum) > best)
    {
      best = fabsf(sum);
    }
  }

  return best;
}
