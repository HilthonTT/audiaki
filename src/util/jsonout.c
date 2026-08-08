/* SPDX-License-Identifier: MIT */
#include "util/jsonout.h"

#include <math.h>

void aud_json_string(FILE *out, const char *s)
{
  if (s == NULL)
  {
    fputs("null", out);
    return;
  }

  fputc('"', out);
  for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++)
  {
    switch (*p)
    {
    case '"':
      fputs("\\\"", out);
      break;
    case '\\':
      fputs("\\\\", out);
      break;
    case '\b':
      fputs("\\b", out);
      break;
    case '\f':
      fputs("\\f", out);
      break;
    case '\n':
      fputs("\\n", out);
      break;
    case '\r':
      fputs("\\r", out);
      break;
    case '\t':
      fputs("\\t", out);
      break;
    default:
      /*
       * Only the C0 controls have to be escaped. The solidus may be, but need
       * not, and escaping it makes a file path harder to read for no gain.
       */
      if (*p < 0x20u)
      {
        fprintf(out, "\\u%04x", (unsigned)*p);
      }
      else
      {
        fputc((int)*p, out);
      }
      break;
    }
  }
  fputc('"', out);
}

void aud_json_number(FILE *out, double value, int decimals)
{
  if (!isfinite(value))
  {
    fputs("null", out);
    return;
  }
  fprintf(out, "%.*f", decimals, value);
}
