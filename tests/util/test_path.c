/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "util/path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A directory of our own to make and move files in, removed as we go. */
static char g_tmp[256];

static void write_file(const char *path, const char *text)
{
  FILE *f = fopen(path, "wb");

  if (f != NULL)
  {
    fputs(text, f);
    fclose(f);
  }
}

/* The whole of `path`, or "" when it could not be read. */
static const char *slurp(const char *path)
{
  static char buf[256];
  FILE *f = fopen(path, "rb");
  size_t n = 0;

  buf[0] = '\0';
  if (f == NULL)
  {
    return buf;
  }
  n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = '\0';
  fclose(f);
  return buf;
}

TEST(join_puts_one_slash_between)
{
  char out[64];

  CHECK_EQ_INT(aud_path_join(out, sizeof(out), "takes", "a.wav"), 0);
  CHECK(strcmp(out, "takes/a.wav") == 0);

  CHECK_EQ_INT(aud_path_join(out, sizeof(out), "takes/", "a.wav"), 0);
  CHECK(strcmp(out, "takes/a.wav") == 0);

  CHECK_EQ_INT(aud_path_join(out, sizeof(out), "takes///", "a.wav"), 0);
  CHECK(strcmp(out, "takes/a.wav") == 0);

  /* the root is the one directory whose name is already a separator */
  CHECK_EQ_INT(aud_path_join(out, sizeof(out), "/", "a.wav"), 0);
  CHECK(strcmp(out, "/a.wav") == 0);
}

TEST(join_leaves_a_named_place_alone)
{
  char out[64];

  CHECK_EQ_INT(aud_path_join(out, sizeof(out), NULL, "a.wav"), 0);
  CHECK(strcmp(out, "a.wav") == 0);

  CHECK_EQ_INT(aud_path_join(out, sizeof(out), "", "a.wav"), 0);
  CHECK(strcmp(out, "a.wav") == 0);

  /* an absolute name says where it goes; a default folder does not move it */
  CHECK_EQ_INT(aud_path_join(out, sizeof(out), "takes", "/tmp/a.wav"), 0);
  CHECK(strcmp(out, "/tmp/a.wav") == 0);
}

TEST(join_that_would_not_fit_leaves_dst_alone)
{
  char out[8];

  memcpy(out, "KEEPME", 7);
  CHECK_EQ_INT(aud_path_join(out, sizeof(out), "a-very-long-folder", "take.wav"), -1);
  CHECK(strcmp(out, "KEEPME") == 0);
}

TEST(place_defers_to_a_name_with_a_folder_in_it)
{
  char out[64];

  CHECK_EQ_INT(aud_path_place(out, sizeof(out), "/takes", "riff.wav"), 0);
  CHECK(strcmp(out, "/takes/riff.wav") == 0);

  /* 'sub/riff.wav' already said where it goes, relative to here */
  CHECK_EQ_INT(aud_path_place(out, sizeof(out), "/takes", "sub/riff.wav"), 0);
  CHECK(strcmp(out, "sub/riff.wav") == 0);

  CHECK_EQ_INT(aud_path_place(out, sizeof(out), "", "riff.wav"), 0);
  CHECK(strcmp(out, "riff.wav") == 0);
}

TEST(basename_and_dirname_split_at_the_last_slash)
{
  char out[64];

  CHECK(strcmp(aud_path_basename("takes/a/riff.wav"), "riff.wav") == 0);
  CHECK(strcmp(aud_path_basename("riff.wav"), "riff.wav") == 0);
  CHECK(strcmp(aud_path_basename("/riff.wav"), "riff.wav") == 0);

  CHECK_EQ_INT(aud_path_dirname(out, sizeof(out), "takes/a/riff.wav"), 0);
  CHECK(strcmp(out, "takes/a") == 0);

  /* no slash at all means here, and one leading slash means the root */
  CHECK_EQ_INT(aud_path_dirname(out, sizeof(out), "riff.wav"), 0);
  CHECK(strcmp(out, ".") == 0);

  CHECK_EQ_INT(aud_path_dirname(out, sizeof(out), "/riff.wav"), 0);
  CHECK(strcmp(out, "/") == 0);
}

