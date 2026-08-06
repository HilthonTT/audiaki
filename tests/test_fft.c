/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "fft.h"
#include "spectrum.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AUD_TEST_PI 3.14159265358979323846

TEST(pow2_check)
{
  CHECK(aud_fft_is_pow2(2));
  CHECK(aud_fft_is_pow2(1024));
  CHECK(aud_fft_is_pow2(2048));
  CHECK(!aud_fft_is_pow2(0));
  CHECK(!aud_fft_is_pow2(1));
  CHECK(!aud_fft_is_pow2(3));
  CHECK(!aud_fft_is_pow2(1000));
}

TEST(dc_lands_in_bin_zero)
{
  float re[8];
  float im[8];

  for (size_t i = 0; i < 8; i++)
  {
    re[i] = 1.0f;
    im[i] = 0.0f;
  }

  aud_fft_forward(re, im, 8);

  /* a constant signal is all bin 0, with magnitude n */
  CHECK_EQ_DBL(aud_fft_magnitude(re[0], im[0]), 8.0, 1e-4);
  for (size_t k = 1; k < 8; k++)
  {
    CHECK_EQ_DBL(aud_fft_magnitude(re[k], im[k]), 0.0, 1e-4);
  }
}

TEST(impulse_is_flat)
{
  float re[16];
  float im[16];

  memset(re, 0, sizeof(re));
  memset(im, 0, sizeof(im));
  re[0] = 1.0f;

  aud_fft_forward(re, im, 16);

  /* a unit impulse at t = 0 transforms to 1.0 in every bin */
  for (size_t k = 0; k < 16; k++)
  {
    CHECK_EQ_DBL(aud_fft_magnitude(re[k], im[k]), 1.0, 1e-5);
  }
}

TEST(sine_lands_in_its_own_bin)
{
  const size_t n = 64;
  const size_t bin = 7;
  float re[64];
  float im[64];

  /* exactly `bin` cycles across the window, so there is nothing to leak */
  for (size_t i = 0; i < n; i++)
  {
    re[i] = (float)sin(2.0 * AUD_TEST_PI * (double)bin * (double)i / (double)n);
    im[i] = 0.0f;
  }

  aud_fft_forward(re, im, n);

  /* a real sine splits its energy between bin and n - bin: n/2 each */
  CHECK_EQ_DBL(aud_fft_magnitude(re[bin], im[bin]), (double)n / 2.0, 1e-3);
  CHECK_EQ_DBL(aud_fft_magnitude(re[n - bin], im[n - bin]), (double)n / 2.0, 1e-3);

  for (size_t k = 0; k < n; k++)
  {
    if (k == bin || k == n - bin)
    {
      continue;
    }
    CHECK_EQ_DBL(aud_fft_magnitude(re[k], im[k]), 0.0, 1e-3);
  }
}

TEST(bad_sizes_are_left_alone)
{
  float re[3] = {1.0f, 2.0f, 3.0f};
  float im[3] = {0.0f, 0.0f, 0.0f};

  aud_fft_forward(re, im, 3);
  aud_fft_forward(NULL, im, 2);

  CHECK_EQ_DBL(re[0], 1.0, 1e-9);
  CHECK_EQ_DBL(re[2], 3.0, 1e-9);
}

TEST(hann_window_shape)
{
  float w[8];

  aud_fft_hann(w, 8);

  /* periodic Hann: zero at the first sample, peak in the middle */
  CHECK_EQ_DBL(w[0], 0.0, 1e-6);
  CHECK_EQ_DBL(w[4], 1.0, 1e-6);
  CHECK_EQ_DBL(w[2], 0.5, 1e-6);
  CHECK_EQ_DBL(w[6], 0.5, 1e-6);

  /* the coherent gain the analyser's normalisation assumes */
  double sum = 0.0;
  for (size_t i = 0; i < 8; i++)
  {
    sum += (double)w[i];
  }
  CHECK_EQ_DBL(sum, 4.0, 1e-5);
}

/* -- spectrum -------------------------------------------------------------- */

static void fill_sine(float *dst, size_t frames, double hz, unsigned rate, double amp)
{
  for (size_t i = 0; i < frames; i++)
  {
    dst[i] = (float)(amp * sin(2.0 * AUD_TEST_PI * hz * (double)i / (double)rate));
  }
}

TEST(spectrum_rejects_bad_config)
{
  aud_spectrum_config cfg;

  aud_spectrum_config_defaults(&cfg, 44100, 32);
  cfg.fft_size = 1000; /* not a power of two */
  CHECK(aud_spectrum_create(&cfg) == NULL);

  aud_spectrum_config_defaults(&cfg, 44100, 2); /* below the band minimum */
  CHECK(aud_spectrum_create(&cfg) == NULL);

  aud_spectrum_config_defaults(&cfg, 100, 32); /* implausible rate */
  CHECK(aud_spectrum_create(&cfg) == NULL);
}

TEST(spectrum_is_silent_before_any_audio)
{
  aud_spectrum_config cfg;
  aud_spectrum *s;
  const float *v;

  aud_spectrum_config_defaults(&cfg, 44100, 16);
  s = aud_spectrum_create(&cfg);
  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  CHECK_EQ_INT(aud_spectrum_bands(s), 16);

  v = aud_spectrum_analyse(s, 1.0 / 60.0);
  for (size_t b = 0; b < 16; b++)
  {
    CHECK_EQ_DBL(v[b], 0.0, 1e-6);
  }

  aud_spectrum_destroy(s);
}

