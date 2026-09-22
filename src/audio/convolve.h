/* SPDX-License-Identifier: MIT */
#ifndef AUDIAKI_CONVOLVE_H
#define AUDIAKI_CONVOLVE_H

#include <stddef.h>

typedef struct aud_convolver aud_convolver;

aud_convolver *aud_convolve_create(const float *ir, size_t ir_frames,
                                   unsigned ir_channels, unsigned channels, size_t block);

void aud_convolve_destroy(aud_convolver *cv);

size_t aud_convolve_block(const aud_convolver *cv);

size_t aud_convolve_partitions(const aud_convolver *cv);

void aud_convolve_reset(aud_convolver *cv);

void aud_convolve_prime(aud_convolver *cv, const float *in);

void aud_convolve_run(aud_convolver *cv, const float *in, float *out);

void aud_convolve_stream(aud_convolver *cv, const float *in, float *out, size_t frames);

#endif /* AUDIAKI_CONVOLVE_H */
