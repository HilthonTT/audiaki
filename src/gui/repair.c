/* SPDX-License-Identifier: MIT */
#include "gui/repair.h"

#include "gui/ui.h"

#include "edit/repair.h"
#include "util/log.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Room the two rows of controls take under the graph. */
#define REPAIR_ROW_H 24.0f
#define REPAIR_ROW_GAP 6.0f
#define REPAIR_CONTROLS_H (REPAIR_ROW_H * 2.0f + REPAIR_ROW_GAP * 3.0f)

/* Below this there is no graph worth drawing, so the panel says so instead. */
#define REPAIR_MIN_GRAPH_H 70.0f
#define REPAIR_MIN_GRAPH_W 260.0f

/* The strip of dB labels down the left of the graph. */
#define REPAIR_SCALE_W 40.0f

/* The gap between grid labels, in dB. */
#define REPAIR_DB_STEP 20.0f

static const double repair_grid_hz[] = {20.0,   30.0,    50.0,   100.0,  200.0,
                                        300.0,  500.0,   1000.0, 2000.0, 3000.0,
                                        5000.0, 10000.0, 20000.0};

#define REPAIR_GRID_COUNT ((int)(sizeof(repair_grid_hz) / sizeof(repair_grid_hz[0])))

void aud_repair_panel_init(aud_repair_panel *p)
{
  if (p == NULL)
  {
    return;
  }

  memset(p, 0, sizeof(*p));
  p->brush = AUD_REPAIR_BRUSH_DEFAULT;
  p->harmonics = (float)AUD_REPAIR_HARMONICS_DEFAULT;
  snprintf(p->note, sizeof(p->note), "select a take and drag the spike down");
}

void aud_repair_panel_free(aud_repair_panel *p)
{
  if (p == NULL)
  {
    return;
  }

  aud_spectral_destroy(p->sp);
  free(p->result);
  p->sp = NULL;
  p->result = NULL;
  p->result_bins = 0;
  p->have = 0;
}

void aud_repair_panel_reset(aud_repair_panel *p)
{
  if (p != NULL)
  {
    p->have = 0;
    p->pending = 0;
  }
}

static void note(aud_repair_panel *p, const char *fmt, ...) AUD_PRINTF(2, 3);

static void note(aud_repair_panel *p, const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  vsnprintf(p->note, sizeof(p->note), fmt, args);
  va_end(args);
}

/* -- what is being looked at ------------------------------------------------ */

/*
 * The lane the panel is working on: the first selected one, or - with nothing
 * selected - the first that has any audio in it. Returns d->count for none.
 *
 * Falling back rather than refusing, because "clean up the take I just
 * recorded" should not need a lane to be clicked first when there is only one.
 * Which lane it settled on is written above the graph either way.
 */
static size_t working_track(const aud_doc *d)
{
  for (size_t i = 0; i < d->count; i++)
  {
    if (d->tracks[i].selected && aud_track_end(&d->tracks[i]) > 0)
    {
      return i;
    }
  }
  for (size_t i = 0; i < d->count; i++)
  {
    if (aud_track_end(&d->tracks[i]) > 0)
    {
      return i;
    }
  }
  return d->count;
}

/* The range on that lane: the selection, or all of it when there is none. */
static void working_range(const aud_doc *d, size_t index, uint64_t *from, uint64_t *to)
{
  uint64_t end = aud_track_end(&d->tracks[index]);

  if (aud_doc_has_range(d))
  {
    *from = d->sel_start;
    *to = d->sel_end < end ? d->sel_end : end;
    if (*to > *from)
    {
      return;
    }
  }

  *from = 0;
  *to = end;
}

/*
 * Take the reading again when what it was taken from has moved, once the
 * moving has stopped. See AUD_REPAIR_SETTLE.
 */
