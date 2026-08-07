/* SPDX-License-Identifier: MIT */
/*
 * preroll.h - the seconds of audio captured before you decided to keep them.
 *
 * A capture stream is usually open before a take is, for the meters. Throwing
 * away what arrives in the meantime is how the take you wanted gets lost: you
 * play the part once to check it, and that was the good one. This is the
 * circular buffer that keeps it, written to the front of the take instead.
 *
 * Frames are stored exactly as the device delivered them and repacked on the
 * way out like any other period. Converting to float to reuse ringbuf.h would
 * cost the bottom bits of a 24 or 32 bit take - these seconds are part of the
 * recording, not a display feed, and have to reach the file bit for bit.
 *
 * Reading is by segment rather than by copy: the caller gets pointers to the
 * one or two contiguous runs the data occupies, so flushing needs no second
 * buffer the size of the ring.
 *
 * Single threaded, which in the desktop app means the capture thread does both
 * the pushing and the flushing; see engine.c. No audio system and no I/O, so it
 * is unit testable on its own.
 */
#ifndef AUDIAKI_PREROLL_H
#define AUDIAKI_PREROLL_H

#include <stddef.h>

/*
 * The longest pre-roll accepted, in seconds. Not a technical limit: it is the
 * point past which the request is more likely a typo than an intention, and a
 * reminder that this buffer is resident memory - a minute of 24-bit stereo at
 * 48 kHz is about 17 MiB, and the rate and channel maxima allow far worse.
 */
#define AUD_PREROLL_MAX_SECONDS 300.0

typedef struct
{
  unsigned char *data;
  size_t frame_bytes; /* channels * bytes per sample, in the hardware format */
  size_t capacity;    /* frames the ring holds before the oldest are dropped */
  size_t head;        /* frame index one past the newest */
  size_t filled;      /* frames currently held; <= capacity */
} aud_preroll;

/* One contiguous run of stored frames, oldest first. */
typedef struct
{
  const unsigned char *data;
  size_t frames;
} aud_preroll_segment;

/*
 * Allocate a ring holding exactly `frames` frames of `frame_bytes` each.
 * Returns 0 on success, -1 with errno set to EINVAL for a zero or overflowing
 * size and ENOMEM when the allocation fails. Unlike ringbuf.h the capacity is
 * taken literally: this one is sized in seconds of audio a person asked for,
 * and rounding it up to a power of two would quietly double the memory.
 */
int aud_preroll_init(aud_preroll *pr, size_t frames, size_t frame_bytes);

/* Safe on NULL and on an already freed ring. */
void aud_preroll_free(aud_preroll *pr);

size_t aud_preroll_capacity(const aud_preroll *pr);
size_t aud_preroll_filled(const aud_preroll *pr);
size_t aud_preroll_bytes(const aud_preroll *pr);

/* Forget everything buffered, without releasing the allocation. */
void aud_preroll_clear(aud_preroll *pr);

/*
 * Store `count` frames, dropping the oldest to make room. Never refuses and
 * never blocks: a capture thread cannot wait for anything.
 */
void aud_preroll_push(aud_preroll *pr, const void *frames, size_t count);

/*
 * Describe the buffered frames as up to two contiguous runs in `out`, oldest
 * first, and return how many runs were filled in (0 when empty). The pointers
 * stay valid until the next push, clear or free.
 */
unsigned aud_preroll_segments(const aud_preroll *pr, aud_preroll_segment out[2]);

/*
 * Frames a `seconds` long ring needs at `rate` Hz, clamped to the maximum and
 * saturating rather than overflowing. Here so the recorder and the desktop app
 * cannot disagree about what "5 seconds of pre-roll" means.
 */
size_t aud_preroll_frames_for(double seconds, unsigned rate);

#endif /* AUDIAKI_PREROLL_H */
