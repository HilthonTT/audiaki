/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "audio/format.h"
#include "audio/loudness.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#define PI 3.14159265358979323846

/* Fed a chunk at a time, so a half minute of stereo is not a half minute of
 * stereo held in memory at once. */
#define CHUNK 4800u

/*
 * Feed `seconds` of a sine at `freq`, or of digital silence when `amplitude` is
 * zero, into `l`. `phase` is where the first sample lands, which is what
 * decides whether the peaks of a high frequency fall on samples or between them.
 */
static void feed_sine(aud_loudness *l, unsigned rate, unsigned channels, double freq,
                      double amplitude, double phase, double seconds)
{
  float buf[CHUNK * 2u];
  size_t total = (size_t)(seconds * rate);
  size_t done = 0;

  while (done < total)
  {
    size_t take = total - done < CHUNK ? total - done : CHUNK;

    for (size_t f = 0; f < take; f++)
    {
      double t = (double)(done + f) / (double)rate;
      double v = amplitude * sin(2.0 * PI * freq * t + phase);

      for (unsigned c = 0; c < channels; c++)
      {
        buf[f * channels + c] = (float)v;
      }
    }

    CHECK_EQ_INT(aud_loudness_feed(l, buf, take), 0);
    done += take;
  }
}

/* -- the K-weighting, against the table in the standard --------------------- */

/* |H|^2 of one biquad at `w` radians a sample. */
static double biquad_power(const double *b, const double *a, double w)
{
  double num_re = b[0] + b[1] * cos(w) + b[2] * cos(2.0 * w);
  double num_im = b[1] * sin(w) + b[2] * sin(2.0 * w);
  double den_re = 1.0 + a[0] * cos(w) + a[1] * cos(2.0 * w);
  double den_im = a[0] * sin(w) + a[1] * sin(2.0 * w);

  return (num_re * num_re + num_im * num_im) / (den_re * den_re + den_im * den_im);
}

/*
 * What BS.1770-4 says the K-weighting is at 48 kHz, printed in its own tables.
 * The module derives its coefficients at whatever rate it is handed rather than
 * carrying this table, so predicting a loudness from these and finding it is
 * what checks the derivation - against the standard, not against itself.
 */
static double reference_k_power(double freq)
{
  static const double shelf_b[3] = {1.53512485958697, -2.69169618940638,
                                    1.19839281085285};
  static const double shelf_a[2] = {-1.69065929318241, 0.73248077421585};
  static const double rlb_b[3] = {1.0, -2.0, 1.0};
  static const double rlb_a[2] = {-1.99004745483398, 0.99007225036621};
  double w = 2.0 * PI * freq / 48000.0;

  return biquad_power(shelf_b, shelf_a, w) * biquad_power(rlb_b, rlb_a, w);
}

/*
 * The loudness a steady sine of this amplitude ought to read: the mean square
 * of a sine is half its amplitude squared, every channel counts at full weight
 * and so adds, and BS.1770's own offset is the -0.691.
 */
static double expected_lufs(double amplitude, unsigned channels, double freq)
{
  double power = (double)channels * amplitude * amplitude / 2.0 * reference_k_power(freq);

  return -0.691 + 10.0 * log10(power);
}

TEST(a_sine_reads_what_the_standard_says_it_should)
{
  static const double freqs[] = {40.0, 200.0, 1000.0, 6000.0, 12000.0};

  for (unsigned i = 0; i < sizeof(freqs) / sizeof(*freqs); i++)
  {
    aud_loudness *l = aud_loudness_create(48000, 2);
    aud_loudness_reading got;

    CHECK(l != NULL);
    feed_sine(l, 48000, 2, freqs[i], 0.5, 0.0, 10.0);
    aud_loudness_read(l, &got);

    /*
     * A tenth of a decibel across five frequencies two and a half octaves
     * apart: the shelf and the high pass are both in the right place and both
     * the right depth, or one of these would be out.
     */
    CHECK_EQ_DBL(got.integrated, expected_lufs(0.5, 2, freqs[i]), 0.1);
    aud_loudness_destroy(l);
  }
}