static void refresh(aud_repair_panel *p, aud_doc *d, size_t index, uint64_t from,
                    uint64_t to)
{
  size_t edits = d->undo_count + d->redo_count;
  size_t clips = d->tracks[index].count;
  int same = p->have && p->track == index && p->from == from && p->to == to &&
             p->edits == edits && p->clips == clips;

  if (same)
  {
    p->pending = 0;
    return;
  }

  if (!p->pending)
  {
    p->pending = 1;
    p->settle_at = GetTime() + AUD_REPAIR_SETTLE;
    /* nothing has been read yet, so the first look is not made to wait */
    if (!p->have)
    {
      p->settle_at = 0.0;
    }
    return;
  }

  if (GetTime() < p->settle_at)
  {
    return;
  }

  p->pending = 0;

  if (aud_repair_read(d, index, from, to, p->sp) != 0)
  {
    p->have = 0;
    return;
  }

  p->have = 1;
  p->track = index;
  p->from = from;
  p->to = to;
  p->edits = edits;
  p->clips = clips;
}

/* -- the graph -------------------------------------------------------------- */

static float x_of_hz(Rectangle g, double hz, double top_hz)
{
  double t;

  if (!(hz > AUD_REPAIR_MIN_HZ))
  {
    return g.x;
  }
  t = log(hz / AUD_REPAIR_MIN_HZ) / log(top_hz / AUD_REPAIR_MIN_HZ);
  if (t > 1.0)
  {
    t = 1.0;
  }
  return g.x + (float)(t * (double)g.width);
}

static double hz_of_x(Rectangle g, float x, double top_hz)
{
  double t = (double)(x - g.x) / (double)g.width;

  if (t < 0.0)
  {
    t = 0.0;
  }
  if (t > 1.0)
  {
    t = 1.0;
  }
  return AUD_REPAIR_MIN_HZ * pow(top_hz / AUD_REPAIR_MIN_HZ, t);
}

static float y_of_db(Rectangle g, float db)
{
  float t;

  if (db > AUD_REPAIR_TOP_DB)
  {
    db = AUD_REPAIR_TOP_DB;
  }
  if (db < AUD_REPAIR_FLOOR_DB)
  {
    db = AUD_REPAIR_FLOOR_DB;
  }
  t = (AUD_REPAIR_TOP_DB - db) / (AUD_REPAIR_TOP_DB - AUD_REPAIR_FLOOR_DB);
  return g.y + t * g.height;
}

static float db_of_y(Rectangle g, float y)
{
  float t = (y - g.y) / g.height;

  if (t < 0.0f)
  {
    t = 0.0f;
  }
  if (t > 1.0f)
  {
    t = 1.0f;
  }
  return AUD_REPAIR_TOP_DB - t * (AUD_REPAIR_TOP_DB - AUD_REPAIR_FLOOR_DB);
}

/* A frequency written the way anyone would say it: "50 Hz", "2.5k". */
static void say_hz(char *dst, size_t size, double hz)
{
  if (hz >= 10000.0)
  {
    snprintf(dst, size, "%.0fk", hz / 1000.0);
  }
  else if (hz >= 1000.0)
  {
    snprintf(dst, size, "%.1fk", hz / 1000.0);
  }
  else if (hz >= 100.0)
  {
    snprintf(dst, size, "%.0f", hz);
  }
  else
  {
    snprintf(dst, size, "%.1f", hz);
  }
}

static void draw_grid(Rectangle g, double top_hz)
{
  for (float db = AUD_REPAIR_TOP_DB; db >= AUD_REPAIR_FLOOR_DB; db -= REPAIR_DB_STEP)
  {
    float y = y_of_db(g, db);
    char label[16];

    DrawLine((int)g.x, (int)y, (int)(g.x + g.width), (int)y, AUD_UI_EDGE);
    snprintf(label, sizeof(label), "%.0f", (double)db);
    aud_ui_text_right(g.x - 6.0f, y - 6.0f, 11, AUD_UI_MUTED, label);
  }

  for (int i = 0; i < REPAIR_GRID_COUNT; i++)
  {
    float x;
    char label[16];

    if (repair_grid_hz[i] > top_hz)
    {
      break;
    }
    x = x_of_hz(g, repair_grid_hz[i], top_hz);
    DrawLine((int)x, (int)g.y, (int)x, (int)(g.y + g.height), AUD_UI_EDGE);

    say_hz(label, sizeof(label), repair_grid_hz[i]);
    aud_ui_text(x + 3.0f, g.y + g.height - 14.0f, 11, AUD_UI_MUTED, label);
  }
}

