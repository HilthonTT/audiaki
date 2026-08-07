/* SPDX-License-Identifier: MIT */
/*
 * meter.h - single line level display drawn on stderr.
 *
 * Two shapes: a peak bar with a hold marker, or a row of spectrum bars. Either
 * way it is one line, redrawn in place, and it disables itself when stderr is
 * not a terminal so redirected output stays free of carriage returns.
 */
#ifndef AUDIAKI_METER_H
#define AUDIAKI_METER_H

#include "tuner.h"

#include <stddef.h>

typedef struct
{
  int enabled;
  int width;        /* bar width in characters */
  int line_dirty;   /* a meter line is currently on screen */
  double hold_peak; /* highest peak seen so far, normalised */
  int clipped;      /* a sample has hit full scale */
  int unicode;      /* the terminal can render block drawing characters */
  int armed;        /* capturing into the pre-roll, not into a file */
} aud_meter;

/*
 * Prepare a meter. It stays off when `want` is zero or stderr is not a tty.
 */
void meter_init(aud_meter *m, int want);

/*
 * How many spectrum bars fit on this terminal, given the readout that shares
 * the line. Returns 0 when the meter is disabled. Clamped to the range
 * aud_spectrum accepts.
 */
size_t meter_fit_bands(const aud_meter *m);

/*
 * Draw as a pre-roll wait rather than a take: the clock becomes how much audio
 * is being held and the xrun counter becomes the key that starts recording.
 * Both forms are the same width, so the line does not jump when a take begins.
 */
void meter_set_armed(aud_meter *m, int armed);

/*
 * Forget the peak hold and the clip flag. Called when a take begins after a
 * pre-roll wait: what the input did while you were setting the level is not
 * what the summary afterwards is reporting on.
 */
void meter_reset_peaks(aud_meter *m);

/* Redraw the meter in place. `peak` is normalised to [0.0, 1.0]. */
void meter_draw(aud_meter *m, double peak, double seconds, unsigned xruns);

/*
 * Redraw as spectrum bars instead. `bands` holds `n` values in [0.0, 1.0], low
 * frequency first; `peak` still feeds the clip and hold tracking so the
 * post-recording warnings do not depend on which display was chosen.
 */
void meter_draw_spectrum(aud_meter *m, const float *bands, size_t n, double peak,
                         double seconds, unsigned xruns);

/*
 * Redraw as a tuner instead: the note being played, and a needle showing how
 * far off it is. The scale runs from a semitone flat on the left to a semitone
 * sharp on the right, with the note itself in the middle.
 *
 * Unlike the other two this draws no clock and tracks no peak - tuning is not a
 * take, and nothing about it needs reporting afterwards.
 */
void meter_draw_tuner(aud_meter *m, const aud_tuner_reading *reading);

/* Erase the meter line so other output starts on a clean row. */
void meter_clear(aud_meter *m);

/* Non-zero if any sample reached full scale since meter_init(). */
int meter_clipped(const aud_meter *m);

#endif /* AUDIAKI_METER_H */
