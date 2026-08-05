/* SPDX-License-Identifier: MIT */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define UI_CORNER 6.0f
#define UI_FONT 20

/* How far the fill of a control is lifted when the pointer is over it. */
#define UI_HOVER_LIFT 0.10f
#define UI_PRESS_DROP 0.06f

static Color mix(Color a, Color b, float t)
{
  Color out;

  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;

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

static int hovering(Rectangle bounds)
{
  return CheckCollisionPointRec(GetMousePosition(), bounds);
}

void aud_ui_panel(Rectangle bounds)
{
  DrawRectangleRounded(bounds, UI_CORNER / bounds.height, 8, AUD_UI_PANEL);
  DrawRectangleRoundedLines(bounds, UI_CORNER / bounds.height, 8, AUD_UI_EDGE);
}

void aud_ui_text(float x, float y, int size, Color color, const char *text)
{
  DrawText(text, (int)x, (int)y, size, color);
}

void aud_ui_text_right(float right, float y, int size, Color color, const char *text)
{
  int width = MeasureText(text, size);

  DrawText(text, (int)right - width, (int)y, size, color);
}

void aud_ui_text_centred(Rectangle bounds, int size, Color color, const char *text)
{
  int width = MeasureText(text, size);
  float x = bounds.x + (bounds.width - (float)width) / 2.0f;
  float y = bounds.y + (bounds.height - (float)size) / 2.0f;

  DrawText(text, (int)x, (int)y, size, color);
}

/* Shared body of button and toggle: returns non-zero when clicked. */
static int clickable(Rectangle bounds, const char *label, Color tint, int enabled,
                     int lit)
{
  int hover = enabled && hovering(bounds);
  int down = hover && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
  float roundness = UI_CORNER / bounds.height;
  Color fill;
  Color edge;
  Color text;

  if (!enabled)
  {
    fill = mix(AUD_UI_PANEL, AUD_UI_BG, 0.5f);
    edge = fade_to(AUD_UI_EDGE, 0.4f);
    text = fade_to(AUD_UI_MUTED, 0.45f);
  }
  else if (lit)
  {
    fill = mix(AUD_UI_PANEL, tint, down ? 0.55f : (hover ? 0.45f : 0.35f));
    edge = tint;
    text = WHITE;
  }
  else
  {
    float lift = down ? -UI_PRESS_DROP : (hover ? UI_HOVER_LIFT : 0.0f);

    fill = mix(AUD_UI_PANEL, lift < 0.0f ? AUD_UI_BG : tint, fabsf(lift));
    edge = hover ? mix(AUD_UI_EDGE, tint, 0.6f) : AUD_UI_EDGE;
    text = hover ? WHITE : AUD_UI_TEXT;
  }

  DrawRectangleRounded(bounds, roundness, 8, fill);
  DrawRectangleRoundedLines(bounds, roundness, 8, edge);
  aud_ui_text_centred(bounds, UI_FONT, text, label);

  if (enabled && hover)
    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);

  return hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

int aud_ui_button(Rectangle bounds, const char *label, Color tint, int enabled)
{
  return clickable(bounds, label, tint, enabled, 0);
}

int aud_ui_toggle(Rectangle bounds, const char *label, int on, Color tint, int enabled)
{
  return clickable(bounds, label, tint, enabled, on);
}

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

  if (value == NULL || !(span > 0.0f) || usable <= 0.0f)
    return 0;

  hover = enabled && hovering(bounds);

  if (enabled && hover && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
  {
    float want = (GetMousePosition().x - bounds.x - knob_r) / usable;
    float next;

    if (want < 0.0f)
      want = 0.0f;
    if (want > 1.0f)
      want = 1.0f;

    next = min + want * span;
    if (next != *value)
    {
      *value = next;
      changed = 1;
    }
  }

  t = (*value - min) / span;
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;
  knob_x = bounds.x + knob_r + usable * t;

  track.x = bounds.x + knob_r;
  track.y = track_y - 3.0f;
  track.width = usable;
  track.height = 6.0f;

  DrawRectangleRounded(track, 1.0f, 6,
                       enabled ? AUD_UI_EDGE : fade_to(AUD_UI_EDGE, 0.4f));
  if (t > 0.0f)
  {
    Rectangle filled = {track.x, track.y, usable * t, track.height};

    DrawRectangleRounded(filled, 1.0f, 6, enabled ? tint : fade_to(tint, 0.35f));
  }

  DrawCircle((int)knob_x, (int)track_y, knob_r - 3.0f,
             enabled ? (hover ? WHITE : AUD_UI_TEXT) : fade_to(AUD_UI_MUTED, 0.5f));

  if (hover)
    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);

  return changed;
}

/*
 * Green up to about -12 dBFS, amber through the last few dB and red at the
 * top, so a glance at the colour is enough to know whether the input is hot.
 */
static Color meter_color(float t)
{
  if (t < 0.7f)
    return AUD_UI_OK;
  if (t < 0.92f)
    return AUD_UI_WARN;
  return AUD_UI_RECORD;
}

void aud_ui_meter(Rectangle bounds, float level, float peak_hold)
{
  float roundness = 1.0f;

  if (level < 0.0f)
    level = 0.0f;
  if (level > 1.0f)
    level = 1.0f;

  DrawRectangleRounded(bounds, roundness, 6, AUD_UI_BG);

  if (level > 0.0f)
  {
    Rectangle filled = {bounds.x, bounds.y, bounds.width * level, bounds.height};

    DrawRectangleRounded(filled, roundness, 6, meter_color(level));
  }

  if (peak_hold >= 0.0f)
  {
    float t = peak_hold > 1.0f ? 1.0f : peak_hold;
    float x = bounds.x + bounds.width * t;

    DrawRectangleRec((Rectangle){x - 1.0f, bounds.y, 2.0f, bounds.height},
                     fade_to(meter_color(t), 0.9f));
  }

  DrawRectangleRoundedLines(bounds, roundness, 6, AUD_UI_EDGE);
}

