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
  float *head = NULL;  /* what the first `look` frames asked for, see below */
  size_t *slot = NULL; /* the queue's indices, allocated with its values */
  float *value = NULL;
  size_t look;
  size_t at = 0;
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

  ring = malloc(look * sizeof(*ring));
  head = malloc(look * sizeof(*head));
  slot = malloc(look * sizeof(*slot));
  value = malloc(look * sizeof(*value));
  if (ring == NULL || head == NULL || slot == NULL || value == NULL)
  {
    free(ring);
    free(head);
    free(slot);
    free(value);
    errno = ENOMEM;
    return -1;
  }

  q.index = slot;
  q.value = value;
  q.head = 0;
  q.tail = 0;
  q.room = look;

  /*
   * The gain applied to a frame is a mean of the last `look` answers of a
   * minimum taken over the `look` frames ahead of each of them. Two passes of a
   * window that long is what makes it smooth, and it is also what makes it
   * safe: every one of those minima was taken over a window containing the
   * frame being turned down, so each is at most what that frame asked for, and
   * so is their mean. The peak is ridden rather than met.
   *
   * The mean starts full of ones, which is the same statement about a range
   * that begins in silence. A range that begins with a transient already over
   * the ceiling has no run-up to have seen it coming, and there `head` is what
   * catches it - see below.
   */
  sum = (double)look;
  for (size_t i = 0; i < look; i++)
  {
    ring[i] = 1.0f;
  }

  for (size_t i = 0; i + 1u < look; i++)
  {
    head[i] = wanted_gain(interleaved, frames, channels, i, &tp, ceiling);
    minqueue_push(&q, i, head[i]);
  }

  /* e over the release time, which is about 8.7 dB - see limiter.h */
  rise = exp(1.0 / (AUD_LIMITER_RELEASE_MS * (double)rate / 1000.0));

  for (size_t n = 0; n < frames; n++)
  {
    size_t ahead = n + look - 1u;
    float smallest;
    float mean;
    float gain;

    /*
     * Retired before the new frame is pushed, not after: the queue is sized for
     * exactly one window, and pushing first would put one more than that in it.
     */
    minqueue_retire(&q, n);
    if (ahead < frames)
    {
      minqueue_push(&q, ahead,
                    wanted_gain(interleaved, frames, channels, ahead, &tp, ceiling));
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

    /*
     * The first frames of the range, where the mean is still partly the ones it
     * started with rather than what the audio asked for. Nothing was heard
     * before the range began, so there is nowhere earlier to have started the
     * ride from and the level is simply held down to what the frame needs. It
     * is the one place this steps rather than rides, it can only happen in the
     * first few milliseconds, and it is better than the alternative of letting
     * exactly the peak the caller asked to be rid of through.
     */
    if (n + 1u < look && head[n] < gain)
    {
      gain = head[n];
    }

    if (gain < lowest)
    {
      lowest = gain;
    }

    if (gain < 1.0f)
    {
      for (unsigned c = 0; c < channels; c++)
      {
        interleaved[n * channels + c] *= gain;
      }
    }
    previous = gain;
  }

  if (reduction_db != NULL && lowest < 1.0f)
  {
    *reduction_db = -20.0 * log10((double)lowest);
  }

  free(ring);
  free(head);
  free(slot);
  free(value);
  return 0;
}
