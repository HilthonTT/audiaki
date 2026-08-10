/* SPDX-License-Identifier: MIT */
#include "media/wav.h"

#include "util/bytes.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* off_t, fseeko and ftello, so the reader can seek past the 2 GB mark */
#include <sys/types.h>

/* The 32-bit size fields RF64 leaves behind to say "look in ds64 instead". */
#define WAV_SIZE_IS_64_BIT 0xFFFFFFFFu

/*
 * The three pieces of a header, written in file order with whatever belongs
 * between them. Separate rather than one block because a reserved ds64 slot
 * sits between the first and the second, and the metadata between the second
 * and the third.
 */

static void build_riff(unsigned char out[12], const char *magic, uint32_t riff_size)
{
  memcpy(out + 0, magic, 4);
  aud_wr_u32le(out + 4, riff_size);
  memcpy(out + 8, "WAVE", 4);
}

static void build_fmt(unsigned char out[24], uint32_t rate, uint16_t channels,
                      uint16_t bits)
{
  uint16_t block_align = (uint16_t)(channels * (bits / 8u));

  memcpy(out + 0, "fmt ", 4);
  aud_wr_u32le(out + 4, 16u); /* PCM fmt chunk size */
  aud_wr_u16le(out + 8, 1u);  /* WAVE_FORMAT_PCM */
  aud_wr_u16le(out + 10, channels);
  aud_wr_u32le(out + 12, rate);
  aud_wr_u32le(out + 16, rate * block_align);
  aud_wr_u16le(out + 20, block_align);
  aud_wr_u16le(out + 22, bits);
}

static void build_data(unsigned char out[8], uint32_t data_bytes)
{
  memcpy(out + 0, "data", 4);
  aud_wr_u32le(out + 4, data_bytes);
}

void wav_build_header(unsigned char out[WAV_HEADER_BYTES], uint32_t data_bytes,
                      uint32_t rate, uint16_t channels, uint16_t bits)
{
  /* RIFF chunks are word aligned; an odd data chunk carries a pad byte. */
  uint32_t pad = data_bytes & 1u;

  build_riff(out, "RIFF", 36u + data_bytes + pad);
  build_fmt(out + 12, rate, channels, bits);
  build_data(out + 36, data_bytes);
}

/*
 * How big the RIFF size field should say the file is: everything after the
 * first eight bytes. Returned as 64 bits so the caller can see whether it fits.
 */
static uint64_t riff_size_of(const wav_writer *w)
{
  return (uint64_t)w->head_bytes - 8u + w->data_bytes + (w->data_bytes & 1u);
}

/* The ds64 body, which only an RF64 file ever has filled in. */
static void build_ds64(unsigned char out[WAV_DS64_BODY_BYTES], uint64_t riff_size,
                       uint64_t data_bytes, uint64_t frames)
{
  aud_wr_u32le(out + 0, (uint32_t)(riff_size & 0xFFFFFFFFu));
  aud_wr_u32le(out + 4, (uint32_t)(riff_size >> 32));
  aud_wr_u32le(out + 8, (uint32_t)(data_bytes & 0xFFFFFFFFu));
  aud_wr_u32le(out + 12, (uint32_t)(data_bytes >> 32));
  aud_wr_u32le(out + 16, (uint32_t)(frames & 0xFFFFFFFFu));
  aud_wr_u32le(out + 20, (uint32_t)(frames >> 32));
  aud_wr_u32le(out + 24, 0u); /* no table: the data chunk is the only big one */
}

/*
 * Patch the sizes in place, promoting the file to RF64 if the payload outgrew
 * what a 32-bit field can say.
 *
 * Every piece is written where it belongs rather than as one block, because
 * what sits between them - the reserved slot, the metadata - is already on disk
 * and must not be trampled.
 */
