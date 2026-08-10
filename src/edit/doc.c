/* SPDX-License-Identifier: MIT */
#include "edit/doc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void aud_doc_init(aud_doc *d, unsigned rate)
{
  if (d == NULL)
  {
    return;
  }

  memset(d, 0, sizeof(*d));
  d->rate = rate != 0 ? rate : 44100u;
  d->tempo = AUD_DOC_DEFAULT_TEMPO;
  d->beats_per_bar = AUD_CLICK_DEFAULT_BEATS;
  d->grid_div = AUD_DOC_GRID_BEAT;
}

void aud_doc_set_tempo(aud_doc *d, double bpm, unsigned beats_per_bar)
{
  if (d == NULL)
  {
    return;
  }

  /* written the way round that catches NaN rather than letting it through */
  if (!(bpm >= AUD_CLICK_BPM_MIN))
  {
    bpm = AUD_CLICK_BPM_MIN;
  }
  else if (bpm > AUD_CLICK_BPM_MAX)
  {
    bpm = AUD_CLICK_BPM_MAX;
  }

  d->tempo = bpm;
  d->beats_per_bar =
      beats_per_bar > AUD_CLICK_BEATS_MAX ? AUD_CLICK_BEATS_MAX : beats_per_bar;
}

double aud_doc_beat_frames(const aud_doc *d)
{
  if (d == NULL || d->rate == 0 || !(d->tempo >= AUD_CLICK_BPM_MIN) ||
      d->tempo > AUD_CLICK_BPM_MAX)
  {
    return 0.0;
  }
  return 60.0 * (double)d->rate / d->tempo;
}

double aud_doc_bar_frames(const aud_doc *d)
{
  double beat = aud_doc_beat_frames(d);

  if (beat <= 0.0 || d->beats_per_bar < 2u)
  {
    return beat;
  }
  return beat * (double)d->beats_per_bar;
}

void aud_doc_set_grid(aud_doc *d, unsigned div)
{
  if (d == NULL)
  {
    return;
  }
  d->grid_div = div > AUD_DOC_GRID_MAX ? AUD_DOC_GRID_MAX : div;
}

double aud_doc_grid_frames(const aud_doc *d)
{
  double beat = aud_doc_beat_frames(d);

  if (beat <= 0.0)
  {
    return 0.0;
  }
  if (d->grid_div == AUD_DOC_GRID_BAR)
  {
    return aud_doc_bar_frames(d);
  }
  return beat / (double)d->grid_div;
}

const char *aud_doc_grid_label(const aud_doc *d)
{
  /* indexed by grid_div, so the array has to be as long as the division may be */
  static const char *const labels[AUD_DOC_GRID_MAX + 1u] = {
      "bars",     "beats",    "1/2 beat", "1/3 beat", "1/4 beat",
      "1/5 beat", "1/6 beat", "1/7 beat", "1/8 beat",
  };
  unsigned div;

  if (d == NULL)
  {
    return "beats";
  }
  div = d->grid_div > AUD_DOC_GRID_MAX ? AUD_DOC_GRID_MAX : d->grid_div;
  return labels[div];
}

uint64_t aud_doc_snap(const aud_doc *d, uint64_t frame)
{
  double unit = aud_doc_grid_frames(d);
  double at;

  if (unit <= 0.0)
  {
    return frame;
  }

  at = ((double)frame / unit + 0.5);
  if (at < 0.0)
  {
    return 0;
  }
  return (uint64_t)(floor(at) * unit + 0.5);
}

uint64_t aud_doc_grid_step(const aud_doc *d, uint64_t frame, int back)
{
  double unit = aud_doc_grid_frames(d);
  double line;
  uint64_t out;

  if (unit <= 0.0)
  {
    return frame;
  }

  line = (double)frame / unit;
  line = back ? ceil(line - 1.0) : floor(line + 1.0);
  if (line < 0.0)
  {
    return 0;
  }
  out = (uint64_t)(line * unit + 0.5);

  /*
   * A grid line has to land on a whole frame, and a tempo that does not divide
   * the sample rate puts one a fraction either side of where the arithmetic
   * says. So the line next to `frame` can round straight back onto `frame`
   * itself - at which point the step has to take the line beyond it, or an
   * arrow key would wedge on every line the rounding went the wrong way for.
   *
   * Checked against the answer rather than absorbed into an epsilon on the way
   * in: the error here scales with how many frames a line is, which no single
   * constant covers.
   */
  if (!back && out <= frame)
  {
    out = (uint64_t)((line + 1.0) * unit + 0.5);
  }
  else if (back && out >= frame)
  {
    if (line < 1.0)
    {
      return 0;
    }
    out = (uint64_t)((line - 1.0) * unit + 0.5);
  }
  return out;
}

