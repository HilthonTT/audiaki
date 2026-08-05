/* SPDX-License-Identifier: MIT */
#include "ringbuf.h"

#include "test_util.h"

#include <stdlib.h>

static void fill(float *buf, size_t n, float base)
{
  for (size_t i = 0; i < n; i++)
    buf[i] = base + (float)i;
}

TEST(init_rounds_capacity_up_to_a_power_of_two)
{
  aud_ringbuf rb;

  /* 100 usable slots needs 101 with the empty guard, so 128 allocated. */
  CHECK_EQ_INT(aud_ringbuf_init(&rb, 100), 0);
  CHECK_EQ_INT(rb.capacity, 128);
  CHECK_EQ_INT(aud_ringbuf_capacity(&rb), 127);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 0);
  CHECK_EQ_INT(aud_ringbuf_space(&rb), 127);
  aud_ringbuf_free(&rb);

  /* An exact power of two still needs the guard slot, so it doubles. */
  CHECK_EQ_INT(aud_ringbuf_init(&rb, 64), 0);
  CHECK_EQ_INT(rb.capacity, 128);
  CHECK_EQ_INT(aud_ringbuf_capacity(&rb), 127);
  aud_ringbuf_free(&rb);
}

TEST(init_rejects_a_zero_size)
{
  aud_ringbuf rb;

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 0), -1);
  CHECK_EQ_INT(aud_ringbuf_init(NULL, 16), -1);
}

TEST(a_write_comes_back_out_in_order)
{
  aud_ringbuf rb;
  float in[16];
  float out[16];

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 64), 0);
  fill(in, 16, 1.0f);

  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 16), 16);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 16);

  CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 16), 16);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 0);

  for (size_t i = 0; i < 16; i++)
    CHECK_EQ_DBL(out[i], in[i], 0.0);
}

TEST(a_partial_read_leaves_the_rest_queued)
{
  aud_ringbuf rb;
  float in[16];
  float out[16];

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 64), 0);
  fill(in, 16, 100.0f);
  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 16), 16);

  CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 6), 6);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 10);
  CHECK_EQ_DBL(out[0], 100.0, 0.0);
  CHECK_EQ_DBL(out[5], 105.0, 0.0);

  CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 16), 10);
  CHECK_EQ_DBL(out[0], 106.0, 0.0);
  CHECK_EQ_DBL(out[9], 115.0, 0.0);

  aud_ringbuf_free(&rb);
}

TEST(a_read_from_an_empty_ring_moves_nothing)
{
  aud_ringbuf rb;
  float out[4] = {9.0f, 9.0f, 9.0f, 9.0f};

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 16), 0);
  CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 4), 0);
  CHECK_EQ_DBL(out[0], 9.0, 0.0);
  aud_ringbuf_free(&rb);
}

TEST(a_plain_write_refuses_to_exceed_the_capacity)
{
  aud_ringbuf rb;
  float in[200];
  float out[200];

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 100), 0); /* 127 usable */
  fill(in, 200, 0.0f);

  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 200), 127);
  CHECK_EQ_INT(aud_ringbuf_space(&rb), 0);
  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 1), 0);

  /* what it did take is the head of the input, not the tail */
  CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 200), 127);
  CHECK_EQ_DBL(out[0], 0.0, 0.0);
  CHECK_EQ_DBL(out[126], 126.0, 0.0);

  aud_ringbuf_free(&rb);
}

/*
 * The wrap is the interesting case: writing and reading in unequal chunks
 * walks both indices past the end of the allocation repeatedly.
 */
TEST(data_survives_wrapping_many_times)
{
  aud_ringbuf rb;
  float in[7];
  float out[5];
  float expected = 0.0f;
  float next_in = 0.0f;

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 16), 0);

  for (int round = 0; round < 200; round++)
  {
    fill(in, 7, next_in);
    next_in += 7.0f;
    CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 7), 7);

    /* drain 5 of the 7, so the backlog grows until it hits the capacity */
    if (aud_ringbuf_available(&rb) >= 5)
    {
      CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 5), 5);
      for (size_t i = 0; i < 5; i++)
      {
        CHECK_EQ_DBL(out[i], expected, 0.0);
        expected += 1.0f;
      }
    }

    /* keep room for the next push so the plain write never has to refuse */
    while (aud_ringbuf_space(&rb) < 7)
    {
      CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 1), 1);
      CHECK_EQ_DBL(out[0], expected, 0.0);
      expected += 1.0f;
    }
  }

  aud_ringbuf_free(&rb);
}

