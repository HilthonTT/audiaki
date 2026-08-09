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

#include "take/meta.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* the canonical header: RIFF, fmt and data, with nothing in between */
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
  uint32_t meta_bytes; /* metadata chunks sitting between fmt and data */
} wav_writer;

/*
 * Create `path` and reserve the header. Returns 0 on success, -1 on failure
 * with errno set. When `overwrite` is zero an existing file is an error
 * (errno == EEXIST). `path` must outlive the writer.
 */
int wav_open(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
             uint16_t bits, int overwrite);

/*
 * The same, with `meta` describing the take written between the fmt and data
 * chunks. A NULL `meta` is exactly wav_open(), down to the 44 byte header.
 *
 * The metadata goes in ahead of the audio, so it is already on disk if the
 * recording is interrupted, and the data chunk simply starts further into the
 * file. Nothing else changes: the sizes are still patched on close, and the
 * payload is still plain interleaved PCM.
 */
int wav_open_meta(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
                  uint16_t bits, int overwrite, const aud_meta *meta);

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
  uint16_t bits;     /* 8, 16, 24, 32 or 64 */
  int is_float;      /* payload is IEEE float rather than signed integer */
  uint64_t frames;   /* total frames in the data chunk */
  uint64_t position; /* frames handed out so far */
  const char *error; /* static description of the last failure, or NULL */
  unsigned block;    /* bytes per frame */
  /*
   * Where the audio starts, kept so a seek can be worked out from a frame
   * number rather than from a rewind and a re-parse. The chunks ahead of it
   * vary - see the metadata stamp - so it is not a constant.
   */
  uint64_t data_offset;
  unsigned char *raw; /* staging buffer for undecoded frames */
  size_t raw_frames;  /* capacity of raw, in frames */
  /*
   * What the file says about itself, from any LIST/INFO or bext chunk ahead of
   * the audio. Empty when there was none - which is most files, and every take
   * audiaki wrote before it started stamping them.
   */
  aud_meta_info meta;
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

/*
 * Decode up to `frames` frames into `interleaved`, keeping the channels apart
 * and scaling to full scale. Unlike wav_read_mono() the values are not clamped
 * to [-1.0, 1.0], because float WAV is allowed to exceed full scale and a
 * caller measuring a take needs to see that it did.
 *
 * `interleaved` must hold frames * r->channels floats. Returns the number of
 * frames decoded, 0 at end of data, or -1 on a read error with r->error set.
 */
long wav_read_frames(wav_reader *r, float *interleaved, size_t frames);

/*
 * Move the read position to `frame`, counting from the start of the audio, so
 * the next read comes from there. A frame past the end lands at the end, where
 * reads return nothing rather than fail.
 *
 * Returns 0, or -1 with r->error set when the file would not seek. A caller
 * that gets -1 has lost nothing but the jump: the position is unchanged and
 * reading straight on still works.
 */
int wav_read_seek(wav_reader *r, uint64_t frame);

/* Seconds of audio in the file. */
double wav_read_duration(const wav_reader *r);

void wav_read_close(wav_reader *r);

#endif /* AUDIAKI_WAV_H */
