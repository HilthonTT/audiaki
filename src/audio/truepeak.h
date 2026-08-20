/* SPDX-License-Identifier: MIT */
/*
 * truepeak.h - looking between the samples.
 *
 * A sample peak only says where the samples landed. The waveform between two of
 * them routinely goes higher, and whatever reconstructs it - a converter, or a
 * lossy encoder - clips there while the sample peak still reports headroom to
 * spare. BS.1770-4 Annex 2 answers that by oversampling four times through a
 * windowed sinc and looking at what turns up in between, and this is that
 * filter.
 *
 * It is its own file because two things need it and they must not disagree.
 * loudness.h measures the true peak of a take, and limiter.h holds one under a
 * ceiling; a limiter built on a slightly different interpolator from the meter
 * that judges it would leave the window reporting a take over a ceiling it had
 * just been put under, and no amount of care in either file would fix that. One
 * table of taps, built once, read by both.
 *
 * Four times is the accuracy limit rather than the filter: the oversampled grid
 * only looks between the samples every quarter of one, so a peak falling
 * between two of those points is missed by up to about 0.4 dB on content near
 * Nyquist. Widening the taps does not help that. Four is what the standard says
 * and what every other implementation of it uses, which is the more useful
 * property - the figure here and the figure another tool reports agree.
 *
 * No allocation, no audio system and no state: build the table, read from it.
 */
#ifndef AUDIAKI_TRUEPEAK_H
#define AUDIAKI_TRUEPEAK_H

#include <stddef.h>

/* BS.1770-4 Annex 2: four times oversampling, twelve taps a phase. */
#define AUD_TRUEPEAK_PHASES 4u
#define AUD_TRUEPEAK_TAPS 12u

/*
 * Where in a window the interpolated points sit.
 *
 * Tap `t` reads the sample `t` places into the window, and the point being
 * interpolated is `frac` of a sample after the one at this offset. So the three
 * points a window yields lie between samples AUD_TRUEPEAK_CENTRE and
 * AUD_TRUEPEAK_CENTRE + 1 of it, and a caller looking for what happens either
 * side of sample n wants the window starting at n - AUD_TRUEPEAK_CENTRE.
 */
#define AUD_TRUEPEAK_CENTRE 5u

typedef struct
{
  float tap[AUD_TRUEPEAK_PHASES][AUD_TRUEPEAK_TAPS];

  /*
   * The most any phase can make of a window, which is the sum of one row's
   * magnitudes: no window can come back larger than this times its largest
   * sample. What lets a caller measuring a whole take dismiss most of it
   * without filtering it at all.
   */
  double bound;
} aud_truepeak;

/* Fill in the table. Cheap enough to do per meter rather than share one. */
void aud_truepeak_build(aud_truepeak *f);

/*
 * The largest magnitude the filter finds between the samples of `window`, which
 * holds AUD_TRUEPEAK_TAPS consecutive samples of one channel.
 *
 * Phase 0 is not run: a sinc through the sample it sits on is one there and
 * zero at every other sample, so it would only rediscover a sample the caller
 * already has. What comes back is therefore strictly what is *between* them,
 * and a caller wanting the true peak takes it against the samples themselves.
 */
float aud_truepeak_between(const aud_truepeak *f, const float *window);

#endif /* AUDIAKI_TRUEPEAK_H */
