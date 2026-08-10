/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "audio/resample.h"

#include <math.h>
#include <stdlib.h>

/* M_PI is an X/Open extension, and this project compiles with _POSIX_C_SOURCE */
#define AUD_TEST_PI 3.14159265358979323846

/* Fill `dst` with `frames` of a sine at `hz`, interleaved across `channels`. */
static void tone(float *dst, size_t frames, unsigned channels, double hz, unsigned rate,
                 double amplitude)
{
  for (size_t f = 0; f < frames; f++)
  {
    float v = (float)(amplitude * sin(2.0 * AUD_TEST_PI * hz * (double)f / rate));

    for (unsigned c = 0; c < channels; c++)
    {
      dst[f * channels + c] = v;
    }
  }
}

/* Largest absolute sample, ignoring the first and last `skip` frames. */
static double peak_of(const float *x, size_t frames, unsigned channels, size_t skip)
{
  double worst = 0.0;

  if (frames < 2 * skip)
  {
    return 0.0;
  }
  for (size_t f = skip; f < frames - skip; f++)
  {
    for (unsigned c = 0; c < channels; c++)
    {
      double v = fabs((double)x[f * channels + c]);

      if (v > worst)
      {
        worst = v;
      }
    }
  }
  return worst;
}

/*
 * How much energy sits at `hz` in `x`, by correlating against a sine and a
 * cosine there. Enough to say "the tone came through" and "nothing else did"
 * without a transform.
 */
static double energy_at(const float *x, size_t frames, unsigned channels, double hz,
                        unsigned rate, size_t skip)
{
  double re = 0.0;
  double im = 0.0;
  size_t n = 0;

  if (frames < 2 * skip)
  {
    return 0.0;
  }
  for (size_t f = skip; f < frames - skip; f++)
  {
    double w = 2.0 * AUD_TEST_PI * hz * (double)f / rate;
    double v = x[f * channels];

    re += v * cos(w);
    im += v * sin(w);
    n++;
  }
  if (n == 0)
  {
    return 0.0;
  }
  return 2.0 * sqrt(re * re + im * im) / (double)n;
}

TEST(a_converter_is_refused_nonsense)
{
  CHECK(aud_resample_create(0, 48000, 2) == NULL);
  CHECK(aud_resample_create(48000, 0, 2) == NULL);
  CHECK(aud_resample_create(48000, 48000, 0) == NULL);

  /* and the API survives being handed nothing */
  aud_resample_destroy(NULL);
  aud_resample_reset(NULL);
  CHECK_EQ_INT(aud_resample_out_max(NULL, 100), 0);
  CHECK_EQ_INT(aud_resample_latency(NULL), 0);
  CHECK_EQ_INT(aud_resample_run(NULL, NULL, 0, NULL, 0), 0);
}

TEST(the_frame_count_follows_the_ratio)
{
  static const struct
  {
    unsigned in;
    unsigned out;
  } rates[] = {
      {44100u, 48000u}, {48000u, 44100u}, {96000u, 48000u},
      {48000u, 96000u}, {48000u, 48000u}, {44100u, 192000u},
  };

  for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
  {
    aud_resampler *rs = aud_resample_create(rates[i].in, rates[i].out, 1);
    float *in = calloc(4800, sizeof(*in));
    float *out;
    size_t total = 0;
    double want;

    CHECK(rs != NULL);
    CHECK(in != NULL);
    if (rs == NULL || in == NULL)
    {
      free(in);
      aud_resample_destroy(rs);
      continue;
    }

    out = calloc(aud_resample_out_max(rs, 4800), sizeof(*out));
    CHECK(out != NULL);
    if (out == NULL)
    {
      free(in);
      aud_resample_destroy(rs);
      continue;
    }

    /* ten passes, so the fractional carry between calls is exercised */
    for (int pass = 0; pass < 10; pass++)
    {
      total += aud_resample_run(rs, in, 4800, out, aud_resample_out_max(rs, 4800));
    }

    want = 48000.0 * (double)rates[i].out / (double)rates[i].in;
    /*
     * The output grid does not have to start on an input boundary, so the
     * count is a frame or so out from the start - and that costs one input
     * frame's worth of output, which is the ratio. What matters is that it
     * does not grow: measured over twenty times this much input the gap stays
     * where it is, because the phase carries between calls rather than being
     * rounded away at each one.
     */
    CHECK(fabs((double)total - want) <= (double)rates[i].out / (double)rates[i].in + 3.0);

    free(out);
    free(in);
    aud_resample_destroy(rs);
  }
}