/*
 * The calibration everyone quotes, and the one worth pinning on its own: a
 * stereo 1 kHz sine at -23 dBFS reads -23.0 LUFS. It comes out even because the
 * K-weighting lifts 1 kHz by 0.691 dB and BS.1770's offset takes 0.691 back off.
 */
TEST(the_reference_tone_reads_minus_twenty_three)
{
  aud_loudness *l = aud_loudness_create(48000, 2);
  aud_loudness_reading got;

  CHECK(l != NULL);
  feed_sine(l, 48000, 2, 1000.0, pow(10.0, -23.0 / 20.0), 0.0, 10.0);
  aud_loudness_read(l, &got);

  CHECK_EQ_DBL(got.integrated, -23.0, 0.1);
  CHECK_EQ_DBL(got.momentary_max, -23.0, 0.1);
  CHECK_EQ_DBL(got.short_max, -23.0, 0.1);

  /* nothing changes from one second to the next, so there is no range */
  CHECK_EQ_DBL(got.range, 0.0, 0.1);
  aud_loudness_destroy(l);
}

/*
 * The same performance at another rate is the same loudness. The coefficients
 * are derived per rate precisely so this holds; a tabulated 48 kHz filter used
 * at 44.1 would put the shelf 350 Hz out and fail here.
 */
TEST(the_rate_does_not_change_the_answer)
{
  static const unsigned rates[] = {44100, 48000, 88200, 96000};
  double at_48k = 0.0;

  for (unsigned i = 0; i < sizeof(rates) / sizeof(*rates); i++)
  {
    aud_loudness *l = aud_loudness_create(rates[i], 2);
    aud_loudness_reading got;

    CHECK(l != NULL);
    feed_sine(l, rates[i], 2, 1000.0, 0.25, 0.0, 8.0);
    aud_loudness_read(l, &got);

    if (rates[i] == 48000)
    {
      at_48k = got.integrated;
    }
    CHECK_EQ_DBL(got.integrated, expected_lufs(0.25, 2, 1000.0), 0.1);
    aud_loudness_destroy(l);
  }

  CHECK(aud_loudness_measured(at_48k));
}

/* Two channels carrying the same thing are twice the power and so 3 dB louder.
 * That is the summation BS.1770 specifies, and it is why LUFS and dBFS do not
 * convert into one another. */
TEST(a_second_channel_is_three_decibels)
{
  aud_loudness *mono = aud_loudness_create(48000, 1);
  aud_loudness *stereo = aud_loudness_create(48000, 2);
  aud_loudness_reading one;
  aud_loudness_reading two;

  CHECK(mono != NULL && stereo != NULL);
  feed_sine(mono, 48000, 1, 1000.0, 0.5, 0.0, 5.0);
  feed_sine(stereo, 48000, 2, 1000.0, 0.5, 0.0, 5.0);
  aud_loudness_read(mono, &one);
  aud_loudness_read(stereo, &two);

  CHECK_EQ_DBL(two.integrated - one.integrated, 3.0103, 0.02);
  aud_loudness_destroy(mono);
  aud_loudness_destroy(stereo);
}

/* -- the gate --------------------------------------------------------------- */

/*
 * The point of gating, and the reason a plain average will not do: a take with
 * four times as much silence in it as playing is not four times quieter. The
 * ungated mean of this would land near -30 LUFS; the gated one is what the
 * playing measured.
 */
TEST(silence_between_the_notes_is_not_averaged_in)
{
  aud_loudness *l = aud_loudness_create(48000, 2);
  aud_loudness_reading got;

  CHECK(l != NULL);
  feed_sine(l, 48000, 2, 1000.0, pow(10.0, -23.0 / 20.0), 0.0, 5.0);
  feed_sine(l, 48000, 2, 1000.0, 0.0, 0.0, 20.0);
  aud_loudness_read(l, &got);

  CHECK_EQ_DBL(got.integrated, -23.0, 0.2);

  /* the loudest moment is unaffected by how much silence follows it */
  CHECK_EQ_DBL(got.momentary_max, -23.0, 0.1);
  aud_loudness_destroy(l);
}

