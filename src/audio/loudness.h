/* SPDX-License-Identifier: MIT */
/*
 * loudness.h - how loud a take is, rather than how large its samples are.
 *
 * info.h already reports peak and RMS, and neither answers the question you
 * actually have after a take. Peak is about headroom and nothing else: a bass
 * note and a cymbal that both stop at -0.1 dBFS are nowhere near equally loud.
 * RMS averages every sample equally, and the ear does not - it is far less
 * sensitive at 50 Hz than at 3 kHz, so RMS rates a boomy take above a bright
 * one that sounds twice as loud beside it. Neither number lets you say "that
 * take came out four decibels quieter than the other one" and be right.
 *
 * This does, by the one method everything else agrees on: ITU-R BS.1770-4, the
 * measurement EBU R 128 and every broadcaster and streaming service is written
 * in terms of. Two filters that stand in for the ear, a mean square over
 * overlapping blocks, and a gate that throws away the silence between the notes
 * so a take with long gaps in it is not rated quieter than the same playing
 * without them.
 *
 * Four numbers come out, and they answer different questions:
 *
 *   integrated    one figure for the whole take, gated. The one to compare two
 *                 takes by, and the one a streaming service normalises to
 *   range         how far the quiet parts sit below the loud ones, in LU.
 *                 EBU Tech 3342. A take at 2 LU has been squashed; one at 15
 *                 has dynamics left in it
 *   momentary     the loudest 400 ms, ungated - where the take peaks as a
 *                 sound rather than as a sample
 *   short-term    the loudest 3 s, ungated - the loudest passage
 *
 * And beside them the true peak, which is a different measurement wearing
 * similar units. Sample peak only sees where the samples landed; the waveform
 * between two samples routinely goes higher, and an encoder or a converter
 * reconstructing it will clip there while `peak` reports headroom to spare.
 * BS.1770-4 Annex 2 answers it by oversampling four times and looking between
 * the samples, which is what aud_loudness_reading::true_peak is.
 *
 * LUFS and dBFS are both decibels but they are not the same scale and do not
 * convert: a full scale sine reads 0 dBFS and about -3.01 LUFS, and anything
 * with content spread across the band reads further apart still. Two channels
 * carrying the same signal are 3 dB louder than one, deliberately - that is the
 * summation BS.1770 specifies, not an error.
 *
 * Reads no file and touches no audio system, so it is unit tested like the rest
 * of audio/.
 */
#ifndef AUDIAKI_LOUDNESS_H
#define AUDIAKI_LOUDNESS_H

#include <stddef.h>

/* the WAV reader refuses anything wider, and so does this */
#define AUD_LOUDNESS_MAX_CHANNELS 64u

/*
 * The lowest rate the K-weighting can be derived at and still mean anything.
 * The shelf sits at 1682 Hz, so a rate approaching twice that warps it out of
 * shape and one below it cannot express it at all. Nothing anybody measures
 * loudness on runs this slow; the floor is here so a telephone-rate file gets
 * no answer rather than a wrong one.
 */
#define AUD_LOUDNESS_MIN_RATE 8000u

/*
 * A loudness that does not exist, rather than one that is very low.
 *
 * A take with nothing above the -70 LUFS absolute gate has no integrated
 * loudness - not a quiet one - and a take under 3 s long has no loudness range,
 * because the measurement is defined over 3 s windows and there is not one. Both
 * come back as this, and aud_loudness_measured() is the test. Well below any
 * real reading, so a caller that compares without testing errs quiet rather than
 * loud.
 */
#define AUD_LUFS_NONE (-1000.0)

/* Non-zero when `lufs` is a measurement rather than AUD_LUFS_NONE. */
int aud_loudness_measured(double lufs);

typedef struct aud_loudness aud_loudness;

/*
 * What a measurement came to. Every loudness is LUFS and may be AUD_LUFS_NONE;
 * `range` is LU, a difference rather than a level, and may be AUD_LUFS_NONE too.
 *
 * `true_peak` is linear and normalised the way info.h's peaks are, so 1.0 is
 * full scale and aud_format_dbfs() turns it into dBTP. Unlike a sample peak it
 * is allowed to exceed 1.0, and a take that has been squashed into a limiter
 * usually does.
 */