TEST(a_tone_survives_going_up_and_coming_down)
{
  static const struct
  {
    unsigned in;
    unsigned out;
  } rates[] = {{44100u, 48000u}, {48000u, 44100u}, {96000u, 48000u}, {48000u, 96000u}};

  for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
  {
    aud_resampler *rs = aud_resample_create(rates[i].in, rates[i].out, 1);
    size_t n = rates[i].in / 2u; /* half a second */
    float *in = calloc(n, sizeof(*in));
    float *out;
    size_t got;

    CHECK(rs != NULL);
    if (rs == NULL || in == NULL)
    {
      free(in);
      aud_resample_destroy(rs);
      continue;
    }
    out = calloc(aud_resample_out_max(rs, n), sizeof(*out));
    CHECK(out != NULL);
    if (out == NULL)
    {
      free(in);
      aud_resample_destroy(rs);
      continue;
    }

    /* 1 kHz, comfortably inside every band involved */
    tone(in, n, 1, 1000.0, rates[i].in, 0.5);
    got = aud_resample_run(rs, in, n, out, aud_resample_out_max(rs, n));
    CHECK(got > 0);

    /*
     * The tone comes out at the same level and the same frequency. Skipping
     * the ends because the filter has to fill up before it is telling the
     * truth, and again as the input runs out.
     */
    CHECK_EQ_DBL(peak_of(out, got, 1, 64), 0.5, 0.02);
    CHECK_EQ_DBL(energy_at(out, got, 1, 1000.0, rates[i].out, 64), 0.5, 0.02);

    free(out);
    free(in);
    aud_resample_destroy(rs);
  }
}

TEST(coming_down_does_not_fold_the_top_back_in)
{
  /*
   * The reason this is a windowed sinc and not a linear interpolator. 30 kHz
   * at 96 kHz is a real tone; at 48 kHz there is nowhere to put it, and an
   * unfiltered converter would drop it back in at 48000 - 30000 = 18 kHz,
   * where it is both audible and not something anybody played.
   */
  aud_resampler *rs = aud_resample_create(96000u, 48000u, 1);
  size_t n = 96000u;
  float *in = calloc(n, sizeof(*in));
  float *out;
  size_t got;

  CHECK(rs != NULL);
  if (rs == NULL || in == NULL)
  {
    free(in);
    aud_resample_destroy(rs);
    return;
  }
  out = calloc(aud_resample_out_max(rs, n), sizeof(*out));
  CHECK(out != NULL);
  if (out == NULL)
  {
    free(in);
    aud_resample_destroy(rs);
    return;
  }

  tone(in, n, 1, 30000.0, 96000u, 0.5);
  got = aud_resample_run(rs, in, n, out, aud_resample_out_max(rs, n));
  CHECK(got > 0);

  /* the alias is not there... */
  CHECK(energy_at(out, got, 1, 18000.0, 48000u, 256) < 0.01);
  /* ...and neither is anything else: the tone had nowhere legal to go */
  CHECK(peak_of(out, got, 1, 256) < 0.02);

  free(out);
  free(in);
  aud_resample_destroy(rs);
}

TEST(the_channels_stay_apart)
{
  aud_resampler *rs = aud_resample_create(44100u, 48000u, 2);
  size_t n = 4410;
  float *in = calloc(n * 2u, sizeof(*in));
  float *out;
  size_t got;

  CHECK(rs != NULL);
  if (rs == NULL || in == NULL)
  {
    free(in);
    aud_resample_destroy(rs);
    return;
  }
  out = calloc(aud_resample_out_max(rs, n) * 2u, sizeof(*out));
  CHECK(out != NULL);
  if (out == NULL)
  {
    free(in);
    aud_resample_destroy(rs);
    return;
  }

  /* a tone on the left, silence on the right */
  for (size_t f = 0; f < n; f++)
  {
    in[f * 2u] = (float)(0.5 * sin(2.0 * AUD_TEST_PI * 1000.0 * (double)f / 44100.0));
    in[f * 2u + 1u] = 0.0f;
  }

  got = aud_resample_run(rs, in, n, out, aud_resample_out_max(rs, n));
  CHECK(got > 64);

  for (size_t f = 64; f + 64 < got; f++)
  {
    /* nothing leaks across, however the taps are indexed */
    CHECK(fabs((double)out[f * 2u + 1u]) < 1e-6);
  }

  free(out);
  free(in);
  aud_resample_destroy(rs);
}