/*
 * The loudest bin any given pixel column covers.
 *
 * A column at the top of the range covers dozens of bins and one at the bottom
 * covers a fraction of one, so a trace drawn bin by bin would be a scribble at
 * one end and a staircase at the other. Taking the maximum is what a spectrum
 * analyser does, and it means a single-bin spike a pixel wide still reaches its
 * full height rather than being averaged into invisibility - which matters
 * here, a single-bin spike being exactly what this panel is for.
 */
static float column_max(const aud_spectral *s, const float *v, double f0, double f1)
{
  size_t k0 = aud_spectral_bin_at(s, f0);
  size_t k1 = aud_spectral_bin_at(s, f1);
  size_t bins = aud_spectral_bins(s);
  float best;

  if (k1 <= k0)
  {
    k1 = k0 + 1u;
  }
  if (k1 > bins)
  {
    k1 = bins;
  }

  best = 0.0f;
  for (size_t k = k0; k < k1; k++)
  {
    if (v[k] > best)
    {
      best = v[k];
    }
  }
  return best;
}

/* The smallest, for the curve: a notch a pixel wide must not be smoothed away. */
static float column_min(const aud_spectral *s, const float *v, double f0, double f1)
{
  size_t k0 = aud_spectral_bin_at(s, f0);
  size_t k1 = aud_spectral_bin_at(s, f1);
  size_t bins = aud_spectral_bins(s);
  float best;

  if (k1 <= k0)
  {
    k1 = k0 + 1u;
  }
  if (k1 > bins)
  {
    k1 = bins;
  }

  best = v[k0 < bins ? k0 : bins - 1u];
  for (size_t k = k0; k < k1; k++)
  {
    if (v[k] < best)
    {
      best = v[k];
    }
  }
  return best;
}

/* One trace, a pixel column at a time. `fill` shades down to the floor. */
static void draw_trace(const aud_spectral *s, const float *v, Rectangle g, double top_hz,
                       Color colour, int fill)
{
  float previous = -1.0f;

  for (int px = 0; px < (int)g.width; px++)
  {
    double f0 = hz_of_x(g, g.x + (float)px, top_hz);
    double f1 = hz_of_x(g, g.x + (float)px + 1.0f, top_hz);
    float y = y_of_db(g, aud_spectral_db(column_max(s, v, f0, f1)));
    float x = g.x + (float)px;

    if (fill)
    {
      DrawLine((int)x, (int)y, (int)x, (int)(g.y + g.height), colour);
    }
    else if (previous >= 0.0f)
    {
      DrawLine((int)(x - 1.0f), (int)previous, (int)x, (int)y, colour);
    }
    previous = y;
  }
}

/*
 * What is being taken out, shaded over the whole height of the graph.
 *
 * The traces say what the audio holds; this says what the edit does to it, and
 * it has to be readable at a glance from across the room - a notch you cannot
 * see is a notch you forget you left in.
 */
static void draw_cut(const aud_spectral *s, Rectangle g, double top_hz)
{
  const float *curve = aud_spectral_curve(s);

  for (int px = 0; px < (int)g.width; px++)
  {
    double f0 = hz_of_x(g, g.x + (float)px, top_hz);
    double f1 = hz_of_x(g, g.x + (float)px + 1.0f, top_hz);
    float gain = column_min(s, curve, f0, f1);
    float depth;

    if (gain >= 0.999f)
    {
      continue;
    }

    depth = 1.0f - gain;
    DrawLine((int)(g.x + (float)px), (int)g.y, (int)(g.x + (float)px),
             (int)(g.y + g.height), Fade(AUD_UI_RECORD, 0.10f + 0.35f * depth));
  }
}

/* -- drawing on it ---------------------------------------------------------- */

/* The band the brush covers around `hz`, at least a few bins wide. */
static void brush_span(const aud_repair_panel *p, double hz, double *lo, double *hi)
{
  double half = pow(2.0, (double)p->brush / 2.0);
  double bin_hz = (double)aud_spectral_rate(p->sp) / (double)aud_spectral_size(p->sp);
  double least = bin_hz * 1.5;

  *lo = hz / half;
  *hi = hz * half;

  if (*hi - *lo < least * 2.0)
  {
    *lo = hz - least;
    *hi = hz + least;
  }
}

