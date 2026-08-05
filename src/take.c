/* SPDX-License-Identifier: MIT */
#include "take.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TAKE_DEFAULT_SUFFIX ".wav"

/*
 * Length of the stem of `prefix`, i.e. everything before its extension. A dot
 * inside a directory name is not an extension, and neither is a leading dot on
 * the basename - ".wav" is a hidden file, not an empty name.
 */
static size_t stem_length(const char *prefix)
{
  const char *slash = strrchr(prefix, '/');
  const char *base = slash != NULL ? slash + 1 : prefix;
  const char *dot = strrchr(base, '.');

  if (dot == NULL || dot == base)
    return strlen(prefix);
  return (size_t)(dot - prefix);
}

int aud_take_path(char *dst, size_t size, const char *prefix, unsigned number)
{
  size_t stem;
  const char *suffix;
  int written;

  if (dst == NULL || size == 0 || prefix == NULL || *prefix == '\0')
    return -1;
  if (number > AUD_TAKE_MAX_NUMBER)
    return -1;

  stem = stem_length(prefix);
  suffix = prefix[stem] != '\0' ? prefix + stem : TAKE_DEFAULT_SUFFIX;

  written = snprintf(dst, size, "%.*s-%03u%s", (int)stem, prefix, number, suffix);
  if (written < 0 || (size_t)written >= size)
    return -1;
  return 0;
}

int aud_take_next(char *dst, size_t size, const char *prefix)
{
  for (unsigned n = 1; n <= AUD_TAKE_MAX_NUMBER; n++)
  {
    if (aud_take_path(dst, size, prefix, n) != 0)
      return -1;
    if (access(dst, F_OK) != 0)
      return 0;
  }
  return -1;
}
