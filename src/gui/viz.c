/* SPDX-License-Identifier: MIT */
#include "viz.h"

#include "ui.h"

#include "spectrum.h"
#include "tuner.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/*
 * Samples kept for the oscilloscope. At 44.1 kHz this is about 90 ms, which
 * leaves room to hunt backwards for a trigger point and still have a full
 * sweep of samples after it.
 */
#define VIZ_WAVE_SAMPLES 4096u

/* Samples the scope actually draws, and the vertices it draws them as. */
#define VIZ_SCOPE_SPAN 1024u
#define VIZ_SCOPE_POINTS 256u

/*
 * Columns of spectrogram history. At 60 fps this is around eight seconds,
 * which covers a phrase without the detail smearing.
 */
#define VIZ_FALL_COLUMNS 512

/*
 * Seconds between pitch analyses. The detection costs millions of operations,
 * which is nothing twenty times a second and far too much sixty - and a needle
 * updated faster than this only shakes.
 */
#define VIZ_TUNER_INTERVAL 0.05f

/* Half a semitone either side of the note, the same scale the CLI tuner draws. */
#define VIZ_TUNER_RANGE_CENTS 50.0

struct aud_viz
{
  aud_spectrum *spectrum;
  size_t bands;
  aud_viz_mode mode;

  Texture2D glow;
  Color *palette;      /* one entry per band */
  const float *values; /* the last analysis, owned by the spectrum */
  int glow_ready;

  /* raw sample history, for the scope */
  float *wave;
  size_t wave_head; /* where the next sample goes */

  /* spectrogram history, as a ring of texture columns */
  Texture2D fall;
  Color *column; /* staging for one column, `bands` entries */
  int fall_head; /* the column written most recently */
  int fall_ready;

  /* pitch detection, for the tuner style */
  aud_tuner *tuner;
  aud_tuner_reading reading;
  float tuner_clock; /* seconds of dt banked since the last analysis */
  double a4_hz;
};

static const char *const mode_names[AUD_VIZ_MODE_COUNT] = {
    "bars", "mirror", "radial", "scope", "waterfall", "tuner",
};

const char *aud_viz_mode_name(aud_viz_mode mode)
{
  if (mode < 0 || mode >= AUD_VIZ_MODE_COUNT)
  {
    return "unknown";
  }

  return mode_names[mode];
}

