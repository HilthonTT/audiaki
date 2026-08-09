/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "take/latency.h"

TEST(the_estimate_is_both_queues_in_series)
{
  CHECK_EQ_INT((int)aud_latency_estimate(2048, 1536), 3584);
  CHECK_EQ_INT((int)aud_latency_estimate(0, 0), 0);
}

TEST(a_measured_number_replaces_the_estimate)
{
  /* 10 ms at 48 kHz is 480 frames, whatever the buffers happen to be */
  CHECK_EQ_INT((int)aud_latency_frames(10.0, 48000, 2048, 1536), 480);
  CHECK_EQ_INT((int)aud_latency_frames(0.0, 48000, 2048, 1536), 0);

  /* negative means "work it out yourself" */
  CHECK_EQ_INT((int)aud_latency_frames(-1.0, 48000, 2048, 1536), 3584);
}

TEST(an_implausible_latency_is_held_to_the_ceiling)
{
  uint64_t ceiling = (uint64_t)(AUD_LATENCY_MAX_MS * 48000.0 / 1000.0);

  CHECK_EQ_INT((int)aud_latency_frames(100000.0, 48000, 0, 0), (int)ceiling);
  /* and an estimate from an absurd buffer is held to the same place */
  CHECK_EQ_INT((int)aud_latency_frames(-1.0, 48000, 100000000ul, 0), (int)ceiling);
  CHECK_EQ_INT((int)aud_latency_frames(10.0, 0, 100, 100), 0);
}

TEST(a_take_is_placed_a_round_trip_before_the_button)
{
  uint64_t start = 999;
  uint64_t skip = 999;

  aud_latency_place(48000, 1000, &start, &skip);
  CHECK_EQ_INT((int)start, 47000);
  CHECK_EQ_INT((int)skip, 0);

  /* no compensation puts it exactly where it was asked for */
  aud_latency_place(48000, 0, &start, &skip);
  CHECK_EQ_INT((int)start, 48000);
  CHECK_EQ_INT((int)skip, 0);
}

TEST(near_the_start_what_will_not_fit_comes_off_the_take)
{
  uint64_t start = 999;
  uint64_t skip = 999;

  /* only 300 frames of room before the cursor, and 1000 to correct for */
  aud_latency_place(300, 1000, &start, &skip);
  CHECK_EQ_INT((int)start, 0);
  CHECK_EQ_INT((int)skip, 700);

  /* recording from the very beginning drops the whole round trip */
  aud_latency_place(0, 1000, &start, &skip);
  CHECK_EQ_INT((int)start, 0);
  CHECK_EQ_INT((int)skip, 1000);

  /* exactly enough room is still a clean shift */
  aud_latency_place(1000, 1000, &start, &skip);
  CHECK_EQ_INT((int)start, 0);
  CHECK_EQ_INT((int)skip, 0);
}

TEST(the_api_tolerates_being_asked_for_nothing)
{
  aud_latency_place(10, 5, NULL, NULL); /* must not write through a null */
  CHECK(1);
}

int main(void)
{
  RUN(the_estimate_is_both_queues_in_series);
  RUN(a_measured_number_replaces_the_estimate);
  RUN(an_implausible_latency_is_held_to_the_ceiling);
  RUN(a_take_is_placed_a_round_trip_before_the_button);
  RUN(near_the_start_what_will_not_fit_comes_off_the_take);
  RUN(the_api_tolerates_being_asked_for_nothing);

  return TEST_RESULT();
}
