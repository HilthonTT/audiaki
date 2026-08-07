/* SPDX-License-Identifier: MIT */
#include "preroll.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int aud_preroll_init(aud_preroll *pr, size_t frames, size_t frame_bytes)
{
  unsigned char *data;

  if (pr == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  memset(pr, 0, sizeof(*pr));

  if (frames == 0 || frame_bytes == 0)
  {
    errno = EINVAL;
    return -1;
  }
  if (frames > SIZE_MAX / frame_bytes)
  {
    errno = ENOMEM;
    return -1;
  }

  data = malloc(frames * frame_bytes);
  if (data == NULL)
  {
    errno = ENOMEM;
    return -1;
  }

  pr->data = data;
  pr->frame_bytes = frame_bytes;
  pr->capacity = frames;
  return 0;
}

void aud_preroll_free(aud_preroll *pr)
{
  if (pr == NULL)
  {
    return;
  }

  free(pr->data);
  memset(pr, 0, sizeof(*pr));
}

size_t aud_preroll_capacity(const aud_preroll *pr)
{
  return (pr != NULL && pr->data != NULL) ? pr->capacity : 0;
}

size_t aud_preroll_filled(const aud_preroll *pr)
{
  return (pr != NULL && pr->data != NULL) ? pr->filled : 0;
}

size_t aud_preroll_bytes(const aud_preroll *pr)
{
  if (pr == NULL || pr->data == NULL)
  {
    return 0;
  }

  return pr->filled * pr->frame_bytes;
}

void aud_preroll_clear(aud_preroll *pr)
{
  if (pr == NULL)
  {
    return;
  }

  pr->head = 0;
  pr->filled = 0;
}

void aud_preroll_push(aud_preroll *pr, const void *frames, size_t count)
{
  const unsigned char *src = frames;
  size_t first;

  if (pr == NULL || pr->data == NULL || src == NULL || count == 0)
  {
    return;
  }

  /*
   * More than the ring holds: nothing already stored can survive, so take the
   * newest `capacity` frames of the input and start again from the beginning.
   */
  if (count >= pr->capacity)
  {
    src += (count - pr->capacity) * pr->frame_bytes;
    memcpy(pr->data, src, pr->capacity * pr->frame_bytes);
    pr->head = 0;
    pr->filled = pr->capacity;
    return;
  }

  first = pr->capacity - pr->head;
  if (first > count)
  {
    first = count;
  }

  memcpy(pr->data + pr->head * pr->frame_bytes, src, first * pr->frame_bytes);
  if (count > first)
  {
    memcpy(pr->data, src + first * pr->frame_bytes, (count - first) * pr->frame_bytes);
  }

  pr->head += count;
  if (pr->head >= pr->capacity)
  {
    pr->head -= pr->capacity;
  }

  pr->filled += count;
  if (pr->filled > pr->capacity)
  {
    pr->filled = pr->capacity;
  }
}

unsigned aud_preroll_segments(const aud_preroll *pr, aud_preroll_segment out[2])
{
  size_t tail;
  size_t first;

  if (out == NULL)
  {
    return 0;
  }

  out[0].data = NULL;
  out[0].frames = 0;
  out[1].data = NULL;
  out[1].frames = 0;

  if (pr == NULL || pr->data == NULL || pr->filled == 0)
  {
    return 0;
  }

  /* where the oldest stored frame sits, counting back from the write head */
  tail = pr->head + pr->capacity - pr->filled;
  if (tail >= pr->capacity)
  {
    tail -= pr->capacity;
  }

  first = pr->capacity - tail;
  if (first > pr->filled)
  {
    first = pr->filled;
  }

  out[0].data = pr->data + tail * pr->frame_bytes;
  out[0].frames = first;

  if (first == pr->filled)
  {
    return 1;
  }

  out[1].data = pr->data;
  out[1].frames = pr->filled - first;
  return 2;
}

size_t aud_preroll_frames_for(double seconds, unsigned rate)
{
  double frames;

  if (!(seconds > 0.0) || rate == 0)
  {
    return 0;
  }

  if (seconds > AUD_PREROLL_MAX_SECONDS)
  {
    seconds = AUD_PREROLL_MAX_SECONDS;
  }

  frames = seconds * (double)rate + 0.5;
  if (frames > (double)SIZE_MAX)
  {
    return SIZE_MAX;
  }

  return (size_t)frames;
}
