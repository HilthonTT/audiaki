/* SPDX-License-Identifier: MIT */
#include "gui/ui.h"

#include "gui/fonts.h"

#include "util/log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The corner every control shares, in pixels. See aud_ui_panel(). */
#define UI_CORNER 8.0f
#define UI_CORNER_SMALL 5.0f
#define UI_FONT 20

/*
 * A label is drawn at the size the caller asked for if it fits its control and
 * a size down otherwise, until it either fits or reaches this. Below it the end
 * of the label is dropped instead. The window is resizable and the toolbars
 * divide whatever width it has, so a control is often narrower than the text in
 * it wants to be - and text drawn at its natural size in a button that shrank
 * under it lands on top of the button next door.
 */
#define UI_FONT_MIN 12

/*
 * The room a label insists on between itself and the edges of its control. It
 * is deliberately less than a toolbar leaves when it measures a button from its
 * label: the two are asking different questions - how much room looks right,
 * and how little a label can be given before it has to start losing letters -
 * and a widget that answered the first would cut short labels that fit.
 */
#define UI_LABEL_PAD 4.0f

/* One wheel notch, as a fraction of the slider's range. */
#define UI_SLIDER_WHEEL 0.02f

/* How long the pointer has to rest on a control before it explains itself. */
#define UI_TIP_DELAY 0.45f
#define UI_TIP_FONT 15
#define UI_TIP_PAD 10.0f

/*
 * The faces are baked once at this size and drawn scaled, rather than baked
 * again for every size the window asks for. Big enough that the 20 pixel
 * heading is a reduction and not a magnification, and bilinear filtering does
 * the rest - which is why one atlas can serve everything from an 11 pixel unit
 * to a 26 pixel title without any of them looking soft or blocky.
 */
#define UI_FONT_BAKE 48

/*
 * A typeface is drawn a little larger than the size asked for. Every one of
 * those sizes was chosen against raylib's own font, whose glyphs fill their box
 * to the top and bottom; a real face spends part of that box on the room above
 * a capital and below a baseline, and set at the same number it comes out
 * visibly smaller than the layout was drawn for.
 */
#define UI_FONT_SCALE 1.12f

/*
 * And sits a little higher in it. Same cause: the descender room at the foot of
 * the box is empty for most words, so text centred on the box reads as centred
 * slightly low. This is that error, as a fraction of the size.
 */
#define UI_FONT_LIFT 0.055f

static Color mix(Color a, Color b, float t)
{
  Color out;

  if (t < 0.0f)
  {
    t = 0.0f;
  }
  if (t > 1.0f)
  {
    t = 1.0f;
  }

  out.r = (unsigned char)((float)a.r + ((float)b.r - (float)a.r) * t);
  out.g = (unsigned char)((float)a.g + ((float)b.g - (float)a.g) * t);
  out.b = (unsigned char)((float)a.b + ((float)b.b - (float)a.b) * t);
  out.a = (unsigned char)((float)a.a + ((float)b.a - (float)a.a) * t);
  return out;
}

static Color fade_to(Color c, float alpha)
{
  c.a = (unsigned char)(alpha * 255.0f + 0.5f);
  return c;
}

static float luminance(Color c)
{
  return (0.299f * (float)c.r + 0.587f * (float)c.g + 0.114f * (float)c.b) / 255.0f;
}

/*
 * What a lit control is filled with, given the colour it is lit in.
 *
 * A pale tint is taken down towards black first, by as much as it is pale. The
 * palette has an amber and a green in it that are the right colours to mean
 * what they mean and the wrong ones to write white on, and a button whose
 * label cannot be read is worse than one that is the wrong shade of amber. The
 * border is still drawn in the tint itself, so the colour is not lost.
 */
static Color lit_fill(Color tint, float toward)
{
  static const Color ink = {8, 9, 12, 255};
  float lum = luminance(tint);
  Color base = tint;

  if (lum > 0.45f)
  {
    base = mix(tint, ink, (lum - 0.45f) * 1.1f);
  }
  return mix(AUD_UI_SURFACE, base, toward);
}

static int hovering(Rectangle bounds)
{
  return CheckCollisionPointRec(GetMousePosition(), bounds);
}

/*
 * Immediate mode has no widget identity, so a control that has to be
 * remembered between frames is remembered by where it is. Nothing here moves
 * while it is being held, and two controls never share a rectangle.
 */