/*
 * A passage 10 dB below another is a range of about 10 LU. Both halves are well
 * above the gates, so nothing is thrown away and what is left is the spread.
 */
TEST(the_range_is_the_spread_between_loud_and_quiet)
{
  aud_loudness *l = aud_loudness_create(48000, 2);
  aud_loudness_reading got;

  CHECK(l != NULL);
  feed_sine(l, 48000, 2, 1000.0, 0.5, 0.0, 20.0);
  feed_sine(l, 48000, 2, 1000.0, 0.5 / pow(10.0, 10.0 / 20.0), 0.0, 20.0);
  aud_loudness_read(l, &got);

  CHECK_EQ_DBL(got.range, 10.0, 0.5);

  /* and the loudest 3 s is the loud half, not an average of the two */
  CHECK_EQ_DBL(got.short_max, expected_lufs(0.5, 2, 1000.0), 0.1);
  aud_loudness_destroy(l);
}

/* Reading twice gives the same answer twice: the read sorts the block history
 * it keeps, and must not lose any of it doing so. */
TEST(reading_twice_reads_the_same)
{
  aud_loudness *l = aud_loudness_create(48000, 2);
  aud_loudness_reading first;
  aud_loudness_reading second;
  aud_loudness_reading third;

  CHECK(l != NULL);
  feed_sine(l, 48000, 2, 1000.0, 0.5, 0.0, 10.0);
  feed_sine(l, 48000, 2, 1000.0, 0.05, 0.0, 10.0);
  aud_loudness_read(l, &first);
  aud_loudness_read(l, &second);

  CHECK_EQ_DBL(second.integrated, first.integrated, 1e-9);
  CHECK_EQ_DBL(second.range, first.range, 1e-9);
  CHECK_EQ_DBL(second.short_max, first.short_max, 1e-9);

  /*
   * And feeding carries on afterwards rather than being disturbed by it. The
   * maximum is what this watches, because it is ungated and so has to move:
   * more of the loud half would leave the integrated figure where it is, since
   * the relative gate has already thrown the quiet half away.
   */
  feed_sine(l, 48000, 2, 1000.0, 1.0, 0.0, 5.0);
  aud_loudness_read(l, &third);
  CHECK(third.momentary_max > first.momentary_max + 5.0);
  CHECK(third.integrated > first.integrated);
  aud_loudness_destroy(l);
}

/* -- what cannot be measured ------------------------------------------------ */

TEST(a_take_too_short_for_the_window_has_no_loudness)
{
  aud_loudness *l = aud_loudness_create(48000, 2);
  aud_loudness_reading got;

  CHECK(l != NULL);
  feed_sine(l, 48000, 2, 1000.0, 0.5, 0.0, 0.25); /* under 400 ms */
  aud_loudness_read(l, &got);

  CHECK(!aud_loudness_measured(got.integrated));
  CHECK(!aud_loudness_measured(got.momentary_max));
  CHECK(!aud_loudness_measured(got.range));

  /* the true peak is not blocked, so it is there whatever the length */
  CHECK_EQ_DBL(got.true_peak, 0.5, 0.01);
  aud_loudness_destroy(l);
}

TEST(a_take_too_short_for_the_range_still_has_a_loudness)
{
  aud_loudness *l = aud_loudness_create(48000, 2);
  aud_loudness_reading got;

  CHECK(l != NULL);
  feed_sine(l, 48000, 2, 1000.0, 0.5, 0.0, 1.5); /* over 400 ms, under 3 s */
  aud_loudness_read(l, &got);

  CHECK(aud_loudness_measured(got.integrated));
  CHECK(aud_loudness_measured(got.momentary_max));
  CHECK(!aud_loudness_measured(got.short_max));
  CHECK(!aud_loudness_measured(got.range));
  aud_loudness_destroy(l);
}

