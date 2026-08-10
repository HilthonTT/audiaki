/* SPDX-License-Identifier: MIT */
/*
 * These are the tests that stand in for a big-endian machine.
 *
 * Every case below starts from an array of bytes and states what value those
 * bytes mean. Nothing in the helpers looks at a host integer's layout - they
 * index the array - so a run that passes here passes anywhere, and the same is
 * true of the WAV reader and the format decoders that are built on them.
 *
 * That is a proof about the source, not a run on big-endian hardware. Actually
 * running it there still wants a machine or an emulator that CI does not have.
 */
#include "test_util.h"

#include "util/bytes.h"

TEST(unsigned_reads_take_the_low_byte_first)
{
  static const unsigned char p[8] = {0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x89};

  CHECK_EQ_INT(aud_rd_u16le(p), 0x5678);
  CHECK_EQ_INT(aud_rd_u32le(p), 0x12345678u);
  CHECK(aud_rd_u64le(p) == 0x89ABCDEF12345678ull);
}

TEST(signed_reads_sign_extend)
{
  static const unsigned char zero[4] = {0x00, 0x00, 0x00, 0x00};
  static const unsigned char minus_one[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  static const unsigned char one[4] = {0x01, 0x00, 0x00, 0x00};

  CHECK_EQ_INT(aud_rd_s16le(zero), 0);
  CHECK_EQ_INT(aud_rd_s16le(minus_one), -1);
  CHECK_EQ_INT(aud_rd_s16le(one), 1);

  CHECK_EQ_INT(aud_rd_s24le(zero), 0);
  CHECK_EQ_INT(aud_rd_s24le(minus_one), -1);
  CHECK_EQ_INT(aud_rd_s24le(one), 1);

  CHECK_EQ_INT(aud_rd_s32le(zero), 0);
  CHECK_EQ_INT(aud_rd_s32le(minus_one), -1);
  CHECK_EQ_INT(aud_rd_s32le(one), 1);
}

TEST(signed_reads_reach_both_extremes)
{
  /* full scale each way, which is where a sloppy sign extension goes wrong */
  static const unsigned char s16_min[2] = {0x00, 0x80};
  static const unsigned char s16_max[2] = {0xFF, 0x7F};
  static const unsigned char s24_min[3] = {0x00, 0x00, 0x80};
  static const unsigned char s24_max[3] = {0xFF, 0xFF, 0x7F};
  static const unsigned char s32_min[4] = {0x00, 0x00, 0x00, 0x80};
  static const unsigned char s32_max[4] = {0xFF, 0xFF, 0xFF, 0x7F};

  CHECK_EQ_INT(aud_rd_s16le(s16_min), -32768);
  CHECK_EQ_INT(aud_rd_s16le(s16_max), 32767);
  CHECK_EQ_INT(aud_rd_s24le(s24_min), -8388608);
  CHECK_EQ_INT(aud_rd_s24le(s24_max), 8388607);
  CHECK_EQ_INT(aud_rd_s32le(s32_min), -2147483647 - 1);
  CHECK_EQ_INT(aud_rd_s32le(s32_max), 2147483647);
}

TEST(the_24_bit_read_ignores_a_padding_byte)
{
  /*
   * S24_LE arrives in a four byte container. The decoders call the same three
   * byte read on it, so whatever the device left in the fourth byte must not
   * reach the sample.
   */
  static const unsigned char padded[4] = {0xFF, 0xFF, 0x7F, 0xAA};

  CHECK_EQ_INT(aud_rd_s24le(padded), 8388607);
}

TEST(float_reads_rebuild_the_bit_pattern)
{
  /* 1.0f is 0x3F800000, and -2.0 is 0xC000000000000000 */
  static const unsigned char one_f[4] = {0x00, 0x00, 0x80, 0x3F};
  static const unsigned char half_f[4] = {0x00, 0x00, 0x00, 0x3F};
  static const unsigned char minus_two_d[8] = {0x00, 0x00, 0x00, 0x00,
                                               0x00, 0x00, 0x00, 0xC0};

  CHECK_EQ_DBL(aud_rd_f32le(one_f), 1.0, 0.0);
  CHECK_EQ_DBL(aud_rd_f32le(half_f), 0.5, 0.0);
  CHECK_EQ_DBL(aud_rd_f64le(minus_two_d), -2.0, 0.0);
}

TEST(writes_put_the_low_byte_first)
{
  unsigned char p[4];

  aud_wr_u16le(p, 0x1234);
  CHECK_EQ_INT(p[0], 0x34);
  CHECK_EQ_INT(p[1], 0x12);

  aud_wr_u32le(p, 0x89ABCDEFu);
  CHECK_EQ_INT(p[0], 0xEF);
  CHECK_EQ_INT(p[1], 0xCD);
  CHECK_EQ_INT(p[2], 0xAB);
  CHECK_EQ_INT(p[3], 0x89);
}

TEST(a_write_read_round_trip_returns_what_went_in)
{
  unsigned char p[4];

  for (uint32_t v = 0; v < 0x10000u; v += 0x0101u)
  {
    aud_wr_u16le(p, (uint16_t)v);
    CHECK_EQ_INT(aud_rd_u16le(p), (uint16_t)v);
  }

  aud_wr_u32le(p, 0xFFFFFFFFu);
  CHECK_EQ_INT(aud_rd_u32le(p), 0xFFFFFFFFu);
  CHECK_EQ_INT(aud_rd_s32le(p), -1);
}

int main(void)
{
  RUN(unsigned_reads_take_the_low_byte_first);
  RUN(signed_reads_sign_extend);
  RUN(signed_reads_reach_both_extremes);
  RUN(the_24_bit_read_ignores_a_padding_byte);
  RUN(float_reads_rebuild_the_bit_pattern);
  RUN(writes_put_the_low_byte_first);
  RUN(a_write_read_round_trip_returns_what_went_in);

  return TEST_RESULT();
}
