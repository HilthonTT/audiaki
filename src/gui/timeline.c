/* SPDX-License-Identifier: MIT */
#include "gui/timeline.h"

#include "gui/ui.h"

#include "edit/edit.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * The two tones a waveform is drawn in: a deep envelope for the peaks and a
 * lighter core for the RMS. Distinct enough to read as two readings rather than
 * as one shape with a highlight, which is the whole point of drawing both.
 */
#define TL_WAVE       \
  CLITERAL(Color)     \
  {                   \
    52, 112, 194, 255 \
  }
#define TL_WAVE_CORE   \
  CLITERAL(Color)      \
  {                    \
    150, 205, 255, 255 \
  }
#define TL_WAVE_SEL   \
  CLITERAL(Color)     \
  {                   \
    96, 158, 236, 255 \
  }
#define TL_WAVE_SEL_CORE \
  CLITERAL(Color)        \
  {                      \
    214, 234, 255, 255   \
  }

#define TL_RULER_FONT 14
#define TL_LABEL_FONT 16
#define TL_SMALL_FONT 14

/* The grab strip along a track's bottom edge that resizes it. */
#define TL_RESIZE_GRIP 6.0f

/* How wide a scrollbar is, and the least of it that is ever draggable. */
#define TL_BAR_H 12.0f
#define TL_BAR_MIN 28.0f

/*
 * Tick spacings the ruler is allowed to use, in seconds. A ruler that picked
 * whatever the zoom happened to give would label 0:03.7 and 0:07.4; these are
 * the intervals people actually count in.
 */
static const double tl_steps[] = {0.001, 0.002, 0.005, 0.01,  0.02,   0.05,  0.1,  0.2,
                                  0.5,   1.0,   2.0,   5.0,   10.0,   15.0,  30.0, 60.0,
                                  120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0};

#define TL_STEP_COUNT ((int)(sizeof(tl_steps) / sizeof(tl_steps[0])))

void aud_timeline_init(aud_timeline *tl)
{
  memset(tl, 0, sizeof(*tl));
  tl->zoom = AUD_TIMELINE_ZOOM_DEFAULT;
}

float aud_timeline_x_of(const aud_timeline *tl, double seconds)
{
  return (float)((seconds - tl->scroll) * tl->zoom);
}

double aud_timeline_seconds_at(const aud_timeline *tl, float x)
{
  return tl->scroll + (double)x / tl->zoom;
}

static double clamp_zoom(double zoom)
{
  if (zoom < AUD_TIMELINE_ZOOM_MIN)
  {
    return AUD_TIMELINE_ZOOM_MIN;
  }
  if (zoom > AUD_TIMELINE_ZOOM_MAX)
  {
    return AUD_TIMELINE_ZOOM_MAX;
  }
  return zoom;
}

void aud_timeline_zoom_at(aud_timeline *tl, double factor, double at_seconds, float width)
{
  double before = aud_timeline_x_of(tl, at_seconds);
  double next = clamp_zoom(tl->zoom * factor);

  (void)width;
  tl->zoom = next;

  /* keep the moment under the pointer where it was, which is what makes
   * zooming feel like moving a lens rather than jumping somewhere else */
  tl->scroll = at_seconds - before / tl->zoom;
  if (tl->scroll < 0.0)
  {
    tl->scroll = 0.0;
  }
}

void aud_timeline_fit(aud_timeline *tl, const aud_doc *d, float width)
{
  double seconds = d->rate > 0 ? (double)aud_doc_end(d) / d->rate : 0.0;

  tl->scroll = 0.0;
  if (seconds <= 0.0 || width <= 1.0f)
  {
    tl->zoom = AUD_TIMELINE_ZOOM_DEFAULT;
    return;
  }

  /* a hair under the full width, so the last sample is not against the edge */
  tl->zoom = clamp_zoom((double)width * 0.98 / seconds);
}

void aud_timeline_fit_selection(aud_timeline *tl, const aud_doc *d, float width)
{
  double from;
  double span;

  if (!aud_doc_has_range(d) || d->rate == 0)
  {
    aud_timeline_fit(tl, d, width);
    return;
  }

  from = (double)d->sel_start / d->rate;
  span = (double)(d->sel_end - d->sel_start) / d->rate;

  tl->zoom = clamp_zoom((double)width * 0.9 / span);
  tl->scroll = from - (double)width * 0.05 / tl->zoom;
  if (tl->scroll < 0.0)
  {
    tl->scroll = 0.0;
  }
}

