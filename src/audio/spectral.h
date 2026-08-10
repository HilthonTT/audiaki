/* SPDX-License-Identifier: MIT */
/*
 * spectral.h - a spectrum you can edit, and the audio put back together from it.
 *
 * The analyser in spectrum.h answers "what is happening now" for a display: a
 * few dozen log-spaced bars, smoothed in time so they look right moving. This
 * answers a different question - "what is in this recording, and what would it
 * sound like without some of it" - and so it keeps every FFT bin, does not
 * smooth anything in time, and can run the transform backwards.
 *
 * Three things live here, and they are together because the editing only makes
 * sense on top of the measuring:
 *
 *   a reading   what a stretch of audio holds, per bin: the average level, the
 *               loudest it got, and the quietest it ever fell to
 *   a curve     a gain per bin, which is what the graph is when you draw on it
 *   a profile   the noise on its own, subtracted from whatever it is buried in
 *
 * The curve is the direct one: paint the graph down at 50 Hz and 50 Hz is gone.
 * That handles a hum, a whine, a switch-mode supply singing into a single-coil
 * pickup - anything that sits at a frequency you can point at.
 *
 * The profile is for the rest: hiss and buzz spread across the whole spectrum,
 * which no notch will take out without taking the instrument with it. Given an
 * estimate of the noise alone, every frame has that estimate subtracted from
 * it, so quiet bins fall away and loud ones - the notes - are barely touched.
 * The estimate is the point, and the honest way to get one is a stretch of
 * recording with nothing being played on it: see aud_spectral_scan() below, and
 * the per-bin minimum, which is what a steady noise looks like from a distance.
 *
 * Neither is a filter with a frequency response you could plot on a bode chart.
 * Both are short-time Fourier work: overlapping Hann windows, transformed,
 * scaled bin by bin, transformed back and added up again. That is the only way
 * "take out exactly this and leave everything around it" is expressible at all,
 * and its cost is the usual one - a gain that changes sharply between one bin
 * and the next smears in time, which is why the curve is painted with a brush
 * of some width rather than set one bin at a time.
 *
 * The overlap-add is normalised by the window energy that actually landed on
 * each output sample rather than by the constant it should come to, so a flat
 * curve and no profile returns the input sample for sample, including at the
 * two ends where the windows do not yet overlap. That is worth having: it makes
 * "changed nothing" mean it, and it is what the test asserts.
 *
 * No file, no device and no drawing, so it is unit tested like the rest of
 * audio/.
 */
#ifndef AUDIAKI_SPECTRAL_H
#define AUDIAKI_SPECTRAL_H

#include <stddef.h>

/*
 * The analysis window. Four times what the live display uses, because this is
 * looking for a mains hum and its harmonics rather than for the shape of a
 * chord: at 48 kHz a 4096 point window puts a bin every 11.7 Hz, which is
 * enough to sit a notch between 50 Hz and the low E of a bass at 41 Hz.
 *
 * Bigger would separate them further and smear a transient further, and a
 * fretted note is not worth spreading over two hundred milliseconds to get at
 * the hum underneath it.
 */
#define AUD_SPECTRAL_FFT 4096u

/* Windows to a hop: 75% overlap, which is what Hann needs to add back up. */
#define AUD_SPECTRAL_OVERLAP 4u

/* What a bin reads as when there is nothing in it, so a log has a floor. */
#define AUD_SPECTRAL_FLOOR_DB (-120.0f)

/*
 * How hard the profile may be subtracted, and how far down it may pull. A
 * strength above 1 oversubtracts, which is the usual way to get the last of a
 * hiss out; the floor is what stops a bin being driven to nothing, because
 * silence in one bin surrounded by signal in its neighbours is the warbling
 * that gives spectral subtraction its bad name.
 */
#define AUD_SPECTRAL_STRENGTH_MAX 4.0f
#define AUD_SPECTRAL_FLOOR_MIN_DB (-60.0f)
#define AUD_SPECTRAL_FLOOR_MAX_DB (0.0f)

/* What the panel comes up with: enough to hear, gentle enough to undo by ear. */
#define AUD_SPECTRAL_DEFAULT_STRENGTH 1.0f
#define AUD_SPECTRAL_DEFAULT_FLOOR_DB (-18.0f)

typedef struct aud_spectral aud_spectral;

