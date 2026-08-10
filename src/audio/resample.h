/* SPDX-License-Identifier: MIT */
/*
 * resample.h - sample rate conversion for the playback path.
 *
 * There to stop an output that will not take the stream's rate from being a
 * refusal. Monitoring and --play both hand interleaved floats to a device that
 * may only offer 48 kHz when the take is at 44.1, and the choice used to be
 * between playing it at the wrong pitch and not playing it at all.
 *
 * Windowed sinc rather than linear interpolation, because the cut runs both
 * ways. Going up, linear interpolation is merely dull - it rolls the top off
 * and leaves images above the old Nyquist. Going down, say 96 kHz to 48, there
 * is no filter at all in a linear interpolator and everything above the new
 * Nyquist folds back into the audible band as tones that were never played.
 * The cutoff here follows the lower of the two rates, so the same code is
 * correct in both directions.
 *
 * This is the playback path only. Nothing resamples what is written to a take:
 * the file is the samples the device delivered, at the rate it delivered them.
 *
 * Free of any audio system, so it builds and is tested anywhere.
 */
#ifndef AUDIAKI_RESAMPLE_H
#define AUDIAKI_RESAMPLE_H

#include <stddef.h>

typedef struct aud_resampler aud_resampler;

/*
 * A converter from `in_rate` to `out_rate` for `channels` interleaved
 * channels. Returns NULL on a bad argument or when out of memory, with errno
 * set to EINVAL or ENOMEM.
 *
 * Equal rates are allowed and are not special-cased away here - a caller that
 * wants to skip the work entirely should not create one.
 */
aud_resampler *aud_resample_create(unsigned in_rate, unsigned out_rate,
                                   unsigned channels);

void aud_resample_destroy(aud_resampler *rs);

/*
 * An upper bound on the frames aud_resample_run() will produce from
 * `in_frames`, for sizing the output buffer. Always at least what the call
 * actually writes.
 */
size_t aud_resample_out_max(const aud_resampler *rs, size_t in_frames);

/*
 * Convert `in_frames` interleaved frames into `out`, which must hold at least
 * aud_resample_out_max(rs, in_frames) frames. Returns the frames written.
 *
 * Every input frame is consumed. How many come out varies by one either way
 * from the ratio, because the output grid does not line up with the input
 * grid and where it falls carries over between calls - which is exactly what
 * makes a stream converted in period-sized pieces come out the same as one
 * converted in a single call.
 */
size_t aud_resample_run(aud_resampler *rs, const float *in, size_t in_frames, float *out,
                        size_t out_cap);

/*
 * Forget what was buffered, for a seek. The next call starts as though the
 * stream had just begun; without this, the taps either side of a jump would
 * smear the audio from before it into the audio after.
 */
void aud_resample_reset(aud_resampler *rs);

/*
 * Frames of input the converter is holding on to for the filter's sake, which
 * is the delay it adds. A caller lining playback up against anything else
 * wants this.
 */
size_t aud_resample_latency(const aud_resampler *rs);

#endif /* AUDIAKI_RESAMPLE_H */
