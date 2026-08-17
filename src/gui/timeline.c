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

/*
 * How close together the tempo grid is allowed to get before it thins itself
 * out: beats give way to bars, and bars double until they are far enough
 * apart to be lines rather than a wash. A ruler that drew every beat of a
 * three minute session would be a grey band.
 */
#define TL_GRID_BEAT_PX 9.0
#define TL_GRID_BAR_PX 26.0

/* The zoom at which there is room to number the beats as well as the bars. */
#define TL_GRID_LABEL_BEAT_PX 54.0

/* The tempo grid as it lands on screen, worked out once and drawn twice. */
typedef struct
{
  double beat; /* seconds to a beat */
  double bar;
  double step; /* seconds between the lines actually drawn */
  int per_bar;
  int beats;  /* non-zero when `step` is one beat rather than whole bars */
  int subdiv; /* lines to a beat when `step` is finer than one; 1 otherwise */
} tl_grid;

/*
 * Work out what the grid looks like at this zoom, or say there is none. No
 * grid is the answer whenever the switch is off, the project has no usable
 * tempo, or the view is so far out that even doubled bars would not separate.
 *
 * The lines drawn follow the division the project snaps to, but only as far as
 * there is room for: a sixteenth grid zoomed out to a whole session would be a
 * grey wash, so the subdivisions drop away first, then the beats, then the bars
 * double. What is drawn is always a multiple of what is snapped to, so a line
 * on screen is always somewhere an edit can land.
 */
static int grid_of(const aud_timeline *tl, const aud_doc *d, tl_grid *g)
{
  double frames = aud_doc_beat_frames(d);

  if (!tl->grid || frames <= 0.0 || d->rate == 0)
  {
    return 0;
  }

  g->per_bar = d->beats_per_bar < 2u ? 1 : (int)d->beats_per_bar;
  g->beat = frames / (double)d->rate;
  g->bar = g->beat * (double)g->per_bar;
  g->subdiv = 1;

  /*
   * Finer than a beat, when the project asks for it and the pixels allow it.
   * Halving down rather than straight to the division keeps every line drawn on
   * the snap grid: a third of a beat drawn at every other line would put lines
   * where nothing can land.
   */
  if (d->grid_div > AUD_DOC_GRID_BEAT)
  {
    for (int n = (int)d->grid_div; n > 1; n--)
    {
      if ((int)d->grid_div % n == 0 && g->beat / n * tl->zoom >= TL_GRID_BEAT_PX)
      {
        g->subdiv = n;
        g->step = g->beat / n;
        g->beats = 1;
        return 1;
      }
    }
  }

  if (g->beat * tl->zoom >= TL_GRID_BEAT_PX)
  {
    g->step = g->beat;
    g->beats = 1;
    return 1;
  }

  g->beats = 0;
  g->step = g->bar;
  while (g->step * tl->zoom < TL_GRID_BAR_PX)
  {
    g->step *= 2.0;
    /* an hour between lines is not a grid any more, whatever the arithmetic */
    if (g->step > 3600.0)
    {
      return 0;
    }
  }
  return 1;
}

/*
 * The beat a grid line falls on, counted from the start of the project, or -1
 * when the line sits between two beats because the grid is subdivided.
 *
 * From the line's index rather than from an accumulated total, for the reason
 * click.h counts its beats the same way: a session is thousands of beats long,
 * and adding a step at a time would have the grid and the metronome drift
 * apart over one.
 */
static long grid_beat_at(const tl_grid *g, long line)
{
  if (g->subdiv > 1)
  {
    return (line % g->subdiv) == 0 ? line / g->subdiv : -1;
  }
  return (long)floor((double)line * g->step / g->beat + 0.5);
}

/* Where the leftmost line on screen is, as an index into the grid. */
static long grid_first(const aud_timeline *tl, const tl_grid *g)
{
  double at = floor(tl->scroll / g->step);

  return at < 0.0 ? 0 : (long)at;
}