/*
 * A window's worth of audio, and what a reading is made of. `size` must be a
 * power of two; AUD_SPECTRAL_FFT is what the window uses.
 *
 * Returns NULL with errno set to EINVAL or ENOMEM.
 */
aud_spectral *aud_spectral_create(unsigned rate, size_t size);

void aud_spectral_destroy(aud_spectral *s);

unsigned aud_spectral_rate(const aud_spectral *s);
size_t aud_spectral_size(const aud_spectral *s);

/* Bins a reading has: size / 2 + 1, DC and Nyquist included. */
size_t aud_spectral_bins(const aud_spectral *s);

/* The frequency bin `k` covers, and the bin `hz` falls in. */
double aud_spectral_hz(const aud_spectral *s, size_t bin);
size_t aud_spectral_bin_at(const aud_spectral *s, double hz);

/* -- taking a reading ------------------------------------------------------ */

/*
 * Look at some audio. `begin` throws away whatever was read before, `scan`
 * takes one window of up to `size` mono samples - short ones are zero-padded,
 * which only happens for a selection shorter than the window - and `end`
 * turns the windows into the three readings below.
 *
 * One window at a time rather than a stream, because a reading of a forty
 * minute take does not need every window in it: the caller spreads a few
 * hundred across the range and gets the same answer for a thousandth of the
 * work. See edit/repair.h, which is what does the spreading.
 */
void aud_spectral_read_begin(aud_spectral *s);
void aud_spectral_read(aud_spectral *s, const float *mono, size_t frames);
void aud_spectral_read_end(aud_spectral *s);

/* Non-zero once a reading has been taken, which is what the graph draws. */
int aud_spectral_has_reading(const aud_spectral *s);

/* Windows the reading was made from. Zero before there is one. */
size_t aud_spectral_windows(const aud_spectral *s);

/*
 * The reading, as linear magnitude per bin, normalised so a full scale sine
 * reads 1.0 - the same scale spectrum.h works in. Each is aud_spectral_bins()
 * long and stays valid until the next read.
 *
 *   mean  the average across the windows: what is there overall
 *   peak  the loudest any window got: where the notes reached
 *   low   the quietest any window fell to, which is the useful one. Anything
 *         playing comes and goes and so has a low near silence; a steady hum
 *         or hiss is there in every window and so cannot fall below itself.
 *         That is what makes the noise floor visible without being told where
 *         to look for it - see aud_spectral_guess_noise().
 */
const float *aud_spectral_mean(const aud_spectral *s);
const float *aud_spectral_peak(const aud_spectral *s);
const float *aud_spectral_low(const aud_spectral *s);

/* -- the noise profile ----------------------------------------------------- */

/*
 * Take the reading just made as the noise itself. For a selection the user has
 * said holds nothing but the noise - the count-in, the tail after the last
 * note - where the average is the estimate.
 */
void aud_spectral_learn_noise(aud_spectral *s);

/*
 * Take the per-bin minimum of the reading as the noise instead, which needs no
 * silent stretch to be found first: select the whole take and this is what was
 * underneath all of it. Less exact than learning from real silence, and very
 * much better than nothing, which is the alternative most of the time.
 */
void aud_spectral_guess_noise(aud_spectral *s);

int aud_spectral_has_noise(const aud_spectral *s);
void aud_spectral_forget_noise(aud_spectral *s);

/* The profile, in the same units as the readings, or NULL when there is none. */
const float *aud_spectral_noise(const aud_spectral *s);

/* How hard it is subtracted, and how far down. Both are clamped to the bounds. */
void aud_spectral_set_reduction(aud_spectral *s, float strength, float floor_db);
float aud_spectral_strength(const aud_spectral *s);
float aud_spectral_floor_db(const aud_spectral *s);

/* -- the curve ------------------------------------------------------------- */

/*
 * The gain per bin, linear, one entry per aud_spectral_bins(). All ones until
 * something is painted on it. This is the array the graph draws and the array
 * the resynthesis multiplies by, which is the whole point: what is on screen
 * is not a picture of the edit, it is the edit.
 */
const float *aud_spectral_curve(const aud_spectral *s);

/* Put every bin back to unity. */
void aud_spectral_flatten(aud_spectral *s);

/*
 * Non-zero when applying `s` would change anything at all: something painted,
 * or a profile with any strength behind it. What the Apply button reads.
 */
int aud_spectral_would_change(const aud_spectral *s);

