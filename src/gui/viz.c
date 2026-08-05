/* SPDX-License-Identifier: MIT */
#include "viz.h"

#include "spectrum.h"

#include <math.h>
#include <stdlib.h>

/* Side of the glow sprite in texels. Bigger only softens what is already soft. */
#define VIZ_GLOW_TEXELS 128

/*
 * Hue sweep across the spectrum, in degrees. Stopping at magenta rather than
 * wrapping to red keeps the two ends of the display distinguishable.
 */
#define VIZ_HUE_LOW 0.0f
#define VIZ_HUE_HIGH 300.0f

/*
 * Proportions, all relative to one band's slot width. Taken from the reference
 * image: hairline stems, and halos several slots wide, so neighbouring caps
 * overlap and sum towards white rather than reading as separate dots.
 */
#define VIZ_STEM_WIDTH 0.22f
#define VIZ_STEM_GLOW_WIDTH 1.10f
#define VIZ_CAP_HALO 7.00f
#define VIZ_CAP_CORE 1.60f

/*
 * Halos are also given a floor in pixels. Everything else here scales with the
 * slot, which means a narrow window or a high band count shrinks the glow into
 * nothing - the bloom should not depend on how wide the window happens to be.
 */
#define VIZ_HALO_MIN_PX 34.0f

/* A bar at zero still shows a stub, so the row of stems never disappears. */
#define VIZ_MIN_HEIGHT 2.0f

struct aud_viz
{
  aud_spectrum *spectrum;
  size_t bands;
  Texture2D glow;
  Color *palette;      /* one entry per band */
  const float *values; /* the last analysis, owned by the spectrum */
  int glow_ready;
};

/*
 * A white disc whose alpha falls off from the centre. Tinting it at draw time
 * is what makes every band a different colour without a texture each.
 *
 * The exponent sets how tight the core is: 1.0 is a linear cone that reads as
 * a flat circle, and much above 3.0 the skirt disappears and the caps become
 * hard dots. 1.6 keeps a bright centre with the long soft falloff the
 * reference image has.
 */
static Texture2D make_glow_texture(void)
{
  Color *pixels = malloc((size_t)VIZ_GLOW_TEXELS * VIZ_GLOW_TEXELS * sizeof(*pixels));
  Image img;
  Texture2D tex;
  float centre = (float)VIZ_GLOW_TEXELS / 2.0f;

  if (pixels == NULL)
  {
    Texture2D none = {0};
    return none;
  }

  for (int y = 0; y < VIZ_GLOW_TEXELS; y++)
  {
    for (int x = 0; x < VIZ_GLOW_TEXELS; x++)
    {
      float dx = ((float)x + 0.5f - centre) / centre;
      float dy = ((float)y + 0.5f - centre) / centre;
      float d = sqrtf(dx * dx + dy * dy);
      float a = d >= 1.0f ? 0.0f : powf(1.0f - d, 1.6f);
      Color c = {255, 255, 255, (unsigned char)(a * 255.0f + 0.5f)};

      pixels[(size_t)y * VIZ_GLOW_TEXELS + (size_t)x] = c;
    }
  }

  img.data = pixels;
  img.width = VIZ_GLOW_TEXELS;
  img.height = VIZ_GLOW_TEXELS;
  img.mipmaps = 1;
  img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

  tex = LoadTextureFromImage(img);
  SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
  free(pixels);
  return tex;
}

aud_viz *aud_viz_create(unsigned rate, size_t bands)
{
  aud_spectrum_config cfg;
  aud_viz *v;

  if (bands < AUD_SPECTRUM_MIN_BANDS)
    bands = AUD_SPECTRUM_MIN_BANDS;
  if (bands > AUD_SPECTRUM_MAX_BANDS)
    bands = AUD_SPECTRUM_MAX_BANDS;

  v = calloc(1, sizeof(*v));
  if (v == NULL)
    return NULL;

  aud_spectrum_config_defaults(&cfg, rate, bands);
  v->spectrum = aud_spectrum_create(&cfg);
  if (v->spectrum == NULL)
  {
    free(v);
    return NULL;
  }

  v->palette = malloc(bands * sizeof(*v->palette));
  if (v->palette == NULL)
  {
    aud_spectrum_destroy(v->spectrum);
    free(v);
    return NULL;
  }

  for (size_t b = 0; b < bands; b++)
  {
    float t = bands > 1 ? (float)b / (float)(bands - 1) : 0.0f;
    float hue = VIZ_HUE_LOW + (VIZ_HUE_HIGH - VIZ_HUE_LOW) * t;

    /* not quite full saturation: pure hues on black look harsher than this */
    v->palette[b] = ColorFromHSV(hue, 0.92f, 1.0f);
  }

  v->bands = bands;
  v->glow = make_glow_texture();
  v->glow_ready = v->glow.id != 0;
  return v;
}

void aud_viz_destroy(aud_viz *v)
{
  if (v == NULL)
    return;

  if (v->glow_ready)
    UnloadTexture(v->glow);
  aud_spectrum_destroy(v->spectrum);
  free(v->palette);
  free(v);
}

void aud_viz_push(aud_viz *v, const float *mono, size_t frames)
{
  if (v == NULL)
    return;

  aud_spectrum_push(v->spectrum, mono, frames);
}

