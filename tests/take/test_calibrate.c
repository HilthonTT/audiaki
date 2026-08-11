/* SPDX-License-Identifier: MIT */
/*
 * test_calibrate.c - the round trip measurement, against a loopback made of
 * arithmetic rather than of cable.
 *
 * The harness below is the whole point of take/calibrate.c carrying no audio
 * system: a run is fed its own output back, delayed by a number of frames the
 * test chose, and the answer either is that number or the test has found
 * something. Everything a real measurement can run into - silence, a return
 * that is not the burst, an output dropping frames, polarity the wrong way up,
 * one burst out of five landing somewhere absurd - is a different thing to feed
 * back, so all of it is reachable here without a device.
 */
#include "take/calibrate.h"

#include "take/latency.h"

#include "../test_util.h"

#include <stdlib.h>

#define RATE 16000u
#define PERIOD 128u
#define CHANNELS 2u

/*
 * The delay to measure, in frames. Comfortably more than a period, because the
 * harness builds a period's input out of frames the run has already emitted and
 * a delay shorter than a period would need frames from the period being built.
 * A real round trip is longer than a period too, for much the same reason.
 */
#define DELAY 500u

/* Everything a run has played, so it can be handed back to it later. */
typedef struct
{
  float *emitted;
  size_t frames;
  size_t capacity;
} loopback;

/* What a period of input is made of, on the way back round. */
typedef enum
{
  RETURN_LOOPBACK = 0, /* the burst, DELAY frames late */
  RETURN_SILENCE,      /* an unplugged input */
  RETURN_NOISE,        /* something arriving that is not the burst */
  RETURN_INVERTED,     /* the burst, upside down, as a balanced cable returns it */
} return_kind;

static void loopback_init(loopback *lb, size_t capacity)
{
  lb->emitted = calloc(capacity, sizeof(*lb->emitted));
  lb->frames = 0;
  lb->capacity = capacity;
}

static void loopback_free(loopback *lb)
{
  free(lb->emitted);
  lb->emitted = NULL;
}

/* One emitted frame, or silence for one that has not been played yet. */
static float emitted_at(const loopback *lb, size_t frame)
{
  return frame < lb->frames ? lb->emitted[frame] : 0.0f;
}

/*
 * A repeatable pseudo-random sample in [-0.5, 0.5). Not white noise with any
 * particular pedigree - just something loud, uncorrelated with a sweep, and the
 * same on every machine the tests run on.
 */
static float noise_at(size_t frame)
{
  unsigned long x = (unsigned long)frame * 1103515245ul + 12345ul;

  x ^= x >> 13;
  x *= 2654435761ul;
  return (float)((double)(x & 0xFFFFul) / 65536.0 - 0.5);
}

/*
 * What a run is put through: what comes back for most bursts, and one burst
 * singled out to go differently. Everything worth testing about a measurement
 * of several bursts is one of them behaving unlike the rest.
 */
typedef struct
{
  return_kind kind;
  int odd_burst; /* the burst treated differently, or -1 for none */
  return_kind odd_kind;
  unsigned odd_delay; /* extra frames that burst's return is late by */
  int drop_odd;       /* the output could not play that burst */
} scenario;

/* Which burst's stretch of the run `frame` falls in, counted as the run counts it. */
static unsigned burst_of(size_t frame)
{
  size_t lead = (size_t)(AUD_CALIBRATE_LEAD_IN * RATE);
  size_t spacing = (size_t)(AUD_LATENCY_MAX_MS * RATE / 1000.0) +
                   (size_t)(AUD_CALIBRATE_BURST_MS * RATE / 1000.0);

  return frame >= lead ? (unsigned)((frame - lead) / spacing) : 0u;
}

