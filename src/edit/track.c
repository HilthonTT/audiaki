/* SPDX-License-Identifier: MIT */
#include "edit/track.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Clips a track starts with room for; it doubles from there. */
#define TRACK_CLIPS_MIN 4u

static int clips_reserve(aud_track *t, size_t want)
{
  size_t capacity = t->capacity;
  aud_clip *grown;

  if (want <= capacity)
  {
    return 0;
  }

  capacity = capacity == 0 ? TRACK_CLIPS_MIN : capacity;
  while (capacity < want)
  {
    capacity *= 2u;
  }

  grown = realloc(t->clips, capacity * sizeof(*grown));
  if (grown == NULL)
  {
    return -1;
  }

  t->clips = grown;
  t->capacity = capacity;
  return 0;
}

/* One past the last frame `c` covers. */
static uint64_t clip_end(const aud_clip *c)
{
  return c->start + c->frames;
}

/*
 * Make room for a clip at `index` and return it, uninitialised. The caller
 * fills it in and is responsible for the sort order still holding.
 */
static aud_clip *clip_insert(aud_track *t, size_t index)
{
  if (clips_reserve(t, t->count + 1) != 0)
  {
    return NULL;
  }

  memmove(&t->clips[index + 1], &t->clips[index], (t->count - index) * sizeof(*t->clips));
  t->count++;
  return &t->clips[index];
}

static void clip_remove(aud_track *t, size_t index)
{
  aud_samples_release(t->clips[index].audio);
  memmove(&t->clips[index], &t->clips[index + 1],
          (t->count - index - 1) * sizeof(*t->clips));
  t->count--;
}

int aud_track_init(aud_track *t, const char *name, unsigned channels)
{
  if (t == NULL || channels == 0)
  {
    return -1;
  }

  memset(t, 0, sizeof(*t));
  t->recording = -1;
  snprintf(t->name, sizeof(t->name), "%s", name != NULL ? name : "Track");
  t->channels = channels;
  t->gain = 1.0f;
  t->pan = 0.0f;
  t->height = AUD_TRACK_HEIGHT_DEFAULT;
  return 0;
}

void aud_track_free(aud_track *t)
{
  if (t == NULL)
  {
    return;
  }

  for (size_t i = 0; i < t->count; i++)
  {
    aud_samples_release(t->clips[i].audio);
  }
  free(t->clips);
  t->clips = NULL;
  t->count = 0;
  t->capacity = 0;
}

int aud_track_copy(aud_track *dst, const aud_track *src)
{
  if (dst == NULL || src == NULL)
  {
    return -1;
  }

  *dst = *src;
  dst->clips = NULL;
  dst->count = 0;
  dst->capacity = 0;

  if (src->count == 0)
  {
    return 0;
  }

  if (clips_reserve(dst, src->count) != 0)
  {
    return -1;
  }

  memcpy(dst->clips, src->clips, src->count * sizeof(*src->clips));
  dst->count = src->count;

  /* the audio is shared, so the copy is a handful of structs whatever the
   * track holds - this is the whole reason undo is affordable */
  for (size_t i = 0; i < dst->count; i++)
  {
    aud_samples_retain(dst->clips[i].audio);
  }
  return 0;
}

uint64_t aud_track_end(const aud_track *t)
{
  if (t == NULL || t->count == 0)
  {
    return 0;
  }
  return clip_end(&t->clips[t->count - 1]);
}

/*
 * The first clip whose end is past `frame`, or count when there is none. The
 * list is sorted, so this is where a scan over a range starts.
 */
static size_t clip_at_or_after(const aud_track *t, uint64_t frame)
{
  size_t lo = 0;
  size_t hi = t->count;

  while (lo < hi)
  {
    size_t mid = lo + (hi - lo) / 2u;

    if (clip_end(&t->clips[mid]) <= frame)
    {
      lo = mid + 1u;
    }
    else
    {
      hi = mid;
    }
  }
  return lo;
}

