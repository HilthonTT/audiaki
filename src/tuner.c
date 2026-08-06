/* SPDX-License-Identifier: MIT */
#include "tuner.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TUNER_DEFAULT_MIN_HZ 40.0
#define TUNER_DEFAULT_MAX_HZ 2000.0
#define TUNER_DEFAULT_THRESHOLD 0.20
#define TUNER_DEFAULT_GATE_DB (-52.0)
#define TUNER_DEFAULT_GLIDE 0.06
#define TUNER_DEFAULT_HOLD 0.7

/*
 * The longest period the difference function will search for. The analysis
 * costs two of these squared per call, so this is what stops a high sample rate
 * from turning a tuner into a load average. A device running at 192 kHz gets
 * its low end raised to about 47 Hz rather than an error: a tuner that will not
 * start is worse than one that does not reach the bottom string of a bass.
 */
#define TUNER_MAX_TAU 4096u

/*
 * A jump wider than this is a different string, not the same note drifting, so
 * the display should follow it at once instead of sliding across. Just over a
 * semitone: bending into a note has to still read as one note moving.
 */
#define TUNER_SNAP_CENTS 130.0

struct aud_tuner
{
  aud_tuner_config cfg;

  float *history; /* `window` samples, oldest first */
  float *scratch; /* decode staging for push_pcm() */
  double *diff;   /* the squared difference function, tau_max + 1 entries */
  double *cmnd;   /* the same, cumulative mean normalised */

  size_t window;
  size_t integration; /* samples each lag is compared over */
  size_t tau_min;
  size_t tau_max;
  size_t offset; /* where the analysis span starts; see analysis_window() */

  double smoothed_hz;  /* what is actually reported; 0 when nothing is */
  double since_voiced; /* seconds since the last detection */
  double confidence;
};

/* -- note arithmetic ------------------------------------------------------- */

/*
 * Sharps rather than flats. A chromatic tuner has no key signature to tell it
 * whether the note between A and B is A# or Bb, and picking one and staying
 * with it beats guessing.
 */
static const char *const note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

const char *aud_tuner_note_name(int midi)
{
  if (midi < 0 || midi > 127)
    return "--";

  return note_names[midi % 12];
}

double aud_tuner_note_frequency(int midi, double a4_hz)
{
  if (midi < 0 || midi > 127 || !(a4_hz > 0.0))
    return 0.0;

  return a4_hz * pow(2.0, ((double)midi - 69.0) / 12.0);
}

void aud_tuner_describe(double frequency, double a4_hz, aud_tuner_reading *out)
{
  double midi;
  int nearest;

  if (out == NULL)
    return;

  memset(out, 0, sizeof(*out));
  out->note = "--";
  out->level_db = AUD_DBFS_FLOOR;

  if (!(frequency > 0.0) || !(a4_hz > 0.0))
    return;

  /* MIDI note numbers are twelve to the octave, which makes this exact */
  midi = 69.0 + 12.0 * log2(frequency / a4_hz);
  if (!(midi >= 0.0) || midi > 127.0)
    return;

  nearest = (int)floor(midi + 0.5);
  if (nearest > 127)
    nearest = 127;

  out->voiced = 1;
  out->frequency = frequency;
  out->midi = nearest;
  out->note = aud_tuner_note_name(nearest);
  /* scientific pitch notation: MIDI 60 is C4, so the octave starts at -1 */
  out->octave = nearest / 12 - 1;
  out->cents = (midi - (double)nearest) * 100.0;
  out->target_hz = aud_tuner_note_frequency(nearest, a4_hz);
}

void aud_tuner_note_label(const aud_tuner_reading *reading, char *dst, size_t size)
{
  if (dst == NULL || size == 0)
    return;

  if (reading == NULL || !reading->voiced)
  {
    snprintf(dst, size, "--");
    return;
  }

  snprintf(dst, size, "%s%d", reading->note, reading->octave);
}

/* -- setup ----------------------------------------------------------------- */

