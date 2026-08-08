/* SPDX-License-Identifier: MIT */
#include "take/meta.h"

#include "version.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* bext is a fixed 602 byte body in every version of the specification - each
 * one spends the reserved area differently rather than growing - and a coding
 * history of whatever length after it. Version 1 is the one with a UMID and
 * without the loudness fields, which is what audiaki can honestly fill in:
 * version 2 promises measured loudness, and measuring is --info's job. */
#define BEXT_FIXED_BYTES 602u
#define BEXT_VERSION 1u

#define BEXT_DESCRIPTION_MAX 256u
#define BEXT_ORIGINATOR_MAX 32u

/* the longest coding history audiaki writes: four fields and a device name */
#define CODING_HISTORY_MAX 256u

typedef struct
{
  unsigned char *buf;
  size_t size;
  size_t used;
  int overflowed;
} writer;

static void put_u16(unsigned char *p, uint16_t v)
{
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put_u32(unsigned char *p, uint32_t v)
{
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
  p[2] = (unsigned char)((v >> 16) & 0xFFu);
  p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* Reserve `n` zeroed bytes and return where they start, or NULL if full. */
static unsigned char *reserve(writer *w, size_t n)
{
  unsigned char *at;

  if (w->overflowed || n > w->size - w->used)
  {
    w->overflowed = 1;
    return NULL;
  }

  at = w->buf + w->used;
  memset(at, 0, n);
  w->used += n;
  return at;
}

static int is_empty(const char *s)
{
  return s == NULL || *s == '\0';
}

/*
 * Copy `s` into a fixed width field, padded with zeros and never terminated by
 * force: bext fields are a fixed size and a value that fills one exactly is
 * allowed to use the last byte.
 */
static void put_fixed(unsigned char *dst, size_t width, const char *s)
{
  size_t n;

  memset(dst, 0, width);
  if (is_empty(s))
  {
    return;
  }

  n = strlen(s);
  if (n > width)
  {
    n = width;
  }
  memcpy(dst, s, n);
}

/*
 * One LIST/INFO tag: a four character id, a length that counts the terminating
 * NUL, the string, and a pad byte when that came to an odd number.
 */
static void put_info_tag(writer *w, const char *id, const char *value, size_t max)
{
  size_t len;
  size_t padded;
  unsigned char *at;

  if (is_empty(value))
  {
    return;
  }

  len = strlen(value);
  if (len > max)
  {
    len = max;
  }
  padded = (len + 1u + 1u) & ~(size_t)1u; /* + NUL, rounded up to even */

  at = reserve(w, 8u + padded);
  if (at == NULL)
  {
    return;
  }

  memcpy(at, id, 4);
  put_u32(at + 4, (uint32_t)(len + 1u));
  memcpy(at + 8, value, len);
  /* reserve() zeroed the rest, which is the NUL and any pad byte */
}

static const char *software_of(const aud_meta *m)
{
  return is_empty(m->software) ? AUDIAKI_NAME " " AUDIAKI_VERSION : m->software;
}

static int have_date(const aud_meta *m)
{
  return m->year > 0 && m->month > 0 && m->day > 0;
}

/*
 * Both fields are a fixed width in bext - ten bytes and eight - and copied by
 * length rather than by terminator, so the wrapping keeps an implausible clock
 * from shortening the field and dragging whatever follows it into the file.
 */
static void format_date(char *dst, size_t size, const aud_meta *m)
{
  snprintf(dst, size, "%04u-%02u-%02u", m->year % 10000u, m->month % 100u, m->day % 100u);
}

static void format_time(char *dst, size_t size, const aud_meta *m)
{
  snprintf(dst, size, "%02u:%02u:%02u", m->hour % 100u, m->minute % 100u,
           m->second % 100u);
}

/* mono, stereo, or a count: the EBU tokens stop at two channels. */
static void format_mode(char *dst, size_t size, unsigned channels)
{
  if (channels == 1)
  {
    snprintf(dst, size, "mono");
  }
  else if (channels == 2)
  {
    snprintf(dst, size, "stereo");
  }
  else
  {
    snprintf(dst, size, "%u-channel", channels);
  }
}

/*
 * The EBU R98 coding history line: what the audio is, and what put it there.
 * The free text T= field is where the capture device belongs - it is the one
 * part of the chain no standard field describes.
 */
static void format_coding_history(char *dst, size_t size, const aud_meta *m)
{
  char mode[32];

  format_mode(mode, sizeof(mode), m->channels);

  if (is_empty(m->device))
  {
    snprintf(dst, size, "A=PCM,F=%u,W=%u,M=%s,T=%s\r\n", m->rate, m->bits, mode,
             software_of(m));
  }
  else
  {
    snprintf(dst, size, "A=PCM,F=%u,W=%u,M=%s,T=%s; %.*s\r\n", m->rate, m->bits, mode,
             software_of(m), (int)AUD_META_DEVICE_MAX, m->device);
  }
}

static void build_list_info(writer *w, const aud_meta *m)
{
  char date[16];
  unsigned char *head;
  size_t body_start;

  head = reserve(w, 12u);
  if (head == NULL)
  {
    return;
  }
  memcpy(head, "LIST", 4);
  memcpy(head + 8, "INFO", 4);
  body_start = w->used;

  put_info_tag(w, "ISFT", software_of(m), 63u);
  if (have_date(m))
  {
    format_date(date, sizeof(date), m);
    put_info_tag(w, "ICRD", date, sizeof(date) - 1u);
  }
  /*
   * ISRC in RIFF/INFO is "source" - where the material came from - which is as
   * close as the tag set gets to naming a capture device. It is not the music
   * industry's recording code, whatever a tagger may label the field.
   */
  put_info_tag(w, "ISRC", m->device, AUD_META_DEVICE_MAX);
  put_info_tag(w, "ICMT", m->note, AUD_META_NOTE_MAX);

  /* the form type counts towards the chunk body, the eight byte header does not */
  put_u32(head + 4, (uint32_t)(w->used - body_start + 4u));
}

static void build_bext(writer *w, const aud_meta *m)
{
  char history[CODING_HISTORY_MAX];
  char date[16];
  char clock[16];
  size_t history_len;
  size_t padded;
  unsigned char *head;
  unsigned char *body;

  format_coding_history(history, sizeof(history), m);
  history_len = strlen(history);
  padded = (history_len + 1u) & ~(size_t)1u;

  head = reserve(w, 8u);
  if (head == NULL)
  {
    return;
  }
  memcpy(head, "bext", 4);
  put_u32(head + 4, (uint32_t)(BEXT_FIXED_BYTES + history_len));

  body = reserve(w, BEXT_FIXED_BYTES + padded);
  if (body == NULL)
  {
    /* the header is already down; unwind it rather than leave half a chunk */
    w->used -= 8u;
    return;
  }

  put_fixed(body, BEXT_DESCRIPTION_MAX, m->note);
  put_fixed(body + 256, BEXT_ORIGINATOR_MAX, software_of(m));
  /* OriginatorReference at 288 stays zero: it is meant to be unique across the
   * world, audiaki has nothing that qualifies, and an invented one is worse
   * than an absent one. */

  if (have_date(m))
  {
    format_date(date, sizeof(date), m);
    format_time(clock, sizeof(clock), m);
    memcpy(body + 320, date, 10);
    memcpy(body + 330, clock, 8);
  }

  put_u32(body + 338, (uint32_t)(m->time_reference & 0xFFFFFFFFu));
  put_u32(body + 342, (uint32_t)(m->time_reference >> 32));
  put_u16(body + 346, BEXT_VERSION);
  /* UMID, the loudness fields and the reserved area stay zero, as a file with
   * nothing measured is required to leave them */

  memcpy(body + BEXT_FIXED_BYTES, history, history_len);
}

void aud_meta_defaults(aud_meta *m)
{
  if (m == NULL)
  {
    return;
  }
  memset(m, 0, sizeof(*m));
}

void aud_meta_stamp_now(aud_meta *m, unsigned rate)
{
  time_t now;
  struct tm parts;

  if (m == NULL)
  {
    return;
  }

  now = time(NULL);
  if (now == (time_t)-1 || localtime_r(&now, &parts) == NULL)
  {
    /* no usable clock: leave the fields zero, and the date out of the file */
    return;
  }

  m->year = (unsigned)parts.tm_year + 1900u;
  m->month = (unsigned)parts.tm_mon + 1u;
  m->day = (unsigned)parts.tm_mday;
  m->hour = (unsigned)parts.tm_hour;
  m->minute = (unsigned)parts.tm_min;
  m->second = (unsigned)parts.tm_sec;

  m->time_reference =
      ((uint64_t)m->hour * 3600u + (uint64_t)m->minute * 60u + m->second) * rate;
}

size_t aud_meta_build(const aud_meta *m, unsigned char *out, size_t size)
{
  writer w;

  if (m == NULL || out == NULL || size == 0)
  {
    return 0;
  }

  w.buf = out;
  w.size = size;
  w.used = 0;
  w.overflowed = 0;

  build_list_info(&w, m);
  build_bext(&w, m);

  /*
   * All or nothing. A caller is about to write this between two chunks it has
   * already sized, and half a chunk list would corrupt the file rather than
   * describe it less well.
   */
  return w.overflowed ? 0 : w.used;
}

/* -- reading back ---------------------------------------------------------- */

static uint32_t get_u32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/*
 * Copy at most `n` bytes of a field into a NUL terminated string, stopping at
 * the first NUL and dropping trailing blanks. Control characters become spaces
 * so a chunk from elsewhere cannot move the terminal's cursor when --info
 * prints it back.
 */
static void take_field(char *dst, size_t size, const unsigned char *src, size_t n)
{
  size_t out = 0;

  if (size == 0)
  {
    return;
  }
  if (n > size - 1u)
  {
    n = size - 1u;
  }

  for (size_t i = 0; i < n && src[i] != '\0'; i++)
  {
    unsigned char c = src[i];

    dst[out++] = (c < 0x20u || c == 0x7Fu) ? ' ' : (char)c;
  }

  while (out > 0 && dst[out - 1u] == ' ')
  {
    out--;
  }
  dst[out] = '\0';
}

void aud_meta_read_list(aud_meta_info *out, const unsigned char *body, size_t bytes)
{
  size_t at = 4u; /* past the INFO form type */

  if (out == NULL || body == NULL || bytes < 12u || memcmp(body, "INFO", 4) != 0)
  {
    return;
  }

  while (at + 8u <= bytes)
  {
    const unsigned char *id = body + at;
    uint32_t size = get_u32(body + at + 4u);
    const unsigned char *value = body + at + 8u;
    size_t available = bytes - (at + 8u);

    if (size > available)
    {
      size = (uint32_t)available; /* truncated tail, read what is there */
    }

    if (memcmp(id, "ISFT", 4) == 0)
    {
      take_field(out->software, sizeof(out->software), value, size);
      out->present = 1;
    }
    else if (memcmp(id, "ISRC", 4) == 0)
    {
      take_field(out->device, sizeof(out->device), value, size);
      out->present = 1;
    }
    else if (memcmp(id, "ICMT", 4) == 0)
    {
      take_field(out->note, sizeof(out->note), value, size);
      out->present = 1;
    }
    else if (memcmp(id, "ICRD", 4) == 0 && out->recorded[0] == '\0')
    {
      /* only as a fallback: bext carries the time of day as well as the date */
      take_field(out->recorded, sizeof(out->recorded), value, size);
      out->present = 1;
    }

    at += 8u + size + (size & 1u);
  }
}

void aud_meta_read_bext(aud_meta_info *out, const unsigned char *body, size_t bytes)
{
  char date[11] = {0};
  char clock[9] = {0};

  if (out == NULL || body == NULL || bytes < 346u)
  {
    return;
  }

  out->present = 1;

  if (out->note[0] == '\0')
  {
    take_field(out->note, sizeof(out->note), body, BEXT_DESCRIPTION_MAX);
  }
  take_field(out->originator, sizeof(out->originator), body + 256, BEXT_ORIGINATOR_MAX);

  take_field(date, sizeof(date), body + 320, 10u);
  take_field(clock, sizeof(clock), body + 330, 8u);
  if (date[0] != '\0')
  {
    snprintf(out->recorded, sizeof(out->recorded), "%s%s%s", date,
             clock[0] != '\0' ? " " : "", clock);
  }

  if (bytes > BEXT_FIXED_BYTES)
  {
    take_field(out->coding_history, sizeof(out->coding_history), body + BEXT_FIXED_BYTES,
               bytes - BEXT_FIXED_BYTES);
  }
}
