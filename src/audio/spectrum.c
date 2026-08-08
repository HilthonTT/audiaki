/* SPDX-License-Identifier: MIT */
#include "audio/spectrum.h"

#include "audio/fft.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SPECTRUM_DEFAULT_FFT 2048u
#define SPECTRUM_DEFAULT_MIN_HZ 40.0
#define SPECTRUM_DEFAULT_MAX_HZ 12000.0
#define SPECTRUM_DEFAULT_FLOOR_DB (-70.0)
#define SPECTRUM_DEFAULT_ATTACK 0.02
#define SPECTRUM_DEFAULT_DECAY 0.25

struct aud_spectrum
{
  aud_spectrum_config cfg;
  float *history; /* fft_size samples, oldest first */
  float *window;  /* Hann coefficients */
  float *re;
  float *im;
  float *values;  /* smoothed output, cfg.bands entries */
  double *centre; /* band centre frequency in Hz */
  size_t *lo;     /* first FFT bin of each band */
  size_t *hi;     /* one past the last FFT bin of each band */
  float *scratch; /* decode staging for push_pcm() */
  double norm;    /* magnitude scale that maps a full scale sine to 1.0 */
};

void aud_spectrum_config_defaults(aud_spectrum_config *cfg, unsigned rate, size_t bands)
{
  if (cfg == NULL)
  {
    return;
  }

  memset(cfg, 0, sizeof(*cfg));
  cfg->rate = rate;
  cfg->fft_size = SPECTRUM_DEFAULT_FFT;
  cfg->bands = bands;
  cfg->min_hz = SPECTRUM_DEFAULT_MIN_HZ;
  cfg->max_hz = SPECTRUM_DEFAULT_MAX_HZ;
  cfg->floor_db = SPECTRUM_DEFAULT_FLOOR_DB;
  cfg->attack = SPECTRUM_DEFAULT_ATTACK;
  cfg->decay = SPECTRUM_DEFAULT_DECAY;
}

static int config_valid(const aud_spectrum_config *cfg)
{
  return cfg != NULL && cfg->rate >= 4000u && aud_fft_is_pow2(cfg->fft_size) &&
         cfg->bands >= AUD_SPECTRUM_MIN_BANDS && cfg->bands <= AUD_SPECTRUM_MAX_BANDS &&
         cfg->min_hz > 0.0 && cfg->max_hz > cfg->min_hz && cfg->floor_db < 0.0 &&
         cfg->attack >= 0.0 && cfg->decay >= 0.0;
}

/*
 * Split [min_hz, max_hz] into `bands` logarithmically spaced buckets and record
 * which FFT bins fall in each. Log spacing because pitch is logarithmic: linear
 * bands would spend most of the display on the octave above 6 kHz, where a
 * guitar has nothing but string noise.
 */
static void map_bands(aud_spectrum *s)
{
  const aud_spectrum_config *cfg = &s->cfg;
  size_t usable = cfg->fft_size / 2; /* bins above Nyquist mirror the rest */
  double nyquist = (double)cfg->rate / 2.0;
  double top = cfg->max_hz < nyquist ? cfg->max_hz : nyquist;
  double ratio;

  if (top <= cfg->min_hz)
  {
    top = cfg->min_hz * 2.0;
  }
  ratio = top / cfg->min_hz;

  for (size_t b = 0; b < cfg->bands; b++)
  {
    double t0 = (double)b / (double)cfg->bands;
    double t1 = (double)(b + 1) / (double)cfg->bands;
    double f0 = cfg->min_hz * pow(ratio, t0);
    double f1 = cfg->min_hz * pow(ratio, t1);
    double bin_hz = (double)cfg->rate / (double)cfg->fft_size;
    size_t lo = (size_t)(f0 / bin_hz);
    size_t hi = (size_t)(f1 / bin_hz);

    if (lo < 1)
    {
      lo = 1;
    } /* bin 0 is DC: a guitar interface's offset, not a note */
    if (hi <= lo)
    {
      hi = lo + 1;
    }
    if (lo > usable)
    {
      lo = usable;
    }
    if (hi > usable)
    {
      hi = usable;
    }
    if (hi <= lo)
    {
      lo = hi > 0 ? hi - 1 : 0;
    }

    s->lo[b] = lo;
    s->hi[b] = hi;
    s->centre[b] = sqrt(f0 * f1);
  }
}