void aud_tuner_config_defaults(aud_tuner_config *cfg, unsigned rate)
{
  if (cfg == NULL)
    return;

  memset(cfg, 0, sizeof(*cfg));
  cfg->rate = rate;
  cfg->min_hz = TUNER_DEFAULT_MIN_HZ;
  /*
   * Nothing above Nyquist can be found, so the defaults have to bend to a very
   * low rate rather than hand back a configuration that aud_tuner_create()
   * would then refuse. An explicit max_hz past Nyquist is still an error: that
   * one is a caller asking for something impossible.
   */
  cfg->max_hz = TUNER_DEFAULT_MAX_HZ;
  if (cfg->max_hz > (double)rate * 0.45)
    cfg->max_hz = (double)rate * 0.45;
  cfg->threshold = TUNER_DEFAULT_THRESHOLD;
  cfg->gate_db = TUNER_DEFAULT_GATE_DB;
  cfg->glide = TUNER_DEFAULT_GLIDE;
  cfg->hold = TUNER_DEFAULT_HOLD;
  cfg->a4_hz = AUD_TUNER_DEFAULT_A4;
}

static int config_valid(const aud_tuner_config *cfg)
{
  return cfg != NULL && cfg->rate >= 4000u && cfg->min_hz > 0.0 &&
         cfg->max_hz > cfg->min_hz * 2.0 && cfg->max_hz < (double)cfg->rate / 2.0 &&
         cfg->threshold > 0.0 && cfg->threshold < 1.0 && cfg->gate_db < 0.0 &&
         cfg->glide >= 0.0 && cfg->hold >= 0.0 && cfg->a4_hz >= AUD_TUNER_A4_MIN &&
         cfg->a4_hz <= AUD_TUNER_A4_MAX;
}

aud_tuner *aud_tuner_create(const aud_tuner_config *cfg)
{
  aud_tuner *t;
  size_t need;

  if (!config_valid(cfg))
  {
    errno = EINVAL;
    return NULL;
  }

  t = calloc(1, sizeof(*t));
  if (t == NULL)
  {
    errno = ENOMEM;
    return NULL;
  }
  t->cfg = *cfg;

  t->tau_max = (size_t)((double)cfg->rate / cfg->min_hz);
  if (t->tau_max > TUNER_MAX_TAU)
    t->tau_max = TUNER_MAX_TAU; /* raises the low end rather than failing */

  t->tau_min = (size_t)((double)cfg->rate / cfg->max_hz);
  if (t->tau_min < 2)
    t->tau_min = 2;

  if (t->tau_min + 1 >= t->tau_max)
  {
    free(t);
    errno = EINVAL;
    return NULL;
  }

  /*
   * Two periods of the lowest note to compare, plus the lag itself to slide it
   * over: three times the longest period, rounded up to a power of two so the
   * buffer is a comfortable size rather than for any transform's sake.
   */
  t->integration = t->tau_max * 2u;
  need = t->integration + t->tau_max;
  for (t->window = 1024u; t->window < need; t->window *= 2u)
    ;

  /*
   * The rounding above leaves slack, and it has to sit at the old end of the
   * buffer rather than the new one. Analysing from index zero would leave the
   * newest window - need samples - nearly 18 ms at 44.1 kHz - out of every
   * reading, so both the pitch and the gate would describe a moment that has
   * already passed.
   */
  t->offset = t->window - need;

  t->history = calloc(t->window, sizeof(*t->history));
  t->scratch = calloc(t->window, sizeof(*t->scratch));
  t->diff = calloc(t->tau_max + 1u, sizeof(*t->diff));
  t->cmnd = calloc(t->tau_max + 1u, sizeof(*t->cmnd));

  if (t->history == NULL || t->scratch == NULL || t->diff == NULL || t->cmnd == NULL)
  {
    aud_tuner_destroy(t);
    errno = ENOMEM;
    return NULL;
  }

  return t;
}

void aud_tuner_destroy(aud_tuner *t)
{
  if (t == NULL)
    return;

  free(t->history);
  free(t->scratch);
  free(t->diff);
  free(t->cmnd);
  free(t);
}

size_t aud_tuner_window(const aud_tuner *t)
{
  return t != NULL ? t->window : 0;
}

