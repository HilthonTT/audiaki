/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "jsonout.h"

#include <stdio.h>

/* Render one value into `buf` so it can be compared as a string. */
static void render_string(char *buf, size_t size, const char *s)
{
  FILE *f = fmemopen(buf, size, "w");

  CHECK(f != NULL);
  if (f == NULL)
  {
    return;
  }
  aud_json_string(f, s);
  fclose(f);
}

static void render_number(char *buf, size_t size, double v, int decimals)
{
  FILE *f = fmemopen(buf, size, "w");

  CHECK(f != NULL);
  if (f == NULL)
  {
    return;
  }
  aud_json_number(f, v, decimals);
  fclose(f);
}

TEST(strings_are_quoted)
{
  char buf[64];

  render_string(buf, sizeof(buf), "hw:CARD=Box,DEV=0");
  CHECK_EQ_STR(buf, "\"hw:CARD=Box,DEV=0\"");

  render_string(buf, sizeof(buf), "");
  CHECK_EQ_STR(buf, "\"\"");
}

TEST(strings_escape_what_the_grammar_requires)
{
  char buf[64];

  render_string(buf, sizeof(buf), "a\"b");
  CHECK_EQ_STR(buf, "\"a\\\"b\"");

  render_string(buf, sizeof(buf), "C:\\takes");
  CHECK_EQ_STR(buf, "\"C:\\\\takes\"");

  render_string(buf, sizeof(buf), "one\ntwo\tthree");
  CHECK_EQ_STR(buf, "\"one\\ntwo\\tthree\"");

  render_string(buf, sizeof(buf), "\b\f\r");
  CHECK_EQ_STR(buf, "\"\\b\\f\\r\"");

  /* a control character with no short escape spells itself out */
  render_string(buf, sizeof(buf), "a\x01z");
  CHECK_EQ_STR(buf, "\"a\\u0001z\"");
}

TEST(strings_leave_utf8_and_slashes_alone)
{
  char buf[64];

  /* escaping the solidus is legal but only makes a path harder to read */
  render_string(buf, sizeof(buf), "takes/riff.wav");
  CHECK_EQ_STR(buf, "\"takes/riff.wav\"");

  /* a device name is whatever the driver says it is, and may not be ASCII */
  render_string(buf, sizeof(buf), "Ka\xc3\xb6rt");
  CHECK_EQ_STR(buf, "\"Ka\xc3\xb6rt\"");
}

TEST(a_null_string_is_the_null_literal)
{
  char buf[16];

  render_string(buf, sizeof(buf), NULL);
  CHECK_EQ_STR(buf, "null");
}

TEST(numbers_keep_the_requested_precision)
{
  char buf[32];

  render_number(buf, sizeof(buf), -8.4213, 2);
  CHECK_EQ_STR(buf, "-8.42");

  render_number(buf, sizeof(buf), 12.0, 3);
  CHECK_EQ_STR(buf, "12.000");

  render_number(buf, sizeof(buf), 7.0, 0);
  CHECK_EQ_STR(buf, "7");
}

TEST(numbers_json_cannot_spell_become_null)
{
  char buf[32];

  /* a bare NaN or Infinity token would make a strict parser reject the lot */
  render_number(buf, sizeof(buf), 0.0 / 0.0, 2);
  CHECK_EQ_STR(buf, "null");

  render_number(buf, sizeof(buf), 1.0 / 0.0, 2);
  CHECK_EQ_STR(buf, "null");

  render_number(buf, sizeof(buf), -1.0 / 0.0, 2);
  CHECK_EQ_STR(buf, "null");
}

int main(void)
{
  RUN(strings_are_quoted);
  RUN(strings_escape_what_the_grammar_requires);
  RUN(strings_leave_utf8_and_slashes_alone);
  RUN(a_null_string_is_the_null_literal);
  RUN(numbers_keep_the_requested_precision);
  RUN(numbers_json_cannot_spell_become_null);

  return TEST_RESULT();
}
