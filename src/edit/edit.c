/* SPDX-License-Identifier: MIT */
#include "edit/edit.h"

#include "audio/loudness.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frames measured at a time, which is a buffer of a few hundred kilobytes. */
#define EDIT_MEASURE_CHUNK 8192u

void aud_clipboard_init(aud_clipboard *c)
{
  if (c != NULL)
  {
    memset(c, 0, sizeof(*c));
  }
}

void aud_clipboard_clear(aud_clipboard *c)
{
  if (c == NULL)
  {
    return;
  }

  for (size_t i = 0; i < c->count; i++)
  {
    aud_track_free(&c->tracks[i]);
  }
  free(c->tracks);
  memset(c, 0, sizeof(*c));
}

int aud_clipboard_empty(const aud_clipboard *c)
{
  return c == NULL || c->count == 0 || c->frames == 0;
}

/* Non-zero when there is a range and at least one track to apply it to. */
static int have_work(const aud_doc *d)
{
  return d != NULL && aud_doc_has_range(d) && aud_doc_any_track_selected(d);
}

int aud_edit_delete(aud_doc *d)
{
  if (!have_work(d))
  {
    return -1;
  }

  aud_doc_checkpoint(d, "delete");

  for (size_t i = 0; i < d->count; i++)
  {
    if (d->tracks[i].selected)
    {
      aud_track_delete(&d->tracks[i], d->sel_start, d->sel_end, 1);
    }
  }

  /* the cursor lands where the removed audio was, which is where the two ends
   * of the take now meet - and where the next paste would go */
  aud_doc_set_cursor(d, d->sel_start);
  return 0;
}

int aud_edit_silence(aud_doc *d)
{
  if (!have_work(d))
  {
    return -1;
  }

  aud_doc_checkpoint(d, "silence");

  for (size_t i = 0; i < d->count; i++)
  {
    if (d->tracks[i].selected)
    {
      aud_track_delete(&d->tracks[i], d->sel_start, d->sel_end, 0);
    }
  }

  /* the selection stays: silencing a bar and then wanting it back is common,
   * and undo is not the only way to change your mind about how much */
  d->dirty = 1;
  return 0;
}

int aud_edit_copy(aud_doc *d, aud_clipboard *c)
{
  size_t wanted = 0;

  if (!have_work(d) || c == NULL)
  {
    return -1;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    wanted += d->tracks[i].selected ? 1u : 0u;
  }

  aud_clipboard_clear(c);
  c->tracks = calloc(wanted, sizeof(*c->tracks));
  if (c->tracks == NULL)
  {
    return -1;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    if (!d->tracks[i].selected)
    {
      continue;
    }

    if (aud_track_extract(&d->tracks[i], d->sel_start, d->sel_end,
                          &c->tracks[c->count]) != 0)
    {
      aud_clipboard_clear(c);
      return -1;
    }
    c->count++;
  }

  c->rate = d->rate;
  c->frames = d->sel_end - d->sel_start;
  return 0;
}

int aud_edit_cut(aud_doc *d, aud_clipboard *c)
{
  if (aud_edit_copy(d, c) != 0)
  {
    return -1;
  }

  /*
   * Copy took no checkpoint - it changed nothing - so the one delete takes is
   * the only one, and a single undo puts back what a single Ctrl+X removed.
   */
  return aud_edit_delete(d);
}

