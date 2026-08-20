/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "audio/limiter.h"
#include "audio/loudness.h"
#include "audio/truepeak.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#define PI 3.14159265358979323846
#define RATE 48000u

/* dBTP of the loudest thing between the samples, by the same measurement
 * --info reports - which is the one the limiter is aiming at. */
static double true_peak_db(const float *buf, size_t frames, unsigned channels)
{
  aud_loudness *l = aud_loudness_create(RATE, channels);
  aud_loudness_reading r;

  CHECK(l != NULL);
  CHECK_EQ_INT(aud_loudness_feed(l, buf, frames), 0);
  aud_loudness_read(l, &r);
  aud_loudness_destroy(l);

  return r.true_peak > 0.0 ? 20.0 * log10(r.true_peak) : -1000.0;
}

static void fill_sine(float *buf, size_t frames, unsigned channels, double freq,
                      double amplitude, double phase)
{
  for (size_t f = 0; f < frames; f++)
  {
    double v = amplitude * sin(2.0 * PI * freq * (double)f / (double)RATE + phase);

    for (unsigned c = 0; c < channels; c++)
    {
      buf[f * channels + c] = (float)v;
    }
  }
}

/* -- what it leaves alone --------------------------------------------------- */

TEST(a_range_under_the_ceiling_comes_back_untouched)
{
  size_t frames = RATE / 2u;
  float *buf = malloc(frames * sizeof(*buf));
  float *was = malloc(frames * sizeof(*was));
  double reduction = -1.0;

  CHECK(buf != NULL && was != NULL);
  fill_sine(buf, frames, 1u, 440.0, 0.2, 0.0);
  memcpy(was, buf, frames * sizeof(*buf));

  CHECK_EQ_INT(aud_limiter_apply(buf, frames, 1u, RATE, -1.0, &reduction), 0);

  /* not "close to what went in" - the same floats, because nothing was over */
  CHECK_EQ_INT(memcmp(buf, was, frames * sizeof(*buf)), 0);
  CHECK_EQ_DBL(reduction, 0.0, 1e-12);

  free(buf);
  free(was);
}

TEST(a_quiet_passage_after_a_loud_one_is_let_back_up)
{
  size_t frames = RATE * 2u;
  float *buf = malloc(frames * sizeof(*buf));

  CHECK(buf != NULL);
  /* a second at full scale, then a second twenty decibels down */
  fill_sine(buf, frames, 1u, 300.0, 1.0, 0.0);
  for (size_t f = RATE; f < frames; f++)
  {
    buf[f] *= 0.1f;
  }

  CHECK_EQ_INT(aud_limiter_apply(buf, frames, 1u, RATE, -1.0, NULL), 0);

  /*
   * Half a second after the loud part stopped is far past any release: the
   * quiet part should be at its own level rather than still ducked. Its peak is
   * 0.1, so anything near that says the gain came back.
   */
  {
    float loudest = 0.0f;

    for (size_t f = RATE + RATE / 2u; f < frames; f++)
    {
      if (fabsf(buf[f]) > loudest)
      {
        loudest = fabsf(buf[f]);
      }
    }
    CHECK_EQ_DBL((double)loudest, 0.1, 0.002);
  }

  free(buf);
}

/* -- what it holds ---------------------------------------------------------- */

TEST(a_take_at_full_scale_is_brought_under_the_ceiling)
{
  size_t frames = RATE;
  float *buf = malloc(frames * sizeof(*buf));
  double reduction = 0.0;

  CHECK(buf != NULL);
  fill_sine(buf, frames, 1u, 997.0, 1.0, 0.3);

  CHECK(true_peak_db(buf, frames, 1u) > -1.0);
  CHECK_EQ_INT(aud_limiter_apply(buf, frames, 1u, RATE, -1.0, &reduction), 0);

  /* the tolerance limiter.h documents: a shade either side, not a decibel */
  CHECK(true_peak_db(buf, frames, 1u) <= -1.0 + 0.05);
  CHECK(reduction > 0.5 && reduction < 3.0);

  free(buf);
}

/*
 * The case a sample-peak ceiling cannot see at all: every sample is well under
 * full scale and the waveform between them is not. Two samples a cycle at a
 * quarter cycle either side of the peak read 0.7 and the peak reads 1.0.
 */
TEST(a_peak_that_falls_between_two_samples_is_still_brought_down)
{
  size_t frames = RATE / 4u;
  float *buf = malloc(frames * sizeof(*buf));
  float largest = 0.0f;

  CHECK(buf != NULL);
  fill_sine(buf, frames, 1u, RATE / 4.0, 0.99, PI / 4.0);

  for (size_t f = 0; f < frames; f++)
  {
    if (fabsf(buf[f]) > largest)
    {
      largest = fabsf(buf[f]);
    }
  }
  CHECK(largest < 0.75f);                      /* no sample is anywhere near over */
  CHECK(true_peak_db(buf, frames, 1u) > -0.5); /* and yet */

  CHECK_EQ_INT(aud_limiter_apply(buf, frames, 1u, RATE, -1.0, NULL), 0);
  CHECK(true_peak_db(buf, frames, 1u) <= -1.0 + 0.05);

  free(buf);
}

