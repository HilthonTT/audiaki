/* SPDX-License-Identifier: MIT */
/*
 * canvas.h - a plain RGBA framebuffer and the few shapes the visualiser needs.
 *
 * The alternative was linking a graphics library to draw rectangles. This is
 * around a hundred lines instead, keeps audiaki's dependency list at libasound
 * plus libm, and hands ffmpeg exactly the byte layout it wants.
 *
 * Pixels are uint32_t words holding 0xAABBGGRR, which on a little-endian host
 * lays out in memory as R, G, B, A - ffmpeg's "rgba" raw video format. Like
 * the rest of audiaki this assumes a little-endian host.
 */
#ifndef AUDIAKI_CANVAS_H
#define AUDIAKI_CANVAS_H

#include <stddef.h>
#include <stdint.h>

#define AUD_RGBA(r, g, b, a)                                                      \
  ((uint32_t)((uint32_t)(r) & 0xFFu) | ((uint32_t)((uint32_t)(g) & 0xFFu) << 8) | \
   ((uint32_t)((uint32_t)(b) & 0xFFu) << 16) |                                    \
   ((uint32_t)((uint32_t)(a) & 0xFFu) << 24))

typedef struct
{
  uint32_t *pixels;
  size_t width;
  size_t height;
} aud_canvas;

/*
 * Allocate a width x height framebuffer. Returns 0 on success, -1 with errno
 * set to EINVAL for a zero or overflowing size, or ENOMEM.
 */
int aud_canvas_init(aud_canvas *c, size_t width, size_t height);

void aud_canvas_free(aud_canvas *c);

/* Bytes one frame occupies, which is what gets written to the ffmpeg pipe. */
size_t aud_canvas_bytes(const aud_canvas *c);

void aud_canvas_clear(aud_canvas *c, uint32_t color);

/*
 * Fill a rectangle, clipped to the canvas. Signed coordinates so callers can
 * compute a position that lands partly off screen without special casing it.
 */
void aud_canvas_fill_rect(aud_canvas *c, long x, long y, long w, long h, uint32_t color);

/* Fill a rectangle with a vertical gradient from `top` to `bottom`. */
void aud_canvas_fill_gradient(aud_canvas *c, long x, long y, long w, long h, uint32_t top,
                              uint32_t bottom);

/* Blend `a` towards `b`; t is clamped to [0.0, 1.0]. Alpha blends too. */
uint32_t aud_canvas_mix(uint32_t a, uint32_t b, double t);

/* Scale a colour's channels by `k`, keeping its alpha. Used for the reflection. */
uint32_t aud_canvas_shade(uint32_t color, double k);

/*
 * Convert HSV to a canvas colour. hue is in degrees and wraps, saturation and
 * value are in [0.0, 1.0]. Cheaper than shipping a palette table and makes the
 * low-to-high colour ramp across the bars a one liner.
 */
uint32_t aud_canvas_hsv(double hue, double saturation, double value, unsigned alpha);

#endif /* AUDIAKI_CANVAS_H */
