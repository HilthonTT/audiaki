/* SPDX-License-Identifier: MIT */
#include "audio/convolve.h"

#include "audio/fft.h"

#include <stdlib.h>
#include <string.h>

struct aud_convolver
{
  size_t block;
  size_t size;
  size_t bins;
  size_t parts;
  unsigned channels;
  size_t head;

  float *h_re;
  float *h_im;
  float *x_re;
  float *x_im;
  float *prev;
  float *cur;
  float *re;
  float *im;

  float *q_in;
  float *q_out;
  size_t q_fill;
};

static float *zeros(size_t count)
{
  return calloc(count, sizeof(float));
}

static size_t spectrum_at(const aud_convolver *cv, unsigned ch, size_t part)
{
  return ((size_t)ch * cv->parts + part) * cv->bins;
}

static void load_partitions(aud_convolver *cv, const float *ir, size_t ir_frames,
                            unsigned ir_channels)
{
  for (unsigned ch = 0; ch < cv->channels; ch++)
  {
    unsigned from = ch % ir_channels;

    for (size_t p = 0; p < cv->parts; p++)
    {
      size_t at = spectrum_at(cv, ch, p);

      memset(cv->re, 0, cv->size * sizeof(float));
      memset(cv->im, 0, cv->size * sizeof(float));
      for (size_t i = 0; i < cv->block; i++)
      {
        size_t frame = p * cv->block + i;

        if (frame >= ir_frames)
        {
          break;
        }
        cv->re[i] = ir[frame * ir_channels + from];
      }

      aud_fft_forward(cv->re, cv->im, cv->size);
      memcpy(cv->h_re + at, cv->re, cv->bins * sizeof(float));
      memcpy(cv->h_im + at, cv->im, cv->bins * sizeof(float));
    }
  }
}

aud_convolver *aud_convolve_create(const float *ir, size_t ir_frames,
                                   unsigned ir_channels, unsigned channels, size_t block)
{
  aud_convolver *cv;
  size_t spectra;

  if (ir == NULL || ir_frames == 0 || ir_channels == 0 || channels == 0 ||
      !aud_fft_is_pow2(block))
  {
    return NULL;
  }

  cv = calloc(1, sizeof(*cv));
  if (cv == NULL)
  {
    return NULL;
  }

  cv->block = block;
  cv->size = 2u * block;
  cv->bins = block + 1u;
  cv->parts = (ir_frames + block - 1u) / block;
  cv->channels = channels;

  spectra = (size_t)channels * cv->parts * cv->bins;
  cv->h_re = zeros(spectra);
  cv->h_im = zeros(spectra);
  cv->x_re = zeros(spectra);
  cv->x_im = zeros(spectra);
  cv->prev = zeros((size_t)channels * block);
  cv->cur = zeros((size_t)channels * block);
  cv->re = zeros(cv->size);
  cv->im = zeros(cv->size);
  cv->q_in = zeros((size_t)channels * block);
  cv->q_out = zeros((size_t)channels * block);

  if (cv->h_re == NULL || cv->h_im == NULL || cv->x_re == NULL || cv->x_im == NULL ||
      cv->prev == NULL || cv->cur == NULL || cv->re == NULL || cv->im == NULL ||
      cv->q_in == NULL || cv->q_out == NULL)
  {
    aud_convolve_destroy(cv);
    return NULL;
  }

  load_partitions(cv, ir, ir_frames, ir_channels);
  return cv;
}

void aud_convolve_destroy(aud_convolver *cv)
{
  if (cv == NULL)
  {
    return;
  }

  free(cv->h_re);
  free(cv->h_im);
  free(cv->x_re);
  free(cv->x_im);
  free(cv->prev);
  free(cv->cur);
  free(cv->re);
  free(cv->im);
  free(cv->q_in);
  free(cv->q_out);
  free(cv);
}

size_t aud_convolve_block(const aud_convolver *cv)
{
  return cv != NULL ? cv->block : 0;
}

size_t aud_convolve_partitions(const aud_convolver *cv)
{
  return cv != NULL ? cv->parts : 0;
}

