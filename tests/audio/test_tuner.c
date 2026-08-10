/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "audio/tuner.h"

#include <math.h>
#include <stdlib.h>

#define TEST_RATE 44100u
#define TEST_SAMPLES 8192u

/* M_PI is an X/Open extension, and this project compiles with _POSIX_C_SOURCE */
#define AUD_TEST_PI 3.14159265358979323846

/* Concert pitches of the notes the tests reach for. */
#define HZ_B0 30.8677 /* low B on a five-string bass: the bottom of the range */
#define HZ_E2 82.4069
#define HZ_A2 110.0
#define HZ_C8 4186.01 /* a piccolo's top C: the top of it */
#define HZ_A4 440.0
#define HZ_C4 261.6256
#define HZ_E4 329.6276

static aud_tuner *make_tuner(double a4_hz)
{
  aud_tuner_config cfg;

  aud_tuner_config_defaults(&cfg, TEST_RATE);
  cfg.a4_hz = a4_hz;
  return aud_tuner_create(&cfg);
}

/* One partial of `hz` at `amplitude`, added into `dst`. */
static void add_partial(float *dst, size_t n, double hz, double amplitude)
{
  for (size_t i = 0; i < n; i++)
  {
    dst[i] += (float)(amplitude * sin(2.0 * AUD_TEST_PI * hz * (double)i / TEST_RATE));
  }
}

/* How far apart two frequencies are, in cents. */
static double cents_between(double a, double b)
{
  return 1200.0 * log2(a / b);
}

/* -- note arithmetic ------------------------------------------------------- */

TEST(describe_names_concert_pitch)
{
  aud_tuner_reading r;

  aud_tuner_describe(HZ_A4, 440.0, &r);
  CHECK(r.voiced);
  CHECK_EQ_INT(r.midi, 69);
  CHECK_EQ_STR(r.note, "A");
  CHECK_EQ_INT(r.octave, 4);
  CHECK_EQ_DBL(r.cents, 0.0, 1e-9);
  CHECK_EQ_DBL(r.target_hz, 440.0, 1e-9);
}

TEST(describe_walks_the_octaves)
{
  aud_tuner_reading r;

  /* middle C is MIDI 60, and an octave down from A4 is A3 */
  aud_tuner_describe(HZ_C4, 440.0, &r);
  CHECK_EQ_INT(r.midi, 60);
  CHECK_EQ_STR(r.note, "C");
  CHECK_EQ_INT(r.octave, 4);

  aud_tuner_describe(220.0, 440.0, &r);
  CHECK_EQ_INT(r.midi, 57);
  CHECK_EQ_STR(r.note, "A");
  CHECK_EQ_INT(r.octave, 3);

  /* a guitar's bottom string */
  aud_tuner_describe(HZ_E2, 440.0, &r);
  CHECK_EQ_INT(r.midi, 40);
  CHECK_EQ_STR(r.note, "E");
  CHECK_EQ_INT(r.octave, 2);
  CHECK_EQ_DBL(r.cents, 0.0, 0.1);
}

TEST(describe_measures_the_offset)
{
  aud_tuner_reading r;

  /* ten cents sharp of A4, and the same below it */
  aud_tuner_describe(440.0 * pow(2.0, 10.0 / 1200.0), 440.0, &r);
  CHECK_EQ_INT(r.midi, 69);
  CHECK_EQ_DBL(r.cents, 10.0, 1e-6);

  aud_tuner_describe(440.0 * pow(2.0, -10.0 / 1200.0), 440.0, &r);
  CHECK_EQ_INT(r.midi, 69);
  CHECK_EQ_DBL(r.cents, -10.0, 1e-6);

  /* just past halfway is the next note down, not a 51 cent error */
  aud_tuner_describe(440.0 * pow(2.0, -51.0 / 1200.0), 440.0, &r);
  CHECK_EQ_INT(r.midi, 68);
  CHECK_EQ_DBL(r.cents, 49.0, 1e-6);
}

TEST(describe_follows_the_reference_pitch)
{
  aud_tuner_reading r;

  /* at A = 432 an unchanged 440 Hz tone is no longer in tune */
  aud_tuner_describe(440.0, 432.0, &r);
  CHECK_EQ_INT(r.midi, 69);
  CHECK(r.cents > 30.0);
  CHECK_EQ_DBL(r.target_hz, 432.0, 1e-9);
}

TEST(describe_rejects_nonsense)
{
  aud_tuner_reading r;

  aud_tuner_describe(0.0, 440.0, &r);
  CHECK(!r.voiced);
  CHECK_EQ_STR(r.note, "--");

  aud_tuner_describe(-100.0, 440.0, &r);
  CHECK(!r.voiced);

  /* above MIDI 127, which is past the top of any instrument audiaki records */
  aud_tuner_describe(1.0e6, 440.0, &r);
  CHECK(!r.voiced);

  aud_tuner_describe(440.0, 0.0, &r);
  CHECK(!r.voiced);
}

