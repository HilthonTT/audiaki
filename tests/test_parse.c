/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "parse.h"

#include <limits.h>

TEST(uint_accepts_plain_decimals)
{
  unsigned value = 0;

  CHECK_EQ_INT(parse_uint("0", 0, 100, &value), 0);
  CHECK_EQ_INT(value, 0);

  CHECK_EQ_INT(parse_uint("44100", 1, 768000, &value), 0);
  CHECK_EQ_INT(value, 44100);

  CHECK_EQ_INT(parse_uint("007", 0, 100, &value), 0);
  CHECK_EQ_INT(value, 7);
}

TEST(uint_rejects_garbage)
{
  unsigned value = 123;

  CHECK_EQ_INT(parse_uint("44100abc", 1, 768000, &value), -1);
  CHECK_EQ_INT(parse_uint("", 1, 10, &value), -1);
  CHECK_EQ_INT(parse_uint(" 42", 1, 100, &value), -1);
  CHECK_EQ_INT(parse_uint("0x10", 1, 100, &value), -1);
  CHECK_EQ_INT(parse_uint("4.5", 1, 100, &value), -1);
  CHECK_EQ_INT(parse_uint(NULL, 1, 100, &value), -1);
  /* strtoul would wrap this into ULONG_MAX */
  CHECK_EQ_INT(parse_uint("-1", 0, 100, &value), -1);
  CHECK_EQ_INT(parse_uint("99999999999999999999", 0, UINT_MAX, &value), -1);
  /* a rejected parse must not disturb the caller's value */
  CHECK_EQ_INT(value, 123);
}

TEST(uint_enforces_range)
{
  unsigned value = 0;

  CHECK_EQ_INT(parse_uint("0", 1, 64, &value), -1);
  CHECK_EQ_INT(parse_uint("65", 1, 64, &value), -1);
  CHECK_EQ_INT(parse_uint("64", 1, 64, &value), 0);
  CHECK_EQ_INT(value, 64);
}

TEST(duration_seconds)
{
  double seconds = -1.0;

  CHECK_EQ_INT(parse_duration("30", &seconds), 0);
  CHECK_EQ_DBL(seconds, 30.0, 1e-9);

  CHECK_EQ_INT(parse_duration("12.5", &seconds), 0);
  CHECK_EQ_DBL(seconds, 12.5, 1e-9);

  CHECK_EQ_INT(parse_duration("0", &seconds), 0);
  CHECK_EQ_DBL(seconds, 0.0, 1e-9);
}

TEST(duration_clock_notation)
{
  double seconds = 0.0;

  CHECK_EQ_INT(parse_duration("1:30", &seconds), 0);
  CHECK_EQ_DBL(seconds, 90.0, 1e-9);

  CHECK_EQ_INT(parse_duration("0:05.5", &seconds), 0);
  CHECK_EQ_DBL(seconds, 5.5, 1e-9);

  CHECK_EQ_INT(parse_duration("1:02:03", &seconds), 0);
  CHECK_EQ_DBL(seconds, 3723.0, 1e-9);

  /* the leading field may exceed 59 */
  CHECK_EQ_INT(parse_duration("90:00", &seconds), 0);
  CHECK_EQ_DBL(seconds, 5400.0, 1e-9);
}

TEST(duration_rejects_garbage)
{
  double seconds = 7.0;

  CHECK_EQ_INT(parse_duration("", &seconds), -1);
  CHECK_EQ_INT(parse_duration("abc", &seconds), -1);
  CHECK_EQ_INT(parse_duration("1:", &seconds), -1);
  CHECK_EQ_INT(parse_duration(":30", &seconds), -1);
  CHECK_EQ_INT(parse_duration("1:2:3:4", &seconds), -1);
  CHECK_EQ_INT(parse_duration("1:90", &seconds), -1); /* 90 seconds is a typo */
  CHECK_EQ_INT(parse_duration("-5", &seconds), -1);
  CHECK_EQ_INT(parse_duration("30s", &seconds), -1);
  CHECK_EQ_INT(parse_duration(NULL, &seconds), -1);
  CHECK_EQ_DBL(seconds, 7.0, 1e-9);
}

