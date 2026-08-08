/* SPDX-License-Identifier: MIT */
#include "gui/ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define UI_CORNER 6.0f
#define UI_FONT 20

/* How far the fill of a control is lifted when the pointer is over it. */
#define UI_HOVER_LIFT 0.10f
#define UI_PRESS_DROP 0.06f

/* One wheel notch, as a fraction of the slider's range. */
#define UI_SLIDER_WHEEL 0.02f

/* How long the pointer has to rest on a control before it explains itself. */
#define UI_TIP_DELAY 0.45f
#define UI_TIP_FONT 16
#define UI_TIP_PAD 9.0f

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
  {
    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
  }

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
             enabled ? ((hover || held) ? WHITE : AUD_UI_TEXT)
                     : fade_to(AUD_UI_MUTED, 0.5f));

  if (hover || held)
  {
    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
  }

  return changed;
}

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

void aud_ui_meter(Rectangle bounds, float level, float peak_hold)
{
  float roundness = 1.0f;

  if (level < 0.0f)
  {
    level = 0.0f;
  }
  if (level > 1.0f)
  {
    level = 1.0f;
  }

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

int aud_ui_tabs(Rectangle bounds, const char *const *labels, int count, int *selected,
                int enabled, int dim)
{
  int changed = 0;
  int over;
  float seg;
  float strength;

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
  strength = (!dim || over) ? 1.0f : 0.45f;
  if (!enabled)
  {
    strength *= 0.5f;
  }

  DrawRectangleRounded(bounds, 0.4f, 8, fade_to(AUD_UI_PANEL, 0.55f * strength));
  DrawRectangleRoundedLines(bounds, 0.4f, 8, fade_to(AUD_UI_EDGE, 0.7f * strength));

  for (int i = 0; i < count; i++)
  {
    Rectangle cell = {bounds.x + seg * (float)i, bounds.y, seg, bounds.height};
    int cell_hover = enabled && hovering(cell);
    Color text;

    if (i == *selected)
    {
      Rectangle lit = {cell.x + 2.0f, cell.y + 2.0f, cell.width - 4.0f,
                       cell.height - 4.0f};

      DrawRectangleRounded(lit, 0.4f, 8, fade_to(AUD_UI_ACCENT, 0.30f * strength));
      text = fade_to(WHITE, strength);
    }
    else
    {
      text = fade_to(cell_hover ? AUD_UI_TEXT : AUD_UI_MUTED, strength);
    }

    aud_ui_text_centred(cell, 16, text, labels[i]);

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
    {
      break;
    }
  }

  DrawText(TextFormat("%s...", buf), (int)x, (int)y, size, color);
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

int aud_ui_dropdown(Rectangle bounds, const char *const *items, int count, int *selected,
                    int *open, int *scroll, int enabled)
{
  int hover;
  int changed = 0;
  int rows;
  int was_open;
  float roundness;
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
    {
      DrawTriangle(a, b, c, enabled ? AUD_UI_MUTED : fade_to(AUD_UI_MUTED, 0.45f));
    }
    else
    {
      DrawTriangle(b, a, c, enabled ? AUD_UI_MUTED : fade_to(AUD_UI_MUTED, 0.45f));
    }
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
    Rectangle panel = {bounds.x, bounds.y + bounds.height + 4.0f, bounds.width,
                       (float)rows * bounds.height + 8.0f};
    int clicked_inside = 0;

    if (count > rows && CheckCollisionPointRec(GetMousePosition(), panel))
    {
      *scroll -= (int)GetMouseWheelMove();
      clamp_scroll(scroll, count, rows);
    }

    DrawRectangleRounded(panel, 8.0f / panel.height, 8, AUD_UI_PANEL);
    DrawRectangleRoundedLines(panel, 8.0f / panel.height, 8, AUD_UI_ACCENT);

    for (int i = 0; i < rows; i++)
    {
      int item = *scroll + i;
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
      else if (item == *selected)
      {
        text = AUD_UI_ACCENT;
      }

      text_fit(row.x + 6.0f, row.y + (row.height - 18.0f) / 2.0f, 18, text, items[item],
               row.width - 12.0f);

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

    /* a hint that there is more, so a long list does not look like a short one */
    if (count > rows)
    {
      char more[48]; /* three int32s, their separators and the terminator */

      snprintf(more, sizeof(more), "%d-%d of %d", *scroll + 1, *scroll + rows, count);
      aud_ui_text_right(panel.x + panel.width - 8.0f, panel.y + panel.height + 2.0f, 14,
                        AUD_UI_MUTED, more);
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

  mouse = GetMousePosition();
  box.width = (float)MeasureText(ui_tip.text, UI_TIP_FONT) + 2.0f * UI_TIP_PAD;
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

  DrawRectangleRounded(box, UI_CORNER / box.height, 8,
                       mix(AUD_UI_PANEL, AUD_UI_EDGE, 0.4f));
  DrawRectangleRoundedLines(box, UI_CORNER / box.height, 8, AUD_UI_EDGE);
  aud_ui_text_centred(box, UI_TIP_FONT, AUD_UI_TEXT, ui_tip.text);
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