void aud_timeline_reveal(aud_timeline *tl, const aud_doc *d, uint64_t frame, float width)
{
  double at;
  double visible;

  if (d->rate == 0 || width <= 1.0f)
  {
    return;
  }

  at = (double)frame / d->rate;
  visible = (double)width / tl->zoom;

  if (at < tl->scroll)
  {
    tl->scroll = at;
  }
  else if (at > tl->scroll + visible)
  {
    /* a third of the way in rather than hard against the right edge, so what
     * comes next is already on screen when the playhead reaches it */
    tl->scroll = at - visible * 0.66;
  }

  if (tl->scroll < 0.0)
  {
    tl->scroll = 0.0;
  }
}

/* The tick interval that leaves labels at least `least` pixels apart. */
static double pick_step(double zoom, double least)
{
  for (int i = 0; i < TL_STEP_COUNT; i++)
  {
    if (tl_steps[i] * zoom >= least)
    {
      return tl_steps[i];
    }
  }
  return tl_steps[TL_STEP_COUNT - 1];
}

/* mm:ss for a ruler label, with tenths only when the zoom is showing them. */
static void label_time(char *dst, size_t size, double seconds, double step)
{
  unsigned m = (unsigned)(seconds / 60.0);
  double s = seconds - (double)m * 60.0;

  if (step < 1.0)
  {
    snprintf(dst, size, "%u:%04.1f", m, s);
    return;
  }
  snprintf(dst, size, "%u:%02u", m, (unsigned)(s + 0.5));
}

static uint64_t frame_at(const aud_timeline *tl, const aud_doc *d, float x)
{
  double seconds = aud_timeline_seconds_at(tl, x);

  if (seconds < 0.0)
  {
    return 0;
  }
  return (uint64_t)(seconds * d->rate + 0.5);
}

/* The waveform area: what is left of `area` once the panel and scale are out. */
static Rectangle wave_bounds(Rectangle area)
{
  Rectangle w = area;

  w.x += AUD_TIMELINE_PANEL_W + AUD_TIMELINE_SCALE_W;
  w.width -= AUD_TIMELINE_PANEL_W + AUD_TIMELINE_SCALE_W;
  if (w.width < 1.0f)
  {
    w.width = 1.0f;
  }
  return w;
}