TEST(expand_and_shorten_are_inverses)
{
  char out[128];

  setenv("HOME", "/home/tester", 1);

  CHECK_EQ_INT(aud_path_expand(out, sizeof(out), "~/Takes"), 0);
  CHECK(strcmp(out, "/home/tester/Takes") == 0);

  CHECK_EQ_INT(aud_path_expand(out, sizeof(out), "~"), 0);
  CHECK(strcmp(out, "/home/tester") == 0);

  CHECK_EQ_INT(aud_path_shorten(out, sizeof(out), "/home/tester/Takes"), 0);
  CHECK(strcmp(out, "~/Takes") == 0);

  CHECK_EQ_INT(aud_path_shorten(out, sizeof(out), "/home/tester"), 0);
  CHECK(strcmp(out, "~") == 0);
}

TEST(a_tilde_that_is_not_a_home_directory_is_a_filename)
{
  char out[128];

  setenv("HOME", "/home/tester", 1);

  CHECK_EQ_INT(aud_path_expand(out, sizeof(out), "~other/Takes"), 0);
  CHECK(strcmp(out, "~other/Takes") == 0);

  CHECK_EQ_INT(aud_path_expand(out, sizeof(out), "a~b.wav"), 0);
  CHECK(strcmp(out, "a~b.wav") == 0);

  /* another user's home merely starts with the same letters */
  CHECK_EQ_INT(aud_path_shorten(out, sizeof(out), "/home/tester-old/Takes"), 0);
  CHECK(strcmp(out, "/home/tester-old/Takes") == 0);
}

TEST(mkdirs_builds_the_whole_chain_and_is_happy_twice)
{
  char deep[512];

  snprintf(deep, sizeof(deep), "%s/one/two/three", g_tmp);

  CHECK_EQ_INT(aud_path_mkdirs(deep), 0);
  CHECK(aud_path_is_dir(deep));

  /* already being there is what was asked for, not a failure */
  CHECK_EQ_INT(aud_path_mkdirs(deep), 0);

  rmdir(deep);
}

TEST(mkdirs_refuses_a_path_through_a_file)
{
  char file[512];
  char through[512];

  snprintf(file, sizeof(file), "%s/not-a-folder", g_tmp);
  snprintf(through, sizeof(through), "%s/not-a-folder/takes", g_tmp);
  write_file(file, "x");

  CHECK_EQ_INT(aud_path_mkdirs(through), -1);

  remove(file);
}

TEST(move_takes_the_file_with_it)
{
  char from[512];
  char to[512];

  snprintf(from, sizeof(from), "%s/from.wav", g_tmp);
  snprintf(to, sizeof(to), "%s/to.wav", g_tmp);
  write_file(from, "take");

  CHECK_EQ_INT(aud_path_move(from, to), 0);
  CHECK(access(from, F_OK) != 0);
  CHECK(strcmp(slurp(to), "take") == 0);

  remove(to);
}

TEST(move_never_lands_on_an_existing_take)
{
  char from[512];
  char to[512];

  snprintf(from, sizeof(from), "%s/from.wav", g_tmp);
  snprintf(to, sizeof(to), "%s/to.wav", g_tmp);
  write_file(from, "the new one");
  write_file(to, "yesterday's");

  errno = 0;
  CHECK_EQ_INT(aud_path_move(from, to), -1);
  CHECK_EQ_INT(errno, EEXIST);

  /* both of them exactly as they were: this is the failure that must not lose */
  CHECK(strcmp(slurp(from), "the new one") == 0);
  CHECK(strcmp(slurp(to), "yesterday's") == 0);

  remove(from);
  remove(to);
}

int main(void)
{
  snprintf(g_tmp, sizeof(g_tmp), "audiaki-path-test-%ld", (long)getpid());
  if (mkdir(g_tmp, 0777) != 0)
  {
    printf("cannot create %s\n", g_tmp);
    return 1;
  }

  RUN(join_puts_one_slash_between);
  RUN(join_leaves_a_named_place_alone);
  RUN(join_that_would_not_fit_leaves_dst_alone);
  RUN(place_defers_to_a_name_with_a_folder_in_it);
  RUN(basename_and_dirname_split_at_the_last_slash);
  RUN(expand_and_shorten_are_inverses);
  RUN(a_tilde_that_is_not_a_home_directory_is_a_filename);
  RUN(mkdirs_builds_the_whole_chain_and_is_happy_twice);
  RUN(mkdirs_refuses_a_path_through_a_file);
  RUN(move_takes_the_file_with_it);
  RUN(move_never_lands_on_an_existing_take);

  {
    char nested[512];

    snprintf(nested, sizeof(nested), "%s/one/two", g_tmp);
    rmdir(nested);
    snprintf(nested, sizeof(nested), "%s/one", g_tmp);
    rmdir(nested);
  }
  rmdir(g_tmp);

  return TEST_RESULT();
}