static int same_rect(Rectangle a, Rectangle b)
{
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/* -- shapes ----------------------------------------------------------------- */

/*
 * raylib takes a corner as a fraction of the shorter side, which means a
 * rectangle's corner changes when the rectangle is resized. Every corner in
 * this window is a fixed number of pixels instead, so a tall panel and a short
 * button are cut to the same curve.
 */
static float roundness(Rectangle bounds, float radius)
{
  float shorter = bounds.width < bounds.height ? bounds.width : bounds.height;

  if (!(shorter > 0.0f))
  {
    return 0.0f;
  }
  if (radius > shorter / 2.0f)
  {
    radius = shorter / 2.0f;
  }
  return 2.0f * radius / shorter;
}

void aud_ui_panel(Rectangle bounds, float radius, Color fill, Color edge)
{
  float r = roundness(bounds, radius);

  if (fill.a > 0)
  {
    DrawRectangleRounded(bounds, r, 10, fill);
  }
  if (edge.a > 0)
  {
    DrawRectangleRoundedLines(bounds, r, 10, edge);
  }
}

void aud_ui_shadow(Rectangle bounds, float radius, float spread)
{
  /*
   * Four rings rather than a blurred texture: it is a handful of rounded
   * rectangles a frame and it is only ever under a menu or a dialog, where
   * there is one of them on the screen at a time.
   */
  for (int i = 4; i >= 1; i--)
  {
    float grow = spread * (float)i / 4.0f;
    Rectangle out = {bounds.x - grow, bounds.y - grow + grow * 0.35f,
                     bounds.width + 2.0f * grow, bounds.height + 2.0f * grow};

    DrawRectangleRounded(out, roundness(out, radius + grow), 10,
                         fade_to(BLACK, 0.16f - 0.03f * (float)i));
  }
}

void aud_ui_rule(float x, float y, float height, Color color)
{
  DrawRectangleRec((Rectangle){x, y, 1.0f, height}, color);
}

/*
 * The pale line along the top inside edge of a raised control. It is one pixel
 * and almost invisible on its own, and it is what makes a button look lit from
 * above rather than cut out of the panel behind it.
 */
static void top_highlight(Rectangle bounds, float radius, float strength)
{
  float inset = radius * 0.6f;

  if (bounds.width <= 2.0f * inset || strength <= 0.0f)
  {
    return;
  }

  DrawRectangleRec(
      (Rectangle){bounds.x + inset, bounds.y + 1.0f, bounds.width - 2.0f * inset, 1.0f},
      fade_to(WHITE, 0.055f * strength));
}

/* -- lettering -------------------------------------------------------------- */

static Font ui_face[AUD_UI_FACE_COUNT];
static int ui_faces_loaded;

static void load_faces(void)
{
  static const aud_fonts_kind wanted[AUD_UI_FACE_COUNT] = {
      AUD_FONTS_SANS, AUD_FONTS_STRONG, AUD_FONTS_MONO};

  if (ui_faces_loaded)
  {
    return;
  }
  ui_faces_loaded = 1;

  /*
   * Baking an atlas warns about every glyph that came out taller than the size
   * asked for, which for a face with a tall bar or a long descender is a line
   * on the terminal for a thing nobody can see and nobody can fix. raylib will
   * not say what its level was, so this puts back the one plug.c set.
   */
  SetTraceLogLevel(LOG_ERROR);

  for (int i = 0; i < AUD_UI_FACE_COUNT; i++)
  {
    char path[512];

    ui_face[i] = GetFontDefault();

    if (!aud_fonts_find(wanted[i], path, sizeof(path)))
    {
      continue;
    }

    {
      Font loaded = LoadFontEx(path, UI_FONT_BAKE, NULL, 0);

      /*
       * A file that turns out not to be a font raylib can read comes back as
       * the default one, and unloading that would take the fallback away from
       * every face at once.
       */
      if (loaded.texture.id == 0 || loaded.texture.id == GetFontDefault().texture.id)
      {
        continue;
      }

      SetTextureFilter(loaded.texture, TEXTURE_FILTER_BILINEAR);
      ui_face[i] = loaded;
    }
  }

  SetTraceLogLevel(aud_log_get_level() == AUD_LOG_VERBOSE ? LOG_INFO : LOG_WARNING);

  /* One face found and not another is normal - a machine can have DejaVu Sans
   * and no mono at all - and either stands in for the other before the bitmap
   * does. */
  for (int i = 0; i < AUD_UI_FACE_COUNT; i++)
  {
    if (ui_face[i].texture.id != GetFontDefault().texture.id)
    {
      continue;
    }
    for (int j = 0; j < AUD_UI_FACE_COUNT; j++)
    {
      if (ui_face[j].texture.id != GetFontDefault().texture.id)
      {
        ui_face[i] = ui_face[j];
        break;
      }
    }
  }
}

void aud_ui_fonts_release(void)
{
  if (!ui_faces_loaded)
  {
    return;
  }

  for (int i = 0; i < AUD_UI_FACE_COUNT; i++)
  {
    /* the fallbacks above leave the same face in two slots, and raylib's own
     * is not ours to free */
    if (ui_face[i].texture.id == GetFontDefault().texture.id)
    {
      continue;
    }
    for (int j = 0; j < i; j++)
    {
      if (ui_face[j].texture.id == ui_face[i].texture.id)
      {
        ui_face[i] = GetFontDefault();
        break;
      }
    }
    if (ui_face[i].texture.id != GetFontDefault().texture.id)
    {
      UnloadFont(ui_face[i]);
    }
  }

  ui_faces_loaded = 0;
}

static Font face_of(aud_ui_face face)
{
  load_faces();

  if (face < 0 || face >= AUD_UI_FACE_COUNT)
  {
    face = AUD_UI_SANS;
  }
  return ui_face[face];
}

/* The pixel size a face is actually set at for a requested `size`. */
static float pixel_size(Font font, int size)
{
  if (font.texture.id == GetFontDefault().texture.id)
  {
    return (float)size; /* the bitmap was measured at its own size */
  }
  return (float)size * UI_FONT_SCALE;
}

static float face_spacing(Font font, float px)
{
  if (font.texture.id == GetFontDefault().texture.id)
  {
    return px / 10.0f; /* what raylib's own DrawText() uses */
  }
  return px * 0.01f;
}

float aud_ui_measure(aud_ui_face face, const char *text, int size)
{
  Font font = face_of(face);
  float px = pixel_size(font, size);

  if (text == NULL || *text == '\0')
  {
    return 0.0f;
  }
  return MeasureTextEx(font, text, px, face_spacing(font, px)).x;
}

void aud_ui_write(aud_ui_face face, float x, float y, int size, Color color,
                  const char *text)
{
  Font font = face_of(face);
  float px = pixel_size(font, size);
  Vector2 at;

  if (text == NULL || *text == '\0')
  {
    return;
  }

  at.x = x;
  at.y = y - px * UI_FONT_LIFT;
  DrawTextEx(font, text, at, px, face_spacing(font, px), color);
}

void aud_ui_write_right(aud_ui_face face, float right, float y, int size, Color color,
                        const char *text)
{
  aud_ui_write(face, right - aud_ui_measure(face, text, size), y, size, color, text);
}

void aud_ui_write_centred(aud_ui_face face, Rectangle bounds, int size, Color color,
                          const char *text)
{
  float width = aud_ui_measure(face, text, size);
  float x = bounds.x + (bounds.width - width) / 2.0f;
  float y = bounds.y + (bounds.height - (float)size) / 2.0f;

  aud_ui_write(face, x, y, size, color, text);
}

void aud_ui_text(float x, float y, int size, Color color, const char *text)
{
  aud_ui_write(AUD_UI_SANS, x, y, size, color, text);
}

void aud_ui_text_right(float right, float y, int size, Color color, const char *text)
{
  aud_ui_write_right(AUD_UI_SANS, right, y, size, color, text);
}

void aud_ui_text_centred(Rectangle bounds, int size, Color color, const char *text)
{
  aud_ui_write_centred(AUD_UI_SANS, bounds, size, color, text);
}

/*
 * The largest size no bigger than `size` at which `text` fits `room` pixels,
 * or UI_FONT_MIN if none of them do.
 */
static int fitted_size(aud_ui_face face, const char *text, int size, float room)
{
  while (size > UI_FONT_MIN && aud_ui_measure(face, text, size) > room)
  {
    size--;
  }
  return size;
}

/*
 * `text` with its end replaced by an ellipsis until what is left fits `room`,
 * returned in a buffer that lasts until the next call - which is long enough,
 * because every caller hands it straight to the drawing.
 */
static const char *shortened(aud_ui_face face, const char *text, int size, float room)
{
  static char buf[192];
  size_t len;

  if (aud_ui_measure(face, text, size) <= room)
  {
    return text;
  }

  snprintf(buf, sizeof(buf), "%s", text);
  len = strlen(buf);

  /* trim a character at a time; the strings are short and this runs per frame
   * only for the one label that overflows */
  while (len > 1)
  {
    buf[--len] = '\0';
    if (len + 4 < sizeof(buf) &&
        aud_ui_measure(face, TextFormat("%s...", buf), size) <= room)
    {
      break;
    }
  }

  snprintf(buf + len, sizeof(buf) - len, "...");
  return buf;
}

/*
 * The size a control letters its label at unless it is told otherwise. A
 * toolbar that has had to pack itself tighter than this says so once for the
 * whole row: passing the size to each of the dozen calls that draw a button
 * would be a dozen chances to letter one of them differently.
 */
static int ui_label_size = UI_FONT;

void aud_ui_label_size(int size)
{
  ui_label_size = size > 0 ? size : UI_FONT;
}

/*
 * A label inside a control: as large as it was asked for while that fits, then
 * smaller, and finally cut short. Never taller than the control either, which
 * is what keeps the drawer's tabs from writing over their own edges.
 */
static void label_centred(Rectangle bounds, int size, Color color, const char *label)
{
  float room = bounds.width - 2.0f * UI_LABEL_PAD;
  int font;

  if (label == NULL || *label == '\0')
  {
    return;
  }

  if (room < 12.0f)
  {
    room = bounds.width - 2.0f;
  }
  if ((float)size > bounds.height - 6.0f)
  {
    size = (int)bounds.height - 6;
  }
  if (size < UI_FONT_MIN)
  {
    size = UI_FONT_MIN;
  }

  font = fitted_size(AUD_UI_STRONG, label, size, room);
  aud_ui_write_centred(AUD_UI_STRONG, bounds, font, color,
                       shortened(AUD_UI_STRONG, label, font, room));
}

/* -- glyphs ----------------------------------------------------------------- */

float aud_ui_icon_width(int font)
{
  return 0.62f * (float)font + 6.0f;
}

/* The transport glyphs, drawn to fit a box `size` across centred on (cx, cy). */
static void draw_icon(aud_ui_icon icon, float cx, float cy, float size, Color color)
{
  float r = size / 2.0f;

  switch (icon)
  {
  case AUD_UI_ICON_PLAY:
    /* counter-clockwise, or raylib culls it */
    DrawTriangle((Vector2){cx - r * 0.75f, cy - r}, (Vector2){cx - r * 0.75f, cy + r},
                 (Vector2){cx + r * 0.9f, cy}, color);
    break;

  case AUD_UI_ICON_RECORD:
    DrawCircleV((Vector2){cx, cy}, r * 0.92f, color);
    break;

  case AUD_UI_ICON_PAUSE:
  {
    float bar = r * 0.42f;

    DrawRectangleRounded((Rectangle){cx - r * 0.85f, cy - r, bar, size}, 0.4f, 4, color);
    DrawRectangleRounded((Rectangle){cx + r * 0.43f, cy - r, bar, size}, 0.4f, 4, color);
    break;
  }

  case AUD_UI_ICON_STOP:
    DrawRectangleRounded((Rectangle){cx - r * 0.85f, cy - r * 0.85f, r * 1.7f, r * 1.7f},
                         0.22f, 4, color);
    break;

  case AUD_UI_ICON_LOOP:
  {
    /* a ring with a bite out of the top right and an arrowhead filling it: a
     * circle that goes round again, which is the whole of what the button does */
    float ring = r * 0.92f;

    DrawRing((Vector2){cx, cy}, ring - 1.6f, ring, 20.0f, 320.0f, 28, color);
    DrawTriangle((Vector2){cx + ring * 0.10f, cy - ring * 0.62f},
                 (Vector2){cx + ring * 1.00f, cy - ring * 0.62f},
                 (Vector2){cx + ring * 0.62f, cy - ring * 1.25f}, color);
    break;
  }

  case AUD_UI_ICON_OPEN:
  case AUD_UI_ICON_SHUT:
  case AUD_UI_ICON_FOLD:
  {
    /* one chevron, turned to point at whatever the click would do next */
    float dx = r * 0.52f;
    float dy = r * 0.34f;
    Vector2 a;
    Vector2 tip;
    Vector2 b;

    if (icon == AUD_UI_ICON_SHUT)
    {
      a = (Vector2){cx - dy, cy - dx};
      tip = (Vector2){cx + dy * 1.2f, cy};
      b = (Vector2){cx - dy, cy + dx};
    }
    else if (icon == AUD_UI_ICON_FOLD)
    {
      a = (Vector2){cx - dx, cy + dy * 0.6f};
      tip = (Vector2){cx, cy - dy * 1.0f};
      b = (Vector2){cx + dx, cy + dy * 0.6f};
    }
    else
    {
      a = (Vector2){cx - dx, cy - dy * 0.6f};
      tip = (Vector2){cx, cy + dy * 1.0f};
      b = (Vector2){cx + dx, cy - dy * 0.6f};
    }

    DrawLineEx(a, tip, 1.8f, color);
    DrawLineEx(b, tip, 1.8f, color);
    break;
  }

  case AUD_UI_ICON_CLOSE:
  {
    float d = r * 0.62f;

    DrawLineEx((Vector2){cx - d, cy - d}, (Vector2){cx + d, cy + d}, 1.8f, color);
    DrawLineEx((Vector2){cx + d, cy - d}, (Vector2){cx - d, cy + d}, 1.8f, color);
    break;
  }

  case AUD_UI_ICON_NONE:
  default:
    break;
  }
}

/* -- buttons ---------------------------------------------------------------- */

/* Shared body of button and toggle: returns non-zero when clicked. */
static int clickable(Rectangle bounds, aud_ui_icon icon, const char *label, Color tint,
                     int enabled, int lit)
{
  int hover = enabled && hovering(bounds);
  int down = hover && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
  Rectangle inner = bounds;
  Color fill;
  Color edge;
  Color text;
  float highlight = 1.0f;

  if (!enabled)
  {
    fill = mix(AUD_UI_PANEL, AUD_UI_BG, 0.45f);
    edge = fade_to(AUD_UI_EDGE_SOFT, 0.7f);
    text = fade_to(AUD_UI_MUTED, 0.40f);
    highlight = 0.0f;
  }
  else if (lit)
  {
    /*
     * A lit control is the tint itself rather than the panel with a coloured
     * border: at a glance across two rows of buttons the one that is on should
     * be the one that is a different colour, not the one with a thinner line
     * round it.
     */
    fill = lit_fill(tint, down ? 0.92f : (hover ? 0.88f : 0.80f));
    edge = fade_to(tint, 0.9f);
    text = WHITE;

    /* the glow that says this one is live, and only while the pointer is off
     * it - under the pointer the fill has already brightened */
    if (!hover)
    {
      Rectangle halo = {bounds.x - 1.5f, bounds.y - 1.5f, bounds.width + 3.0f,
                        bounds.height + 3.0f};

      aud_ui_panel(halo, UI_CORNER + 1.5f, BLANK, fade_to(tint, 0.22f));
    }
  }
  else
  {
    fill = down ? mix(AUD_UI_SURFACE, AUD_UI_BG, 0.35f)
                : (hover ? mix(AUD_UI_SURFACE_HI, tint, 0.10f) : AUD_UI_SURFACE);
    edge = hover ? mix(AUD_UI_EDGE, tint, 0.55f) : AUD_UI_EDGE;
    text = hover ? WHITE : AUD_UI_TEXT;
    highlight = down ? 0.0f : 1.0f;
  }

  /* pressed sinks by a pixel, which is the cheapest way to make a flat control
   * feel like it answered */
  if (down)
  {
    inner.y += 1.0f;
  }

  aud_ui_panel(inner, UI_CORNER, fill, edge);
  top_highlight(inner, UI_CORNER, highlight);

  if (icon == AUD_UI_ICON_NONE)
  {
    label_centred(inner, ui_label_size, text, label);
  }
  else if (label == NULL || *label == '\0')
  {
    draw_icon(icon, inner.x + inner.width / 2.0f, inner.y + inner.height / 2.0f,
              0.62f * (float)ui_label_size, text);
  }
  else
  {
    /*
     * The glyph and the words are centred together rather than the glyph being
     * pinned left and the words centred in what is left: on a button wider than
     * its label - which is every button on a row measured from its longest one -
     * that leaves the two of them adrift from each other at opposite ends.
     */
    float room = aud_ui_icon_width(ui_label_size);
    float words = aud_ui_measure(AUD_UI_STRONG, label, ui_label_size);
    float most = inner.width - room - 2.0f * UI_LABEL_PAD;
    float start;
    int size = ui_label_size;

    if (words > most)
    {
      size = fitted_size(AUD_UI_STRONG, label, size, most);
      words = aud_ui_measure(AUD_UI_STRONG, label, size);
    }

    start = inner.x + (inner.width - room - words) / 2.0f;
    if (start < inner.x + UI_LABEL_PAD)
    {
      start = inner.x + UI_LABEL_PAD;
    }

    draw_icon(icon, start + room / 2.0f - 3.0f, inner.y + inner.height / 2.0f,
              0.62f * (float)size, text);
    aud_ui_write(AUD_UI_STRONG, start + room,
                 inner.y + (inner.height - (float)size) / 2.0f, size, text,
                 shortened(AUD_UI_STRONG, label, size, most));
  }

  if (enabled && hover)
  {
    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
  }

  return hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

int aud_ui_button(Rectangle bounds, const char *label, Color tint, int enabled)
{
  return clickable(bounds, AUD_UI_ICON_NONE, label, tint, enabled, 0);
}

int aud_ui_button_icon(Rectangle bounds, aud_ui_icon icon, const char *label, Color tint,
                       int enabled)
{
  return clickable(bounds, icon, label, tint, enabled, 0);
}

int aud_ui_toggle(Rectangle bounds, const char *label, int on, Color tint, int enabled)
{
  return clickable(bounds, AUD_UI_ICON_NONE, label, tint, enabled, on);
}

int aud_ui_toggle_icon(Rectangle bounds, aud_ui_icon icon, const char *label, int on,
                       Color tint, int enabled)
{
  return clickable(bounds, icon, label, tint, enabled, on);
}

int aud_ui_ghost(Rectangle bounds, const char *label, Color tint, int enabled)
{
  int hover = enabled && hovering(bounds);
  int down = hover && IsMouseButtonDown(MOUSE_BUTTON_LEFT);

  if (down)
  {
    aud_ui_panel(bounds, UI_CORNER_SMALL, fade_to(tint, 0.20f), BLANK);
  }
  else if (hover)
  {
    aud_ui_panel(bounds, UI_CORNER_SMALL, fade_to(WHITE, 0.06f), BLANK);
  }

  label_centred(bounds, ui_label_size,
                enabled ? (hover ? WHITE : AUD_UI_MUTED) : fade_to(AUD_UI_MUTED, 0.40f),
                label);

  if (hover)
  {
    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
  }

  return hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

/* -- slider ----------------------------------------------------------------- */

/* The slider the pointer grabbed, so the drag survives leaving its bounds. */
static Rectangle ui_slider_held;
static int ui_slider_dragging;

int aud_ui_slider(Rectangle bounds, float *value, float min, float max, Color tint,
                  int enabled)
{
  float span = max - min;
  float knob_r = bounds.height / 2.0f;
  float track_y = bounds.y + bounds.height / 2.0f;
  float usable = bounds.width - bounds.height; /* the knob needs room at both ends */
  float t;
  float knob_x;
  Rectangle track;
  int changed = 0;
  int hover;
  int held; /* this slider is the one being dragged */

  if (value == NULL || !(span > 0.0f) || usable <= 0.0f)
  {
    return 0;
  }

  hover = enabled && hovering(bounds);
  held = ui_slider_dragging && same_rect(ui_slider_held, bounds);

  if (held && (!enabled || !IsMouseButtonDown(MOUSE_BUTTON_LEFT)))
  {
    ui_slider_dragging = 0;
    held = 0;
  }
  else if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
  {
    ui_slider_held = bounds;
    ui_slider_dragging = 1;
    held = 1;
  }

  if (held)
  {
    float want = (GetMousePosition().x - bounds.x - knob_r) / usable;
    float next;

    if (want < 0.0f)
    {
      want = 0.0f;
    }
    if (want > 1.0f)
    {
      want = 1.0f;
    }

    next = min + want * span;
    if (next != *value)
    {
      *value = next;
      changed = 1;
    }
  }
  else if (hover)
  {
    float wheel = GetMouseWheelMove();

    if (wheel != 0.0f)
    {
      float next = *value + wheel * span * UI_SLIDER_WHEEL;

      if (next < min)
      {
        next = min;
      }
      if (next > max)
      {
        next = max;
      }
      if (next != *value)
      {
        *value = next;
        changed = 1;
      }
    }
  }

  t = (*value - min) / span;
  if (t < 0.0f)
  {
    t = 0.0f;
  }
  if (t > 1.0f)
  {
    t = 1.0f;
  }
  knob_x = bounds.x + knob_r + usable * t;

  track.x = bounds.x + knob_r;
  track.y = track_y - 2.5f;
  track.width = usable;
  track.height = 5.0f;

  aud_ui_panel(track, 2.5f, enabled ? AUD_UI_EDGE_SOFT : fade_to(AUD_UI_EDGE_SOFT, 0.5f),
               BLANK);
  if (t > 0.0f)
  {
    Rectangle filled = {track.x, track.y, usable * t, track.height};

    aud_ui_panel(filled, 2.5f, enabled ? tint : fade_to(tint, 0.3f), BLANK);
  }

  /*
   * A knob with a ring round it rather than a plain disc: the ring is what
   * keeps it visible where it crosses its own filled track, and it grows under
   * the pointer so a control this small says which one is about to move.
   */
  {
    float r = knob_r - 3.0f;
    Color face = enabled ? WHITE : fade_to(AUD_UI_MUTED, 0.45f);

    if (r < 3.0f)
    {
      r = 3.0f;
    }
    if (hover || held)
    {
      r += 1.0f;
    }

    if (enabled)
    {
      DrawCircleV((Vector2){knob_x, track_y + 1.0f}, r + 1.0f, fade_to(BLACK, 0.35f));
      DrawCircleV((Vector2){knob_x, track_y}, r + 1.0f,
                  fade_to(tint, (hover || held) ? 0.75f : 0.45f));
    }
    DrawCircleV((Vector2){knob_x, track_y}, r, face);
  }

  if (hover || held)
  {
    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
  }

  return changed;
}

/* -- meter ------------------------------------------------------------------ */

/*
 * Green up to about -12 dBFS, amber through the last few dB and red at the
 * top, so a glance at the colour is enough to know whether the input is hot.
 */
static Color meter_color(float t)
{
  if (t < 0.7f)
  {
    return AUD_UI_OK;
  }
  if (t < 0.92f)
  {
    return AUD_UI_WARN;
  }
  return AUD_UI_RECORD;
}

/* How many blocks a meter this wide is cut into. */
#define UI_METER_SEG 3.0f
#define UI_METER_GAP 1.5f

void aud_ui_meter(Rectangle bounds, float level, float peak_hold)
{
  int count;
  float step;

  if (level < 0.0f)
  {
    level = 0.0f;
  }
  if (level > 1.0f)
  {
    level = 1.0f;
  }

  aud_ui_panel(bounds, 3.0f, AUD_UI_BG, AUD_UI_EDGE_SOFT);

  /*
   * Blocks rather than one sliding bar. A bar says how loud it is; blocks say
   * that and also how fast it is moving, because the eye counts the ones that
   * light and not the distance one travelled - which is why every hardware
   * meter worth looking at has been built this way.
   */
  count = (int)((bounds.width - 4.0f) / (UI_METER_SEG + UI_METER_GAP));
  if (count < 4)
  {
    count = 4;
  }
  step = (bounds.width - 4.0f) / (float)count;

  for (int i = 0; i < count; i++)
  {
    float at = ((float)i + 0.5f) / (float)count;
    Rectangle block = {bounds.x + 2.0f + (float)i * step, bounds.y + 2.5f,
                       step - UI_METER_GAP, bounds.height - 5.0f};
    Color c = meter_color(at);

    if (block.width < 1.0f)
    {
      block.width = 1.0f;
    }

    DrawRectangleRec(block, at <= level ? c : fade_to(c, 0.10f));
  }

  if (peak_hold >= 0.0f)
  {
    float t = peak_hold > 1.0f ? 1.0f : peak_hold;
    float x = bounds.x + 2.0f + (bounds.width - 4.0f) * t;

    DrawRectangleRec((Rectangle){x - 1.0f, bounds.y + 1.5f, 2.0f, bounds.height - 3.0f},
                     mix(meter_color(t), WHITE, 0.5f));
  }
}

/* -- tabs ------------------------------------------------------------------- */

/*
 * Where the lit pill was last frame, so it can slide to where it is now rather
 * than jumping. Remembered by the strip's own rectangle, the way the slider's
 * drag is: there is only ever one strip being pointed at, and a second one
 * elsewhere on the window simply starts its own slide from wherever it is.
 */
static struct
{
  Rectangle strip;
  float x;
  float width;
} ui_tab_pill;

int aud_ui_tabs(Rectangle bounds, const char *const *labels, int count, int *selected,
                int enabled, int dim)
{
  int changed = 0;
  int over;
  float seg;
  float strength;
  Rectangle lit;

  if (labels == NULL || selected == NULL || count <= 0 || bounds.width <= 0.0f)
  {
    return 0;
  }

  if (*selected < 0 || *selected >= count)
  {
    *selected = 0;
  }

  over = enabled && hovering(bounds);
  seg = bounds.width / (float)count;

  /* faded until pointed at, so it stays out of the way of the visualiser */
  strength = (!dim || over) ? 1.0f : 0.5f;
  if (!enabled)
  {
    strength *= 0.5f;
  }

  aud_ui_panel(bounds, bounds.height / 2.0f, fade_to(AUD_UI_PANEL, 0.7f * strength),
               fade_to(AUD_UI_EDGE_SOFT, 0.9f * strength));

  lit.x = bounds.x + seg * (float)*selected + 2.0f;
  lit.y = bounds.y + 2.0f;
  lit.width = seg - 4.0f;
  lit.height = bounds.height - 4.0f;

  /*
   * The slide. Exponential rather than linear so it arrives without a stop,
   * and frame-rate independent so it takes the same fifth of a second whatever
   * the window is drawing at.
   */
  if (!same_rect(ui_tab_pill.strip, bounds))
  {
    ui_tab_pill.strip = bounds;
    ui_tab_pill.x = lit.x;
    ui_tab_pill.width = lit.width;
  }
  else
  {
    float k = 1.0f - expf(-16.0f * GetFrameTime());

    ui_tab_pill.x += (lit.x - ui_tab_pill.x) * k;
    ui_tab_pill.width += (lit.width - ui_tab_pill.width) * k;
    lit.x = ui_tab_pill.x;
    lit.width = ui_tab_pill.width;
  }

  aud_ui_panel(lit, lit.height / 2.0f, fade_to(AUD_UI_SURFACE_HI, 0.95f * strength),
               fade_to(AUD_UI_ACCENT, 0.35f * strength));

  for (int i = 0; i < count; i++)
  {
    Rectangle cell = {bounds.x + seg * (float)i, bounds.y, seg, bounds.height};
    int cell_hover = enabled && hovering(cell);
    Color text;

    if (i == *selected)
    {
      text = fade_to(WHITE, strength);
    }
    else
    {
      text = fade_to(cell_hover ? AUD_UI_TEXT : AUD_UI_MUTED, strength);
    }

    {
      float room = cell.width - 2.0f * UI_LABEL_PAD;
      int size = fitted_size(AUD_UI_STRONG, labels[i], 15, room);

      aud_ui_write_centred(AUD_UI_STRONG, cell, size, text,
                           shortened(AUD_UI_STRONG, labels[i], size, room));
    }

    if (cell_hover)
    {
      SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    }

    if (cell_hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && i != *selected)
    {
      *selected = i;
      changed = 1;
    }
  }

  return changed;
}

/* -- rows, fields and menus -------------------------------------------------- */

/*
 * Draw `text` clipped to `max_width`, ending in an ellipsis when it does not
 * fit. Device descriptions are as long as their vendor felt like making them.
 */
static void text_fit(aud_ui_face face, float x, float y, int size, Color color,
                     const char *text, float max_width)
{
  aud_ui_write(face, x, y, size, color, shortened(face, text, size, max_width));
}

/*
 * The highlight under the row the pointer is on, or the row that is already
 * chosen: a rounded fill and, for the chosen one, a bar down its leading edge.
 * The bar is what tells "this is where you are" from "this is what you would
 * get", which a colour alone does not.
 */
static void row_highlight(Rectangle row, int hover, int current)
{
  if (hover)
  {
    aud_ui_panel(row, UI_CORNER_SMALL, mix(AUD_UI_SURFACE, AUD_UI_ACCENT, 0.28f), BLANK);
  }
  else if (current)
  {
    aud_ui_panel(row, UI_CORNER_SMALL, fade_to(AUD_UI_ACCENT, 0.10f), BLANK);
  }

  if (current)
  {
    DrawRectangleRounded((Rectangle){row.x + 2.0f, row.y + 4.0f, 2.5f, row.height - 8.0f},
                         1.0f, 4, AUD_UI_ACCENT);
  }
}

/*
 * Append `c` to `text` if there is room. Printable ASCII only: the fields this
 * is used for name files, and a character that arrives as one code point and
 * leaves as a box helps nobody.
 */
static int field_append(char *text, size_t size, int c, size_t len)
{
  if (c < 0x20 || c > 0x7e || len + 1 >= size)
  {
    return 0;
  }

  text[len] = (char)c;
  text[len + 1] = '\0';
  return 1;
}

int aud_ui_field(Rectangle bounds, char *text, size_t size, int focused, int enabled)
{
  float pad = 11.0f;
  size_t len;
  int result = 0;
  int hover;

  if (text == NULL || size == 0 || bounds.width <= 2.0f * pad)
  {
    return 0;
  }

  hover = enabled && hovering(bounds);
  len = strlen(text);

  if (enabled && focused)
  {
    int c;

    while ((c = GetCharPressed()) != 0)
    {
      if (field_append(text, size, c, len))
      {
        len++;
        result |= AUD_UI_FIELD_EDITED;
      }
    }

    if (len > 0 && (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)))
    {
      text[--len] = '\0';
      result |= AUD_UI_FIELD_EDITED;
    }

    /* the two the terminal has taught everyone: clear the line, paste over it */
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
    {
      if (IsKeyPressed(KEY_U) && len > 0)
      {
        text[0] = '\0';
        len = 0;
        result |= AUD_UI_FIELD_EDITED;
      }
      if (IsKeyPressed(KEY_V))
      {
        const char *paste = GetClipboardText();

        if (paste != NULL)
        {
          for (; *paste != '\0'; paste++)
          {
            if (!field_append(text, size, (unsigned char)*paste, len))
            {
              continue;
            }
            len++;
            result |= AUD_UI_FIELD_EDITED;
          }
        }
      }
    }

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
    {
      result |= AUD_UI_FIELD_SUBMITTED;
    }
  }

  /*
   * A focused field is ringed rather than merely outlined in the accent: two
   * pixels of colour and a soft halo outside them, which is the one thing on a
   * dark window that reads as "typing goes here" from across the room.
   */
  if (enabled && focused)
  {
    Rectangle halo = {bounds.x - 2.0f, bounds.y - 2.0f, bounds.width + 4.0f,
                      bounds.height + 4.0f};

    aud_ui_panel(halo, UI_CORNER + 2.0f, BLANK, fade_to(AUD_UI_ACCENT, 0.28f));
  }

  aud_ui_panel(bounds, UI_CORNER,
               enabled ? mix(AUD_UI_BG, AUD_UI_PANEL, 0.6f)
                       : mix(AUD_UI_PANEL, AUD_UI_BG, 0.5f),
               focused ? AUD_UI_ACCENT
                       : (hover ? mix(AUD_UI_EDGE, AUD_UI_ACCENT, 0.5f) : AUD_UI_EDGE));

  {
    float inner = bounds.width - 2.0f * pad;
    float y = bounds.y + (bounds.height - 17.0f) / 2.0f;
    const char *shown = text;
    float width;

    /*
     * Scrolled from the right: the caret is at the end of the line, and a
     * field that showed the start of a long path would hide the part being
     * typed. Whole characters at a time, so nothing is drawn half off the box.
     */
    while (*shown != '\0' && aud_ui_measure(AUD_UI_SANS, shown, 17) > inner - 8.0f)
    {
      shown++;
    }
    width = aud_ui_measure(AUD_UI_SANS, shown, 17);

    aud_ui_text(bounds.x + pad, y, 17,
                enabled ? AUD_UI_TEXT : fade_to(AUD_UI_MUTED, 0.45f), shown);

    /* a caret that blinks, so a field with focus is obvious while it is empty */
    if (enabled && focused && fmod(GetTime(), 1.0) < 0.55)
    {
      DrawRectangleRounded(
          (Rectangle){bounds.x + pad + width + 1.0f, y - 1.0f, 2.0f, 20.0f}, 1.0f, 4,
          AUD_UI_ACCENT);
    }
  }

  if (hover)
  {
    SetMouseCursor(MOUSE_CURSOR_IBEAM);
  }
  if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
  {
    result |= AUD_UI_FIELD_CLICKED;
  }

  return result;
}

static void clamp_scroll(int *scroll, int count, int rows)
{
  if (*scroll > count - rows)
  {
    *scroll = count - rows;
  }
  if (*scroll < 0)
  {
    *scroll = 0;
  }
}

/*
 * The thumb down the right-hand edge of anything that scrolls. A count in the
 * corner says how far down a list you are; this says it without being read, and
 * says how much of the list you are looking at as well.
 */
static void scroll_thumb(Rectangle bounds, int scroll, int rows, int count)
{
  float track_h = bounds.height - 8.0f;
  float thumb_h = track_h * (float)rows / (float)count;
  float at;

  if (count <= rows || track_h <= 0.0f)
  {
    return;
  }

  if (thumb_h < 18.0f)
  {
    thumb_h = 18.0f;
  }
  at = (count == rows) ? 0.0f : (float)scroll / (float)(count - rows);

  DrawRectangleRounded((Rectangle){bounds.x + bounds.width - 6.0f,
                                   bounds.y + 4.0f + (track_h - thumb_h) * at, 3.0f,
                                   thumb_h},
                       1.0f, 4, fade_to(AUD_UI_MUTED, 0.45f));
}

int aud_ui_list(Rectangle bounds, const char *const *items, int count, int marked,
                int *scroll, int enabled)
{
  int rows;
  int clicked = -1;

  if (items == NULL || scroll == NULL || bounds.height < AUD_UI_LIST_ROW)
  {
    return -1;
  }

  aud_ui_panel(bounds, UI_CORNER, mix(AUD_UI_BG, AUD_UI_PANEL, 0.5f), AUD_UI_EDGE);

  rows = (int)(bounds.height / AUD_UI_LIST_ROW);
  if (rows > count)
  {
    rows = count;
  }

  if (count > rows && enabled && hovering(bounds))
  {
    *scroll -= (int)GetMouseWheelMove();
  }
  clamp_scroll(scroll, count, rows);

  if (count == 0)
  {
    aud_ui_text_centred(bounds, 15, AUD_UI_FAINT, "nothing here");
    return -1;
  }

  for (int i = 0; i < rows; i++)
  {
    int item = *scroll + i;
    Rectangle row = {bounds.x + 4.0f, bounds.y + 3.0f + (float)i * AUD_UI_LIST_ROW,
                     bounds.width - 8.0f, AUD_UI_LIST_ROW};
    int row_hover = enabled && hovering(row);
    Color text = AUD_UI_TEXT;

    row_highlight(row, row_hover, item == marked);

    if (row_hover)
    {
      text = WHITE;
      SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    }
    else if (item == marked)
    {
      text = AUD_UI_ACCENT;
    }
    else if (!enabled)
    {
      text = fade_to(AUD_UI_MUTED, 0.45f);
    }

    text_fit(AUD_UI_SANS, row.x + 12.0f, row.y + (row.height - 16.0f) / 2.0f, 16, text,
             items[item], row.width - 24.0f);

    if (row_hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
      clicked = item;
    }
  }

  scroll_thumb(bounds, *scroll, rows, count);

  return clicked;
}

int aud_ui_dropdown(Rectangle bounds, const char *const *items, int count, int *selected,
                    int *open, int *scroll, int enabled)
{
  int hover;
  int changed = 0;
  int rows;
  int was_open;
  Color edge;
  const char *label;

  if (items == NULL || selected == NULL || open == NULL || scroll == NULL || count <= 0)
  {
    return 0;
  }

  if (*selected < 0 || *selected >= count)
  {
    *selected = 0;
  }
  if (!enabled)
  {
    *open = 0;
  }
  was_open = *open;

  hover = enabled && hovering(bounds);
  edge = *open ? AUD_UI_ACCENT
               : (hover ? mix(AUD_UI_EDGE, AUD_UI_ACCENT, 0.6f) : AUD_UI_EDGE);

  aud_ui_panel(bounds, UI_CORNER,
               enabled ? (hover || *open ? AUD_UI_SURFACE_HI : AUD_UI_SURFACE)
                       : mix(AUD_UI_PANEL, AUD_UI_BG, 0.45f),
               edge);
  if (enabled)
  {
    top_highlight(bounds, UI_CORNER, 1.0f);
  }

  label = items[*selected];
  text_fit(AUD_UI_SANS, bounds.x + 12.0f, bounds.y + (bounds.height - 17.0f) / 2.0f, 17,
           enabled ? AUD_UI_TEXT : fade_to(AUD_UI_MUTED, 0.45f), label,
           bounds.width - 40.0f);

  /* the chevron, pointing the way the list will open */
  {
    float cx = bounds.x + bounds.width - 17.0f;
    float cy = bounds.y + bounds.height / 2.0f;
    float dy = *open ? -2.5f : 2.5f;
    Color c = enabled ? (hover || *open ? AUD_UI_TEXT : AUD_UI_MUTED)
                      : fade_to(AUD_UI_MUTED, 0.45f);

    DrawLineEx((Vector2){cx - 4.5f, cy - dy}, (Vector2){cx, cy + dy}, 1.6f, c);
    DrawLineEx((Vector2){cx + 4.5f, cy - dy}, (Vector2){cx, cy + dy}, 1.6f, c);
  }

  if (hover)
  {
    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
  }

  if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
  {
    *open = !*open;
  }

  if (!*open)
  {
    return 0;
  }

  rows = count < AUD_UI_DROPDOWN_MAX_ROWS ? count : AUD_UI_DROPDOWN_MAX_ROWS;

  /* opening: bring the current selection into view rather than jumping to the top */
  if (!was_open && *selected >= rows)
  {
    *scroll = *selected - rows + 1;
  }
  clamp_scroll(scroll, count, rows);

  {
    Rectangle panel = {bounds.x, bounds.y + bounds.height + 6.0f, bounds.width,
                       (float)rows * bounds.height + 10.0f};
    int clicked_inside = 0;

    if (count > rows && CheckCollisionPointRec(GetMousePosition(), panel))
    {
      *scroll -= (int)GetMouseWheelMove();
      clamp_scroll(scroll, count, rows);
    }

    aud_ui_shadow(panel, UI_CORNER + 2.0f, 10.0f);
    aud_ui_panel(panel, UI_CORNER + 2.0f, AUD_UI_SURFACE, AUD_UI_EDGE);

    for (int i = 0; i < rows; i++)
    {
      int item = *scroll + i;
      Rectangle row = {panel.x + 5.0f, panel.y + 5.0f + (float)i * bounds.height,
                       panel.width - 10.0f, bounds.height};
      int row_hover = hovering(row);
      Color text = AUD_UI_TEXT;

      row_highlight(row, row_hover, item == *selected);

      if (row_hover)
      {
        text = WHITE;
        SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
      }
      else if (item == *selected)
      {
        text = AUD_UI_ACCENT;
      }

      text_fit(AUD_UI_SANS, row.x + 12.0f, row.y + (row.height - 17.0f) / 2.0f, 17, text,
               items[item], row.width - 24.0f);

      if (row_hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
      {
        clicked_inside = 1;
        if (item != *selected)
        {
          *selected = item;
          changed = 1;
        }
        *open = 0;
      }
    }

    scroll_thumb(panel, *scroll, rows, count);

    /* a hint that there is more, so a long list does not look like a short one */
    if (count > rows)
    {
      char more[48]; /* three int32s, their separators and the terminator */

      snprintf(more, sizeof(more), "%d-%d of %d", *scroll + 1, *scroll + rows, count);
      aud_ui_write_right(AUD_UI_MONO, panel.x + panel.width - 8.0f,
                         panel.y + panel.height + 4.0f, 13, AUD_UI_FAINT, more);
    }

    /* a click anywhere else closes it, which is what every other menu does */
    if (!clicked_inside && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(GetMousePosition(), panel) && !hover)
    {
      *open = 0;
    }
  }

  return changed;
}

/* -- hover help -------------------------------------------------------------- */

/* The one line of hover help a frame can show, held between ask and draw. */
static struct
{
  Rectangle over; /* the control the pointer settled on */
  char text[128];
  float waited; /* seconds it has rested there */
  int pending;  /* asked for this frame */
} ui_tip;

void aud_ui_tooltip(Rectangle bounds, const char *text)
{
  if (text == NULL || *text == '\0' || !hovering(bounds))
  {
    return;
  }

  /*
   * Crossing to another control starts the wait again, so dragging the pointer
   * along the transport does not trail a tooltip behind it.
   */
  if (!same_rect(ui_tip.over, bounds))
  {
    ui_tip.over = bounds;
    ui_tip.waited = 0.0f;
  }

  snprintf(ui_tip.text, sizeof(ui_tip.text), "%s", text);
  ui_tip.waited += GetFrameTime();
  ui_tip.pending = 1;
}

void aud_ui_tooltip_draw(void)
{
  Vector2 mouse;
  Rectangle box;
  float alpha;

  if (!ui_tip.pending)
  {
    Rectangle nowhere = {-1.0f, -1.0f, 0.0f, 0.0f};

    ui_tip.over = nowhere;
    ui_tip.waited = 0.0f;
    return;
  }
  ui_tip.pending = 0;

  if (ui_tip.waited < UI_TIP_DELAY)
  {
    return;
  }

  /* faded in over a fifth of a second, so it arrives rather than appears */
  alpha = (ui_tip.waited - UI_TIP_DELAY) / 0.18f;
  if (alpha > 1.0f)
  {
    alpha = 1.0f;
  }

  mouse = GetMousePosition();
  box.width = aud_ui_measure(AUD_UI_SANS, ui_tip.text, UI_TIP_FONT) + 2.0f * UI_TIP_PAD;
  box.height = (float)UI_TIP_FONT + 2.0f * UI_TIP_PAD;
  box.x = mouse.x + 14.0f;
  box.y = mouse.y + 22.0f;

  /* the controls it describes are at the edges of the window, so it has to be
   * allowed to open the other way rather than half off the screen */
  if (box.x + box.width > (float)GetScreenWidth() - 6.0f)
  {
    box.x = (float)GetScreenWidth() - 6.0f - box.width;
  }
  if (box.x < 6.0f)
  {
    box.x = 6.0f;
  }

  /* above the pointer down at the foot of the window: the transport lives there
   * and its help should not be sitting on the clock and the meter */
  if (box.y + box.height > (float)GetScreenHeight() * 0.75f)
  {
    box.y = mouse.y - 14.0f - box.height;
  }
  if (box.y < 6.0f)
  {
    box.y = 6.0f;
  }

  aud_ui_shadow(box, UI_CORNER, 8.0f * alpha);
  aud_ui_panel(box, UI_CORNER, fade_to(AUD_UI_SURFACE_HI, alpha),
               fade_to(AUD_UI_EDGE, alpha));
  aud_ui_write_centred(AUD_UI_SANS, box, UI_TIP_FONT, fade_to(AUD_UI_TEXT, alpha),
                       ui_tip.text);
}

void aud_ui_format_clock(char *dst, size_t size, double seconds)
{
  unsigned total;
  unsigned minutes;
  unsigned secs;
  unsigned tenths;

  if (dst == NULL || size == 0)
  {
    return;
  }

  if (!(seconds > 0.0))
  {
    seconds = 0.0;
  }

  total = (unsigned)(seconds * 10.0 + 0.5);
  tenths = total % 10u;
  secs = (total / 10u) % 60u;
  minutes = total / 600u;

  if (minutes > 99u)
  {
    minutes = 99u;
  } /* the layout is sized for two digits */

  snprintf(dst, size, "%02u:%02u.%u", minutes, secs, tenths);
}
