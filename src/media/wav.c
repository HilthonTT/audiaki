/* SPDX-License-Identifier: MIT */
#include "media/wav.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* off_t, fseeko and ftello, so the reader can seek past the 2 GB mark */
#include <sys/types.h>

static void put_u16(unsigned char *p, uint16_t v)
{
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put_u32(unsigned char *p, uint32_t v)
{
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
  p[2] = (unsigned char)((v >> 16) & 0xFFu);
  p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/*
 * The canonical header, with `extra_bytes` of chunks understood to sit between
 * the fmt and data chunks. They are not written here - only counted, because
 * the RIFF size has to include them - so the caller writes the first 36 bytes,
 * then its own chunks, then the last 8.
 */
static void build_header(unsigned char out[WAV_HEADER_BYTES], uint32_t data_bytes,
                         uint32_t extra_bytes, uint32_t rate, uint16_t channels,
                         uint16_t bits)
{
  uint16_t block_align = (uint16_t)(channels * (bits / 8u));
  uint32_t byte_rate = rate * block_align;
  /* RIFF chunks are word aligned; an odd data chunk carries a pad byte. */
  uint32_t pad = data_bytes & 1u;

  memcpy(out + 0, "RIFF", 4);
  put_u32(out + 4, 36u + extra_bytes + data_bytes + pad);
  memcpy(out + 8, "WAVE", 4);

  memcpy(out + 12, "fmt ", 4);
  put_u32(out + 16, 16u); /* PCM fmt chunk size */
  put_u16(out + 20, 1u);  /* WAVE_FORMAT_PCM */
  put_u16(out + 22, channels);
  put_u32(out + 24, rate);
  put_u32(out + 28, byte_rate);
  put_u16(out + 32, block_align);
  put_u16(out + 34, bits);

  memcpy(out + 36, "data", 4);
  put_u32(out + 40, data_bytes);
}

void wav_build_header(unsigned char out[WAV_HEADER_BYTES], uint32_t data_bytes,
                      uint32_t rate, uint16_t channels, uint16_t bits)
{
  build_header(out, data_bytes, 0, rate, channels, bits);
}

/*
 * Patch the two size fields in place. With no metadata this is one write of the
 * whole 44 byte header; with metadata the header is in two pieces with the
 * chunks between them, and each piece is written where it belongs.
 */
static int write_header(wav_writer *w, uint32_t data_bytes)
{
  unsigned char header[WAV_HEADER_BYTES];

  build_header(header, data_bytes, w->meta_bytes, w->rate, w->channels, w->bits);

  if (fseek(w->file, 0, SEEK_SET) != 0)
  {
    return -1;
  }
  if (w->meta_bytes == 0)
  {
    return fwrite(header, 1, sizeof(header), w->file) == sizeof(header) ? 0 : -1;
  }

  if (fwrite(header, 1, 36u, w->file) != 36u)
  {
    return -1;
  }
  if (fseek(w->file, (long)(36u + w->meta_bytes), SEEK_SET) != 0)
  {
    return -1;
  }
  return fwrite(header + 36, 1, 8u, w->file) == 8u ? 0 : -1;
}

int wav_open_meta(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
                  uint16_t bits, int overwrite, const aud_meta *meta)
{
  unsigned char chunks[AUD_META_MAX_BYTES];
  unsigned char header[WAV_HEADER_BYTES];
  size_t meta_bytes = 0;

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

  /* straight through, in file order: RIFF and fmt, the chunks, the data header */
  build_header(header, 0, w->meta_bytes, rate, channels, bits);
  if (fwrite(header, 1, 36u, w->file) != 36u ||
      (meta_bytes > 0 && fwrite(chunks, 1, meta_bytes, w->file) != meta_bytes) ||
      fwrite(header + 36, 1, 8u, w->file) != 8u)
  {
    int saved = errno;
    fclose(w->file);
    w->file = NULL;
    errno = saved;
    return -1;
  }
  return 0;
}

int wav_open(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
             uint16_t bits, int overwrite)
{
  return wav_open_meta(w, path, rate, channels, bits, overwrite, NULL);
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
  return w->data_bytes + (uint64_t)bytes > (uint64_t)WAV_MAX_DATA_BYTES;
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

  if (rc == 0 && write_header(w, (uint32_t)w->data_bytes) != 0)
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

static uint16_t get_u16(const unsigned char *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

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
  if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0)
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
    size = get_u32(head + 4);
    /* RIFF chunks are word aligned; an odd body is followed by a pad byte. */
    skip = (off_t)size + (size & 1u);

    if (memcmp(head, "fmt ", 4) == 0)
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

      tag = get_u16(fmt);
      r->channels = get_u16(fmt + 2);
      r->rate = get_u32(fmt + 4);
      r->bits = get_u16(fmt + 14);
      /* WAVE_FORMAT_EXTENSIBLE hides the real tag in the SubFormat GUID. */
      if (tag == WAV_FORMAT_EXTENSIBLE && size >= 40u)
      {
        tag = get_u16(fmt + 24);
      }
      have_fmt = 1;
    }
    /*
     * Metadata is read where it belongs, ahead of the audio - the loop stops
     * at the data chunk, so chunks an editor appended after the payload are
     * not seen. A take audiaki wrote always has its own before the data.
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
      data_offset = ftello(r->file);
      if (data_offset < 0)
      {
        msg = "cannot locate the data chunk";
        goto fail;
      }
      data_bytes = size;
      if (have_fmt)
      {
        break;
      } /* nothing after this point can matter */
      if (fseeko(r->file, skip, SEEK_CUR) != 0)
      {
        break;
      }
    }
    else if (fseeko(r->file, skip, SEEK_CUR) != 0)
    {
      break;
    }

    if (have_fmt && data_offset >= 0)
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
    if (r->bits == 32u)
    {
      float v;
      memcpy(&v, q, sizeof(v));
      return (double)v;
    }
    else
    {
      double v;
      memcpy(&v, q, sizeof(v));
      return v;
    }
  }

  switch (r->bits)
  {
  case 8u:
    /* 8 bit WAV is unsigned with 128 as silence, unlike every other depth */
    return ((double)q[0] - 128.0) / 128.0;
  case 16u:
  {
    int16_t v;
    memcpy(&v, q, sizeof(v));
    return (double)v / 32768.0;
  }
  case 24u:
  {
    uint32_t raw = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16);
    int32_t v = (raw & 0x800000u) ? (int32_t)(raw | 0xFF000000u) : (int32_t)raw;
    return (double)v / 8388608.0;
  }
  default:
  {
    int32_t v;
    memcpy(&v, q, sizeof(v));
    return (double)v / 2147483648.0;
  }
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
