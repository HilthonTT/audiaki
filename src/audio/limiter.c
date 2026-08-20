/* SPDX-License-Identifier: MIT */
#include "audio/limiter.h"

#include "audio/truepeak.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

/*
 * What one frame asks for: the gain that would put its true peak exactly on the
 * ceiling, or 1 when it is already under.
 *
 * The interpolator wants AUD_TRUEPEAK_TAPS consecutive samples of one channel,
 * with the point being looked at AUD_TRUEPEAK_CENTRE into them, so the window
 * for frame `n` starts at n - AUD_TRUEPEAK_CENTRE and anything off either end
 * of the buffer reads as silence. Gathered a sample at a time rather than
 * copied per channel: the range is interleaved and staying that way, and
 * twelve gathers is nothing beside the thirty-six multiplies they feed.
 */
static float wanted_gain(const float *in, size_t frames, unsigned channels, size_t n,
                         const aud_truepeak *tp, float ceiling)
{
  float loudest = 0.0f;

  for (unsigned c = 0; c < channels; c++)
  {
    float window[AUD_TRUEPEAK_TAPS];
    float here = fabsf(in[n * channels + c]);
    float between;

    if (here > loudest)
    {
      loudest = here;
    }

    for (unsigned t = 0; t < AUD_TRUEPEAK_TAPS; t++)
    {
      long at = (long)n - (long)AUD_TRUEPEAK_CENTRE + (long)t;

      window[t] = (at < 0 || (size_t)at >= frames) ? 0.0f : in[(size_t)at * channels + c];
    }

    between = aud_truepeak_between(tp, window);
    if (between > loudest)
    {
      loudest = between;
    }
  }

  return loudest > ceiling ? ceiling / loudest : 1.0f;
}

/*
 * How far either side of a frame the gain applied to it has to be safe for.
 *
 * The interpolator reading around frame n touches samples n - AUD_TRUEPEAK_CENTRE
 * through n + AUD_TRUEPEAK_TAPS - 1 - AUD_TRUEPEAK_CENTRE, and every one of them
 * carries its *own* gain rather than n's. So it is not enough for the gain at n
 * to be under what n asked for: every gain that reaches into n's window has to
 * be, or the twelve taps add back up to something over the ceiling. Widening
 * the minimum by exactly that reach is what makes the guarantee hold of the
 * output rather than only of the samples.
 */
#define LIM_TAPS_BEFORE ((size_t)AUD_TRUEPEAK_CENTRE)
#define LIM_TAPS_AFTER ((size_t)(AUD_TRUEPEAK_TAPS - 1u - AUD_TRUEPEAK_CENTRE))

/*
 * A sliding minimum over the look-ahead window, kept as the usual monotonic
 * queue: values that arrive smaller than what is already waiting can never be
 * the answer while they are in the window, so they are dropped as they arrive
 * and the front is always the smallest of the last L asked for.
 */
typedef struct
{
  size_t *index;
  float *value;
  size_t head;
  size_t tail; /* one past the last, so tail - head is what is in it */
  size_t room;
} minqueue;

static void minqueue_push(minqueue *q, size_t index, float value)
{
  while (q->tail > q->head && q->value[(q->tail - 1u) % q->room] >= value)
  {
    q->tail--;
  }
  q->index[q->tail % q->room] = index;
  q->value[q->tail % q->room] = value;
  q->tail++;
}

/* Drop whatever has fallen out of the window that now starts at `from`. */
static void minqueue_retire(minqueue *q, size_t from)
{
  while (q->tail > q->head && q->index[q->head % q->room] < from)
  {
    q->head++;
  }
}

static float minqueue_front(const minqueue *q)
{
  return q->tail > q->head ? q->value[q->head % q->room] : 1.0f;
}

