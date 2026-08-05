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

/* -- reader ---------------------------------------------------------------- */

/* Write a file the reader can be pointed at. Returns 0 on success. */
static int write_take(const char *path, uint32_t rate, uint16_t channels, uint16_t bits,
                      const void *payload, size_t bytes)
{
  wav_writer w;

  if (wav_open(&w, path, rate, channels, bits, 1) != 0)
    return -1;
  if (wav_write(&w, payload, bytes) != 0)
  {
    wav_discard(&w);
    return -1;
  }
  return wav_close(&w);
}

TEST(reader_round_trips_the_writer)
{
  wav_reader r;
  int16_t samples[8] = {0, 16384, -16384, 32767, -32768, 0, 8192, -8192};
  float mono[8];
  long got;

  CHECK_EQ_INT(write_take(g_path, 44100, 1, 16, samples, sizeof(samples)), 0);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.rate, 44100);
  CHECK_EQ_INT(r.channels, 1);
  CHECK_EQ_INT(r.bits, 16);
  CHECK_EQ_INT(r.is_float, 0);
  CHECK_EQ_INT(r.frames, 8);
  CHECK_EQ_DBL(wav_read_duration(&r), 8.0 / 44100.0, 1e-9);

  got = wav_read_mono(&r, mono, 8);
  CHECK_EQ_INT(got, 8);
  CHECK_EQ_DBL(mono[0], 0.0, 1e-6);
  CHECK_EQ_DBL(mono[1], 0.5, 1e-4);
  CHECK_EQ_DBL(mono[2], -0.5, 1e-4);
  CHECK_EQ_DBL(mono[3], 1.0, 1e-4);
  CHECK_EQ_DBL(mono[4], -1.0, 1e-6);

  /* and then it is empty, not an error */
  CHECK_EQ_INT(wav_read_mono(&r, mono, 8), 0);

  wav_read_close(&r);
  CHECK(r.file == NULL);
}

TEST(reader_keeps_channels_apart_on_request)
{
  wav_reader r;
  /* two stereo frames: hard left, then equal and opposite */
  int16_t samples[4] = {32767, 0, 16000, -16000};
  float frames[4];

  CHECK_EQ_INT(write_take(g_path, 48000, 2, 16, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);

  CHECK_EQ_INT(wav_read_frames(&r, frames, 2), 2);
  CHECK_EQ_DBL(frames[0], 32767.0 / 32768.0, 1e-6);
  CHECK_EQ_DBL(frames[1], 0.0, 1e-6);
  CHECK_EQ_DBL(frames[2], 16000.0 / 32768.0, 1e-6);
  CHECK_EQ_DBL(frames[3], -16000.0 / 32768.0, 1e-6);

  /* and the same end-of-data behaviour as the mono decoder */
  CHECK_EQ_INT(wav_read_frames(&r, frames, 2), 0);
  wav_read_close(&r);
}

TEST(the_per_channel_reader_does_not_clamp_float)
{
  wav_reader r;
  /*
   * 32 bit float WAV is allowed past full scale. wav_read_mono() clamps,
   * because the analyser wants a bounded signal; wav_read_frames() must not,
   * because measuring a take means seeing the overshoot.
   */
  static const unsigned char file[] = {
      'R',  'I',  'F',  'F',  0x2C, 0x00, 0x00, 0x00, 'W',  'A',  'V',  'E',
      'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00,
      0x44, 0xAC, 0x00, 0x00, 0x10, 0xB1, 0x02, 0x00, 0x04, 0x00, 0x20, 0x00,
      'd',  'a',  't',  'a',  0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x3F, /* 1.5f */
      0x00, 0x00, 0xC0, 0xBF,                                                 /* -1.5f */
  };
  FILE *f = fopen(g_path, "wb");
  float frames[2];
  float mono[2];

  CHECK(f != NULL);
  if (f == NULL)
    return;
  CHECK_EQ_INT(fwrite(file, 1, sizeof(file), f), (int)sizeof(file));
  fclose(f);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.is_float, 1);
  CHECK_EQ_INT(wav_read_frames(&r, frames, 2), 2);
  CHECK_EQ_DBL(frames[0], 1.5, 1e-6);
  CHECK_EQ_DBL(frames[1], -1.5, 1e-6);
  wav_read_close(&r);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 2), 2);
  CHECK_EQ_DBL(mono[0], 1.0, 1e-6);
  CHECK_EQ_DBL(mono[1], -1.0, 1e-6);
  wav_read_close(&r);
}

TEST(reader_downmixes_channels)
{
  wav_reader r;
  /* two stereo frames: hard left, then equal and opposite */
  int16_t samples[4] = {32000, 0, 16000, -16000};
  float mono[2];

  CHECK_EQ_INT(write_take(g_path, 48000, 2, 16, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.frames, 2);
  CHECK_EQ_INT(r.block, 4);

  CHECK_EQ_INT(wav_read_mono(&r, mono, 2), 2);
  CHECK_EQ_DBL(mono[0], 0.48828, 1e-4); /* (32000/32768) / 2 */
  CHECK_EQ_DBL(mono[1], 0.0, 1e-6);     /* cancels out */

  wav_read_close(&r);
}

TEST(reader_handles_24_bit)
{
  wav_reader r;
  /* one frame at +half scale, one at -half scale, packed little endian */
  unsigned char samples[6] = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0};
  float mono[2];

  CHECK_EQ_INT(write_take(g_path, 44100, 1, 24, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.bits, 24);
  CHECK_EQ_INT(r.frames, 2);

  CHECK_EQ_INT(wav_read_mono(&r, mono, 2), 2);
  CHECK_EQ_DBL(mono[0], 0.5, 1e-6);
  CHECK_EQ_DBL(mono[1], -0.5, 1e-6);

  wav_read_close(&r);
}

TEST(reader_reads_in_several_calls)
{
  wav_reader r;
  int16_t samples[6] = {100, 200, 300, 400, 500, 600};
  float mono[6];

  CHECK_EQ_INT(write_take(g_path, 44100, 1, 16, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);

  CHECK_EQ_INT(wav_read_mono(&r, mono, 4), 4);
  CHECK_EQ_INT(r.position, 4);
  /* the tail is short, and clamped to what is left rather than over-reading */
  CHECK_EQ_INT(wav_read_mono(&r, mono + 4, 4), 2);
  CHECK_EQ_INT(r.position, 6);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 4), 0);

  CHECK_EQ_DBL(mono[5], 600.0 / 32768.0, 1e-6);

  wav_read_close(&r);
}