TEST(note_frequency_round_trips)
{
  for (int midi = 21; midi <= 108; midi++)
  {
    aud_tuner_reading r;
    double hz = aud_tuner_note_frequency(midi, 440.0);

    aud_tuner_describe(hz, 440.0, &r);
    CHECK_EQ_INT(r.midi, midi);
    CHECK_EQ_DBL(r.cents, 0.0, 1e-6);
  }

  CHECK_EQ_DBL(aud_tuner_note_frequency(69, 440.0), 440.0, 1e-9);
  CHECK_EQ_DBL(aud_tuner_note_frequency(81, 440.0), 880.0, 1e-9);
  CHECK_EQ_DBL(aud_tuner_note_frequency(-1, 440.0), 0.0, 1e-9);
}

TEST(note_names_are_chromatic)
{
  CHECK_EQ_STR(aud_tuner_note_name(60), "C");
  CHECK_EQ_STR(aud_tuner_note_name(61), "C#");
  CHECK_EQ_STR(aud_tuner_note_name(69), "A");
  CHECK_EQ_STR(aud_tuner_note_name(-1), "--");
  CHECK_EQ_STR(aud_tuner_note_name(128), "--");
}

TEST(note_label_joins_name_and_octave)
{
  aud_tuner_reading r;
  char label[AUD_TUNER_LABEL_MAX];

  aud_tuner_describe(HZ_E2, 440.0, &r);
  aud_tuner_note_label(&r, label, sizeof(label));
  CHECK_EQ_STR(label, "E2");

  aud_tuner_describe(aud_tuner_note_frequency(70, 440.0), 440.0, &r);
  aud_tuner_note_label(&r, label, sizeof(label));
  CHECK_EQ_STR(label, "A#4");

  aud_tuner_describe(0.0, 440.0, &r);
  aud_tuner_note_label(&r, label, sizeof(label));
  CHECK_EQ_STR(label, "--");
}

/* -- configuration --------------------------------------------------------- */

TEST(create_rejects_a_bad_configuration)
{
  aud_tuner_config cfg;

  aud_tuner_config_defaults(&cfg, TEST_RATE);
  cfg.rate = 100u;
  CHECK(aud_tuner_create(&cfg) == NULL);

  aud_tuner_config_defaults(&cfg, TEST_RATE);
  cfg.max_hz = cfg.min_hz;
  CHECK(aud_tuner_create(&cfg) == NULL);

  /* nothing above Nyquist can be detected, so asking for it is a mistake */
  aud_tuner_config_defaults(&cfg, TEST_RATE);
  cfg.max_hz = (double)TEST_RATE;
  CHECK(aud_tuner_create(&cfg) == NULL);

  aud_tuner_config_defaults(&cfg, TEST_RATE);
  cfg.a4_hz = 100.0;
  CHECK(aud_tuner_create(&cfg) == NULL);
}

TEST(window_spans_the_lowest_note)
{
  aud_tuner *t = make_tuner(440.0);

  aud_tuner_config cfg;

  CHECK(t != NULL);
  /* three periods of the lowest pitch it looks for: two to compare, one to slide */
  aud_tuner_config_defaults(&cfg, TEST_RATE);
  CHECK(aud_tuner_window(t) >= (size_t)(3.0 * TEST_RATE / cfg.min_hz));
  aud_tuner_destroy(t);
}

/* -- detection ------------------------------------------------------------- */

/* Fill a tuner with `hz` and read the pitch back, in cents off `hz`. */
static double detect_cents_error(aud_tuner *t, double hz, aud_tuner_reading *out)
{
  float *buf = calloc(TEST_SAMPLES, sizeof(*buf));

  CHECK(buf != NULL);
  if (buf == NULL)
  {
    return 1.0e9;
  }

  add_partial(buf, TEST_SAMPLES, hz, 0.5);
  aud_tuner_push(t, buf, TEST_SAMPLES);
  free(buf);

  aud_tuner_analyse(t, 0.05, out);
  if (!out->voiced)
  {
    return 1.0e9;
  }

  return cents_between(out->frequency, hz);
}

