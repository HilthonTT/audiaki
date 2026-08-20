/* SPDX-License-Identifier: MIT */
/*
 * limiter.h - putting a take under a ceiling without squaring it off.
 *
 * The window can already make something louder two ways. Turning a lane up is
 * one number on a clip, and normalizing to a peak is the same number worked out
 * for you; neither can go past full scale, because both stop where the loudest
 * sample does. Normalizing to a *loudness* is the one that can: BS.1770 is a
 * gated mean over the whole take, and a take with one transient far above its
 * average has to go past full scale to reach a loudness target. Until there was
 * this, the window's answer was to do it anyway and say so, because clipping
 * quietly would have been worse.
 *
 * This is the other answer. The signal is turned down only where it would have
 * gone over, and it is turned down *before* it gets there, which is the whole
 * difference between a limiter and a clipper: a clipper flattens the top of the
 * waveform and the flat is the distortion, where this rides the level down over
 * a few milliseconds, holds it, and lets it back up. What comes out of a
 * passage that never approaches the ceiling is what went in, sample for sample.
 *
 * The ceiling is a *true* peak, measured through audio/truepeak.h - the same
 * interpolator audio/loudness.h judges a take by, deliberately, so that a take
 * put under -1 dBTP here reads at or under -1 dBTP in --info afterwards rather
 * than a quarter of a decibel over it and no way to tell why.
 *
 * Two things it is not:
 *
 * It is not a compressor. There is one threshold, it is the ceiling, and the
 * ratio at it is infinite. Nothing here is trying to make a take sound louder
 * or denser; it is trying to stop a normalize from clipping, and a passage
 * under the ceiling comes through untouched.
 *
 * It is not on the mix. The window's export deliberately does not clamp - see
 * edit/export.h - and a limiter there would break the one promise stems make,
 * that they add back up to the mixdown. This works on a range of a lane, where
 * what it did is visible on the waveform and one press of Undo takes it back.
 *
 * Offline and in memory, because the caller has the range in memory already:
 * this is the tail of an edit, not something in a playback loop. No allocation
 * beyond a few hundred floats, no audio system, unit tested like the rest.
 */
#ifndef AUDIAKI_LIMITER_H
#define AUDIAKI_LIMITER_H

#include <stddef.h>

/*
 * The ceiling to reach for when nobody has said otherwise, in dBTP.
 *
 * -1 is what every delivery specification asks for, and the reason is between
 * the samples: a lossy encoder reconstructs a waveform that goes higher than
 * the samples it was given, so a file mastered to 0 clips on playback and one
 * mastered here does not. The same figure AUD_NORMALIZE_PEAK_DEFAULT uses, for
 * the same reason.
 */
#define AUD_LIMITER_CEILING_DEFAULT (-1.0)

/*
 * How far ahead it looks, and how long it takes to let go, in milliseconds.
 *
 * The look-ahead is what buys the difference from a clipper: the gain is
 * already down by the time the transient arrives, so the peak is ridden rather
 * than flattened. Five milliseconds is long enough that the ride is inaudible
 * on anything with an attack and short enough not to duck the note before the
 * one that was too loud - at 48 kHz it is 240 frames.
 *
 * The release is a factor of e - about 8.7 dB - over that time. Long enough
 * that a run of transients is held down as one gesture rather than pumping
 * between them, short enough that a quiet passage after a loud one is not
 * still turned down when it arrives.
 */
#define AUD_LIMITER_LOOKAHEAD_MS 5.0
#define AUD_LIMITER_RELEASE_MS 120.0

/*
 * The slowest rate this is worth deriving at. The look-ahead is a time, and at
 * a rate where five milliseconds is a handful of samples the interpolator has
 * nothing to interpolate. Nothing anybody records music at runs this slow.
 */
#define AUD_LIMITER_MIN_RATE 8000u

/*
 * Hold `frames` interleaved frames under `ceiling_db` dBTP, in place.
 *
 * One gain for every channel rather than one each: a limiter that turned the
 * left down on its own would move the image of anything panned, and the point
 * of this is to change nothing except the level of what was over.
 *
 * `reduction_db` comes back as the most it turned anything down by, which is 0
 * for a range that was already under the ceiling and never touched. It may be
 * NULL. That number is what makes the operation reportable: "limited by 2.4 dB"
 * says how much work it did, and a 0 says the press did nothing at all.
 *
 * The ceiling is held to within a few hundredths of a decibel rather than
 * exactly - the last of it is float rounding, and it is well inside the 0.4 dB
 * the four-times grid itself is uncertain by, see truepeak.h.
 *
 * It is held of the *output* rather than of the samples, which is a stronger
 * thing and takes saying. Each of the twelve samples the interpolator reads
 * around a frame carries its own gain, not that frame's, and the envelope comes
 * down fast and back up slowly - so a tap a few samples earlier can be carrying
 * a much higher gain than the frame being turned down. Bounding a frame's own
 * gain would therefore not bound what the meter reads there. The minimum the
 * gain is built from is taken over a window wide enough to cover every gain
 * that reaches into a frame's window, which is what makes the guarantee hold of
 * what comes out. Dense material - where the gain is moving at every frame - is
 * where the difference shows, and it is worth about a quarter of a decibel.
 *
 * Returns 0, or -1 with errno set to EINVAL for a shape this is not defined
 * for - no channels, a rate under AUD_LIMITER_MIN_RATE, an absurd ceiling - or
 * to ENOMEM. A failure leaves the audio exactly as it was.
 */
int aud_limiter_apply(float *interleaved, size_t frames, unsigned channels, unsigned rate,
                      double ceiling_db, double *reduction_db);

#endif /* AUDIAKI_LIMITER_H */
