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

int main(void)
{
  RUN(uint_accepts_plain_decimals);
  RUN(uint_rejects_garbage);
  RUN(uint_enforces_range);
  RUN(duration_seconds);
  RUN(duration_clock_notation);
  RUN(duration_rejects_garbage);
  return TEST_RESULT();
}
