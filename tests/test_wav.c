/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "wav.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_path[256];

static uint32_t read_u32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint16_t read_u16(const unsigned char *p)
{
  return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* Read a whole file into `buf`, returning its size or -1. */
static long slurp(const char *path, unsigned char *buf, size_t cap)
{
  FILE *f = fopen(path, "rb");
  size_t n;

  if (f == NULL)
    return -1;
  n = fread(buf, 1, cap, f);
  fclose(f);
  return (long)n;
}

TEST(header_layout)
{
  unsigned char h[WAV_HEADER_BYTES];

  wav_build_header(h, 1000, 44100, 2, 24);

  CHECK(memcmp(h + 0, "RIFF", 4) == 0);
  CHECK(memcmp(h + 8, "WAVE", 4) == 0);
  CHECK(memcmp(h + 12, "fmt ", 4) == 0);
  CHECK(memcmp(h + 36, "data", 4) == 0);

  CHECK_EQ_INT(read_u32(h + 4), 36 + 1000);
  CHECK_EQ_INT(read_u32(h + 16), 16);     /* fmt chunk size */
  CHECK_EQ_INT(read_u16(h + 20), 1);      /* WAVE_FORMAT_PCM */
  CHECK_EQ_INT(read_u16(h + 22), 2);      /* channels */
  CHECK_EQ_INT(read_u32(h + 24), 44100);  /* sample rate */
  CHECK_EQ_INT(read_u32(h + 28), 264600); /* 44100 * 2 * 3 */
  CHECK_EQ_INT(read_u16(h + 32), 6);      /* block align */
  CHECK_EQ_INT(read_u16(h + 34), 24);     /* bits */
  CHECK_EQ_INT(read_u32(h + 40), 1000);   /* data size */
}

TEST(header_counts_pad_byte_in_riff_size)
{
  unsigned char h[WAV_HEADER_BYTES];

  /* an odd payload is followed by a pad byte that RIFF counts but data does not */
  wav_build_header(h, 3, 44100, 1, 24);
  CHECK_EQ_INT(read_u32(h + 40), 3);
  CHECK_EQ_INT(read_u32(h + 4), 36 + 3 + 1);

  wav_build_header(h, 4, 44100, 1, 16);
  CHECK_EQ_INT(read_u32(h + 4), 36 + 4);
}

TEST(write_and_finalise)
{
  wav_writer w;
  unsigned char file[128];
  const unsigned char pcm[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  long size;

  CHECK_EQ_INT(wav_open(&w, g_path, 48000, 2, 16, 1), 0);
  CHECK_EQ_INT(wav_write(&w, pcm, sizeof(pcm)), 0);
  CHECK_EQ_DBL(wav_duration(&w), 3.0 / 48000.0, 1e-12);
  CHECK_EQ_INT(wav_close(&w), 0);

  size = slurp(g_path, file, sizeof(file));
  CHECK_EQ_INT(size, WAV_HEADER_BYTES + 12);
  CHECK_EQ_INT(read_u32(file + 40), 12);
  CHECK_EQ_INT(read_u32(file + 4), 36 + 12);
  CHECK(memcmp(file + WAV_HEADER_BYTES, pcm, sizeof(pcm)) == 0);

  remove(g_path);
}

TEST(odd_payload_gets_pad_byte)
{
  wav_writer w;
  unsigned char file[128];
  const unsigned char pcm[3] = {0xAA, 0xBB, 0xCC};
  long size;

  CHECK_EQ_INT(wav_open(&w, g_path, 44100, 1, 24, 1), 0);
  CHECK_EQ_INT(wav_write(&w, pcm, sizeof(pcm)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  size = slurp(g_path, file, sizeof(file));
  CHECK_EQ_INT(size, WAV_HEADER_BYTES + 3 + 1);
  CHECK_EQ_INT(read_u32(file + 40), 3);
  CHECK_EQ_INT(file[WAV_HEADER_BYTES + 3], 0);

  remove(g_path);
}

TEST(refuses_to_clobber_without_force)
{
  wav_writer w;

  CHECK_EQ_INT(wav_open(&w, g_path, 44100, 2, 16, 0), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  errno = 0;
  CHECK_EQ_INT(wav_open(&w, g_path, 44100, 2, 16, 0), -1);
  CHECK_EQ_INT(errno, EEXIST);

  /* the same call with overwrite enabled succeeds */
  CHECK_EQ_INT(wav_open(&w, g_path, 44100, 2, 16, 1), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  remove(g_path);
}

TEST(rejects_invalid_geometry)
{
  wav_writer w;

  CHECK_EQ_INT(wav_open(&w, g_path, 0, 2, 16, 1), -1);
  CHECK_EQ_INT(wav_open(&w, g_path, 44100, 0, 16, 1), -1);
  CHECK_EQ_INT(wav_open(&w, g_path, 44100, 2, 0, 1), -1);
  CHECK_EQ_INT(wav_open(&w, g_path, 44100, 2, 20, 1), -1); /* not byte aligned */
  CHECK_EQ_INT(wav_open(&w, NULL, 44100, 2, 16, 1), -1);
}

TEST(overflow_guard)
{
  wav_writer w;

  memset(&w, 0, sizeof(w));
  w.data_bytes = WAV_MAX_DATA_BYTES - 10;
  CHECK(!wav_would_overflow(&w, 10));
  CHECK(wav_would_overflow(&w, 11));
}

TEST(discard_removes_the_file)
{
  wav_writer w;
  unsigned char file[8];

  CHECK_EQ_INT(wav_open(&w, g_path, 44100, 2, 16, 1), 0);
  wav_discard(&w);
  CHECK_EQ_INT(slurp(g_path, file, sizeof(file)), -1);
}

int main(void)
{
  int rc;

  snprintf(g_path, sizeof(g_path), "audiaki-test-%ld.wav", (long)getpid());

  RUN(header_layout);
  RUN(header_counts_pad_byte_in_riff_size);
  RUN(write_and_finalise);
  RUN(odd_payload_gets_pad_byte);
  RUN(refuses_to_clobber_without_force);
  RUN(rejects_invalid_geometry);
  RUN(overflow_guard);
  RUN(discard_removes_the_file);

  rc = TEST_RESULT();
  remove(g_path);
  return rc;
}
