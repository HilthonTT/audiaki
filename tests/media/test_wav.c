/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "media/wav.h"

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
  {
    return -1;
  }
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

TEST(metadata_sits_between_fmt_and_data)
{
  wav_writer w;
  wav_reader r;
  unsigned char file[4096];
  const unsigned char pcm[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  aud_meta meta;
  long size;
  uint32_t meta_bytes;

  aud_meta_defaults(&meta);
  meta.device = "hw:CARD=Box,DEV=0";
  meta.note = "take three";
  meta.rate = 48000;
  meta.channels = 2;
  meta.bits = 16;
  meta.year = 2026;
  meta.month = 8;
  meta.day = 8;

  CHECK_EQ_INT(wav_open_meta(&w, g_path, 48000, 2, 16, 1, &meta), 0);
  meta_bytes = w.meta_bytes;
  CHECK(meta_bytes > 0);
  CHECK_EQ_INT(wav_write(&w, pcm, sizeof(pcm)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  size = slurp(g_path, file, sizeof(file));
  /*
   * A stamped take also reserves the ds64 slot, so everything after the RIFF
   * header sits WAV_DS64_CHUNK_BYTES further in than it does in a plain file.
   */
  CHECK_EQ_INT(w.head_bytes,
               (long)(WAV_HEADER_BYTES + meta_bytes + WAV_DS64_CHUNK_BYTES));
  CHECK_EQ_INT(size, (long)(w.head_bytes + sizeof(pcm)));

  /* the reserved slot, then fmt, then the chunks, then data */
  CHECK(memcmp(file + 12, "JUNK", 4) == 0);
  CHECK_EQ_INT(read_u32(file + 16), WAV_DS64_BODY_BYTES);
  CHECK(memcmp(file + 48, "fmt ", 4) == 0);
  CHECK(memcmp(file + 72, "LIST", 4) == 0);
  CHECK(memcmp(file + 72 + meta_bytes, "data", 4) == 0);

  /* it is still a plain RIFF file: the slot was not needed and was not used */
  CHECK(memcmp(file + 0, "RIFF", 4) == 0);

  /* both sizes account for what is between them */
  CHECK_EQ_INT(read_u32(file + 4), (long)(w.head_bytes - 8u + sizeof(pcm)));
  CHECK_EQ_INT(read_u32(file + 72 + meta_bytes + 4), (long)sizeof(pcm));
  CHECK(memcmp(file + w.head_bytes, pcm, sizeof(pcm)) == 0);

  /* and the reader finds the audio and the description of it */
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.rate, 48000);
  CHECK_EQ_INT(r.channels, 2);
  CHECK_EQ_INT(r.frames, 2);
  CHECK(r.meta.present);
  CHECK_EQ_STR(r.meta.note, "take three");
  CHECK_EQ_STR(r.meta.device, "hw:CARD=Box,DEV=0");
  CHECK_EQ_STR(r.meta.recorded, "2026-08-08 00:00:00");
  wav_read_close(&r);

  remove(g_path);
}

TEST(a_plain_file_keeps_the_44_byte_header_and_the_4_gb_cap)
{
  wav_writer w;
  unsigned char file[128];
  const unsigned char pcm[4] = {1, 2, 3, 4};

  /*
   * --no-metadata asks for the canonical header and gets exactly that: no
   * reserved slot, and therefore no way past 4 GB, because a plain header has
   * nowhere to put a 64-bit size.
   */
  CHECK_EQ_INT(wav_open(&w, g_path, 44100, 1, 16, 1), 0);
  CHECK_EQ_INT(w.large, 0);
  CHECK_EQ_INT(w.head_bytes, WAV_HEADER_BYTES);
  CHECK_EQ_INT(wav_write(&w, pcm, sizeof(pcm)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  CHECK_EQ_INT(slurp(g_path, file, sizeof(file)), (long)(WAV_HEADER_BYTES + sizeof(pcm)));
  CHECK(memcmp(file + 12, "fmt ", 4) == 0);
  CHECK(memcmp(file + 36, "data", 4) == 0);

  remove(g_path);
}

TEST(a_take_that_outgrows_riff_is_promoted_to_rf64)
{
  wav_writer w;
  unsigned char file[256];
  const unsigned char pcm[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint64_t huge = 5ull * 1024ull * 1024ull * 1024ull; /* 5 GB, comfortably over */
  uint32_t head;

  CHECK_EQ_INT(wav_open_ex(&w, g_path, 48000, 2, 16, 1, NULL, WAV_OPEN_LARGE), 0);
  CHECK_EQ_INT(w.large, 1);
  head = w.head_bytes;
  CHECK_EQ_INT(head, WAV_HEADER_BYTES + WAV_DS64_CHUNK_BYTES);
  CHECK_EQ_INT(wav_write(&w, pcm, sizeof(pcm)), 0);

  /*
   * The payload is claimed rather than written. Actually recording five
   * gigabytes to check the header arithmetic would take minutes and a spare
   * disk; what is under test is what wav_close() writes into the reserved slot,
   * and that is a pure function of this number.
   */
  w.data_bytes = huge;
  CHECK_EQ_INT(wav_close(&w), 0);

  CHECK(slurp(g_path, file, sizeof(file)) > 0);

  /* the magic changed, and both 32-bit sizes now point at ds64 */
  CHECK(memcmp(file + 0, "RF64", 4) == 0);
  CHECK_EQ_INT(read_u32(file + 4), 0xFFFFFFFF);
  CHECK(memcmp(file + 12, "ds64", 4) == 0);
  CHECK_EQ_INT(read_u32(file + 16), WAV_DS64_BODY_BYTES);
  CHECK(memcmp(file + head - 8, "data", 4) == 0);
  CHECK_EQ_INT(read_u32(file + head - 4), 0xFFFFFFFF);

  /* and the real sizes are in the slot, in 64 bits, low word first */
  CHECK_EQ_INT(read_u32(file + 20), (long)((head - 8u + huge) & 0xFFFFFFFFu));
  CHECK_EQ_INT(read_u32(file + 24), (long)((head - 8u + huge) >> 32));
  CHECK_EQ_INT(read_u32(file + 28), (long)(huge & 0xFFFFFFFFu));
  CHECK_EQ_INT(read_u32(file + 32), (long)(huge >> 32));
  /* the frame count: 5 GB of 16-bit stereo */
  CHECK_EQ_INT(read_u32(file + 36), (long)((huge / 4u) & 0xFFFFFFFFu));
  CHECK_EQ_INT(read_u32(file + 40), (long)((huge / 4u) >> 32));
  /* no table, because the data chunk is the only one that got big */
  CHECK_EQ_INT(read_u32(file + 44), 0);

  remove(g_path);
}

TEST(a_take_that_stayed_small_keeps_its_slot_as_junk)
{
  wav_writer w;
  wav_reader r;
  unsigned char file[256];
  const unsigned char pcm[8] = {0, 0, 0, 0x40, 0, 0, 0, 0xC0};

  CHECK_EQ_INT(wav_open_ex(&w, g_path, 48000, 2, 16, 1, NULL, WAV_OPEN_LARGE), 0);
  CHECK_EQ_INT(wav_write(&w, pcm, sizeof(pcm)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  CHECK(slurp(g_path, file, sizeof(file)) > 0);

  /*
   * The whole point of reserving rather than promoting eagerly: a file that
   * did not need the room is an ordinary RIFF/WAVE with one chunk in it that
   * every reader of the format already skips.
   */
  CHECK(memcmp(file + 0, "RIFF", 4) == 0);
  CHECK(memcmp(file + 12, "JUNK", 4) == 0);
  CHECK_EQ_INT(read_u32(file + 4), (long)(w.head_bytes - 8u + sizeof(pcm)));

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.rate, 48000);
  CHECK_EQ_INT(r.frames, 2);
  wav_read_close(&r);

  remove(g_path);
}

TEST(the_reader_takes_rf64_and_bw64)
{
  /*
   * Hand-built, because the sizes that matter are the ones a 4 GB file would
   * have and this has to stay a test. RF64 does not require the real length to
   * be large - 0xFFFFFFFF only means "read it from ds64" - so a short file with
   * the full layout exercises every branch the big one would.
   */
  static const unsigned char base[] = {
      'R',  'F',  '6',  '4',  0xFF, 0xFF, 0xFF, 0xFF, 'W',  'A',
      'V',  'E',  'd',  's',  '6',  '4',  0x1C, 0x00, 0x00, 0x00, /* ds64, 28 */
      0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* riffSize */
      0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* dataSize */
      0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* frames   */
      0x00, 0x00, 0x00, 0x00,                                     /* no table */
      'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, 0x01, 0x00, /* PCM */
      0x01, 0x00,                                                 /* mono */
      0x44, 0xAC, 0x00, 0x00,                                     /* 44100 */
      0x88, 0x58, 0x01, 0x00,                                     /* byte rate */
      0x02, 0x00,                                                 /* align */
      0x10, 0x00,                                                 /* 16 bit */
      'd',  'a',  't',  'a',  0xFF, 0xFF, 0xFF, 0xFF,             /* size from ds64 */
      0x00, 0x40, 0x00, 0xC0,
  };
  unsigned char file[sizeof(base)];
  wav_reader r;
  float mono[2];

  for (int variant = 0; variant < 2; variant++)
  {
    FILE *f;

    memcpy(file, base, sizeof(base));
    if (variant == 1)
    {
      memcpy(file, "BW64", 4); /* the ITU's name for the same layout */
    }

    f = fopen(g_path, "wb");
    CHECK(f != NULL);
    if (f == NULL)
    {
      return;
    }
    CHECK_EQ_INT(fwrite(file, 1, sizeof(file), f), (int)sizeof(file));
    fclose(f);

    CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
    CHECK_EQ_INT(r.rate, 44100);
    CHECK_EQ_INT(r.channels, 1);
    /* four bytes of payload, from ds64 rather than from the 0xFFFFFFFF */
    CHECK_EQ_INT(r.frames, 2);
    CHECK_EQ_INT(wav_read_mono(&r, mono, 2), 2);
    CHECK_EQ_DBL(mono[0], 0.5, 1e-4);
    CHECK_EQ_DBL(mono[1], -0.5, 1e-4);
    wav_read_close(&r);
  }

  remove(g_path);
}

TEST(an_rf64_file_with_no_ds64_is_refused)
{
  static const unsigned char file[] = {
      'R',  'F',  '6',  '4',  0xFF, 0xFF, 0xFF, 0xFF, 'W',  'A',  'V',  'E',
      'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
      0x44, 0xAC, 0x00, 0x00, 0x88, 0x58, 0x01, 0x00, 0x02, 0x00, 0x10, 0x00,
      'd',  'a',  't',  'a',  0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x40, 0x00, 0xC0,
  };
  wav_reader r;
  FILE *f = fopen(g_path, "wb");

  CHECK(f != NULL);
  if (f == NULL)
  {
    return;
  }
  CHECK_EQ_INT(fwrite(file, 1, sizeof(file), f), (int)sizeof(file));
  fclose(f);

  /* there is no length to be had, and guessing one would invent audio */
  CHECK_EQ_INT(wav_read_open(&r, g_path), -1);
  CHECK(r.error != NULL);

  remove(g_path);
}

TEST(a_take_can_be_carried_on_in_the_same_file)
{
  wav_writer w;
  wav_reader r;
  const unsigned char first[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  const unsigned char second[8] = {9, 10, 11, 12, 13, 14, 15, 16};
  unsigned char file[4096]; /* the metadata alone is most of a kilobyte */
  aud_meta meta;
  uint32_t head;

  aud_meta_defaults(&meta);
  meta.device = "hw:CARD=Box,DEV=0";
  meta.rate = 48000;
  meta.channels = 2;
  meta.bits = 16;

  /* the take as it was when the device went: closed, patched, complete */
  CHECK_EQ_INT(wav_open_meta(&w, g_path, 48000, 2, 16, 1, &meta), 0);
  head = w.head_bytes;
  CHECK_EQ_INT(wav_write(&w, first, sizeof(first)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.frames, 2);
  CHECK_EQ_INT(r.has_ds64_slot, 1);
  wav_read_close(&r);

  /* the device came back, and the rest of the take goes on the end of it */
  CHECK_EQ_INT(wav_open_append(&w, g_path, 48000, 2, 16), 0);
  CHECK_EQ_INT((long long)w.data_bytes, (long long)sizeof(first));
  CHECK_EQ_INT(w.head_bytes, (long)head);
  CHECK_EQ_INT(w.large, 1); /* the reserved slot came back with it */
  CHECK_EQ_INT(wav_write(&w, second, sizeof(second)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  /* one file, both halves, in order, and the metadata still in front of them */
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.frames, 4);
  CHECK(r.meta.present);
  CHECK_EQ_STR(r.meta.device, "hw:CARD=Box,DEV=0");
  wav_read_close(&r);

  CHECK(slurp(g_path, file, sizeof(file)) > 0);
  CHECK(memcmp(file + head, first, sizeof(first)) == 0);
  CHECK(memcmp(file + head + sizeof(first), second, sizeof(second)) == 0);

  remove(g_path);
}

TEST(carrying_on_refuses_a_stream_that_does_not_match)
{
  wav_writer w;

  CHECK_EQ_INT(wav_open(&w, g_path, 48000, 2, 16, 1), 0);
  CHECK_EQ_INT(wav_write(&w, "abcd", 4), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  /*
   * A device that comes back at another rate, or another width, is a different
   * recording - laying it on the end of this one would make a file that lies
   * about what is in it.
   */
  CHECK_EQ_INT(wav_open_append(&w, g_path, 44100, 2, 16), -1);
  CHECK_EQ_INT(wav_open_append(&w, g_path, 48000, 1, 16), -1);
  CHECK_EQ_INT(wav_open_append(&w, g_path, 48000, 2, 24), -1);

  /* and a file that is not there at all */
  CHECK_EQ_INT(wav_open_append(&w, "audiaki-no-such-take.wav", 48000, 2, 16), -1);

  /* the matching one still works, so the refusals above were about the stream */
  CHECK_EQ_INT(wav_open_append(&w, g_path, 48000, 2, 16), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  remove(g_path);
}

TEST(carrying_on_writes_over_the_pad_byte)
{
  wav_writer w;
  wav_reader r;
  const unsigned char odd[3] = {1, 2, 3}; /* 3 frames of 8-bit mono */
  const unsigned char more[2] = {7, 8};

  /*
   * An odd payload is followed by a pad byte that RIFF counts and the audio
   * does not. Carrying on has to put the next frame over it rather than after
   * it, or the pad would end up inside the take as a click.
   *
   * Mono, so that an odd number of bytes is still a whole number of frames -
   * which is the only kind of file this is meant to reopen.
   */
  CHECK_EQ_INT(wav_open(&w, g_path, 8000, 1, 8, 1), 0);
  CHECK_EQ_INT(wav_write(&w, odd, sizeof(odd)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  CHECK_EQ_INT(wav_open_append(&w, g_path, 8000, 1, 8), 0);
  CHECK_EQ_INT((long long)w.data_bytes, 3);
  CHECK_EQ_INT(wav_write(&w, more, sizeof(more)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  /* five frames, not six: the pad was written over rather than kept */
  CHECK_EQ_INT(r.frames, 5);
  wav_read_close(&r);

  {
    unsigned char file[128];
    long size = slurp(g_path, file, sizeof(file));

    CHECK_EQ_INT(size, WAV_HEADER_BYTES + 6); /* 5 of payload, and a new pad */
    CHECK_EQ_INT(file[WAV_HEADER_BYTES + 3], 7);
    CHECK_EQ_INT(file[WAV_HEADER_BYTES + 4], 8);
  }

  remove(g_path);
}

TEST(carrying_on_refuses_a_file_with_something_after_the_audio)
{
  wav_writer w;
  FILE *f;

  CHECK_EQ_INT(wav_open(&w, g_path, 48000, 2, 16, 1), 0);
  CHECK_EQ_INT(wav_write(&w, "abcd", 4), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  /* what an editor that retags a file in place leaves behind */
  f = fopen(g_path, "ab");
  CHECK(f != NULL);
  if (f == NULL)
  {
    return;
  }
  fwrite("LIST\004\000\000\000INFO", 1, 12, f);
  fclose(f);

  /*
   * Appending here would write the next frame over somebody's tags. Refused,
   * so the take carries on in a second file instead - which is worse than one
   * file and much better than a damaged one.
   */
  CHECK_EQ_INT(wav_open_append(&w, g_path, 48000, 2, 16), -1);

  remove(g_path);
}

TEST(a_file_without_metadata_reads_back_empty)
{
  wav_writer w;
  wav_reader r;
  const unsigned char pcm[4] = {0, 0, 0, 0};

  CHECK_EQ_INT(wav_open(&w, g_path, 44100, 1, 16, 1), 0);
  CHECK_EQ_INT(w.meta_bytes, 0);
  CHECK_EQ_INT(wav_write(&w, pcm, sizeof(pcm)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.meta.present, 0);
  CHECK_EQ_STR(r.meta.note, "");
  wav_read_close(&r);

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
  {
    return -1;
  }
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
  {
    return;
  }
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

TEST(reader_seeks_both_ways)
{
  wav_reader r;
  int16_t samples[6] = {100, 200, 300, 400, 500, 600};
  float mono[6];

  CHECK_EQ_INT(write_take(g_path, 44100, 1, 16, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);

  CHECK_EQ_INT(wav_read_seek(&r, 4), 0);
  CHECK_EQ_INT(r.position, 4);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 1), 1);
  CHECK_EQ_DBL(mono[0], 500.0 / 32768.0, 1e-6);

  /* backwards, which is what a cursor key mostly asks for */
  CHECK_EQ_INT(wav_read_seek(&r, 1), 0);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 1), 1);
  CHECK_EQ_DBL(mono[0], 200.0 / 32768.0, 1e-6);

  CHECK_EQ_INT(wav_read_seek(&r, 0), 0);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 6), 6);
  CHECK_EQ_DBL(mono[0], 100.0 / 32768.0, 1e-6);

  wav_read_close(&r);
}

TEST(a_seek_past_the_end_lands_on_it)
{
  wav_reader r;
  int16_t samples[4] = {1, 2, 3, 4};
  float mono[4];

  CHECK_EQ_INT(write_take(g_path, 44100, 1, 16, samples, sizeof(samples)), 0);
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);

  /* the End key, and any seek that overshoots: the end, not a failure */
  CHECK_EQ_INT(wav_read_seek(&r, 9999), 0);
  CHECK_EQ_INT(r.position, 4);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 4), 0);

  /* and it is still a usable reader afterwards */
  CHECK_EQ_INT(wav_read_seek(&r, 2), 0);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 4), 2);

  wav_read_close(&r);
}

TEST(a_seek_counts_from_the_audio_not_the_file)
{
  wav_writer w;
  wav_reader r;
  const int16_t pcm[4] = {11, 22, 33, 44};
  aud_meta meta;
  float mono[2];

  /*
   * A stamped take has a LIST/INFO and a bext chunk ahead of its audio, so
   * frame zero is nowhere near byte 44. A seek that assumed the canonical
   * header would land in the middle of the metadata and decode it as samples.
   */
  aud_meta_defaults(&meta);
  meta.device = "hw:CARD=Box,DEV=0";
  meta.note = "somewhere to seek past";
  meta.rate = 44100;
  meta.channels = 1;
  meta.bits = 16;

  CHECK_EQ_INT(wav_open_meta(&w, g_path, 44100, 1, 16, 1, &meta), 0);
  CHECK(w.meta_bytes > 0);
  CHECK_EQ_INT(wav_write(&w, pcm, sizeof(pcm)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.frames, 4);
  CHECK_EQ_INT(wav_read_seek(&r, 3), 0);
  CHECK_EQ_INT(wav_read_mono(&r, mono, 2), 1);
  CHECK_EQ_DBL(mono[0], 44.0 / 32768.0, 1e-6);

  wav_read_close(&r);
}

TEST(seeking_a_closed_reader_is_refused)
{
  wav_reader r;

  memset(&r, 0, sizeof(r));
  CHECK_EQ_INT(wav_read_seek(&r, 0), -1);
  CHECK_EQ_INT(wav_read_seek(NULL, 0), -1);
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
  {
    return;
  }
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

/* Write `bytes` to the scratch file. Returns non-zero when it got there. */
static int put_file(const unsigned char *bytes, size_t n)
{
  FILE *f = fopen(g_path, "wb");

  if (f == NULL)
  {
    return 0;
  }
  if (fwrite(bytes, 1, n, f) != n)
  {
    fclose(f);
    return 0;
  }
  fclose(f);
  return 1;
}

TEST(the_reserved_slot_is_only_the_one_at_the_front)
{
  /*
   * write_header() promotes a file to RF64 by writing ds64 at offset 12, which
   * is the only place either RF64 or wav_open_ex() puts one. A JUNK chunk of
   * the same length further in is somebody else's padding, and taking it for
   * the slot would have a later promotion write over whatever really is there.
   */
  static const unsigned char slotted[] = {
      'R',  'I',  'F',  'F',  0x4C, 0x00, 0x00, 0x00, 'W',  'A',  'V',  'E',  'J',
      'U',  'N',  'K',  0x1C, 0x00, 0x00, 0x00, /* 28, the ds64 body */
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
      0x00, 0x44, 0xAC, 0x00, 0x00, 0x88, 0x58, 0x01, 0x00, 0x02, 0x00, 0x10, 0x00,
      'd',  'a',  't',  'a',  0x04, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0xC0,
  };
  /* the same file with a LIST chunk in front of the JUNK, so it is not at 12 */
  static const unsigned char padded[] = {
      'R',  'I',  'F',  'F',  0x58, 0x00, 0x00, 0x00, 'W',  'A',  'V',  'E',  'L',  'I',
      'S',  'T',  0x04, 0x00, 0x00, 0x00, 'I',  'N',  'F',  'O',  'J',  'U',  'N',  'K',
      0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 'f',  'm',  't',  ' ',  0x10, 0x00, 0x00, 0x00, 0x01, 0x00,
      0x01, 0x00, 0x44, 0xAC, 0x00, 0x00, 0x88, 0x58, 0x01, 0x00, 0x02, 0x00, 0x10, 0x00,
      'd',  'a',  't',  'a',  0x04, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0xC0,
  };
  wav_reader r;

  CHECK(put_file(slotted, sizeof(slotted)));
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.frames, 2);
  CHECK_EQ_INT(r.has_ds64_slot, 1);
  wav_read_close(&r);

  CHECK(put_file(padded, sizeof(padded)));
  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.frames, 2); /* still a perfectly readable file */
  CHECK_EQ_INT(r.has_ds64_slot, 0);
  wav_read_close(&r);
}

TEST(reader_finds_metadata_appended_after_the_audio)
{
  /*
   * What an editor that retags a file in place produces: the audio where it
   * was, and a LIST/INFO chunk stuck on the end rather than moved in ahead of
   * it. audiaki writes its own before the data, but it has to read both.
   */
  static const unsigned char file[] = {
      'R',
      'I',
      'F',
      'F',
      0x40,
      0x00,
      0x00,
      0x00,
      'W',
      'A',
      'V',
      'E',
      'f',
      'm',
      't',
      ' ',
      0x10,
      0x00,
      0x00,
      0x00, /* size 16 */
      0x01,
      0x00, /* PCM */
      0x01,
      0x00, /* mono */
      0x44,
      0xAC,
      0x00,
      0x00, /* 44100 */
      0x88,
      0x58,
      0x01,
      0x00, /* byte rate */
      0x02,
      0x00, /* align */
      0x10,
      0x00, /* 16 bit */
      'd',
      'a',
      't',
      'a',
      0x04,
      0x00,
      0x00,
      0x00,
      0x00,
      0x40,
      0x00,
      0xC0,
      /* LIST/INFO with one ICMT tag, entirely after the payload */
      'L',
      'I',
      'S',
      'T',
      0x14,
      0x00,
      0x00,
      0x00,
      'I',
      'N',
      'F',
      'O',
      'I',
      'C',
      'M',
      'T',
      0x08,
      0x00,
      0x00,
      0x00,
      't',
      'a',
      'i',
      'l',
      'e',
      'd',
      '!',
      0x00,
  };
  wav_reader r;
  FILE *f;
  float mono[2];

  f = fopen(g_path, "wb");
  CHECK(f != NULL);
  if (f == NULL)
  {
    return;
  }
  CHECK_EQ_INT(fwrite(file, 1, sizeof(file), f), (int)sizeof(file));
  fclose(f);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.meta.present, 1);
  CHECK_EQ_STR(r.meta.note, "tailed!");

  /* and the audio still reads from the right place afterwards */
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
  {
    return;
  }
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
  RUN(metadata_sits_between_fmt_and_data);
  RUN(a_plain_file_keeps_the_44_byte_header_and_the_4_gb_cap);
  RUN(a_take_that_outgrows_riff_is_promoted_to_rf64);
  RUN(a_take_that_stayed_small_keeps_its_slot_as_junk);
  RUN(the_reader_takes_rf64_and_bw64);
  RUN(an_rf64_file_with_no_ds64_is_refused);
  RUN(a_take_can_be_carried_on_in_the_same_file);
  RUN(carrying_on_refuses_a_stream_that_does_not_match);
  RUN(carrying_on_writes_over_the_pad_byte);
  RUN(carrying_on_refuses_a_file_with_something_after_the_audio);
  RUN(a_file_without_metadata_reads_back_empty);
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
  RUN(reader_seeks_both_ways);
  RUN(a_seek_past_the_end_lands_on_it);
  RUN(a_seek_counts_from_the_audio_not_the_file);
  RUN(seeking_a_closed_reader_is_refused);
  RUN(reader_skips_unknown_chunks);
  RUN(the_reserved_slot_is_only_the_one_at_the_front);
  RUN(reader_finds_metadata_appended_after_the_audio);
  RUN(reader_rejects_what_it_cannot_decode);
  RUN(reader_survives_a_lying_header);

  rc = TEST_RESULT();
  remove(g_path);
  return rc;
}