TEST(a_lone_transient_is_ridden_rather_than_flattened)
{
  size_t frames = RATE / 2u;
  size_t spike = frames / 2u;
  float *buf = calloc(frames, sizeof(*buf));

  CHECK(buf != NULL);
  fill_sine(buf, frames, 1u, 220.0, 0.3, 0.0);
  for (size_t f = spike; f < spike + 40u; f++)
  {
    buf[f] = (f - spike) % 2u == 0u ? 1.6f : -1.6f;
  }

  CHECK_EQ_INT(aud_limiter_apply(buf, frames, 1u, RATE, -1.0, NULL), 0);
  CHECK(true_peak_db(buf, frames, 1u) <= -1.0 + 0.05);

  /*
   * The gain is already on its way down before the spike arrives - that is what
   * the look-ahead is for - so the sample just before it is below where the
   * 0.3 sine would have put it rather than exactly on it.
   */
  CHECK(fabsf(buf[spike - 1u]) < 0.3f);

  free(buf);
}

TEST(every_channel_is_turned_down_by_the_same_amount)
{
  size_t frames = RATE / 2u;
  float *buf = malloc(frames * 2u * sizeof(*buf));

  CHECK(buf != NULL);
  for (size_t f = 0; f < frames; f++)
  {
    double v = sin(2.0 * PI * 300.0 * (double)f / (double)RATE);

    buf[f * 2u] = (float)v;             /* left at full scale */
    buf[f * 2u + 1u] = (float)v * 0.5f; /* right six decibels under it */
  }

  CHECK_EQ_INT(aud_limiter_apply(buf, frames, 2u, RATE, -1.0, NULL), 0);

  /* the image has not moved: whatever was done to one side was done to both */
  for (size_t f = 0; f < frames; f++)
  {
    CHECK_EQ_DBL((double)buf[f * 2u + 1u], (double)buf[f * 2u] * 0.5, 1e-6);
  }

  free(buf);
}

TEST(a_range_that_opens_on_a_peak_is_still_held_under)
{
  size_t frames = RATE / 4u;
  float *buf = malloc(frames * sizeof(*buf));

  CHECK(buf != NULL);
  fill_sine(buf, frames, 1u, 1000.0, 1.0, PI / 2.0); /* starts at full scale */

  CHECK_EQ_INT(aud_limiter_apply(buf, frames, 1u, RATE, -1.0, NULL), 0);
  CHECK(true_peak_db(buf, frames, 1u) <= -1.0 + 0.05);

  free(buf);
}

/*
 * The case that is hardest on the guarantee, and the one a sample-peak check
 * cannot catch: material that needs turning down at every single frame, so the
 * gain is moving the whole time.
 *
 * The twelve taps the meter reads around a frame each carry their own gain, not
 * that frame's, and an earlier tap's gain can be far higher than the one being
 * applied here - the envelope only ever comes down quickly. So it is not enough
 * for a frame's own gain to be under what it asked for: every gain reaching into
 * its window has to be, which is what the widened minimum in limiter.c is for.
 * Without it this reads about a quarter of a decibel over.
 */
TEST(dense_material_is_held_under_by_the_measurement_that_judges_it)
{
  size_t frames = RATE / 2u;
  unsigned channels = 2u;
  float *buf = malloc(frames * channels * sizeof(*buf));
  unsigned long long state = 0x2545F491u;

  CHECK(buf != NULL);

  /* full scale noise, the same sequence every run */
  for (size_t i = 0; i < frames * channels; i++)
  {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    buf[i] = (float)((double)((state >> 33) % 2000001u) / 1000000.0 - 1.0);
  }

  CHECK(true_peak_db(buf, frames, channels) > -1.0);
  CHECK_EQ_INT(aud_limiter_apply(buf, frames, channels, RATE, -1.0, NULL), 0);
  CHECK(true_peak_db(buf, frames, channels) <= -1.0 + 0.05);

  free(buf);
}

TEST(limiting_something_already_limited_changes_nothing)
{
  size_t frames = RATE / 2u;
  float *buf = malloc(frames * sizeof(*buf));
  float *was = malloc(frames * sizeof(*was));
  double again = -1.0;

  CHECK(buf != NULL && was != NULL);
  fill_sine(buf, frames, 1u, 440.0, 1.0, 0.0);

  CHECK_EQ_INT(aud_limiter_apply(buf, frames, 1u, RATE, -1.0, NULL), 0);
  memcpy(was, buf, frames * sizeof(*buf));

  CHECK_EQ_INT(aud_limiter_apply(buf, frames, 1u, RATE, -1.0, &again), 0);

  /* it may find a few hundredths left to take, but not a decibel of it */
  CHECK(again < 0.05);
  for (size_t f = 0; f < frames; f++)
  {
    CHECK_EQ_DBL((double)buf[f], (double)was[f], 0.005);
  }

  free(buf);
  free(was);
}