int aud_viz_mode_from_name(const char *name, aud_viz_mode *out)
{
  if (name == NULL || out == NULL)
  {
    return -1;
  }

  for (int i = 0; i < AUD_VIZ_MODE_COUNT; i++)
  {
    const char *a = name;
    const char *b = mode_names[i];

    while (*a != '\0' && *b != '\0')
    {
      char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
      if (ca != *b)
      {
        break;
      }
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0')
    {
      *out = (aud_viz_mode)i;
      return 0;
    }
  }
  return -1;
}

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

/* An empty spectrogram, one texel column per frame of history. */
static Texture2D make_fall_texture(size_t bands)
{
  Color *pixels =
      calloc((size_t)VIZ_FALL_COLUMNS * bands, sizeof(*pixels)); /* transparent */
  Image img;
  Texture2D tex;

  if (pixels == NULL)
  {
    Texture2D none = {0};
    return none;
  }

  img.data = pixels;
  img.width = VIZ_FALL_COLUMNS;
  img.height = (int)bands;
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
  aud_tuner_config tuner_cfg;
  aud_viz *v;

  if (bands < AUD_SPECTRUM_MIN_BANDS)
  {
    bands = AUD_SPECTRUM_MIN_BANDS;
  }
  if (bands > AUD_SPECTRUM_MAX_BANDS)
  {
    bands = AUD_SPECTRUM_MAX_BANDS;
  }

  v = calloc(1, sizeof(*v));
  if (v == NULL)
  {
    return NULL;
  }

  aud_spectrum_config_defaults(&cfg, rate, bands);
  v->spectrum = aud_spectrum_create(&cfg);
  if (v->spectrum == NULL)
  {
    free(v);
    return NULL;
  }

  aud_tuner_config_defaults(&tuner_cfg, rate);
  v->a4_hz = tuner_cfg.a4_hz;
  v->tuner = aud_tuner_create(&tuner_cfg);
  if (v->tuner == NULL)
  {
    aud_viz_destroy(v);
    return NULL;
  }
  aud_tuner_describe(0.0, 0.0, &v->reading); /* unvoiced until the first analysis */

  v->palette = malloc(bands * sizeof(*v->palette));
  v->wave = calloc(VIZ_WAVE_SAMPLES, sizeof(*v->wave));
  v->column = malloc(bands * sizeof(*v->column));

  if (v->palette == NULL || v->wave == NULL || v->column == NULL)
  {
    aud_viz_destroy(v);
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
  v->mode = AUD_VIZ_MODE_BARS;

  v->glow = make_glow_texture();
  v->glow_ready = v->glow.id != 0;

  v->fall = make_fall_texture(bands);
  v->fall_ready = v->fall.id != 0;
  v->fall_head = VIZ_FALL_COLUMNS - 1;

  return v;
}

void aud_viz_destroy(aud_viz *v)
{
  if (v == NULL)
  {
    return;
  }

  if (v->glow_ready)
  {
    UnloadTexture(v->glow);
  }
  if (v->fall_ready)
  {
    UnloadTexture(v->fall);
  }

  aud_spectrum_destroy(v->spectrum);
  aud_tuner_destroy(v->tuner);
  free(v->palette);
  free(v->wave);
  free(v->column);
  free(v);
}

void aud_viz_push(aud_viz *v, const float *mono, size_t frames)
{
  if (v == NULL || mono == NULL || frames == 0)
  {
    return;
  }

  aud_spectrum_push(v->spectrum, mono, frames);
  aud_tuner_push(v->tuner, mono, frames);

  /* only the newest VIZ_WAVE_SAMPLES can ever be drawn */
  if (frames >= VIZ_WAVE_SAMPLES)
  {
    memcpy(v->wave, mono + (frames - VIZ_WAVE_SAMPLES),
           VIZ_WAVE_SAMPLES * sizeof(*v->wave));
    v->wave_head = 0;
    return;
  }

  for (size_t i = 0; i < frames; i++)
  {
    v->wave[v->wave_head] = mono[i];
    v->wave_head = (v->wave_head + 1) % VIZ_WAVE_SAMPLES;
  }
}

/* The scope's history, oldest first, so the draw code can index it plainly. */
static float wave_at(const aud_viz *v, size_t age)
{
  size_t i = (v->wave_head + VIZ_WAVE_SAMPLES - 1 - age) % VIZ_WAVE_SAMPLES;

  return v->wave[i];
}

/*
 * Append the current spectrum to the spectrogram. Writing one texel column and
 * moving a read offset, rather than scrolling the whole image, keeps this at a
 * single small texture upload per frame.
 */
static void push_column(aud_viz *v)
{
  Rectangle dst;

  if (!v->fall_ready || v->values == NULL)
  {
    return;
  }

  v->fall_head = (v->fall_head + 1) % VIZ_FALL_COLUMNS;

  for (size_t b = 0; b < v->bands; b++)
  {
    float value = v->values[b];
    Color c = v->palette[b];
    /* gamma below 1 lifts the quiet detail that a spectrogram is read for */
    float lit = powf(value, 0.7f);

    c.r = (unsigned char)((float)c.r * lit);
    c.g = (unsigned char)((float)c.g * lit);
    c.b = (unsigned char)((float)c.b * lit);
    c.a = 255;

    /* row 0 is the top of the texture, and low frequencies belong at the bottom */
    v->column[v->bands - 1 - b] = c;
  }

  dst.x = (float)v->fall_head;
  dst.y = 0.0f;
  dst.width = 1.0f;
  dst.height = (float)v->bands;
  UpdateTextureRec(v->fall, dst, v->column);
}

void aud_viz_update(aud_viz *v, float dt)
{
  if (v == NULL)
  {
    return;
  }

  /*
   * The transform runs here, not in the draw call, so a frame that draws the
   * bars more than once - or not at all - still advances the smoothing exactly
   * once per elapsed dt.
   */
  v->values = aud_spectrum_analyse(v->spectrum, (double)dt);
  push_column(v);

  /*
   * The pitch runs on its own slower clock, and banks the dt it skipped so the
   * needle settles at the same speed whichever rate the window is drawing at.
   *
   * Only while it is the visible style. The detection is millions of operations
   * a go, and a video render calls this as fast as the encoder will take frames
   * - paying for a needle nobody is looking at would slow every render down.
   */
  if (v->mode != AUD_VIZ_MODE_TUNER)
  {
    return;
  }

  v->tuner_clock += dt;
  if (v->tuner_clock >= VIZ_TUNER_INTERVAL)
  {
    aud_tuner_analyse(v->tuner, (double)v->tuner_clock, &v->reading);
    v->tuner_clock = 0.0f;
  }
}

void aud_viz_set_mode(aud_viz *v, aud_viz_mode mode)
{
  if (v == NULL || mode < 0 || mode >= AUD_VIZ_MODE_COUNT)
  {
    return;
  }

  v->mode = mode;

  /*
   * Arrive at the tuner already due an analysis. The detection only runs while
   * the tuner is on screen, so without this the first fiftieth of a second of
   * it would be the reading from whenever it was last looked at.
   */
  if (mode == AUD_VIZ_MODE_TUNER)
  {
    v->tuner_clock = VIZ_TUNER_INTERVAL;
  }
}

aud_viz_mode aud_viz_cycle_mode(aud_viz *v)
{
  if (v == NULL)
  {
    return AUD_VIZ_MODE_BARS;
  }

  aud_viz_set_mode(v, (aud_viz_mode)((v->mode + 1) % AUD_VIZ_MODE_COUNT));
  return v->mode;
}

/* -- shared drawing helpers ------------------------------------------------ */

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
  {
    alpha = 0.0f;
  }
  if (alpha > 1.0f)
  {
    alpha = 1.0f;
  }

  c.a = (unsigned char)(alpha * 255.0f + 0.5f);
  return c;
}

/* The halo and core a cap is made of, at whatever point it has ended up. */
static void draw_cap(const aud_viz *v, size_t band, float cx, float cy, float slot,
                     float value)
{
  float halo = slot * VIZ_CAP_HALO;

  if (halo < VIZ_HALO_MIN_PX)
  {
    halo = VIZ_HALO_MIN_PX;
  }

  /* louder bands bloom wider, which is what gives the display its dynamics */
  draw_glow(v, cx, cy, halo * (0.55f + 0.45f * value),
            with_alpha(v->palette[band], 0.30f + 0.40f * value));
  draw_glow(v, cx, cy, slot * VIZ_CAP_CORE * (0.70f + 0.30f * value),
            with_alpha(v->palette[band], 0.65f + 0.35f * value));
}

/* -- bars ------------------------------------------------------------------ */

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
  {
    stem_w = 1.0f;
  }

  /* stems: a soft additive wash first, then the solid hairline over it */
  BeginBlendMode(BLEND_ADDITIVE);
  for (size_t b = 0; b < v->bands; b++)
  {
    float h = values[b] * area.height;
    float cx = area.x + slot * ((float)b + 0.5f);
    Rectangle r;

    if (h < VIZ_MIN_HEIGHT)
    {
      h = VIZ_MIN_HEIGHT;
    }

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
    {
      h = VIZ_MIN_HEIGHT;
    }

    DrawRectangleRec((Rectangle){cx - stem_w / 2.0f, baseline - h, stem_w, h},
                     v->palette[b]);
  }

  BeginBlendMode(BLEND_ADDITIVE);
  for (size_t b = 0; b < v->bands; b++)
  {
    float h = values[b] * area.height;

    if (h < VIZ_MIN_HEIGHT)
    {
      h = VIZ_MIN_HEIGHT;
    }

    draw_cap(v, b, area.x + slot * ((float)b + 0.5f), baseline - h, slot, values[b]);
  }
  EndBlendMode();
}