static int write_header(wav_writer *w)
{
  unsigned char riff[12];
  unsigned char data[8];
  uint64_t riff_size = riff_size_of(w);
  int promote = w->large && (riff_size > 0xFFFFFFFEu || w->data_bytes > 0xFFFFFFFEu);

  if (promote)
  {
    unsigned char slot[WAV_DS64_CHUNK_BYTES];
    uint16_t block = (uint16_t)(w->channels * (w->bits / 8u));

    build_riff(riff, "RF64", WAV_SIZE_IS_64_BIT);
    build_data(data, WAV_SIZE_IS_64_BIT);

    memcpy(slot, "ds64", 4);
    aud_wr_u32le(slot + 4, WAV_DS64_BODY_BYTES);
    build_ds64(slot + 8, riff_size, w->data_bytes, block > 0 ? w->data_bytes / block : 0);

    if (fseek(w->file, 12, SEEK_SET) != 0 ||
        fwrite(slot, 1, sizeof(slot), w->file) != sizeof(slot))
    {
      return -1;
    }
  }
  else
  {
    build_riff(riff, "RIFF", (uint32_t)riff_size);
    build_data(data, (uint32_t)w->data_bytes);
  }

  if (fseek(w->file, 0, SEEK_SET) != 0 || fwrite(riff, 1, sizeof(riff), w->file) != 12u)
  {
    return -1;
  }
  /* the data chunk header is the last eight bytes before the payload */
  if (fseek(w->file, (long)(w->head_bytes - 8u), SEEK_SET) != 0 ||
      fwrite(data, 1, sizeof(data), w->file) != 8u)
  {
    return -1;
  }
  return 0;
}

int wav_open_ex(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
                uint16_t bits, int overwrite, const aud_meta *meta, unsigned flags)
{
  unsigned char chunks[AUD_META_MAX_BYTES];
  unsigned char riff[12];
  unsigned char fmt[24];
  unsigned char data[8];
  unsigned char slot[WAV_DS64_CHUNK_BYTES];
  size_t meta_bytes = 0;
  int large = (flags & WAV_OPEN_LARGE) != 0;

  memset(w, 0, sizeof(*w));

  if (path == NULL || rate == 0 || channels == 0 || bits == 0 || (bits % 8u) != 0)
  {
    errno = EINVAL;
    return -1;
  }

  if (meta != NULL)
  {
    meta_bytes = aud_meta_build(meta, chunks, sizeof(chunks));
  }

  /* "x" fails with EEXIST rather than truncating someone's earlier take. */
  w->file = fopen(path, overwrite ? "wb" : "wbx");
  if (w->file == NULL)
  {
    return -1;
  }

  w->path = path;
  w->rate = rate;
  w->channels = channels;
  w->bits = bits;
  w->data_bytes = 0;
  w->meta_bytes = (uint32_t)meta_bytes;
  w->large = large;
  w->head_bytes =
      (uint32_t)(WAV_HEADER_BYTES + meta_bytes + (large ? WAV_DS64_CHUNK_BYTES : 0u));

  /*
   * Written as JUNK rather than ds64, and left that way unless the payload
   * needs it. A reader that has never heard of RF64 skips a JUNK chunk without
   * being told to, so a take that stayed small is an ordinary WAV file.
   */
  memcpy(slot, "JUNK", 4);
  aud_wr_u32le(slot + 4, WAV_DS64_BODY_BYTES);
  memset(slot + 8, 0, WAV_DS64_BODY_BYTES);

  build_riff(riff, "RIFF", (uint32_t)riff_size_of(w));
  build_fmt(fmt, rate, channels, bits);
  build_data(data, 0);

  /* straight through, in file order */
  if (fwrite(riff, 1, sizeof(riff), w->file) != sizeof(riff) ||
      (large && fwrite(slot, 1, sizeof(slot), w->file) != sizeof(slot)) ||
      fwrite(fmt, 1, sizeof(fmt), w->file) != sizeof(fmt) ||
      (meta_bytes > 0 && fwrite(chunks, 1, meta_bytes, w->file) != meta_bytes) ||
      fwrite(data, 1, sizeof(data), w->file) != sizeof(data))
  {
    int saved = errno;
    fclose(w->file);
    w->file = NULL;
    errno = saved;
    return -1;
  }
  return 0;
}

