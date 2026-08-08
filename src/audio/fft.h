/* SPDX-License-Identifier: MIT */
/*
 * fft.h - in-place radix-2 fast Fourier transform.
 *
 * Just enough DFT for a spectrum display: power-of-two sizes, split real and
 * imaginary arrays, no plan objects and no dependencies beyond libm. Free of
 * any ALSA or I/O concern so it can be unit tested anywhere.
 */
#ifndef AUDIAKI_FFT_H
#define AUDIAKI_FFT_H

#include <stddef.h>

/* Non-zero when `n` is a power of two and at least 2. */
int aud_fft_is_pow2(size_t n);

/*
 * Forward DFT of `n` complex samples, in place. re[] and im[] each hold `n`
 * values; a real input signal passes zeros in im[].
 *
 * Does nothing unless aud_fft_is_pow2(n), so callers that already validated
 * their size do not need to check again.
 *
 * Output bin k covers frequency k * rate / n. Only bins 0..n/2 are meaningful
 * for real input; the rest mirror them.
 */
void aud_fft_forward(float *re, float *im, size_t n);

/*
 * Fill window[0..n) with a periodic Hann window. Periodic rather than
 * symmetric because the input is a stream of frames, not one isolated record.
 * The coherent gain is 0.5, which aud_spectrum() relies on when normalising.
 */
void aud_fft_hann(float *window, size_t n);

/* Magnitude sqrt(re^2 + im^2) of one bin. */
float aud_fft_magnitude(float re, float im);

#endif /* AUDIAKI_FFT_H */
