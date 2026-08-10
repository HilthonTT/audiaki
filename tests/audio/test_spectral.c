/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "audio/fft.h"
#include "audio/spectral.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AUD_TEST_PI 3.14159265358979323846

#define TEST_RATE 48000u
#define TEST_FFT 1024u

static void add_sine(float *dst, size_t frames, double hz, double amp)
{
  for (size_t i = 0; i < frames; i++)
  {
    dst[i] += (float)(amp * sin(2.0 * AUD_TEST_PI * hz * (double)i / (double)TEST_RATE));
  }
}

/* Root mean square of one channel of an interleaved buffer. */
static double rms_at(const float *buf, size_t frames, unsigned channels, unsigned ch)
{
  double total = 0.0;

  for (size_t i = 0; i < frames; i++)
  {
    double v = (double)buf[i * channels + ch];
    total += v * v;
  }
  return frames > 0 ? sqrt(total / (double)frames) : 0.0;
}

/* Read a whole buffer into `s` as a run of windows, the way edit/repair does. */
static void read_all(aud_spectral *s, const float *mono, size_t frames)
{
  size_t n = aud_spectral_size(s);

  aud_spectral_read_begin(s);
  for (size_t at = 0; at + n <= frames; at += n / 2u)
  {
    aud_spectral_read(s, mono + at, n);
  }
  aud_spectral_read_end(s);
}

/* -- the inverse transform -------------------------------------------------- */

TEST(inverse_undoes_forward)
{
  float re[32];
  float im[32];
  float want[32];

  for (size_t i = 0; i < 32; i++)
  {
    re[i] = (float)sin(0.3 * (double)i) + 0.25f * (float)i;
    im[i] = 0.0f;
    want[i] = re[i];
  }

  aud_fft_forward(re, im, 32);
  aud_fft_inverse(re, im, 32);

  for (size_t i = 0; i < 32; i++)
  {
    CHECK_EQ_DBL(re[i], want[i], 1e-4);
    CHECK_EQ_DBL(im[i], 0.0, 1e-4);
  }
}

TEST(inverse_leaves_bad_sizes_alone)
{
  float re[3] = {1.0f, 2.0f, 3.0f};
  float im[3] = {0.0f, 0.0f, 0.0f};

  aud_fft_inverse(re, im, 3);
  aud_fft_inverse(NULL, im, 2);

  CHECK_EQ_DBL(re[0], 1.0, 1e-9);
  CHECK_EQ_DBL(re[2], 3.0, 1e-9);
}

/* -- the analyser itself ---------------------------------------------------- */

TEST(spectral_rejects_bad_geometry)
{
  CHECK(aud_spectral_create(TEST_RATE, 1000) == NULL); /* not a power of two */
  CHECK(aud_spectral_create(100, TEST_FFT) == NULL);   /* implausible rate */
}

TEST(spectral_bins_and_frequencies_line_up)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  CHECK_EQ_INT(aud_spectral_bins(s), TEST_FFT / 2u + 1u);
  CHECK_EQ_DBL(aud_spectral_hz(s, 0), 0.0, 1e-9);

  /* the top bin is Nyquist, and a frequency maps back to the bin it fell in */
  CHECK_EQ_DBL(aud_spectral_hz(s, aud_spectral_bins(s) - 1), TEST_RATE / 2.0, 1e-6);
  CHECK_EQ_INT(aud_spectral_bin_at(s, aud_spectral_hz(s, 40)), 40);

  aud_spectral_destroy(s);
}

