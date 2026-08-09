/* SPDX-License-Identifier: MIT */
#include "util/path.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* One read/write of a take being copied to another filesystem. */
#define PATH_COPY_CHUNK 65536u

int aud_path_expand(char *dst, size_t size, const char *path)
{
  const char *home;
  int written;

  if (dst == NULL || size == 0 || path == NULL)
  {
    return -1;
  }

  if (path[0] != '~' || (path[1] != '\0' && path[1] != '/'))
  {
    written = snprintf(NULL, 0, "%s", path);
    if (written < 0 || (size_t)written >= size)
    {
      return -1;
    }
    snprintf(dst, size, "%s", path);
    return 0;
  }

  home = getenv("HOME");
  if (home == NULL || *home == '\0')
  {
    return -1;
  }

  return aud_path_join(dst, size, home, path[1] == '/' ? path + 2 : "");
}

int aud_path_shorten(char *dst, size_t size, const char *path)
{
  const char *home = getenv("HOME");
  size_t home_len;
  int written;

  if (dst == NULL || size == 0 || path == NULL)
  {
    return -1;
  }

  if (home == NULL || *home == '\0')
  {
    home_len = 0;
  }
  else
  {
    home_len = strlen(home);
    while (home_len > 1 && home[home_len - 1] == '/')
    {
      home_len--;
    }
  }

  /*
   * The home directory itself, or something inside it. A path that merely
   * starts with the same letters - "/home/anna-old" against "/home/anna" - is
   * somewhere else entirely, which is what the check on the next character is
   * for.
   */
  if (home_len == 0 || strncmp(path, home, home_len) != 0 ||
      (path[home_len] != '\0' && path[home_len] != '/'))
  {
    written = snprintf(NULL, 0, "%s", path);
    if (written < 0 || (size_t)written >= size)
    {
      return -1;
    }
    snprintf(dst, size, "%s", path);
    return 0;
  }

  written = snprintf(NULL, 0, "~%s", path + home_len);
  if (written < 0 || (size_t)written >= size)
  {
    return -1;
  }
  snprintf(dst, size, "~%s", path + home_len);
  return 0;
}

int aud_path_join(char *dst, size_t size, const char *dir, const char *name)
{
  size_t dir_len;
  int written;

  if (dst == NULL || size == 0 || name == NULL)
  {
    return -1;
  }

  if (dir == NULL || *dir == '\0' || *name == '/')
  {
    written = snprintf(NULL, 0, "%s", name);
    if (written < 0 || (size_t)written >= size)
    {
      return -1;
    }
    snprintf(dst, size, "%s", name);
    return 0;
  }

  /* the caller's trailing slash, or the lack of one, is not worth carrying */
  dir_len = strlen(dir);
  while (dir_len > 1 && dir[dir_len - 1] == '/')
  {
    dir_len--;
  }

  /* "/" is the one directory whose name is its own separator */
  if (dir_len == 1 && dir[0] == '/')
  {
    written = snprintf(NULL, 0, "/%s", name);
  }
  else if (*name == '\0')
  {
    written = snprintf(NULL, 0, "%.*s", (int)dir_len, dir);
  }
  else
  {
    written = snprintf(NULL, 0, "%.*s/%s", (int)dir_len, dir, name);
  }

  /* measured before writing, so a name that does not fit leaves dst alone
   * rather than half of one in it - the same rule take.c follows */
  if (written < 0 || (size_t)written >= size)
  {
    return -1;
  }

  if (dir_len == 1 && dir[0] == '/')
  {
    snprintf(dst, size, "/%s", name);
  }
  else if (*name == '\0')
  {
    snprintf(dst, size, "%.*s", (int)dir_len, dir);
  }
  else
  {
    snprintf(dst, size, "%.*s/%s", (int)dir_len, dir, name);
  }
  return 0;
}

int aud_path_place(char *dst, size_t size, const char *dir, const char *name)
{
  if (name == NULL)
  {
    return -1;
  }

  if (strchr(name, '/') != NULL)
  {
    dir = NULL;
  }
  return aud_path_join(dst, size, dir, name);
}

const char *aud_path_basename(const char *path)
{
  const char *slash;

  if (path == NULL)
  {
    return "";
  }

  slash = strrchr(path, '/');
  return slash != NULL ? slash + 1 : path;
}