int aud_edit_paste(aud_doc *d, const aud_clipboard *c)
{
  size_t targets[AUD_DOC_MAX_TRACKS];
  size_t target_count = 0;
  uint64_t at;

  if (d == NULL || aud_clipboard_empty(c))
  {
    return -1;
  }
  if (c->rate != d->rate)
  {
    return -1; /* pasting 48 kHz into 44.1 would play back at the wrong pitch */
  }

  aud_doc_checkpoint(d, "paste");

  /*
   * Replacing a selection rather than pushing it along, which is what typing
   * over selected text does and what anyone pressing Ctrl+V over a highlighted
   * bar means by it.
   */
  if (aud_doc_has_range(d) && aud_doc_any_track_selected(d))
  {
    for (size_t i = 0; i < d->count; i++)
    {
      if (d->tracks[i].selected)
      {
        aud_track_delete(&d->tracks[i], d->sel_start, d->sel_end, 1);
      }
    }
  }
  at = d->sel_start;

  for (size_t i = 0; i < d->count && target_count < AUD_DOC_MAX_TRACKS; i++)
  {
    if (d->tracks[i].selected)
    {
      targets[target_count++] = i;
    }
  }

  /* nothing selected: the top of the stack is where a paste has to go, since
   * refusing outright would leave the clipboard with nowhere it could ever go */
  if (target_count == 0)
  {
    for (size_t i = 0; i < d->count && target_count < c->count; i++)
    {
      targets[target_count++] = i;
    }
  }

  for (size_t i = 0; i < c->count; i++)
  {
    if (i < target_count)
    {
      if (aud_track_paste(&d->tracks[targets[i]], at, &c->tracks[i]) != 0)
      {
        return -1;
      }
      continue;
    }

    /* the clipboard was wider than what it is being pasted into: the rest
     * become tracks of their own rather than being quietly dropped */
    {
      aud_track *fresh = aud_doc_add_track(d, c->tracks[i].name, c->tracks[i].channels);

      if (fresh == NULL || aud_track_paste(fresh, at, &c->tracks[i]) != 0)
      {
        return -1;
      }
      fresh->selected = 1;
    }
  }

  aud_doc_select(d, at, at + c->frames);
  return 0;
}

int aud_edit_trim(aud_doc *d)
{
  uint64_t from;
  uint64_t to;

  if (!have_work(d))
  {
    return -1;
  }

  from = d->sel_start;
  to = d->sel_end;
  aud_doc_checkpoint(d, "trim");

  for (size_t i = 0; i < d->count; i++)
  {
    aud_track *t = &d->tracks[i];

    if (!t->selected)
    {
      continue;
    }

    /*
     * The tail first. Taking the head off would move everything after it, and
     * the range that was to be removed from the end with it.
     */
    aud_track_delete(t, to, aud_track_end(t), 1);
    aud_track_delete(t, 0, from, 1);
  }

  aud_doc_select(d, 0, to - from);
  return 0;
}

int aud_edit_split(aud_doc *d)
{
  int any = 0;

  if (d == NULL || !aud_doc_any_track_selected(d))
  {
    return -1;
  }

  aud_doc_checkpoint(d, "split");

  for (size_t i = 0; i < d->count; i++)
  {
    aud_track *t = &d->tracks[i];

    if (!t->selected)
    {
      continue;
    }

    aud_track_split(t, d->sel_start);
    if (aud_doc_has_range(d))
    {
      aud_track_split(t, d->sel_end);
    }
    any = 1;
  }

  d->dirty = 1;
  return any ? 0 : -1;
}

int aud_edit_duplicate(aud_doc *d)
{
  size_t was = 0;

  if (!have_work(d))
  {
    return -1;
  }

  was = d->count;
  aud_doc_checkpoint(d, "duplicate");

  for (size_t i = 0; i < was; i++)
  {
    aud_track piece;
    aud_track *fresh;
    char name[AUD_TRACK_NAME_MAX];
    unsigned channels;

    if (!d->tracks[i].selected)
    {
      continue;
    }

    if (aud_track_extract(&d->tracks[i], d->sel_start, d->sel_end, &piece) != 0)
    {
      return -1;
    }

    /*
     * Taken before the add, not passed out of the array into it: the track list
     * is one block and aud_doc_add_track() reallocs it, so a pointer into the
     * old one is dangling by the time the new track is initialised from it.
     */
    snprintf(name, sizeof(name), "%s", d->tracks[i].name);
    channels = d->tracks[i].channels;

    fresh = aud_doc_add_track(d, name, channels);
    if (fresh == NULL)
    {
      aud_track_free(&piece);
      return -1;
    }

    /* at the same place on the timeline, so it lines up with what it came
     * from rather than jumping to the start of the project */
    if (aud_track_paste(fresh, d->sel_start, &piece) != 0)
    {
      aud_track_free(&piece);
      return -1;
    }
    aud_track_free(&piece);
  }

  return 0;
}

