/* SPDX-License-Identifier: MIT */
/*
 * wav.h - streaming writer for canonical 44 byte PCM WAV files.
 *
 * The header is written once with zero sizes and patched on close, so the
 * total length does not have to be known up front.
 */
#ifndef AUDIAKI_WAV_H
#define AUDIAKI_WAV_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define WAV_HEADER_BYTES 44u

/*
 * RIFF stores sizes in 32 bits. Stop short of the limit rather than write a
 * file whose header cannot describe its own payload.
 */
#define WAV_MAX_DATA_BYTES ((uint32_t)0xF0000000u)

typedef struct
{
  FILE *file;
  const char *path;
  uint32_t rate;
  uint16_t channels;
  uint16_t bits;
  uint64_t data_bytes;
} wav_writer;

/*
 * Create `path` and reserve the header. Returns 0 on success, -1 on failure
 * with errno set. When `overwrite` is zero an existing file is an error
 * (errno == EEXIST). `path` must outlive the writer.
 */
int wav_open(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
             uint16_t bits, int overwrite);

/* Append `bytes` of interleaved PCM. Returns 0 on success, -1 with errno set. */
int wav_write(wav_writer *w, const void *data, size_t bytes);

/* Non-zero when appending `bytes` more would exceed WAV_MAX_DATA_BYTES. */
int wav_would_overflow(const wav_writer *w, size_t bytes);

/*
 * Patch the header, add the RIFF pad byte if needed and close the file.
 * Returns 0 on success, -1 with errno set. The writer is left closed either
 * way, so it is safe to ignore the result on a fatal path.
 */
int wav_close(wav_writer *w);

/* Close and unlink without patching the header. Used on unrecoverable errors. */
void wav_discard(wav_writer *w);

/* Seconds of audio written so far. */
double wav_duration(const wav_writer *w);

/*
 * Serialise a 44 byte canonical header into `out`. Exposed for tests; wav_open
 * and wav_close use it internally.
 */
void wav_build_header(unsigned char out[WAV_HEADER_BYTES], uint32_t data_bytes,
                      uint32_t rate, uint16_t channels, uint16_t bits);

#endif /* AUDIAKI_WAV_H */