/* -- mirror ---------------------------------------------------------------- */

/* The same bars opened around the centre line, each half at half the height. */
static void draw_mirror(const aud_viz *v, Rectangle area, const float *values)
{
  float slot = area.width / (float)v->bands;
  float stem_w = slot * VIZ_STEM_WIDTH;
  float centre = area.y + area.height / 2.0f;
  float reach = area.height / 2.0f;

  if (stem_w < 1.0f)
  {
    stem_w = 1.0f;
  }

  for (size_t b = 0; b < v->bands; b++)
  {
    float h = values[b] * reach;
    float cx = area.x + slot * ((float)b + 0.5f);

    if (h < VIZ_MIN_HEIGHT)
    {
      h = VIZ_MIN_HEIGHT;
    }

    DrawRectangleRec((Rectangle){cx - stem_w / 2.0f, centre - h, stem_w, h * 2.0f},
                     v->palette[b]);
  }

  BeginBlendMode(BLEND_ADDITIVE);
  for (size_t b = 0; b < v->bands; b++)
  {
    float h = values[b] * reach;
    float cx = area.x + slot * ((float)b + 0.5f);

    if (h < VIZ_MIN_HEIGHT)
    {
      h = VIZ_MIN_HEIGHT;
    }

    draw_cap(v, b, cx, centre - h, slot, values[b]);
    draw_cap(v, b, cx, centre + h, slot, values[b]);
  }
  EndBlendMode();
}

