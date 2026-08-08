/* SPDX-License-Identifier: MIT */
/*
 * ringbuf.h - a lock-free single-producer single-consumer float ring.
 *
 * The desktop app captures on one thread and draws on another. A mutex between
 * them would let a slow redraw stall the capture loop and cause an xrun, so the
 * handoff is a wait-free ring instead: the capture thread only ever advances
 * the write index, the consumer only ever advances the read index, and the two
 * are ordinary atomics with release/acquire ordering.
 *
 * One ring serves one reader. The engine keeps separate rings for the
 * visualiser and the monitor rather than trying to share a single one.
 *
 * Capacity is rounded up to a power of two so the wrap is a mask rather than a
 * modulo, and one slot is left empty so full and empty stay distinguishable
 * without a third counter.
 *
 * No ALSA and no I/O, so it is unit testable on its own.
 */
#ifndef AUDIAKI_RINGBUF_H
#define AUDIAKI_RINGBUF_H

#include <stdatomic.h>
#include <stddef.h>

typedef struct
{
  float *data;
  size_t capacity; /* allocated slots; a power of two */
  size_t mask;     /* capacity - 1 */
  atomic_size_t write;
  atomic_size_t read;
} aud_ringbuf;

/*
 * Allocate a ring holding at least `min_slots` samples. Returns 0 on success,
 * -1 with errno set to EINVAL for a zero or overflowing size, or ENOMEM.
 */
int aud_ringbuf_init(aud_ringbuf *rb, size_t min_slots);

void aud_ringbuf_free(aud_ringbuf *rb);

/* Samples that can be stored at once, which is one less than the allocation. */
size_t aud_ringbuf_capacity(const aud_ringbuf *rb);

/* Samples waiting to be read. Safe from either thread. */
size_t aud_ringbuf_available(const aud_ringbuf *rb);

/* Samples that would fit without dropping anything. Safe from either thread. */
size_t aud_ringbuf_space(const aud_ringbuf *rb);

/*
 * Producer side. Copies as much of `src` as fits and returns how many samples
 * were taken, which is less than `count` when the consumer has fallen behind.
 */
size_t aud_ringbuf_write(aud_ringbuf *rb, const float *src, size_t count);

/*
 * Producer side, but never refuses: when `src` does not fit, the oldest
 * unread samples are dropped to make room. This is what the visualiser feed
 * wants - a late frame should show current audio, not a backlog - and what the
 * capture thread wants, since it cannot block. Returns the number of samples
 * dropped.
 *
 * Dropping moves the read index, so unlike the rest of the API this must be
 * called from the producer while the consumer is quiescent, or accepted as
 * racy for a display where a torn sample does not matter.
 */
size_t aud_ringbuf_write_overwrite(aud_ringbuf *rb, const float *src, size_t count);

/*
 * Consumer side. Copies up to `count` samples into `dst` and returns how many
 * were moved, which is less than `count` when the ring has run dry.
 */
size_t aud_ringbuf_read(aud_ringbuf *rb, float *dst, size_t count);

/* Consumer side: discard up to `count` samples. Returns how many were dropped. */
size_t aud_ringbuf_skip(aud_ringbuf *rb, size_t count);

/* Drop everything buffered. Call with the producer stopped. */
void aud_ringbuf_reset(aud_ringbuf *rb);

#endif /* AUDIAKI_RINGBUF_H */