/*
 * Carry out a stroke from wherever the last frame left the pointer to where it
 * is now, so a fast drag paints a band rather than a row of dots.
 */
static void stroke(aud_repair_panel *p, double hz, float magnitude, int lift)
{
  double lo;
  double hi;
  double from_lo;
  double from_hi;

  brush_span(p, hz, &lo, &hi);

  if (p->last_hz > 0.0)
  {
    brush_span(p, p->last_hz, &from_lo, &from_hi);
    if (from_lo < lo)
    {
      lo = from_lo;
    }
    if (from_hi > hi)
    {
      hi = from_hi;
    }
  }

  if (lift)
  {
    aud_spectral_paint(p->sp, lo, hi, 1.0f);
  }
  else
  {
    aud_spectral_pull_down(p->sp, lo, hi, magnitude);
  }

  p->last_hz = hz;
}

static void handle_graph(aud_repair_panel *p, Rectangle g, double top_hz, int enabled)
{
  Vector2 mouse = GetMousePosition();
  int over = enabled && CheckCollisionPointRec(mouse, g);
  double hz;
  float db;

  if (over)
  {
    float wheel = GetMouseWheelMove();

    if (wheel != 0.0f)
    {
      p->brush *= wheel > 0.0f ? 1.15f : 1.0f / 1.15f;
      if (p->brush < AUD_REPAIR_BRUSH_MIN)
      {
        p->brush = AUD_REPAIR_BRUSH_MIN;
      }
      if (p->brush > AUD_REPAIR_BRUSH_MAX)
      {
        p->brush = AUD_REPAIR_BRUSH_MAX;
      }
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
      p->painting = 1;
      p->last_hz = 0.0;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    {
      p->lifting = 1;
      p->last_hz = 0.0;
    }
  }

  /*
   * Held until the button is let go, wherever the pointer wanders to - a
   * stroke that stopped following the moment it left the graph would be worse
   * than one that clamps to its edge, which is what this does.
   */
  if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) || !enabled)
  {
    if (p->painting)
    {
      p->painting = 0;
      p->last_hz = 0.0;
    }
  }
  if (!IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || !enabled)
  {
    if (p->lifting)
    {
      p->lifting = 0;
      p->last_hz = 0.0;
    }
  }

  if (!p->painting && !p->lifting)
  {
    if (over)
    {
      char label[64];
      char freq[16];

      hz = hz_of_x(g, mouse.x, top_hz);
      db = db_of_y(g, mouse.y);
      say_hz(freq, sizeof(freq), hz);
      snprintf(label, sizeof(label), "%s Hz   %.0f dB", freq, (double)db);
      aud_ui_text_right(g.x + g.width - 8.0f, g.y + 6.0f, 12, AUD_UI_TEXT, label);

      /* the brush, so its width is something you can see rather than guess */
      {
        double lo;
        double hi;

        brush_span(p, hz, &lo, &hi);
        DrawRectangleRec((Rectangle){x_of_hz(g, lo, top_hz), g.y,
                                     x_of_hz(g, hi, top_hz) - x_of_hz(g, lo, top_hz),
                                     g.height},
                         Fade(AUD_UI_ACCENT, 0.12f));
        DrawLine((int)mouse.x, (int)g.y, (int)mouse.x, (int)(g.y + g.height),
                 Fade(AUD_UI_ACCENT, 0.4f));
        DrawLine((int)g.x, (int)mouse.y, (int)(g.x + g.width), (int)mouse.y,
                 Fade(AUD_UI_ACCENT, 0.4f));
      }
    }
    return;
  }

  hz = hz_of_x(g, mouse.x, top_hz);
  db = db_of_y(g, mouse.y);
  stroke(p, hz, (float)pow(10.0, (double)db / 20.0), p->lifting);
}

/* -- the controls ----------------------------------------------------------- */

/* One slot of a row of `count` equal ones, with a gap between them. */
static Rectangle slot(Rectangle row, float width, float *x)
{
  Rectangle r = {*x, row.y, width, row.height};

  *x += width + 6.0f;
  return r;
}