/* -- radial ---------------------------------------------------------------- */

/*
 * The spectrum wrapped into a ring, bass at the top and running clockwise.
 * Reads less precisely than the bars but shows the shape of a chord at a
 * glance, and it is the one that looks like something rather than a readout.
 */
static void draw_radial(const aud_viz *v, Rectangle area, const float *values)
{
  float cx = area.x + area.width / 2.0f;
  float cy = area.y + area.height / 2.0f;
  float span = area.width < area.height ? area.width : area.height;
  float inner = span * 0.17f;
  float reach = span * 0.30f;
  float thickness = (2.0f * 3.14159265f * inner) / (float)v->bands * 0.55f;
  float slot = span * 0.02f; /* what a cap's glow is sized against here */

  if (thickness < 1.5f)
  {
    thickness = 1.5f;
  }

  /* the ring the spokes stand on, so the centre is not an empty hole */
  DrawCircleLines((int)cx, (int)cy, inner - 3.0f, with_alpha(v->palette[0], 0.18f));

  for (size_t b = 0; b < v->bands; b++)
  {
    float angle = -1.57079633f + 6.28318531f * ((float)b / (float)v->bands);
    float ca = cosf(angle);
    float sa = sinf(angle);
    float len = values[b] * reach;
    Vector2 from;
    Vector2 to;

    if (len < VIZ_MIN_HEIGHT)
    {
      len = VIZ_MIN_HEIGHT;
    }

    from.x = cx + ca * inner;
    from.y = cy + sa * inner;
    to.x = cx + ca * (inner + len);
    to.y = cy + sa * (inner + len);

    DrawLineEx(from, to, thickness, v->palette[b]);
  }

  BeginBlendMode(BLEND_ADDITIVE);
  for (size_t b = 0; b < v->bands; b++)
  {
    float angle = -1.57079633f + 6.28318531f * ((float)b / (float)v->bands);
    float len = values[b] * reach;

    if (len < VIZ_MIN_HEIGHT)
    {
      len = VIZ_MIN_HEIGHT;
    }

    draw_cap(v, b, cx + cosf(angle) * (inner + len), cy + sinf(angle) * (inner + len),
             slot, values[b]);
  }
  EndBlendMode();
}

/* -- scope ----------------------------------------------------------------- */

/*
 * Find a rising zero crossing to start the sweep from. Without one the trace
 * slides sideways by however many samples arrived since the last frame, and a
 * steady note looks like it is scrolling rather than standing still.
 *
 * Returns an age in samples, counting back from the newest.
 */
static size_t scope_trigger(const aud_viz *v)
{
  size_t newest = VIZ_SCOPE_SPAN;
  size_t oldest = VIZ_WAVE_SAMPLES - 2;

  for (size_t age = newest; age < oldest; age++)
  {
    /* ages run backwards in time, so age+1 is the earlier sample */
    if (wave_at(v, age + 1) <= 0.0f && wave_at(v, age) > 0.0f)
    {
      return age;
    }
  }

  return newest; /* silence, or no crossing in range: draw the latest anyway */
}

static void draw_scope(const aud_viz *v, Rectangle area)
{
  float centre = area.y + area.height / 2.0f;
  float reach = area.height * 0.45f;
  size_t start = scope_trigger(v);
  Vector2 points[VIZ_SCOPE_POINTS];

  for (size_t i = 0; i < VIZ_SCOPE_POINTS; i++)
  {
    float t = (float)i / (float)(VIZ_SCOPE_POINTS - 1);
    size_t age = start - (size_t)(t * (float)(VIZ_SCOPE_SPAN - 1));
    float sample = wave_at(v, age);

    if (sample > 1.0f)
    {
      sample = 1.0f;
    }
    if (sample < -1.0f)
    {
      sample = -1.0f;
    }

    points[i].x = area.x + area.width * t;
    points[i].y = centre - sample * reach;
  }

  DrawLineEx((Vector2){area.x, centre}, (Vector2){area.x + area.width, centre}, 1.0f,
             with_alpha(v->palette[v->bands / 2], 0.15f));

  /*
   * Two passes: a thick translucent trace that blooms, then a thin opaque one
   * for the line itself. The colour walks the palette across the sweep, which
   * ties the scope to the other styles.
   */
  BeginBlendMode(BLEND_ADDITIVE);
  for (size_t i = 1; i < VIZ_SCOPE_POINTS; i++)
  {
    size_t band = i * v->bands / VIZ_SCOPE_POINTS;

    DrawLineEx(points[i - 1], points[i], 9.0f, with_alpha(v->palette[band], 0.10f));
    DrawLineEx(points[i - 1], points[i], 4.0f, with_alpha(v->palette[band], 0.18f));
  }
  EndBlendMode();

  for (size_t i = 1; i < VIZ_SCOPE_POINTS; i++)
  {
    size_t band = i * v->bands / VIZ_SCOPE_POINTS;

    DrawLineEx(points[i - 1], points[i], 2.0f, v->palette[band]);
  }
}

