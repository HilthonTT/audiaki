/* SPDX-License-Identifier: MIT */
#include "audio/format.h"

#include "util/bytes.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

unsigned aud_format_hw_bytes(aud_format fmt)
{
  switch (fmt)
  {
  case AUD_FORMAT_S16_LE:
    return 2;
  case AUD_FORMAT_S24_3LE:
    return 3;
  case AUD_FORMAT_S24_LE:
    return 4;
  case AUD_FORMAT_S32_LE:
    return 4;
  case AUD_FORMAT_UNKNOWN:
  default:
    return 0;
  }
}

unsigned aud_format_wav_bits(aud_format fmt)
{
  switch (fmt)
  {
  case AUD_FORMAT_S16_LE:
    return 16;
  case AUD_FORMAT_S24_3LE:
  case AUD_FORMAT_S24_LE: /* repacked to 3 bytes on write */
    return 24;
  case AUD_FORMAT_S32_LE:
    return 32;
  case AUD_FORMAT_UNKNOWN:
  default:
    return 0;
  }
}

unsigned aud_format_wav_bytes(aud_format fmt)
{
  return aud_format_wav_bits(fmt) / 8u;
}

int aud_format_needs_repack(aud_format fmt)
{
  /* S24_LE is the only format whose container is wider than its WAV layout. */
  return fmt == AUD_FORMAT_S24_LE;
}

const char *aud_format_name(aud_format fmt)
{
  switch (fmt)
  {
  case AUD_FORMAT_S16_LE:
    return "s16_le";
  case AUD_FORMAT_S24_3LE:
    return "s24_3le";
  case AUD_FORMAT_S24_LE:
    return "s24_le";
  case AUD_FORMAT_S32_LE:
    return "s32_le";
  case AUD_FORMAT_UNKNOWN:
  default:
    return "unknown";
  }
}