/*
 * Draw `text` clipped to `max_width`, ending in an ellipsis when it does not
 * fit. Device descriptions are as long as their vendor felt like making them.
 */
static void text_fit(float x, float y, int size, Color color, const char *text,
                     float max_width)
{
  char buf[192];
  size_t len;

  if ((float)MeasureText(text, size) <= max_width)
  {
    DrawText(text, (int)x, (int)y, size, color);
    return;
  }

  snprintf(buf, sizeof(buf), "%s", text);
  len = strlen(buf);

  /* trim a character at a time; the strings are short and this runs per frame
   * only for the one label that overflows */
  while (len > 1)
  {
    buf[--len] = '\0';
    if (len + 3 < sizeof(buf) &&
        (float)MeasureText(TextFormat("%s...", buf), size) <= max_width)
      break;
  }

  DrawText(TextFormat("%s...", buf), (int)x, (int)y, size, color);
}

int aud_ui_dropdown(Rectangle bounds, const char *const *items, int count, int *selected,
                    int *open, int enabled)
{
  int hover;
  int changed = 0;
  int rows;
  float roundness;
  Color edge;
  const char *label;

  if (items == NULL || selected == NULL || open == NULL || count <= 0)
    return 0;

  if (*selected < 0 || *selected >= count)
    *selected = 0;
  if (!enabled)
    *open = 0;

  hover = enabled && hovering(bounds);
  roundness = UI_CORNER / bounds.height;
  edge = *open ? AUD_UI_ACCENT
               : (hover ? mix(AUD_UI_EDGE, AUD_UI_ACCENT, 0.6f) : AUD_UI_EDGE);

  DrawRectangleRounded(bounds, roundness, 8,
                       enabled ? AUD_UI_PANEL : mix(AUD_UI_PANEL, AUD_UI_BG, 0.5f));
  DrawRectangleRoundedLines(bounds, roundness, 8, edge);

  label = items[*selected];
  text_fit(bounds.x + 10.0f, bounds.y + (bounds.height - 18.0f) / 2.0f, 18,
           enabled ? AUD_UI_TEXT : fade_to(AUD_UI_MUTED, 0.45f), label,
           bounds.width - 34.0f);

  /* the caret, pointing the way the list will open */
  {
    float cx = bounds.x + bounds.width - 16.0f;
    float cy = bounds.y + bounds.height / 2.0f;
    Vector2 a = {cx - 5.0f, cy - (*open ? -2.0f : 2.0f)};
    Vector2 b = {cx + 5.0f, cy - (*open ? -2.0f : 2.0f)};
    Vector2 c = {cx, cy + (*open ? -3.0f : 3.0f)};

    /* raylib wants triangle vertices counter-clockwise or it culls them */
    if (*open)
      DrawTriangle(a, b, c, enabled ? AUD_UI_MUTED : fade_to(AUD_UI_MUTED, 0.45f));
    else
      DrawTriangle(b, a, c, enabled ? AUD_UI_MUTED : fade_to(AUD_UI_MUTED, 0.45f));
  }

  if (hover)
    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);

  if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    *open = !*open;

  if (!*open)
    return 0;

  rows = count < AUD_UI_DROPDOWN_MAX_ROWS ? count : AUD_UI_DROPDOWN_MAX_ROWS;

  {
    Rectangle panel = {bounds.x, bounds.y + bounds.height + 4.0f, bounds.width,
                       (float)rows * bounds.height + 8.0f};
    int clicked_inside = 0;

    DrawRectangleRounded(panel, 8.0f / panel.height, 8, AUD_UI_PANEL);
    DrawRectangleRoundedLines(panel, 8.0f / panel.height, 8, AUD_UI_ACCENT);

    for (int i = 0; i < rows; i++)
    {
      Rectangle row = {panel.x + 4.0f, panel.y + 4.0f + (float)i * bounds.height,
                       panel.width - 8.0f, bounds.height};
      int row_hover = hovering(row);
      Color text = AUD_UI_TEXT;

      if (row_hover)
      {
        DrawRectangleRounded(row, 0.35f, 6, mix(AUD_UI_PANEL, AUD_UI_ACCENT, 0.35f));
        text = WHITE;
        SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
      }
      else if (i == *selected)
      {
        text = AUD_UI_ACCENT;
      }

      text_fit(row.x + 6.0f, row.y + (row.height - 18.0f) / 2.0f, 18, text, items[i],
               row.width - 12.0f);

      if (row_hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
      {
        clicked_inside = 1;
        if (i != *selected)
        {
          *selected = i;
          changed = 1;
        }
        *open = 0;
      }
    }

    /* a click anywhere else closes it, which is what every other menu does */
    if (!clicked_inside && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(GetMousePosition(), panel) && !hover)
      *open = 0;
  }

  return changed;
}

void aud_ui_format_clock(char *dst, size_t size, double seconds)
{
  unsigned total;
  unsigned minutes;
  unsigned secs;
  unsigned tenths;

  if (dst == NULL || size == 0)
    return;

  if (!(seconds > 0.0))
    seconds = 0.0;

  total = (unsigned)(seconds * 10.0 + 0.5);
  tenths = total % 10u;
  secs = (total / 10u) % 60u;
  minutes = total / 600u;

  if (minutes > 99u)
    minutes = 99u; /* the layout is sized for two digits */

  snprintf(dst, size, "%02u:%02u.%u", minutes, secs, tenths);
}
