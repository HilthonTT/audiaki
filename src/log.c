/* SPDX-License-Identifier: MIT */
#include "log.h"

#include "version.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static aud_log_level g_level = AUD_LOG_NORMAL;

void aud_log_set_level(aud_log_level level)
{
  g_level = level;
}

aud_log_level aud_log_get_level(void)
{
  return g_level;
}

/*
 * The format(printf, N, 0) annotation marks this as a vprintf-style helper.
 * Without it clang's -Wformat-nonliteral rejects forwarding `fmt` to vfprintf.
 */
static void emit(const char *prefix, const char *fmt, va_list ap) AUD_PRINTF(2, 0);

static void emit(const char *prefix, const char *fmt, va_list ap)
{
  fputs(AUDIAKI_NAME ": ", stderr);
  if (prefix != NULL)
  {
    fputs(prefix, stderr);
  }
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}

void aud_error(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  emit("error: ", fmt, ap);
  va_end(ap);
}

void aud_warn(const char *fmt, ...)
{
  va_list ap;

  if (g_level < AUD_LOG_NORMAL)
  {
    return;
  }
  va_start(ap, fmt);
  emit("warning: ", fmt, ap);
  va_end(ap);
}

void aud_info(const char *fmt, ...)
{
  va_list ap;

  if (g_level < AUD_LOG_NORMAL)
  {
    return;
  }
  va_start(ap, fmt);
  emit(NULL, fmt, ap);
  va_end(ap);
}

void aud_debug(const char *fmt, ...)
{
  va_list ap;

  if (g_level < AUD_LOG_VERBOSE)
  {
    return;
  }
  va_start(ap, fmt);
  emit("debug: ", fmt, ap);
  va_end(ap);
}

void aud_perror(const char *fmt, ...)
{
  int saved = errno;
  va_list ap;

  fputs(AUDIAKI_NAME ": error: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, ": %s\n", strerror(saved));

  errno = saved;
}
