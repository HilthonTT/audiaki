/* SPDX-License-Identifier: MIT */
/*
 * bytes.h - little-endian sample and word access, independent of host order.
 *
 * WAV is little-endian and so are the formats every backend hands over
 * (ALSA's S16_LE and friends), so the bytes are little-endian wherever they
 * came from. Reading them with a memcpy into an int16_t is only correct
 * because most hosts happen to agree; these read and write the bytes one at a
 * time instead, which is right on any host.
 *
 * There is no conditional compilation here on purpose. On a little-endian
 * host a compiler folds each of these back into the single load or store the
 * memcpy would have been, so the portable version costs nothing and there is
 * only ever one path to test.
 */
#ifndef AUDIAKI_BYTES_H
#define AUDIAKI_BYTES_H

#include <stdint.h>
#include <string.h>

/*
 * Non-zero when a uint32_t already sits in memory as little-endian bytes, so a
 * caller with a whole buffer of them can hand it over as-is instead of
 * serialising word by word. Only worth asking where the volume makes the
 * difference measurable - the framebuffer going down the ffmpeg pipe.
 *
 * A compiler that does not say gets zero, which takes the portable path. That
 * is slower and always correct, which is the right way round for a guess.
 */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#define AUD_HOST_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#else
#define AUD_HOST_LITTLE_ENDIAN 0
#endif

/* -- reads ----------------------------------------------------------------- */

static inline uint16_t aud_rd_u16le(const unsigned char *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t aud_rd_u32le(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline uint64_t aud_rd_u64le(const unsigned char *p)
{
  return (uint64_t)aud_rd_u32le(p) | ((uint64_t)aud_rd_u32le(p + 4) << 32);
}

static inline int16_t aud_rd_s16le(const unsigned char *p)
{
  return (int16_t)aud_rd_u16le(p);
}

static inline int32_t aud_rd_s32le(const unsigned char *p)
{
  uint32_t v = aud_rd_u32le(p);

  /*
   * Casting a value above INT32_MAX straight to int32_t is implementation
   * defined. Bringing it into range first and then subtracting the same amount
   * back off is the identical arithmetic with no such corner - and the
   * subtrahend is written as 2147483647 + 1 because the constant it adds up to
   * has no positive literal.
   */
  return (v & 0x80000000u) ? (int32_t)(v - 0x80000000u) - 2147483647 - 1 : (int32_t)v;
}

/* 24 bits in three packed bytes, sign extended to 32. */
static inline int32_t aud_rd_s24le(const unsigned char *p)
{
  uint32_t raw = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);

  return (raw & 0x800000u) ? (int32_t)raw - 0x1000000 : (int32_t)raw;
}

static inline float aud_rd_f32le(const unsigned char *p)
{
  uint32_t bits = aud_rd_u32le(p);
  float v;

  /* IEEE 754 bit patterns are the same everywhere; only the byte order moves. */
  memcpy(&v, &bits, sizeof(v));
  return v;
}

static inline double aud_rd_f64le(const unsigned char *p)
{
  uint64_t bits = aud_rd_u64le(p);
  double v;

  memcpy(&v, &bits, sizeof(v));
  return v;
}

/* -- writes ---------------------------------------------------------------- */

static inline void aud_wr_u16le(unsigned char *p, uint16_t v)
{
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static inline void aud_wr_u32le(unsigned char *p, uint32_t v)
{
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
  p[2] = (unsigned char)((v >> 16) & 0xFFu);
  p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static inline void aud_wr_s16le(unsigned char *p, int32_t v)
{
  aud_wr_u16le(p, (uint16_t)(uint32_t)v);
}

/* 24 bits into three packed bytes; the top byte of `v` is dropped. */
static inline void aud_wr_s24le(unsigned char *p, int32_t v)
{
  uint32_t u = (uint32_t)v;

  p[0] = (unsigned char)(u & 0xFFu);
  p[1] = (unsigned char)((u >> 8) & 0xFFu);
  p[2] = (unsigned char)((u >> 16) & 0xFFu);
}

/* The same 24 bits in a four byte container, sign extended into the padding. */
static inline void aud_wr_s24le_padded(unsigned char *p, int32_t v)
{
  aud_wr_u32le(p, (uint32_t)v);
}

static inline void aud_wr_s32le(unsigned char *p, int32_t v)
{
  aud_wr_u32le(p, (uint32_t)v);
}

#endif /* AUDIAKI_BYTES_H */
