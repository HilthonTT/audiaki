/* SPDX-License-Identifier: MIT */
#include "wav.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

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

void wav_build_header(unsigned char out[WAV_HEADER_BYTES], uint32_t data_bytes,
                      uint32_t rate, uint16_t channels, uint16_t bits)
{
  uint16_t block_align = (uint16_t)(channels * (bits / 8u));
  uint32_t byte_rate = rate * block_align;
  /* RIFF chunks are word aligned; an odd data chunk carries a pad byte. */
  uint32_t pad = data_bytes & 1u;

  memcpy(out + 0, "RIFF", 4);
  put_u32(out + 4, 36u + data_bytes + pad);
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

static int write_header(wav_writer *w, uint32_t data_bytes)
{
  unsigned char header[WAV_HEADER_BYTES];

  wav_build_header(header, data_bytes, w->rate, w->channels, w->bits);

  if (fseek(w->file, 0, SEEK_SET) != 0)
    return -1;
  if (fwrite(header, 1, sizeof(header), w->file) != sizeof(header))
    return -1;
  return 0;
}

int wav_open(wav_writer *w, const char *path, uint32_t rate, uint16_t channels,
             uint16_t bits, int overwrite)
{
  memset(w, 0, sizeof(*w));

  if (path == NULL || rate == 0 || channels == 0 || bits == 0 || (bits % 8u) != 0)
  {
    errno = EINVAL;
    return -1;
  }

  /* "x" fails with EEXIST rather than truncating someone's earlier take. */
  w->file = fopen(path, overwrite ? "wb" : "wbx");
  if (w->file == NULL)
    return -1;

  w->path = path;
  w->rate = rate;
  w->channels = channels;
  w->bits = bits;
  w->data_bytes = 0;

  if (write_header(w, 0) != 0)
  {
    int saved = errno;
    fclose(w->file);
    w->file = NULL;
    errno = saved;
    return -1;
  }
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
    return 0;

  if (fwrite(data, 1, bytes, w->file) != bytes)
    return -1;

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
    return 0;

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
    errno = saved;
  return rc;
}

void wav_discard(wav_writer *w)
{
  if (w->file == NULL)
    return;

  fclose(w->file);
  w->file = NULL;
  if (w->path != NULL)
    remove(w->path);
}

double wav_duration(const wav_writer *w)
{
  uint32_t block_align = (uint32_t)w->channels * (w->bits / 8u);

  if (block_align == 0 || w->rate == 0)
    return 0.0;
  return (double)(w->data_bytes / block_align) / (double)w->rate;
}
