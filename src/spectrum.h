/* SPDX-License-Identifier: MIT */
/*
 * spectrum.h - streaming spectrum analyser.
 *
 * Feed it audio as it arrives; ask it for a fixed number of bar heights in
 * [0.0, 1.0] whenever you want to draw. It keeps the last fft_size samples,
 * windows them, transforms them, folds the bins into log-spaced bands and
 * smooths the result over time so bars rise fast and fall slowly.
 *
 * Both consumers share this: the live terminal display in recorder.c and the
 * offline video renderer in visualize.c. No ALSA and no I/O, so it is unit
 * testable on its own.
 */
#ifndef AUDIAKI_SPECTRUM_H
#define AUDIAKI_SPECTRUM_H

#include "format.h"

#include <stddef.h>

#define AUD_SPECTRUM_MIN_BANDS 4u
#define AUD_SPECTRUM_MAX_BANDS 512u

typedef struct
{
  unsigned rate;   /* sample rate of the audio being pushed */
  size_t fft_size; /* window length; must be a power of two */
  size_t bands;    /* number of output bars */
  double min_hz;   /* centre of the lowest band */
  double max_hz;   /* centre of the highest band; clamped to rate / 2 */
  double floor_db; /* level that reads as an empty bar, e.g. -70.0 */
  double attack;   /* seconds for a bar to approach a rising target */
  double decay;    /* seconds for a bar to fall away from a peak */
} aud_spectrum_config;

typedef struct aud_spectrum aud_spectrum;

/*
 * Sensible defaults for a guitar or line input: a 2048 point window, 40 Hz to
 * 12 kHz, a -70 dBFS floor, and a fast attack with a slow decay.
 */
void aud_spectrum_config_defaults(aud_spectrum_config *cfg, unsigned rate, size_t bands);

/*
 * Allocate an analyser. Returns NULL on a bad configuration or when out of
 * memory, with errno set to EINVAL or ENOMEM.
 */
aud_spectrum *aud_spectrum_create(const aud_spectrum_config *cfg);

void aud_spectrum_destroy(aud_spectrum *s);

/* Number of bands the analyser was created with. */
size_t aud_spectrum_bands(const aud_spectrum *s);

/* Append mono samples in [-1.0, 1.0]. Samples older than the window are lost. */
void aud_spectrum_push(aud_spectrum *s, const float *mono, size_t frames);

/* Same, but decoding interleaved PCM in a capture format first. */
void aud_spectrum_push_pcm(aud_spectrum *s, const void *buf, size_t frames,
                           unsigned channels, aud_format fmt);

/*
 * Analyse the buffered window and advance the smoothing by `dt` seconds.
 * Returns an internal array of aud_spectrum_bands() values in [0.0, 1.0],
 * valid until the next call. Never NULL for a valid analyser.
 */
const float *aud_spectrum_analyse(aud_spectrum *s, double dt);

/* The band centre frequencies, for labelling an axis. */
const double *aud_spectrum_centres(const aud_spectrum *s);

#endif /* AUDIAKI_SPECTRUM_H */
