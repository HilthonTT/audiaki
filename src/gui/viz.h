/* SPDX-License-Identifier: MIT */
/*
 * viz.h - the live spectrum display.
 *
 * The analysis is not reimplemented here: this feeds the same aud_spectrum the
 * CLI meter and the offline video renderer use, and concerns itself only with
 * turning the band values into something worth looking at.
 *
 * Several styles share that one analysis. The default is a stem per band with
 * a glowing cap riding on top, after tsoding's musializer; the glow is a
 * single radial-gradient texture drawn additively, so overlapping halos sum
 * towards white and loud clusters bloom. Every style that draws a glow reuses
 * that same texture.
 */
#ifndef AUDIAKI_GUI_VIZ_H
#define AUDIAKI_GUI_VIZ_H

#include "raylib.h"

#include <stddef.h>

/*
 * Wide enough to read as a spectrum rather than a graphic equaliser, narrow
 * enough that each stem still gets a few pixels at a normal window width.
 */
#define AUD_VIZ_DEFAULT_BANDS 96u

/*
 * What a frame shows. The first three read the current instant; the waterfall
 * is the only one that shows history, which is what makes it the useful one
 * for spotting a hum or a dropout that has already happened.
 *
 * The tuner is the odd one out: it is a reading rather than a picture of the
 * sound, and it runs its own detector instead of the shared spectrum. It sits
 * with the styles anyway because it wants the same place on the screen and the
 * same key to reach it - tuning up is the thing you do immediately before you
 * press record, not a mode you go somewhere else for.
 */
typedef enum
{
  AUD_VIZ_MODE_BARS = 0,  /* stems with glowing caps, growing from the floor */
  AUD_VIZ_MODE_MIRROR,    /* the same bars, opening from the centre line */
  AUD_VIZ_MODE_RADIAL,    /* the spectrum wrapped into a ring */
  AUD_VIZ_MODE_SCOPE,     /* an oscilloscope trace of the last few ms */
  AUD_VIZ_MODE_WATERFALL, /* a scrolling spectrogram, newest at the right */
  AUD_VIZ_MODE_TUNER,     /* the note being played, and how far off it is */
  AUD_VIZ_MODE_COUNT
} aud_viz_mode;

/* Canonical lower-case name, e.g. "waterfall". "unknown" if unrecognised. */
const char *aud_viz_mode_name(aud_viz_mode mode);

/*
 * Parse a style name (case insensitive) into *out. Returns 0 on success, -1
 * when the name is not one of the styles, leaving *out untouched.
 */
int aud_viz_mode_from_name(const char *name, aud_viz_mode *out);

typedef struct aud_viz aud_viz;

/*
 * Create a display for audio at `rate` Hz with `bands` bars. `bands` is
 * clamped to what aud_spectrum accepts. Returns NULL on failure.
 */
aud_viz *aud_viz_create(unsigned rate, size_t bands);

void aud_viz_destroy(aud_viz *v);

/* Hand over freshly captured mono samples. */
void aud_viz_push(aud_viz *v, const float *mono, size_t frames);

/*
 * Advance the analysis by `dt` seconds. Called once per drawn frame, so the
 * bars decay in wall-clock time rather than per redraw, and the waterfall
 * scrolls by exactly one column.
 */
void aud_viz_update(aud_viz *v, float dt);

/* Draw into `area`, in whichever style is currently selected. */
void aud_viz_draw(const aud_viz *v, Rectangle area);

void aud_viz_set_mode(aud_viz *v, aud_viz_mode mode);
aud_viz_mode aud_viz_mode_get(const aud_viz *v);

/* Step to the next style, wrapping. Returns the one now selected. */
aud_viz_mode aud_viz_cycle_mode(aud_viz *v);

size_t aud_viz_bands(const aud_viz *v);

#endif /* AUDIAKI_GUI_VIZ_H */
