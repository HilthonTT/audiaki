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
 *
 * RIFF counts in 32 bits, which stops a file at 4 GB - about three and a half
 * hours of 24-bit stereo at 48 kHz. RF64 (EBU Tech 3306, and the same format
 * the ITU calls BW64) is the standard way past that: a `ds64` chunk carries the
 * sizes again in 64 bits, and the 32-bit fields are left at 0xFFFFFFFF to say
 * so.
 *
 * A writer opened with WAV_OPEN_LARGE reserves room for that chunk up front, as
 * a `JUNK` chunk of exactly its size, and only fills it in on close if the
 * payload actually needed it. A take that stayed under 4 GB is therefore an
 * ordinary RIFF/WAVE file with one chunk in it that every reader already skips -
 * the promotion costs nothing to files that did not need it, and no audio has
 * to be shifted to make room when one does.
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
 * The reserved `ds64` slot: an eight byte chunk header and a body of three
 * 64-bit sizes and a table length that is always zero here.
 */
#define WAV_DS64_BODY_BYTES 28u
#define WAV_DS64_CHUNK_BYTES (8u + WAV_DS64_BODY_BYTES)

/*
 * RIFF stores sizes in 32 bits. A writer without the RF64 reservation stops
 * short of the limit rather than write a file whose header cannot describe its
 * own payload.
 */
#define WAV_MAX_DATA_BYTES ((uint32_t)0xF0000000u)

/*
 * What a writer opened WAV_OPEN_LARGE will go up to. Not a format limit - RF64
 * counts in 64 bits - but a number past which something has gone wrong rather
 * than a session having run long. Eight terabytes is a fortnight of 24-bit
 * stereo at 48 kHz.
 */
#define WAV_MAX_LARGE_DATA_BYTES ((uint64_t)0x0000080000000000ull)

/* Reserve the ds64 slot, so the take can pass 4 GB by becoming an RF64 file. */
#define WAV_OPEN_LARGE 0x1u

typedef struct
{
  FILE *file;
  const char *path;
  uint32_t rate;
  uint16_t channels;
  uint16_t bits;
  uint64_t data_bytes;
  uint32_t meta_bytes; /* metadata chunks sitting between fmt and data */
  /*
   * Where the payload starts, which is also the size of everything written
   * ahead of it. 44 + meta for a plain file, and WAV_DS64_CHUNK_BYTES more
   * when the ds64 slot was reserved.
   */
  uint32_t head_bytes;
  int large; /* the ds64 slot is there to be filled in */
} wav_writer;

/*
 * Create `path` and reserve the header. Returns 0 on success, -1 on failure
 * with errno set. When `overwrite` is zero an existing file is an error
 * (errno == EEXIST). `path` must outlive the writer.
 *
 * The canonical 44 byte header, with no metadata and no room to grow past
 * 4 GB. wav_open_ex() is the one with the choices.
 */
int wav_open(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
             uint16_t bits, int overwrite);

/*
 * The same, with `meta` describing the take written between the fmt and data
 * chunks, and the ds64 slot reserved so the take is not capped at 4 GB.
 *
 * A NULL `meta` is exactly wav_open(), down to the 44 byte header - which is
 * what --no-metadata asks for, and it keeps the 4 GB cap with it, because a
 * plain header has nowhere to put a 64-bit size.
 *
 * The metadata goes in ahead of the audio, so it is already on disk if the
 * recording is interrupted, and the data chunk simply starts further into the
 * file. Nothing else changes: the sizes are still patched on close, and the
 * payload is still plain interleaved PCM.
 */
int wav_open_meta(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
                  uint16_t bits, int overwrite, const aud_meta *meta);

/*
 * Both of the above, spelled out: `meta` may be NULL, and `flags` is zero or
 * WAV_OPEN_LARGE. The two are independent - a file can reserve the ds64 slot
 * without carrying metadata, which is what an export does.
 */
int wav_open_ex(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
                uint16_t bits, int overwrite, const aud_meta *meta, unsigned flags);

/*
 * Reopen a WAV this writer wrote and carry on appending to it, as though
 * wav_close() had never been called.
 *
 * For a take the capture device was pulled out of mid-way: the file was
 * closed and its header patched the instant the stream died, so it is a
 * complete, playable recording either way - and if the same device comes back
 * a moment later, the rest of the take belongs on the end of it rather than in
 * a second file beside it.
 *
 * `rate`, `channels` and `bits` are what the caller is about to write; the
 * file's own are checked against them and a mismatch is refused (errno
 * EINVAL), because a stream at another rate cannot be laid on the end of one
 * at this rate. Refused too if the file is not one this writer could have
 * produced - the payload has to be the last thing in it, or appending would
 * write over whatever followed.
 *
 * Nothing is rewritten on open. The bytes go on the end and the header is
 * patched by wav_close(), so a crash part way through leaves the file exactly
 * as long as it was before - the appended audio is simply not described yet,
 * which is the same amount lost as a second file that was never created.
 *
 * Returns 0 on success, -1 with errno set. `path` must outlive the writer.
 */
int wav_open_append(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
                    uint16_t bits);

/* Append `bytes` of interleaved PCM. Returns 0 on success, -1 with errno set. */
int wav_write(wav_writer *w, const void *data, size_t bytes);

/*
 * Non-zero when appending `bytes` more would pass what this writer can
 * describe: WAV_MAX_DATA_BYTES for a plain file, WAV_MAX_LARGE_DATA_BYTES for
 * one that reserved the ds64 slot.
 */
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
  /*
   * Whether there is room to describe this file in 64 bits: either a reserved
   * JUNK slot of exactly the right size, or a ds64 chunk already in use. What
   * wav_open_append() reads to decide whether a file it carries on writing can
   * still pass 4 GB.
   */
  int has_ds64_slot;
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