int aud_limiter_apply(float *interleaved, size_t frames, unsigned channels, unsigned rate,
                      double ceiling_db, double *reduction_db)
{
  aud_truepeak tp;
  minqueue q;
  float *ring = NULL;  /* the last `look` answers of the sliding minimum */
  size_t *slot = NULL; /* the queue's indices, allocated with its values */
  float *value = NULL;
  size_t look;
  size_t room;
  size_t at = 0;
  long first;
  double sum;
  double rise;
  float ceiling;
  float lowest = 1.0f;
  float previous = 1.0f;

  if (reduction_db != NULL)
  {
    *reduction_db = 0.0;
  }

  if (interleaved == NULL || frames == 0 || channels == 0 ||
      rate < AUD_LIMITER_MIN_RATE || !(ceiling_db > -100.0 && ceiling_db < 24.0))
  {
    errno = EINVAL;
    return -1;
  }

  look = (size_t)(AUD_LIMITER_LOOKAHEAD_MS * (double)rate / 1000.0);
  if (look < 2u)
  {
    look = 2u;
  }
  if (look > frames)
  {
    look = frames;
  }

  ceiling = (float)pow(10.0, ceiling_db / 20.0);
  aud_truepeak_build(&tp);

  /*
   * The queue holds one whole minimum window, which is the look-ahead plus the
   * interpolator's reach either side of it - see LIM_TAPS_BEFORE.
   */
  room = look + LIM_TAPS_BEFORE + LIM_TAPS_AFTER;

  ring = malloc(look * sizeof(*ring));
  slot = malloc(room * sizeof(*slot));
  value = malloc(room * sizeof(*value));
  if (ring == NULL || slot == NULL || value == NULL)
  {
    free(ring);
    free(slot);
    free(value);
    errno = ENOMEM;
    return -1;
  }

  q.index = slot;
  q.value = value;
  q.head = 0;
  q.tail = 0;
  q.room = room;

  /*
   * The gain applied to a frame is a mean of the last `look` answers of a
   * sliding minimum, and that is what makes it both smooth and safe. Every one
   * of those minima is taken over a window wide enough to contain every frame
   * whose gain reaches into the one being turned down, so each is at most what
   * that frame asked for, and so is their mean. The peak is ridden rather than
   * met, and the twelve taps the meter will read it back through cannot add up
   * to more than the ceiling.
   *
   * The run-up is the `look - 1` turns before frame zero. They produce no audio
   * - nothing was heard before the range began - but they fill the mean and
   * ride the envelope down, so a range that opens on a transient already over
   * the ceiling opens with the gain already where it needs to be rather than
   * stepping there. That is why the loop starts at a negative frame.
   */
  sum = (double)look;
  for (size_t i = 0; i < look; i++)
  {
    ring[i] = 1.0f;
  }

  /* the frames the run-up's first turn cannot reach forward far enough to push */
  for (size_t i = 0; i < LIM_TAPS_BEFORE && i < frames; i++)
  {
    minqueue_push(&q, i, wanted_gain(interleaved, frames, channels, i, &tp, ceiling));
  }

  /* e over the release time, which is about 8.7 dB - see limiter.h */
  rise = exp(1.0 / (AUD_LIMITER_RELEASE_MS * (double)rate / 1000.0));

  first = -(long)(look - 1u);

  for (long n = first; n < (long)frames; n++)
  {
    long behind = n - (long)LIM_TAPS_AFTER;
    long ahead = n + (long)look - 1L + (long)LIM_TAPS_BEFORE;
    float smallest;
    float mean;
    float gain;

    /*
     * Retired before the new frame is pushed, not after: the queue is sized for
     * exactly one window, and pushing first would put one more than that in it.
     */
    minqueue_retire(&q, behind > 0 ? (size_t)behind : 0u);
    if (ahead >= 0 && ahead < (long)frames)
    {
      minqueue_push(
          &q, (size_t)ahead,
          wanted_gain(interleaved, frames, channels, (size_t)ahead, &tp, ceiling));
    }

    smallest = minqueue_front(&q);
    sum -= (double)ring[at];
    ring[at] = smallest;
    sum += (double)smallest;
    at = (at + 1u) % look;

    mean = (float)(sum / (double)look);

    gain = (float)((double)previous * rise);
    if (gain > 1.0f)
    {
      gain = 1.0f;
    }
    if (mean < gain)
    {
      gain = mean;
    }
    previous = gain;

    if (n < 0)
    {
      continue; /* the run-up: it set the envelope, and that is all it is for */
    }

    if (gain < lowest)
    {
      lowest = gain;
    }

    if (gain < 1.0f)
    {
      for (unsigned c = 0; c < channels; c++)
      {
        interleaved[(size_t)n * channels + c] *= gain;
      }
    }
  }

  if (reduction_db != NULL && lowest < 1.0f)
  {
    *reduction_db = -20.0 * log10((double)lowest);
  }

  free(ring);
  free(slot);
  free(value);
  return 0;
}
