/* SPDX-License-Identifier: MIT */
/*
 * wav.h - PCM WAV file I/O.
 *
 * The writer streams: the header is written once with zero sizes and patched on
 * close, so the total length does not have to be known up front.
 *
 * The reader is deliberately more forgiving than the writer is strict, because
 * it has to cope with files other tools produced - extra chunks, a wider fmt
 * chunk, float samples.
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

/* -- reader ---------------------------------------------------------------- */

typedef struct
{
  FILE *file;
  uint32_t rate;
  uint16_t channels;
  uint16_t bits;      /* 8, 16, 24, 32 or 64 */
  int is_float;       /* payload is IEEE float rather than signed integer */
  uint64_t frames;    /* total frames in the data chunk */
  uint64_t position;  /* frames handed out so far */
  const char *error;  /* static description of the last failure, or NULL */
  unsigned block;     /* bytes per frame */
  unsigned char *raw; /* staging buffer for undecoded frames */
  size_t raw_frames;  /* capacity of raw, in frames */
} wav_reader;

/*
 * Open `path` and parse its header. Returns 0 on success, -1 on failure with
 * r->error set to a description and errno set when the failure came from the
 * C library. The reader is left closed on failure.
 *
 * Accepts uncompressed PCM (8, 16, 24 or 32 bit) and IEEE float (32 or 64
 * bit), in any chunk order, with unknown chunks skipped.
 */
int wav_read_open(wav_reader *r, const char *path);

/*
 * Decode up to `frames` frames into `mono`, averaging the channels and scaling
 * to [-1.0, 1.0]. Returns the number of frames decoded, 0 at end of data, or
 * -1 on a read error with r->error set.
 */
long wav_read_mono(wav_reader *r, float *mono, size_t frames);

/* Seconds of audio in the file. */
double wav_read_duration(const wav_reader *r);

void wav_read_close(wav_reader *r);

#endif /* AUDIAKI_WAV_H */