void aud_timeline_init(aud_timeline *tl)
{
  memset(tl, 0, sizeof(*tl));
  tl->zoom = AUD_TIMELINE_ZOOM_DEFAULT;
  tl->close_requested = -1;
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

static int track_row_height(const aud_track *t);

void aud_timeline_reveal_track(aud_timeline *tl, const aud_doc *d, size_t index,
                               float height)
{
  float top = 0.0f;
  float own;

  if (d == NULL || index >= d->count || height <= 1.0f)
  {
    return;
  }

  for (size_t i = 0; i < index; i++)
  {
    top += (float)track_row_height(&d->tracks[i]) + 1.0f;
  }
  own = (float)track_row_height(&d->tracks[index]) + 1.0f;

  if (top < tl->rows)
  {
    tl->rows = top;
  }
  else if (top + own > tl->rows + height)
  {
    tl->rows = top + own - height;
  }

  if (tl->rows < 0.0f)
  {
    tl->rows = 0.0f;
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

/* Where the pointer is, in frames, before anything is snapped to anything. */
static uint64_t frame_at_raw(const aud_timeline *tl, const aud_doc *d, float x)
{
  double seconds = aud_timeline_seconds_at(tl, x);

  if (seconds < 0.0)
  {
    return 0;
  }
  return (uint64_t)(seconds * d->rate + 0.5);
}

/* Whether the grid is on and the key that steps off it is not held down. */
static int snapping(const aud_timeline *tl)
{
  return tl->grid && !IsKeyDown(KEY_LEFT_ALT) && !IsKeyDown(KEY_RIGHT_ALT);
}

static uint64_t frame_at(const aud_timeline *tl, const aud_doc *d, float x)
{
  uint64_t frame = frame_at_raw(tl, d, x);

  /*
   * The one place the pointer becomes a position, so the one place snapping
   * belongs: clicking, scrubbing and dragging a selection all arrive here and
   * all land on the grid together. Alt steps off it - a cut that has to go
   * between two beats should not need the grid turned off and back on.
   *
   * A move is the exception, and snaps its landing edge rather than the pointer
   * - see move_offset(), which is where dragging a take onto the beat happens.
   */
  if (snapping(tl))
  {
    frame = aud_doc_snap(d, frame);
  }
  return frame;
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

/*
 * How far a move drag has come, in frames: from where it took hold to where the
 * pointer is now, held to the room the selection has to move in.
 *
 * What is snapped is the landing edge rather than the pointer, and that is the
 * whole difference between a grid that puts a take on the beat and one that
 * does not: snapping the pointer would move a take that came in late by a whole
 * number of beats and leave it exactly as late as it was.
 */
static int64_t move_offset(const aud_timeline *tl, const aud_doc *d, float x,
                           int64_t *asked)
{
  uint64_t now = frame_at_raw(tl, d, x);
  int64_t want = now >= tl->move_anchor ? (int64_t)(now - tl->move_anchor)
                                        : -(int64_t)(tl->move_anchor - now);

  /* the timeline has no frames before its first one to be dragged back into */
  if (want < 0 && (uint64_t)(-(want + 1)) + 1u > d->sel_start)
  {
    want = -(int64_t)d->sel_start;
  }

  if (snapping(tl))
  {
    uint64_t landing = aud_doc_snap(d, aud_frame_offset(d->sel_start, want));

    want = landing >= d->sel_start ? (int64_t)(landing - d->sel_start)
                                   : -(int64_t)(d->sel_start - landing);
  }

  *asked = want;
  return aud_edit_move_room(d, want);
}

/* Non-zero when the pointer is inside the selection, which is what takes hold. */
static int over_selection(const aud_timeline *tl, const aud_doc *d, Rectangle lane)
{
  uint64_t at;

  if (!aud_doc_has_range(d) || d->rate == 0 ||
      !CheckCollisionPointRec(GetMousePosition(), lane))
  {
    return 0;
  }

  at = frame_at_raw(tl, d, GetMousePosition().x - lane.x);
  return at >= d->sel_start && at < d->sel_end;
}

/*
 * Where the selection would land, over the lane it would land on.
 *
 * An outline rather than the audio itself: what moves is decided when the
 * button comes up, so until then this is a promise about where it is going and
 * drawing the waveform there would be a claim that it had already gone. A drag
 * that has run out of room sits over the selection in the colour of a warning,
 * which is the honest answer to a pointer that has gone further than the audio
 * can.
 */
static void draw_move_ghost(Rectangle lane, const aud_doc *d, const aud_timeline *tl)
{
  float from;
  float to;
  Rectangle box;
  Color tint = tl->move_blocked ? AUD_UI_WARN : AUD_UI_ACCENT;

  if (d->rate == 0)
  {
    return;
  }

  from = lane.x + aud_timeline_x_of(
                      tl, (double)aud_frame_offset(d->sel_start, tl->move_by) / d->rate);
  to = lane.x +
       aud_timeline_x_of(tl, (double)aud_frame_offset(d->sel_end, tl->move_by) / d->rate);

  if (to <= lane.x || from >= lane.x + lane.width)
  {
    return;
  }

  box.x = from < lane.x ? lane.x : from;
  box.y = lane.y + 1.0f;
  box.width = (to > lane.x + lane.width ? lane.x + lane.width : to) - box.x;
  box.height = lane.height - 2.0f;
  if (box.width < 1.0f)
  {
    box.width = 1.0f;
  }

  DrawRectangleRec(box, Fade(tint, 0.16f));
  DrawRectangleLinesEx(box, 1.0f, Fade(tint, 0.9f));
}

/* The ruler counted in minutes and seconds, which is what it says with no grid. */
static void draw_ruler_time(const aud_timeline *tl, Rectangle ruler, Rectangle strip)
{
  double step = pick_step(tl->zoom, 64.0);
  double first = floor(tl->scroll / step) * step;

  for (double t = first;; t += step)
  {
    float x = strip.x + aud_timeline_x_of(tl, t);
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
        float x = strip.x + aud_timeline_x_of(tl, t);

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
}

/*
 * The same ruler counted in bars, which is what it says once there is a grid.
 *
 * It replaces the clock rather than sitting beside it: a strip this tall
 * carrying two numbering schemes at once is one you have to read twice to find
 * out which one you are looking at. What the seconds were is on the status
 * line and under the playhead regardless.
 */
static void draw_ruler_bars(const aud_timeline *tl, const tl_grid *g, Rectangle ruler,
                            Rectangle strip)
{
  long first = grid_first(tl, g);
  int label_beats = g->beats && g->beat * tl->zoom >= TL_GRID_LABEL_BEAT_PX;

  for (long line = first;; line++)
  {
    double t = (double)line * g->step;
    float x = strip.x + aud_timeline_x_of(tl, t);
    long beat = grid_beat_at(g, line);
    int on_bar = beat >= 0 && (beat % g->per_bar) == 0;
    char text[48];
    float tick;

    if (x > strip.x + strip.width)
    {
      break;
    }
    if (x < strip.x)
    {
      continue;
    }

    /* three lengths for three kinds of line: bar, beat, and between beats */
    tick = on_bar ? 9.0f : (beat >= 0 ? 4.0f : 2.0f);
    DrawLine((int)x, (int)(ruler.y + ruler.height - tick), (int)x,
             (int)(ruler.y + ruler.height), on_bar ? AUD_UI_MUTED : AUD_UI_EDGE);

    /* bars and beats are counted from one, the way anybody playing counts them */
    if (on_bar)
    {
      snprintf(text, sizeof(text), "%ld", beat / g->per_bar + 1);
    }
    else if (label_beats && beat >= 0)
    {
      snprintf(text, sizeof(text), "%ld.%ld", beat / g->per_bar + 1,
               beat % g->per_bar + 1);
    }
    else
    {
      continue;
    }

    aud_ui_text(x + 4.0f, ruler.y + 4.0f, TL_RULER_FONT,
                on_bar ? AUD_UI_MUTED : AUD_UI_EDGE, text);
  }
}

static void draw_ruler(aud_timeline *tl, aud_doc *d, Rectangle ruler, uint64_t playhead,
                       int enabled)
{
  Rectangle strip = ruler;
  tl_grid grid;
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

  if (grid_of(tl, d, &grid))
  {
    draw_ruler_bars(tl, &grid, ruler, strip);
  }
  else
  {
    draw_ruler_time(tl, ruler, strip);
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

/*
 * The ramp itself, drawn as the line it is: from silence at one end of the
 * fade to full at the other, with the space it takes out of the waveform
 * shaded. The waveform underneath already follows the fade - see
 * aud_track_range() - so this says where the ramp was asked for rather than
 * what it did, which is what you need to adjust one.
 */
static void draw_fade(Rectangle lane, float from_x, float to_x, int rising)
{
  Vector2 left = {from_x, lane.y};
  Vector2 right = {to_x, lane.y};
  Vector2 foot;
  Vector2 head;

  if (to_x <= from_x || to_x < lane.x || from_x > lane.x + lane.width)
  {
    return;
  }

  /* the ramp runs from silence at the floor to full at the ceiling */
  foot.x = rising ? from_x : to_x;
  foot.y = lane.y + lane.height;
  head.x = rising ? to_x : from_x;
  head.y = lane.y;

  /*
   * The corner the ramp cuts off, shaded. Wound so the three vertices have a
   * negative cross product in screen space, which is the order raylib draws
   * rather than culls - the same rule the dropdown's caret follows in ui.c.
   */
  DrawTriangle(left, foot, right, Fade(BLACK, 0.35f));
  DrawLineEx(foot, head, 1.5f, Fade(AUD_UI_WARN, 0.85f));
}

static void draw_fades(const aud_track *t, Rectangle lane, unsigned rate,
                       const aud_timeline *tl)
{
  for (size_t i = 0; i < t->count; i++)
  {
    const aud_clip *c = &t->clips[i];

    if (c->fade_in > 0)
    {
      float a = lane.x + aud_timeline_x_of(tl, (double)c->start / rate);
      float b = lane.x + aud_timeline_x_of(tl, (double)(c->start + c->fade_in) / rate);

      draw_fade(lane, a, b, 1);
    }
    if (c->fade_out > 0)
    {
      uint64_t end = c->start + c->frames;
      float a = lane.x + aud_timeline_x_of(tl, (double)(end - c->fade_out) / rate);
      float b = lane.x + aud_timeline_x_of(tl, (double)end / rate);

      draw_fade(lane, a, b, 0);
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
    /* asked for, not done: see aud_timeline.close_requested */
    tl->close_requested = (long)index;
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

  /*
   * Before the lanes rather than after them, so the outline drawn on each lane
   * is where the pointer is now and not where it was a frame ago.
   */
  if (tl->moving)
  {
    int64_t asked = 0;

    tl->move_by = move_offset(tl, d, GetMousePosition().x - wave.x, &asked);
    tl->move_blocked = asked != 0 && tl->move_by == 0;
  }

  draw_ruler(tl, d, ruler, playhead, enabled);

  DrawRectangleRec(area, AUD_UI_BG);

  rows.height -= TL_BAR_H;
  if (rows.height < 1.0f)
  {
    rows.height = 1.0f;
  }
  tl->rows_h = rows.height;
  tl->wave_w = wave.width;

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
    int grab;

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
      draw_fades(t, lane, d->rate, tl);
      draw_clip_edges(t, lane, d->rate, tl);
    }

    if (tl->moving && t->selected)
    {
      draw_move_ghost(lane, d, tl);
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

    /*
     * A press inside the selection takes hold of it rather than starting
     * another one, the way dragging selected text moves it rather than
     * reselecting it. Ctrl is left out of it: that is how a second lane is
     * added to the selection, and adding one is not grabbing it.
     */
    grab = enabled && !tl->resizing && !tl->scrubbing && !tl->selecting && !tl->moving &&
           t->selected && !IsKeyDown(KEY_LEFT_CONTROL) && !IsKeyDown(KEY_RIGHT_CONTROL) &&
           over_selection(tl, d, lane);

    if (grab)
    {
      SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
      {
        tl->moving = 1;
        tl->move_anchor = frame_at_raw(tl, d, GetMousePosition().x - lane.x);
        tl->move_by = 0;
      }
    }

    /* a click in the waveform picks the track and starts a selection */
    if (enabled && !tl->resizing && !tl->scrubbing && !tl->moving &&
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

    if (enabled && CheckCollisionPointRec(GetMousePosition(), lane) && !tl->selecting &&
        !tl->moving)
    {
      snprintf(tl->hint, sizeof(tl->hint),
               grab ? "drag to move the selection along the lane"
                    : "click and drag to select audio");
    }

    y += h + 1.0f;
  }

  /*
   * The grid, over every lane at once and under the cursor.
   *
   * Over the waveforms rather than behind them: each lane paints its own
   * background, and a line drawn before that is a line nobody sees. Faint
   * enough that the audio stays the thing being looked at.
   */
  {
    tl_grid grid;

    if (grid_of(tl, d, &grid))
    {
      for (long line = grid_first(tl, &grid);; line++)
      {
        double t = (double)line * grid.step;
        float x = wave.x + aud_timeline_x_of(tl, t);
        long beat = grid_beat_at(&grid, line);
        int on_bar = beat >= 0 && (beat % grid.per_bar) == 0;

        if (x > wave.x + wave.width)
        {
          break;
        }
        if (x < wave.x)
        {
          continue;
        }
        /* the same three weights the ruler draws, so the two agree at a glance */
        DrawLine((int)x, (int)rows.y, (int)x, (int)(rows.y + rows.height),
                 Fade(AUD_UI_EDGE, on_bar ? 0.85f : (beat >= 0 ? 0.35f : 0.18f)));
      }
    }
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

  if (tl->moving)
  {
    SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);

    if (tl->move_blocked)
    {
      snprintf(tl->hint, sizeof(tl->hint), "no room to move it that way");
    }
    else
    {
      snprintf(tl->hint, sizeof(tl->hint), "moving %+.3f s",
               d->rate > 0 ? (double)tl->move_by / d->rate : 0.0);
    }

    /* the same scroll a selection drag gets, so a take can be moved further
     * than the window is wide without letting go of it */
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
      /*
       * A press that never went anywhere is a click, and a click inside a
       * selection is how anyone puts the cursor down in the middle of one -
       * the grab intercepted it, so it is answered here rather than lost.
       */
      if (tl->move_by == 0 &&
          frame_at_raw(tl, d, GetMousePosition().x - wave.x) == tl->move_anchor)
      {
        aud_doc_set_cursor(d, frame_at(tl, d, GetMousePosition().x - wave.x));
      }
      else
      {
        /* asked for, not done: see aud_timeline.move_requested */
        tl->move_requested = tl->move_by;
      }

      tl->moving = 0;
      tl->move_by = 0;
      tl->move_blocked = 0;
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