static void draw_ruler(aud_timeline *tl, aud_doc *d, Rectangle ruler, uint64_t playhead,
                       int enabled)
{
  Rectangle strip = ruler;
  double step;
  double first;
  float wave_x = ruler.x + AUD_TIMELINE_PANEL_W + AUD_TIMELINE_SCALE_W;
  float wave_w = ruler.x + ruler.width - wave_x;

  DrawRectangleRec(ruler, AUD_UI_PANEL);
  DrawLine((int)ruler.x, (int)(ruler.y + ruler.height - 1.0f),
           (int)(ruler.x + ruler.width), (int)(ruler.y + ruler.height - 1.0f),
           AUD_UI_EDGE);

  if (wave_w < 1.0f)
  {
    return;
  }

  strip.x = wave_x;
  strip.width = wave_w;

  /* the selection, shown on the ruler too, so how much is selected is legible
   * without having to look down at whichever lane it is in */
  if (aud_doc_has_range(d) && d->rate > 0)
  {
    float from = wave_x + aud_timeline_x_of(tl, (double)d->sel_start / d->rate);
    float to = wave_x + aud_timeline_x_of(tl, (double)d->sel_end / d->rate);

    if (to > strip.x && from < strip.x + strip.width)
    {
      from = from < strip.x ? strip.x : from;
      to = to > strip.x + strip.width ? strip.x + strip.width : to;
      DrawRectangleRec((Rectangle){from, ruler.y, to - from, ruler.height},
                       Fade(AUD_UI_ACCENT, 0.22f));
    }
  }

  step = pick_step(tl->zoom, 64.0);
  first = floor(tl->scroll / step) * step;

  for (double t = first;; t += step)
  {
    float x = wave_x + aud_timeline_x_of(tl, t);
    char text[32];

    if (x > strip.x + strip.width)
    {
      break;
    }
    if (x < strip.x - 40.0f || t < 0.0)
    {
      continue;
    }

    DrawLine((int)x, (int)(ruler.y + ruler.height - 7.0f), (int)x,
             (int)(ruler.y + ruler.height), AUD_UI_MUTED);

    label_time(text, sizeof(text), t, step);
    if (x >= strip.x)
    {
      aud_ui_text(x + 4.0f, ruler.y + 4.0f, TL_RULER_FONT, AUD_UI_MUTED, text);
    }
  }

  /* half-height ticks between the labelled ones, for reading off a position */
  {
    double minor = step / 5.0;

    if (minor * tl->zoom >= 6.0)
    {
      for (double t = first;; t += minor)
      {
        float x = wave_x + aud_timeline_x_of(tl, t);

        if (x > strip.x + strip.width)
        {
          break;
        }
        if (x < strip.x || t < 0.0)
        {
          continue;
        }
        DrawLine((int)x, (int)(ruler.y + ruler.height - 4.0f), (int)x,
                 (int)(ruler.y + ruler.height), Fade(AUD_UI_EDGE, 0.9f));
      }
    }
  }

  /* the playhead, drawn as a small triangle so it reads as a handle */
  if (d->rate > 0)
  {
    float x = wave_x + aud_timeline_x_of(tl, (double)playhead / d->rate);

    if (x >= strip.x && x <= strip.x + strip.width)
    {
      Vector2 a = {x - 5.0f, ruler.y + ruler.height - 9.0f};
      Vector2 b = {x + 5.0f, ruler.y + ruler.height - 9.0f};
      Vector2 c = {x, ruler.y + ruler.height - 1.0f};

      DrawTriangle(a, b, c, AUD_UI_TEXT);
    }
  }

  /* clicking or dragging the ruler moves the cursor, the way it does in every
   * editor with one - and it is the only way to place it past the last take */
  if (enabled && CheckCollisionPointRec(GetMousePosition(), ruler) &&
      GetMousePosition().x >= strip.x)
  {
    SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
      tl->scrubbing = 1;
    }
  }

  if (tl->scrubbing)
  {
    aud_doc_set_cursor(d, frame_at(tl, d, GetMousePosition().x - wave_x));
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
      tl->scrubbing = 0;
    }
  }
}

/* The amplitude labels down the left of a lane: 1.0, 0.5, 0.0, -0.5, -1.0. */
static void draw_scale(Rectangle lane, int channels)
{
  static const float marks[] = {1.0f, 0.5f, 0.0f, -0.5f, -1.0f};
  float per = lane.height / (float)channels;

  DrawRectangleRec(lane, AUD_UI_PANEL);

  if (per < 46.0f)
  {
    return; /* no room for five labels; the waveform is the point, not these */
  }

  for (int ch = 0; ch < channels; ch++)
  {
    float top = lane.y + per * (float)ch;
    float mid = top + per / 2.0f;

    for (int i = 0; i < 5; i++)
    {
      float y = mid - marks[i] * (per / 2.0f - 3.0f);
      char text[8];

      snprintf(text, sizeof(text), "%.1f", (double)marks[i]);
      aud_ui_text_right(lane.x + lane.width - 5.0f, y - 5.0f, 11, AUD_UI_MUTED, text);
      DrawLine((int)(lane.x + lane.width - 3.0f), (int)y, (int)(lane.x + lane.width),
               (int)y, AUD_UI_EDGE);
    }
  }
}

/*
 * One channel of one track, one column of pixels at a time.
 *
 * The peak envelope and the RMS core, which is the pair of readings a waveform
 * is: how loud it got, and how loud it is. Drawn as two rectangles per column
 * rather than a polygon, because a column is what the peak index answers for
 * and joining them into a shape would only invent detail between them.
 */