void aud_convolve_reset(aud_convolver *cv)
{
  size_t spectra;

  if (cv == NULL)
  {
    return;
  }

  spectra = (size_t)cv->channels * cv->parts * cv->bins;
  memset(cv->x_re, 0, spectra * sizeof(float));
  memset(cv->x_im, 0, spectra * sizeof(float));
  memset(cv->prev, 0, (size_t)cv->channels * cv->block * sizeof(float));
  memset(cv->q_in, 0, (size_t)cv->channels * cv->block * sizeof(float));
  memset(cv->q_out, 0, (size_t)cv->channels * cv->block * sizeof(float));
  cv->q_fill = 0;
  cv->head = 0;
}

static void push_block(aud_convolver *cv, const float *in)
{
  cv->head = (cv->head + 1u) % cv->parts;

  for (unsigned ch = 0; ch < cv->channels; ch++)
  {
    float *prev = cv->prev + (size_t)ch * cv->block;
    float *cur = cv->cur + (size_t)ch * cv->block;
    size_t at = spectrum_at(cv, ch, cv->head);

    for (size_t i = 0; i < cv->block; i++)
    {
      cur[i] = in[i * cv->channels + ch];
    }

    memcpy(cv->re, prev, cv->block * sizeof(float));
    memcpy(cv->re + cv->block, cur, cv->block * sizeof(float));
    memset(cv->im, 0, cv->size * sizeof(float));
    aud_fft_forward(cv->re, cv->im, cv->size);

    memcpy(cv->x_re + at, cv->re, cv->bins * sizeof(float));
    memcpy(cv->x_im + at, cv->im, cv->bins * sizeof(float));
    memcpy(prev, cur, cv->block * sizeof(float));
  }
}

void aud_convolve_prime(aud_convolver *cv, const float *in)
{
  if (cv == NULL || in == NULL)
  {
    return;
  }
  push_block(cv, in);
}

static void render_channel(aud_convolver *cv, unsigned ch, float *out)
{
  size_t bins = cv->bins;

  memset(cv->re, 0, cv->size * sizeof(float));
  memset(cv->im, 0, cv->size * sizeof(float));

  for (size_t p = 0; p < cv->parts; p++)
  {
    size_t slot = (cv->head + cv->parts - p) % cv->parts;
    const float *xr = cv->x_re + spectrum_at(cv, ch, slot);
    const float *xi = cv->x_im + spectrum_at(cv, ch, slot);
    const float *hr = cv->h_re + spectrum_at(cv, ch, p);
    const float *hi = cv->h_im + spectrum_at(cv, ch, p);

    for (size_t k = 0; k < bins; k++)
    {
      cv->re[k] += xr[k] * hr[k] - xi[k] * hi[k];
      cv->im[k] += xr[k] * hi[k] + xi[k] * hr[k];
    }
  }

  for (size_t k = 1; k < cv->block; k++)
  {
    cv->re[cv->size - k] = cv->re[k];
    cv->im[cv->size - k] = -cv->im[k];
  }

  aud_fft_inverse(cv->re, cv->im, cv->size);

  for (size_t i = 0; i < cv->block; i++)
  {
    out[i * cv->channels + ch] = cv->re[cv->block + i];
  }
}

void aud_convolve_run(aud_convolver *cv, const float *in, float *out)
{
  if (cv == NULL || in == NULL || out == NULL)
  {
    return;
  }

  push_block(cv, in);

  for (unsigned ch = 0; ch < cv->channels; ch++)
  {
    render_channel(cv, ch, out);
  }
}

void aud_convolve_stream(aud_convolver *cv, const float *in, float *out, size_t frames)
{
  unsigned channels;

  if (cv == NULL || in == NULL || out == NULL)
  {
    return;
  }

  channels = cv->channels;

  for (size_t f = 0; f < frames; f++)
  {
    float *q_in = cv->q_in + cv->q_fill * channels;
    float *q_out = cv->q_out + cv->q_fill * channels;

    memcpy(q_in, in + f * channels, channels * sizeof(float));
    memcpy(out + f * channels, q_out, channels * sizeof(float));

    if (++cv->q_fill == cv->block)
    {
      aud_convolve_run(cv, cv->q_in, cv->q_out);
      cv->q_fill = 0;
    }
  }
}
