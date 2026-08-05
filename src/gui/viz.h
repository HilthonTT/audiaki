/* SPDX-License-Identifier: MIT */
/*
 * viz.h - the live spectrum display.
 *
 * The analysis is not reimplemented here: this feeds the same aud_spectrum the
 * CLI meter and the offline video renderer use, and concerns itself only with
 * turning the band values into something worth looking at.
 *
 * The look is a stem per band with a glowing cap riding on top, after
 * tsoding's musializer. The glow is a single radial-gradient texture drawn
 * additively - overlapping halos sum towards white, which is what gives the
 * bright cores where bars cluster, and it costs one textured quad per bar
 * rather than a shader pass.
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
 * bars decay in wall-clock time rather than per redraw.
 */
void aud_viz_update(aud_viz *v, float dt);

/* Draw into `area`. Bars grow upwards from its bottom edge. */
void aud_viz_draw(const aud_viz *v, Rectangle area);

/*
 * Draw the idle state: a flat row of dim stems, so the window still looks like
 * a spectrum analyser when nothing is playing rather than an empty black box.
 */
void aud_viz_draw_idle(const aud_viz *v, Rectangle area);

size_t aud_viz_bands(const aud_viz *v);

/* The colour a band is drawn in, for tinting the rest of the interface. */
Color aud_viz_band_color(const aud_viz *v, size_t band);

#endif /* AUDIAKI_GUI_VIZ_H */