static void draw_wave(const aud_track *t, unsigned ch, Rectangle lane, unsigned rate,
                      const aud_timeline *tl, int selected_range, uint64_t sel_from,
                      uint64_t sel_to)
{
  float mid = lane.y + lane.height / 2.0f;
  float half = lane.height / 2.0f - 2.0f;

  if (half <= 1.0f)
  {
    return;
  }

  DrawLine((int)lane.x, (int)mid, (int)(lane.x + lane.width), (int)mid,
           Fade(AUD_UI_EDGE, 0.8f));

  for (float px = 0.0f; px < lane.width; px += 1.0f)
  {
    double t0 = aud_timeline_seconds_at(tl, px);
    double t1 = aud_timeline_seconds_at(tl, px + 1.0f);
    uint64_t f0;
    uint64_t f1;
    aud_peak p;
    float top;
    float bottom;
    Color envelope = TL_WAVE;
    Color core = TL_WAVE_CORE;

    if (t1 <= 0.0)
    {
      continue;
    }
    f0 = t0 < 0.0 ? 0 : (uint64_t)(t0 * rate);
    f1 = (uint64_t)(t1 * rate);
    if (f1 <= f0)
    {
      f1 = f0 + 1u;
    }
    if (f0 >= aud_track_end(t))
    {
      break;
    }

    aud_track_range(t, ch, f0, f1, &p);

    /* inside the selection the whole lane is lifted, so the waveform there
     * has to lift with it or it reads as the part that is not selected */
    if (selected_range && sel_to > sel_from && f1 > sel_from && f0 < sel_to)
    {
      envelope = TL_WAVE_SEL;
      core = TL_WAVE_SEL_CORE;
    }

    top = mid - p.max * half;
    bottom = mid - p.min * half;
    if (bottom - top < 1.0f)
    {
      bottom = top + 1.0f;
    }

    DrawRectangleRec((Rectangle){lane.x + px, top, 1.0f, bottom - top}, envelope);

    if (p.rms > 0.0f)
    {
      float r = p.rms * half;

      DrawRectangleRec((Rectangle){lane.x + px, mid - r, 1.0f, r * 2.0f}, core);
    }
  }
}

/* The boundaries between clips, so a split is visible as one. */
static void draw_clip_edges(const aud_track *t, Rectangle lane, unsigned rate,
                            const aud_timeline *tl)
{
  for (size_t i = 0; i < t->count; i++)
  {
    float x = lane.x + aud_timeline_x_of(tl, (double)t->clips[i].start / rate);
    float e = lane.x + aud_timeline_x_of(
                           tl, (double)(t->clips[i].start + t->clips[i].frames) / rate);

    if (i > 0 && x >= lane.x && x <= lane.x + lane.width)
    {
      DrawLine((int)x, (int)lane.y, (int)x, (int)(lane.y + lane.height),
               Fade(AUD_UI_WARN, 0.5f));
    }
    if (e >= lane.x && e <= lane.x + lane.width && i + 1u < t->count)
    {
      DrawLine((int)e, (int)lane.y, (int)e, (int)(lane.y + lane.height),
               Fade(AUD_UI_WARN, 0.5f));
    }
  }
}

/* Selected lanes are tinted, so which tracks an edit would reach is visible. */
static Color mix_panel_selected(void)
{
  Color c = AUD_UI_PANEL;

  c.r = (unsigned char)(c.r + 14);
  c.g = (unsigned char)(c.g + 18);
  c.b = (unsigned char)(c.b + 30);
  return c;
}

static int hovering_bar(Rectangle r)
{
  return CheckCollisionPointRec(GetMousePosition(), r);
}