/*
 * Set the curve to `gain` across [lo_hz, hi_hz], with the edges eased over a
 * few bins rather than stepped.
 *
 * Eased because a wall in the frequency domain is a long ring in the time
 * domain: a brick-wall notch on a bass note does not remove a hum, it removes
 * a hum and adds a chirp after every string it was under. The easing is a
 * cosine over AUD_SPECTRAL_EDGE_BINS at each end.
 */
void aud_spectral_paint(aud_spectral *s, double lo_hz, double hi_hz, float gain);

/*
 * Bring the reading down to `magnitude` across [lo_hz, hi_hz]: each bin gets
 * whatever gain it takes to put what is there at that level, and a bin already
 * quieter than that is left where it is.
 *
 * This is what dragging on the graph does, and why the graph is worth having.
 * A hum is a spike standing out of the floor around it, and "pull that spike
 * down to the floor" is the whole edit - said by pointing at it, rather than by
 * working out what attenuation the spike happens to need.
 *
 * Only ever takes more out: a bin already pulled further down by an earlier
 * stroke stays there, so dragging back and forth over a peak deepens the cut
 * rather than undoing half of it. aud_spectral_paint() with a gain of 1.0 is
 * the way back up.
 *
 * Does nothing without a reading, there being no spectrum to pull down.
 */
void aud_spectral_pull_down(aud_spectral *s, double lo_hz, double hi_hz, float magnitude);

/* Bins each edge of a painted band is eased over. */
#define AUD_SPECTRAL_EDGE_BINS 3u

/*
 * Notch `hz` and, when `harmonics` is more than one, that many multiples of it,
 * each `width_hz` wide. One call for the whole of a mains hum, which is never
 * the fundamental alone: 50 Hz on its own leaves 100, 150 and 200 buzzing away,
 * and hunting them down one at a time on the graph is the tedium this exists to
 * remove.
 *
 * Multiples past Nyquist are skipped rather than folded back.
 */
void aud_spectral_notch(aud_spectral *s, double hz, double width_hz, unsigned harmonics,
                        float gain);

/*
 * The most likely fundamental of a steady hum in the reading, in Hz, or 0.0
 * when nothing in it looks like one.
 *
 * Runs over the `low` reading, which is where a steady tone stands out and a
 * played note does not, and scores each candidate on how much its harmonics
 * stand above their surroundings. What the "Find hum" button offers, and it is
 * an offer: it fills the frequency in, and the user is the one who presses
 * Notch.
 */
double aud_spectral_find_hum(const aud_spectral *s);

/* Where find_hum() looks, which covers 50 and 60 Hz mains and their neighbours. */
#define AUD_SPECTRAL_HUM_MIN_HZ 30.0
#define AUD_SPECTRAL_HUM_MAX_HZ 130.0

/* -- what it would come to ------------------------------------------------- */

/*
 * The reading as it would be after the curve and the profile, in the same
 * linear units, written into `out` (aud_spectral_bins() entries).
 *
 * So the graph can draw the result over the original rather than making anyone
 * imagine it. Costs a pass over the bins and no audio at all, so it is cheap
 * enough to recompute on every frame of a drag.
 */
void aud_spectral_result(const aud_spectral *s, float *out);

/* Magnitude to dBFS, floored at AUD_SPECTRAL_FLOOR_DB. */
float aud_spectral_db(float magnitude);

/* -- putting the audio back together --------------------------------------- */

/*
 * Frames of run-up the caller should hand over either side of the range it
 * actually wants, and throw away afterwards.
 *
 * The transform at the edge of a buffer sees the silence past it and filters
 * accordingly, which colours the first and last window's worth. Handing it the
 * audio that really is there either side and then discarding the result over
 * that span is what makes an edit in the middle of a take join onto the rest of
 * it without a seam.
 */
size_t aud_spectral_context(const aud_spectral *s);

/*
 * Put `frames` frames of interleaved audio through the curve and the profile,
 * `in` to `out`. Both hold frames * channels floats, and `in == out` is allowed.
 *
 * Every channel gets the same treatment, analysed separately - a hum is in both
 * sides of a stereo take and is not necessarily the same size in each.
 *
 * Returns 0, or -1 with errno set to EINVAL or ENOMEM, having left `out`
 * untouched.
 */
int aud_spectral_process(aud_spectral *s, const float *in, float *out, size_t frames,
                         unsigned channels);

#endif /* AUDIAKI_SPECTRAL_H */