TEST(a_stream_cut_into_pieces_comes_out_the_same)
{
  aud_resampler *whole = aud_resample_create(44100u, 48000u, 1);
  aud_resampler *pieces = aud_resample_create(44100u, 48000u, 1);
  size_t n = 8820;
  float *in = calloc(n, sizeof(*in));
  float *a;
  float *b;
  size_t got_a = 0;
  size_t got_b = 0;

  CHECK(whole != NULL && pieces != NULL && in != NULL);
  if (whole == NULL || pieces == NULL || in == NULL)
  {
    free(in);
    aud_resample_destroy(whole);
    aud_resample_destroy(pieces);
    return;
  }

  a = calloc(aud_resample_out_max(whole, n), sizeof(*a));
  b = calloc(aud_resample_out_max(pieces, n) + 64u, sizeof(*b));
  CHECK(a != NULL && b != NULL);
  if (a == NULL || b == NULL)
  {
    free(a);
    free(b);
    free(in);
    aud_resample_destroy(whole);
    aud_resample_destroy(pieces);
    return;
  }

  tone(in, n, 1, 440.0, 44100u, 0.4);

  got_a = aud_resample_run(whole, in, n, a, aud_resample_out_max(whole, n));

  /* awkward pieces, landing mid-phase rather than on a convenient boundary */
  for (size_t at = 0; at < n; at += 137u)
  {
    size_t take = n - at < 137u ? n - at : 137u;

    got_b += aud_resample_run(pieces, in + at, take, b + got_b,
                              aud_resample_out_max(pieces, take));
  }

  /*
   * The whole point of carrying the phase and the tail between calls: a stream
   * handed over in period-sized pieces has to be the same stream, sample for
   * sample.
   *
   * The counts may differ by one, and that is not the same thing as differing.
   * An output frame can only be produced once there is input under its right
   * hand taps, so each call holds a few back for the next one - and how many
   * depends on where that call's boundary happened to fall. Whichever
   * converter has the luckier last boundary gets one more frame out of the
   * same audio. Nothing is lost or repeated, which is what the comparison
   * below is actually checking.
   */
  CHECK(got_b + 1u >= got_a && got_a + 1u >= got_b);
  CHECK(got_a > 8000u);
  for (size_t i = 0; i < (got_a < got_b ? got_a : got_b); i++)
  {
    CHECK_EQ_DBL(b[i], a[i], 1e-6);
  }

  free(a);
  free(b);
  free(in);
  aud_resample_destroy(whole);
  aud_resample_destroy(pieces);
}

TEST(equal_rates_pass_the_audio_through)
{
  aud_resampler *rs = aud_resample_create(48000u, 48000u, 1);
  size_t n = 4800;
  float *in = calloc(n, sizeof(*in));
  float *out;
  size_t got;

  CHECK(rs != NULL);
  if (rs == NULL || in == NULL)
  {
    free(in);
    aud_resample_destroy(rs);
    return;
  }
  out = calloc(aud_resample_out_max(rs, n), sizeof(*out));
  CHECK(out != NULL);
  if (out == NULL)
  {
    free(in);
    aud_resample_destroy(rs);
    return;
  }

  tone(in, n, 1, 1000.0, 48000u, 0.5);
  got = aud_resample_run(rs, in, n, out, aud_resample_out_max(rs, n));
  CHECK(got > 0);

  /*
   * Not a bit-exact copy - it still runs through the filter - but the delay is
   * a whole number of samples and the level is unchanged, so lining the two up
   * shows the audio came through untouched.
   */
  for (size_t f = 0; f + aud_resample_latency(rs) < got && f < 2000u; f++)
  {
    CHECK_EQ_DBL(out[f + aud_resample_latency(rs)], in[f], 2e-3);
  }

  free(out);
  free(in);
  aud_resample_destroy(rs);
}

TEST(a_reset_forgets_what_came_before)
{
  aud_resampler *rs = aud_resample_create(44100u, 48000u, 1);
  float loud[512];
  float quiet[512];
  float out[1024];
  size_t got;

  CHECK(rs != NULL);
  if (rs == NULL)
  {
    return;
  }

  tone(loud, 512, 1, 1000.0, 44100u, 1.0);
  for (size_t i = 0; i < 512; i++)
  {
    quiet[i] = 0.0f;
  }

  aud_resample_run(rs, loud, 512, out, 1024);

  /*
   * Without the reset the taps behind the seam would still be holding the loud
   * passage, and the silence after a seek would open with a burst of whatever
   * was playing before it.
   */
  aud_resample_reset(rs);
  got = aud_resample_run(rs, quiet, 512, out, 1024);
  CHECK(got > 0);
  for (size_t i = 0; i < got; i++)
  {
    CHECK(fabs((double)out[i]) < 1e-6);
  }

  aud_resample_destroy(rs);
}

int main(void)
{
  RUN(a_converter_is_refused_nonsense);
  RUN(the_frame_count_follows_the_ratio);
  RUN(a_tone_survives_going_up_and_coming_down);
  RUN(coming_down_does_not_fold_the_top_back_in);
  RUN(the_channels_stay_apart);
  RUN(a_stream_cut_into_pieces_comes_out_the_same);
  RUN(equal_rates_pass_the_audio_through);
  RUN(a_reset_forgets_what_came_before);

  return TEST_RESULT();
}
