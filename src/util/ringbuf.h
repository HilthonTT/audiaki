/* SPDX-License-Identifier: MIT */
/*
 * ringbuf.h - a lock-free single-producer single-consumer ring.
 *
 * The desktop app captures on one thread and draws on another, and writes the
 * take to disk on a third. A mutex between any two of them would let one stall
 * the others - a slow redraw holding up the capture loop, or a disk stalling it
 * - so every handoff is a wait-free ring instead: the producer only ever
 * advances the write index, the consumer only ever advances the read index, and
 * the two are ordinary atomics with release/acquire ordering.
 *
 * That division is the whole correctness argument, and it is absolute. A
 * producer that reached for the read index to make room for itself would be
 * racing the consumer for it - the consumer's own store would be lost, the read
 * index could go backwards, and `available()` would then report more than the
 * ring can hold. So there is no such call here. When a ring fills, the producer
 * takes what fits and tells the caller what it left behind; deciding what to do
 * about a backlog - drain it, or skip to the newest - belongs to the consumer,
 * which is the only side that may move the read index at all.
 *
 * One ring serves one reader. The engine keeps separate rings for the
 * visualiser, the timeline and the file rather than trying to share one.
 *
 * Slots are of a size fixed at init: aud_ringbuf_init() makes a ring of floats,
 * which is what the meters, the visualiser and the monitors pass about, and
 * aud_ringbuf_init_bytes() one of raw bytes, which is what the take's own
 * samples are on their way to the file - they are in the device's format there,
 * and going through float to reach the disk would round S32 on the way.
 *
 * Counts are in slots throughout, and the buffers are void * for the same
 * reason fread() and fwrite() are: the slot size is the ring's, set once. A
 * float ring wants a float *, a byte ring an unsigned char *, and neither is
 * checked, so pass what the ring was made for.
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
  unsigned char *data;
  size_t elem;     /* bytes in a slot */
  size_t capacity; /* allocated slots; a power of two */
  size_t mask;     /* capacity - 1 */
  atomic_size_t write;
  atomic_size_t read;
} aud_ringbuf;

/*
 * Allocate a ring holding at least `min_slots` slots of `elem` bytes. Returns 0
 * on success, -1 with errno set to EINVAL for a zero or overflowing size, or
 * ENOMEM.
 */
int aud_ringbuf_init_elem(aud_ringbuf *rb, size_t min_slots, size_t elem);

/* A ring of floats, which is what everything but the take writer wants. */
int aud_ringbuf_init(aud_ringbuf *rb, size_t min_slots);

/* A ring of raw bytes, for samples that must reach the file unconverted. */
int aud_ringbuf_init_bytes(aud_ringbuf *rb, size_t min_bytes);

void aud_ringbuf_free(aud_ringbuf *rb);

/* Slots that can be stored at once, which is one less than the allocation. */
size_t aud_ringbuf_capacity(const aud_ringbuf *rb);

/* Slots waiting to be read. Safe from either thread. */
size_t aud_ringbuf_available(const aud_ringbuf *rb);

/* Slots that would fit without dropping anything. Safe from either thread. */
size_t aud_ringbuf_space(const aud_ringbuf *rb);

/*
 * Producer side. Copies as much of `src` as fits and returns how many slots
 * were taken, which is less than `count` when the consumer has fallen behind.
 *
 * Never overwrites what the consumer has not read: making room is the
 * consumer's business, and a producer that did it would be racing for the read
 * index. A caller that would rather show current audio than a backlog skips on
 * the consumer side - see aud_ringbuf_skip().
 */
size_t aud_ringbuf_write(aud_ringbuf *rb, const void *src, size_t count);

/*
 * Consumer side. Copies up to `count` slots into `dst` and returns how many
 * were moved, which is less than `count` when the ring has run dry.
 */
size_t aud_ringbuf_read(aud_ringbuf *rb, void *dst, size_t count);

/*
 * Consumer side: discard up to `count` slots. Returns how many were dropped.
 *
 * This is where a stale backlog goes. A consumer that only wants the newest
 * `n` slots skips `available() - n` before reading, and one that wants a clean
 * start skips everything.
 */
size_t aud_ringbuf_skip(aud_ringbuf *rb, size_t count);

/* Drop everything buffered. Call with the producer stopped. */
void aud_ringbuf_reset(aud_ringbuf *rb);

#endif /* AUDIAKI_RINGBUF_H */