int aud_track_covered(const aud_track *t, uint64_t frame)
{
  size_t i;

  if (t == NULL || t->count == 0)
  {
    return 0;
  }

  i = clip_at_or_after(t, frame);
  return i < t->count && t->clips[i].start <= frame;
}

void aud_track_range(const aud_track *t, unsigned ch, uint64_t from, uint64_t to,
                     aud_peak *out)
{
  double energy = 0.0;
  uint64_t covered = 0; /* how much of the span any clip accounted for */
  float lo = 0.0f;
  float hi = 0.0f;
  int any = 0;

  if (out == NULL)
  {
    return;
  }

  out->min = 0.0f;
  out->max = 0.0f;
  out->rms = 0.0f;

  if (t == NULL || from >= to)
  {
    return;
  }

  for (size_t i = clip_at_or_after(t, from); i < t->count; i++)
  {
    const aud_clip *c = &t->clips[i];
    uint64_t overlap_from;
    uint64_t overlap_to;
    uint64_t span;
    aud_peak p;

    if (c->start >= to)
    {
      break;
    }

    overlap_from = c->start > from ? c->start : from;
    overlap_to = clip_end(c) < to ? clip_end(c) : to;
    if (overlap_from >= overlap_to)
    {
      continue;
    }

    span = overlap_to - overlap_from;
    covered += span;

    aud_samples_range(c->audio, ch, c->offset + (size_t)(overlap_from - c->start),
                      c->offset + (size_t)(overlap_to - c->start), &p);

    if (!any)
    {
      lo = p.min;
      hi = p.max;
      any = 1;
    }
    else
    {
      lo = p.min < lo ? p.min : lo;
      hi = p.max > hi ? p.max : hi;
    }

    /* back to energy, so the clips of a span add up rather than averaging
     * their averages - two clips of very different lengths would not */
    energy += (double)p.rms * (double)p.rms * (double)span;
  }

  /*
   * Any part of the span no clip covered is silence, and silence is a value.
   * Folding it in is what makes a gap in a take look like a gap rather than
   * like the clips on either side of it joined up. It carries no energy, but it
   * does carry length, which is what pulls the RMS down across a hole.
   */
  if (covered < to - from)
  {
    lo = lo < 0.0f ? lo : 0.0f;
    hi = hi > 0.0f ? hi : 0.0f;
  }

  out->min = lo;
  out->max = hi;
  out->rms = (float)sqrt(energy / (double)(to - from));
}

void aud_track_read(const aud_track *t, uint64_t at, float *interleaved, size_t frames)
{
  if (interleaved == NULL || t == NULL || frames == 0)
  {
    return;
  }

  memset(interleaved, 0, frames * t->channels * sizeof(float));

  for (size_t i = clip_at_or_after(t, at); i < t->count; i++)
  {
    const aud_clip *c = &t->clips[i];
    uint64_t want_to = at + frames;
    uint64_t overlap_from;
    uint64_t overlap_to;
    size_t span;
    size_t dst;
    size_t src;
    unsigned copy;

    if (c->start >= want_to)
    {
      break;
    }

    overlap_from = c->start > at ? c->start : at;
    overlap_to = clip_end(c) < want_to ? clip_end(c) : want_to;
    if (overlap_from >= overlap_to)
    {
      continue;
    }

    span = (size_t)(overlap_to - overlap_from);
    dst = (size_t)(overlap_from - at) * t->channels;
    src = (c->offset + (size_t)(overlap_from - c->start)) * c->audio->channels;

    /*
     * A clip whose block has fewer channels than the track - a mono take
     * dropped on a stereo lane - fills what it has and leaves the rest silent
     * rather than reading past the end of somebody else's frame.
     */
    copy = c->audio->channels < t->channels ? c->audio->channels : t->channels;

    for (size_t f = 0; f < span; f++)
    {
      for (unsigned ch = 0; ch < copy; ch++)
      {
        interleaved[dst + f * t->channels + ch] =
            c->audio->data[src + f * c->audio->channels + ch];
      }
    }
  }
}