typedef struct
{
  double integrated;
  double range;
  double momentary_max;
  double short_max;
  double true_peak;
} aud_loudness_reading;

/*
 * Non-zero when a stream of this shape can be measured at all. Checked here
 * rather than left to a NULL return, so a caller can tell "this file is not
 * something BS.1770 describes" from "there was no memory".
 */
int aud_loudness_supported(unsigned rate, unsigned channels);

/*
 * A meter for `channels` interleaved channels at `rate`.
 *
 * Returns NULL with errno set to EINVAL when aud_loudness_supported() is false,
 * or to ENOMEM when there was no memory.
 *
 * Every channel counts at full weight. BS.1770 lifts the surround channels by
 * 1.5 dB and drops the LFE, but which channel is which comes from a layout, and
 * a WAV that audiaki wrote does not carry one: its channels are the inputs of an
 * interface, in the order the interface delivered them. Weighting the fourth
 * input of a four-input box as an LFE and dropping it would be a guess, and a
 * guess that silently changes the answer. For the mono and stereo takes this
 * measures in practice the two rules agree exactly, because L, R and C all
 * count at full weight anyway.
 */
aud_loudness *aud_loudness_create(unsigned rate, unsigned channels);

void aud_loudness_destroy(aud_loudness *l);

/*
 * Feed `frames` interleaved frames, scaled so full scale is 1.0 - which is what
 * wav_read_frames() delivers. Values beyond full scale are kept rather than
 * clamped, because a float take is allowed to hold them and a measurement that
 * quietly clamped would report the wrong loudness for one.
 *
 * Returns 0, or -1 with errno set to ENOMEM when the block history could not
 * grow. A failed call leaves the meter usable but incomplete, so a caller that
 * cares should stop and discard it rather than read it.
 */
int aud_loudness_feed(aud_loudness *l, const float *interleaved, size_t frames);

/*
 * What has been fed so far, into `out`.
 *
 * A NULL meter fills `out` with nothing measured, which is the state to start
 * from: a caller that could not create one - an unsupported rate - then has the
 * same shape of answer as one that measured a silent file, and needs no second
 * path to say "there is no figure here".
 *
 * May be called at any point and consumes nothing, so feeding can carry on
 * afterwards. It does sort the block history it keeps, which changes nothing:
 * every figure here is a mean or a percentile over that history and none of
 * them depends on the order blocks arrived in.
 *
 * The trailing part-block is not counted, as BS.1770 says: a 400 ms mean over
 * 250 ms of audio is not a quieter block, it is not a block. So a take under
 * 400 ms has no loudness at all, and one under 3 s has no range.
 */
void aud_loudness_read(aud_loudness *l, aud_loudness_reading *out);

/*
 * What a meter reads now, as opposed to what a finished take came to.
 *
 * The same three numbers every R 128 meter shows, and they answer different
 * halves of "is this the right level": `momentary` is the block that just went
 * past, which moves with the playing and is the one to watch; `short_term` is
 * steadier and is what a passage sounds like; `integrated` is everything fed so
 * far, gated, and is the figure the finished thing will be judged on.
 *
 * Note that `momentary` and `short_term` are the latest blocks rather than the
 * loudest ones - aud_loudness_reading holds the maxima, which is the question a
 * file that has already been read asks and not the question a meter does.
 *
 * Cheap enough to call every drawn frame, and consumes nothing: it neither
 * reorders the history nor looks between the samples, so feeding carries on
 * unaffected and calling it and aud_loudness_read() on the same meter is
 * allowed in either order.
 */
typedef struct
{
  double momentary;  /* the 400 ms block that just completed */
  double short_term; /* the 3 s block that just completed */
  double integrated; /* everything fed so far, gated */
} aud_loudness_live;

void aud_loudness_read_live(const aud_loudness *l, aud_loudness_live *out);

#endif /* AUDIAKI_LOUDNESS_H */
