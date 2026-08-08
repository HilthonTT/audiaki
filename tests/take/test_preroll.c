/* SPDX-License-Identifier: MIT */
#include "take/preroll.h"

#include "test_util.h"

#include <stdlib.h>
#include <string.h>

/*
 * Frames are two bytes here - a stand-in for one channel of s16_le - so a frame
 * boundary that gets miscounted shows up as a shifted byte rather than as
 * plausible looking audio.
 */
#define FRAME_BYTES 2u

static void fill(unsigned char *buf, size_t frames, unsigned char base)
{
  for (size_t i = 0; i < frames; i++)
  {
    buf[i * FRAME_BYTES] = (unsigned char)(base + i);
    buf[i * FRAME_BYTES + 1] = (unsigned char)0xA0;
  }
}

/* Copy the ring out through its segments, oldest first, and return the count. */
static size_t drain(const aud_preroll *pr, unsigned char *out, size_t max_frames)
{
  aud_preroll_segment seg[2];
  unsigned n = aud_preroll_segments(pr, seg);
  size_t total = 0;

  for (unsigned i = 0; i < n; i++)
  {
    CHECK(seg[i].frames > 0);
    CHECK(seg[i].data != NULL);
    if (total + seg[i].frames > max_frames)
    {
      return total;
    }
    memcpy(out + total * FRAME_BYTES, seg[i].data, seg[i].frames * FRAME_BYTES);
    total += seg[i].frames;
  }

  return total;
}

TEST(init_takes_the_capacity_literally)
{
  aud_preroll pr;

  CHECK_EQ_INT(aud_preroll_init(&pr, 100, FRAME_BYTES), 0);
  CHECK_EQ_INT(aud_preroll_capacity(&pr), 100);
  CHECK_EQ_INT(aud_preroll_filled(&pr), 0);
  CHECK_EQ_INT(aud_preroll_bytes(&pr), 0);
  aud_preroll_free(&pr);
}

TEST(init_rejects_a_zero_or_overflowing_size)
{
  aud_preroll pr;

  CHECK_EQ_INT(aud_preroll_init(&pr, 0, FRAME_BYTES), -1);
  CHECK_EQ_INT(aud_preroll_init(&pr, 16, 0), -1);
  CHECK_EQ_INT(aud_preroll_init(NULL, 16, FRAME_BYTES), -1);

  /* frames * frame_bytes must not wrap into a small allocation */
  CHECK_EQ_INT(aud_preroll_init(&pr, (size_t)-1 / 2, 8), -1);
}

TEST(a_partial_fill_comes_back_in_one_segment)
{
  aud_preroll pr;
  aud_preroll_segment seg[2];
  unsigned char in[10 * FRAME_BYTES];
  unsigned char out[10 * FRAME_BYTES];

  CHECK_EQ_INT(aud_preroll_init(&pr, 32, FRAME_BYTES), 0);
  fill(in, 10, 0);
  aud_preroll_push(&pr, in, 10);

  CHECK_EQ_INT(aud_preroll_filled(&pr), 10);
  CHECK_EQ_INT(aud_preroll_bytes(&pr), 10 * FRAME_BYTES);
  CHECK_EQ_INT(aud_preroll_segments(&pr, seg), 1);

  CHECK_EQ_INT(drain(&pr, out, 10), 10);
  CHECK_EQ_INT(memcmp(out, in, sizeof(in)), 0);

  aud_preroll_free(&pr);
}

TEST(an_empty_ring_has_no_segments)
{
  aud_preroll pr;
  aud_preroll_segment seg[2];

  CHECK_EQ_INT(aud_preroll_init(&pr, 8, FRAME_BYTES), 0);
  CHECK_EQ_INT(aud_preroll_segments(&pr, seg), 0);
  CHECK(seg[0].data == NULL);
  CHECK_EQ_INT(seg[0].frames, 0);
  aud_preroll_free(&pr);
}