int aud_track_add(aud_track *t, aud_samples *audio, uint64_t start)
{
  aud_clip *slot;
  size_t index;

  if (t == NULL || audio == NULL || audio->frames == 0)
  {
    return -1;
  }

  index = clip_at_or_after(t, start);
  if (index < t->count && t->clips[index].start < start + audio->frames)
  {
    return -1; /* it would overlap what is already there */
  }

  slot = clip_insert(t, index);
  if (slot == NULL)
  {
    return -1;
  }

  slot->audio = aud_samples_retain(audio);
  slot->offset = 0;
  slot->frames = audio->frames;
  slot->start = start;
  return 0;
}

int aud_track_split(aud_track *t, uint64_t frame)
{
  size_t i;
  aud_clip *left;
  aud_clip *right;
  size_t before;

  if (t == NULL || t->count == 0)
  {
    return 0;
  }

  i = clip_at_or_after(t, frame);
  if (i >= t->count || t->clips[i].start >= frame)
  {
    return 0; /* a gap, a boundary, or past the end: nothing straddles it */
  }

  before = (size_t)(frame - t->clips[i].start);

  right = clip_insert(t, i + 1u);
  if (right == NULL)
  {
    return -1;
  }
  left = &t->clips[i];

  right->audio = aud_samples_retain(left->audio);
  right->offset = left->offset + before;
  right->frames = left->frames - before;
  right->start = frame;

  left->frames = before;
  return 0;
}

int aud_track_delete(aud_track *t, uint64_t from, uint64_t to, int ripple)
{
  size_t i;

  if (t == NULL || from >= to)
  {
    return 0;
  }

  /*
   * Split at both edges first, and everything after it is whole clips. Doing
   * it this way rather than trimming windows in place is what keeps this
   * function short enough to be obviously right.
   */
  if (aud_track_split(t, from) != 0 || aud_track_split(t, to) != 0)
  {
    return -1;
  }

  i = clip_at_or_after(t, from);
  while (i < t->count && t->clips[i].start < to)
  {
    clip_remove(t, i);
  }

  if (ripple)
  {
    uint64_t by = to - from;

    for (; i < t->count; i++)
    {
      t->clips[i].start -= by;
    }
  }

  aud_track_tidy(t);
  return 0;
}

int aud_track_insert_gap(aud_track *t, uint64_t at, uint64_t frames)
{
  size_t i;

  if (t == NULL || frames == 0)
  {
    return 0;
  }

  if (aud_track_split(t, at) != 0)
  {
    return -1;
  }

  for (i = clip_at_or_after(t, at); i < t->count; i++)
  {
    t->clips[i].start += frames;
  }
  return 0;
}

int aud_track_extract(const aud_track *src, uint64_t from, uint64_t to, aud_track *dst)
{
  if (src == NULL || dst == NULL || from >= to)
  {
    return -1;
  }

  if (aud_track_init(dst, src->name, src->channels) != 0)
  {
    return -1;
  }

  for (size_t i = clip_at_or_after(src, from); i < src->count; i++)
  {
    const aud_clip *c = &src->clips[i];
    uint64_t overlap_from;
    uint64_t overlap_to;
    aud_clip *slot;

    if (c->start >= to)
    {
      break;
    }

    overlap_from = c->start > from ? c->start : from;
    overlap_to = clip_end(c) < to ? clip_end(c) : to;
    if (overlap_from >= overlap_to)
    {
      continue;
    }

    slot = clip_insert(dst, dst->count);
    if (slot == NULL)
    {
      aud_track_free(dst);
      return -1;
    }

    slot->audio = aud_samples_retain(c->audio);
    slot->offset = c->offset + (size_t)(overlap_from - c->start);
    slot->frames = (size_t)(overlap_to - overlap_from);
    slot->start = overlap_from - from;
  }

  return 0;
}

