/* SPDX-License-Identifier: MIT */
#include "fuzz.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Two files, made once and rewritten in place: the input, and somewhere a
 * target may write its own output without landing on the input it is still
 * holding. A target runs millions of times, and creating and unlinking a file
 * each time would be most of what gets measured.
 */
typedef struct
{
  char path[256];
  int fd;
} fuzz_slot;

static fuzz_slot g_in;
static fuzz_slot g_out;

static void close_slot(fuzz_slot *s)
{
  if (s->fd >= 0)
  {
    close(s->fd);
    s->fd = -1;
  }
  if (s->path[0] != '\0')
  {
    unlink(s->path);
    s->path[0] = '\0';
  }
}

static void cleanup(void)
{
  close_slot(&g_in);
  close_slot(&g_out);
}

/*
 * The process id is in the name because libFuzzer forks workers, and two of
 * them sharing one file would have each fuzzing the other's input. Not
 * mkstemps(): that is a BSD extension rather than POSIX, and the suffix has to
 * survive - aud_project_is_project() reads the extension, and a project loader
 * handed a file called .wav is being asked a different question.
 */
static int open_slot(fuzz_slot *s, const char *what, const char *suffix)
{
  const char *dir;

  if (s->fd >= 0)
  {
    return 0;
  }

  dir = getenv("TMPDIR");
  if (dir == NULL || *dir == '\0')
  {
    dir = "/tmp";
  }

  if ((size_t)snprintf(s->path, sizeof(s->path), "%s/audiaki-fuzz-%ld-%s%s", dir,
                       (long)getpid(), what, suffix) >= sizeof(s->path))
  {
    s->path[0] = '\0';
    return -1;
  }

  s->fd = open(s->path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (s->fd < 0)
  {
    s->path[0] = '\0';
    return -1;
  }

  atexit(cleanup);
  return 0;
}

const char *fuzz_file(const uint8_t *data, size_t size, const char *suffix)
{
  ssize_t written;

  if (open_slot(&g_in, "in", suffix) != 0 || ftruncate(g_in.fd, 0) != 0)
  {
    return NULL;
  }

  written = size > 0 ? pwrite(g_in.fd, data, size, 0) : 0;
  if (written < 0 || (size_t)written != size)
  {
    return NULL;
  }

  return g_in.path;
}

const char *fuzz_scratch(const char *suffix)
{
  if (open_slot(&g_out, "out", suffix) != 0 || ftruncate(g_out.fd, 0) != 0)
  {
    return NULL;
  }
  return g_out.path;
}
