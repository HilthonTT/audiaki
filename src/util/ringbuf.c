/* SPDX-License-Identifier: MIT */
#include "util/ringbuf.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Smallest power of two that is >= n, or 0 when that would overflow. The
 * ring reserves one empty slot, so callers ask for min_slots + 1 here.
 */
static size_t round_up_pow2(size_t n)
{
  size_t p = 1;

  while (p < n)
  {
    if (p > SIZE_MAX / 2)
    {
      return 0;
    }
    p *= 2;
  }
  return p;
}

int aud_ringbuf_init(aud_ringbuf *rb, size_t min_slots)
{
  size_t capacity;

  if (rb == NULL || min_slots == 0)
  {
    errno = EINVAL;
    return -1;
  }

  if (min_slots > SIZE_MAX - 1)
  {
    errno = EINVAL;
    return -1;
  }

  capacity = round_up_pow2(min_slots + 1);
  if (capacity == 0 || capacity > SIZE_MAX / sizeof(float))
  {
    errno = EINVAL;
    return -1;
  }

  rb->data = malloc(capacity * sizeof(*rb->data));
  if (rb->data == NULL)
  {
    errno = ENOMEM;
    return -1;
  }

  rb->capacity = capacity;
  rb->mask = capacity - 1;
  atomic_init(&rb->write, 0);
  atomic_init(&rb->read, 0);
  return 0;
}

void aud_ringbuf_free(aud_ringbuf *rb)
{
  if (rb == NULL)
  {
    return;
  }

  free(rb->data);
  rb->data = NULL;
  rb->capacity = 0;
  rb->mask = 0;
  atomic_init(&rb->write, 0);
  atomic_init(&rb->read, 0);
}

size_t aud_ringbuf_capacity(const aud_ringbuf *rb)
{
  if (rb == NULL || rb->data == NULL)
  {
    return 0;
  }

  return rb->capacity - 1;
}

/*
 * Both indices grow without bound and are masked on use, so the distance
 * between them is correct even across the size_t wrap.
 */
size_t aud_ringbuf_available(const aud_ringbuf *rb)
{
  size_t w;
  size_t r;

  if (rb == NULL || rb->data == NULL)
  {
    return 0;
  }

  w = atomic_load_explicit(&rb->write, memory_order_acquire);
  r = atomic_load_explicit(&rb->read, memory_order_acquire);
  return w - r;
}

size_t aud_ringbuf_space(const aud_ringbuf *rb)
{
  if (rb == NULL || rb->data == NULL)
  {
    return 0;
  }

  return aud_ringbuf_capacity(rb) - aud_ringbuf_available(rb);
}

/* Copy into the ring at `w`, splitting at the wrap. Does not publish the index. */
static void store(aud_ringbuf *rb, size_t w, const float *src, size_t count)
{
  size_t offset = w & rb->mask;
  size_t first = rb->capacity - offset;

  if (first > count)
  {
    first = count;
  }

  memcpy(rb->data + offset, src, first * sizeof(*rb->data));
  if (count > first)
  {
    memcpy(rb->data, src + first, (count - first) * sizeof(*rb->data));
  }
}

/* Copy out of the ring at `r`, splitting at the wrap. Does not publish. */
static void load(const aud_ringbuf *rb, size_t r, float *dst, size_t count)
{
  size_t offset = r & rb->mask;
  size_t first = rb->capacity - offset;

  if (first > count)
  {
    first = count;
  }

  memcpy(dst, rb->data + offset, first * sizeof(*rb->data));
  if (count > first)
  {
    memcpy(dst + first, rb->data, (count - first) * sizeof(*rb->data));
  }
}

size_t aud_ringbuf_write(aud_ringbuf *rb, const float *src, size_t count)
{
  size_t w;
  size_t space;

  if (rb == NULL || rb->data == NULL || src == NULL || count == 0)
  {
    return 0;
  }

  space = aud_ringbuf_space(rb);
  if (count > space)
  {
    count = space;
  }
  if (count == 0)
  {
    return 0;
  }

  w = atomic_load_explicit(&rb->write, memory_order_relaxed);
  store(rb, w, src, count);

  /* release: the samples above are visible before the index that exposes them */
  atomic_store_explicit(&rb->write, w + count, memory_order_release);
  return count;
}

size_t aud_ringbuf_write_overwrite(aud_ringbuf *rb, const float *src, size_t count)
{
  size_t capacity;
  size_t dropped = 0;
  size_t space;

  if (rb == NULL || rb->data == NULL || src == NULL || count == 0)
  {
    return 0;
  }

  capacity = aud_ringbuf_capacity(rb);

  /*
   * A push longer than the whole ring can only leave its tail behind, so skip
   * straight to that and count the rest as dropped.
   */
  if (count > capacity)
  {
    dropped = count - capacity;
    src += dropped;
    count = capacity;
  }

  space = aud_ringbuf_space(rb);
  if (count > space)
  {
    dropped += aud_ringbuf_skip(rb, count - space);
  }

  return dropped + (count - aud_ringbuf_write(rb, src, count));
}

size_t aud_ringbuf_read(aud_ringbuf *rb, float *dst, size_t count)
{
  size_t r;
  size_t have;

  if (rb == NULL || rb->data == NULL || dst == NULL || count == 0)
  {
    return 0;
  }

  have = aud_ringbuf_available(rb);
  if (count > have)
  {
    count = have;
  }
  if (count == 0)
  {
    return 0;
  }

  r = atomic_load_explicit(&rb->read, memory_order_relaxed);
  load(rb, r, dst, count);

  /* release: the copy above completes before the slots are offered for reuse */
  atomic_store_explicit(&rb->read, r + count, memory_order_release);
  return count;
}

size_t aud_ringbuf_skip(aud_ringbuf *rb, size_t count)
{
  size_t r;
  size_t have;

  if (rb == NULL || rb->data == NULL || count == 0)
  {
    return 0;
  }

  have = aud_ringbuf_available(rb);
  if (count > have)
  {
    count = have;
  }
  if (count == 0)
  {
    return 0;
  }

  r = atomic_load_explicit(&rb->read, memory_order_relaxed);
  atomic_store_explicit(&rb->read, r + count, memory_order_release);
  return count;
}

void aud_ringbuf_reset(aud_ringbuf *rb)
{
  if (rb == NULL || rb->data == NULL)
  {
    return;
  }

  atomic_store_explicit(&rb->read, 0, memory_order_relaxed);
  atomic_store_explicit(&rb->write, 0, memory_order_release);
}
