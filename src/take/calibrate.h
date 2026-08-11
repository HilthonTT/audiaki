/* SPDX-License-Identifier: MIT */
/*
 * calibrate.h - measuring the round trip instead of estimating it.
 *
 * take/latency.h works out how far behind an overdub lands from the two buffer
 * sizes, and says outright that this is an estimate: the converters, the driver
 * and the interface all add delay that nothing in the program can see. The
 * honest answer has always been to measure it - play a click into a loopback,
 * look at where it lands in the file, put that number in latency_ms - which is
 * a good instruction and a poor thing to ask anybody to do by hand in another
 * editor.
 *
 * This measures it. A burst goes out of the output, comes back in the input,
 * and the number of frames between the two is the round trip: the same one the
 * click is struck ahead of and an overdub is placed by, including every part of
 * it neither buffer size knows about.
 *
 * -- why a chirp and not a click --
 *
 * The burst is a short sweep from AUD_CALIBRATE_LOW_HZ to AUD_CALIBRATE_HIGH_HZ
 * rather than the metronome's tone, because what happens to it afterwards is
 * correlation and the two shapes behave very differently under one. A tone
 * correlates with itself once per cycle, so a match a whole period out is worth
 * as much as the right one and the reading can land a cycle late without
 * anything looking wrong. A sweep correlates with itself sharply and once, so
 * there is a single peak to find. Spreading the energy over a decade also means
 * it survives a path that rolls off at either end - an instrument input, an
 * AC-coupled line in - where a single tone might land where the response does
 * not.
 *
 * The match is normalised, so what comes back is compared by shape rather than
 * by level and a quiet return is as findable as a loud one, and it is taken on
 * the absolute value, so a path that inverts polarity - which a balanced cable
 * or an inverting stage will do - is still a match rather than a miss.
 *
 * -- why several --
 *
 * Playback here is fed from the capture loop and the two clocks are not the
 * same crystal, so consecutive readings differ by a millisecond or two however
 * good the measurement is. One reading cannot tell that apart from a door
 * slamming during the burst. Several can: the run takes the median, throws away
 * anything far from it, and reports the spread of what is left, which is the
 * jitter the rest of the documentation is careful to say is there.
 *
 * Resolution is a frame - 0.02 ms at 44.1 kHz - and there is no sub-sample
 * interpolation, because the jitter above is three orders of magnitude larger
 * and a number refined past its own repeatability is a false precision.
 *
 * No audio system, no clock and no device: frames in, frames out, so it is unit
 * tested against synthesised returns rather than by plugging a cable in. See
 * cmd/calibrate.c for the part that owns the streams, and the layout rule in
 * DESIGN.md for why the split is where it is.
 */
#ifndef AUDIAKI_CALIBRATE_H
#define AUDIAKI_CALIBRATE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Bursts fired in a run. Five takes a few seconds and leaves room to discard
 * one to a cough and still have a majority agreeing.
 */
#define AUD_CALIBRATE_DEFAULT_REPEATS 5u
#define AUD_CALIBRATE_MIN_REPEATS 1u
#define AUD_CALIBRATE_MAX_REPEATS 32u

/*
 * How long a burst lasts.
 *
 * Longer than it needs to be to be audible, because length is what separates a
 * real return from a chance one. The best match noise can manage against a
 * template falls off as the square root of the template's length, and the
 * search is looking at half a second of it for something ten or twenty
 * milliseconds long - so at ten the best fluke is close enough to
 * AUD_CALIBRATE_MIN_MATCH to be worth avoiding, and at twenty it is not.
 */
#define AUD_CALIBRATE_BURST_MS 20.0

/*
 * The sweep. The bottom clears the roll-off of anything AC-coupled and the top
 * stays well inside what a 4 kHz stream - the narrowest audiaki accepts - can
 * carry, so the burst is the same shape whatever the device settled on.
 */
#define AUD_CALIBRATE_LOW_HZ 300.0
#define AUD_CALIBRATE_HIGH_HZ 3000.0

/*
 * Half scale. Loud enough to come back above the floor of an input that has not
 * been turned up, quiet enough that it is not the loudest thing anyone's
 * headphones have done today.
 */
#define AUD_CALIBRATE_GAIN 0.5f

/* Seconds of stream before the first burst, so both queues reach their depth. */
#define AUD_CALIBRATE_LEAD_IN 0.5

/*
 * How well the return has to match to count.
 *
 * A loopback cable - which is what this is built around, and what the rest of
 * the documentation asks for - returns the burst nearly unchanged and matches
 * well above 0.9. Set for that rather than for a microphone across a room,
 * where reflections arrive on top of the burst and smear it: a run down a cable
 * that has to be believed is worth more than one through the air that might be.
 */
#define AUD_CALIBRATE_MIN_MATCH 0.40

/* Below this peak, nothing came back at all - as opposed to coming back wrong. */
#define AUD_CALIBRATE_SILENCE_DBFS (-60.0)