int wav_open_meta(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
                  uint16_t bits, int overwrite, const aud_meta *meta)
{
  /*
   * Metadata and the room to grow travel together: --no-metadata asks for the
   * plain 44 byte header, and a plain header has nowhere to put a 64-bit size.
   */
  return wav_open_ex(w, path, rate, channels, bits, overwrite, meta,
                     meta != NULL ? WAV_OPEN_LARGE : 0u);
}

int wav_open(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
             uint16_t bits, int overwrite)
{
  return wav_open_ex(w, path, rate, channels, bits, overwrite, NULL, 0u);
}

int wav_open_append(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
                    uint16_t bits)
{
  wav_reader r;
  uint64_t data_offset;
  uint64_t data_bytes;
  int large;
  long end;

  memset(w, 0, sizeof(*w));

  if (path == NULL || rate == 0 || channels == 0 || bits == 0 || (bits % 8u) != 0)
  {
    errno = EINVAL;
    return -1;
  }

  /* the reader already knows how to find the payload, whatever is in front of it */
  if (wav_read_open(&r, path) != 0)
  {
    if (errno == 0)
    {
      errno = EINVAL;
    }
    return -1;
  }

  /*
   * The stream has to match, because these frames are going on the end of
   * those. A device that came back at another rate is a different recording.
   */
  if (r.rate != rate || r.channels != channels || r.bits != bits || r.is_float)
  {
    wav_read_close(&r);
    errno = EINVAL;
    return -1;
  }

  data_offset = r.data_offset;
  data_bytes = r.frames * r.block;
  large = r.has_ds64_slot;
  wav_read_close(&r);

  w->file = fopen(path, "r+b");
  if (w->file == NULL)
  {
    return -1;
  }

  /*
   * The payload must be the last thing in the file. audiaki writes nothing
   * after it, but a file an editor has appended tags to would have them
   * overwritten by the first frame - so that one is refused rather than
   * quietly damaged.
   */
  if (fseek(w->file, 0, SEEK_END) != 0 || (end = ftell(w->file)) < 0 ||
      (uint64_t)end != data_offset + data_bytes + (data_bytes & 1u))
  {
    fclose(w->file);
    w->file = NULL;
    errno = EINVAL;
    return -1;
  }

  /*
   * An odd payload was followed by a pad byte on close. It is not part of the
   * audio, so the next frame goes over it.
   */
  if (fseek(w->file, (long)(data_offset + data_bytes), SEEK_SET) != 0)
  {
    fclose(w->file);
    w->file = NULL;
    errno = EINVAL;
    return -1;
  }

  w->path = path;
  w->rate = rate;
  w->channels = channels;
  w->bits = bits;
  w->data_bytes = data_bytes;
  w->head_bytes = (uint32_t)data_offset;
  w->meta_bytes = 0; /* only used to place the header, which head_bytes now does */
  w->large = large;
  return 0;
}

int wav_write(wav_writer *w, const void *data, size_t bytes)
{
  if (w->file == NULL)
  {
    errno = EBADF;
    return -1;
  }
  if (bytes == 0)
  {
    return 0;
  }

  if (fwrite(data, 1, bytes, w->file) != bytes)
  {
    return -1;
  }

  w->data_bytes += bytes;
  return 0;
}

int wav_would_overflow(const wav_writer *w, size_t bytes)
{
  uint64_t cap = w->large ? WAV_MAX_LARGE_DATA_BYTES : (uint64_t)WAV_MAX_DATA_BYTES;

  return w->data_bytes + (uint64_t)bytes > cap;
}

