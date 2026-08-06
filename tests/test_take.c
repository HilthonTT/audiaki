/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "take.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char g_prefix[256];

TEST(numbers_are_padded_to_three_digits)
{
  char path[64];

  CHECK_EQ_INT(aud_take_path(path, sizeof(path), "session", 1), 0);
  CHECK_EQ_STR(path, "session-001.wav");

  CHECK_EQ_INT(aud_take_path(path, sizeof(path), "session", 42), 0);
  CHECK_EQ_STR(path, "session-042.wav");

  /* past three digits the number simply gets longer rather than being refused */
  CHECK_EQ_INT(aud_take_path(path, sizeof(path), "session", 1234), 0);
  CHECK_EQ_STR(path, "session-1234.wav");
}

TEST(an_extension_on_the_prefix_is_kept)
{
  char path[64];

  /* typing the name of the file you expect should do the obvious thing */
  CHECK_EQ_INT(aud_take_path(path, sizeof(path), "session.wav", 3), 0);
  CHECK_EQ_STR(path, "session-003.wav");

  CHECK_EQ_INT(aud_take_path(path, sizeof(path), "riff.WAV", 3), 0);
  CHECK_EQ_STR(path, "riff-003.WAV");

  CHECK_EQ_INT(aud_take_path(path, sizeof(path), "take.flac", 3), 0);
  CHECK_EQ_STR(path, "take-003.flac");
}

TEST(directories_are_not_mistaken_for_extensions)
{
  char path[64];

  CHECK_EQ_INT(aud_take_path(path, sizeof(path), "takes/session", 7), 0);
  CHECK_EQ_STR(path, "takes/session-007.wav");

  /* a dot in a directory name is not an extension of the basename */
  CHECK_EQ_INT(aud_take_path(path, sizeof(path), "v1.2/session", 7), 0);
  CHECK_EQ_STR(path, "v1.2/session-007.wav");

  /* a leading dot is a hidden file, not an empty name with an extension */
  CHECK_EQ_INT(aud_take_path(path, sizeof(path), ".session", 7), 0);
  CHECK_EQ_STR(path, ".session-007.wav");
}

TEST(unusable_prefixes_are_refused)
{
  char path[64];

  CHECK_EQ_INT(aud_take_path(path, sizeof(path), NULL, 1), -1);
  CHECK_EQ_INT(aud_take_path(path, sizeof(path), "", 1), -1);
  CHECK_EQ_INT(aud_take_path(NULL, sizeof(path), "session", 1), -1);
  CHECK_EQ_INT(aud_take_path(path, 0, "session", 1), -1);

  CHECK_EQ_INT(aud_take_path(path, sizeof(path), "session", AUD_TAKE_MAX_NUMBER + 1), -1);
}

TEST(a_name_that_does_not_fit_is_refused_not_truncated)
{
  char path[16];

  /* "session-001.wav" is 15 bytes plus a terminator */
  CHECK_EQ_INT(aud_take_path(path, 16, "session", 1), 0);

  /* and a refusal must leave dst alone rather than half a filename in it */
  snprintf(path, sizeof(path), "guard");
  CHECK_EQ_INT(aud_take_path(path, 15, "session", 1), -1);
  CHECK_EQ_STR(path, "guard");
}

TEST(next_skips_the_takes_that_already_exist)
{
  char path[512];
  char expected[512];
  FILE *f;

  /* nothing recorded yet: the first take is 001 */
  CHECK_EQ_INT(aud_take_next(path, sizeof(path), g_prefix), 0);
  CHECK_EQ_INT(aud_take_path(expected, sizeof(expected), g_prefix, 1), 0);
  CHECK_EQ_STR(path, expected);

  f = fopen(path, "wb");
  CHECK(f != NULL);
  if (f != NULL)
  {
    fclose(f);
  }

  /* now it is taken, so the next call moves on rather than clobbering it */
  CHECK_EQ_INT(aud_take_next(path, sizeof(path), g_prefix), 0);
  CHECK_EQ_INT(aud_take_path(expected, sizeof(expected), g_prefix, 2), 0);
  CHECK_EQ_STR(path, expected);

  CHECK_EQ_INT(aud_take_path(expected, sizeof(expected), g_prefix, 1), 0);
  remove(expected);
}

/*
 * Deriving a video name from a take name. Both the CLI's --visualize default
 * and the desktop app's video capture go through this, so the two cannot
 * disagree about what take-003.wav's video is called.
 */
TEST(an_extension_is_swapped_for_the_new_one)
{
  char out[64];

  CHECK_EQ_INT(aud_take_with_extension(out, sizeof(out), "take-003.wav", ".mp4"), 0);
  CHECK_EQ_STR(out, "take-003.mp4");

  CHECK_EQ_INT(aud_take_with_extension(out, sizeof(out), "session.WAV", ".mkv"), 0);
  CHECK_EQ_STR(out, "session.mkv");
}

TEST(a_path_without_an_extension_gains_one)
{
  char out[64];

  CHECK_EQ_INT(aud_take_with_extension(out, sizeof(out), "take", ".mp4"), 0);
  CHECK_EQ_STR(out, "take.mp4");
}

TEST(a_dot_in_a_directory_is_not_an_extension)
{
  char out[64];

  CHECK_EQ_INT(aud_take_with_extension(out, sizeof(out), "my.takes/session", ".mp4"), 0);
  CHECK_EQ_STR(out, "my.takes/session.mp4");

  CHECK_EQ_INT(aud_take_with_extension(out, sizeof(out), "my.takes/a.wav", ".mp4"), 0);
  CHECK_EQ_STR(out, "my.takes/a.mp4");
}

TEST(a_hidden_file_keeps_its_leading_dot)
{
  char out[64];

  /* ".wav" is a hidden file called .wav, not an empty name with an extension */
  CHECK_EQ_INT(aud_take_with_extension(out, sizeof(out), ".wav", ".mp4"), 0);
  CHECK_EQ_STR(out, ".wav.mp4");
}

TEST(an_extension_swap_that_does_not_fit_is_refused)
{
  char out[8] = "guard";

  CHECK_EQ_INT(aud_take_with_extension(out, sizeof(out), "a-long-take.wav", ".mp4"), -1);
  CHECK_EQ_STR(out, "guard");

  CHECK_EQ_INT(aud_take_with_extension(NULL, 16, "take.wav", ".mp4"), -1);
  CHECK_EQ_INT(aud_take_with_extension(out, 0, "take.wav", ".mp4"), -1);
  CHECK_EQ_INT(aud_take_with_extension(out, sizeof(out), NULL, ".mp4"), -1);
  CHECK_EQ_INT(aud_take_with_extension(out, sizeof(out), "take.wav", NULL), -1);
}

int main(void)
{
  snprintf(g_prefix, sizeof(g_prefix), "audiaki-take-%ld", (long)getpid());

  RUN(numbers_are_padded_to_three_digits);
  RUN(an_extension_on_the_prefix_is_kept);
  RUN(directories_are_not_mistaken_for_extensions);
  RUN(unusable_prefixes_are_refused);
  RUN(a_name_that_does_not_fit_is_refused_not_truncated);
  RUN(next_skips_the_takes_that_already_exist);
  RUN(an_extension_is_swapped_for_the_new_one);
  RUN(a_path_without_an_extension_gains_one);
  RUN(a_dot_in_a_directory_is_not_an_extension);
  RUN(a_hidden_file_keeps_its_leading_dot);
  RUN(an_extension_swap_that_does_not_fit_is_refused);

  return TEST_RESULT();
}
