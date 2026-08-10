/* SPDX-License-Identifier: MIT */
/*
 * tuner.h - monophonic pitch detection, for tuning an instrument.
 *
 * Feed it audio as it arrives; ask it what note is being played. The answer is
 * a frequency, the nearest note to it, and how far off that note the frequency
 * is, in cents.
 *
 * Not the spectrum analyser with a peak finder bolted on. The strongest FFT bin
 * is often a harmonic rather than the fundamental - a plucked low E on a guitar
 * frequently has more energy at 165 Hz than at 82 Hz - and a tuner that reports
 * the octave above is worse than no tuner. This uses YIN instead, which works in
 * the time domain on the period of the wave rather than on where its energy sits,
 * and is not fooled by a weak fundamental.
 *
 * A 2048 point transform also puts its bins about 21 Hz apart at 44.1 kHz, which
 * near the bottom of a guitar's range is several semitones. Tuning needs cents.
 *
 * No ALSA and no I/O, so it can be built and unit tested anywhere. The terminal
 * tuner in cmd/tune.c and the desktop app's tuner display both run this.
 */
#ifndef AUDIAKI_TUNER_H
#define AUDIAKI_TUNER_H

#include "audio/format.h"

#include <stddef.h>

/* Concert pitch. Historic and orchestral tunings sit either side of it. */
#define AUD_TUNER_DEFAULT_A4 440.0
#define AUD_TUNER_A4_MIN 390.0
#define AUD_TUNER_A4_MAX 500.0

/*
 * How close counts as in tune. Below about five cents is inaudible on a plucked
 * string, and is well inside what the string will drift on its own as it warms.
 */
#define AUD_TUNER_IN_TUNE_CENTS 5.0

/* Longest name aud_tuner_note_label() writes, e.g. "A#2", plus the terminator. */
#define AUD_TUNER_LABEL_MAX 8u

typedef struct
{
  unsigned rate;    /* sample rate of the audio being pushed */
  double min_hz;    /* lowest pitch to look for; sets the analysis window */
  double max_hz;    /* highest pitch to look for */
  double threshold; /* YIN aperiodicity cutoff; lower is stricter */
  double gate_db;   /* below this RMS nothing is being played */
  double glide;     /* seconds to follow a pitch that is moving */
  double hold;      /* seconds a reading outlives the note that made it */
  double a4_hz;     /* what A above middle C is being called */
} aud_tuner_config;

typedef struct
{
  int voiced;        /* a pitch was found; every field below is meaningless without it */
  double frequency;  /* Hz, smoothed */
  double confidence; /* 0.0 to 1.0; how periodic the window was */
  double level_db;   /* RMS of the analysis window, in dBFS */
  int midi;          /* nearest MIDI note number, 69 = A4 */
  const char *note;  /* its name, e.g. "A#"; never NULL */
  int octave;        /* its octave in scientific pitch notation, A4 = 4 */
  double cents;      /* how far above (+) or below (-) that note, -50 to +50 */
  double target_hz;  /* what that note should be, at this a4_hz */
} aud_tuner_reading;

typedef struct aud_tuner aud_tuner;

/*
 * Sensible defaults: 30 Hz to 4.5 kHz, with concert pitch A4 = 440 Hz. That
 * runs from a five-string bass's low B (30.87 Hz) up past a piccolo's top C
 * (4186 Hz), so it covers the instruments people bring to a tuner rather than
 * only the fretted ones.
 *
 * The two ends do not cost the same. See aud_tuner_analyse(): raising `max_hz`
 * is nearly free, lowering `min_hz` is quadratic.
 */
void aud_tuner_config_defaults(aud_tuner_config *cfg, unsigned rate);

/*
 * Bounds on what a caller may ask for. The floor is a piano's A0, which is the
 * lowest note on a normal instrument; the ceiling is left to Nyquist, which
 * aud_tuner_create() checks against the actual rate.
 */
#define AUD_TUNER_MIN_HZ_FLOOR 20.0
#define AUD_TUNER_MAX_HZ_CEILING 12000.0

/*
 * Allocate a tuner. Returns NULL on a bad configuration or when out of memory,
 * with errno set to EINVAL or ENOMEM.
 */
aud_tuner *aud_tuner_create(const aud_tuner_config *cfg);

void aud_tuner_destroy(aud_tuner *t);

/* Append mono samples in [-1.0, 1.0]. Samples older than the window are lost. */
void aud_tuner_push(aud_tuner *t, const float *mono, size_t frames);

/* Same, but decoding interleaved PCM in a capture format first. */
void aud_tuner_push_pcm(aud_tuner *t, const void *buf, size_t frames, unsigned channels,
                        aud_format fmt);

/*
 * Analyse the buffered window and advance the smoothing by `dt` seconds. Fills
 * `out` and returns non-zero when a pitch is being reported.
 *
 * Costs roughly 2 * (rate / min_hz)^2 multiply-adds - a few million at 48 kHz,
 * which is cheap at the twenty or so analyses a second a tuner needs and much
 * too expensive to call once per drawn frame. Callers should rate limit
 * themselves and pass the time that actually elapsed as `dt`.
 *
 * Note the square: it is the *low* end of the range that costs. At 48 kHz one
 * call takes about 3.5 ms with min_hz at 40 and about 7.6 ms at 27.5, while
 * moving max_hz makes almost no difference at all. A caller that does not need
 * to reach the bottom of a bass should say so.
 */
int aud_tuner_analyse(aud_tuner *t, double dt, aud_tuner_reading *out);

/* Samples the analysis window spans, for a caller sizing its own buffer. */
size_t aud_tuner_window(const aud_tuner *t);

/* -- note arithmetic ------------------------------------------------------- */

/*
 * Describe `frequency` as a note. Pure: no analyser needed, which is what makes
 * the note mapping testable on its own.
 *
 * Leaves `out` unvoiced when the frequency is not a positive number inside the
 * MIDI range. Sharps rather than flats, because a chromatic tuner has no key to
 * decide between them.
 */
void aud_tuner_describe(double frequency, double a4_hz, aud_tuner_reading *out);

/* The name of a MIDI note without its octave, e.g. 70 -> "A#". */
const char *aud_tuner_note_name(int midi);

/* What MIDI note `midi` should be, at this a4_hz. 0.0 if out of range. */
double aud_tuner_note_frequency(int midi, double a4_hz);

/*
 * Write a reading's note and octave into `dst`, e.g. "A#2", or "--" when it is
 * unvoiced. `size` should be at least AUD_TUNER_LABEL_MAX.
 */
void aud_tuner_note_label(const aud_tuner_reading *reading, char *dst, size_t size);

#endif /* AUDIAKI_TUNER_H */