TEST(reading_finds_the_tone_that_is_there)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  const size_t frames = TEST_FFT * 8u;
  float *audio;
  const float *mean;
  size_t loudest = 0;

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  audio = calloc(frames, sizeof(*audio));
  CHECK(audio != NULL);
  if (audio == NULL)
  {
    aud_spectral_destroy(s);
    return;
  }

  CHECK(!aud_spectral_has_reading(s));

  add_sine(audio, frames, 1000.0, 0.5);
  read_all(s, audio, frames);

  CHECK(aud_spectral_has_reading(s));
  CHECK(aud_spectral_windows(s) > 4);

  mean = aud_spectral_mean(s);
  for (size_t k = 1; k < aud_spectral_bins(s); k++)
  {
    if (mean[k] > mean[loudest])
    {
      loudest = k;
    }
  }

  CHECK_EQ_DBL(aud_spectral_hz(s, loudest), 1000.0, 30.0);
  /* half amplitude is -6 dBFS, and the window's coherent gain is normalised out */
  CHECK_EQ_DBL(aud_spectral_db(mean[loudest]), -6.0, 1.0);

  free(audio);
  aud_spectral_destroy(s);
}

/* -- resynthesis ------------------------------------------------------------ */

TEST(a_flat_curve_returns_the_audio_unchanged)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  const size_t frames = 9000;
  float *in;
  float *out;

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  in = calloc(frames, sizeof(*in));
  out = calloc(frames, sizeof(*out));
  CHECK(in != NULL && out != NULL);
  if (in == NULL || out == NULL)
  {
    free(in);
    free(out);
    aud_spectral_destroy(s);
    return;
  }

  add_sine(in, frames, 220.0, 0.4);
  add_sine(in, frames, 1370.0, 0.2);

  CHECK(!aud_spectral_would_change(s));
  CHECK_EQ_INT(aud_spectral_process(s, in, out, frames, 1), 0);

  /*
   * Every sample, the two ends included. The overlap-add divides by the window
   * energy that actually landed on each sample rather than by the constant it
   * comes to in the middle, so there is no fade in or out to allow for.
   */
  for (size_t i = 0; i < frames; i++)
  {
    CHECK_EQ_DBL(out[i], in[i], 1e-4);
  }

  free(in);
  free(out);
  aud_spectral_destroy(s);
}

TEST(a_notch_takes_out_one_tone_and_leaves_the_other)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  const size_t frames = 16000;
  float *hum;
  float *note;
  float *both;
  float *out;

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  hum = calloc(frames, sizeof(*hum));
  note = calloc(frames, sizeof(*note));
  both = calloc(frames, sizeof(*both));
  out = calloc(frames, sizeof(*out));
  CHECK(hum != NULL && note != NULL && both != NULL && out != NULL);
  if (hum == NULL || note == NULL || both == NULL || out == NULL)
  {
    free(hum);
    free(note);
    free(both);
    free(out);
    aud_spectral_destroy(s);
    return;
  }

  /* a bass low E with mains hum sitting under it, which is the whole point */
  add_sine(hum, frames, 300.0, 0.3);
  add_sine(note, frames, 2000.0, 0.3);
  for (size_t i = 0; i < frames; i++)
  {
    both[i] = hum[i] + note[i];
  }

  aud_spectral_notch(s, 300.0, 60.0, 1u, 0.0f);
  CHECK(aud_spectral_would_change(s));
  CHECK_EQ_INT(aud_spectral_process(s, both, out, frames, 1), 0);

  /*
   * What is left should be the other tone on its own. Measured away from the
   * ends, where the notch's own ring has not settled.
   */
  {
    size_t skip = TEST_FFT;
    double kept = rms_at(out + skip, frames - 2u * skip, 1u, 0u);
    double wanted = rms_at(note + skip, frames - 2u * skip, 1u, 0u);
    double before = rms_at(both + skip, frames - 2u * skip, 1u, 0u);

    CHECK(before > wanted * 1.2); /* the hum really was in there */
    CHECK_EQ_DBL(kept, wanted, wanted * 0.1);
  }

  free(hum);
  free(note);
  free(both);
  free(out);
  aud_spectral_destroy(s);
}

