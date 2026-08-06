/* SPDX-License-Identifier: MIT */
#include "canvas.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int aud_canvas_init(aud_canvas *c, size_t width, size_t height)
{
  size_t pixels;

  if (c == NULL || width == 0 || height == 0)
  {
    errno = EINVAL;
    return -1;
  }

  memset(c, 0, sizeof(*c));

  /* reject a size whose pixel count or byte count would wrap */
  if (height > SIZE_MAX / width)
  {
    errno = EINVAL;
    return -1;
  }
  pixels = width * height;
  if (pixels > SIZE_MAX / sizeof(uint32_t))
  {
    errno = EINVAL;
    return -1;
  }

  c->pixels = malloc(pixels * sizeof(uint32_t));
  if (c->pixels == NULL)
  {
    errno = ENOMEM;
    return -1;
  }
  c->width = width;
  c->height = height;
  return 0;
}

void aud_canvas_free(aud_canvas *c)
{
  if (c == NULL)
  {
    return;
  }

  free(c->pixels);
  c->pixels = NULL;
  c->width = 0;
  c->height = 0;
}

size_t aud_canvas_bytes(const aud_canvas *c)
{
  if (c == NULL || c->pixels == NULL)
  {
    return 0;
  }
  return c->width * c->height * sizeof(uint32_t);
}

void aud_canvas_clear(aud_canvas *c, uint32_t color)
{
  size_t n;

  if (c == NULL || c->pixels == NULL)
  {
    return;
  }

  n = c->width * c->height;
  for (size_t i = 0; i < n; i++)
  {
    c->pixels[i] = color;
  }
}

/*
 * Clip a rectangle to the canvas. Returns 0 when nothing is left to draw,
 * otherwise fills in the half-open pixel ranges.
 */
static int clip_rect(const aud_canvas *c, long x, long y, long w, long h, size_t *x0,
                     size_t *y0, size_t *x1, size_t *y1)
{
  long right;
  long bottom;

  if (c == NULL || c->pixels == NULL || w <= 0 || h <= 0)
  {
    return 0;
  }

  right = x + w;
  bottom = y + h;

  if (x < 0)
  {
    x = 0;
  }
  if (y < 0)
  {
    y = 0;
  }
  if (right > (long)c->width)
  {
    right = (long)c->width;
  }
  if (bottom > (long)c->height)
  {
    bottom = (long)c->height;
  }
  if (x >= right || y >= bottom)
  {
    return 0;
  }

  *x0 = (size_t)x;
  *y0 = (size_t)y;
  *x1 = (size_t)right;
  *y1 = (size_t)bottom;
  return 1;
}

void aud_canvas_fill_rect(aud_canvas *c, long x, long y, long w, long h, uint32_t color)
{
  size_t x0, y0, x1, y1;

  if (!clip_rect(c, x, y, w, h, &x0, &y0, &x1, &y1))
  {
    return;
  }

  for (size_t row = y0; row < y1; row++)
  {
    uint32_t *line = c->pixels + row * c->width;
    for (size_t col = x0; col < x1; col++)
    {
      line[col] = color;
    }
  }
}

void aud_canvas_fill_gradient(aud_canvas *c, long x, long y, long w, long h, uint32_t top,
                              uint32_t bottom)
{
  size_t x0, y0, x1, y1;

  if (!clip_rect(c, x, y, w, h, &x0, &y0, &x1, &y1))
  {
    return;
  }

  for (size_t row = y0; row < y1; row++)
  {
    /*
     * Interpolate against the unclipped rectangle so a bar that runs off the
     * top of the frame keeps the same colour ramp as one that fits.
     */
    double t = h > 1 ? (double)((long)row - y) / (double)(h - 1) : 0.0;
    uint32_t color = aud_canvas_mix(top, bottom, t);
    uint32_t *line = c->pixels + row * c->width;

    for (size_t col = x0; col < x1; col++)
    {
      line[col] = color;
    }
  }
}

static unsigned channel(uint32_t color, unsigned shift)
{
  return (unsigned)((color >> shift) & 0xFFu);
}

uint32_t aud_canvas_mix(uint32_t a, uint32_t b, double t)
{
  unsigned out[4];

  if (t < 0.0)
  {
    t = 0.0;
  }
  if (t > 1.0)
  {
    t = 1.0;
  }

  for (unsigned i = 0; i < 4; i++)
  {
    double va = (double)channel(a, i * 8);
    double vb = (double)channel(b, i * 8);
    out[i] = (unsigned)(va + (vb - va) * t + 0.5);
  }

  return AUD_RGBA(out[0], out[1], out[2], out[3]);
}

uint32_t aud_canvas_shade(uint32_t color, double k)
{
  unsigned out[3];

  if (k < 0.0)
  {
    k = 0.0;
  }

  for (unsigned i = 0; i < 3; i++)
  {
    double v = (double)channel(color, i * 8) * k;
    if (v > 255.0)
    {
      v = 255.0;
    }
    out[i] = (unsigned)(v + 0.5);
  }

  return AUD_RGBA(out[0], out[1], out[2], channel(color, 24));
}

uint32_t aud_canvas_hsv(double hue, double saturation, double value, unsigned alpha)
{
  double c;
  double x;
  double m;
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  int sector;

  hue = fmod(hue, 360.0);
  if (hue < 0.0)
  {
    hue += 360.0;
  }
  if (saturation < 0.0)
  {
    saturation = 0.0;
  }
  if (saturation > 1.0)
  {
    saturation = 1.0;
  }
  if (value < 0.0)
  {
    value = 0.0;
  }
  if (value > 1.0)
  {
    value = 1.0;
  }

  c = value * saturation;
  x = c * (1.0 - fabs(fmod(hue / 60.0, 2.0) - 1.0));
  m = value - c;
  sector = (int)(hue / 60.0);

  switch (sector)
  {
  case 0:
    r = c;
    g = x;
    break;
  case 1:
    r = x;
    g = c;
    break;
  case 2:
    g = c;
    b = x;
    break;
  case 3:
    g = x;
    b = c;
    break;
  case 4:
    r = x;
    b = c;
    break;
  default:
    r = c;
    b = x;
    break;
  }

  return AUD_RGBA((unsigned)((r + m) * 255.0 + 0.5), (unsigned)((g + m) * 255.0 + 0.5),
                  (unsigned)((b + m) * 255.0 + 0.5), alpha & 0xFFu);
}