/* -- what it refuses -------------------------------------------------------- */

TEST(a_shape_it_is_not_defined_for_is_refused_rather_than_guessed_at)
{
  float buf[64];

  fill_sine(buf, 64u, 1u, 440.0, 0.5, 0.0);

  errno = 0;
  CHECK_EQ_INT(aud_limiter_apply(NULL, 64u, 1u, RATE, -1.0, NULL), -1);
  CHECK_EQ_INT(aud_limiter_apply(buf, 0u, 1u, RATE, -1.0, NULL), -1);
  CHECK_EQ_INT(aud_limiter_apply(buf, 64u, 0u, RATE, -1.0, NULL), -1);
  CHECK_EQ_INT(aud_limiter_apply(buf, 64u, 1u, AUD_LIMITER_MIN_RATE - 1u, -1.0, NULL),
               -1);
  CHECK_EQ_INT(aud_limiter_apply(buf, 64u, 1u, RATE, -500.0, NULL), -1);
  CHECK_EQ_INT(errno, EINVAL);
}

TEST(a_range_shorter_than_the_look_ahead_is_still_limited)
{
  float buf[16];
  double reduction = 0.0;

  for (size_t f = 0; f < 16u; f++)
  {
    buf[f] = 1.0f;
  }

  CHECK_EQ_INT(aud_limiter_apply(buf, 16u, 1u, RATE, -6.0, &reduction), 0);
  CHECK(reduction > 5.0 && reduction < 7.0);
  for (size_t f = 0; f < 16u; f++)
  {
    CHECK(buf[f] <= (float)pow(10.0, -6.0 / 20.0) + 1e-4f);
  }
}

/* -- the interpolator underneath it ----------------------------------------- */

TEST(the_taps_neither_lift_nor_drop_a_steady_level)
{
  aud_truepeak f;

  aud_truepeak_build(&f);

  for (unsigned p = 0; p < AUD_TRUEPEAK_PHASES; p++)
  {
    double sum = 0.0;

    for (unsigned t = 0; t < AUD_TRUEPEAK_TAPS; t++)
    {
      sum += (double)f.tap[p][t];
    }
    CHECK_EQ_DBL(sum, 1.0, 1e-5);
  }

  /* no row can make a window smaller than its largest sample */
  CHECK(f.bound >= 1.0);
}

TEST(the_filter_finds_the_peak_a_pair_of_samples_straddles)
{
  aud_truepeak f;
  float window[AUD_TRUEPEAK_TAPS];

  aud_truepeak_build(&f);

  /*
   * A quarter of the sample rate, phased so that its peak falls exactly halfway
   * between the two samples the filter looks between. Every sample of it reads
   * 0.707 and the waveform really reaches 1.0.
   */
  for (unsigned t = 0; t < AUD_TRUEPEAK_TAPS; t++)
  {
    double phase = PI / 2.0 - 2.0 * PI * 0.25 * ((double)AUD_TRUEPEAK_CENTRE + 0.5);

    window[t] = (float)sin(2.0 * PI * 0.25 * (double)t + phase);
  }

  CHECK_EQ_DBL((double)window[AUD_TRUEPEAK_CENTRE], 0.7071, 1e-3);
  CHECK_EQ_DBL((double)aud_truepeak_between(&f, window), 1.0, 0.01);
}

int main(void)
{
  RUN(a_range_under_the_ceiling_comes_back_untouched);
  RUN(a_quiet_passage_after_a_loud_one_is_let_back_up);
  RUN(a_take_at_full_scale_is_brought_under_the_ceiling);
  RUN(a_peak_that_falls_between_two_samples_is_still_brought_down);
  RUN(a_lone_transient_is_ridden_rather_than_flattened);
  RUN(every_channel_is_turned_down_by_the_same_amount);
  RUN(a_range_that_opens_on_a_peak_is_still_held_under);
  RUN(dense_material_is_held_under_by_the_measurement_that_judges_it);
  RUN(limiting_something_already_limited_changes_nothing);
  RUN(a_shape_it_is_not_defined_for_is_refused_rather_than_guessed_at);
  RUN(a_range_shorter_than_the_look_ahead_is_still_limited);
  RUN(the_taps_neither_lift_nor_drop_a_steady_level);
  RUN(the_filter_finds_the_peak_a_pair_of_samples_straddles);
  return TEST_RESULT();
}