TEST(a_notch_reaches_the_harmonics_too)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  const float *curve;

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  aud_spectral_notch(s, 50.0, 20.0, 4u, 0.0f);
  curve = aud_spectral_curve(s);

  for (unsigned h = 1u; h <= 4u; h++)
  {
    CHECK_EQ_DBL(curve[aud_spectral_bin_at(s, 50.0 * h)], 0.0, 1e-6);
  }

  /* and nothing was taken out between them */
  CHECK_EQ_DBL(curve[aud_spectral_bin_at(s, 1000.0)], 1.0, 1e-6);

  aud_spectral_flatten(s);
  CHECK(!aud_spectral_would_change(s));
  CHECK_EQ_DBL(aud_spectral_curve(s)[aud_spectral_bin_at(s, 50.0)], 1.0, 1e-6);

  aud_spectral_destroy(s);
}

TEST(process_handles_every_channel)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  const size_t frames = 12000;
  float *buf;

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  buf = calloc(frames * 2u, sizeof(*buf));
  CHECK(buf != NULL);
  if (buf == NULL)
  {
    aud_spectral_destroy(s);
    return;
  }

  /* the same hum in both sides, a note in the right one only */
  for (size_t i = 0; i < frames; i++)
  {
    double t = (double)i / (double)TEST_RATE;

    buf[i * 2u] = (float)(0.3 * sin(2.0 * AUD_TEST_PI * 300.0 * t));
    buf[i * 2u + 1u] = (float)(0.3 * sin(2.0 * AUD_TEST_PI * 300.0 * t) +
                               0.3 * sin(2.0 * AUD_TEST_PI * 2000.0 * t));
  }

  /* wide enough to cover the tone's skirt, which falls between two bins here */
  aud_spectral_notch(s, 300.0, 200.0, 1u, 0.0f);
  /* in place, which is what edit/repair.c does */
  CHECK_EQ_INT(aud_spectral_process(s, buf, buf, frames, 2u), 0);

  {
    size_t skip = TEST_FFT;
    double left = rms_at(buf + skip * 2u, frames - 2u * skip, 2u, 0u);
    double right = rms_at(buf + skip * 2u, frames - 2u * skip, 2u, 1u);

    /* the left held nothing but hum, so it should be all but empty now */
    CHECK(left < 0.3 / sqrt(2.0) * 0.02);
    CHECK_EQ_DBL(right, 0.3 / sqrt(2.0), 0.02); /* the note came through */
  }

  free(buf);
  aud_spectral_destroy(s);
}

TEST(process_refuses_what_it_cannot_do)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  float buf[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  CHECK_EQ_INT(aud_spectral_process(s, NULL, buf, 4, 1), -1);
  CHECK_EQ_INT(aud_spectral_process(s, buf, buf, 0, 1), -1);
  CHECK_EQ_INT(aud_spectral_process(s, buf, buf, 4, 0), -1);
  CHECK_EQ_INT(aud_spectral_context(s), TEST_FFT);

  aud_spectral_destroy(s);
}

/* -- the noise profile ------------------------------------------------------ */

/* A steady hiss, from a fixed seed so the test says the same thing every run. */
static void add_hiss(float *dst, size_t frames, double amp, unsigned seed)
{
  unsigned state = seed;

  for (size_t i = 0; i < frames; i++)
  {
    state = state * 1103515245u + 12345u;
    dst[i] += (float)(amp * ((double)((state >> 16) & 0x7fffu) / 16383.5 - 1.0));
  }
}

TEST(guessing_the_noise_finds_what_never_goes_away)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  const size_t frames = TEST_FFT * 32u;
  float *audio;

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  audio = calloc(frames, sizeof(*audio));
  CHECK(audio != NULL);
  if (audio == NULL)
  {
    aud_spectral_destroy(s);
    return;
  }

  /* hiss all the way through, and a note over the first half only */
  add_hiss(audio, frames, 0.02, 7u);
  add_sine(audio, frames / 2u, 500.0, 0.5);

  read_all(s, audio, frames);

  CHECK(!aud_spectral_has_noise(s));
  aud_spectral_guess_noise(s);
  CHECK(aud_spectral_has_noise(s));

  {
    const float *noise = aud_spectral_noise(s);
    size_t at_note = aud_spectral_bin_at(s, 500.0);
    size_t elsewhere = aud_spectral_bin_at(s, 5000.0);

    /*
     * The minimum over time at the note's own frequency is the hiss under it,
     * not the note: the note is absent from half the windows. So the profile
     * should read about the same either side of it.
     */
    CHECK(aud_spectral_db(noise[at_note]) < aud_spectral_db(noise[elsewhere]) + 12.0f);
  }

  aud_spectral_forget_noise(s);
  CHECK(!aud_spectral_has_noise(s));
  CHECK(aud_spectral_noise(s) == NULL);

  free(audio);
  aud_spectral_destroy(s);
}

