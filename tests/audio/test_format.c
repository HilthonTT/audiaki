/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "audio/format.h"

#include <stdint.h>
#include <string.h>

TEST(format_sizes)
{
  CHECK_EQ_INT(aud_format_hw_bytes(AUD_FORMAT_S16_LE), 2);
  CHECK_EQ_INT(aud_format_hw_bytes(AUD_FORMAT_S24_3LE), 3);
  CHECK_EQ_INT(aud_format_hw_bytes(AUD_FORMAT_S24_LE), 4);
  CHECK_EQ_INT(aud_format_hw_bytes(AUD_FORMAT_S32_LE), 4);
  CHECK_EQ_INT(aud_format_hw_bytes(AUD_FORMAT_UNKNOWN), 0);

  CHECK_EQ_INT(aud_format_wav_bits(AUD_FORMAT_S24_LE), 24);
  CHECK_EQ_INT(aud_format_wav_bytes(AUD_FORMAT_S24_LE), 3);
  CHECK_EQ_INT(aud_format_wav_bytes(AUD_FORMAT_S32_LE), 4);

  /* only the 4 byte 24 bit container differs between capture and file */
  CHECK(aud_format_needs_repack(AUD_FORMAT_S24_LE));
  CHECK(!aud_format_needs_repack(AUD_FORMAT_S24_3LE));
  CHECK(!aud_format_needs_repack(AUD_FORMAT_S32_LE));
}

TEST(format_names)
{
  CHECK_EQ_STR(aud_format_name(AUD_FORMAT_S24_3LE), "s24_3le");
  CHECK_EQ_STR(aud_format_name(AUD_FORMAT_UNKNOWN), "unknown");

  CHECK_EQ_INT(aud_format_from_name("s16_le"), AUD_FORMAT_S16_LE);
  CHECK_EQ_INT(aud_format_from_name("S32_LE"), AUD_FORMAT_S32_LE);
  CHECK_EQ_INT(aud_format_from_name("s24"), AUD_FORMAT_S24_3LE);
  CHECK_EQ_INT(aud_format_from_name("s24_le"), AUD_FORMAT_S24_LE);
  CHECK_EQ_INT(aud_format_from_name("float32"), AUD_FORMAT_UNKNOWN);
  CHECK_EQ_INT(aud_format_from_name("s16_le_extra"), AUD_FORMAT_UNKNOWN);
  CHECK_EQ_INT(aud_format_from_name(""), AUD_FORMAT_UNKNOWN);
  CHECK_EQ_INT(aud_format_from_name(NULL), AUD_FORMAT_UNKNOWN);
}

TEST(repack_s24_drops_pad_byte)
{
  /* two samples in 4 byte containers, little endian, pad byte last */
  const unsigned char src[8] = {0x11, 0x22, 0x33, 0x00, 0xAA, 0xBB, 0xCC, 0xFF};
  unsigned char dst[6] = {0};

  aud_format_repack(dst, src, 2, AUD_FORMAT_S24_LE);

  CHECK_EQ_INT(dst[0], 0x11);
  CHECK_EQ_INT(dst[1], 0x22);
  CHECK_EQ_INT(dst[2], 0x33);
  CHECK_EQ_INT(dst[3], 0xAA);
  CHECK_EQ_INT(dst[4], 0xBB);
  CHECK_EQ_INT(dst[5], 0xCC);
}

TEST(peak_s16)
{
  int16_t samples[4] = {0, 16384, -16384, 8192};
  /* 16384 / 32768 == 0.5 */
  CHECK_EQ_DBL(aud_format_peak(samples, 2, 2, AUD_FORMAT_S16_LE), 0.5, 1e-9);

  samples[1] = 0;
  samples[2] = 0;
  samples[3] = 0;
  CHECK_EQ_DBL(aud_format_peak(samples, 2, 2, AUD_FORMAT_S16_LE), 0.0, 1e-9);
}

TEST(peak_full_scale_is_clamped)
{
  int16_t min_sample = -32768; /* one LSB past positive full scale */
  int32_t min32 = (int32_t)0x80000000;

  CHECK_EQ_DBL(aud_format_peak(&min_sample, 1, 1, AUD_FORMAT_S16_LE), 1.0, 1e-9);
  CHECK_EQ_DBL(aud_format_peak(&min32, 1, 1, AUD_FORMAT_S32_LE), 1.0, 1e-9);
}

TEST(peak_s24_variants_agree)
{
  /* -4194304 == -0.5 full scale in 24 bit */
  const unsigned char packed[3] = {0x00, 0x00, 0xC0};
  const unsigned char padded[4] = {0x00, 0x00, 0xC0, 0x7F}; /* pad byte ignored */

  CHECK_EQ_DBL(aud_format_peak(packed, 1, 1, AUD_FORMAT_S24_3LE), 0.5, 1e-9);
  CHECK_EQ_DBL(aud_format_peak(padded, 1, 1, AUD_FORMAT_S24_LE), 0.5, 1e-9);
}