/* Free the tracks of a snapshot, or of the project itself. */
static void free_tracks(aud_track *tracks, size_t count)
{
  for (size_t i = 0; i < count; i++)
  {
    aud_track_free(&tracks[i]);
  }
  free(tracks);
}

static void state_free(aud_doc_state *s)
{
  free_tracks(s->tracks, s->count);
  s->tracks = NULL;
  s->count = 0;
}

void aud_doc_free(aud_doc *d)
{
  if (d == NULL)
  {
    return;
  }

  for (size_t i = 0; i < d->undo_count; i++)
  {
    state_free(&d->undo[i]);
  }
  for (size_t i = 0; i < d->redo_count; i++)
  {
    state_free(&d->redo[i]);
  }

  free_tracks(d->tracks, d->count);
  memset(d, 0, sizeof(*d));
}

aud_track *aud_doc_add_track(aud_doc *d, const char *name, unsigned channels)
{
  aud_track *grown;
  aud_track *slot;

  if (d == NULL || d->count >= AUD_DOC_MAX_TRACKS)
  {
    return NULL;
  }

  if (d->count == d->capacity)
  {
    size_t capacity = d->capacity == 0 ? 4u : d->capacity * 2u;

    grown = realloc(d->tracks, capacity * sizeof(*grown));
    if (grown == NULL)
    {
      return NULL;
    }
    d->tracks = grown;
    d->capacity = capacity;
  }

  slot = &d->tracks[d->count];
  if (aud_track_init(slot, name, channels) != 0)
  {
    return NULL;
  }

  d->count++;
  d->dirty = 1;
  return slot;
}

void aud_doc_remove_track(aud_doc *d, size_t index)
{
  if (d == NULL || index >= d->count)
  {
    return;
  }

  aud_track_free(&d->tracks[index]);
  memmove(&d->tracks[index], &d->tracks[index + 1u],
          (d->count - index - 1u) * sizeof(*d->tracks));
  d->count--;
  d->dirty = 1;
}

void aud_doc_move_track(aud_doc *d, size_t index, int down)
{
  size_t other;
  aud_track swap;

  if (d == NULL || index >= d->count)
  {
    return;
  }

  if (down)
  {
    if (index + 1u >= d->count)
    {
      return;
    }
    other = index + 1u;
  }
  else
  {
    if (index == 0)
    {
      return;
    }
    other = index - 1u;
  }

  swap = d->tracks[index];
  d->tracks[index] = d->tracks[other];
  d->tracks[other] = swap;
  d->dirty = 1;
}

uint64_t aud_doc_end(const aud_doc *d)
{
  uint64_t end = 0;

  if (d == NULL)
  {
    return 0;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    uint64_t t_end = aud_track_end(&d->tracks[i]);

    if (t_end > end)
    {
      end = t_end;
    }
  }
  return end;
}

size_t aud_doc_bytes(const aud_doc *d)
{
  size_t bytes = 0;

  if (d == NULL)
  {
    return 0;
  }

  /*
   * Counted per clip, so a block two clips share is counted twice. Wrong in
   * principle and right in practice: this is shown to say "the session is
   * getting big", and walking a set of distinct blocks to be exact about it
   * would cost more than the number is worth.
   */
  for (size_t i = 0; i < d->count; i++)
  {
    const aud_track *t = &d->tracks[i];

    for (size_t c = 0; c < t->count; c++)
    {
      bytes += t->clips[c].frames * t->channels * sizeof(float);
    }
  }
  return bytes;
}

void aud_doc_set_cursor(aud_doc *d, uint64_t frame)
{
  if (d == NULL)
  {
    return;
  }

  d->cursor = frame;
  d->sel_start = frame;
  d->sel_end = frame;
  d->dirty = 1;
}

void aud_doc_select(aud_doc *d, uint64_t from, uint64_t to)
{
  if (d == NULL)
  {
    return;
  }

  /* dragging right to left is the same selection as dragging left to right */
  if (from > to)
  {
    uint64_t swap = from;

    from = to;
    to = swap;
  }

  d->sel_start = from;
  d->sel_end = to;
  d->cursor = from;
  d->dirty = 1;
}

void aud_doc_select_from(aud_doc *d, uint64_t anchor, uint64_t edge)
{
  if (d == NULL)
  {
    return;
  }

  d->sel_start = anchor < edge ? anchor : edge;
  d->sel_end = anchor < edge ? edge : anchor;
  d->cursor = anchor;
  d->dirty = 1;
}