/* The control column: the name, what it is doing, and how loud. */
static void draw_panel(aud_doc *d, size_t index, Rectangle panel, int enabled,
                       aud_timeline *tl)
{
  aud_track *t = &d->tracks[index];
  Rectangle close = {panel.x + 6.0f, panel.y + 6.0f, 18.0f, 18.0f};
  Rectangle fold = {panel.x + panel.width - 24.0f, panel.y + 6.0f, 18.0f, 18.0f};
  float y = panel.y + 30.0f;

  DrawRectangleRec(panel, t->selected ? mix_panel_selected() : AUD_UI_PANEL);
  DrawLine((int)(panel.x + panel.width), (int)panel.y, (int)(panel.x + panel.width),
           (int)(panel.y + panel.height), AUD_UI_EDGE);

  if (aud_ui_button(close, "x", AUD_UI_RECORD, enabled))
  {
    aud_edit_remove_track(d, index);
    return;
  }

  if (aud_ui_button(fold, t->collapsed ? "v" : "^", AUD_UI_ACCENT, enabled))
  {
    t->collapsed = !t->collapsed;
  }

  {
    char name[AUD_TRACK_NAME_MAX + 8];

    snprintf(name, sizeof(name), "%s", t->name);
    aud_ui_text(close.x + 24.0f, panel.y + 8.0f, TL_LABEL_FONT,
                t->selected ? AUD_UI_TEXT : AUD_UI_MUTED, name);
  }

  if (t->collapsed || panel.height < 74.0f)
  {
    return;
  }

  {
    Rectangle mute = {panel.x + 8.0f, y, 52.0f, 22.0f};
    Rectangle solo = {panel.x + 66.0f, y, 52.0f, 22.0f};

    if (aud_ui_toggle(mute, "Mute", t->muted, AUD_UI_WARN, enabled))
    {
      t->muted = !t->muted;
    }
    if (aud_ui_toggle(solo, "Solo", t->soloed, AUD_UI_OK, enabled))
    {
      t->soloed = !t->soloed;
    }
    y += 28.0f;
  }

  if (panel.height < 108.0f)
  {
    return;
  }

  {
    Rectangle gain = {panel.x + 20.0f, y, panel.width - 40.0f, 18.0f};

    aud_ui_text(panel.x + 6.0f, y + 2.0f, 12, AUD_UI_MUTED, "-");
    aud_ui_text_right(panel.x + panel.width - 4.0f, y + 2.0f, 12, AUD_UI_MUTED, "+");
    aud_ui_slider(gain, &t->gain, 0.0f, 2.0f, AUD_UI_ACCENT, enabled);
    y += 22.0f;
  }

  if (panel.height < 132.0f)
  {
    return;
  }

  {
    Rectangle pan = {panel.x + 20.0f, y, panel.width - 40.0f, 18.0f};

    aud_ui_text(panel.x + 6.0f, y + 2.0f, 12, AUD_UI_MUTED, "L");
    aud_ui_text_right(panel.x + panel.width - 4.0f, y + 2.0f, 12, AUD_UI_MUTED, "R");
    aud_ui_slider(pan, &t->pan, -1.0f, 1.0f, AUD_UI_ACCENT, enabled);
  }

  (void)tl;
}

static int track_row_height(const aud_track *t)
{
  return t->collapsed ? AUD_TRACK_HEIGHT_COLLAPSED : t->height;
}

/* The horizontal scrollbar under the tracks, and the drag that moves it. */
static void draw_hbar(aud_timeline *tl, const aud_doc *d, Rectangle area, int enabled)
{
  Rectangle track_bar;
  Rectangle grip;
  double total;
  double visible;
  Rectangle wave = wave_bounds(area);

  total = d->rate > 0 ? (double)aud_doc_end(d) / d->rate : 0.0;
  visible = (double)wave.width / tl->zoom;
  if (total < tl->scroll + visible)
  {
    total = tl->scroll + visible;
  }
  if (total <= 0.0)
  {
    return;
  }

  track_bar.x = wave.x;
  track_bar.width = wave.width;
  track_bar.height = TL_BAR_H;
  track_bar.y = area.y + area.height - TL_BAR_H;

  DrawRectangleRec(track_bar, AUD_UI_BG);

  grip = track_bar;
  grip.width = (float)(visible / total) * track_bar.width;
  if (grip.width < TL_BAR_MIN)
  {
    grip.width = TL_BAR_MIN;
  }
  grip.x = track_bar.x + (float)(tl->scroll / total) * (track_bar.width - grip.width);
  grip.y += 2.0f;
  grip.height -= 4.0f;

  DrawRectangleRounded(grip, 1.0f, 6, hovering_bar(grip) ? AUD_UI_MUTED : AUD_UI_EDGE);

  if (enabled && CheckCollisionPointRec(GetMousePosition(), track_bar) &&
      IsMouseButtonDown(MOUSE_BUTTON_LEFT))
  {
    float want = GetMousePosition().x - track_bar.x - grip.width / 2.0f;
    double span = track_bar.width - grip.width;

    if (span > 1.0)
    {
      tl->scroll = (double)want / span * total;
      if (tl->scroll < 0.0)
      {
        tl->scroll = 0.0;
      }
    }
  }
}