TEST(peak_edge_cases)
{
  const unsigned char buf[4] = {0};

  CHECK_EQ_DBL(aud_format_peak(NULL, 4, 2, AUD_FORMAT_S16_LE), 0.0, 1e-9);
  CHECK_EQ_DBL(aud_format_peak(buf, 0, 2, AUD_FORMAT_S16_LE), 0.0, 1e-9);
  CHECK_EQ_DBL(aud_format_peak(buf, 1, 1, AUD_FORMAT_UNKNOWN), 0.0, 1e-9);
}

TEST(dbfs_scale)
{
  CHECK_EQ_DBL(aud_format_dbfs(1.0), 0.0, 1e-9);
  CHECK_EQ_DBL(aud_format_dbfs(0.5), -6.0206, 1e-3);
  CHECK_EQ_DBL(aud_format_dbfs(0.0), AUD_DBFS_FLOOR, 1e-9);
  CHECK_EQ_DBL(aud_format_dbfs(1e-30), AUD_DBFS_FLOOR, 1e-9);
}

/*
 * The interleaved decoder is what feeds playback monitoring, so the property
 * that matters is that the channels come back out where they went in - the
 * mono decoder deliberately loses exactly that.
 */
TEST(to_float_keeps_the_channels_apart)
{
  /* two stereo frames: L = +0.5, R = -0.5, then L = 0, R = full negative */
  const unsigned char buf[8] = {0x00, 0x40, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x80};
  float out[4] = {0};

  aud_format_to_float(out, buf, 2, 2, AUD_FORMAT_S16_LE);
  CHECK_EQ_DBL(out[0], 0.5, 1e-6);
  CHECK_EQ_DBL(out[1], -0.5, 1e-6);
  CHECK_EQ_DBL(out[2], 0.0, 1e-6);
  CHECK_EQ_DBL(out[3], -1.0, 1e-6);
}

TEST(to_float_agrees_across_the_24_bit_layouts)
{
  const unsigned char packed[3] = {0x00, 0x00, 0x40};       /* 0x400000 */
  const unsigned char padded[4] = {0x00, 0x00, 0x40, 0x00}; /* same, padded */
  const unsigned char wide[4] = {0x00, 0x00, 0x00, 0x40};   /* 0x40000000 */
  float out = 0.0f;

  aud_format_to_float(&out, packed, 1, 1, AUD_FORMAT_S24_3LE);
  CHECK_EQ_DBL(out, 0.5, 1e-6);

  out = 0.0f;
  aud_format_to_float(&out, padded, 1, 1, AUD_FORMAT_S24_LE);
  CHECK_EQ_DBL(out, 0.5, 1e-6);

  out = 0.0f;
  aud_format_to_float(&out, wide, 1, 1, AUD_FORMAT_S32_LE);
  CHECK_EQ_DBL(out, 0.5, 1e-6);
}

TEST(to_float_averages_nothing_where_to_mono_does)
{
  /* one stereo frame, hard panned: mono halves it, interleaved must not */
  const unsigned char buf[4] = {0x00, 0x40, 0x00, 0x00};
  float stereo[2] = {0};
  float mono = 0.0f;

  aud_format_to_float(stereo, buf, 1, 2, AUD_FORMAT_S16_LE);
  aud_format_to_mono(&mono, buf, 1, 2, AUD_FORMAT_S16_LE);

  CHECK_EQ_DBL(stereo[0], 0.5, 1e-6);
  CHECK_EQ_DBL(stereo[1], 0.0, 1e-6);
  CHECK_EQ_DBL(mono, 0.25, 1e-6);
}

TEST(to_float_edge_cases)
{
  const unsigned char buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  float out[2] = {7.0f, 7.0f};

  /* an unknown format zeroes rather than leaving the caller's buffer stale */
  aud_format_to_float(out, buf, 2, 1, AUD_FORMAT_UNKNOWN);
  CHECK_EQ_DBL(out[0], 0.0, 1e-9);
  CHECK_EQ_DBL(out[1], 0.0, 1e-9);

  out[0] = 7.0f;
  aud_format_to_float(out, NULL, 2, 1, AUD_FORMAT_S16_LE);
  CHECK_EQ_DBL(out[0], 0.0, 1e-9);

  /* nothing to decode leaves the buffer untouched rather than clearing it */
  out[0] = 7.0f;
  aud_format_to_float(out, buf, 0, 1, AUD_FORMAT_S16_LE);
  aud_format_to_float(out, buf, 2, 0, AUD_FORMAT_S16_LE);
  aud_format_to_float(NULL, buf, 2, 1, AUD_FORMAT_S16_LE);
  CHECK_EQ_DBL(out[0], 7.0, 1e-9);
}

int main(void)
{
  RUN(format_sizes);
  RUN(format_names);
  RUN(repack_s24_drops_pad_byte);
  RUN(peak_s16);
  RUN(peak_full_scale_is_clamped);
  RUN(peak_s24_variants_agree);
  RUN(peak_edge_cases);
  RUN(dbfs_scale);
  RUN(to_float_keeps_the_channels_apart);
  RUN(to_float_agrees_across_the_24_bit_layouts);
  RUN(to_float_averages_nothing_where_to_mono_does);
  RUN(to_float_edge_cases);
  return TEST_RESULT();
}