/*
 * The transform runs here, not in the draw call, so a frame that draws the
 * bars more than once - or not at all - still advances the smoothing exactly
 * once per elapsed dt.
 */
void aud_viz_update(aud_viz *v, float dt)
{
  if (v == NULL)
    return;

  v->values = aud_spectrum_analyse(v->spectrum, (double)dt);
}

size_t aud_viz_bands(const aud_viz *v)
{
  return v != NULL ? v->bands : 0;
}

Color aud_viz_band_color(const aud_viz *v, size_t band)
{
  if (v == NULL || band >= v->bands)
    return RAYWHITE;

  return v->palette[band];
}

/* Draw the glow sprite centred on (cx, cy) at `size` pixels across. */
static void draw_glow(const aud_viz *v, float cx, float cy, float size, Color tint)
{
  Rectangle src = {0.0f, 0.0f, (float)v->glow.width, (float)v->glow.height};
  Rectangle dst = {cx - size / 2.0f, cy - size / 2.0f, size, size};
  Vector2 origin = {0.0f, 0.0f};

  DrawTexturePro(v->glow, src, dst, origin, 0.0f, tint);
}

static Color with_alpha(Color c, float alpha)
{
  if (alpha < 0.0f)
    alpha = 0.0f;
  if (alpha > 1.0f)
    alpha = 1.0f;

  c.a = (unsigned char)(alpha * 255.0f + 0.5f);
  return c;
}

/*
 * One pass of stems and one of caps, rather than finishing each bar before
 * starting the next. Additive blending is order independent, but batching the
 * textured quads together keeps raylib from flushing between blend modes on
 * every band.
 */
static void draw_bars(const aud_viz *v, Rectangle area, const float *values)
{
  float slot = area.width / (float)v->bands;
  float stem_w = slot * VIZ_STEM_WIDTH;
  float glow_w = slot * VIZ_STEM_GLOW_WIDTH;
  float baseline = area.y + area.height;

  if (stem_w < 1.0f)
    stem_w = 1.0f;

  /* stems: a soft additive wash first, then the solid hairline over it */
  BeginBlendMode(BLEND_ADDITIVE);
  for (size_t b = 0; b < v->bands; b++)
  {
    float h = values[b] * area.height;
    float cx = area.x + slot * ((float)b + 0.5f);
    Rectangle r;

    if (h < VIZ_MIN_HEIGHT)
      h = VIZ_MIN_HEIGHT;

    r.x = cx - glow_w / 2.0f;
    r.y = baseline - h;
    r.width = glow_w;
    r.height = h;
    DrawRectangleRec(r, with_alpha(v->palette[b], 0.10f + 0.12f * values[b]));
  }
  EndBlendMode();

  for (size_t b = 0; b < v->bands; b++)
  {
    float h = values[b] * area.height;
    float cx = area.x + slot * ((float)b + 0.5f);

    if (h < VIZ_MIN_HEIGHT)
      h = VIZ_MIN_HEIGHT;

    DrawRectangleRec((Rectangle){cx - stem_w / 2.0f, baseline - h, stem_w, h},
                     v->palette[b]);
  }

  /* caps: a wide halo, then a tight core that saturates towards white */
  BeginBlendMode(BLEND_ADDITIVE);
  for (size_t b = 0; b < v->bands; b++)
  {
    float value = values[b];
    float h = value * area.height;
    float cx = area.x + slot * ((float)b + 0.5f);
    float halo = slot * VIZ_CAP_HALO;
    float cy;

    if (h < VIZ_MIN_HEIGHT)
      h = VIZ_MIN_HEIGHT;
    cy = baseline - h;

    if (halo < VIZ_HALO_MIN_PX)
      halo = VIZ_HALO_MIN_PX;

    /* louder bands bloom wider, which is what gives the display its dynamics */
    draw_glow(v, cx, cy, halo * (0.55f + 0.45f * value),
              with_alpha(v->palette[b], 0.30f + 0.40f * value));
    draw_glow(v, cx, cy, slot * VIZ_CAP_CORE * (0.70f + 0.30f * value),
              with_alpha(v->palette[b], 0.65f + 0.35f * value));
  }
  EndBlendMode();
}

void aud_viz_draw(const aud_viz *v, Rectangle area)
{
  if (v == NULL || !v->glow_ready || v->values == NULL)
    return;
  if (area.width <= 0.0f || area.height <= 0.0f)
    return;

  draw_bars(v, area, v->values);
}

void aud_viz_draw_idle(const aud_viz *v, Rectangle area)
{
  float slot;
  float baseline;
  float stem_w;

  if (v == NULL || area.width <= 0.0f || area.height <= 0.0f)
    return;

  slot = area.width / (float)v->bands;
  baseline = area.y + area.height;
  stem_w = slot * VIZ_STEM_WIDTH;
  if (stem_w < 1.0f)
    stem_w = 1.0f;

  for (size_t b = 0; b < v->bands; b++)
  {
    float cx = area.x + slot * ((float)b + 0.5f);

    DrawRectangleRec((Rectangle){cx - stem_w / 2.0f, baseline - VIZ_MIN_HEIGHT, stem_w,
                                 VIZ_MIN_HEIGHT},
                     with_alpha(v->palette[b], 0.35f));
  }
}