TEST(the_oldest_frames_are_dropped_once_it_is_full)
{
  aud_preroll pr;
  unsigned char in[20 * FRAME_BYTES];
  unsigned char out[16 * FRAME_BYTES];

  CHECK_EQ_INT(aud_preroll_init(&pr, 16, FRAME_BYTES), 0);
  fill(in, 20, 0);

  aud_preroll_push(&pr, in, 12);
  aud_preroll_push(&pr, in + 12 * FRAME_BYTES, 8); /* wraps, and overflows by 4 */

  CHECK_EQ_INT(aud_preroll_filled(&pr), 16);
  CHECK_EQ_INT(drain(&pr, out, 16), 16);

  /* frames 0..3 fell off the front; what is left is 4..19 in order */
  CHECK_EQ_INT(memcmp(out, in + 4 * FRAME_BYTES, sizeof(out)), 0);

  aud_preroll_free(&pr);
}

TEST(a_push_larger_than_the_ring_keeps_only_the_newest)
{
  aud_preroll pr;
  unsigned char in[100 * FRAME_BYTES];
  unsigned char out[16 * FRAME_BYTES];

  CHECK_EQ_INT(aud_preroll_init(&pr, 16, FRAME_BYTES), 0);
  fill(in, 100, 0);

  aud_preroll_push(&pr, in, 5); /* thrown away by what follows */
  aud_preroll_push(&pr, in, 100);

  CHECK_EQ_INT(aud_preroll_filled(&pr), 16);
  CHECK_EQ_INT(drain(&pr, out, 16), 16);
  CHECK_EQ_INT(memcmp(out, in + 84 * FRAME_BYTES, sizeof(out)), 0);

  aud_preroll_free(&pr);
}

TEST(a_push_of_exactly_the_capacity_replaces_everything)
{
  aud_preroll pr;
  unsigned char in[16 * FRAME_BYTES];
  unsigned char out[16 * FRAME_BYTES];

  CHECK_EQ_INT(aud_preroll_init(&pr, 16, FRAME_BYTES), 0);

  fill(in, 16, 0);
  aud_preroll_push(&pr, in, 7);

  fill(in, 16, 100);
  aud_preroll_push(&pr, in, 16);

  CHECK_EQ_INT(aud_preroll_filled(&pr), 16);
  CHECK_EQ_INT(drain(&pr, out, 16), 16);
  CHECK_EQ_INT(memcmp(out, in, sizeof(out)), 0);

  aud_preroll_free(&pr);
}

/*
 * The wrap is the case worth hammering: a period size that does not divide the
 * capacity walks the head past the end on a different frame every time round.
 */
TEST(the_contents_stay_in_order_across_many_wraps)
{
  aud_preroll pr;
  unsigned char in[7 * FRAME_BYTES];
  unsigned char out[16 * FRAME_BYTES];
  unsigned char next = 0;

  CHECK_EQ_INT(aud_preroll_init(&pr, 16, FRAME_BYTES), 0);

  for (int round = 0; round < 200; round++)
  {
    fill(in, 7, next);
    next = (unsigned char)(next + 7);
    aud_preroll_push(&pr, in, 7);

    if (round >= 3)
    {
      CHECK_EQ_INT(aud_preroll_filled(&pr), 16);
    }

    CHECK_EQ_INT(drain(&pr, out, 16), aud_preroll_filled(&pr));

    /* whatever is held, it is a consecutive run ending at the newest frame */
    for (size_t i = 1; i < aud_preroll_filled(&pr); i++)
    {
      CHECK_EQ_INT(out[i * FRAME_BYTES], (unsigned char)(out[(i - 1) * FRAME_BYTES] + 1));
    }
    CHECK_EQ_INT(out[(aud_preroll_filled(&pr) - 1) * FRAME_BYTES],
                 (unsigned char)(next - 1));
  }

  aud_preroll_free(&pr);
}

TEST(clear_empties_the_ring_without_freeing_it)
{
  aud_preroll pr;
  unsigned char in[8 * FRAME_BYTES];
  unsigned char out[8 * FRAME_BYTES];

  CHECK_EQ_INT(aud_preroll_init(&pr, 8, FRAME_BYTES), 0);
  fill(in, 8, 0);
  aud_preroll_push(&pr, in, 8);

  aud_preroll_clear(&pr);
  CHECK_EQ_INT(aud_preroll_filled(&pr), 0);
  CHECK_EQ_INT(aud_preroll_capacity(&pr), 8);

  /* still usable, and starting from the front again */
  fill(in, 8, 50);
  aud_preroll_push(&pr, in, 3);
  CHECK_EQ_INT(drain(&pr, out, 8), 3);
  CHECK_EQ_INT(memcmp(out, in, 3 * FRAME_BYTES), 0);

  aud_preroll_free(&pr);
}