TEST(size_dimensions)
{
  unsigned w = 0;
  unsigned h = 0;

  CHECK_EQ_INT(parse_size("1280x720", 64, 7680, &w, &h), 0);
  CHECK_EQ_INT(w, 1280);
  CHECK_EQ_INT(h, 720);

  CHECK_EQ_INT(parse_size("640X480", 64, 7680, &w, &h), 0);
  CHECK_EQ_INT(w, 640);
  CHECK_EQ_INT(h, 480);

  CHECK_EQ_INT(parse_size("100x100", 64, 7680, &w, &h), 0);
  CHECK_EQ_INT(w, 100);
  CHECK_EQ_INT(h, 100);
}

TEST(size_shorthand)
{
  unsigned w = 0;
  unsigned h = 0;

  CHECK_EQ_INT(parse_size("720p", 64, 7680, &w, &h), 0);
  CHECK_EQ_INT(w, 1280);
  CHECK_EQ_INT(h, 720);

  CHECK_EQ_INT(parse_size("1080p", 64, 7680, &w, &h), 0);
  CHECK_EQ_INT(w, 1920);
  CHECK_EQ_INT(h, 1080);

  CHECK_EQ_INT(parse_size("2160p", 64, 7680, &w, &h), 0);
  CHECK_EQ_INT(w, 3840);
  CHECK_EQ_INT(h, 2160);

  /* every shorthand is 16:9 with even dimensions, which libx264 requires */
  const char *names[] = {"480p", "720p", "1080p", "1440p", "2160p"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
  {
    CHECK_EQ_INT(parse_size(names[i], 64, 7680, &w, &h), 0);
    CHECK_EQ_INT(w % 2, 0);
    CHECK_EQ_INT(h % 2, 0);
  }

  /* shorthand still has to fit the caller's bounds */
  CHECK_EQ_INT(parse_size("2160p", 64, 1080, &w, &h), -1);
}

TEST(size_rejects_garbage)
{
  unsigned w = 7;
  unsigned h = 7;

  CHECK_EQ_INT(parse_size("", 64, 7680, &w, &h), -1);
  CHECK_EQ_INT(parse_size("1280", 64, 7680, &w, &h), -1);
  CHECK_EQ_INT(parse_size("x720", 64, 7680, &w, &h), -1);
  CHECK_EQ_INT(parse_size("1280x", 64, 7680, &w, &h), -1);
  CHECK_EQ_INT(parse_size("1280x720x30", 64, 7680, &w, &h), -1);
  CHECK_EQ_INT(parse_size("1280 x 720", 64, 7680, &w, &h), -1);
  CHECK_EQ_INT(parse_size("-8x-8", 64, 7680, &w, &h), -1);
  CHECK_EQ_INT(parse_size("32x32", 64, 7680, &w, &h), -1);     /* below min */
  CHECK_EQ_INT(parse_size("9000x9000", 64, 7680, &w, &h), -1); /* above max */
  CHECK_EQ_INT(parse_size("1080P", 64, 7680, &w, &h), -1);     /* case matters */
  CHECK_EQ_INT(parse_size(NULL, 64, 7680, &w, &h), -1);

  /* nothing was written on any of those */
  CHECK_EQ_INT(w, 7);
  CHECK_EQ_INT(h, 7);
}

int main(void)
{
  RUN(uint_accepts_plain_decimals);
  RUN(uint_rejects_garbage);
  RUN(uint_enforces_range);
  RUN(duration_seconds);
  RUN(duration_clock_notation);
  RUN(duration_rejects_garbage);
  RUN(size_dimensions);
  RUN(size_shorthand);
  RUN(size_rejects_garbage);
  return TEST_RESULT();
}