int aud_path_dirname(char *dst, size_t size, const char *path)
{
  const char *slash;
  size_t len;

  if (dst == NULL || size == 0 || path == NULL)
  {
    return -1;
  }

  slash = strrchr(path, '/');
  if (slash == NULL)
  {
    if (size < 2)
    {
      return -1;
    }
    dst[0] = '.';
    dst[1] = '\0';
    return 0;
  }

  /* "/take.wav" lives in "/", not in the empty string */
  len = (size_t)(slash - path);
  if (len == 0)
  {
    if (size < 2)
    {
      return -1;
    }
    dst[0] = '/';
    dst[1] = '\0';
    return 0;
  }

  if (len >= size)
  {
    return -1;
  }
  memcpy(dst, path, len);
  dst[len] = '\0';
  return 0;
}

int aud_path_is_dir(const char *path)
{
  struct stat st;

  if (path == NULL || *path == '\0')
  {
    return 0;
  }

  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int aud_path_mkdirs(const char *dir)
{
  char work[AUD_PATH_MAX];
  size_t len;

  if (dir == NULL || *dir == '\0')
  {
    errno = EINVAL;
    return -1;
  }

  len = strlen(dir);
  if (len >= sizeof(work))
  {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(work, dir, len + 1);

  /*
   * Each component in turn, terminating the string where the next slash is and
   * putting it back afterwards. An existing component is only an error when it
   * is not a directory - "the folder is already there" is what was asked for.
   */
  for (size_t i = 1; i <= len; i++)
  {
    int last = i == len;

    if (!last && work[i] != '/')
    {
      continue;
    }
    if (work[i] == '/' && work[i - 1] == '/')
    {
      continue; /* a doubled separator names the same directory twice */
    }

    if (!last)
    {
      work[i] = '\0';
    }

    if (mkdir(work, 0777) != 0)
    {
      if (errno != EEXIST)
      {
        return -1;
      }
      if (!aud_path_is_dir(work))
      {
        errno = ENOTDIR;
        return -1;
      }
    }

    if (!last)
    {
      work[i] = '/';
    }
  }

  return 0;
}

/*
 * Copy `src` to `dst`, which is created exclusively so a name that appeared
 * underneath us is refused rather than overwritten. A copy that fails part way
 * takes its own half-written file with it: what is left behind should be the
 * take as it was, not a truncated second one beside it.
 */
static int copy_file(const char *src, const char *dst)
{
  char buf[PATH_COPY_CHUNK];
  int in;
  int out;
  int saved;

  in = open(src, O_RDONLY);
  if (in < 0)
  {
    return -1;
  }

  out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (out < 0)
  {
    saved = errno;
    close(in);
    errno = saved;
    return -1;
  }

  for (;;)
  {
    ssize_t got = read(in, buf, sizeof(buf));
    const char *at = buf;

    if (got == 0)
    {
      break;
    }
    if (got < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      goto failed;
    }

    while (got > 0)
    {
      ssize_t put = write(out, at, (size_t)got);

      if (put < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }
        goto failed;
      }
      at += put;
      got -= put;
    }
  }

  if (close(out) != 0)
  {
    saved = errno;
    close(in);
    unlink(dst);
    errno = saved;
    return -1;
  }
  close(in);
  return 0;

failed:
  saved = errno;
  close(out);
  close(in);
  unlink(dst);
  errno = saved;
  return -1;
}

int aud_path_move(const char *src, const char *dst)
{
  if (src == NULL || dst == NULL || *src == '\0' || *dst == '\0')
  {
    errno = EINVAL;
    return -1;
  }

  if (link(src, dst) == 0)
  {
    /*
     * Two names for one file until this succeeds. If it does not, the take is
     * readable under both and nothing has been lost, which is the right way
     * round to fail.
     */
    if (unlink(src) != 0)
    {
      int saved = errno;

      unlink(dst);
      errno = saved;
      return -1;
    }
    return 0;
  }

  if (errno == EEXIST)
  {
    return -1;
  }

  /*
   * EXDEV is the other filesystem; the rest are filesystems that have links
   * and will not make one - some FUSE mounts, and anything mounted from a
   * system that never had them. Copying is right in all of them.
   */
  if (errno != EXDEV && errno != EPERM && errno != EOPNOTSUPP && errno != ENOSYS)
  {
    return -1;
  }

  if (copy_file(src, dst) != 0)
  {
    return -1;
  }

  if (unlink(src) != 0)
  {
    /* the copy arrived, so the take is safe; the original being left behind is
     * worth reporting but not worth deleting the good copy over */
    return -1;
  }
  return 0;
}