/* Run a calibration to the end, feeding it whatever `s` says it hears. */
static void run(aud_calibrate *cal, unsigned repeats, const scenario *s,
                unsigned long *dropped_out)
{
  loopback lb;
  float captured[PERIOD * CHANNELS];
  float playback[PERIOD * CHANNELS];
  unsigned long dropped = 0;
  size_t frame = 0;
  size_t limit = (size_t)RATE * 8u * repeats; /* far past any run's length */

  loopback_init(&lb, limit + PERIOD);

  while (frame < limit)
  {
    int odd = s->odd_burst >= 0 && burst_of(frame) == (unsigned)s->odd_burst;
    return_kind now = odd ? s->odd_kind : s->kind;
    size_t late = DELAY + (odd ? s->odd_delay : 0u);
    size_t i;
    int done;

    for (i = 0; i < PERIOD; i++)
    {
      size_t at = frame + i;
      float sample = 0.0f;
      unsigned ch;

      switch (now)
      {
      case RETURN_LOOPBACK:
        sample = at >= late ? emitted_at(&lb, at - late) : 0.0f;
        break;
      case RETURN_INVERTED:
        sample = at >= late ? -emitted_at(&lb, at - late) : 0.0f;
        break;
      case RETURN_NOISE:
        sample = noise_at(at);
        break;
      case RETURN_SILENCE:
      default:
        break;
      }

      for (ch = 0; ch < CHANNELS; ch++)
      {
        captured[i * CHANNELS + ch] = sample;
      }
    }

    done = aud_calibrate_step(cal, captured, CHANNELS, playback, CHANNELS, PERIOD);

    for (i = 0; i < PERIOD; i++)
    {
      lb.emitted[lb.frames++] = playback[i * CHANNELS];
    }

    /*
     * An output that could not fit what it was handed. Counted only while the
     * singled-out burst is actually going out, which is the only moment a drop
     * changes an answer.
     */
    if (s->drop_odd && odd && playback[0] != 0.0f)
    {
      dropped += PERIOD;
      if (dropped_out != NULL)
      {
        *dropped_out = dropped;
      }
    }
    aud_calibrate_note_dropped(cal, dropped);

    if (done)
    {
      break;
    }
    frame += PERIOD;
  }

  loopback_free(&lb);
}

/* The ordinary case: a cable from the output to the input, and nothing else. */
static aud_calibrate *measure(unsigned repeats, return_kind kind,
                              aud_calibrate_result *result)
{
  aud_calibrate_config cfg;
  aud_calibrate *cal;
  scenario s;

  aud_calibrate_config_defaults(&cfg, RATE);
  cfg.repeats = repeats;

  cal = aud_calibrate_create(&cfg);
  if (cal == NULL)
  {
    return NULL;
  }

  memset(&s, 0, sizeof(s));
  s.kind = kind;
  s.odd_burst = -1;

  run(cal, repeats, &s, NULL);
  aud_calibrate_analyse(cal, result);
  return cal;
}

TEST(create_rejects_nonsense)
{
  aud_calibrate_config cfg;

  aud_calibrate_config_defaults(&cfg, RATE);
  CHECK(aud_calibrate_create(NULL) == NULL);

  cfg.rate = 0;
  CHECK(aud_calibrate_create(&cfg) == NULL);

  /* a rate this low cannot carry a sweep, never mind be searched for one */
  cfg.rate = 20;
  CHECK(aud_calibrate_create(&cfg) == NULL);

  aud_calibrate_config_defaults(&cfg, RATE);
  cfg.repeats = 0;
  CHECK(aud_calibrate_create(&cfg) == NULL);

  cfg.repeats = AUD_CALIBRATE_MAX_REPEATS + 1u;
  CHECK(aud_calibrate_create(&cfg) == NULL);
}

TEST(defaults_are_the_documented_ones)
{
  aud_calibrate_config cfg;

  aud_calibrate_config_defaults(&cfg, RATE);
  CHECK_EQ_INT(cfg.rate, RATE);
  CHECK_EQ_INT(cfg.repeats, AUD_CALIBRATE_DEFAULT_REPEATS);
  CHECK_EQ_DBL(cfg.gain, AUD_CALIBRATE_GAIN, 1e-6);
}