int aud_track_paste(aud_track *t, uint64_t at, const aud_track *src)
{
  uint64_t length;

  if (t == NULL || src == NULL || src->count == 0)
  {
    return 0;
  }

  length = aud_track_end(src);
  if (aud_track_insert_gap(t, at, length) != 0)
  {
    return -1;
  }

  for (size_t i = 0; i < src->count; i++)
  {
    size_t where = clip_at_or_after(t, at + src->clips[i].start);
    aud_clip *slot = clip_insert(t, where);

    if (slot == NULL)
    {
      return -1;
    }

    *slot = src->clips[i];
    slot->start += at;
    aud_samples_retain(slot->audio);
  }

  aud_track_tidy(t);
  return 0;
}

void aud_track_tidy(aud_track *t)
{
  size_t i = 0;

  if (t == NULL)
  {
    return;
  }

  while (i < t->count)
  {
    if (t->clips[i].frames == 0)
    {
      clip_remove(t, i);
      continue;
    }
    i++;
  }

  i = 0;
  while (i + 1u < t->count)
  {
    aud_clip *a = &t->clips[i];
    aud_clip *b = &t->clips[i + 1u];

    /*
     * Joinable only when the two are the same audio read straight through: the
     * end of a split put back together. Two clips that merely touch are left
     * alone, because the boundary between them is a real one.
     */
    if (a->audio == b->audio && clip_end(a) == b->start &&
        a->offset + a->frames == b->offset)
    {
      a->frames += b->frames;
      clip_remove(t, i + 1u);
      continue;
    }
    i++;
  }
}

/* -- recording -------------------------------------------------------------- */

int aud_track_recording(const aud_track *t)
{
  return t != NULL && t->recording >= 0;
}

int aud_track_record_begin(aud_track *t, uint64_t start, size_t capacity_hint)
{
  aud_samples *audio;
  aud_clip *slot;
  size_t index;

  if (t == NULL || t->recording >= 0)
  {
    return -1;
  }

  /* the space has to be free: a take cannot be laid over one already there,
   * and finding that out at the end would be finding it out too late */
  index = clip_at_or_after(t, start);
  if (index < t->count)
  {
    return -1;
  }

  audio = aud_samples_create_empty(t->channels, capacity_hint > 0 ? capacity_hint : 1u);
  if (audio == NULL)
  {
    return -1;
  }

  slot = clip_insert(t, index);
  if (slot == NULL)
  {
    aud_samples_release(audio);
    return -1;
  }

  /* the clip's reference is the only one, which is what lets the block grow */
  slot->audio = audio;
  slot->offset = 0;
  slot->frames = 0;
  slot->start = start;
  t->recording = (long)index;
  return 0;
}

size_t aud_track_record_push(aud_track *t, const float *interleaved, size_t frames)
{
  aud_clip *c;

  if (t == NULL || t->recording < 0 || interleaved == NULL || frames == 0)
  {
    return 0;
  }

  c = &t->clips[t->recording];
  if (aud_samples_reserve(c->audio, frames) != 0)
  {
    return 0;
  }

  memcpy(aud_samples_tail(c->audio), interleaved, frames * t->channels * sizeof(float));
  aud_samples_advance(c->audio, frames);
  c->frames = c->audio->frames;
  return frames;
}

void aud_track_record_end(aud_track *t)
{
  aud_clip *c;

  if (t == NULL || t->recording < 0)
  {
    return;
  }

  c = &t->clips[t->recording];
  aud_samples_index(c->audio); /* the tail bucket, left short while it grew */

  /* pressing record and stopping again without playing anything should leave
   * no trace, rather than a clip of no length for the tidy to find later */
  if (c->frames == 0)
  {
    clip_remove(t, (size_t)t->recording);
  }

  t->recording = -1;
}
