/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "canvas.h"

#include <stdlib.h>

static uint32_t pixel_at(const aud_canvas *c, size_t x, size_t y)
{
  return c->pixels[y * c->width + x];
}

TEST(init_rejects_empty_and_oversized)
{
  aud_canvas c;

  CHECK_EQ_INT(aud_canvas_init(&c, 0, 10), -1);
  CHECK_EQ_INT(aud_canvas_init(&c, 10, 0), -1);
  /* a pixel count that would wrap size_t */
  CHECK_EQ_INT(aud_canvas_init(&c, SIZE_MAX, SIZE_MAX), -1);
}

TEST(rgba_byte_order_matches_ffmpeg)
{
  uint32_t color = AUD_RGBA(0x12, 0x34, 0x56, 0x78);
  unsigned char bytes[4];

  memcpy(bytes, &color, sizeof(color));

  /*
   * ffmpeg's "rgba" wants R, G, B, A in memory order. This is the assumption
   * the whole render path rests on, and it is little-endian specific.
   */
  CHECK_EQ_INT(bytes[0], 0x12);
  CHECK_EQ_INT(bytes[1], 0x34);
  CHECK_EQ_INT(bytes[2], 0x56);
  CHECK_EQ_INT(bytes[3], 0x78);
}

TEST(clear_and_bytes)
{
  aud_canvas c;
  uint32_t color = AUD_RGBA(1, 2, 3, 4);

  CHECK_EQ_INT(aud_canvas_init(&c, 4, 3), 0);
  CHECK_EQ_INT(aud_canvas_bytes(&c), 4 * 3 * 4);

  aud_canvas_clear(&c, color);
  for (size_t y = 0; y < 3; y++)
  {
    for (size_t x = 0; x < 4; x++)
      CHECK_EQ_INT(pixel_at(&c, x, y), color);
  }

  aud_canvas_free(&c);
  CHECK(c.pixels == NULL);
  CHECK_EQ_INT(aud_canvas_bytes(&c), 0);
}

TEST(fill_rect_stays_inside)
{
  aud_canvas c;
  uint32_t bg = AUD_RGBA(0, 0, 0, 255);
  uint32_t fg = AUD_RGBA(255, 0, 0, 255);

  CHECK_EQ_INT(aud_canvas_init(&c, 8, 8), 0);
  aud_canvas_clear(&c, bg);

  aud_canvas_fill_rect(&c, 2, 3, 3, 2, fg);

  CHECK_EQ_INT(pixel_at(&c, 2, 3), fg);
  CHECK_EQ_INT(pixel_at(&c, 4, 4), fg);
  CHECK_EQ_INT(pixel_at(&c, 1, 3), bg); /* one left of the rect */
  CHECK_EQ_INT(pixel_at(&c, 5, 3), bg); /* one right of it */
  CHECK_EQ_INT(pixel_at(&c, 2, 2), bg); /* one above */
  CHECK_EQ_INT(pixel_at(&c, 2, 5), bg); /* one below */

  aud_canvas_free(&c);
}

TEST(fill_rect_clips_rather_than_overflows)
{
  aud_canvas c;
  uint32_t bg = AUD_RGBA(0, 0, 0, 255);
  uint32_t fg = AUD_RGBA(0, 255, 0, 255);

  CHECK_EQ_INT(aud_canvas_init(&c, 4, 4), 0);
  aud_canvas_clear(&c, bg);

  /* straddling every edge, plus degenerate and fully outside rectangles */
  aud_canvas_fill_rect(&c, -2, -2, 3, 3, fg);
  CHECK_EQ_INT(pixel_at(&c, 0, 0), fg);
  CHECK_EQ_INT(pixel_at(&c, 1, 1), bg);

  aud_canvas_fill_rect(&c, 3, 3, 100, 100, fg);
  CHECK_EQ_INT(pixel_at(&c, 3, 3), fg);

  aud_canvas_fill_rect(&c, 0, 0, 0, 5, fg);
  aud_canvas_fill_rect(&c, 0, 0, 5, -1, fg);
  aud_canvas_fill_rect(&c, 100, 100, 5, 5, fg);
  aud_canvas_fill_rect(&c, -50, 0, 10, 4, fg);
  CHECK_EQ_INT(pixel_at(&c, 1, 1), bg); /* none of those touched anything */

  aud_canvas_free(&c);
}