/* A slider with its name and reading above it, which is all of them here. */
static int labelled_slider(Rectangle r, const char *name, const char *reading,
                           float *value, float lo, float hi, int enabled)
{
  Rectangle bar = r;
  int changed;

  bar.y += 10.0f;
  bar.height -= 10.0f;

  aud_ui_text(r.x, r.y - 1.0f, 10, AUD_UI_MUTED, name);
  aud_ui_text_right(r.x + r.width, r.y - 1.0f, 10, AUD_UI_TEXT, reading);
  changed = aud_ui_slider(bar, value, lo, hi, AUD_UI_ACCENT, enabled);
  return changed;
}

int aud_repair_panel_draw(aud_repair_panel *p, aud_doc *d, Rectangle area,
                          const char *dir, int enabled)
{
  Rectangle graph;
  Rectangle row_one;
  Rectangle row_two;
  size_t index;
  uint64_t from = 0;
  uint64_t to = 0;
  double top_hz;
  int changed = 0;
  int usable;
  float x;

  if (p == NULL || d == NULL)
  {
    return 0;
  }

  DrawRectangleRec(area, AUD_UI_PANEL);
  DrawRectangleLinesEx(area, 1.0f, AUD_UI_EDGE);

  if (d->rate == 0 || d->count == 0)
  {
    aud_ui_text_centred(area, 15, AUD_UI_MUTED,
                        "record or open something, and its spectrum turns up here");
    return 0;
  }

  index = working_track(d);
  if (index >= d->count)
  {
    aud_ui_text_centred(area, 15, AUD_UI_MUTED, "no audio on any track yet");
    return 0;
  }

  /* The analyser belongs to the project's rate, and a session can change it. */
  if (p->sp != NULL && aud_spectral_rate(p->sp) != d->rate)
  {
    aud_repair_panel_free(p);
  }
  if (p->sp == NULL)
  {
    p->sp = aud_spectral_create(d->rate, AUD_SPECTRAL_FFT);
    if (p->sp == NULL)
    {
      aud_ui_text_centred(area, 15, AUD_UI_WARN, "not enough memory to analyse");
      return 0;
    }
    p->have = 0;
  }

  working_range(d, index, &from, &to);
  refresh(p, d, index, from, to);

  if (p->result_bins != aud_spectral_bins(p->sp))
  {
    free(p->result);
    p->result_bins = aud_spectral_bins(p->sp);
    p->result = calloc(p->result_bins, sizeof(*p->result));
    if (p->result == NULL)
    {
      p->result_bins = 0;
    }
  }

  graph = area;
  graph.x += REPAIR_SCALE_W + 8.0f;
  graph.y += 24.0f;
  graph.width -= REPAIR_SCALE_W + 20.0f;
  graph.height -= 24.0f + REPAIR_CONTROLS_H;

  /* the lane and the range, above the graph, because both are inferred */
  {
    char heading[192];
    char span[64];
    double seconds = d->rate > 0 ? (double)(to - from) / (double)d->rate : 0.0;

    if (aud_doc_has_range(d))
    {
      snprintf(span, sizeof(span), "%.1fs selected", seconds);
    }
    else
    {
      snprintf(span, sizeof(span), "all %.1fs of it", seconds);
    }

    snprintf(heading, sizeof(heading), "%s  -  %s", d->tracks[index].name, span);
    aud_ui_text(area.x + 8.0f, area.y + 5.0f, 13, AUD_UI_TEXT, heading);
    aud_ui_text_right(area.x + area.width - 8.0f, area.y + 5.0f, 12, AUD_UI_MUTED,
                      p->note);
  }

  if (graph.width < REPAIR_MIN_GRAPH_W || graph.height < REPAIR_MIN_GRAPH_H)
  {
    aud_ui_text_centred(area, 14, AUD_UI_MUTED, "the window is too small for the graph");
    return 0;
  }

  top_hz = (double)d->rate / 2.0;
  usable = enabled && p->have;

  DrawRectangleRec(graph, BLACK);
  draw_grid(graph, top_hz);

  if (!p->have)
  {
    aud_ui_text_centred(graph, 14, AUD_UI_MUTED, "reading the audio...");
  }
  else
  {
    /*
     * Loudest first and quietest last, so nothing important is drawn under
     * something faint: the envelope is background, what is being kept is the
     * line to read.
     */
    draw_trace(p->sp, aud_spectral_peak(p->sp), graph, top_hz, Fade(AUD_UI_MUTED, 0.20f),
               1);
    draw_trace(p->sp, aud_spectral_mean(p->sp), graph, top_hz, AUD_UI_MUTED, 0);

    if (aud_spectral_noise(p->sp) != NULL)
    {
      draw_trace(p->sp, aud_spectral_noise(p->sp), graph, top_hz, AUD_UI_WARN, 0);
    }

    draw_cut(p->sp, graph, top_hz);

    if (p->result != NULL)
    {
      aud_spectral_result(p->sp, p->result);
      draw_trace(p->sp, p->result, graph, top_hz, AUD_UI_ACCENT, 0);
    }

    handle_graph(p, graph, top_hz, usable);
  }

  DrawRectangleLinesEx(graph, 1.0f, AUD_UI_EDGE);

  /* -- the two rows of controls -- */

  row_one.x = area.x + 8.0f;
  row_one.width = area.width - 16.0f;
  row_one.height = REPAIR_ROW_H;
  row_one.y = area.y + area.height - REPAIR_CONTROLS_H + REPAIR_ROW_GAP;

  row_two = row_one;
  row_two.y = row_one.y + REPAIR_ROW_H + REPAIR_ROW_GAP;

  /* Row one: the hum, and the brush. */
  x = row_one.x;
  {
    Rectangle find = slot(row_one, 84.0f, &x);
    Rectangle reading = slot(row_one, 76.0f, &x);
    Rectangle harmonics = slot(row_one, 130.0f, &x);
    Rectangle notch = slot(row_one, 84.0f, &x);
    Rectangle brush = slot(row_one, 130.0f, &x);
    char label[32];

    if (aud_ui_button(find, "Find hum", AUD_UI_ACCENT, usable))
    {
      double found = aud_spectral_find_hum(p->sp);

      p->hum_hz = (float)found;
      if (found > 0.0)
      {
        note(p,
             "a steady tone at %.0f Hz - press Notch to take it and its "
             "harmonics out",
             found);
      }
      else
      {
        note(p, "nothing steady enough to call a hum; drag the graph instead");
      }
    }
    aud_ui_tooltip(find, "look for a steady tone under the take, and its harmonics");

    if (p->hum_hz > 0.0f)
    {
      snprintf(label, sizeof(label), "%.0f Hz", (double)p->hum_hz);
    }
    else
    {
      snprintf(label, sizeof(label), "-");
    }
    aud_ui_text_centred(reading, 14, p->hum_hz > 0.0f ? AUD_UI_WARN : AUD_UI_MUTED,
                        label);

    snprintf(label, sizeof(label), "%d", (int)p->harmonics);
    labelled_slider(harmonics, "harmonics", label, &p->harmonics, 1.0f,
                    (float)AUD_REPAIR_HARMONICS_MAX, usable);

    if (aud_ui_button(notch, "Notch", AUD_UI_ACCENT, usable && p->hum_hz > 0.0f))
    {
      double lo;
      double hi;

      brush_span(p, (double)p->hum_hz, &lo, &hi);
      aud_spectral_notch(p->sp, (double)p->hum_hz, hi - lo, (unsigned)p->harmonics, 0.0f);

      if ((int)p->harmonics > 1)
      {
        note(p, "notched %.0f Hz and %d harmonics of it", (double)p->hum_hz,
             (int)p->harmonics - 1);
      }
      else
      {
        note(p, "notched %.0f Hz - raise harmonics to reach the rest of the series",
             (double)p->hum_hz);
      }
    }
    aud_ui_tooltip(notch, p->hum_hz > 0.0f
                              ? "take that frequency and its harmonics out"
                              : "press Find hum first, or drag on the graph");

    snprintf(label, sizeof(label), "1/%.0f oct", 1.0 / (double)p->brush);
    labelled_slider(brush, "brush", label, &p->brush, AUD_REPAIR_BRUSH_MIN,
                    AUD_REPAIR_BRUSH_MAX, usable);
    aud_ui_tooltip(brush, "how wide a stroke on the graph is; the wheel over the "
                          "graph does this too");
  }

  /* Row two: the noise profile, and what to do with the result. */
  x = row_two.x;
  {
    Rectangle learn = slot(row_two, 84.0f, &x);
    Rectangle guess = slot(row_two, 84.0f, &x);
    Rectangle strength = slot(row_two, 130.0f, &x);
    Rectangle floor_at = slot(row_two, 130.0f, &x);
    float apply_w = 84.0f;
    Rectangle apply = {row_two.x + row_two.width - apply_w, row_two.y, apply_w,
                       row_two.height};
    Rectangle reset = {apply.x - apply_w - 6.0f, row_two.y, apply_w, row_two.height};
    char label[32];
    float strength_v = aud_spectral_strength(p->sp);
    float floor_v = aud_spectral_floor_db(p->sp);
    int have_noise = aud_spectral_noise(p->sp) != NULL;

    if (aud_ui_toggle(learn, "Learn", have_noise, AUD_UI_WARN, usable))
    {
      if (have_noise)
      {
        aud_spectral_forget_noise(p->sp);
        note(p, "noise profile forgotten");
      }
      else
      {
        aud_spectral_learn_noise(p->sp);
        note(p, "took this selection as the noise itself - now select the take");
      }
    }
    aud_ui_tooltip(learn, have_noise ? "forget the noise profile"
                                     : "select a stretch with nothing played on it, then "
                                       "press this");

    if (aud_ui_button(guess, "Guess", AUD_UI_WARN, usable))
    {
      aud_spectral_guess_noise(p->sp);
      note(p, "took the quietest each frequency ever got as the noise floor");
    }
    aud_ui_tooltip(guess, "work the noise floor out from this selection, without "
                          "needing a silent stretch");

    snprintf(label, sizeof(label), "%.2fx", (double)strength_v);
    if (labelled_slider(strength, "reduce", label, &strength_v, 0.0f,
                        AUD_SPECTRAL_STRENGTH_MAX, usable && have_noise))
    {
      aud_spectral_set_reduction(p->sp, strength_v, floor_v);
    }
    aud_ui_tooltip(strength, have_noise ? "how hard the noise profile is subtracted"
                                        : "learn or guess a noise profile first");

    snprintf(label, sizeof(label), "%.0f dB", (double)floor_v);
    if (labelled_slider(floor_at, "floor", label, &floor_v, AUD_SPECTRAL_FLOOR_MIN_DB,
                        AUD_SPECTRAL_FLOOR_MAX_DB, usable && have_noise))
    {
      aud_spectral_set_reduction(p->sp, strength_v, floor_v);
    }
    aud_ui_tooltip(floor_at, "how far down it may pull; less is gentler and warbles "
                             "less");

    if (aud_ui_button(reset, "Reset", AUD_UI_MUTED, usable))
    {
      aud_spectral_flatten(p->sp);
      aud_spectral_forget_noise(p->sp);
      p->hum_hz = 0.0f;
      note(p, "back to the audio as it was recorded");
    }
    aud_ui_tooltip(reset, "put the whole curve back to flat");

    {
      int ready = usable && aud_spectral_would_change(p->sp);

      if (aud_ui_button(apply, "Apply", AUD_UI_OK, ready))
      {
        const char *why = NULL;

        if (aud_repair_apply(d, index, from, to, p->sp, dir, &why) == 0)
        {
          double seconds = (double)(to - from) / (double)d->rate;

          /*
           * The edit is in the audio now, so the curve that described it goes
           * flat and the profile is let go. Leaving either up would mean the
           * graph showing a notch over audio it has already been taken out of,
           * and a second press of Apply quietly taking it out twice.
           */
          aud_spectral_flatten(p->sp);
          aud_spectral_forget_noise(p->sp);
          p->hum_hz = 0.0f;

          note(p, "cleaned up %.1fs of %s - ctrl+Z puts it back", seconds,
               d->tracks[index].name);
          aud_repair_panel_reset(p);
          changed = 1;
        }
        else
        {
          note(p, "%s", why != NULL ? why : "that could not be applied");
        }
      }
      aud_ui_tooltip(apply, ready ? "write this back onto the track; one press of "
                                    "Undo takes it off again"
                                  : "drag something out of the spectrum first");
    }
  }

  return changed;
}