/* -- waterfall ------------------------------------------------------------- */

/*
 * The spectrogram ring, unwrapped into two quads: the columns after the write
 * head are the oldest and go on the left, the rest follow. Time runs left to
 * right, frequency bottom to top.
 */
static void draw_waterfall(const aud_viz *v, Rectangle area)
{
  int head = v->fall_head;
  int older = VIZ_FALL_COLUMNS - 1 - head; /* columns to the right of the head */
  float split = area.width * (float)older / (float)VIZ_FALL_COLUMNS;
  Vector2 origin = {0.0f, 0.0f};
  float bands = (float)v->bands;

  if (older > 0)
  {
    Rectangle src = {(float)(head + 1), 0.0f, (float)older, bands};
    Rectangle dst = {area.x, area.y, split, area.height};

    DrawTexturePro(v->fall, src, dst, origin, 0.0f, WHITE);
  }

  {
    Rectangle src = {0.0f, 0.0f, (float)(head + 1), bands};
    Rectangle dst = {area.x + split, area.y, area.width - split, area.height};

    DrawTexturePro(v->fall, src, dst, origin, 0.0f, WHITE);
  }

  /* a hairline at the write head, so "now" is obvious */
  DrawRectangleRec((Rectangle){area.x + area.width - 1.0f, area.y, 1.0f, area.height},
                   with_alpha(WHITE, 0.25f));
}

/* -- tuner ----------------------------------------------------------------- */

/* Text centred on `cx`, which is how every line of the tuner is placed. */
static void draw_centred(const char *text, float cx, float y, int size, Color tint)
{
  DrawText(text, (int)(cx - (float)MeasureText(text, size) / 2.0f), (int)y, size, tint);
}

static float clampf(float v, float lo, float hi)
{
  if (v < lo)
  {
    return lo;
  }
  if (v > hi)
  {
    return hi;
  }
  return v;
}

/*
 * The scale the needle rides on: a track, a tick every twelve and a half cents
 * and a taller one in the middle for the note itself. Drawn before the needle
 * so it reads as something the needle sits on rather than through.
 */
static void draw_tuner_scale(Rectangle track, Color tint)
{
  float cx = track.x + track.width / 2.0f;
  float cy = track.y + track.height / 2.0f;

  DrawRectangleRec((Rectangle){track.x, cy - 1.0f, track.width, 2.0f},
                   with_alpha(tint, 0.22f));

  for (int step = -4; step <= 4; step++)
  {
    float t = (float)step / 4.0f;
    int major = (step % 2) == 0;
    float h = major ? track.height * 0.55f : track.height * 0.28f;
    float w = major ? 2.0f : 1.0f;
    float x = cx + t * track.width / 2.0f;

    /* the centre tick is the target, so it stays lit even when nothing else is */
    DrawRectangleRec((Rectangle){x - w / 2.0f, cy - h / 2.0f, w, h},
                     with_alpha(tint, step == 0 ? 0.85f : 0.30f));
  }
}

/*
 * A reading rather than a picture of the sound. Green when the note is close
 * enough that the string will drift further than this on its own, amber when it
 * is not, grey when nothing is being played - the colour is meant to be
 * readable from across the room, with the numbers there for when it is not.
 */
