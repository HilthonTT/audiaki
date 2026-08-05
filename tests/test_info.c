/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "info.h"
#include "wav.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char g_path[256];

/* Write a file --info can be pointed at. Returns 0 on success. */
static int write_take(uint32_t rate, uint16_t channels, uint16_t bits,
                      const void *payload, size_t bytes)
{
  wav_writer w;

  if (wav_open(&w, g_path, rate, channels, bits, 1) != 0)
    return -1;
  if (wav_write(&w, payload, bytes) != 0)
  {
    wav_discard(&w);
    return -1;
  }
  return wav_close(&w);
}

TEST(reports_what_the_header_says)
{
  aud_info_report r;
  int16_t samples[8] = {0, 0, 0, 0, 0, 0, 0, 0};

  CHECK_EQ_INT(write_take(48000, 2, 16, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(aud_info_analyse(g_path, &r), 0);

  CHECK_EQ_INT(r.rate, 48000);
  CHECK_EQ_INT(r.channels, 2);
  CHECK_EQ_INT(r.bits, 16);
  CHECK_EQ_INT(r.is_float, 0);
  CHECK_EQ_INT(r.frames, 4);
  CHECK_EQ_INT(r.samples, 8);
  CHECK_EQ_DBL(r.duration, 4.0 / 48000.0, 1e-9);

  /* digital silence is a real answer, not a missing one */
  CHECK_EQ_DBL(r.peak, 0.0, 1e-12);
  CHECK_EQ_DBL(r.rms, 0.0, 1e-12);
  CHECK_EQ_INT(r.clipped, 0);
}

TEST(measures_each_channel_separately)
{
  aud_info_report r;
  /* four stereo frames; the right channel is quiet apart from one full stop */
  int16_t samples[8] = {0, 0, 16384, 0, -16384, 0, 32767, -32768};

  CHECK_EQ_INT(write_take(44100, 2, 16, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(aud_info_analyse(g_path, &r), 0);

  CHECK_EQ_INT(r.frames, 4);
  CHECK_EQ_DBL(r.channel_peak[0], 32767.0 / 32768.0, 1e-6);
  CHECK_EQ_DBL(r.channel_peak[1], 1.0, 1e-6);

  /* the loudest sample anywhere, which a downmix would have averaged away */
  CHECK_EQ_DBL(r.peak, 1.0, 1e-6);

  CHECK_EQ_DBL(r.channel_rms[0], 0.612372, 1e-4);
  CHECK_EQ_DBL(r.channel_rms[1], 0.5, 1e-4);
  CHECK_EQ_DBL(r.rms, 0.559017, 1e-4);

  /* a lopsided channel shows up as a DC offset the other one does not have */
  CHECK_EQ_DBL(r.channel_dc[0], 0.25, 1e-4);
  CHECK_EQ_DBL(r.channel_dc[1], -0.25, 1e-4);
}

TEST(counts_samples_that_reached_full_scale)
{
  aud_info_report r;
  /*
   * The threshold is on magnitude, so it sits one step below the top of the
   * range: signed PCM can reach -32768 but only +32767, and a sample that
   * loud is at full scale whichever way up it is.
   */
  int16_t samples[4] = {32767, -32768, 32766, -32766};

  CHECK_EQ_INT(write_take(44100, 1, 16, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(aud_info_analyse(g_path, &r), 0);

  CHECK_EQ_INT(r.frames, 4);
  CHECK_EQ_INT(r.clipped, 2);
}

TEST(clipping_is_judged_at_the_files_own_depth)
{
  aud_info_report r;
  /* 24 bit: 0x7FFFFF is full scale, and 0x7FFF is nowhere near it */
  unsigned char samples[6] = {0xFF, 0xFF, 0x7F, 0xFF, 0x7F, 0x00};

  CHECK_EQ_INT(write_take(44100, 1, 24, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(aud_info_analyse(g_path, &r), 0);

  CHECK_EQ_INT(r.frames, 2);
  CHECK_EQ_INT(r.clipped, 1);
  CHECK_EQ_DBL(r.channel_peak[0], 8388607.0 / 8388608.0, 1e-6);
}

TEST(the_noise_floor_ignores_the_loud_part)
{
  aud_info_report r;
  /*
   * A second of quiet followed by a second of loud, at a rate low enough to
   * keep the file small. The floor should describe the quiet half; the peak
   * and the RMS should not.
   */
  static int16_t samples[2000];

  for (size_t i = 0; i < 1000; i++)
    samples[i] = (i % 2 == 0) ? 100 : -100;
  for (size_t i = 1000; i < 2000; i++)
    samples[i] = (i % 2 == 0) ? 30000 : -30000;

  CHECK_EQ_INT(write_take(1000, 1, 16, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(aud_info_analyse(g_path, &r), 0);

  CHECK_EQ_INT(r.frames, 2000);
  CHECK_EQ_DBL(r.peak, 30000.0 / 32768.0, 1e-4);
  CHECK_EQ_DBL(r.noise_floor, 100.0 / 32768.0, 1e-4);
  CHECK(r.rms > r.noise_floor * 10.0);
}

TEST(a_truncated_take_is_reported_as_short)
{
  aud_info_report r;
  /* claims 100 frames of 16 bit mono and carries two */
  static const unsigned char file[] = {
      'R',  'I',  'F',  'F',  0x28, 0x00, 0x00, 0x00, 'W',  'A',  'V',  'E',
      'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
      0x44, 0xAC, 0x00, 0x00, 0x88, 0x58, 0x01, 0x00, 0x02, 0x00, 0x10, 0x00,
      'd',  'a',  't',  'a',  0xC8, 0x00, 0x00, 0x00, /* 200 bytes claimed */
      0x00, 0x40, 0x00, 0xC0,                         /* 4 bytes present */
  };
  FILE *f = fopen(g_path, "wb");

  CHECK(f != NULL);
  if (f == NULL)
    return;
  CHECK_EQ_INT(fwrite(file, 1, sizeof(file), f), (int)sizeof(file));
  fclose(f);

  CHECK_EQ_INT(aud_info_analyse(g_path, &r), 0);
  CHECK_EQ_INT(r.frames, 2);
  CHECK_EQ_INT(r.header_frames, 100);
  /* the duration is what is there to listen to, not what was promised */
  CHECK_EQ_DBL(r.duration, 2.0 / 44100.0, 1e-9);
}

TEST(a_file_it_cannot_read_is_an_error)
{
  aud_info_report r;

  CHECK_EQ_INT(aud_info_analyse("audiaki-no-such-file.wav", &r), -1);
  CHECK_EQ_INT(aud_info_analyse(NULL, &r), -1);
  CHECK_EQ_INT(aud_info_analyse(g_path, NULL), -1);
}

int main(void)
{
  int rc;

  snprintf(g_path, sizeof(g_path), "audiaki-info-%ld.wav", (long)getpid());

  RUN(reports_what_the_header_says);
  RUN(measures_each_channel_separately);
  RUN(counts_samples_that_reached_full_scale);
  RUN(clipping_is_judged_at_the_files_own_depth);
  RUN(the_noise_floor_ignores_the_loud_part);
  RUN(a_truncated_take_is_reported_as_short);
  RUN(a_file_it_cannot_read_is_an_error);

  rc = TEST_RESULT();
  remove(g_path);
  return rc;
}