/* -- input ----------------------------------------------------------------- */

void aud_tuner_push(aud_tuner *t, const float *mono, size_t frames)
{
  size_t n;

  if (t == NULL || mono == NULL || frames == 0)
    return;

  n = t->window;

  if (frames >= n)
  {
    memcpy(t->history, mono + (frames - n), n * sizeof(*t->history));
    return;
  }

  /* slide rather than wrap, so the difference function can index it plainly */
  memmove(t->history, t->history + frames, (n - frames) * sizeof(*t->history));
  memcpy(t->history + (n - frames), mono, frames * sizeof(*t->history));
}

void aud_tuner_push_pcm(aud_tuner *t, const void *buf, size_t frames, unsigned channels,
                        aud_format fmt)
{
  const unsigned char *p = (const unsigned char *)buf;
  unsigned bytes;

  if (t == NULL || p == NULL || frames == 0 || channels == 0)
    return;

  bytes = aud_format_hw_bytes(fmt);
  if (bytes == 0)
    return;

  while (frames > 0)
  {
    size_t take = frames < t->window ? frames : t->window;

    aud_format_to_mono(t->scratch, p, take, channels, fmt);
    aud_tuner_push(t, t->scratch, take);

    p += take * channels * bytes;
    frames -= take;
  }
}

/* -- YIN ------------------------------------------------------------------- */

/*
 * The newest integration + tau_max samples, which is the span every step below
 * works on. Not t->history itself: the buffer is longer than the analysis needs
 * and the newest samples live at its end.
 */
static const float *analysis_window(const aud_tuner *t)
{
  return t->history + t->offset;
}

/*
 * Step 2 of the YIN paper: how different the window is from itself `tau`
 * samples later. A periodic signal is least different from itself one period
 * on, so the period shows up as a dip - and unlike a spectrum peak, that dip
 * lands on the fundamental whether or not the fundamental is the loud part.
 */
static void difference(aud_tuner *t)
{
  const float *x = analysis_window(t);

  t->diff[0] = 0.0;

  for (size_t tau = 1; tau <= t->tau_max; tau++)
  {
    double sum = 0.0;

    for (size_t j = 0; j < t->integration; j++)
    {
      double d = (double)x[j] - (double)x[j + tau];

      sum += d * d;
    }
    t->diff[tau] = sum;
  }
}

/*
 * Step 3: divide each lag by the mean of everything shorter than it. Without
 * this, tau = 0 is always the deepest dip and a quiet signal always looks more
 * periodic than a loud one; with it the values are comparable across lags and
 * against a fixed threshold.
 */
static void cumulative_mean(aud_tuner *t)
{
  double running = 0.0;

  t->cmnd[0] = 1.0;

  for (size_t tau = 1; tau <= t->tau_max; tau++)
  {
    running += t->diff[tau];
    t->cmnd[tau] = running > 0.0 ? t->diff[tau] * (double)tau / running : 1.0;
  }
}

/*
 * Step 4: the first lag that dips below the threshold, not the deepest one.
 * The deepest is frequently a multiple of the period, which would report the
 * note an octave or two down - the mirror of the error a spectrum peak makes.
 *
 * Returns 0 when nothing was periodic enough.
 */
static size_t first_dip(const aud_tuner *t)
{
  for (size_t tau = t->tau_min; tau <= t->tau_max; tau++)
  {
    if (t->cmnd[tau] >= t->cfg.threshold)
      continue;

    /* the crossing is on the way into the dip; walk down to its bottom */
    while (tau + 1u <= t->tau_max && t->cmnd[tau + 1u] < t->cmnd[tau])
      tau++;

    return tau;
  }

  return 0;
}

/*
 * Step 5: fit a parabola through the dip and its neighbours to find where the
 * true minimum lies between two whole samples. At 44.1 kHz a guitar's top E is
 * only 67 samples long, so one sample of lag is 26 cents - a tuner reading in
 * whole samples would be useless without this.
 */
