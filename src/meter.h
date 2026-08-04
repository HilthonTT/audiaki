/* SPDX-License-Identifier: MIT */
/*
 * meter.h - single line peak meter drawn on stderr.
 *
 * Disables itself when stderr is not a terminal, so redirected output stays
 * free of carriage returns.
 */
#ifndef AUDIAKI_METER_H
#define AUDIAKI_METER_H

typedef struct
{
  int enabled;
  int width;        /* bar width in characters */
  int line_dirty;   /* a meter line is currently on screen */
  double hold_peak; /* highest peak seen so far, normalised */
  int clipped;      /* a sample has hit full scale */
} aud_meter;

/*
 * Prepare a meter. It stays off when `want` is zero or stderr is not a tty.
 */
void meter_init(aud_meter *m, int want);

/* Redraw the meter in place. `peak` is normalised to [0.0, 1.0]. */
void meter_draw(aud_meter *m, double peak, double seconds, unsigned xruns);

/* Erase the meter line so other output starts on a clean row. */
void meter_clear(aud_meter *m);

/* Non-zero if any sample reached full scale since meter_init(). */
int meter_clipped(const aud_meter *m);

#endif /* AUDIAKI_METER_H */
