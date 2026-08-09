/* SPDX-License-Identifier: MIT */
#include "take/latency.h"

uint64_t aud_latency_estimate(unsigned long capture_frames, unsigned long playback_frames)
{
  /*
   * The two queues in series. A frame waits in the playback buffer on its way
   * out and in the capture side's period on its way back, so the round trip is
   * their sum rather than either one.
   */
  return (uint64_t)capture_frames + (uint64_t)playback_frames;
}

uint64_t aud_latency_frames(double override_ms, unsigned rate,
                            unsigned long capture_frames, unsigned long playback_frames)
{
  double ms;
  uint64_t ceiling;

  if (rate == 0)
  {
    return 0;
  }

  ceiling = (uint64_t)(AUD_LATENCY_MAX_MS * (double)rate / 1000.0);

  /* written the way round that rejects a NaN rather than letting it through */
  if (!(override_ms >= 0.0))
  {
    uint64_t estimate = aud_latency_estimate(capture_frames, playback_frames);

    return estimate > ceiling ? ceiling : estimate;
  }

  ms = override_ms > AUD_LATENCY_MAX_MS ? AUD_LATENCY_MAX_MS : override_ms;
  return (uint64_t)(ms * (double)rate / 1000.0 + 0.5);
}

void aud_latency_place(uint64_t at, uint64_t latency, uint64_t *start, uint64_t *skip)
{
  if (start == NULL || skip == NULL)
  {
    return;
  }

  if (at >= latency)
  {
    /* the usual case: the whole correction is a shift, and nothing is lost */
    *start = at - latency;
    *skip = 0;
    return;
  }

  /*
   * Recording from near the beginning, with less room before the cursor than
   * the round trip needs. What is left over describes a moment before the
   * timeline starts, so it comes off the front of the take.
   */
  *start = 0;
  *skip = latency - at;
}