TEST(detects_a_plain_tone)
{
  aud_tuner *t = make_tuner(440.0);
  aud_tuner_reading r;

  CHECK(t != NULL);
  CHECK_EQ_DBL(detect_cents_error(t, HZ_A2, &r), 0.0, 1.0);
  CHECK(r.voiced);
  CHECK_EQ_INT(r.midi, 45);
  CHECK_EQ_STR(r.note, "A");
  CHECK_EQ_INT(r.octave, 2);
  CHECK(r.confidence > 0.9);
  /* a 0.5 amplitude sine is about -9 dBFS RMS */
  CHECK(r.level_db > -12.0 && r.level_db < -6.0);

  aud_tuner_destroy(t);
}

TEST(detects_across_the_range)
{
  static const double notes[] = {HZ_E2, HZ_A2, 146.8324, HZ_C4, HZ_E4, 659.2551};

  for (size_t i = 0; i < sizeof(notes) / sizeof(notes[0]); i++)
  {
    aud_tuner *t = make_tuner(440.0);
    aud_tuner_reading r;

    CHECK(t != NULL);
    /* within two cents everywhere: a semitone is a hundred */
    CHECK_EQ_DBL(detect_cents_error(t, notes[i], &r), 0.0, 2.0);
    CHECK(fabs(r.cents) < 2.0);
    aud_tuner_destroy(t);
  }
}

TEST(the_default_range_reaches_the_instruments_it_claims)
{
  /*
   * The two ends of what the defaults promise: a five-string bass's low B, and
   * a piccolo's top C. Both used to be outside the range - the low end by three
   * semitones, the high end by more than an octave.
   */
  static const struct
  {
    double hz;
    double tolerance_cents;
  } ends[] = {
      {HZ_B0, 1.0},
      /*
       * Looser up here, and honestly so: at 44.1 kHz a 4186 Hz wave is about
       * ten samples long, so the lag the detector works in is coarse and the
       * parabolic interpolation between whole samples is doing the work. A few
       * cents at the top of a piccolo is still well inside what anyone can
       * hear, but it is not the hundredth of a cent the low end manages.
       */
      {HZ_C8, 5.0},
  };

  for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++)
  {
    aud_tuner *t = make_tuner(440.0);
    aud_tuner_reading r;

    CHECK(t != NULL);
    CHECK_EQ_DBL(detect_cents_error(t, ends[i].hz, &r), 0.0, ends[i].tolerance_cents);
    CHECK(r.voiced);
    aud_tuner_destroy(t);
  }
}

TEST(the_range_can_be_narrowed_and_widened)
{
  aud_tuner_config cfg;
  aud_tuner *t;
  aud_tuner_reading r;

  /*
   * Asking for less than the default is what a caller does to buy back the
   * cost: the analysis is quadratic in the lowest pitch searched, so a narrower
   * low end makes the window shorter and every reading cheaper.
   */
  aud_tuner_config_defaults(&cfg, TEST_RATE);
  cfg.min_hz = 80.0;
  t = aud_tuner_create(&cfg);
  CHECK(t != NULL);
  if (t != NULL)
  {
    aud_tuner *wide = make_tuner(440.0);

    CHECK(aud_tuner_window(t) < aud_tuner_window(wide));
    /* and it still finds what is inside the narrowed range */
    CHECK_EQ_DBL(detect_cents_error(t, HZ_A2, &r), 0.0, 2.0);
    aud_tuner_destroy(wide);
    aud_tuner_destroy(t);
  }

  /* an inverted range is refused rather than silently swapped */
  aud_tuner_config_defaults(&cfg, TEST_RATE);
  cfg.min_hz = 2000.0;
  cfg.max_hz = 100.0;
  CHECK(aud_tuner_create(&cfg) == NULL);
}

TEST(reads_a_detuned_string)
{
  aud_tuner *t = make_tuner(440.0);
  aud_tuner_reading r;
  double flat = HZ_E2 * pow(2.0, -25.0 / 1200.0);

  CHECK(t != NULL);
  detect_cents_error(t, flat, &r);

  CHECK(r.voiced);
  CHECK_EQ_INT(r.midi, 40); /* still E2, just not a very good one */
  CHECK_EQ_DBL(r.cents, -25.0, 2.0);
  CHECK(fabs(r.cents) > AUD_TUNER_IN_TUNE_CENTS);

  aud_tuner_destroy(t);
}

/*
 * The reason this is not a peak finder over the spectrum. A plucked low string
 * often has far more energy in its harmonics than in the fundamental, and here
 * the fundamental is missing altogether - the strongest bin is an octave up, and
 * a tuner that believed it would send you tuning to the wrong note.
 */