int64_t aud_edit_move_room(const aud_doc *d, int64_t want)
{
  int64_t room = want;

  if (!have_work(d))
  {
    return 0;
  }

  for (size_t i = 0; i < d->count && room != 0; i++)
  {
    const aud_track *t = &d->tracks[i];

    if (!t->selected)
    {
      continue;
    }

    /*
     * A take still arriving stops the whole move rather than only its own lane,
     * for the same reason the distance is shared: what is left behind has to
     * line up with what went with it.
     */
    if (aud_track_recording(t))
    {
      return 0;
    }

    /* each lane can only bring it closer to zero, so threading it through is
     * the same as taking the least of them */
    room = aud_track_move_room(t, d->sel_start, d->sel_end, room);
  }

  return room;
}

int aud_edit_move(aud_doc *d, int64_t by)
{
  int64_t room = aud_edit_move_room(d, by);

  if (room == 0)
  {
    return -1;
  }

  aud_doc_checkpoint(d, "move");

  for (size_t i = 0; i < d->count; i++)
  {
    if (d->tracks[i].selected)
    {
      aud_track_move(&d->tracks[i], d->sel_start, d->sel_end, room);
    }
  }

  aud_doc_select(d, aud_frame_offset(d->sel_start, room),
                 aud_frame_offset(d->sel_end, room));
  return 0;
}

/*
 * Ramp the selection up from silence, or down into it.
 *
 * The selection says how long the fade is and which end it is at: a fade-in
 * runs from its start, a fade-out ends at its end. The clip is split at that
 * edge first, so there is a boundary for the ramp to begin or finish at - which
 * is also what makes fading the middle of a take mean something.
 */
static int fade(aud_doc *d, int out)
{
  uint64_t edge;
  size_t length;
  int any = 0;

  if (!have_work(d))
  {
    return -1;
  }

  edge = out ? d->sel_end : d->sel_start;
  length = (size_t)(d->sel_end - d->sel_start);
  aud_doc_checkpoint(d, out ? "fade out" : "fade in");

  for (size_t i = 0; i < d->count; i++)
  {
    aud_track *t = &d->tracks[i];
    int done;

    if (!t->selected)
    {
      continue;
    }

    if (aud_track_split(t, edge) != 0)
    {
      return -1;
    }

    done = out ? aud_track_fade_out_at(t, edge, length) == 0
               : aud_track_fade_in_at(t, edge, length) == 0;
    any = any || done;
  }

  d->dirty = 1;
  return any ? 0 : -1;
}

int aud_edit_fade_in(aud_doc *d)
{
  return fade(d, 0);
}

int aud_edit_fade_out(aud_doc *d)
{
  return fade(d, 1);
}

int aud_edit_gain(aud_doc *d, double db)
{
  float by;
  int any = 0;

  if (!have_work(d))
  {
    return -1;
  }

  /*
   * Held to what the model can hold anyway, so a caller cannot ask for an
   * exponent that overflows on the way to being clamped.
   */
  if (!(db > -200.0 && db < 200.0))
  {
    return -1;
  }

  by = (float)pow(10.0, db / 20.0);
  aud_doc_checkpoint(d, db < 0.0 ? "quieter" : "louder");

  for (size_t i = 0; i < d->count; i++)
  {
    if (d->tracks[i].selected)
    {
      any = aud_track_gain_scale(&d->tracks[i], d->sel_start, d->sel_end, by) == 0 || any;
    }
  }

  /* the selection stays: turning something down and then wanting a little more
   * of it back is the ordinary way this key gets used */
  d->dirty = 1;
  return any ? 0 : -1;
}

/*
 * How loud [from, to) of one lane is, through the same read the mix does - so
 * what is measured is what an export of that range would hold, fades, gain and
 * the silence in the gaps included.
 *
 * Returns 0 with `out` filled in, or -1 when this lane cannot be measured at
 * all: a rate BS.1770 is not defined at, or no memory.
 */
