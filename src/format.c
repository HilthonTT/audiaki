/* SPDX-License-Identifier: MIT */
#include "format.h"

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
    return AUD_FORMAT_UNKNOWN;

  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
  {
    const char *a = name;
    const char *b = table[i].name;
    while (*a != '\0' && *b != '\0')
    {
      char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
      if (ca != *b)
        break;
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0')
      return table[i].fmt;
  }
  return AUD_FORMAT_UNKNOWN;
}

void aud_format_repack(void *dst, const void *src, size_t samples, aud_format fmt)
{
  unsigned char *out = (unsigned char *)dst;
  const unsigned char *in = (const unsigned char *)src;

  if (fmt != AUD_FORMAT_S24_LE)
    return;

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
    int16_t s;
    memcpy(&s, p + i * 2, 2);
    int32_t v = (s < 0) ? -(int32_t)s : (int32_t)s;
    if (v > worst)
      worst = v;
  }
  return (double)worst / 32768.0;
}

static double peak_s24_3(const unsigned char *p, size_t n)
{
  int32_t worst = 0;
  for (size_t i = 0; i < n; i++)
  {
    uint32_t raw = (uint32_t)p[i * 3] | ((uint32_t)p[i * 3 + 1] << 8) |
                   ((uint32_t)p[i * 3 + 2] << 16);
    /* sign extend 24 -> 32 without relying on implementation defined shifts */
    int32_t s = (raw & 0x800000u) ? (int32_t)(raw | 0xFF000000u) : (int32_t)raw;
    int32_t v = (s < 0) ? -s : s;
    if (v > worst)
      worst = v;
  }
  return (double)worst / 8388608.0;
}

static double peak_s24_4(const unsigned char *p, size_t n)
{
  int32_t worst = 0;
  for (size_t i = 0; i < n; i++)
  {
    uint32_t raw;
    memcpy(&raw, p + i * 4, 4);
    raw &= 0x00FFFFFFu;
    int32_t s = (raw & 0x800000u) ? (int32_t)(raw | 0xFF000000u) : (int32_t)raw;
    int32_t v = (s < 0) ? -s : s;
    if (v > worst)
      worst = v;
  }
  return (double)worst / 8388608.0;
}

static double peak_s32(const unsigned char *p, size_t n)
{
  double worst = 0.0;
  for (size_t i = 0; i < n; i++)
  {
    int32_t s;
    memcpy(&s, p + i * 4, 4);
    /* -INT32_MIN overflows, so widen before taking the absolute value */
    double v = (double)s;
    if (v < 0.0)
      v = -v;
    if (v > worst)
      worst = v;
  }
  return worst / 2147483648.0;
}

double aud_format_peak(const void *buf, size_t frames, unsigned channels, aud_format fmt)
{
  const unsigned char *p = (const unsigned char *)buf;
  size_t n = frames * (size_t)channels;
  double peak;

  if (p == NULL || n == 0)
    return 0.0;

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

double aud_format_dbfs(double peak)
{
  if (!(peak > 0.0))
    return AUD_DBFS_FLOOR;

  double db = 20.0 * log10(peak);
  return db < AUD_DBFS_FLOOR ? AUD_DBFS_FLOOR : db;
}