TEST(a_loopback_measures_its_own_delay)
{
  aud_calibrate_result result;
  aud_calibrate *cal = measure(3, RETURN_LOOPBACK, &result);

  CHECK(cal != NULL);
  CHECK_EQ_INT(result.verdict, AUD_CALIBRATE_OK);
  CHECK_EQ_INT(result.frames, DELAY);
  CHECK_EQ_DBL(result.ms, 1000.0 * DELAY / RATE, 0.05);
  CHECK_EQ_INT(result.taken, 3);
  CHECK_EQ_INT(result.fired, 3);

  /* every burst went down the same cable, so they cannot disagree */
  CHECK_EQ_DBL(result.spread_ms, 0.0, 1e-9);
  CHECK(result.match > 0.9);

  aud_calibrate_destroy(cal);
}

TEST(polarity_the_wrong_way_up_is_still_the_burst)
{
  aud_calibrate_result result;
  aud_calibrate *cal = measure(2, RETURN_INVERTED, &result);

  CHECK(cal != NULL);
  CHECK_EQ_INT(result.verdict, AUD_CALIBRATE_OK);
  CHECK_EQ_INT(result.frames, DELAY);
  CHECK(result.match > 0.9);

  aud_calibrate_destroy(cal);
}

TEST(an_unplugged_input_says_so)
{
  aud_calibrate_result result;
  aud_calibrate *cal = measure(2, RETURN_SILENCE, &result);

  CHECK(cal != NULL);
  CHECK_EQ_INT(result.verdict, AUD_CALIBRATE_SILENT);
  CHECK_EQ_INT(result.taken, 0);
  CHECK_EQ_INT(result.fired, 2);

  aud_calibrate_destroy(cal);
}

TEST(noise_is_not_mistaken_for_the_burst)
{
  aud_calibrate_result result;
  aud_calibrate *cal = measure(2, RETURN_NOISE, &result);

  CHECK(cal != NULL);
  CHECK_EQ_INT(result.verdict, AUD_CALIBRATE_UNRECOGNISED);
  CHECK_EQ_INT(result.taken, 0);

  /* it was not silent, and the report says as much rather than blaming a cable */
  CHECK(result.peak_dbfs > AUD_CALIBRATE_SILENCE_DBFS);

  aud_calibrate_destroy(cal);
}

TEST(one_bad_burst_does_not_move_the_answer)
{
  aud_calibrate_config cfg;
  aud_calibrate *cal;
  aud_calibrate_result result;
  scenario s;

  aud_calibrate_config_defaults(&cfg, RATE);
  cfg.repeats = 3;
  cal = aud_calibrate_create(&cfg);
  CHECK(cal != NULL);

  /* something else made a noise over the second burst, and only that one */
  memset(&s, 0, sizeof(s));
  s.kind = RETURN_LOOPBACK;
  s.odd_burst = 1;
  s.odd_kind = RETURN_NOISE;

  run(cal, 3, &s, NULL);
  aud_calibrate_analyse(cal, &result);

  CHECK_EQ_INT(result.verdict, AUD_CALIBRATE_OK);
  CHECK_EQ_INT(result.frames, DELAY);
  CHECK_EQ_INT(result.taken, 2);
  CHECK_EQ_INT(result.fired, 3);

  aud_calibrate_destroy(cal);
}

/*
 * A reading a long way from the others is thrown out rather than averaged in,
 * and when there are not enough left to be a majority the run says it could not
 * tell rather than picking one of them.
 */