TEST(wide_frames_are_copied_whole)
{
  aud_preroll pr;
  unsigned char in[4 * 24];
  unsigned char out[3 * 24];
  aud_preroll_segment seg[2];
  unsigned n;
  size_t at = 0;

  /* 8 channels of s24_le: 24 bytes a frame, and nothing may straddle one */
  CHECK_EQ_INT(aud_preroll_init(&pr, 3, 24), 0);
  for (size_t i = 0; i < sizeof(in); i++)
  {
    in[i] = (unsigned char)i;
  }

  aud_preroll_push(&pr, in, 4);
  CHECK_EQ_INT(aud_preroll_filled(&pr), 3);
  CHECK_EQ_INT(aud_preroll_bytes(&pr), 72);

  n = aud_preroll_segments(&pr, seg);
  for (unsigned i = 0; i < n; i++)
  {
    memcpy(out + at, seg[i].data, seg[i].frames * 24);
    at += seg[i].frames * 24;
  }
  CHECK_EQ_INT(at, sizeof(out));
  CHECK_EQ_INT(memcmp(out, in + 24, sizeof(out)), 0);

  aud_preroll_free(&pr);
}

TEST(frames_for_converts_seconds_and_clamps)
{
  CHECK_EQ_INT(aud_preroll_frames_for(5.0, 48000), 240000);
  CHECK_EQ_INT(aud_preroll_frames_for(0.5, 44100), 22050);

  /* nothing to hold, so nothing is allocated */
  CHECK_EQ_INT(aud_preroll_frames_for(0.0, 48000), 0);
  CHECK_EQ_INT(aud_preroll_frames_for(-1.0, 48000), 0);
  CHECK_EQ_INT(aud_preroll_frames_for(5.0, 0), 0);

  /* past the ceiling it saturates rather than allocating what was asked for */
  CHECK_EQ_INT(aud_preroll_frames_for(1e9, 48000),
               (long long)(AUD_PREROLL_MAX_SECONDS * 48000.0));
}

TEST(the_api_tolerates_null_and_freed_rings)
{
  aud_preroll pr;
  aud_preroll_segment seg[2];
  unsigned char buf[4] = {0};

  CHECK_EQ_INT(aud_preroll_capacity(NULL), 0);
  CHECK_EQ_INT(aud_preroll_filled(NULL), 0);
  CHECK_EQ_INT(aud_preroll_bytes(NULL), 0);
  CHECK_EQ_INT(aud_preroll_segments(NULL, seg), 0);
  aud_preroll_push(NULL, buf, 2);
  aud_preroll_clear(NULL);
  aud_preroll_free(NULL);

  CHECK_EQ_INT(aud_preroll_init(&pr, 8, FRAME_BYTES), 0);
  aud_preroll_free(&pr);
  /* a double free is a no-op, and so is using the ring afterwards */
  aud_preroll_free(&pr);
  aud_preroll_push(&pr, buf, 2);
  CHECK_EQ_INT(aud_preroll_filled(&pr), 0);
  CHECK_EQ_INT(aud_preroll_segments(&pr, seg), 0);
}

int main(void)
{
  RUN(init_takes_the_capacity_literally);
  RUN(init_rejects_a_zero_or_overflowing_size);
  RUN(a_partial_fill_comes_back_in_one_segment);
  RUN(an_empty_ring_has_no_segments);
  RUN(the_oldest_frames_are_dropped_once_it_is_full);
  RUN(a_push_larger_than_the_ring_keeps_only_the_newest);
  RUN(a_push_of_exactly_the_capacity_replaces_everything);
  RUN(the_contents_stay_in_order_across_many_wraps);
  RUN(clear_empties_the_ring_without_freeing_it);
  RUN(wide_frames_are_copied_whole);
  RUN(frames_for_converts_seconds_and_clamps);
  RUN(the_api_tolerates_null_and_freed_rings);
  return TEST_RESULT();
}
