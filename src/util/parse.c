/* SPDX-License-Identifier: MIT */
#include "util/parse.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int parse_uint(const char *text, unsigned min, unsigned max, unsigned *out)
{
  char *end = NULL;
  unsigned long value;

  if (text == NULL || out == NULL || *text == '\0')
  {
    return -1;
  }
  /* strtoul happily wraps "-1" into ULONG_MAX; reject signs outright. */
  if (*text < '0' || *text > '9')
  {
    return -1;
  }

  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0')
  {
    return -1;
  }
  if (value > (unsigned long)UINT_MAX || value < (unsigned long)min ||
      value > (unsigned long)max)
  {
    return -1;
  }

  *out = (unsigned)value;
  return 0;
}

int parse_size(const char *text, unsigned min, unsigned max, unsigned *out_width,
               unsigned *out_height)
{
  static const struct
  {
    const char *name;
    unsigned width;
    unsigned height;
  } shorthand[] = {
      {"480p", 854u, 480u},    {"720p", 1280u, 720u},   {"1080p", 1920u, 1080u},
      {"1440p", 2560u, 1440u}, {"2160p", 3840u, 2160u},
  };

  char buf[32];
  char *cross;
  unsigned width;
  unsigned height;
  size_t len;

  if (text == NULL || out_width == NULL || out_height == NULL || *text == '\0')
  {
    return -1;
  }

  for (size_t i = 0; i < sizeof(shorthand) / sizeof(shorthand[0]); i++)
  {
    if (strcmp(text, shorthand[i].name) != 0)
    {
      continue;
    }
    if (shorthand[i].width < min || shorthand[i].width > max ||
        shorthand[i].height < min || shorthand[i].height > max)
    {
      return -1;
    }
    *out_width = shorthand[i].width;
    *out_height = shorthand[i].height;
    return 0;
  }

  len = strlen(text);
  if (len >= sizeof(buf))
  {
    return -1;
  }
  memcpy(buf, text, len + 1);

  cross = strchr(buf, 'x');
  if (cross == NULL)
  {
    cross = strchr(buf, 'X');
  }
  if (cross == NULL || cross == buf)
  {
    return -1;
  }
  *cross = '\0';

  if (parse_uint(buf, min, max, &width) != 0 ||
      parse_uint(cross + 1, min, max, &height) != 0)
  {
    return -1;
  }

  *out_width = width;
  *out_height = height;
  return 0;
}

int parse_double(const char *text, double min, double max, double *out)
{
  char *end = NULL;
  double value;

  if (text == NULL || out == NULL || *text == '\0')
  {
    return -1;
  }
  /* a leading digit rules out "-1", "+1", "inf" and " 1" in one test */
  if (*text < '0' || *text > '9')
  {
    return -1;
  }

  errno = 0;
  value = strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0' || !isfinite(value))
  {
    return -1;
  }
  if (value < min || value > max)
  {
    return -1;
  }

  *out = value;
  return 0;
}

/* Parse one "12" or "12.5" field of a duration. Returns 0 on success. */
static int parse_field(const char *text, double *out)
{
  return parse_double(text, 0.0, DBL_MAX, out);
}

int parse_duration(const char *text, double *out_seconds)
{
  char buf[64];
  const char *fields[3];
  size_t n_fields = 0;
  size_t len;
  double total = 0.0;

  if (text == NULL || out_seconds == NULL || *text == '\0')
  {
    return -1;
  }

  len = strlen(text);
  if (len >= sizeof(buf))
  {
    return -1;
  }
  memcpy(buf, text, len + 1);

  /* split on ':' in place, most significant field first */
  fields[n_fields++] = buf;
  for (char *p = buf; *p != '\0'; p++)
  {
    if (*p != ':')
    {
      continue;
    }
    if (n_fields == sizeof(fields) / sizeof(fields[0]))
    {
      return -1;
    }
    *p = '\0';
    fields[n_fields++] = p + 1;
  }

  for (size_t i = 0; i < n_fields; i++)
  {
    double value;
    if (parse_field(fields[i], &value) != 0)
    {
      return -1;
    }
    /* only the leading field may exceed 59 ("90" seconds, "90:00" minutes) */
    if (i > 0 && value >= 60.0)
    {
      return -1;
    }
    total = total * 60.0 + value;
  }

  *out_seconds = total;
  return 0;
}
