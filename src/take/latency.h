/* SPDX-License-Identifier: MIT */
/*
 * latency.h - where an overdub actually belongs on the timeline.
 *
 * Playing along to what is already there means hearing it first, and hearing it
 * costs time: the output holds a buffer before a sample is audible, and the
 * input holds another before a sample captured reaches the program. So what the
 * performer played in response to timeline frame N does not arrive labelled N -
 * it arrives a round trip late, every time, by the same amount.
 *
 * Left alone, that puts every overdub behind the take it was played to, by tens
 * of milliseconds, which is exactly the error nobody can play their way out of.
 * The fix is not to record differently but to place the result differently: the
 * clip starts a round trip earlier than the button was pressed, because that is
 * when the sound it holds was actually made.
 *
 * This is the arithmetic of that, and nothing else - no device, no clock, no
 * audio - so it is unit tested rather than tuned by ear.
 *
 * What it cannot correct for is jitter. audiaki's playback is fed from the
 * drawing loop and its capture runs on its own thread, so the two start within
 * a drawn frame of each other rather than on the same sample. The systematic
 * part is what this removes; the last few milliseconds are the price of not
 * having one clock behind both, and are worth knowing about rather than
 * pretending away.
 */
#ifndef AUDIAKI_LATENCY_H
#define AUDIAKI_LATENCY_H

#include <stddef.h>
#include <stdint.h>

/*
 * A round trip longer than this is a mistyped number rather than a sound card.
 * Half a second is already far past anything anyone could play to.
 */
#define AUD_LATENCY_MAX_MS 500.0

/*
 * Estimate the round trip from the two buffers it is made of, in frames at
 * `rate`. Both are what the program knows: the capture buffer the device
 * negotiated, and the playback buffer the output was opened with.
 *
 * An estimate, and honestly so - the converters, the driver and the interface
 * all add delay that nothing here can see. It is the right starting point and
 * the wrong final answer, which is why aud_latency_frames() takes an override.
 */
uint64_t aud_latency_estimate(unsigned long capture_frames,
                              unsigned long playback_frames);

/*
 * What to actually compensate by: `override_ms` when it is not negative, and
 * the estimate when it is. Clamped to AUD_LATENCY_MAX_MS either way.
 *
 * A measured number beats a computed one - play a click into a loopback and see
 * how late it lands - so the override is what anyone tuning this reaches for,
 * and zero turns compensation off entirely.
 */
uint64_t aud_latency_frames(double override_ms, unsigned rate,
                            unsigned long capture_frames, unsigned long playback_frames);

/*
 * Where a take that starts at `at` should be placed, given `latency` frames of
 * round trip.
 *
 * `*start` is the frame the clip begins on and `*skip` is how many captured
 * frames to throw away first. Normally the whole correction is a shift and
 * nothing is skipped; only near the very beginning of the timeline, where there
 * is not a round trip's worth of room to shift into, does the rest come off the
 * front of the take instead - those frames belong before frame zero, and there
 * is nowhere to put them.
 */
void aud_latency_place(uint64_t at, uint64_t latency, uint64_t *start, uint64_t *skip);

#endif /* AUDIAKI_LATENCY_H */