TEST(gradient_runs_top_to_bottom)
{
  aud_canvas c;
  uint32_t top = AUD_RGBA(0, 0, 0, 255);
  uint32_t bottom = AUD_RGBA(255, 255, 255, 255);

  CHECK_EQ_INT(aud_canvas_init(&c, 2, 5), 0);
  aud_canvas_fill_gradient(&c, 0, 0, 2, 5, top, bottom);

  CHECK_EQ_INT(pixel_at(&c, 0, 0), top);
  CHECK_EQ_INT(pixel_at(&c, 0, 4), bottom);
  /* the middle row is the halfway mix, to within rounding */
  CHECK_EQ_INT(pixel_at(&c, 0, 2) & 0xFFu, 128);

  aud_canvas_free(&c);
}

TEST(mix_clamps_and_interpolates)
{
  uint32_t a = AUD_RGBA(0, 10, 20, 30);
  uint32_t b = AUD_RGBA(100, 110, 120, 130);

  CHECK_EQ_INT(aud_canvas_mix(a, b, 0.0), a);
  CHECK_EQ_INT(aud_canvas_mix(a, b, 1.0), b);
  CHECK_EQ_INT(aud_canvas_mix(a, b, -5.0), a);
  CHECK_EQ_INT(aud_canvas_mix(a, b, 5.0), b);
  CHECK_EQ_INT(aud_canvas_mix(a, b, 0.5), AUD_RGBA(50, 60, 70, 80));
}

TEST(shade_scales_colour_and_keeps_alpha)
{
  uint32_t c = AUD_RGBA(100, 200, 50, 0xFF);
  uint32_t half = aud_canvas_shade(c, 0.5);

  CHECK_EQ_INT(half, AUD_RGBA(50, 100, 25, 0xFF));
  CHECK_EQ_INT(aud_canvas_shade(c, 0.0), AUD_RGBA(0, 0, 0, 0xFF));
  /* saturating rather than wrapping */
  CHECK_EQ_INT(aud_canvas_shade(c, 10.0), AUD_RGBA(255, 255, 255, 0xFF));
  CHECK_EQ_INT(aud_canvas_shade(c, -1.0), AUD_RGBA(0, 0, 0, 0xFF));
}

TEST(hsv_hits_the_primaries)
{
  CHECK_EQ_INT(aud_canvas_hsv(0.0, 1.0, 1.0, 0xFF), AUD_RGBA(255, 0, 0, 0xFF));
  CHECK_EQ_INT(aud_canvas_hsv(120.0, 1.0, 1.0, 0xFF), AUD_RGBA(0, 255, 0, 0xFF));
  CHECK_EQ_INT(aud_canvas_hsv(240.0, 1.0, 1.0, 0xFF), AUD_RGBA(0, 0, 255, 0xFF));
  /* hue wraps, saturation zero is grey, value zero is black */
  CHECK_EQ_INT(aud_canvas_hsv(360.0, 1.0, 1.0, 0xFF), AUD_RGBA(255, 0, 0, 0xFF));
  CHECK_EQ_INT(aud_canvas_hsv(-120.0, 1.0, 1.0, 0xFF), AUD_RGBA(0, 0, 255, 0xFF));
  CHECK_EQ_INT(aud_canvas_hsv(200.0, 0.0, 1.0, 0xFF), AUD_RGBA(255, 255, 255, 0xFF));
  CHECK_EQ_INT(aud_canvas_hsv(200.0, 1.0, 0.0, 0xFF), AUD_RGBA(0, 0, 0, 0xFF));
}

int main(void)
{
  RUN(init_rejects_empty_and_oversized);
  RUN(rgba_byte_order_matches_ffmpeg);
  RUN(clear_and_bytes);
  RUN(fill_rect_stays_inside);
  RUN(fill_rect_clips_rather_than_overflows);
  RUN(gradient_runs_top_to_bottom);
  RUN(mix_clamps_and_interpolates);
  RUN(shade_scales_colour_and_keeps_alpha);
  RUN(hsv_hits_the_primaries);
  return TEST_RESULT();
}