/*
 * How far from the median a reading may sit and still be one of them. Wider
 * than any jitter between two clocks in the same machine and far narrower than
 * a false match, which lands wherever the noise happened to look like a sweep.
 */
#define AUD_CALIBRATE_OUTLIER_MS 10.0

typedef struct
{
  unsigned rate;    /* the capture rate; everything here is counted in its frames */
  unsigned repeats; /* bursts to fire */
  float gain;       /* burst amplitude, before the output's own level */
} aud_calibrate_config;

/* Why a run did not produce a number, which is most of what there is to say. */
typedef enum
{
  AUD_CALIBRATE_OK = 0,
  AUD_CALIBRATE_SILENT,       /* the input never rose above the noise floor */
  AUD_CALIBRATE_UNRECOGNISED, /* something arrived, but it was not the burst */
  AUD_CALIBRATE_UNSTEADY,     /* the readings did not agree with each other */
  AUD_CALIBRATE_DROPPED,      /* the output could not play the bursts */
  AUD_CALIBRATE_SHORT,        /* the run ended before a burst had been fired */
} aud_calibrate_verdict;

typedef struct
{
  aud_calibrate_verdict verdict;
  double ms;        /* the round trip, and what belongs in latency_ms */
  uint64_t frames;  /* the same, in capture frames */
  double spread_ms; /* widest disagreement among the readings kept */
  double match;     /* the weakest match accepted, from 0 to 1 */
  double peak_dbfs; /* the loudest anything got in the windows searched */
  unsigned taken;   /* readings that counted */
  unsigned fired;   /* bursts that went out */
} aud_calibrate_result;

typedef struct aud_calibrate aud_calibrate;

/* Fill `cfg` with the defaults above for a stream of `rate` Hz. */
void aud_calibrate_config_defaults(aud_calibrate_config *cfg, unsigned rate);

/*
 * Prepare a run. Returns NULL when out of memory or when the config asks for a
 * rate or a repeat count outside what this file accepts.
 */
aud_calibrate *aud_calibrate_create(const aud_calibrate_config *cfg);
void aud_calibrate_destroy(aud_calibrate *c);

/*
 * One period, in both directions at once.
 *
 * `captured` is `frames` interleaved frames of `channels` as they arrived, and
 * `playback` is filled with the `frames` interleaved frames of `play_channels`
 * that should go out over the same period - the burst when one is due, silence
 * the rest of the time. Overwrites `playback` rather than adding to it: nothing
 * else is meant to be coming out of the output while this runs.
 *
 * Taking both sides in one call is what makes the arithmetic hold. The round
 * trip being measured is the distance between the frame a burst was written at
 * and the frame it came back at, so the two have to be counted on the same
 * clock, and the only way to be sure of that is for one call to advance it.
 *
 * Returns non-zero once the run has everything it needs.
 */
int aud_calibrate_step(aud_calibrate *c, const float *captured, unsigned channels,
                       float *playback, unsigned play_channels, size_t frames);

/*
 * Tell the run how many frames the output has dropped in total, after handing
 * the period from aud_calibrate_step() over to it.
 *
 * A burst the output could not fit is a burst nobody heard, and a burst nobody
 * heard is not evidence of a long round trip - it is no evidence at all. Told
 * about the drops, the run discards those readings rather than reporting a
 * silence they caused themselves.
 */
void aud_calibrate_note_dropped(aud_calibrate *c, unsigned long dropped);

/* Non-zero once every burst has been fired and its window has been listened to. */
int aud_calibrate_finished(const aud_calibrate *c);

/* Bursts fired so far, for a caller reporting progress. */
unsigned aud_calibrate_fired(const aud_calibrate *c);

/*
 * Non-zero once burst `index` has been fired and the whole of its window has
 * arrived, so aud_calibrate_reading() has everything it is ever going to have
 * about that one.
 *
 * What a caller reporting each burst as it lands needs, and the reason it is
 * here rather than left to a failed reading: "not yet" and "nothing came back"
 * are the same answer from aud_calibrate_reading() and want opposite responses.
 */
int aud_calibrate_ready(const aud_calibrate *c, unsigned index);

/*
 * Work out the answer from what was captured. Safe to call on a run that was
 * interrupted: the bursts that completed are used and the rest are not.
 *
 * Takes a run rather than a const one because a burst is matched once and the
 * answer kept - the search is the expensive part of this file, and a caller
 * that reports each burst as it lands and then asks for the total should pay
 * for it once.
 */
void aud_calibrate_analyse(aud_calibrate *c, aud_calibrate_result *out);

/*
 * What a single burst measured, for a caller reporting each as it lands.
 * Returns 0 and fills `*ms` and `*match` when burst `index` produced a reading,
 * and -1 when it did not - because it has not been fired, or was dropped, or
 * nothing came back.
 */
int aud_calibrate_reading(aud_calibrate *c, unsigned index, double *ms, double *match);

/* One line saying what went wrong, and what to do about it. */
const char *aud_calibrate_verdict_text(aud_calibrate_verdict verdict);

#endif /* AUDIAKI_CALIBRATE_H */