TEST(reader_skips_unknown_chunks)
{
  /*
   * A hand-built file with a LIST chunk between fmt and data, and an odd sized
   * one at that, so the pad byte has to be skipped too. This is what files that
   * have been through an editor actually look like.
   */
  static const unsigned char file[] = {
      'R',  'I',  'F',  'F',  0x33, 0x00, 0x00, 0x00, 'W',  'A',  'V',  'E',
      'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, /* size 16 */
      0x01, 0x00,                                     /* PCM */
      0x01, 0x00,                                     /* mono */
      0x44, 0xAC, 0x00, 0x00,                         /* 44100 */
      0x88, 0x58, 0x01, 0x00,                         /* byte rate */
      0x02, 0x00,                                     /* align */
      0x10, 0x00,                                     /* 16 bit */
      'L',  'I',  'S',  'T',  0x03, 0x00, 0x00, 0x00, 'I',  'N',  'F',  0x00, /* + pad */
      'd',  'a',  't',  'a',  0x04, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0xC0,
  };
  wav_reader r;
  FILE *f;
  float mono[2];

  f = fopen(g_path, "wb");
  CHECK(f != NULL);
  if (f == NULL)
    return;
  CHECK_EQ_INT(fwrite(file, 1, sizeof(file), f), (int)sizeof(file));
  fclose(f);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.rate, 44100);
  CHECK_EQ_INT(r.frames, 2);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 2), 2);
  CHECK_EQ_DBL(mono[0], 0.5, 1e-4);
  CHECK_EQ_DBL(mono[1], -0.5, 1e-4);

  wav_read_close(&r);
}

TEST(reader_rejects_what_it_cannot_decode)
{
  wav_reader r;
  int16_t samples[2] = {0, 0};
  float mono[2];

  /* not a WAV at all */
  {
    FILE *f = fopen(g_path, "wb");
    CHECK(f != NULL);
    if (f != NULL)
    {
      fputs("this is not audio", f);
      fclose(f);
    }
  }
  CHECK_EQ_INT(wav_read_open(&r, g_path), -1);
  CHECK(r.error != NULL);
  CHECK(r.file == NULL);

  /* too short to hold even a RIFF header */
  {
    FILE *f = fopen(g_path, "wb");
    CHECK(f != NULL);
    if (f != NULL)
    {
      fputs("RIF", f);
      fclose(f);
    }
  }
  CHECK_EQ_INT(wav_read_open(&r, g_path), -1);
  CHECK(r.error != NULL);

  CHECK_EQ_INT(wav_read_open(&r, "audiaki-no-such-file.wav"), -1);
  CHECK(r.error != NULL);

  /* a valid file, but the arguments are not */
  CHECK_EQ_INT(write_take(g_path, 44100, 1, 16, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(wav_read_mono(&r, NULL, 4), -1);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 0), 0);
  wav_read_close(&r);

  /* closing twice is harmless */
  wav_read_close(&r);
}

TEST(reader_survives_a_lying_header)
{
  /* claims 100 frames of data but carries 2: an unpatched interrupted take */
  static const unsigned char file[] = {
      'R',  'I',  'F',  'F',  0x28, 0x00, 0x00, 0x00, 'W',  'A',  'V',  'E',
      'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
      0x44, 0xAC, 0x00, 0x00, 0x88, 0x58, 0x01, 0x00, 0x02, 0x00, 0x10, 0x00,
      'd',  'a',  't',  'a',  0xC8, 0x00, 0x00, 0x00, /* 200 bytes claimed */
      0x00, 0x40, 0x00, 0xC0,                         /* 4 bytes present */
  };
  wav_reader r;
  FILE *f;
  float mono[100];

  f = fopen(g_path, "wb");
  CHECK(f != NULL);
  if (f == NULL)
    return;
  CHECK_EQ_INT(fwrite(file, 1, sizeof(file), f), (int)sizeof(file));
  fclose(f);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.frames, 100);

  /* short read, and the frame count is corrected to the truth */
  CHECK_EQ_INT(wav_read_mono(&r, mono, 100), 2);
  CHECK_EQ_INT(r.frames, 2);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 100), 0);

  wav_read_close(&r);
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
  RUN(reader_round_trips_the_writer);
  RUN(reader_keeps_channels_apart_on_request);
  RUN(the_per_channel_reader_does_not_clamp_float);
  RUN(reader_downmixes_channels);
  RUN(reader_handles_24_bit);
  RUN(reader_reads_in_several_calls);
  RUN(reader_skips_unknown_chunks);
  RUN(reader_rejects_what_it_cannot_decode);
  RUN(reader_survives_a_lying_header);

  rc = TEST_RESULT();
  remove(g_path);
  return rc;
}