static double refine(const aud_tuner *t, size_t tau)
{
  double a;
  double delta;

  if (tau < 1u || tau + 1u > t->tau_max)
    return (double)tau;

  a = t->cmnd[tau - 1u] - 2.0 * t->cmnd[tau] + t->cmnd[tau + 1u];
  if (!(a > 0.0))
    return (double)tau;

  delta = (t->cmnd[tau - 1u] - t->cmnd[tau + 1u]) / (2.0 * a);
  if (delta < -1.0)
    delta = -1.0;
  if (delta > 1.0)
    delta = 1.0;

  return (double)tau + delta;
}

/* RMS of the analysis window, in dBFS. */
static double window_level_db(const aud_tuner *t)
{
  const float *x = analysis_window(t);
  double sum = 0.0;

  for (size_t i = 0; i < t->integration; i++)
    sum += (double)x[i] * (double)x[i];

  return aud_format_dbfs(sqrt(sum / (double)t->integration));
}

/* -- analysis -------------------------------------------------------------- */

/* Exponential approach: the fraction of the remaining distance to cover. */
static double smoothing_step(double dt, double tau)
{
  if (!(tau > 0.0) || !(dt > 0.0))
    return 1.0;

  return 1.0 - exp(-dt / tau);
}

/*
 * Move the reported pitch towards a new detection. Interpolating in cents
 * rather than in Hz so the needle moves at the same speed wherever it is: a
 * semitone is 5 Hz at the bottom of a bass and 100 Hz at the top of a guitar,
 * and a tuner that crawls on one and snaps on the other reads as broken.
 */
static void glide_towards(aud_tuner *t, double detected, double dt)
{
  double cents;

  if (!(t->smoothed_hz > 0.0))
  {
    t->smoothed_hz = detected;
    return;
  }

  cents = 1200.0 * log2(detected / t->smoothed_hz);
  if (fabs(cents) > TUNER_SNAP_CENTS)
  {
    t->smoothed_hz = detected;
    return;
  }

  t->smoothed_hz *= pow(2.0, (cents * smoothing_step(dt, t->cfg.glide)) / 1200.0);
}

int aud_tuner_analyse(aud_tuner *t, double dt, aud_tuner_reading *out)
{
  aud_tuner_reading scratch;
  double level_db;
  double detected = 0.0;
  size_t tau;

  if (out == NULL)
    out = &scratch;

  aud_tuner_describe(0.0, 0.0, out); /* unvoiced, with every field cleared */

  if (t == NULL)
    return 0;

  if (dt < 0.0)
    dt = 0.0;

  level_db = window_level_db(t);

  /*
   * The gate comes first because the whole difference function is skipped when
   * nothing is being played, which is most of the time a tuner is on screen.
   */
  if (level_db > t->cfg.gate_db)
  {
    difference(t);
    cumulative_mean(t);

    tau = first_dip(t);
    if (tau != 0)
    {
      double period = refine(t, tau);
      double hz = period > 0.0 ? (double)t->cfg.rate / period : 0.0;

      if (hz >= t->cfg.min_hz && hz <= t->cfg.max_hz)
      {
        detected = hz;
        t->confidence = 1.0 - t->cmnd[tau];
        if (t->confidence < 0.0)
          t->confidence = 0.0;
        if (t->confidence > 1.0)
          t->confidence = 1.0;
      }
    }
  }

  if (detected > 0.0)
  {
    glide_towards(t, detected, dt);
    t->since_voiced = 0.0;
  }
  else
  {
    t->since_voiced += dt;

    /*
     * A plucked note decays through the gate long before it stops being the
     * note you are tuning. Holding the last reading for a moment stops the
     * display blinking out between strums; past that, silence means silence.
     */
    if (t->since_voiced > t->cfg.hold)
    {
      t->smoothed_hz = 0.0;
      t->confidence = 0.0;
    }
  }

  if (!(t->smoothed_hz > 0.0))
  {
    out->level_db = level_db;
    return 0;
  }

  aud_tuner_describe(t->smoothed_hz, t->cfg.a4_hz, out);
  out->confidence = t->confidence;
  out->level_db = level_db;

  return out->voiced;
}