int wav_close(wav_writer *w)
{
  int rc = 0;
  int saved = 0;

  if (w->file == NULL)
  {
    return 0;
  }

  if ((w->data_bytes & 1u) != 0)
  {
    /* pad byte lives after the payload and is not counted in the data size */
    if (fseek(w->file, 0, SEEK_END) != 0 || fputc(0, w->file) == EOF)
    {
      rc = -1;
      saved = errno;
    }
  }

  if (rc == 0 && write_header(w) != 0)
  {
    rc = -1;
    saved = errno;
  }

  if (fclose(w->file) != 0 && rc == 0)
  {
    rc = -1;
    saved = errno;
  }

  w->file = NULL;
  if (rc != 0)
  {
    errno = saved;
  }
  return rc;
}

void wav_discard(wav_writer *w)
{
  if (w->file == NULL)
  {
    return;
  }

  fclose(w->file);
  w->file = NULL;
  if (w->path != NULL)
  {
    remove(w->path);
  }
}

double wav_duration(const wav_writer *w)
{
  uint32_t block_align = (uint32_t)w->channels * (w->bits / 8u);

  if (block_align == 0 || w->rate == 0)
  {
    return 0.0;
  }
  return (double)(w->data_bytes / block_align) / (double)w->rate;
}

/* -- reader ---------------------------------------------------------------- */

#define WAV_FORMAT_PCM 0x0001u
#define WAV_FORMAT_FLOAT 0x0003u
#define WAV_FORMAT_EXTENSIBLE 0xFFFEu

/* frames staged per fread(); one page or so of audio, not a tuning knob */
#define WAV_READ_CHUNK_FRAMES 4096u

static int read_exact(FILE *f, void *buf, size_t n)
{
  return fread(buf, 1, n, f) == n ? 0 : -1;
}