void aud_timeline_draw(aud_timeline *tl, aud_doc *d, Rectangle ruler, Rectangle area,
                       uint64_t playhead, int playing, int enabled)
{
  Rectangle wave = wave_bounds(area);
  Rectangle rows = area;
  float y;
  float total_h = 0.0f;

  tl->hint[0] = '\0';

  draw_ruler(tl, d, ruler, playhead, enabled);

  DrawRectangleRec(area, AUD_UI_BG);

  rows.height -= TL_BAR_H;
  if (rows.height < 1.0f)
  {
    rows.height = 1.0f;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    total_h += (float)track_row_height(&d->tracks[i]) + 1.0f;
  }

  /* the wheel walks the stack when there is more of it than there is room */
  if (enabled && CheckCollisionPointRec(GetMousePosition(), rows))
  {
    float wheel = GetMouseWheelMove();

    if (wheel != 0.0f)
    {
      if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
      {
        double at = aud_timeline_seconds_at(tl, GetMousePosition().x - wave.x);

        aud_timeline_zoom_at(
            tl, wheel > 0.0f ? AUD_TIMELINE_ZOOM_STEP : 1.0 / AUD_TIMELINE_ZOOM_STEP, at,
            wave.width);
      }
      else if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
      {
        tl->scroll -= (double)wheel * 60.0 / tl->zoom;
        if (tl->scroll < 0.0)
        {
          tl->scroll = 0.0;
        }
      }
      else
      {
        tl->rows -= wheel * 48.0f;
      }
    }
  }

  if (tl->rows > total_h - rows.height)
  {
    tl->rows = total_h - rows.height;
  }
  if (tl->rows < 0.0f)
  {
    tl->rows = 0.0f;
  }

  BeginScissorMode((int)rows.x, (int)rows.y, (int)rows.width, (int)rows.height);

  y = rows.y - tl->rows;
  for (size_t i = 0; i < d->count; i++)
  {
    aud_track *t = &d->tracks[i];
    float h = (float)track_row_height(t);
    Rectangle panel = {rows.x, y, AUD_TIMELINE_PANEL_W, h};
    Rectangle scale = {rows.x + AUD_TIMELINE_PANEL_W, y, AUD_TIMELINE_SCALE_W, h};
    Rectangle lane = {wave.x, y, wave.width, h};
    size_t before = d->count;

    if (y > rows.y + rows.height || y + h < rows.y)
    {
      y += h + 1.0f;
      continue; /* off screen; nothing to draw and nothing to click */
    }

    draw_panel(d, i, panel, enabled, tl);
    if (d->count != before)
    {
      break; /* the track was closed underneath us; the list has moved */
    }
    t = &d->tracks[i];

    if (!t->collapsed)
    {
      draw_scale(scale, (int)t->channels);
    }
    else
    {
      DrawRectangleRec(scale, AUD_UI_PANEL);
    }

    DrawRectangleRec(lane, t->selected ? Fade(AUD_UI_ACCENT, 0.05f) : BLACK);

    /* the selected range, behind the waveform so the waveform stays readable */
    if (aud_doc_has_range(d) && t->selected && d->rate > 0)
    {
      float from = lane.x + aud_timeline_x_of(tl, (double)d->sel_start / d->rate);
      float to = lane.x + aud_timeline_x_of(tl, (double)d->sel_end / d->rate);

      from = from < lane.x ? lane.x : from;
      to = to > lane.x + lane.width ? lane.x + lane.width : to;
      if (to > from)
      {
        DrawRectangleRec((Rectangle){from, lane.y, to - from, lane.height},
                         Fade(AUD_UI_ACCENT, 0.20f));
      }
    }

    if (!t->collapsed && d->rate > 0)
    {
      float per = lane.height / (float)t->channels;

      for (unsigned ch = 0; ch < t->channels; ch++)
      {
        Rectangle one = {lane.x, lane.y + per * (float)ch, lane.width, per};

        draw_wave(t, ch, one, d->rate, tl, t->selected, d->sel_start, d->sel_end);
        if (ch > 0)
        {
          DrawLine((int)one.x, (int)one.y, (int)(one.x + one.width), (int)one.y,
                   Fade(AUD_UI_EDGE, 0.7f));
        }
      }
      draw_clip_edges(t, lane, d->rate, tl);
    }

    DrawLine((int)lane.x, (int)(y + h), (int)(lane.x + lane.width), (int)(y + h),
             AUD_UI_EDGE);

    /* dragging the bottom edge makes a lane taller, which is the only way to
     * see detail in one take without shrinking the window around it */
    {
      Rectangle grip = {rows.x, y + h - TL_RESIZE_GRIP, rows.width,
                        TL_RESIZE_GRIP * 2.0f};

      if (enabled && !t->collapsed && CheckCollisionPointRec(GetMousePosition(), grip))
      {
        SetMouseCursor(MOUSE_CURSOR_RESIZE_NS);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
          tl->resizing = 1;
          tl->resize_track = i;
          tl->resize_from_h = t->height;
          tl->resize_from_y = GetMousePosition().y;
        }
      }
    }

    /* a click in the waveform picks the track and starts a selection */
    if (enabled && !tl->resizing && !tl->scrubbing &&
        CheckCollisionPointRec(GetMousePosition(), lane) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
      if (!IsKeyDown(KEY_LEFT_CONTROL) && !IsKeyDown(KEY_RIGHT_CONTROL))
      {
        aud_doc_select_tracks(d, 0);
      }
      t->selected = 1;
      tl->anchor = frame_at(tl, d, GetMousePosition().x - lane.x);
      tl->selecting = 1;
      aud_doc_set_cursor(d, tl->anchor);
    }

    if (enabled && CheckCollisionPointRec(GetMousePosition(), lane) && !tl->selecting)
    {
      snprintf(tl->hint, sizeof(tl->hint), "click and drag to select audio");
    }

    y += h + 1.0f;
  }

  /* the cursor and the playhead, over every lane at once */
  if (d->rate > 0)
  {
    float cursor_x = wave.x + aud_timeline_x_of(tl, (double)d->cursor / d->rate);
    float head_x = wave.x + aud_timeline_x_of(tl, (double)playhead / d->rate);

    if (cursor_x >= wave.x && cursor_x <= wave.x + wave.width)
    {
      DrawLine((int)cursor_x, (int)rows.y, (int)cursor_x, (int)(rows.y + rows.height),
               Fade(AUD_UI_ACCENT, 0.9f));
    }
    if (playing && head_x >= wave.x && head_x <= wave.x + wave.width)
    {
      DrawLine((int)head_x, (int)rows.y, (int)head_x, (int)(rows.y + rows.height),
               AUD_UI_TEXT);
    }
  }

  EndScissorMode();

  /* a project with nothing in it says what to do about that */
  if (d->count == 0)
  {
    Rectangle line = rows;

    line.height = 24.0f;
    line.y = rows.y + rows.height / 2.0f - 24.0f;
    aud_ui_text_centred(line, 20, AUD_UI_MUTED, "no tracks yet");
    line.y += 28.0f;
    aud_ui_text_centred(line, 16, AUD_UI_EDGE,
                        "press Record to make one, or Import to open a WAV");
  }

  /* the drags, resolved after the lanes so they are not cut short by scrolling
   * out of the one they began in */
  if (tl->selecting)
  {
    uint64_t now = frame_at(tl, d, GetMousePosition().x - wave.x);

    aud_doc_select(d, tl->anchor, now);
    snprintf(tl->hint, sizeof(tl->hint), "selecting");

    /* dragging past either edge scrolls, so a selection can be longer than the
     * window is wide without letting go */
    if (GetMousePosition().x > wave.x + wave.width)
    {
      tl->scroll += 30.0 / tl->zoom;
    }
    else if (GetMousePosition().x < wave.x && tl->scroll > 0.0)
    {
      tl->scroll -= 30.0 / tl->zoom;
      if (tl->scroll < 0.0)
      {
        tl->scroll = 0.0;
      }
    }

    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
      tl->selecting = 0;
    }
  }

  if (tl->resizing)
  {
    if (tl->resize_track < d->count)
    {
      int h = tl->resize_from_h + (int)(GetMousePosition().y - tl->resize_from_y);

      if (h < AUD_TRACK_HEIGHT_MIN)
      {
        h = AUD_TRACK_HEIGHT_MIN;
      }
      if (h > AUD_TRACK_HEIGHT_MAX)
      {
        h = AUD_TRACK_HEIGHT_MAX;
      }
      d->tracks[tl->resize_track].height = h;
    }
    SetMouseCursor(MOUSE_CURSOR_RESIZE_NS);
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
      tl->resizing = 0;
    }
  }

  draw_hbar(tl, d, area, enabled);
}