TEST(digital_silence_has_no_loudness_at_all)
{
  aud_loudness *l = aud_loudness_create(48000, 2);
  aud_loudness_reading got;

  CHECK(l != NULL);
  feed_sine(l, 48000, 2, 1000.0, 0.0, 0.0, 10.0);
  aud_loudness_read(l, &got);

  /* not a very quiet take - no take. Every figure says so rather than
   * bottoming out at some floor that could be mistaken for a reading. */
  CHECK(!aud_loudness_measured(got.integrated));
  CHECK(!aud_loudness_measured(got.momentary_max));
  CHECK(!aud_loudness_measured(got.short_max));
  CHECK(!aud_loudness_measured(got.range));
  CHECK_EQ_DBL(got.true_peak, 0.0, 1e-12);
  aud_loudness_destroy(l);
}

TEST(a_rate_it_cannot_be_derived_at_is_refused)
{
  CHECK(!aud_loudness_supported(4000, 2));
  CHECK(!aud_loudness_supported(48000, 0));
  CHECK(!aud_loudness_supported(48000, AUD_LOUDNESS_MAX_CHANNELS + 1u));
  CHECK(aud_loudness_supported(AUD_LOUDNESS_MIN_RATE, 1));
  CHECK(aud_loudness_supported(48000, 2));

  errno = 0;
  CHECK(aud_loudness_create(4000, 2) == NULL);
  CHECK_EQ_INT(errno, EINVAL);
}

/* A meter that could not be made still answers, so a caller needs no second
 * path for "there is no figure here". */
TEST(no_meter_reads_as_nothing_measured)
{
  aud_loudness_reading got;

  aud_loudness_read(NULL, &got);
  CHECK(!aud_loudness_measured(got.integrated));
  CHECK(!aud_loudness_measured(got.range));
  CHECK(!aud_loudness_measured(got.momentary_max));
  CHECK(!aud_loudness_measured(got.short_max));
  CHECK_EQ_DBL(got.true_peak, 0.0, 1e-12);

  aud_loudness_read(NULL, NULL);
  CHECK_EQ_INT(aud_loudness_feed(NULL, NULL, 0), -1);
}

/* -- the true peak ---------------------------------------------------------- */

/*
 * The case the whole measurement exists for. A full scale sine at a quarter of
 * the rate, started an eighth of a cycle along, puts every sample at +-0.7071
 * and every peak exactly half way between two of them. Sample peak calls that
 * -3 dBFS and reports 3 dB of headroom that is not there; the waveform a
 * converter reconstructs reaches full scale.
 */
TEST(a_peak_between_two_samples_is_found)
{
  aud_loudness *l = aud_loudness_create(48000, 1);
  aud_loudness_reading got;

  CHECK(l != NULL);
  feed_sine(l, 48000, 1, 12000.0, 1.0, PI / 4.0, 1.0);
  aud_loudness_read(l, &got);

  /*
   * Within a quarter of a decibel of full scale, and the tolerance is the
   * oversampling rather than the filter: four times leaves the interpolated
   * grid a quarter of a sample coarse, so a peak between two of those points is
   * still slightly missed. BS.1770-4 specifies four times regardless, and every
   * other implementation reads a shade under here too.
   */
  CHECK_EQ_DBL(aud_format_dbfs(got.true_peak), 0.0, 0.25);

  /* the point being that it is well above what the samples themselves reach */
  CHECK(aud_format_dbfs(got.true_peak) > -1.0);
  aud_loudness_destroy(l);
}

/* A peak that does land on a sample is not inflated: phase 0 of the
 * interpolator is the sample itself, so this must come back exactly. */
TEST(a_peak_on_a_sample_is_not_exaggerated)
{
  aud_loudness *l = aud_loudness_create(48000, 1);
  aud_loudness_reading got;

  CHECK(l != NULL);
  /* 1 kHz at 48 kHz is 48 samples a cycle, so a sample lands on each crest */
  feed_sine(l, 48000, 1, 1000.0, 0.5, 0.0, 1.0);
  aud_loudness_read(l, &got);

  CHECK(got.true_peak >= 0.5 - 1e-4);
  CHECK(got.true_peak < 0.5 + 0.01);
  aud_loudness_destroy(l);
}