TEST(readings_that_disagree_are_not_averaged_into_a_wrong_answer)
{
  aud_calibrate_config cfg;
  aud_calibrate *cal;
  aud_calibrate_result result;
  scenario s;

  aud_calibrate_config_defaults(&cfg, RATE);
  cfg.repeats = 2;
  cal = aud_calibrate_create(&cfg);
  CHECK(cal != NULL);

  memset(&s, 0, sizeof(s));
  s.kind = RETURN_LOOPBACK;
  s.odd_burst = 1;
  s.odd_kind = RETURN_LOOPBACK;
  s.odd_delay = RATE / 10u; /* a tenth of a second further off, which is absurd */

  run(cal, 2, &s, NULL);
  aud_calibrate_analyse(cal, &result);

  CHECK_EQ_INT(result.verdict, AUD_CALIBRATE_UNSTEADY);
  /* and it does not offer the average of the two, which is neither of them */
  CHECK_EQ_DBL(result.ms, 0.0, 1e-9);

  aud_calibrate_destroy(cal);

  /* three of them, one absurd: the two that agree still carry it */
  cfg.repeats = 3;
  cal = aud_calibrate_create(&cfg);
  CHECK(cal != NULL);

  run(cal, 3, &s, NULL);
  aud_calibrate_analyse(cal, &result);

  CHECK_EQ_INT(result.verdict, AUD_CALIBRATE_OK);
  CHECK_EQ_INT(result.frames, DELAY);
  CHECK_EQ_INT(result.taken, 2);

  aud_calibrate_destroy(cal);
}

TEST(an_output_that_dropped_the_bursts_says_so)
{
  aud_calibrate_config cfg;
  aud_calibrate *cal;
  aud_calibrate_result result;
  unsigned long dropped = 0;
  scenario s;

  aud_calibrate_config_defaults(&cfg, RATE);
  cfg.repeats = 1;
  cal = aud_calibrate_create(&cfg);
  CHECK(cal != NULL);

  /* the only burst was dropped on the way out, and nothing came back with it */
  memset(&s, 0, sizeof(s));
  s.kind = RETURN_SILENCE;
  s.odd_burst = 0;
  s.odd_kind = RETURN_SILENCE;
  s.drop_odd = 1;

  run(cal, 1, &s, &dropped);
  aud_calibrate_analyse(cal, &result);

  CHECK(dropped > 0);
  CHECK_EQ_INT(result.verdict, AUD_CALIBRATE_DROPPED);

  aud_calibrate_destroy(cal);
}

TEST(a_run_that_never_started_is_not_a_failed_one)
{
  aud_calibrate_config cfg;
  aud_calibrate *cal;
  aud_calibrate_result result;
  float captured[PERIOD * CHANNELS] = {0};
  float playback[PERIOD * CHANNELS];

  aud_calibrate_config_defaults(&cfg, RATE);
  cfg.repeats = 2;
  cal = aud_calibrate_create(&cfg);
  CHECK(cal != NULL);

  /* one period in, which is inside the lead-in and before any burst */
  CHECK(!aud_calibrate_step(cal, captured, CHANNELS, playback, CHANNELS, PERIOD));
  CHECK_EQ_INT(aud_calibrate_fired(cal), 0);
  CHECK(!aud_calibrate_finished(cal));

  aud_calibrate_analyse(cal, &result);
  CHECK_EQ_INT(result.verdict, AUD_CALIBRATE_SHORT);

  aud_calibrate_destroy(cal);
}

TEST(a_burst_is_not_readable_until_its_window_closes)
{
  aud_calibrate_config cfg;
  aud_calibrate *cal;
  aud_calibrate_result result;
  float captured[PERIOD * CHANNELS] = {0};
  float playback[PERIOD * CHANNELS];
  size_t periods = 0;

  aud_calibrate_config_defaults(&cfg, RATE);
  cfg.repeats = 1;
  cal = aud_calibrate_create(&cfg);
  CHECK(cal != NULL);

  CHECK(!aud_calibrate_ready(cal, 0));
  CHECK(!aud_calibrate_ready(cal, 1)); /* there is no burst 1 */

  while (!aud_calibrate_step(cal, captured, CHANNELS, playback, CHANNELS, PERIOD))
  {
    periods++;
    CHECK(periods < 100000); /* a run that never finishes is the bug being caught */
  }

  CHECK(aud_calibrate_finished(cal));
  CHECK(aud_calibrate_ready(cal, 0));
  CHECK_EQ_INT(aud_calibrate_fired(cal), 1);

  /*
   * Polling for a reading while the window was still filling must not have
   * fixed the answer: the run heard silence, and that is what it should say.
   */
  aud_calibrate_analyse(cal, &result);
  CHECK_EQ_INT(result.verdict, AUD_CALIBRATE_SILENT);

  aud_calibrate_destroy(cal);
}

