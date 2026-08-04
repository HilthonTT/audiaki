/* SPDX-License-Identifier: MIT */
/*
 * test_util.h - a deliberately tiny assertion helper.
 *
 * Each test binary defines TEST(name) functions, registers them in main() with
 * RUN(), and returns TEST_RESULT(). No framework, no dependencies.
 */
#ifndef AUDIAKI_TEST_UTIL_H
#define AUDIAKI_TEST_UTIL_H

#include <math.h>
#include <stdio.h>
#include <string.h>

static int t_failures;
static int t_checks;
static const char *t_current;

#define TEST(name) static void name(void)

#define RUN(fn)               \
  do                          \
  {                           \
    t_current = #fn;          \
    fn();                     \
    printf("  ok %s\n", #fn); \
  } while (0)

#define CHECK(cond)                                                             \
  do                                                                            \
  {                                                                             \
    t_checks++;                                                                 \
    if (!(cond))                                                                \
    {                                                                           \
      t_failures++;                                                             \
      printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, t_current, #cond); \
    }                                                                           \
  } while (0)

#define CHECK_EQ_INT(actual, expected)                                              \
  do                                                                                \
  {                                                                                 \
    long long a_ = (long long)(actual);                                             \
    long long e_ = (long long)(expected);                                           \
    t_checks++;                                                                     \
    if (a_ != e_)                                                                   \
    {                                                                               \
      t_failures++;                                                                 \
      printf("  FAIL %s:%d in %s: %s == %lld, expected %lld\n", __FILE__, __LINE__, \
             t_current, #actual, a_, e_);                                           \
    }                                                                               \
  } while (0)

#define CHECK_EQ_DBL(actual, expected, tolerance)                               \
  do                                                                            \
  {                                                                             \
    double a_ = (double)(actual);                                               \
    double e_ = (double)(expected);                                             \
    t_checks++;                                                                 \
    if (!(fabs(a_ - e_) <= (tolerance)))                                        \
    {                                                                           \
      t_failures++;                                                             \
      printf("  FAIL %s:%d in %s: %s == %g, expected %g\n", __FILE__, __LINE__, \
             t_current, #actual, a_, e_);                                       \
    }                                                                           \
  } while (0)

#define CHECK_EQ_STR(actual, expected)                                                  \
  do                                                                                    \
  {                                                                                     \
    const char *a_ = (actual);                                                          \
    const char *e_ = (expected);                                                        \
    t_checks++;                                                                         \
    if (a_ == NULL || e_ == NULL || strcmp(a_, e_) != 0)                                \
    {                                                                                   \
      t_failures++;                                                                     \
      printf("  FAIL %s:%d in %s: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, \
             t_current, #actual, a_ ? a_ : "(null)", e_ ? e_ : "(null)");               \
    }                                                                                   \
  } while (0)

#define TEST_RESULT() \
  (printf("%d check(s), %d failure(s)\n", t_checks, t_failures), t_failures == 0 ? 0 : 1)

#endif /* AUDIAKI_TEST_UTIL_H */
