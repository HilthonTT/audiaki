/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/samples.h"

#include <math.h>
#include <stdlib.h>

/* What the index is supposed to agree with: read every sample and compare. */
static void brute_range(const aud_samples *s, unsigned ch, size_t from, size_t to,
                        float *lo, float *hi)
{
  *lo = 0.0f;
  *hi = 0.0f;
  if (from >= to)
  {
    return;
  }

  *lo = s->data[from * s->channels + ch];
  *hi = *lo;
  for (size_t f = from + 1; f < to; f++)
  {
    float v = s->data[f * s->channels + ch];

    if (v < *lo)
    {
      *lo = v;
    }
    if (v > *hi)
    {
      *hi = v;
    }
  }
}

/* A signal with a distinct value everywhere, so an off-by-one shows up. */
static aud_samples *ramp(unsigned channels, size_t frames)
{
  aud_samples *s = aud_samples_create(channels, frames);

  for (size_t f = 0; f < frames; f++)
  {
    for (unsigned ch = 0; ch < channels; ch++)
    {
      s->data[f * channels + ch] = sinf((float)f * 0.001f) * (ch == 0 ? 1.0f : 0.5f);
    }
  }
  return s;
}

TEST(create_gives_silence_and_one_owner)
{
  aud_samples *s = aud_samples_create(2, 128);

  CHECK(s != NULL);
  CHECK_EQ_INT(s->refs, 1);
  CHECK_EQ_INT(s->channels, 2);
  CHECK_EQ_INT(s->frames, 128);
  for (size_t i = 0; i < 128u * 2u; i++)
  {
    CHECK(s->data[i] == 0.0f);
  }

  aud_samples_release(s);
}

TEST(create_refuses_unusable_geometry)
{
  CHECK(aud_samples_create(0, 128) == NULL);
  CHECK(aud_samples_create(2, 0) == NULL);
  CHECK(aud_samples_create(2, (size_t)-1) == NULL);
}

TEST(a_block_lives_until_the_last_owner_lets_go)
{
  aud_samples *s = aud_samples_create(1, 16);

  aud_samples_retain(s);
  aud_samples_retain(s);
  CHECK_EQ_INT(s->refs, 3);

  aud_samples_release(s);
  aud_samples_release(s);
  CHECK_EQ_INT(s->refs, 1);

  /* only the last release frees it; ASan is what checks that claim */
  aud_samples_release(s);
  aud_samples_release(NULL);
}

TEST(the_index_agrees_with_reading_every_sample)
{
  /* long enough to reach the coarse level: 256 * 256 frames per bucket */
  aud_samples *s = ramp(2, 400000);
  const size_t spans[] = {1, 2, 255, 256, 257, 512, 5000, 65536, 65537, 200000, 399999};

  aud_samples_index(s);
  CHECK(s->fine != NULL);
  CHECK(s->coarse != NULL);

  for (size_t i = 0; i < sizeof(spans) / sizeof(spans[0]); i++)
  {
    /* several offsets per span, so partial buckets at both ends are covered */
    const size_t starts[] = {0, 1, 255, 1000, 65535, 130000};

    for (size_t j = 0; j < sizeof(starts) / sizeof(starts[0]); j++)
    {
      size_t from = starts[j];
      size_t to = from + spans[i];
      aud_peak got;
      float lo;
      float hi;
      float want_lo;
      float want_hi;

      if (to > s->frames)
      {
        continue;
      }

      for (unsigned ch = 0; ch < 2; ch++)
      {
        aud_samples_range(s, ch, from, to, &got);
        lo = got.min;
        hi = got.max;
        brute_range(s, ch, from, to, &want_lo, &want_hi);
        CHECK_EQ_DBL(lo, want_lo, 1e-6);
        CHECK_EQ_DBL(hi, want_hi, 1e-6);
      }
    }
  }

  aud_samples_release(s);
}

TEST(a_range_reads_the_same_without_an_index)
{
  aud_samples *s = ramp(1, 20000);
  aud_peak got;
  float lo;
  float hi;
  float want_lo;
  float want_hi;

  /* no aud_samples_index(): the readers have to fall back to the samples */
  aud_samples_range(s, 0, 100, 19000, &got);
  lo = got.min;
  hi = got.max;
  brute_range(s, 0, 100, 19000, &want_lo, &want_hi);
  CHECK_EQ_DBL(lo, want_lo, 1e-6);
  CHECK_EQ_DBL(hi, want_hi, 1e-6);

  aud_samples_release(s);
}

TEST(a_range_outside_the_block_reads_as_silence)
{
  aud_samples *s = ramp(2, 1000);
  aud_peak got;

  aud_samples_range(s, 0, 500, 500, &got);
  CHECK(got.min == 0.0f && got.max == 0.0f && got.rms == 0.0f);

  aud_samples_range(s, 0, 2000, 3000, &got);
  CHECK(got.min == 0.0f && got.max == 0.0f);

  /* a channel the block does not have is not a crash */
  aud_samples_range(s, 7, 0, 100, &got);
  CHECK(got.min == 0.0f && got.max == 0.0f);

  aud_samples_range(NULL, 0, 0, 100, &got);
  CHECK(got.min == 0.0f && got.max == 0.0f);

  aud_samples_release(s);
}

TEST(a_range_that_runs_past_the_end_is_clamped_to_it)
{
  aud_samples *s = ramp(1, 1000);
  aud_peak got;
  float want_lo;
  float want_hi;

  aud_samples_index(s);
  aud_samples_range(s, 0, 900, 5000, &got);
  brute_range(s, 0, 900, 1000, &want_lo, &want_hi);
  CHECK_EQ_DBL(got.min, want_lo, 1e-6);
  CHECK_EQ_DBL(got.max, want_hi, 1e-6);

  aud_samples_release(s);
}

TEST(indexing_twice_does_nothing_the_second_time)
{
  aud_samples *s = ramp(1, 5000);

  aud_samples_index(s);
  {
    const aud_peak *first = s->fine;

    aud_samples_index(s);
    CHECK(s->fine == first);
  }

  aud_samples_release(s);
}

int main(void)
{
  RUN(create_gives_silence_and_one_owner);
  RUN(create_refuses_unusable_geometry);
  RUN(a_block_lives_until_the_last_owner_lets_go);
  RUN(the_index_agrees_with_reading_every_sample);
  RUN(a_range_reads_the_same_without_an_index);
  RUN(a_range_outside_the_block_reads_as_silence);
  RUN(a_range_that_runs_past_the_end_is_clamped_to_it);
  RUN(indexing_twice_does_nothing_the_second_time);

  return TEST_RESULT();
}
