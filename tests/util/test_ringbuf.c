/* SPDX-License-Identifier: MIT */
#include "util/ringbuf.h"

#include "test_util.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

static void fill(float *buf, size_t n, float base)
{
  for (size_t i = 0; i < n; i++)
  {
    buf[i] = base + (float)i;
  }
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
  {
    CHECK_EQ_DBL(out[i], in[i], 0.0);
  }

  aud_ringbuf_free(&rb);
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

TEST(a_full_ring_keeps_what_it_has_and_reports_the_shortfall)
{
  aud_ringbuf rb;
  float in[100];
  float out[100];

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 16), 0); /* 31 usable */
  fill(in, 100, 0.0f);

  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 20), 20);

  /*
   * 20 queued and 20 more offered against 31 slots. The producer takes the 11
   * that fit and leaves the rest behind; it may not evict the 20 the consumer
   * has yet to read, because the read index is not its to move.
   */
  CHECK_EQ_INT(aud_ringbuf_write(&rb, in + 20, 20), 11);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 31);

  CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 100), 31);
  CHECK_EQ_DBL(out[0], 0.0, 0.0);
  CHECK_EQ_DBL(out[30], 30.0, 0.0);

  aud_ringbuf_free(&rb);
}

/*
 * What the visualiser used to ask the producer to do for it: reach the newest
 * audio rather than the oldest. It is a skip and a read, both on the consumer's
 * side of the ring, which is the only place either may happen.
 */
TEST(a_consumer_can_skip_a_backlog_to_reach_the_newest_samples)
{
  aud_ringbuf rb;
  float in[100];
  float out[100];
  size_t backlog;

  CHECK_EQ_INT(aud_ringbuf_init(&rb, 128), 0);
  fill(in, 100, 0.0f);
  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 100), 100);

  backlog = aud_ringbuf_available(&rb);
  CHECK_EQ_INT(aud_ringbuf_skip(&rb, backlog - 8), 92);
  CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 8), 8);
  CHECK_EQ_DBL(out[0], 92.0, 0.0);
  CHECK_EQ_DBL(out[7], 99.0, 0.0);

  aud_ringbuf_free(&rb);
}

TEST(a_byte_ring_carries_bytes_rather_than_floats)
{
  aud_ringbuf rb;
  unsigned char in[300];
  unsigned char out[300];

  /* 200 bytes needs 201 with the guard slot, so 256 are allocated */
  CHECK_EQ_INT(aud_ringbuf_init_bytes(&rb, 200), 0);
  CHECK_EQ_INT(rb.elem, 1);
  CHECK_EQ_INT(rb.capacity, 256);
  CHECK_EQ_INT(aud_ringbuf_capacity(&rb), 255);

  for (size_t i = 0; i < sizeof(in); i++)
  {
    in[i] = (unsigned char)(i & 0xFFu);
  }

  /* twice round, so the split memcpy is exercised with a slot of one byte */
  for (int pass = 0; pass < 3; pass++)
  {
    CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 200), 200);
    CHECK_EQ_INT(aud_ringbuf_read(&rb, out, 200), 200);
    CHECK_EQ_INT(memcmp(in, out, 200), 0);
  }

  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 255), 255);
  CHECK_EQ_INT(aud_ringbuf_write(&rb, in, 1), 0);

  aud_ringbuf_free(&rb);
}

TEST(init_rejects_a_slot_size_of_zero_or_one_that_overflows)
{
  aud_ringbuf rb;

  CHECK_EQ_INT(aud_ringbuf_init_elem(&rb, 16, 0), -1);
  CHECK_EQ_INT(aud_ringbuf_init_elem(&rb, 16, SIZE_MAX), -1);
}

/*
 * The one test that runs both sides at once. Everything above checks the
 * arithmetic; this checks the handoff, and it is what a run under
 * ThreadSanitizer has to come back clean on - one thread only ever advancing
 * the write index, one only ever the read index, and no lock between them.
 *
 * The producer sends a ramp and the consumer checks every sample against its
 * own count of what it has already had, so a slot that is lost, duplicated or
 * delivered out of order fails the test rather than having to be spotted by
 * eye. The ring is deliberately far smaller than the run, so it wraps
 * thousands of times and both sides spend real time waiting for the other.
 */
#define STRESS_SLOTS 300000u

typedef struct
{
  aud_ringbuf *rb;
  size_t moved;
  int bad; /* a sample that was not the next one expected */
} stress_side;

static void *stress_producer(void *arg)
{
  stress_side *p = arg;
  float chunk[97];
  const size_t chunk_slots = sizeof(chunk) / sizeof(chunk[0]);

  while (p->moved < STRESS_SLOTS)
  {
    size_t want = STRESS_SLOTS - p->moved;

    if (want > chunk_slots)
    {
      want = chunk_slots;
    }
    for (size_t i = 0; i < want; i++)
    {
      chunk[i] = (float)(p->moved + i);
    }

    /*
     * A short write means the consumer is behind. Offer the rest again rather
     * than dropping it: the consumer is checking for an unbroken sequence, so
     * a gap here would be indistinguishable from the bug being looked for.
     */
    p->moved += aud_ringbuf_write(p->rb, chunk, want);
  }
  return NULL;
}

static void *stress_consumer(void *arg)
{
  stress_side *c = arg;
  float chunk[61];
  const size_t chunk_slots = sizeof(chunk) / sizeof(chunk[0]);

  while (c->moved < STRESS_SLOTS)
  {
    size_t got = aud_ringbuf_read(c->rb, chunk, chunk_slots);

    for (size_t i = 0; i < got; i++)
    {
      if (chunk[i] != (float)(c->moved + i))
      {
        c->bad = 1;
      }
    }
    c->moved += got;
  }
  return NULL;
}

TEST(a_producer_and_a_consumer_hand_over_every_slot_in_order)
{
  aud_ringbuf rb;
  pthread_t producer;
  pthread_t consumer;
  stress_side in = {&rb, 0, 0};
  stress_side out = {&rb, 0, 0};

  /* 1023 usable against 300000 sent: it wraps some three hundred times */
  CHECK_EQ_INT(aud_ringbuf_init(&rb, 1000), 0);

  CHECK_EQ_INT(pthread_create(&producer, NULL, stress_producer, &in), 0);
  CHECK_EQ_INT(pthread_create(&consumer, NULL, stress_consumer, &out), 0);
  CHECK_EQ_INT(pthread_join(producer, NULL), 0);
  CHECK_EQ_INT(pthread_join(consumer, NULL), 0);

  CHECK_EQ_INT(in.moved, STRESS_SLOTS);
  CHECK_EQ_INT(out.moved, STRESS_SLOTS);
  CHECK_EQ_INT(out.bad, 0);
  CHECK_EQ_INT(aud_ringbuf_available(&rb), 0);

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
  RUN(a_full_ring_keeps_what_it_has_and_reports_the_shortfall);
  RUN(a_consumer_can_skip_a_backlog_to_reach_the_newest_samples);
  RUN(a_byte_ring_carries_bytes_rather_than_floats);
  RUN(init_rejects_a_slot_size_of_zero_or_one_that_overflows);
  RUN(a_producer_and_a_consumer_hand_over_every_slot_in_order);
  RUN(skip_and_reset_discard_without_copying);
  RUN(the_api_tolerates_null_and_freed_rings);
  return TEST_RESULT();
}