aud_format aud_format_from_name(const char *name)
{
  static const struct
  {
    const char *name;
    aud_format fmt;
  } table[] = {
      {"s16_le", AUD_FORMAT_S16_LE},   {"s16", AUD_FORMAT_S16_LE},
      {"s24_3le", AUD_FORMAT_S24_3LE}, {"s24_le", AUD_FORMAT_S24_LE},
      {"s24", AUD_FORMAT_S24_3LE},     {"s32_le", AUD_FORMAT_S32_LE},
      {"s32", AUD_FORMAT_S32_LE},
  };

  if (name == NULL)
  {
    return AUD_FORMAT_UNKNOWN;
  }

  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
  {
    const char *a = name;
    const char *b = table[i].name;
    while (*a != '\0' && *b != '\0')
    {
      char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
      if (ca != *b)
      {
        break;
      }
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0')
    {
      return table[i].fmt;
    }
  }
  return AUD_FORMAT_UNKNOWN;
}

void aud_format_repack(void *dst, const void *src, size_t samples, aud_format fmt)
{
  unsigned char *out = (unsigned char *)dst;
  const unsigned char *in = (const unsigned char *)src;

  if (fmt != AUD_FORMAT_S24_LE)
  {
    return;
  }

  /* 4 byte container -> 3 packed little-endian bytes, dropping the pad byte. */
  for (size_t i = 0; i < samples; i++)
  {
    out[i * 3 + 0] = in[i * 4 + 0];
    out[i * 3 + 1] = in[i * 4 + 1];
    out[i * 3 + 2] = in[i * 4 + 2];
  }
}

/*
 * Peak helpers. The format switch is hoisted out of the sample loop: this
 * runs on every captured period, so a branch per sample is worth avoiding.
 */

static double peak_s16(const unsigned char *p, size_t n)
{
  int32_t worst = 0;
  for (size_t i = 0; i < n; i++)
  {
    int32_t s = aud_rd_s16le(p + i * 2);
    int32_t v = (s < 0) ? -s : s;
    if (v > worst)
    {
      worst = v;
    }
  }
  return (double)worst / 32768.0;
}

static double peak_s24_3(const unsigned char *p, size_t n)
{
  int32_t worst = 0;
  for (size_t i = 0; i < n; i++)
  {
    int32_t s = aud_rd_s24le(p + i * 3);
    int32_t v = (s < 0) ? -s : s;
    if (v > worst)
    {
      worst = v;
    }
  }
  return (double)worst / 8388608.0;
}

static double peak_s24_4(const unsigned char *p, size_t n)
{
  int32_t worst = 0;
  for (size_t i = 0; i < n; i++)
  {
    /* the fourth byte is padding, so this is the three byte read again */
    int32_t s = aud_rd_s24le(p + i * 4);
    int32_t v = (s < 0) ? -s : s;
    if (v > worst)
    {
      worst = v;
    }
  }
  return (double)worst / 8388608.0;
}

static double peak_s32(const unsigned char *p, size_t n)
{
  double worst = 0.0;
  for (size_t i = 0; i < n; i++)
  {
    /* -INT32_MIN overflows, so widen before taking the absolute value */
    double v = (double)aud_rd_s32le(p + i * 4);
    if (v < 0.0)
    {
      v = -v;
    }
    if (v > worst)
    {
      worst = v;
    }
  }
  return worst / 2147483648.0;
}

void aud_format_pick_channel(void *dst, const void *src, size_t frames, unsigned channels,
                             unsigned channel, aud_format fmt)
{
  unsigned char *out = (unsigned char *)dst;
  const unsigned char *in = (const unsigned char *)src;
  unsigned bytes = aud_format_hw_bytes(fmt);
  size_t stride;

  if (out == NULL || in == NULL || bytes == 0 || channels == 0 || channel >= channels)
  {
    return;
  }

  stride = (size_t)channels * bytes;
  in += (size_t)channel * bytes;

  for (size_t i = 0; i < frames; i++)
  {
    memcpy(out + i * bytes, in + i * stride, bytes);
  }
}

/*
 * One frame's channels averaged, in and out of the capture layout. Written as
 * one loop per format for the same reason the peak and decode helpers are: this
 * runs on every captured period, and hoisting the switch out of the sample loop
 * is worth the repetition.
 */
static void mix_frames(unsigned char *out, const unsigned char *in, size_t frames,
                       unsigned ch, aud_format fmt)
{
  unsigned width = aud_format_hw_bytes(fmt);
  size_t stride = (size_t)ch * width;

  for (size_t f = 0; f < frames; f++)
  {
    const unsigned char *p = in + f * stride;
    int64_t sum = 0;

    switch (fmt)
    {
    case AUD_FORMAT_S16_LE:
      for (unsigned c = 0; c < ch; c++)
      {
        sum += aud_rd_s16le(p + (size_t)c * 2u);
      }
      aud_wr_s16le(out + f * 2u, (int32_t)(sum / (int64_t)ch));
      break;
    case AUD_FORMAT_S24_3LE:
      for (unsigned c = 0; c < ch; c++)
      {
        sum += aud_rd_s24le(p + (size_t)c * 3u);
      }
      aud_wr_s24le(out + f * 3u, (int32_t)(sum / (int64_t)ch));
      break;
    case AUD_FORMAT_S24_LE:
      for (unsigned c = 0; c < ch; c++)
      {
        sum += aud_rd_s24le(p + (size_t)c * 4u);
      }
      /* back into the four byte container the repack step expects to find */
      aud_wr_s24le_padded(out + f * 4u, (int32_t)(sum / (int64_t)ch));
      break;
    case AUD_FORMAT_S32_LE:
      for (unsigned c = 0; c < ch; c++)
      {
        sum += aud_rd_s32le(p + (size_t)c * 4u);
      }
      aud_wr_s32le(out + f * 4u, (int32_t)(sum / (int64_t)ch));
      break;
    case AUD_FORMAT_UNKNOWN:
    default:
      return;
    }
  }
}

void aud_format_mix_channels(void *dst, const void *src, size_t frames, unsigned channels,
                             aud_format fmt)
{
  unsigned char *out = (unsigned char *)dst;
  const unsigned char *in = (const unsigned char *)src;
  unsigned bytes = aud_format_hw_bytes(fmt);

  if (out == NULL || in == NULL || bytes == 0 || channels == 0)
  {
    return;
  }

  /* one channel is already the mix of itself, and copying beats decoding it */
  if (channels == 1u)
  {
    memcpy(out, in, frames * bytes);
    return;
  }

  mix_frames(out, in, frames, channels, fmt);
}

double aud_format_peak(const void *buf, size_t frames, unsigned channels, aud_format fmt)
{
  const unsigned char *p = (const unsigned char *)buf;
  size_t n = frames * (size_t)channels;
  double peak;

  if (p == NULL || n == 0)
  {
    return 0.0;
  }

  switch (fmt)
  {
  case AUD_FORMAT_S16_LE:
    peak = peak_s16(p, n);
    break;
  case AUD_FORMAT_S24_3LE:
    peak = peak_s24_3(p, n);
    break;
  case AUD_FORMAT_S24_LE:
    peak = peak_s24_4(p, n);
    break;
  case AUD_FORMAT_S32_LE:
    peak = peak_s32(p, n);
    break;
  case AUD_FORMAT_UNKNOWN:
  default:
    return 0.0;
  }

  /* Full negative scale is one LSB past full positive scale; clamp to 1.0. */
  return peak > 1.0 ? 1.0 : peak;
}

/*
 * Mono decoders. Same shape as the peak helpers above: one loop per format so
 * the switch is paid once per buffer rather than once per sample.
 */

static void mono_s16(float *dst, const unsigned char *p, size_t frames, unsigned ch)
{
  for (size_t f = 0; f < frames; f++)
  {
    double sum = 0.0;
    for (unsigned c = 0; c < ch; c++)
    {
      sum += (double)aud_rd_s16le(p + (f * ch + c) * 2) / 32768.0;
    }
    dst[f] = (float)(sum / ch);
  }
}

static void mono_s24_3(float *dst, const unsigned char *p, size_t frames, unsigned ch)
{
  for (size_t f = 0; f < frames; f++)
  {
    double sum = 0.0;
    for (unsigned c = 0; c < ch; c++)
    {
      sum += (double)aud_rd_s24le(p + (f * ch + c) * 3) / 8388608.0;
    }
    dst[f] = (float)(sum / ch);
  }
}

static void mono_s24_4(float *dst, const unsigned char *p, size_t frames, unsigned ch)
{
  for (size_t f = 0; f < frames; f++)
  {
    double sum = 0.0;
    for (unsigned c = 0; c < ch; c++)
    {
      sum += (double)aud_rd_s24le(p + (f * ch + c) * 4) / 8388608.0;
    }
    dst[f] = (float)(sum / ch);
  }
}

static void mono_s32(float *dst, const unsigned char *p, size_t frames, unsigned ch)
{
  for (size_t f = 0; f < frames; f++)
  {
    double sum = 0.0;
    for (unsigned c = 0; c < ch; c++)
    {
      sum += (double)aud_rd_s32le(p + (f * ch + c) * 4) / 2147483648.0;
    }
    dst[f] = (float)(sum / ch);
  }
}

void aud_format_to_mono(float *dst, const void *src, size_t frames, unsigned channels,
                        aud_format fmt)
{
  const unsigned char *p = (const unsigned char *)src;

  if (dst == NULL || frames == 0)
  {
    return;
  }

  if (p == NULL || channels == 0)
  {
    memset(dst, 0, frames * sizeof(*dst));
    return;
  }

  switch (fmt)
  {
  case AUD_FORMAT_S16_LE:
    mono_s16(dst, p, frames, channels);
    return;
  case AUD_FORMAT_S24_3LE:
    mono_s24_3(dst, p, frames, channels);
    return;
  case AUD_FORMAT_S24_LE:
    mono_s24_4(dst, p, frames, channels);
    return;
  case AUD_FORMAT_S32_LE:
    mono_s32(dst, p, frames, channels);
    return;
  case AUD_FORMAT_UNKNOWN:
  default:
    memset(dst, 0, frames * sizeof(*dst));
    return;
  }
}

/*
 * Interleaved decoders. Channel layout is preserved, so unlike the mono pass
 * there is nothing to average and the sample index runs flat across the buffer.
 */

static void flt_s16(float *dst, const unsigned char *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
  {
    dst[i] = (float)((double)aud_rd_s16le(p + i * 2) / 32768.0);
  }
}

static void flt_s24_3(float *dst, const unsigned char *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
  {
    dst[i] = (float)((double)aud_rd_s24le(p + i * 3) / 8388608.0);
  }
}

static void flt_s24_4(float *dst, const unsigned char *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
  {
    dst[i] = (float)((double)aud_rd_s24le(p + i * 4) / 8388608.0);
  }
}

static void flt_s32(float *dst, const unsigned char *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
  {
    dst[i] = (float)((double)aud_rd_s32le(p + i * 4) / 2147483648.0);
  }
}

void aud_format_to_float(float *dst, const void *src, size_t frames, unsigned channels,
                         aud_format fmt)
{
  const unsigned char *p = (const unsigned char *)src;
  size_t n = frames * (size_t)channels;

  if (dst == NULL || frames == 0 || channels == 0)
  {
    return;
  }

  if (p == NULL)
  {
    memset(dst, 0, n * sizeof(*dst));
    return;
  }

  switch (fmt)
  {
  case AUD_FORMAT_S16_LE:
    flt_s16(dst, p, n);
    return;
  case AUD_FORMAT_S24_3LE:
    flt_s24_3(dst, p, n);
    return;
  case AUD_FORMAT_S24_LE:
    flt_s24_4(dst, p, n);
    return;
  case AUD_FORMAT_S32_LE:
    flt_s32(dst, p, n);
    return;
  case AUD_FORMAT_UNKNOWN:
  default:
    memset(dst, 0, n * sizeof(*dst));
    return;
  }
}

double aud_format_dbfs(double peak)
{
  if (!(peak > 0.0))
  {
    return AUD_DBFS_FLOOR;
  }

  double db = 20.0 * log10(peak);
  return db < AUD_DBFS_FLOOR ? AUD_DBFS_FLOOR : db;
}