int wav_read_open(wav_reader *r, const char *path)
{
  unsigned char riff[12];
  unsigned char fmt[40];
  const char *msg;
  off_t data_offset = -1;
  uint64_t data_bytes = 0;
  uint16_t tag = 0;
  int have_fmt = 0;
  int is_rf64 = 0;
  uint64_t ds64_data_bytes = 0;
  int have_ds64 = 0;

  if (r == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  memset(r, 0, sizeof(*r));
  /* cleared so the caller can tell a libc failure from a bad file layout */
  errno = 0;

  if (path == NULL)
  {
    r->error = "no input path";
    errno = EINVAL;
    return -1;
  }

  r->file = fopen(path, "rb");
  if (r->file == NULL)
  {
    r->error = "cannot open the file";
    return -1;
  }

  if (read_exact(r->file, riff, sizeof(riff)) != 0)
  {
    msg = "too short to be a WAV file";
    goto fail;
  }
  /*
   * RF64 is the EBU's name and BW64 the ITU's for the same layout: a file
   * whose 32-bit sizes have overflowed and whose real ones are in a ds64
   * chunk. Both are accepted, and the only difference from here on is where
   * the data chunk's length is read from.
   */
  is_rf64 = memcmp(riff, "RF64", 4) == 0 || memcmp(riff, "BW64", 4) == 0;
  if ((memcmp(riff, "RIFF", 4) != 0 && !is_rf64) || memcmp(riff + 8, "WAVE", 4) != 0)
  {
    msg = "not a RIFF/WAVE file";
    goto fail;
  }

  /*
   * Walk the chunk list rather than assuming our own 44 byte layout: files that
   * have been through ffmpeg or an editor carry LIST/fact/id3 chunks, and the
   * data chunk is not always last.
   */
  for (;;)
  {
    unsigned char head[8];
    uint32_t size;
    off_t skip;

    if (read_exact(r->file, head, sizeof(head)) != 0)
    {
      break;
    } /* ran out of chunks */
    size = aud_rd_u32le(head + 4);
    /* RIFF chunks are word aligned; an odd body is followed by a pad byte. */
    skip = (off_t)size + (size & 1u);

    /*
     * The 64-bit sizes, which in an RF64 file are the real ones. Only the data
     * size is taken: the RIFF size describes the file rather than the audio,
     * and the sample count is a convenience that has to agree with the data
     * size anyway.
     */
    if (memcmp(head, "ds64", 4) == 0)
    {
      /* riffSize and dataSize, which is as much of the body as matters here */
      unsigned char body[16];

      if (size < sizeof(body))
      {
        msg = "malformed ds64 chunk";
        goto fail;
      }
      if (read_exact(r->file, body, sizeof(body)) != 0)
      {
        msg = "truncated ds64 chunk";
        goto fail;
      }
      ds64_data_bytes = aud_rd_u64le(body + 8);
      have_ds64 = 1;
      r->has_ds64_slot = 1;

      if (fseeko(r->file, skip - (off_t)sizeof(body), SEEK_CUR) != 0)
      {
        msg = "cannot seek past the ds64 chunk";
        goto fail;
      }
    }
    else if (memcmp(head, "fmt ", 4) == 0)
    {
      size_t take = size < sizeof(fmt) ? size : sizeof(fmt);

      if (size < 16u)
      {
        msg = "malformed fmt chunk";
        goto fail;
      }
      if (read_exact(r->file, fmt, take) != 0)
      {
        msg = "truncated fmt chunk";
        goto fail;
      }
      if (fseeko(r->file, skip - (off_t)take, SEEK_CUR) != 0)
      {
        msg = "cannot seek past the fmt chunk";
        goto fail;
      }

      tag = aud_rd_u16le(fmt);
      r->channels = aud_rd_u16le(fmt + 2);
      r->rate = aud_rd_u32le(fmt + 4);
      r->bits = aud_rd_u16le(fmt + 14);
      /* WAVE_FORMAT_EXTENSIBLE hides the real tag in the SubFormat GUID. */
      if (tag == WAV_FORMAT_EXTENSIBLE && size >= 40u)
      {
        tag = aud_rd_u16le(fmt + 24);
      }
      have_fmt = 1;
    }
    /*
     * Metadata is taken from wherever it is. A take audiaki wrote has its own
     * ahead of the audio, which is where bext is specified to go, but an editor
     * is free to append its tags after the payload instead - so the walk only
     * stops early once there is metadata in hand, and otherwise carries on past
     * the data chunk to the end of the list looking for it.
     */
    else if (memcmp(head, "LIST", 4) == 0 || memcmp(head, "bext", 4) == 0)
    {
      unsigned char body[AUD_META_MAX_BYTES];
      size_t take = size < sizeof(body) ? size : sizeof(body);

      if (read_exact(r->file, body, take) != 0)
      {
        break;
      } /* truncated: the audio may still be readable */

      if (memcmp(head, "LIST", 4) == 0)
      {
        aud_meta_read_list(&r->meta, body, take);
      }
      else
      {
        aud_meta_read_bext(&r->meta, body, take);
      }

      if (fseeko(r->file, skip - (off_t)take, SEEK_CUR) != 0)
      {
        break;
      }
    }
    else if (memcmp(head, "data", 4) == 0)
    {
      /*
       * 0xFFFFFFFF is RF64's way of saying the real length is in ds64, which
       * is required to sit ahead of this and so has already been read. Without
       * one the number is taken at face value: a chunk of very nearly 4 GB is
       * legal in plain RIFF, and it is not this reader's place to decide it
       * meant something else.
       */
      uint64_t payload = size;

      if (size == WAV_SIZE_IS_64_BIT && (have_ds64 || is_rf64))
      {
        if (!have_ds64)
        {
          msg = "RF64 file with no ds64 chunk to size it";
          goto fail;
        }
        payload = ds64_data_bytes;
      }

      /* the first one is the audio; a malformed second is not a second take */
      if (data_offset < 0)
      {
        data_offset = ftello(r->file);
        if (data_offset < 0)
        {
          msg = "cannot locate the data chunk";
          goto fail;
        }
        data_bytes = payload;
      }
      /*
       * Stepping over the payload rather than reading it, so reaching the tail
       * of a long take costs one seek. A data size larger than the file - a
       * recording killed before its header was patched - lands past the end,
       * where the next read fails and ends the walk with what has been found.
       */
      if (fseeko(r->file, (off_t)(payload + (payload & 1u)), SEEK_CUR) != 0)
      {
        break;
      }
    }
    else
    {
      /*
       * The reserved slot, still unused: a JUNK chunk of exactly ds64's size,
       * sitting where ds64 would go. That is this writer's own reservation
       * rather than anybody's padding, and it means the file can still be
       * promoted if it is carried on and grows past 4 GB.
       */
      if (memcmp(head, "JUNK", 4) == 0 && size == WAV_DS64_BODY_BYTES &&
          data_offset < 0 && !have_fmt)
      {
        r->has_ds64_slot = 1;
      }
      if (fseeko(r->file, skip, SEEK_CUR) != 0)
      {
        break;
      }
    }

    if (have_fmt && data_offset >= 0 && r->meta.present)
    {
      break;
    }
  }

  if (!have_fmt)
  {
    msg = "no fmt chunk";
    goto fail;
  }
  if (data_offset < 0)
  {
    msg = "no data chunk";
    goto fail;
  }
  if (r->channels == 0 || r->channels > 64u || r->rate == 0)
  {
    msg = "implausible channel count or sample rate";
    goto fail;
  }

  if (tag == WAV_FORMAT_FLOAT)
  {
    if (r->bits != 32u && r->bits != 64u)
    {
      msg = "only 32 and 64 bit float WAV is supported";
      goto fail;
    }
    r->is_float = 1;
  }
  else if (tag == WAV_FORMAT_PCM)
  {
    if (r->bits != 8u && r->bits != 16u && r->bits != 24u && r->bits != 32u)
    {
      msg = "only 8, 16, 24 and 32 bit PCM WAV is supported";
      goto fail;
    }
  }
  else
  {
    msg = "compressed WAV is not supported";
    goto fail;
  }

  r->block = (unsigned)r->channels * (r->bits / 8u);
  r->frames = data_bytes / r->block;
  r->position = 0;
  r->data_offset = (uint64_t)data_offset;

  if (fseeko(r->file, data_offset, SEEK_SET) != 0)
  {
    msg = "cannot rewind to the audio data";
    goto fail;
  }

  r->raw_frames = WAV_READ_CHUNK_FRAMES;
  r->raw = malloc(r->raw_frames * r->block);
  if (r->raw == NULL)
  {
    msg = "out of memory";
    errno = ENOMEM;
    goto fail;
  }

  return 0;

fail:
  fclose(r->file);
  r->file = NULL;
  r->error = msg;
  return -1;
}

/*
 * One sample, normalised to full scale. Not clamped: float WAV is allowed to
 * exceed it, and a caller measuring a file needs to see that it did.
 */
static double decode_sample(const wav_reader *r, const unsigned char *q)
{
  if (r->is_float)
  {
    return r->bits == 32u ? (double)aud_rd_f32le(q) : aud_rd_f64le(q);
  }

  switch (r->bits)
  {
  case 8u:
    /* 8 bit WAV is unsigned with 128 as silence, unlike every other depth */
    return ((double)q[0] - 128.0) / 128.0;
  case 16u:
    return (double)aud_rd_s16le(q) / 32768.0;
  case 24u:
    return (double)aud_rd_s24le(q) / 8388608.0;
  default:
    return (double)aud_rd_s32le(q) / 2147483648.0;
  }
}

/* Decode one staged block of interleaved frames down to mono. */
static void decode_mono(const wav_reader *r, float *dst, const unsigned char *src,
                        size_t frames)
{
  unsigned ch = r->channels;
  unsigned width = r->bits / 8u;

  for (size_t f = 0; f < frames; f++)
  {
    const unsigned char *p = src + (size_t)f * r->block;
    double sum = 0.0;

    for (unsigned c = 0; c < ch; c++)
    {
      sum += decode_sample(r, p + (size_t)c * width);
    }

    /* float WAV is allowed to exceed full scale; the analyser expects it not to */
    sum /= ch;
    if (sum > 1.0)
    {
      sum = 1.0;
    }
    if (sum < -1.0)
    {
      sum = -1.0;
    }
    dst[f] = (float)sum;
  }
}

/* The same block, keeping the channels apart and the values unclamped. */
static void decode_interleaved(const wav_reader *r, float *dst, const unsigned char *src,
                               size_t frames)
{
  unsigned ch = r->channels;
  unsigned width = r->bits / 8u;

  for (size_t f = 0; f < frames; f++)
  {
    const unsigned char *p = src + (size_t)f * r->block;

    for (unsigned c = 0; c < ch; c++)
    {
      dst[(size_t)f * ch + c] = (float)decode_sample(r, p + (size_t)c * width);
    }
  }
}

/*
 * The read loop both public decoders share: stage raw frames, hand each block
 * to `mono` or the per-channel decoder, stop at the end of the data chunk.
 */
static long read_decoded(wav_reader *r, float *dst, size_t frames, int mono)
{
  size_t done = 0;

  if (r == NULL || r->file == NULL || dst == NULL)
  {
    errno = EINVAL;
    return -1;
  }
  if (frames == 0 || r->position >= r->frames)
  {
    return 0;
  }

  if ((uint64_t)frames > r->frames - r->position)
  {
    frames = (size_t)(r->frames - r->position);
  }
  if (frames > (size_t)LONG_MAX)
  {
    frames = (size_t)LONG_MAX;
  }

  while (done < frames)
  {
    size_t want = frames - done;
    size_t got;

    if (want > r->raw_frames)
    {
      want = r->raw_frames;
    }

    got = fread(r->raw, r->block, want, r->file);
    if (got == 0)
    {
      if (ferror(r->file))
      {
        r->error = "read error";
        return -1;
      }
      /*
       * The data chunk claimed more than the file holds - a recording that was
       * killed before its header was patched. Report what is really there.
       */
      r->frames = r->position;
      break;
    }

    if (mono)
    {
      decode_mono(r, dst + done, r->raw, got);
    }
    else
    {
      decode_interleaved(r, dst + done * r->channels, r->raw, got);
    }
    done += got;
    r->position += got;
  }

  return (long)done;
}

long wav_read_mono(wav_reader *r, float *mono, size_t frames)
{
  return read_decoded(r, mono, frames, 1);
}

long wav_read_frames(wav_reader *r, float *interleaved, size_t frames)
{
  return read_decoded(r, interleaved, frames, 0);
}

int wav_read_seek(wav_reader *r, uint64_t frame)
{
  if (r == NULL || r->file == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  /*
   * Past the end lands at it rather than failing. Seeking forward is how a
   * player skips, and skipping past the last frame means "that is the end of
   * this one", which the read after this reports by returning nothing.
   */
  if (frame > r->frames)
  {
    frame = r->frames;
  }

  if (fseeko(r->file, (off_t)(r->data_offset + frame * r->block), SEEK_SET) != 0)
  {
    r->error = "cannot seek within the audio data";
    return -1;
  }

  r->position = frame;
  return 0;
}

double wav_read_duration(const wav_reader *r)
{
  if (r == NULL || r->rate == 0)
  {
    return 0.0;
  }
  return (double)r->frames / (double)r->rate;
}

void wav_read_close(wav_reader *r)
{
  if (r == NULL)
  {
    return;
  }

  if (r->file != NULL)
  {
    fclose(r->file);
  }
  r->file = NULL;
  free(r->raw);
  r->raw = NULL;
  r->raw_frames = 0;
}