static int measure_range(const aud_track *t, uint64_t from, uint64_t to, unsigned rate,
                         aud_loudness_reading *out)
{
  aud_loudness *meter = aud_loudness_create(rate, t->channels);
  float *buf;
  int ok = 0;

  if (meter == NULL)
  {
    return -1;
  }

  buf = calloc((size_t)EDIT_MEASURE_CHUNK * t->channels, sizeof(*buf));
  if (buf == NULL)
  {
    aud_loudness_destroy(meter);
    return -1;
  }

  for (uint64_t at = from; at < to; at += EDIT_MEASURE_CHUNK)
  {
    size_t take = to - at < (uint64_t)EDIT_MEASURE_CHUNK ? (size_t)(to - at)
                                                         : (size_t)EDIT_MEASURE_CHUNK;

    aud_track_read(t, at, buf, take);
    if (aud_loudness_feed(meter, buf, take) != 0)
    {
      ok = -1;
      break;
    }
  }

  if (ok == 0)
  {
    aud_loudness_read(meter, out);
  }

  free(buf);
  aud_loudness_destroy(meter);
  return ok;
}

/*
 * What to multiply a lane by to land it on `level`, or 0 when the measurement
 * gives nothing to work from - silence has no peak to raise and a range under
 * 400 ms has no loudness, and multiplying either by a guess would be inventing
 * a number rather than measuring one.
 */
static double normalize_factor(const aud_loudness_reading *r, aud_normalize_target to,
                               double level)
{
  if (to == AUD_NORMALIZE_LOUDNESS)
  {
    if (!aud_loudness_measured(r->integrated))
    {
      return 0.0;
    }
    /* LUFS is already a decibel scale, and multiplying a signal by g moves it
     * by 20 log10 g - the same arithmetic a peak takes */
    return pow(10.0, (level - r->integrated) / 20.0);
  }

  if (!(r->true_peak > 0.0))
  {
    return 0.0;
  }
  return pow(10.0, level / 20.0) / r->true_peak;
}

int aud_edit_normalize(aud_doc *d, aud_normalize_target to, double level)
{
  /* the doc holds no more than this many lanes - see aud_doc_add_track() */
  double factor[AUD_DOC_MAX_TRACKS];
  size_t lanes;
  int wanted = 0;
  int any = 0;

  if (!have_work(d) || !(level > -200.0 && level < 200.0))
  {
    return -1;
  }

  /*
   * Every lane measured before any of them is changed. Measuring costs a read
   * of the selection and changes nothing, so a normalize that turns out to
   * have found nothing measurable can return without having spent an undo step
   * - and without a half-normalized session behind it.
   */
  lanes = d->count < AUD_DOC_MAX_TRACKS ? d->count : AUD_DOC_MAX_TRACKS;
  for (size_t i = 0; i < lanes; i++)
  {
    aud_loudness_reading r;

    factor[i] = 0.0;
    if (!d->tracks[i].selected)
    {
      continue;
    }

    if (measure_range(&d->tracks[i], d->sel_start, d->sel_end, d->rate, &r) != 0)
    {
      continue;
    }

    factor[i] = normalize_factor(&r, to, level);
    wanted = wanted || factor[i] > 0.0;
  }

  if (!wanted)
  {
    return -1;
  }

  aud_doc_checkpoint(d, "normalize");

  for (size_t i = 0; i < lanes; i++)
  {
    if (factor[i] > 0.0)
    {
      any = aud_track_gain_scale(&d->tracks[i], d->sel_start, d->sel_end,
                                 (float)factor[i]) == 0 ||
            any;
    }
  }

  d->dirty = 1;
  return any ? 0 : -1;
}

int aud_edit_remove_track(aud_doc *d, size_t index)
{
  if (d == NULL || index >= d->count)
  {
    return -1;
  }

  aud_doc_checkpoint(d, "close track");
  aud_doc_remove_track(d, index);
  return 0;
}
