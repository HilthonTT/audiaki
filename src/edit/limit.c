/* SPDX-License-Identifier: MIT */
#include "edit/limit.h"

#include "audio/limiter.h"
#include "audio/loudness.h"
#include "audio/truepeak.h"
#include "edit/load.h"
#include "util/path.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Frames measured at a time by aud_limit_peak_db(), a few hundred kilobytes. */
#define LIMIT_MEASURE_CHUNK 8192u

static void say(const char **why, const char *text)
{
  if (why != NULL)
  {
    *why = text;
  }
}

/* Non-zero when there is a range selected and a lane selected to apply it to. */
static int have_work(const aud_doc *d)
{
  return d != NULL && d->rate != 0 && d->sel_end > d->sel_start &&
         aud_doc_any_track_selected(d);
}

/*
 * The selection of one lane, read through the same path the mix uses - so what
 * is limited is what an export of that range would hold, fades, clip gains and
 * the silence in the gaps included - and then put under the ceiling.
 *
 * Returns the block, which the caller owns, with `*reduction` set to how far it
 * had to be turned down. A block that came back untouched still comes back:
 * whether that is worth putting on the timeline is the caller's judgement, and
 * it is the one thing it needs `*reduction` for.
 */
static aud_samples *limited_range(const aud_track *t, uint64_t from, uint64_t to,
                                  unsigned rate, double ceiling_db, double *reduction,
                                  const char **why)
{
  aud_samples *block;
  size_t length;

  *reduction = 0.0;

  if (t->channels == 0 || to <= from)
  {
    say(why, "there is nothing selected to limit");
    return NULL;
  }

  if (to - from > (uint64_t)SIZE_MAX / (2u * t->channels * sizeof(float)))
  {
    say(why, "that selection is too long to limit in one go");
    return NULL;
  }
  length = (size_t)(to - from);

  block = aud_samples_create(t->channels, length);
  if (block == NULL)
  {
    say(why, "not enough memory to limit that much audio");
    return NULL;
  }

  aud_track_read(t, from, block->data, length);

  if (aud_limiter_apply(block->data, length, t->channels, rate, ceiling_db, reduction) !=
      0)
  {
    say(why, "that project is at a rate the limiter is not defined at");
    aud_samples_release(block);
    return NULL;
  }

  return block;
}

int aud_limit_selection(aud_doc *d, double ceiling_db, const char *dir,
                        double *reduction_db, const char **why)
{
  const char *failed = NULL;
  double most = 0.0;
  int touched = 0;

  say(why, NULL);
  if (reduction_db != NULL)
  {
    *reduction_db = 0.0;
  }

  if (!have_work(d))
  {
    say(why, "there is nothing selected to limit");
    return -1;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    aud_samples *block;
    char path[AUD_PATH_MAX];
    double reduction = 0.0;
    size_t length = (size_t)(d->sel_end - d->sel_start);
    const char *reason = NULL;

    if (!d->tracks[i].selected)
    {
      continue;
    }

    block = limited_range(&d->tracks[i], d->sel_start, d->sel_end, d->rate, ceiling_db,
                          &reduction, &reason);
    if (block == NULL)
    {
      /*
       * A lane that could not be read at all. Remembered rather than returned
       * on the spot: one bad lane in a selection of six should not throw away
       * the five that can be limited, and what it has to say is said at the
       * end.
       */
      failed = reason;
      continue;
    }

    /*
     * Nothing over the ceiling on this lane, so nothing to do to it. Dropped
     * here rather than earlier because finding out costs the same read either
     * way, and stopping here is what keeps the operation free of side effects
     * everywhere it has no work: no file written, no clip disturbed, and a
     * second press changing nothing.
     */
    if (!(reduction > 0.0))
    {
      aud_samples_release(block);
      continue;
    }

    aud_samples_index(block);

    if (aud_edit_write_block(block, d->rate, dir, AUD_LIMIT_PREFIX, path, sizeof(path),
                             &reason) != 0 ||
        aud_samples_set_source(block, path) != 0)
    {
      failed = reason != NULL ? reason
                              : "not enough memory to remember where that was written";
      aud_samples_release(block);
      break;
    }

    /*
     * The first lane that actually has work is where the undo step is taken:
     * everything that could have failed on it already has, and a press that
     * turns out to have nothing to do anywhere spends no step at all.
     */
    if (!touched)
    {
      aud_doc_checkpoint(d, AUD_LIMIT_LABEL);
    }

    aud_track_delete(&d->tracks[i], d->sel_start, d->sel_end, 0);
    if (aud_track_place(&d->tracks[i], block, 0, length, d->sel_start, 0, 0) != 0)
    {
      /*
       * Only reachable if the clip list would not grow, and the range has just
       * been emptied so there is room for exactly this one clip.
       */
      failed = "the limited audio would not go back onto the track";
      aud_samples_release(block);
      if (!touched)
      {
        aud_doc_undo(d); /* nothing else had been changed yet; take it all back */
        say(why, failed);
        return -1;
      }
      break;
    }

    aud_samples_release(block); /* the clip holds it now */
    aud_track_tidy(&d->tracks[i]);

    touched = 1;
    if (reduction > most)
    {
      most = reduction;
    }
  }

  if (reduction_db != NULL)
  {
    *reduction_db = most;
  }
  if (touched)
  {
    d->dirty = 1;
  }

  /*
   * A lane that would not go through is a failure even where others did. What
   * was limited stays - it is behind one checkpoint, so Undo takes all of it
   * back at once - and the caller is told rather than being handed a success
   * that quietly did less than it was asked.
   */
  if (failed != NULL)
  {
    say(why, failed);
    return -1;
  }

  if (!touched)
  {
    say(why, "nothing in that selection is over the ceiling");
    return -1;
  }

  return 0;
}

double aud_limit_peak_db(const aud_doc *d)
{
  double loudest = 0.0;

  if (!have_work(d))
  {
    return AUD_LUFS_NONE;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    const aud_track *t = &d->tracks[i];
    aud_loudness *meter;
    aud_loudness_reading r;
    float *buf;

    if (!t->selected || t->channels == 0)
    {
      continue;
    }

    /*
     * Through the meter rather than through audio/truepeak.h directly. It is
     * the same interpolator either way, and this is the arrangement that has
     * already been made to look between the samples of a stream arriving a
     * chunk at a time.
     */
    meter = aud_loudness_create(d->rate, t->channels);
    if (meter == NULL)
    {
      continue;
    }

    buf = calloc((size_t)LIMIT_MEASURE_CHUNK * t->channels, sizeof(*buf));
    if (buf == NULL)
    {
      aud_loudness_destroy(meter);
      continue;
    }

    for (uint64_t at = d->sel_start; at < d->sel_end; at += LIMIT_MEASURE_CHUNK)
    {
      size_t take = d->sel_end - at < (uint64_t)LIMIT_MEASURE_CHUNK
                        ? (size_t)(d->sel_end - at)
                        : (size_t)LIMIT_MEASURE_CHUNK;

      aud_track_read(t, at, buf, take);
      if (aud_loudness_feed(meter, buf, take) != 0)
      {
        break;
      }
    }

    aud_loudness_read(meter, &r);
    if (r.true_peak > loudest)
    {
      loudest = r.true_peak;
    }

    free(buf);
    aud_loudness_destroy(meter);
  }

  return loudest > 0.0 ? 20.0 * log10(loudest) : AUD_LUFS_NONE;
}
