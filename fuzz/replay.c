/* SPDX-License-Identifier: MIT */
/*
 * replay.c - a main() for a fuzz target, without libFuzzer.
 *
 * Two jobs for the corpus, and only one of them needs clang. Finding new inputs
 * does; running the ones already found does not, and that is what belongs in
 * CI on every change: a crash that was found once and fixed should be a test
 * from then on, checked by whatever compiler is to hand, rather than something
 * only rediscovered by a fuzzer that happens to be run again.
 *
 * So each target builds twice - once against libFuzzer to go looking, and once
 * against this to replay what was found. Every file named on the command line
 * is one input.
 */
#include "fuzz.h"

#include <stdio.h>
#include <stdlib.h>

/* Larger than any corpus entry has cause to be; a bigger file is truncated. */
#define REPLAY_MAX (1u << 20)

static int replay(const char *path)
{
  static unsigned char buf[REPLAY_MAX];
  FILE *f = fopen(path, "rb");
  size_t got;

  if (f == NULL)
  {
    fprintf(stderr, "replay: cannot open %s\n", path);
    return -1;
  }

  got = fread(buf, 1, sizeof(buf), f);
  fclose(f);

  LLVMFuzzerTestOneInput(buf, got);
  return 0;
}

int main(int argc, char **argv)
{
  int failures = 0;

  if (argc < 2)
  {
    fprintf(stderr, "usage: %s <input> [more ...]\n", argv[0]);
    return 2;
  }

  for (int i = 1; i < argc; i++)
  {
    failures += replay(argv[i]) != 0;
  }

  printf("%d input(s) replayed, %d unreadable\n", argc - 1, failures);
  return failures == 0 ? 0 : 1;
}