TEST(spectrum_peaks_at_the_input_frequency)
{
  aud_spectrum_config cfg;
  aud_spectrum *s;
  float *audio;
  const float *v;
  size_t bands = 24;
  size_t loudest = 0;
  const double hz = 440.0;

  aud_spectrum_config_defaults(&cfg, 44100, bands);
  s = aud_spectrum_create(&cfg);
  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  audio = malloc(cfg.fft_size * sizeof(*audio));
  CHECK(audio != NULL);
  if (audio == NULL)
  {
    aud_spectrum_destroy(s);
    return;
  }

  fill_sine(audio, cfg.fft_size, hz, cfg.rate, 1.0);
  aud_spectrum_push(s, audio, cfg.fft_size);

  /* a long step so the smoothing settles on the target in one call */
  v = aud_spectrum_analyse(s, 10.0);

  for (size_t b = 1; b < bands; b++)
  {
    if (v[b] > v[loudest])
    {
      loudest = b;
    }
  }

  /*
   * The loudest band must be the one containing 440 Hz. Bands are log spaced,
   * so check against the centre frequencies rather than a hard coded index.
   */
  const double *centres = aud_spectrum_centres(s);
  double ratio = centres[loudest] / hz;
  CHECK(ratio > 0.8 && ratio < 1.25);

  /* a full scale sine should read near the top of the scale */
  CHECK(v[loudest] > 0.85);

  /* and the far end of the spectrum should be empty */
  CHECK(v[bands - 1] < 0.2);

  free(audio);
  aud_spectrum_destroy(s);
}

TEST(spectrum_decays_towards_silence)
{
  aud_spectrum_config cfg;
  aud_spectrum *s;
  float *audio;
  const float *v;
  float loud;

  aud_spectrum_config_defaults(&cfg, 44100, 16);
  s = aud_spectrum_create(&cfg);
  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  audio = calloc(cfg.fft_size, sizeof(*audio));
  CHECK(audio != NULL);
  if (audio == NULL)
  {
    aud_spectrum_destroy(s);
    return;
  }

  fill_sine(audio, cfg.fft_size, 440.0, cfg.rate, 1.0);
  aud_spectrum_push(s, audio, cfg.fft_size);
  v = aud_spectrum_analyse(s, 10.0);
  loud = v[0];
  for (size_t b = 0; b < 16; b++)
  {
    if (v[b] > loud)
    {
      loud = v[b];
    }
  }
  CHECK(loud > 0.5);

  /* push silence over the whole window, then let the decay run */
  memset(audio, 0, cfg.fft_size * sizeof(*audio));
  aud_spectrum_push(s, audio, cfg.fft_size);
  for (int i = 0; i < 60; i++)
  {
    v = aud_spectrum_analyse(s, 1.0 / 60.0);
  }

  for (size_t b = 0; b < 16; b++)
  {
    CHECK(v[b] < 0.05);
  }

  free(audio);
  aud_spectrum_destroy(s);
}

TEST(spectrum_accepts_pcm_directly)
{
  aud_spectrum_config cfg;
  aud_spectrum *s;
  int16_t *pcm;
  const float *from_pcm;
  size_t frames;

  aud_spectrum_config_defaults(&cfg, 44100, 16);
  s = aud_spectrum_create(&cfg);
  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  frames = cfg.fft_size;
  pcm = malloc(frames * 2 * sizeof(*pcm)); /* stereo */
  CHECK(pcm != NULL);
  if (pcm == NULL)
  {
    aud_spectrum_destroy(s);
    return;
  }

  for (size_t i = 0; i < frames; i++)
  {
    double x = sin(2.0 * AUD_TEST_PI * 440.0 * (double)i / (double)cfg.rate);
    pcm[i * 2] = (int16_t)(x * 32000.0);
    pcm[i * 2 + 1] = (int16_t)(x * 32000.0);
  }

  aud_spectrum_push_pcm(s, pcm, frames, 2, AUD_FORMAT_S16_LE);
  from_pcm = aud_spectrum_analyse(s, 10.0);

  float loudest = 0.0f;
  for (size_t b = 0; b < 16; b++)
  {
    if (from_pcm[b] > loudest)
    {
      loudest = from_pcm[b];
    }
  }
  CHECK(loudest > 0.8);

  free(pcm);
  aud_spectrum_destroy(s);
}

int main(void)
{
  RUN(pow2_check);
  RUN(dc_lands_in_bin_zero);
  RUN(impulse_is_flat);
  RUN(sine_lands_in_its_own_bin);
  RUN(bad_sizes_are_left_alone);
  RUN(hann_window_shape);
  RUN(spectrum_rejects_bad_config);
  RUN(spectrum_is_silent_before_any_audio);
  RUN(spectrum_peaks_at_the_input_frequency);
  RUN(spectrum_decays_towards_silence);
  RUN(spectrum_accepts_pcm_directly);
  return TEST_RESULT();
}