static void draw_tuner(const aud_viz *v, Rectangle area)
{
  const aud_tuner_reading *r = &v->reading;
  char label[AUD_TUNER_LABEL_MAX];
  char line[96];
  float cx = area.x + area.width / 2.0f;
  float cy = area.y + area.height / 2.0f;
  int in_tune = r->voiced && fabs(r->cents) <= AUD_TUNER_IN_TUNE_CENTS;
  Color tint = !r->voiced ? AUD_UI_MUTED : (in_tune ? AUD_UI_OK : AUD_UI_WARN);
  int note_size = (int)clampf(area.height * 0.30f, 32.0f, 190.0f);
  int read_size = (int)clampf(area.height * 0.075f, 16.0f, 34.0f);
  int small_size = (int)clampf(area.height * 0.055f, 12.0f, 22.0f);
  Rectangle track;

  aud_tuner_note_label(r, label, sizeof(label));
  draw_centred(label, cx, cy - (float)note_size * 0.95f, note_size, tint);

  track.width = clampf(area.width * 0.72f, 120.0f, 900.0f);
  track.height = clampf(area.height * 0.16f, 20.0f, 70.0f);
  track.x = cx - track.width / 2.0f;
  track.y = cy + area.height * 0.08f;
  draw_tuner_scale(track, tint);

  if (r->voiced)
  {
    float offset = clampf((float)(r->cents / VIZ_TUNER_RANGE_CENTS), -1.0f, 1.0f);
    float nx = track.x + track.width / 2.0f + offset * track.width / 2.0f;
    float ny = track.y + track.height / 2.0f;

    if (v->glow_ready)
    {
      BeginBlendMode(BLEND_ADDITIVE);
      draw_glow(v, nx, ny, track.height * 3.2f,
                with_alpha(tint, in_tune ? 0.55f : 0.35f));
      EndBlendMode();
    }
    DrawRectangleRec((Rectangle){nx - 2.0f, track.y, 4.0f, track.height}, tint);

    if (in_tune)
    {
      snprintf(line, sizeof(line), "in tune");
    }
    else
    {
      snprintf(line, sizeof(line), "%+.0f cents", r->cents);
    }
    draw_centred(line, cx, track.y + track.height + (float)read_size * 0.7f, read_size,
                 tint);

    /*
     * Two decimals on what is being played and one on what it should be. At the
     * bottom of a bass a whole cent is under a hundredth of a hertz, so a
     * single decimal would show the same number either side of "in tune".
     */
    snprintf(line, sizeof(line), "%.2f Hz      target %.1f Hz", r->frequency,
             r->target_hz);
  }
  else
  {
    draw_centred("play a note", cx, track.y + track.height + (float)read_size * 0.7f,
                 read_size, AUD_UI_MUTED);
    snprintf(line, sizeof(line), "listening on the input");
  }

  draw_centred(line, cx, track.y + track.height + (float)read_size * 2.1f, small_size,
               AUD_UI_MUTED);

  /* the reference pitch, out of the way, because it changes what all of this means */
  snprintf(line, sizeof(line), "A = %.0f Hz", v->a4_hz);
  DrawText(line, (int)(area.x + (float)small_size * 0.5f),
           (int)(area.y + area.height - (float)small_size * 1.6f), small_size,
           AUD_UI_MUTED);
}

/* -- dispatch -------------------------------------------------------------- */

void aud_viz_draw(const aud_viz *v, Rectangle area)
{
  if (v == NULL || area.width <= 0.0f || area.height <= 0.0f)
  {
    return;
  }

  /*
   * The tuner is text and a needle, so it needs neither the glow sprite nor a
   * spectrum to have been analysed yet - and it has something worth saying
   * before the first analysis, which the others do not.
   */
  if (v->mode == AUD_VIZ_MODE_TUNER)
  {
    draw_tuner(v, area);
    return;
  }

  if (!v->glow_ready || v->values == NULL)
  {
    return;
  }

  switch (v->mode)
  {
  case AUD_VIZ_MODE_MIRROR:
    draw_mirror(v, area, v->values);
    return;
  case AUD_VIZ_MODE_RADIAL:
    draw_radial(v, area, v->values);
    return;
  case AUD_VIZ_MODE_SCOPE:
    draw_scope(v, area);
    return;
  case AUD_VIZ_MODE_WATERFALL:
    if (v->fall_ready)
    {
      draw_waterfall(v, area);
    }
    return;
  case AUD_VIZ_MODE_TUNER: /* handled above, before the spectrum is needed */
  case AUD_VIZ_MODE_BARS:
  case AUD_VIZ_MODE_COUNT:
  default:
    draw_bars(v, area, v->values);
    return;
  }
}
