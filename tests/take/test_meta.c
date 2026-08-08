/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "take/meta.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t get_u32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* Find a chunk by id in a built blob, returning its body and size. */
static const unsigned char *find_chunk(const unsigned char *buf, size_t bytes,
                                       const char *id, uint32_t *size)
{
  size_t at = 0;

  while (at + 8u <= bytes)
  {
    uint32_t chunk = get_u32(buf + at + 4u);

    if (memcmp(buf + at, id, 4) == 0)
    {
      *size = chunk;
      return buf + at + 8u;
    }
    at += 8u + chunk + (chunk & 1u);
  }
  *size = 0;
  return NULL;
}

static void a_take(aud_meta *m)
{
  aud_meta_defaults(m);
  m->device = "hw:CARD=Box,DEV=0";
  m->note = "second chorus, clean tone";
  m->rate = 48000;
  m->channels = 2;
  m->bits = 24;
  m->year = 2026;
  m->month = 8;
  m->day = 8;
  m->hour = 14;
  m->minute = 5;
  m->second = 9;
  m->time_reference = 2419632000ull;
}

TEST(the_chunks_are_word_aligned_and_complete)
{
  unsigned char buf[AUD_META_MAX_BYTES];
  aud_meta m;
  size_t bytes;
  size_t at = 0;
  int chunks = 0;

  a_take(&m);
  bytes = aud_meta_build(&m, buf, sizeof(buf));
  CHECK(bytes > 0);

  /* walking the result has to land exactly on the end, or a caller splicing
   * this between fmt and data would corrupt the file */
  while (at + 8u <= bytes)
  {
    uint32_t size = get_u32(buf + at + 4u);

    at += 8u + size + (size & 1u);
    chunks++;
  }
  CHECK_EQ_INT(at, (int)bytes);
  CHECK_EQ_INT(chunks, 2);
  CHECK_EQ_INT(bytes % 2u, 0);
}

TEST(list_info_carries_the_tags)
{
  unsigned char buf[AUD_META_MAX_BYTES];
  aud_meta m;
  size_t bytes;
  const unsigned char *list;
  uint32_t size = 0;
  uint32_t at = 4u;
  int seen_note = 0;
  int seen_date = 0;
  int seen_device = 0;
  int seen_software = 0;

  a_take(&m);
  bytes = aud_meta_build(&m, buf, sizeof(buf));

  list = find_chunk(buf, bytes, "LIST", &size);
  CHECK(list != NULL);
  if (list == NULL)
  {
    return;
  }
  CHECK_EQ_INT(memcmp(list, "INFO", 4), 0);

  while (at + 8u <= size)
  {
    uint32_t len = get_u32(list + at + 4u);
    const char *value = (const char *)(list + at + 8u);

    if (memcmp(list + at, "ICMT", 4) == 0)
    {
      CHECK_EQ_STR(value, "second chorus, clean tone");
      seen_note = 1;
    }
    else if (memcmp(list + at, "ICRD", 4) == 0)
    {
      CHECK_EQ_STR(value, "2026-08-08");
      seen_date = 1;
    }
    else if (memcmp(list + at, "ISRC", 4) == 0)
    {
      CHECK_EQ_STR(value, "hw:CARD=Box,DEV=0");
      seen_device = 1;
    }
    else if (memcmp(list + at, "ISFT", 4) == 0)
    {
      seen_software = 1;
    }
    /* the length counts the NUL, so a tag is never zero long */
    CHECK(len > 0);
    at += 8u + len + (len & 1u);
  }

  CHECK(seen_note);
  CHECK(seen_date);
  CHECK(seen_device);
  CHECK(seen_software);
}

TEST(bext_is_the_fixed_body_plus_a_coding_history)
{
  unsigned char buf[AUD_META_MAX_BYTES];
  aud_meta m;
  size_t bytes;
  const unsigned char *bext;
  uint32_t size = 0;

  a_take(&m);
  bytes = aud_meta_build(&m, buf, sizeof(buf));

  bext = find_chunk(buf, bytes, "bext", &size);
  CHECK(bext != NULL);
  if (bext == NULL)
  {
    return;
  }

  CHECK(size > 602u); /* the fixed part, and a history line after it */
  CHECK_EQ_INT(memcmp(bext, "second chorus, clean tone", 25), 0);
  CHECK_EQ_INT(memcmp(bext + 320, "2026-08-08", 10), 0);
  CHECK_EQ_INT(memcmp(bext + 330, "14:05:09", 8), 0);

  /* TimeReference is a 64 bit count split into two little-endian halves */
  CHECK_EQ_INT(get_u32(bext + 338), (uint32_t)(2419632000ull & 0xFFFFFFFFu));
  CHECK_EQ_INT(get_u32(bext + 342), (uint32_t)(2419632000ull >> 32));

  CHECK_EQ_INT(bext[346], 1); /* version 1: a UMID, and no loudness fields */

  CHECK(strstr((const char *)bext + 602, "A=PCM,F=48000,W=24,M=stereo") != NULL);
  CHECK(strstr((const char *)bext + 602, "hw:CARD=Box,DEV=0") != NULL);
}