TEST(overwrite_drops_the_oldest_samples_to_make_room)
{
  aud_ringbuf rb;
  float in[100];
  float out[100];

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 16), 0); /* 31 usable */
  fill(in, 100, 0.0f);

  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 20), 20);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 20);

  /* 20 queued + 20 more against 31 slots: the 9 oldest have to go */
  CHECK_EQ_INT(aud_ringbuf_write_overwrite(&rb, in + 20, 20), 9);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 31);

  CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 100), 31);
  CHECK_EQ_DBL(out[0], 9.0, 0.0); /* 0..8 dropped */
  CHECK_EQ_DBL(out[30], 39.0, 0.0);

  aud_ringbuf_free(&rb);
}

TEST(overwrite_of_more_than_the_ring_keeps_only_the_tail)
{
  aud_ringbuf rb;
  float in[100];
  float out[100];

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 16), 0); /* 31 usable */
  fill(in, 100, 0.0f);

  /* 100 samples into 31 slots: everything but the last 31 is dropped */
  CHECK_EQ_INT(aud_ringbuf_write_overwrite(&rb, in, 100), 69);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 31);

  CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 100), 31);
  CHECK_EQ_DBL(out[0], 69.0, 0.0);
  CHECK_EQ_DBL(out[30], 99.0, 0.0);

  aud_ringbuf_free(&rb);
}

TEST(overwrite_that_fits_drops_nothing)
{
  aud_ringbuf rb;
  float in[8];

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 64), 0);
  fill(in, 8, 0.0f);

  CHECK_EQ_INT(aud_ringbuf_write_overwrite(&rb, in, 8), 0);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 8);

  aud_ringbuf_free(&rb);
}

TEST(skip_and_reset_discard_without_copying)
{
  aud_ringbuf rb;
  float in[16];
  float out[16];

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 64), 0);
  fill(in, 16, 0.0f);
  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 16), 16);

  CHECK_EQ_INT(aud_ringbuf_skip(&rb, 4), 4);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 12);
  CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 1), 1);
  CHECK_EQ_DBL(out[0], 4.0, 0.0);

  /* skipping past the end takes only what is there */
  CHECK_EQ_INT(aud_ringbuf_skip(&rb, 999), 11);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 0);

  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 16), 16);
  aud_ringbuf_reset(&rb);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 0);
  CHECK_EQ_INT(aud_ringbuf_space(&rb), 127);

  aud_ringbuf_free(&rb);
}

TEST(the_api_tolerates_null_and_freed_rings)
{
  aud_ringbuf rb;
  float buf[4] = {0};

  CHECK_EQ_INT(aud_ringbuf_capacity(NULL), 0);
  CHECK_EQ_INT(aud_ringbuf_available(NULL), 0);
  CHECK_EQ_INT(aud_ringbuf_space(NULL), 0);
  CHECK_EQ_INT(aud_ringbuf_write(NULL, buf, 4), 0);
  CHECK_EQ_INT(aud_ringbuf_read(NULL, buf, 4), 0);
  CHECK_EQ_INT(aud_ringbuf_skip(NULL, 4), 0);
  aud_ringbuf_reset(NULL);
  aud_ringbuf_free(NULL);

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 16), 0);
  aud_ringbuf_free(&rb);
  /* a double free is a no-op, and so is using the ring afterwards */
  aud_ringbuf_free(&rb);
  CHECK_EQ_INT(aud_ringbuf_write(&rb, buf, 4), 0);
  CHECK_EQ_INT(aud_ringbuf_read(&rb, buf, 4), 0);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 0);
}

int main(void)
{
  RUN(init_rounds_capacity_up_to_a_power_of_two);
  RUN(init_rejects_a_zero_size);
  RUN(a_write_comes_back_out_in_order);
  RUN(a_partial_read_leaves_the_rest_queued);
  RUN(a_read_from_an_empty_ring_moves_nothing);
  RUN(a_plain_write_refuses_to_exceed_the_capacity);
  RUN(data_survives_wrapping_many_times);
  RUN(overwrite_drops_the_oldest_samples_to_make_room);
  RUN(overwrite_of_more_than_the_ring_keeps_only_the_tail);
  RUN(overwrite_that_fits_drops_nothing);
  RUN(skip_and_reset_discard_without_copying);
  RUN(the_api_tolerates_null_and_freed_rings);
  return TEST_RESULT();
}