int aud_doc_has_range(const aud_doc *d)
{
  return d != NULL && d->sel_end > d->sel_start;
}

void aud_doc_select_tracks(aud_doc *d, int selected)
{
  if (d == NULL)
  {
    return;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    d->tracks[i].selected = selected;
  }
  d->dirty = 1;
}

int aud_doc_any_track_selected(const aud_doc *d)
{
  if (d == NULL)
  {
    return 0;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    if (d->tracks[i].selected)
    {
      return 1;
    }
  }
  return 0;
}

void aud_doc_select_all(aud_doc *d)
{
  if (d == NULL)
  {
    return;
  }

  aud_doc_select_tracks(d, 1);
  aud_doc_select(d, 0, aud_doc_end(d));
}

/* Snapshot the project's tracks into `s`. Returns 0, or -1 on failure. */
static int state_capture(aud_doc_state *s, const aud_doc *d, const char *label)
{
  memset(s, 0, sizeof(*s));
  snprintf(s->label, sizeof(s->label), "%s", label != NULL ? label : "edit");
  s->cursor = d->cursor;
  s->sel_start = d->sel_start;
  s->sel_end = d->sel_end;

  if (d->count == 0)
  {
    return 0;
  }

  s->tracks = calloc(d->count, sizeof(*s->tracks));
  if (s->tracks == NULL)
  {
    return -1;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    if (aud_track_copy(&s->tracks[i], &d->tracks[i]) != 0)
    {
      free_tracks(s->tracks, i);
      s->tracks = NULL;
      return -1;
    }
  }
  s->count = d->count;
  return 0;
}

/* Put `s` back, handing the project the tracks it holds. `s` is emptied. */
static void state_restore(aud_doc *d, aud_doc_state *s)
{
  free_tracks(d->tracks, d->count);

  d->tracks = s->tracks;
  d->count = s->count;
  d->capacity = s->count;
  d->cursor = s->cursor;
  d->sel_start = s->sel_start;
  d->sel_end = s->sel_end;
  d->dirty = 1;

  s->tracks = NULL;
  s->count = 0;
}

/* Push `s` onto a stack, dropping the oldest step when it is full. */
static void state_push(aud_doc_state *stack, size_t *count, aud_doc_state *s)
{
  if (*count == AUD_DOC_UNDO_DEPTH)
  {
    state_free(&stack[0]);
    memmove(&stack[0], &stack[1], (AUD_DOC_UNDO_DEPTH - 1u) * sizeof(*stack));
    (*count)--;
  }

  stack[(*count)++] = *s;
}

void aud_doc_checkpoint(aud_doc *d, const char *label)
{
  aud_doc_state s;

  if (d == NULL || state_capture(&s, d, label) != 0)
  {
    return; /* out of memory: lose a step of undo rather than the edit */
  }

  state_push(d->undo, &d->undo_count, &s);

  /*
   * The redo stack described a future reached from a project that no longer
   * exists. Keeping it would offer to step forward into an edit made against
   * different audio.
   */
  for (size_t i = 0; i < d->redo_count; i++)
  {
    state_free(&d->redo[i]);
  }
  d->redo_count = 0;
}

int aud_doc_undo(aud_doc *d)
{
  aud_doc_state now;
  aud_doc_state *step;

  if (d == NULL || d->undo_count == 0)
  {
    return -1;
  }

  step = &d->undo[d->undo_count - 1u];

  /* where we are now becomes the redo step, labelled with the same edit */
  if (state_capture(&now, d, step->label) != 0)
  {
    return -1;
  }
  state_push(d->redo, &d->redo_count, &now);

  state_restore(d, step);
  d->undo_count--;
  return 0;
}

int aud_doc_redo(aud_doc *d)
{
  aud_doc_state now;
  aud_doc_state *step;

  if (d == NULL || d->redo_count == 0)
  {
    return -1;
  }

  step = &d->redo[d->redo_count - 1u];

  if (state_capture(&now, d, step->label) != 0)
  {
    return -1;
  }
  state_push(d->undo, &d->undo_count, &now);

  state_restore(d, step);
  d->redo_count--;
  return 0;
}

const char *aud_doc_undo_label(const aud_doc *d)
{
  if (d == NULL || d->undo_count == 0)
  {
    return NULL;
  }
  return d->undo[d->undo_count - 1u].label;
}

const char *aud_doc_redo_label(const aud_doc *d)
{
  if (d == NULL || d->redo_count == 0)
  {
    return NULL;
  }
  return d->redo[d->redo_count - 1u].label;
}