TEST(detects_a_missing_fundamental)
{
  aud_tuner *t = make_tuner(440.0);
  aud_tuner_reading r;
  float *buf = calloc(TEST_SAMPLES, sizeof(*buf));

  CHECK(t != NULL);
  CHECK(buf != NULL);
  if (t == NULL || buf == NULL)
  {
    return;
  }

  add_partial(buf, TEST_SAMPLES, HZ_E2 * 2.0, 0.5);
  add_partial(buf, TEST_SAMPLES, HZ_E2 * 3.0, 0.3);
  add_partial(buf, TEST_SAMPLES, HZ_E2 * 4.0, 0.2);

  aud_tuner_push(t, buf, TEST_SAMPLES);
  free(buf);

  aud_tuner_analyse(t, 0.05, &r);

  CHECK(r.voiced);
  CHECK_EQ_INT(r.midi, 40); /* E2, not the E3 that dominates the spectrum */
  CHECK_EQ_DBL(cents_between(r.frequency, HZ_E2), 0.0, 2.0);

  aud_tuner_destroy(t);
}

TEST(silence_is_not_a_note)
{
  aud_tuner *t = make_tuner(440.0);
  aud_tuner_reading r;
  float quiet[TEST_SAMPLES];

  CHECK(t != NULL);

  for (size_t i = 0; i < TEST_SAMPLES; i++)
  {
    quiet[i] = 0.0f;
  }

  aud_tuner_push(t, quiet, TEST_SAMPLES);
  CHECK(aud_tuner_analyse(t, 0.05, &r) == 0);
  CHECK(!r.voiced);
  CHECK_EQ_STR(r.note, "--");

  /* and a tone far too quiet to be anything but the room */
  add_partial(quiet, TEST_SAMPLES, HZ_A2, 0.0005);
  aud_tuner_push(t, quiet, TEST_SAMPLES);
  CHECK(aud_tuner_analyse(t, 0.05, &r) == 0);
  CHECK(!r.voiced);

  aud_tuner_destroy(t);
}

/*
 * A plucked note decays through the gate long before it stops being the note
 * you are tuning, so a reading outlives the sound that made it for a moment.
 */
TEST(a_reading_outlives_the_note)
{
  aud_tuner_config cfg;
  aud_tuner *t;
  aud_tuner_reading r;
  float quiet[TEST_SAMPLES];

  aud_tuner_config_defaults(&cfg, TEST_RATE);
  cfg.hold = 0.5;
  t = aud_tuner_create(&cfg);
  CHECK(t != NULL);
  if (t == NULL)
  {
    return;
  }

  detect_cents_error(t, HZ_A2, &r);
  CHECK(r.voiced);

  for (size_t i = 0; i < TEST_SAMPLES; i++)
  {
    quiet[i] = 0.0f;
  }

  /* silence, but not for long enough to have let go of the note */
  aud_tuner_push(t, quiet, TEST_SAMPLES);
  CHECK(aud_tuner_analyse(t, 0.2, &r) != 0);
  CHECK(r.voiced);
  CHECK_EQ_INT(r.midi, 45);

  /* past the hold, silence means silence */
  aud_tuner_push(t, quiet, TEST_SAMPLES);
  CHECK(aud_tuner_analyse(t, 0.5, &r) == 0);
  CHECK(!r.voiced);

  aud_tuner_destroy(t);
}

TEST(handles_being_handed_nothing)
{
  aud_tuner_reading r;

  CHECK(aud_tuner_analyse(NULL, 0.05, &r) == 0);
  CHECK(!r.voiced);
  CHECK_EQ_STR(r.note, "--");

  CHECK_EQ_INT(aud_tuner_window(NULL), 0);

  /* none of these should reach for a pointer they were not given */
  aud_tuner_push(NULL, NULL, 0);
  aud_tuner_push_pcm(NULL, NULL, 0, 0, AUD_FORMAT_S16_LE);
  aud_tuner_destroy(NULL);
  aud_tuner_describe(440.0, 440.0, NULL);
  aud_tuner_note_label(NULL, NULL, 0);
}

int main(void)
{
  RUN(describe_names_concert_pitch);
  RUN(describe_walks_the_octaves);
  RUN(describe_measures_the_offset);
  RUN(describe_follows_the_reference_pitch);
  RUN(describe_rejects_nonsense);
  RUN(note_frequency_round_trips);
  RUN(note_names_are_chromatic);
  RUN(note_label_joins_name_and_octave);
  RUN(create_rejects_a_bad_configuration);
  RUN(window_spans_the_lowest_note);
  RUN(detects_a_plain_tone);
  RUN(detects_across_the_range);
  RUN(the_default_range_reaches_the_instruments_it_claims);
  RUN(the_range_can_be_narrowed_and_widened);
  RUN(reads_a_detuned_string);
  RUN(detects_a_missing_fundamental);
  RUN(silence_is_not_a_note);
  RUN(a_reading_outlives_the_note);
  RUN(handles_being_handed_nothing);

  return TEST_RESULT();
}