TEST(readings_are_reported_one_at_a_time)
{
  aud_calibrate_config cfg;
  aud_calibrate *cal;
  double ms = 0.0;
  double match = 0.0;
  scenario s;

  aud_calibrate_config_defaults(&cfg, RATE);
  cfg.repeats = 2;
  cal = aud_calibrate_create(&cfg);
  CHECK(cal != NULL);

  memset(&s, 0, sizeof(s));
  s.kind = RETURN_LOOPBACK;
  s.odd_burst = -1;

  run(cal, 2, &s, NULL);

  CHECK_EQ_INT(aud_calibrate_reading(cal, 0, &ms, &match), 0);
  CHECK_EQ_DBL(ms, 1000.0 * DELAY / RATE, 0.05);
  CHECK(match > 0.9);

  CHECK_EQ_INT(aud_calibrate_reading(cal, 1, &ms, &match), 0);
  CHECK_EQ_DBL(ms, 1000.0 * DELAY / RATE, 0.05);

  /* asking about a burst that does not exist is not a reading */
  CHECK_EQ_INT(aud_calibrate_reading(cal, 2, &ms, &match), -1);
  CHECK_EQ_INT(aud_calibrate_reading(NULL, 0, &ms, &match), -1);

  aud_calibrate_destroy(cal);
}

TEST(every_verdict_says_something)
{
  CHECK(aud_calibrate_verdict_text(AUD_CALIBRATE_OK) != NULL);
  CHECK(aud_calibrate_verdict_text(AUD_CALIBRATE_SILENT) != NULL);
  CHECK(aud_calibrate_verdict_text(AUD_CALIBRATE_UNRECOGNISED) != NULL);
  CHECK(aud_calibrate_verdict_text(AUD_CALIBRATE_UNSTEADY) != NULL);
  CHECK(aud_calibrate_verdict_text(AUD_CALIBRATE_DROPPED) != NULL);
  CHECK(aud_calibrate_verdict_text(AUD_CALIBRATE_SHORT) != NULL);
  CHECK(aud_calibrate_verdict_text((aud_calibrate_verdict)99) != NULL);
}

TEST(destroy_takes_null)
{
  aud_calibrate_destroy(NULL);
  aud_calibrate_config_defaults(NULL, RATE);
  CHECK(aud_calibrate_fired(NULL) == 0);
  CHECK(aud_calibrate_finished(NULL));
  CHECK(!aud_calibrate_ready(NULL, 0));
}

int main(void)
{
  RUN(create_rejects_nonsense);
  RUN(defaults_are_the_documented_ones);
  RUN(a_loopback_measures_its_own_delay);
  RUN(polarity_the_wrong_way_up_is_still_the_burst);
  RUN(an_unplugged_input_says_so);
  RUN(noise_is_not_mistaken_for_the_burst);
  RUN(one_bad_burst_does_not_move_the_answer);
  RUN(readings_that_disagree_are_not_averaged_into_a_wrong_answer);
  RUN(an_output_that_dropped_the_bursts_says_so);
  RUN(a_run_that_never_started_is_not_a_failed_one);
  RUN(a_burst_is_not_readable_until_its_window_closes);
  RUN(readings_are_reported_one_at_a_time);
  RUN(every_verdict_says_something);
  RUN(destroy_takes_null);
  return TEST_RESULT();
}
