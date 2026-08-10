/* SPDX-License-Identifier: MIT */
#include "audio/fft.h"

#include <math.h>

/*
 * M_PI is an X/Open extension, and this project compiles with
 * _POSIX_C_SOURCE alone, so define the constant rather than hope for it.
 */
#define AUD_PI 3.14159265358979323846

int aud_fft_is_pow2(size_t n)
{
  return n >= 2 && (n & (n - 1)) == 0;
}

static unsigned log2_exact(size_t n)
{
  unsigned bits = 0;

  while ((n >> bits) > 1u)
  {
    bits++;
  }
  return bits;
}

static size_t reverse_bits(size_t value, unsigned bits)
{
  size_t out = 0;

  for (unsigned i = 0; i < bits; i++)
  {
    out = (out << 1) | ((value >> i) & (size_t)1);
  }
  return out;
}

void aud_fft_forward(float *re, float *im, size_t n)
{
  unsigned bits;

  if (re == NULL || im == NULL || !aud_fft_is_pow2(n))
  {
    return;
  }

  bits = log2_exact(n);

  /* Decimation in time: permute into bit-reversed order, then butterfly. */
  for (size_t i = 0; i < n; i++)
  {
    size_t j = reverse_bits(i, bits);
    if (j > i)
    {
      float tr = re[i];
      float ti = im[i];
      re[i] = re[j];
      im[i] = im[j];
      re[j] = tr;
      im[j] = ti;
    }
  }

  for (size_t len = 2; len <= n; len <<= 1)
  {
    size_t half = len / 2;

    /*
     * The twiddle loop sits outside the block loop so each factor is
     * evaluated with cos/sin exactly once - n-1 trig calls per transform.
     * The usual recurrence is cheaper but drifts noticeably by n = 2048.
     */
    for (size_t k = 0; k < half; k++)
    {
      double angle = -2.0 * AUD_PI * (double)k / (double)len;
      float wr = (float)cos(angle);
      float wi = (float)sin(angle);

      for (size_t start = 0; start < n; start += len)
      {
        size_t i = start + k;
        size_t j = i + half;
        float xr = re[j] * wr - im[j] * wi;
        float xi = re[j] * wi + im[j] * wr;

        re[j] = re[i] - xr;
        im[j] = im[i] - xi;
        re[i] += xr;
        im[i] += xi;
      }
    }
  }
}

void aud_fft_inverse(float *re, float *im, size_t n)
{
  float scale;

  if (re == NULL || im == NULL || !aud_fft_is_pow2(n))
  {
    return;
  }

  /*
   * Conjugate, transform forward, conjugate again. The inverse DFT differs
   * from the forward only in the sign of its exponent, and that identity gets
   * it out of the one butterfly loop above rather than out of a second copy of
   * it that could drift away from the first.
   */
  for (size_t i = 0; i < n; i++)
  {
    im[i] = -im[i];
  }

  aud_fft_forward(re, im, n);

  scale = 1.0f / (float)n;
  for (size_t i = 0; i < n; i++)
  {
    re[i] *= scale;
    im[i] *= -scale;
  }
}

void aud_fft_hann(float *window, size_t n)
{
  if (window == NULL || n == 0)
  {
    return;
  }

  if (n == 1)
  {
    window[0] = 1.0f;
    return;
  }

  for (size_t i = 0; i < n; i++)
  {
    window[i] = (float)(0.5 * (1.0 - cos(2.0 * AUD_PI * (double)i / (double)n)));
  }
}

float aud_fft_magnitude(float re, float im)
{
  return (float)sqrt((double)re * (double)re + (double)im * (double)im);
}
