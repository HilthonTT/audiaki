/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "audio/format.h"
#include "util/bytes.h"

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

TEST(pick_channel_takes_one_of_the_interleave)
{
  /* three frames of stereo s16: left counts up, right counts down */
  const unsigned char src[12] = {0x01, 0x00, 0xF1, 0x00, 0x02, 0x00,
                                 0xF2, 0x00, 0x03, 0x00, 0xF3, 0x00};
  unsigned char out[6];

  memset(out, 0xEE, sizeof(out));
  aud_format_pick_channel(out, src, 3, 2, 0, AUD_FORMAT_S16_LE);
  CHECK_EQ_INT(out[0], 0x01);
  CHECK_EQ_INT(out[2], 0x02);
  CHECK_EQ_INT(out[4], 0x03);

  memset(out, 0xEE, sizeof(out));
  aud_format_pick_channel(out, src, 3, 2, 1, AUD_FORMAT_S16_LE);
  CHECK_EQ_INT(out[0], 0xF1);
  CHECK_EQ_INT(out[2], 0xF2);
  CHECK_EQ_INT(out[4], 0xF3);
}

TEST(pick_channel_carries_the_whole_container)
{
  /* the 4 byte 24 bit container: the pad byte travels with its sample, so a
   * picked period is still exactly what a repack expects to be handed */
  const unsigned char src[16] = {0x11, 0x22, 0x33, 0x00, 0xAA, 0xBB, 0xCC, 0xFF,
                                 0x44, 0x55, 0x66, 0x00, 0xDD, 0xEE, 0x77, 0xFF};
  unsigned char out[8];
  unsigned char packed[6];

  aud_format_pick_channel(out, src, 2, 2, 1, AUD_FORMAT_S24_LE);
  CHECK_EQ_INT(out[0], 0xAA);
  CHECK_EQ_INT(out[1], 0xBB);
  CHECK_EQ_INT(out[2], 0xCC);
  CHECK_EQ_INT(out[4], 0xDD);
  CHECK_EQ_INT(out[6], 0x77);

  /* and the pair still composes the way the recorder relies on */
  aud_format_repack(packed, out, 2, AUD_FORMAT_S24_LE);
  CHECK_EQ_INT(packed[0], 0xAA);
  CHECK_EQ_INT(packed[2], 0xCC);
  CHECK_EQ_INT(packed[3], 0xDD);
  CHECK_EQ_INT(packed[5], 0x77);
}

TEST(pick_channel_agrees_with_the_float_decoder)
{
  /* whichever way a channel is taken out, it has to hold the same level */
  const unsigned char src[24] = {0x00, 0x40, 0x00, 0x10, 0x00, 0x80, 0x00, 0x20,
                                 0x00, 0xC0, 0x00, 0x30, 0x00, 0x00, 0x00, 0x40,
                                 0x00, 0x50, 0x00, 0x60, 0x00, 0x70, 0x00, 0x08};
  unsigned char one[12];
  float split[12];
  float picked[6];

  aud_format_to_float(split, src, 6, 2, AUD_FORMAT_S16_LE);
  aud_format_pick_channel(one, src, 6, 2, 1, AUD_FORMAT_S16_LE);
  aud_format_to_float(picked, one, 6, 1, AUD_FORMAT_S16_LE);

  for (size_t i = 0; i < 6; i++)
  {
    CHECK_EQ_DBL(picked[i], split[i * 2 + 1], 1e-9);
  }

  /*
   * And the peak of the picked channel is that channel's own. This is the
   * reason the meter reads the picked buffer: the left channel here hits full
   * scale and the right reaches 24576/32768, so metering the pair would report
   * clipping on a take that never comes close to it.
   */
  CHECK_EQ_DBL(aud_format_peak(one, 6, 1, AUD_FORMAT_S16_LE), 0.75, 1e-6);
  CHECK_EQ_DBL(aud_format_peak(src, 6, 2, AUD_FORMAT_S16_LE), 1.0, 1e-6);
}

TEST(pick_channel_edge_cases)
{
  const unsigned char src[4] = {0x11, 0x22, 0x33, 0x44};
  unsigned char out[4];

  /* out of range writes nothing rather than reading past the frame */
  memset(out, 0xEE, sizeof(out));
  aud_format_pick_channel(out, src, 2, 2, 2, AUD_FORMAT_S16_LE);
  CHECK_EQ_INT(out[0], 0xEE);

  memset(out, 0xEE, sizeof(out));
  aud_format_pick_channel(out, src, 2, 0, 0, AUD_FORMAT_S16_LE);
  CHECK_EQ_INT(out[0], 0xEE);

  memset(out, 0xEE, sizeof(out));
  aud_format_pick_channel(out, src, 2, 2, 0, AUD_FORMAT_UNKNOWN);
  CHECK_EQ_INT(out[0], 0xEE);

  /* mono in, mono out: the copy is the identity */
  aud_format_pick_channel(out, src, 2, 1, 0, AUD_FORMAT_S16_LE);
  CHECK_EQ_INT(out[0], 0x11);
  CHECK_EQ_INT(out[3], 0x44);

  /* NULLs are survivable */
  aud_format_pick_channel(NULL, src, 2, 2, 0, AUD_FORMAT_S16_LE);
  aud_format_pick_channel(out, NULL, 2, 2, 0, AUD_FORMAT_S16_LE);
  aud_format_pick_channel(out, src, 0, 2, 0, AUD_FORMAT_S16_LE);
}

