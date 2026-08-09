/* SPDX-License-Identifier: MIT */
#include "edit/edit.h"

#include <stdlib.h>
#include <string.h>

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

    if (!d->tracks[i].selected)
    {
      continue;
    }

    if (aud_track_extract(&d->tracks[i], d->sel_start, d->sel_end, &piece) != 0)
    {
      return -1;
    }

    fresh = aud_doc_add_track(d, d->tracks[i].name, d->tracks[i].channels);
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