TEST(subtraction_lowers_the_floor_and_keeps_the_note)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  aud_spectral *after_view = aud_spectral_create(TEST_RATE, TEST_FFT);
  const size_t frames = TEST_FFT * 24u;
  float *quiet_part;
  float *audio;
  float *out;
  double before;
  double after;
  size_t at_note;
  size_t at_hiss;

  CHECK(s != NULL && after_view != NULL);
  if (s == NULL || after_view == NULL)
  {
    aud_spectral_destroy(s);
    aud_spectral_destroy(after_view);
    return;
  }

  quiet_part = calloc(frames, sizeof(*quiet_part));
  audio = calloc(frames, sizeof(*audio));
  out = calloc(frames, sizeof(*out));
  CHECK(quiet_part != NULL && audio != NULL && out != NULL);
  if (quiet_part == NULL || audio == NULL || out == NULL)
  {
    free(quiet_part);
    free(audio);
    free(out);
    aud_spectral_destroy(s);
    aud_spectral_destroy(after_view);
    return;
  }

  /*
   * The workflow the panel is built around: read a stretch with nothing being
   * played on it, call that the noise, then put the take through it.
   */
  add_hiss(quiet_part, frames, 0.05, 11u);
  read_all(s, quiet_part, frames);
  aud_spectral_learn_noise(s);
  CHECK(aud_spectral_has_noise(s));

  add_hiss(audio, frames, 0.05, 11u);
  add_sine(audio, frames, 800.0, 0.4);

  aud_spectral_set_reduction(s, 2.0f, -24.0f);
  CHECK(aud_spectral_would_change(s));
  CHECK_EQ_INT(aud_spectral_process(s, audio, out, frames, 1), 0);

  read_all(after_view, out, frames);
  at_note = aud_spectral_bin_at(after_view, 800.0);
  at_hiss = aud_spectral_bin_at(after_view, 6000.0);

  /* the note came through at about the level it went in at: 0.4 is -8 dBFS */
  CHECK_EQ_DBL(aud_spectral_db(aud_spectral_mean(after_view)[at_note]), -8.0, 3.0);

  /* and the hiss well away from it is a long way down */
  read_all(s, audio, frames);
  before = (double)aud_spectral_db(aud_spectral_mean(s)[at_hiss]);
  after = (double)aud_spectral_db(aud_spectral_mean(after_view)[at_hiss]);
  CHECK(after < before - 10.0);

  free(quiet_part);
  free(audio);
  free(out);
  aud_spectral_destroy(s);
  aud_spectral_destroy(after_view);
}

TEST(the_strength_and_floor_are_held_to_their_bounds)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  aud_spectral_set_reduction(s, 99.0f, 40.0f);
  CHECK_EQ_DBL(aud_spectral_strength(s), AUD_SPECTRAL_STRENGTH_MAX, 1e-6);
  CHECK_EQ_DBL(aud_spectral_floor_db(s), AUD_SPECTRAL_FLOOR_MAX_DB, 1e-6);

  aud_spectral_set_reduction(s, -1.0f, -999.0f);
  CHECK_EQ_DBL(aud_spectral_strength(s), 0.0, 1e-6);
  CHECK_EQ_DBL(aud_spectral_floor_db(s), AUD_SPECTRAL_FLOOR_MIN_DB, 1e-6);

  /* no strength behind it is nothing to apply, profile or not */
  aud_spectral_guess_noise(s);
  CHECK(!aud_spectral_would_change(s));

  aud_spectral_destroy(s);
}

/* -- finding a hum ---------------------------------------------------------- */