/* Float takes are allowed past full scale and the measurement has to say so
 * rather than clamping, or a mix that needs turning down would look fine. */
TEST(a_true_peak_past_full_scale_is_reported)
{
  aud_loudness *l = aud_loudness_create(48000, 1);
  aud_loudness_reading got;

  CHECK(l != NULL);
  feed_sine(l, 48000, 1, 1000.0, 1.5, 0.0, 1.0);
  aud_loudness_read(l, &got);

  CHECK(got.true_peak > 1.0);
  CHECK_EQ_DBL(aud_format_dbfs(got.true_peak), 20.0 * log10(1.5), 0.1);
  aud_loudness_destroy(l);
}

/*
 * Between the samples is only looked at where something could beat the peak so
 * far, which is what keeps the measurement cheap - and the risk that carries is
 * quietly missing one. A burst whose samples sit below the running peak but
 * whose waveform passes well above it is the case that catches a bound set too
 * tight: the samples here reach 0.53 against a peak of 0.5 already found, and
 * what is between them reaches 0.75.
 */
TEST(a_late_peak_below_the_running_one_is_still_found)
{
  aud_loudness *l = aud_loudness_create(48000, 1);
  aud_loudness_reading got;

  CHECK(l != NULL);
  feed_sine(l, 48000, 1, 1000.0, 0.5, 0.0, 10.0);
  feed_sine(l, 48000, 1, 12000.0, 0.75, PI / 4.0, 0.05);
  aud_loudness_read(l, &got);

  CHECK(got.true_peak > 0.70);
  CHECK_EQ_DBL(got.true_peak, 0.75, 0.03);
  aud_loudness_destroy(l);
}

/*
 * A take that ends on its loudest moment. The samples are looked between a
 * group at a time, so the few that have not filled a group when the take stops
 * still have to be read - or a peak in the last quarter of a millisecond would
 * be reported as whatever the samples themselves happened to reach.
 */
TEST(a_peak_in_the_last_few_samples_is_not_missed)
{
  aud_loudness *l = aud_loudness_create(48000, 1);
  aud_loudness_reading got;
  float tail[7];

  CHECK(l != NULL);
  feed_sine(l, 48000, 1, 1000.0, 0.1, 0.0, 1.0);

  /* seven samples - short of the twelve a group holds - of the pattern whose
   * peaks all fall midway between two samples */
  for (unsigned i = 0; i < 7u; i++)
  {
    tail[i] = (float)sin(PI * (double)i / 2.0 + PI / 4.0) * 0.7071f;
  }
  CHECK_EQ_INT(aud_loudness_feed(l, tail, 7), 0);
  aud_loudness_read(l, &got);

  /* the samples themselves never exceed 0.5, so anything above that was found
   * between them, in a group that never completed */
  CHECK(got.true_peak > 0.55);
  aud_loudness_destroy(l);
}

int main(void)
{
  RUN(a_sine_reads_what_the_standard_says_it_should);
  RUN(the_reference_tone_reads_minus_twenty_three);
  RUN(the_rate_does_not_change_the_answer);
  RUN(a_second_channel_is_three_decibels);
  RUN(silence_between_the_notes_is_not_averaged_in);
  RUN(the_range_is_the_spread_between_loud_and_quiet);
  RUN(reading_twice_reads_the_same);
  RUN(a_take_too_short_for_the_window_has_no_loudness);
  RUN(a_take_too_short_for_the_range_still_has_a_loudness);
  RUN(digital_silence_has_no_loudness_at_all);
  RUN(a_rate_it_cannot_be_derived_at_is_refused);
  RUN(no_meter_reads_as_nothing_measured);
  RUN(a_peak_between_two_samples_is_found);
  RUN(a_peak_on_a_sample_is_not_exaggerated);
  RUN(a_true_peak_past_full_scale_is_reported);
  RUN(a_late_peak_below_the_running_one_is_still_found);
  RUN(a_peak_in_the_last_few_samples_is_not_missed);

  return TEST_RESULT();
}