aud_spectrum *aud_spectrum_create(const aud_spectrum_config *cfg)
{
  aud_spectrum *s;

  if (!config_valid(cfg))
  {
    errno = EINVAL;
    return NULL;
  }

  s = calloc(1, sizeof(*s));
  if (s == NULL)
  {
    errno = ENOMEM;
    return NULL;
  }
  s->cfg = *cfg;

  s->history = calloc(cfg->fft_size, sizeof(*s->history));
  s->window = calloc(cfg->fft_size, sizeof(*s->window));
  s->re = calloc(cfg->fft_size, sizeof(*s->re));
  s->im = calloc(cfg->fft_size, sizeof(*s->im));
  s->scratch = calloc(cfg->fft_size, sizeof(*s->scratch));
  s->values = calloc(cfg->bands, sizeof(*s->values));
  s->centre = calloc(cfg->bands, sizeof(*s->centre));
  s->lo = calloc(cfg->bands, sizeof(*s->lo));
  s->hi = calloc(cfg->bands, sizeof(*s->hi));

  if (s->history == NULL || s->window == NULL || s->re == NULL || s->im == NULL ||
      s->scratch == NULL || s->values == NULL || s->centre == NULL || s->lo == NULL ||
      s->hi == NULL)
  {
    aud_spectrum_destroy(s);
    errno = ENOMEM;
    return NULL;
  }

  aud_fft_hann(s->window, cfg->fft_size);
  map_bands(s);

  /*
   * A full scale sine concentrates into one bin with magnitude
   * amplitude * sum(window) / 2, and a Hann window sums to fft_size / 2.
   * So 4 / fft_size scales that peak to 1.0.
   */
  s->norm = 4.0 / (double)cfg->fft_size;

  return s;
}

void aud_spectrum_destroy(aud_spectrum *s)
{
  if (s == NULL)
  {
    return;
  }

  free(s->history);
  free(s->window);
  free(s->re);
  free(s->im);
  free(s->scratch);
  free(s->values);
  free(s->centre);
  free(s->lo);
  free(s->hi);
  free(s);
}

size_t aud_spectrum_bands(const aud_spectrum *s)
{
  return s != NULL ? s->cfg.bands : 0;
}

const double *aud_spectrum_centres(const aud_spectrum *s)
{
  return s != NULL ? s->centre : NULL;
}

void aud_spectrum_push(aud_spectrum *s, const float *mono, size_t frames)
{
  size_t n;

  if (s == NULL || mono == NULL || frames == 0)
  {
    return;
  }

  n = s->cfg.fft_size;

  if (frames >= n)
  {
    /* a single push longer than the window: only its tail can matter */
    memcpy(s->history, mono + (frames - n), n * sizeof(*s->history));
    return;
  }

  /*
   * Slide rather than wrap. The window is a couple of thousand floats and a
   * push happens once per capture period, so the memmove costs nothing and the
   * samples stay contiguous and in order for the transform.
   */
  memmove(s->history, s->history + frames, (n - frames) * sizeof(*s->history));
  memcpy(s->history + (n - frames), mono, frames * sizeof(*s->history));
}

void aud_spectrum_push_pcm(aud_spectrum *s, const void *buf, size_t frames,
                           unsigned channels, aud_format fmt)
{
  const unsigned char *p = (const unsigned char *)buf;
  unsigned bytes;
  size_t chunk;

  if (s == NULL || p == NULL || frames == 0 || channels == 0)
  {
    return;
  }

  bytes = aud_format_hw_bytes(fmt);
  if (bytes == 0)
  {
    return;
  }

  chunk = s->cfg.fft_size; /* scratch capacity */

  while (frames > 0)
  {
    size_t take = frames < chunk ? frames : chunk;

    aud_format_to_mono(s->scratch, p, take, channels, fmt);
    aud_spectrum_push(s, s->scratch, take);

    p += take * channels * bytes;
    frames -= take;
  }
}

/* Exponential approach: the fraction of the remaining distance to cover. */
static double smoothing_step(double dt, double tau)
{
  if (!(tau > 0.0) || !(dt > 0.0))
  {
    return 1.0;
  }
  return 1.0 - exp(-dt / tau);
}

const float *aud_spectrum_analyse(aud_spectrum *s, double dt)
{
  size_t n;
  double rise;
  double fall;

  if (s == NULL)
  {
    return NULL;
  }

  n = s->cfg.fft_size;

  for (size_t i = 0; i < n; i++)
  {
    s->re[i] = s->history[i] * s->window[i];
    s->im[i] = 0.0f;
  }

  aud_fft_forward(s->re, s->im, n);

  rise = smoothing_step(dt, s->cfg.attack);
  fall = smoothing_step(dt, s->cfg.decay);

  for (size_t b = 0; b < s->cfg.bands; b++)
  {
    double peak = 0.0;
    double db;
    double target;
    double step;

    /*
     * Peak rather than mean across the band. A mean drags a single strong
     * partial down towards the silence either side of it, which makes a clean
     * plucked note look weaker than a noisy chord.
     */
    for (size_t k = s->lo[b]; k < s->hi[b]; k++)
    {
      double mag = (double)aud_fft_magnitude(s->re[k], s->im[k]) * s->norm;
      if (mag > peak)
      {
        peak = mag;
      }
    }

    db = peak > 0.0 ? 20.0 * log10(peak) : s->cfg.floor_db;
    target = (db - s->cfg.floor_db) / -s->cfg.floor_db;
    if (target < 0.0)
    {
      target = 0.0;
    }
    if (target > 1.0)
    {
      target = 1.0;
    }

    step = target > (double)s->values[b] ? rise : fall;
    s->values[b] = (float)((double)s->values[b] + (target - (double)s->values[b]) * step);
  }

  return s->values;
}