TEST(what_was_written_reads_back)
{
  unsigned char buf[AUD_META_MAX_BYTES];
  aud_meta m;
  aud_meta_info got;
  size_t bytes;
  const unsigned char *body;
  uint32_t size = 0;

  a_take(&m);
  bytes = aud_meta_build(&m, buf, sizeof(buf));
  memset(&got, 0, sizeof(got));

  body = find_chunk(buf, bytes, "LIST", &size);
  CHECK(body != NULL);
  if (body != NULL)
  {
    aud_meta_read_list(&got, body, size);
  }

  body = find_chunk(buf, bytes, "bext", &size);
  CHECK(body != NULL);
  if (body != NULL)
  {
    aud_meta_read_bext(&got, body, size);
  }

  CHECK(got.present);
  CHECK_EQ_STR(got.note, "second chorus, clean tone");
  CHECK_EQ_STR(got.device, "hw:CARD=Box,DEV=0");
  CHECK_EQ_STR(got.recorded, "2026-08-08 14:05:09");
  CHECK(strstr(got.software, "audiaki") != NULL);
  CHECK(strstr(got.coding_history, "M=stereo") != NULL);
}

TEST(a_take_without_a_note_or_a_clock_still_describes_itself)
{
  unsigned char buf[AUD_META_MAX_BYTES];
  aud_meta m;
  aud_meta_info got;
  size_t bytes;
  const unsigned char *body;
  uint32_t size = 0;

  aud_meta_defaults(&m);
  m.device = "default";
  m.rate = 44100;
  m.channels = 1;
  m.bits = 16;

  bytes = aud_meta_build(&m, buf, sizeof(buf));
  CHECK(bytes > 0);

  memset(&got, 0, sizeof(got));
  body = find_chunk(buf, bytes, "LIST", &size);
  if (body != NULL)
  {
    aud_meta_read_list(&got, body, size);
  }
  body = find_chunk(buf, bytes, "bext", &size);
  if (body != NULL)
  {
    aud_meta_read_bext(&got, body, size);
  }

  CHECK_EQ_STR(got.note, "");
  CHECK_EQ_STR(got.recorded, ""); /* no date rather than a wrong one */
  CHECK_EQ_STR(got.device, "default");
  CHECK(strstr(got.coding_history, "M=mono") != NULL);
}

TEST(a_note_at_the_limit_survives_both_chunks)
{
  unsigned char buf[AUD_META_MAX_BYTES];
  char note[AUD_META_NOTE_MAX + 1];
  aud_meta m;
  aud_meta_info got;
  size_t bytes;
  const unsigned char *body;
  uint32_t size = 0;

  memset(note, 'x', sizeof(note) - 1u);
  note[sizeof(note) - 1u] = '\0';

  aud_meta_defaults(&m);
  m.note = note;
  m.device = "default";
  m.rate = 96000;
  m.channels = 4;
  m.bits = 32;

  bytes = aud_meta_build(&m, buf, sizeof(buf));
  CHECK(bytes > 0);
  CHECK(bytes <= AUD_META_MAX_BYTES);

  memset(&got, 0, sizeof(got));
  body = find_chunk(buf, bytes, "LIST", &size);
  if (body != NULL)
  {
    aud_meta_read_list(&got, body, size);
  }
  CHECK_EQ_STR(got.note, note);

  body = find_chunk(buf, bytes, "bext", &size);
  CHECK(body != NULL);
  if (body != NULL)
  {
    CHECK(strstr((const char *)body + 602, "M=4-channel") != NULL);
  }
}

TEST(a_buffer_too_small_yields_nothing_rather_than_half_a_chunk)
{
  unsigned char buf[64];
  aud_meta m;

  a_take(&m);
  CHECK_EQ_INT(aud_meta_build(&m, buf, sizeof(buf)), 0);
  CHECK_EQ_INT(aud_meta_build(&m, buf, 0), 0);
}

TEST(a_malformed_chunk_is_read_without_running_off_the_end)
{
  aud_meta_info got;
  unsigned char list[16];
  unsigned char bext[700];

  /* a LIST whose tag claims far more than the chunk holds */
  memset(&got, 0, sizeof(got));
  memcpy(list, "INFO", 4);
  memcpy(list + 4, "ICMT", 4);
  list[8] = 0xFF;
  list[9] = 0xFF;
  list[10] = 0xFF;
  list[11] = 0x7F;
  memcpy(list + 12, "hi", 3);
  aud_meta_read_list(&got, list, sizeof(list));
  CHECK_EQ_STR(got.note, "hi");

  /* a bext body cut short of even its date */
  memset(&got, 0, sizeof(got));
  memset(bext, 0, sizeof(bext));
  aud_meta_read_bext(&got, bext, 100u);
  CHECK_EQ_INT(got.present, 0);

  /* control characters are neutered rather than passed to the terminal */
  memset(&got, 0, sizeof(got));
  memset(bext, 0, sizeof(bext));
  memcpy(bext, "a\033[2Jb", 6);
  aud_meta_read_bext(&got, bext, sizeof(bext));
  CHECK_EQ_STR(got.note, "a [2Jb");
}

int main(void)
{
  RUN(the_chunks_are_word_aligned_and_complete);
  RUN(list_info_carries_the_tags);
  RUN(bext_is_the_fixed_body_plus_a_coding_history);
  RUN(what_was_written_reads_back);
  RUN(a_take_without_a_note_or_a_clock_still_describes_itself);
  RUN(a_note_at_the_limit_survives_both_chunks);
  RUN(a_buffer_too_small_yields_nothing_rather_than_half_a_chunk);
  RUN(a_malformed_chunk_is_read_without_running_off_the_end);

  return TEST_RESULT();
}