TEST(find_hum_picks_out_a_planted_mains_buzz)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  const size_t frames = TEST_FFT * 32u;
  float *audio;

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  audio = calloc(frames, sizeof(*audio));
  CHECK(audio != NULL);
  if (audio == NULL)
  {
    aud_spectral_destroy(s);
    return;
  }

  /* 50 Hz and its first three harmonics, all the way through */
  add_sine(audio, frames, 50.0, 0.05);
  add_sine(audio, frames, 100.0, 0.04);
  add_sine(audio, frames, 150.0, 0.03);
  add_sine(audio, frames, 200.0, 0.02);
  /* a bass note over the first half, which must not be mistaken for the hum */
  add_sine(audio, frames / 2u, 82.4, 0.5);

  read_all(s, audio, frames);
  CHECK_EQ_DBL(aud_spectral_find_hum(s), 50.0, 25.0);

  free(audio);
  aud_spectral_destroy(s);
}

TEST(find_hum_says_nothing_when_there_is_none)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  const size_t frames = TEST_FFT * 16u;
  float *audio;

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  /* no reading at all yet */
  CHECK_EQ_DBL(aud_spectral_find_hum(s), 0.0, 1e-9);

  audio = calloc(frames, sizeof(*audio));
  CHECK(audio != NULL);
  if (audio == NULL)
  {
    aud_spectral_destroy(s);
    return;
  }

  add_hiss(audio, frames, 0.05, 3u);
  read_all(s, audio, frames);

  CHECK_EQ_DBL(aud_spectral_find_hum(s), 0.0, 1e-9);

  free(audio);
  aud_spectral_destroy(s);
}

/* -- the predicted curve ---------------------------------------------------- */

TEST(the_result_curve_matches_what_processing_does)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  const size_t frames = TEST_FFT * 16u;
  float *audio;
  float *result;

  CHECK(s != NULL);
  if (s == NULL)
  {
    return;
  }

  audio = calloc(frames, sizeof(*audio));
  result = calloc(aud_spectral_bins(s), sizeof(*result));
  CHECK(audio != NULL && result != NULL);
  if (audio == NULL || result == NULL)
  {
    free(audio);
    free(result);
    aud_spectral_destroy(s);
    return;
  }

  add_sine(audio, frames, 300.0, 0.3);
  add_sine(audio, frames, 2000.0, 0.3);
  read_all(s, audio, frames);

  aud_spectral_notch(s, 300.0, 60.0, 1u, 0.0f);
  aud_spectral_result(s, result);

  /* the notched tone is gone from the prediction, the other one untouched */
  CHECK_EQ_DBL(result[aud_spectral_bin_at(s, 300.0)], 0.0, 1e-6);
  CHECK_EQ_DBL(result[aud_spectral_bin_at(s, 2000.0)],
               aud_spectral_mean(s)[aud_spectral_bin_at(s, 2000.0)], 1e-6);

  free(audio);
  free(result);
  aud_spectral_destroy(s);
}

int main(void)
{
  RUN(inverse_undoes_forward);
  RUN(inverse_leaves_bad_sizes_alone);
  RUN(spectral_rejects_bad_geometry);
  RUN(spectral_bins_and_frequencies_line_up);
  RUN(reading_finds_the_tone_that_is_there);
  RUN(a_flat_curve_returns_the_audio_unchanged);
  RUN(a_notch_takes_out_one_tone_and_leaves_the_other);
  RUN(a_notch_reaches_the_harmonics_too);
  RUN(process_handles_every_channel);
  RUN(process_refuses_what_it_cannot_do);
  RUN(guessing_the_noise_finds_what_never_goes_away);
  RUN(subtraction_lowers_the_floor_and_keeps_the_note);
  RUN(the_strength_and_floor_are_held_to_their_bounds);
  RUN(find_hum_picks_out_a_planted_mains_buzz);
  RUN(find_hum_says_nothing_when_there_is_none);
  RUN(the_result_curve_matches_what_processing_does);
  return TEST_RESULT();
}
