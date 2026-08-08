/* SPDX-License-Identifier: MIT */
/*
 * log.h - diagnostics on stderr.
 *
 * stdout stays clean so `audiaki --list` and `--probe` can be piped into
 * other tools without status text getting mixed in.
 */
#ifndef AUDIAKI_LOG_H
#define AUDIAKI_LOG_H

#if defined(__GNUC__)
#define AUD_PRINTF(fmt_index, first_arg) \
  __attribute__((format(printf, fmt_index, first_arg)))
#else
#define AUD_PRINTF(fmt_index, first_arg)
#endif

typedef enum
{
  AUD_LOG_QUIET = 0, /* errors only */
  AUD_LOG_NORMAL,    /* errors, warnings and status */
  AUD_LOG_VERBOSE,   /* everything, including device negotiation details */
} aud_log_level;

void aud_log_set_level(aud_log_level level);
aud_log_level aud_log_get_level(void);

void aud_error(const char *fmt, ...) AUD_PRINTF(1, 2);
void aud_warn(const char *fmt, ...) AUD_PRINTF(1, 2);
void aud_info(const char *fmt, ...) AUD_PRINTF(1, 2);
void aud_debug(const char *fmt, ...) AUD_PRINTF(1, 2);

/* Like aud_error() but appends ": <strerror(errno)>" and preserves errno. */
void aud_perror(const char *fmt, ...) AUD_PRINTF(1, 2);

#endif /* AUDIAKI_LOG_H */
