/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "audio/convolve.h"

#include <stdlib.h>

static float noise(unsigned *seed)
{
  *seed = *seed * 1103515245u + 12345u;
  return (float)((*seed >> 8) & 0xffffu) / 32768.0f - 1.0f;
}

static void direct(const float *x, size_t frames, unsigned channels, const float *h,
                   size_t taps, unsigned h_channels, float *y)
{
  for (size_t n = 0; n < frames; n++)
  {
    for (unsigned ch = 0; ch < channels; ch++)
    {
      double sum = 0.0;

      for (size_t k = 0; k < taps && k <= n; k++)
      {
        sum += (double)h[k * h_channels + ch % h_channels] * x[(n - k) * channels + ch];
      }
      y[n * channels + ch] = (float)sum;
    }
  }
}

static double worst(const float *a, const float *b, size_t count)
{
  double w = 0.0;

  for (size_t i = 0; i < count; i++)
  {
    double d = fabs((double)a[i] - b[i]);

    if (d > w)
    {
      w = d;
    }
  }
  return w;
}

TEST(blocks_match_direct_convolution)
{
  enum
  {
    BLOCK = 64,
    TAPS = 300,
    FRAMES = BLOCK * 12,
    CH = 2
  };
  float *h = malloc(TAPS * sizeof(float));
  float *x = malloc(FRAMES * CH * sizeof(float));
  float *want = malloc(FRAMES * CH * sizeof(float));
  float *got = malloc(FRAMES * CH * sizeof(float));
  unsigned seed = 7;
  aud_convolver *cv;

  for (size_t i = 0; i < TAPS; i++)
  {
    h[i] = noise(&seed) * 0.1f;
  }
  for (size_t i = 0; i < FRAMES * CH; i++)
  {
    x[i] = noise(&seed);
  }

  direct(x, FRAMES, CH, h, TAPS, 1, want);

  cv = aud_convolve_create(h, TAPS, 1, CH, BLOCK);
  CHECK(cv != NULL);
  CHECK_EQ_INT(aud_convolve_partitions(cv), 5);

  for (size_t b = 0; b < FRAMES / BLOCK; b++)
  {
    aud_convolve_run(cv, x + b * BLOCK * CH, got + b * BLOCK * CH);
  }
  CHECK(worst(want, got, FRAMES * CH) < 1e-4);

  aud_convolve_destroy(cv);
  free(h);
  free(x);
  free(want);
  free(got);
}

TEST(a_stereo_response_keeps_its_sides_apart)
{
  float h[4] = {1.0f, 0.0f, 0.0f, 0.5f};
  float x[16 * 2] = {0};
  float y[16 * 2];
  aud_convolver *cv = aud_convolve_create(h, 2, 2, 2, 16);

  x[0] = 1.0f;
  x[1] = 1.0f;
  aud_convolve_run(cv, x, y);

  CHECK_EQ_DBL(y[0], 1.0, 1e-5);
  CHECK_EQ_DBL(y[1], 0.0, 1e-5);
  CHECK_EQ_DBL(y[2], 0.0, 1e-5);
  CHECK_EQ_DBL(y[3], 0.5, 1e-5);

  aud_convolve_destroy(cv);
}

TEST(priming_gives_the_same_answer_as_having_played_it)
{
  enum
  {
    BLOCK = 32,
    TAPS = 100,
    FRAMES = BLOCK * 8
  };
  float h[TAPS];
  float x[FRAMES];
  float played[BLOCK];
  float primed[BLOCK];
  unsigned seed = 3;
  aud_convolver *a;
  aud_convolver *b;

  for (size_t i = 0; i < TAPS; i++)
  {
    h[i] = noise(&seed);
  }
  for (size_t i = 0; i < FRAMES; i++)
  {
    x[i] = noise(&seed);
  }

  a = aud_convolve_create(h, TAPS, 1, 1, BLOCK);
  b = aud_convolve_create(h, TAPS, 1, 1, BLOCK);

  for (size_t k = 0; k < 7; k++)
  {
    aud_convolve_run(a, x + k * BLOCK, played);
  }
  aud_convolve_run(a, x + 7 * BLOCK, played);

  aud_convolve_run(b, x, primed);
  aud_convolve_reset(b);
  for (size_t k = 3; k < 7; k++)
  {
    aud_convolve_prime(b, x + k * BLOCK);
  }
  aud_convolve_run(b, x + 7 * BLOCK, primed);

  CHECK(worst(played, primed, BLOCK) < 1e-4);

  aud_convolve_destroy(a);
  aud_convolve_destroy(b);
}

TEST(a_stream_is_the_blocks_one_block_late)
{
  enum
  {
    BLOCK = 16,
    FRAMES = 100
  };
  float h[1] = {1.0f};
  float x[FRAMES];
  float y[FRAMES];
  aud_convolver *cv = aud_convolve_create(h, 1, 1, 1, BLOCK);

  for (size_t i = 0; i < FRAMES; i++)
  {
    x[i] = (float)(i + 1u);
  }

  aud_convolve_stream(cv, x, y, 7);
  aud_convolve_stream(cv, x + 7, y + 7, FRAMES - 7);

  for (size_t i = 0; i < BLOCK; i++)
  {
    CHECK_EQ_DBL(y[i], 0.0, 1e-4);
  }
  for (size_t i = BLOCK; i < FRAMES; i++)
  {
    CHECK_EQ_DBL(y[i], x[i - BLOCK], 1e-3);
  }

  aud_convolve_destroy(cv);
}

TEST(a_stream_can_be_converted_in_place)
{
  float h[1] = {1.0f};
  float buf[64];
  aud_convolver *cv = aud_convolve_create(h, 1, 1, 1, 16);

  for (size_t i = 0; i < 64; i++)
  {
    buf[i] = (float)i;
  }
  aud_convolve_stream(cv, buf, buf, 64);
  CHECK_EQ_DBL(buf[20], 4.0, 1e-3);
  CHECK_EQ_DBL(buf[63], 47.0, 1e-3);

  aud_convolve_destroy(cv);
}

TEST(nonsense_is_refused)
{
  float h[1] = {1.0f};

  CHECK(aud_convolve_create(NULL, 1, 1, 1, 16) == NULL);
  CHECK(aud_convolve_create(h, 0, 1, 1, 16) == NULL);
  CHECK(aud_convolve_create(h, 1, 1, 1, 24) == NULL);
  CHECK(aud_convolve_create(h, 1, 0, 1, 16) == NULL);
}

int main(void)
{
  RUN(blocks_match_direct_convolution);
  RUN(a_stereo_response_keeps_its_sides_apart);
  RUN(priming_gives_the_same_answer_as_having_played_it);
  RUN(a_stream_is_the_blocks_one_block_late);
  RUN(a_stream_can_be_converted_in_place);
  RUN(nonsense_is_refused);
  return TEST_RESULT();
}