TEST(mix_channels_averages_the_interleave)
{
  /* three frames of stereo s16: left at +0.5, right at -0.25 */
  const unsigned char src[12] = {0x00, 0x40, 0x00, 0xE0, 0x00, 0x40,
                                 0x00, 0xE0, 0x00, 0x40, 0x00, 0xE0};
  unsigned char out[6];
  float mixed[3];

  aud_format_mix_channels(out, src, 3, 2, AUD_FORMAT_S16_LE);
  aud_format_to_float(mixed, out, 3, 1, AUD_FORMAT_S16_LE);

  /* (0.5 + -0.25) / 2 */
  for (size_t i = 0; i < 3; i++)
  {
    CHECK_EQ_DBL(mixed[i], 0.125, 1e-4);
  }
}

TEST(mix_channels_agrees_with_the_mono_decoder)
{
  /*
   * aud_format_to_mono() already averages, for the spectrum. Mixing down for
   * the file has to reach the same numbers, or a take would not sound like the
   * meter that watched it being made.
   */
  const unsigned char src[24] = {0x00, 0x40, 0x00, 0x10, 0x00, 0x80, 0x00, 0x20,
                                 0x00, 0xC0, 0x00, 0x30, 0x00, 0x00, 0x00, 0x40,
                                 0x00, 0x50, 0x00, 0x60, 0x00, 0x70, 0x00, 0x08};
  unsigned char one[12];
  float direct[6];
  float through_mix[6];

  aud_format_to_mono(direct, src, 6, 2, AUD_FORMAT_S16_LE);
  aud_format_mix_channels(one, src, 6, 2, AUD_FORMAT_S16_LE);
  aud_format_to_float(through_mix, one, 6, 1, AUD_FORMAT_S16_LE);

  /* one LSB of slack: the mix rounds in the integer domain, to_mono does not */
  for (size_t i = 0; i < 6; i++)
  {
    CHECK_EQ_DBL(through_mix[i], direct[i], 1.0 / 32768.0);
  }
}

TEST(a_mixdown_cannot_clip)
{
  /* every channel pinned to full scale, which a summed mix would overflow */
  const unsigned char src[16] = {0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80,
                                 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F};
  unsigned char out[4];

  aud_format_mix_channels(out, src, 2, 4, AUD_FORMAT_S16_LE);

  /* the mean of a set is never further out than its furthest member */
  CHECK_EQ_DBL(aud_format_peak(out, 2, 1, AUD_FORMAT_S16_LE), 1.0, 1e-6);
  CHECK_EQ_INT(aud_rd_s16le(out), -32768);
  CHECK_EQ_INT(aud_rd_s16le(out + 2), 32767);
}

TEST(mix_channels_keeps_the_24_bit_container)
{
  /*
   * S24_LE goes out in its four byte container, because the recorder hands the
   * result to the repack step exactly as it hands over a picked channel.
   */
  const unsigned char src[8] = {0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00};
  unsigned char out[4];
  unsigned char packed[3];

  aud_format_mix_channels(out, src, 1, 2, AUD_FORMAT_S24_LE);
  /* half of 0x400000 and half of zero */
  CHECK_EQ_INT(aud_rd_s24le(out), 0x200000);

  aud_format_repack(packed, out, 1, AUD_FORMAT_S24_LE);
  CHECK_EQ_INT(aud_rd_s24le(packed), 0x200000);
}

TEST(mix_channels_edge_cases)
{
  const unsigned char src[4] = {0x11, 0x22, 0x33, 0x44};
  unsigned char out[4];

  /* mono in, mono out: already its own mix, so this is a copy */
  memset(out, 0xEE, sizeof(out));
  aud_format_mix_channels(out, src, 2, 1, AUD_FORMAT_S16_LE);
  CHECK_EQ_INT(out[0], 0x11);
  CHECK_EQ_INT(out[3], 0x44);

  memset(out, 0xEE, sizeof(out));
  aud_format_mix_channels(out, src, 2, 0, AUD_FORMAT_S16_LE);
  CHECK_EQ_INT(out[0], 0xEE);

  memset(out, 0xEE, sizeof(out));
  aud_format_mix_channels(out, src, 2, 2, AUD_FORMAT_UNKNOWN);
  CHECK_EQ_INT(out[0], 0xEE);

  /* NULLs are survivable */
  aud_format_mix_channels(NULL, src, 2, 2, AUD_FORMAT_S16_LE);
  aud_format_mix_channels(out, NULL, 2, 2, AUD_FORMAT_S16_LE);
  aud_format_mix_channels(out, src, 0, 2, AUD_FORMAT_S16_LE);
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
  RUN(pick_channel_takes_one_of_the_interleave);
  RUN(pick_channel_carries_the_whole_container);
  RUN(pick_channel_agrees_with_the_float_decoder);
  RUN(pick_channel_edge_cases);
  RUN(mix_channels_averages_the_interleave);
  RUN(mix_channels_agrees_with_the_mono_decoder);
  RUN(a_mixdown_cannot_clip);
  RUN(mix_channels_keeps_the_24_bit_container);
  RUN(mix_channels_edge_cases);
  return TEST_RESULT();
}
