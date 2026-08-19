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
 * Linear in amplitude, not in decibels. A fade to silence has no bottom on a
 * dB scale to start from, and linear is what every editor draws and what a
 * hand on a fader does.
 *
 * The ramp is open at the silent end - frame 0 of an n frame fade-in is 0.0 and
 * frame n is already past it at 1.0 - so a fade of one frame is a single
 * silent sample rather than a fade that does nothing.
 */
float aud_clip_fade_gain(const aud_clip *c, size_t frame)
{
  float gain = 1.0f;

  if (c == NULL || frame >= c->frames)
  {
    return gain;
  }

  if (c->fade_in > 0 && frame < c->fade_in)
  {
    gain = (float)frame / (float)c->fade_in;
  }

  if (c->fade_out > 0 && c->frames - frame <= c->fade_out)
  {
    gain *= (float)(c->frames - frame - 1u) / (float)c->fade_out;
  }

  return gain;
}

/* The mean of the ramp over [from, to) of a clip, for summarising a span. */
static float fade_mean(const aud_clip *c, size_t from, size_t to)
{
  double sum = 0.0;
  size_t steps;
  size_t step;

  if (c->fade_in == 0 && c->fade_out == 0)
  {
    return 1.0f;
  }
  if (from >= to)
  {
    return 1.0f;
  }

  /*
   * Sampled rather than integrated: this feeds a waveform column, and eight
   * points across a span describe a straight line more than well enough.
   */
  steps = to - from < 8u ? to - from : 8u;
  for (step = 0; step < steps; step++)
  {
    sum += (double)aud_clip_fade_gain(c, from + (to - from) * step / steps);
  }
  return (float)(sum / (double)steps);
}

/* Hold a gain to what the model can say, whatever asked for it. */
static float clamp_gain(float gain)
{
  if (!(gain > 0.0f))
  {
    return 0.0f; /* and a NaN lands here rather than propagating through a mix */
  }
  return gain > AUD_CLIP_GAIN_MAX ? AUD_CLIP_GAIN_MAX : gain;
}

/* Hold both fades inside the clip they belong to, after it has been resized. */
static void clip_clamp_fades(aud_clip *c)
{
  if (c->fade_in > c->frames)
  {
    c->fade_in = c->frames;
  }
  if (c->fade_out > c->frames)
  {
    c->fade_out = c->frames;
  }
}

/*
 * Make room for a clip at `index` and return it, uninitialised. The caller
 * fills it in and is responsible for the sort order still holding.
 *
 * `recording` is a position in this list, so it moves with the list. Splitting
 * or deleting on a lane that is being recorded into is a perfectly ordinary
 * thing to do from the window, and an index left pointing at whatever slid into
 * its place would send the next captured period to the wrong clip - or, once
 * the list had shrunk past it, off the end of it.
 */
static aud_clip *clip_insert(aud_track *t, size_t index)
{
  if (clips_reserve(t, t->count + 1) != 0)
  {
    return NULL;
  }

  memmove(&t->clips[index + 1], &t->clips[index], (t->count - index) * sizeof(*t->clips));
  t->count++;

  if (t->recording >= (long)index)
  {
    t->recording++;
  }
  return &t->clips[index];
}

static void clip_remove(aud_track *t, size_t index)
{
  aud_samples_release(t->clips[index].audio);
  memmove(&t->clips[index], &t->clips[index + 1],
          (t->count - index - 1) * sizeof(*t->clips));
  t->count--;

  /* the take's own clip going away ends the take, rather than moving it onto
   * whichever clip inherited the index */
  if (t->recording == (long)index)
  {
    t->recording = -1;
  }
  else if (t->recording > (long)index)
  {
    t->recording--;
  }
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

uint64_t aud_track_edge_after(const aud_track *t, uint64_t frame)
{
  if (t == NULL)
  {
    return frame;
  }

  /* sorted and non-overlapping, so the first edge past `frame` is the answer */
  for (size_t i = clip_at_or_after(t, frame); i < t->count; i++)
  {
    if (t->clips[i].start > frame)
    {
      return t->clips[i].start;
    }
    if (clip_end(&t->clips[i]) > frame)
    {
      return clip_end(&t->clips[i]);
    }
  }
  return frame;
}

uint64_t aud_track_edge_before(const aud_track *t, uint64_t frame)
{
  uint64_t best = frame;

  if (t == NULL || frame == 0)
  {
    return frame;
  }

  /* walking forward and keeping the last one under `frame`: the list is short
   * next to the audio it describes, and this needs no second index */
  for (size_t i = 0; i < t->count; i++)
  {
    uint64_t start = t->clips[i].start;
    uint64_t end = clip_end(&t->clips[i]);

    if (start >= frame)
    {
      break;
    }
    best = start;
    if (end < frame)
    {
      best = end;
    }
  }
  return best;
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

    /*
     * Scaled by the ramp and the gain over the same span, so the waveform shows
     * both. A fade or a gain the ear could hear but the eye could not would
     * make the display the one thing in the editor that was lying.
     */
    if (c->fade_in > 0 || c->fade_out > 0 || c->gain != 1.0f)
    {
      float k = c->gain * fade_mean(c, (size_t)(overlap_from - c->start),
                                    (size_t)(overlap_to - c->start));

      p.min *= k;
      p.max *= k;
      p.rms *= k;
    }

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

    /* where in the clip this window starts, which is what the ramp counts from */
    {
      size_t head = (size_t)(overlap_from - c->start);
      int fading = c->fade_in > 0 || c->fade_out > 0;

      for (size_t f = 0; f < span; f++)
      {
        float k = fading ? c->gain * aud_clip_fade_gain(c, head + f) : c->gain;

        for (unsigned ch = 0; ch < copy; ch++)
        {
          interleaved[dst + f * t->channels + ch] =
              c->audio->data[src + f * c->audio->channels + ch] * k;
        }
      }
    }
  }
}

int aud_track_place(aud_track *t, aud_samples *audio, size_t offset, size_t frames,
                    uint64_t start, size_t fade_in, size_t fade_out)
{
  aud_clip *slot;
  size_t index;

  if (t == NULL || audio == NULL || frames == 0)
  {
    return -1;
  }
  /* the window has to be inside the block, whatever a file may have claimed */
  if (offset > audio->frames || frames > audio->frames - offset)
  {
    return -1;
  }
  /*
   * And it has to end somewhere. A start near the top of the range - which a
   * hand-edited project can name - would wrap clip_end() round to a number
   * below the clip's own start, and the sorted, non-overlapping list every
   * walk here assumes would be neither.
   */
  if (start > UINT64_MAX - (uint64_t)frames)
  {
    return -1;
  }

  index = clip_at_or_after(t, start);
  if (index < t->count && t->clips[index].start < start + frames)
  {
    return -1; /* it would overlap what is already there */
  }

  slot = clip_insert(t, index);
  if (slot == NULL)
  {
    return -1;
  }

  slot->audio = aud_samples_retain(audio);
  slot->offset = offset;
  slot->frames = frames;
  slot->start = start;
  slot->fade_in = fade_in;
  slot->fade_out = fade_out;
  slot->gain = 1.0f;
  clip_clamp_fades(slot);
  return 0;
}

int aud_track_add(aud_track *t, aud_samples *audio, uint64_t start)
{
  if (audio == NULL)
  {
    return -1;
  }
  return aud_track_place(t, audio, 0, audio->frames, start, 0, 0);
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

  /*
   * A take still arriving grows the block at its end, so the half that owns
   * that end is the half the frames belong to. Leaving the take on the left
   * half would have the next captured period stretch it back over the right
   * one, and the sorted, non-overlapping list everything else here assumes
   * would stop being either.
   */
  if (t->recording == (long)i)
  {
    t->recording = (long)i + 1;
  }

  right->audio = aud_samples_retain(left->audio);
  right->offset = left->offset + before;
  right->frames = left->frames - before;
  right->start = frame;

  /*
   * Each half keeps the fade at the end it still owns. A cut that lands inside
   * a ramp truncates it: the second half would need to start part way up a
   * fade, and a clip can only say "ramp from silence".
   */
  right->fade_in = 0;
  right->fade_out = left->fade_out;
  /* both halves are still the same piece, and turned the same way */
  right->gain = left->gain;
  left->fade_out = 0;
  left->frames = before;
  clip_clamp_fades(left);
  clip_clamp_fades(right);
  return 0;
}

/* The clip that starts exactly at `frame`, or NULL. */
static aud_clip *clip_starting_at(aud_track *t, uint64_t frame)
{
  size_t i;

  if (t == NULL)
  {
    return NULL;
  }

  i = clip_at_or_after(t, frame);
  return (i < t->count && t->clips[i].start == frame) ? &t->clips[i] : NULL;
}

/* The clip that ends exactly at `frame`, or NULL. */
static aud_clip *clip_ending_at(aud_track *t, uint64_t frame)
{
  if (t == NULL || frame == 0)
  {
    return NULL;
  }

  for (size_t i = clip_at_or_after(t, frame - 1u); i < t->count; i++)
  {
    if (t->clips[i].start >= frame)
    {
      break;
    }
    if (clip_end(&t->clips[i]) == frame)
    {
      return &t->clips[i];
    }
  }
  return NULL;
}

int aud_track_fade_in_at(aud_track *t, uint64_t frame, size_t frames)
{
  aud_clip *c = clip_starting_at(t, frame);

  if (c == NULL)
  {
    return -1;
  }

  c->fade_in = frames;
  clip_clamp_fades(c);
  return 0;
}

int aud_track_fade_out_at(aud_track *t, uint64_t frame, size_t frames)
{
  aud_clip *c = clip_ending_at(t, frame);

  if (c == NULL)
  {
    return -1;
  }

  c->fade_out = frames;
  clip_clamp_fades(c);
  return 0;
}

int aud_track_gain_scale(aud_track *t, uint64_t from, uint64_t to, float by)
{
  size_t first;
  int any = 0;

  if (t == NULL || from >= to)
  {
    return -1;
  }

  /*
   * A range with no audio in it is not something to split for. Answering
   * before touching anything is what lets a caller that got -1 say the track
   * is exactly as it was, rather than as it was but cut in two places.
   */
  first = clip_at_or_after(t, from);
  if (first >= t->count || t->clips[first].start >= to)
  {
    return -1;
  }

  /*
   * Split at both edges and everything between them is a whole clip, the way
   * a delete does it. Without the splits the change would spill over whatever
   * clip happened to straddle the selection.
   */
  if (aud_track_split(t, from) != 0 || aud_track_split(t, to) != 0)
  {
    return -1;
  }

  for (size_t i = clip_at_or_after(t, from); i < t->count; i++)
  {
    aud_clip *c = &t->clips[i];

    if (c->start >= to)
    {
      break;
    }

    c->gain = clamp_gain(c->gain * by);
    any = 1;
  }

  return any ? 0 : -1;
}

int aud_track_gain_at(aud_track *t, uint64_t frame, float gain)
{
  aud_clip *c = clip_starting_at(t, frame);

  if (c == NULL)
  {
    return -1;
  }

  c->gain = clamp_gain(gain);
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

/*
 * Negating `by` to subtract it would overflow at INT64_MIN, which is a value a
 * drag cannot produce but a project file could ask for. Negating `by + 1`
 * cannot overflow, and the frame it is short by goes back on as an unsigned one.
 */
uint64_t aud_frame_offset(uint64_t frame, int64_t by)
{
  if (by >= 0)
  {
    return frame + (uint64_t)by;
  }
  return frame - (uint64_t)(-(by + 1)) - 1u;
}

/* A distance in frames, as far as an int64_t can carry it. */
static int64_t as_offset(uint64_t frames)
{
  return frames > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)frames;
}

int64_t aud_track_move_room(const aud_track *t, uint64_t from, uint64_t to, int64_t want)
{
  uint64_t behind = 0; /* where the last obstacle before the range ends */
  uint64_t ahead = 0;  /* where the first one after it starts */
  int stopped_ahead = 0;

  if (t == NULL || from >= to || want == 0)
  {
    return 0;
  }

  for (size_t i = 0; i < t->count; i++)
  {
    const aud_clip *c = &t->clips[i];
    uint64_t end = clip_end(c);

    /*
     * Each clip is looked at for the parts of it outside the range, which are
     * the parts that stay put. A clip wholly inside has neither and is going to
     * move; one that straddles an edge has the piece a split will leave behind,
     * hard against that edge.
     */
    if (c->start < from)
    {
      uint64_t stops = end < from ? end : from;

      behind = stops > behind ? stops : behind;
    }

    /* sorted and non-overlapping, so the first one past the range is the nearest */
    if (!stopped_ahead && end > to)
    {
      ahead = c->start > to ? c->start : to;
      stopped_ahead = 1;
    }
  }

  if (want > 0)
  {
    int64_t room = stopped_ahead ? as_offset(ahead - to) : INT64_MAX;

    return want < room ? want : room;
  }

  /* the clear space before the range, which is as far back as it can go */
  {
    int64_t room = as_offset(from - behind);

    return want > -room ? want : -room;
  }
}

int aud_track_move(aud_track *t, uint64_t from, uint64_t to, int64_t by)
{
  size_t first;
  size_t past;
  size_t moving;
  size_t where;
  aud_clip *lifted;

  if (t == NULL || from >= to || by == 0)
  {
    return 0;
  }

  /*
   * Not while a take is arriving on this lane. The clip being recorded into
   * grows at its end and the block behind it has exactly one owner - moving
   * either that clip or the ground under it is not something to work out
   * halfway through a performance.
   */
  if (aud_track_recording(t))
  {
    return -1;
  }

  /* the caller makes room before asking, because a move across several tracks
   * has to be the same distance on all of them */
  if (aud_track_move_room(t, from, to, by) != by)
  {
    return -1;
  }

  if (aud_track_split(t, from) != 0 || aud_track_split(t, to) != 0)
  {
    return -1;
  }

  first = clip_at_or_after(t, from);
  past = first;
  while (past < t->count && t->clips[past].start < to)
  {
    past++;
  }
  moving = past - first;
  if (moving == 0)
  {
    return 0; /* the range held nothing but silence, and silence moves by itself */
  }

  lifted = malloc(moving * sizeof(*lifted));
  if (lifted == NULL)
  {
    return -1;
  }
  memcpy(lifted, &t->clips[first], moving * sizeof(*lifted));

  /*
   * Out of the list and back into it, rather than resorted in place: the clips
   * keep their references throughout - they are the same clips over the same
   * blocks, laid down somewhere else - so clip_remove(), which releases one, is
   * not what this wants. The list has room for them by definition, having just
   * had them taken out of it.
   */
  memmove(&t->clips[first], &t->clips[past], (t->count - past) * sizeof(*t->clips));
  t->count -= moving;

  for (size_t i = 0; i < moving; i++)
  {
    lifted[i].start = aud_frame_offset(lifted[i].start, by);
  }

  /* the ground checked clear above, so nothing here can straddle the landing */
  where = clip_at_or_after(t, lifted[0].start);
  memmove(&t->clips[where + moving], &t->clips[where],
          (t->count - where) * sizeof(*t->clips));
  memcpy(&t->clips[where], lifted, moving * sizeof(*lifted));
  t->count += moving;
  free(lifted);

  /* a piece put back where it was split from is one clip again */
  aud_track_tidy(t);
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

    /* the fade at an end the extract kept, and nothing at an end it cut */
    slot->fade_in = overlap_from == c->start ? c->fade_in : 0;
    slot->fade_out = overlap_to == clip_end(c) ? c->fade_out : 0;
    slot->gain = c->gain; /* how loud it is belongs to all of it, not to an end */
    clip_clamp_fades(slot);
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
     * alone, because the boundary between them is a real one - and so is a
     * fade at the join or a gain that differs across it, either of which
     * merging would silently throw away.
     */
    if (a->audio == b->audio && clip_end(a) == b->start &&
        a->offset + a->frames == b->offset && a->fade_out == 0 && b->fade_in == 0 &&
        a->gain == b->gain)
    {
      a->frames += b->frames;
      a->fade_out = b->fade_out;
      clip_remove(t, i + 1u);
      clip_clamp_fades(a);
      continue;
    }
    i++;
  }
}

/* -- recording -------------------------------------------------------------- */

int aud_track_recording(const aud_track *t)
{
  return t != NULL && t->recording >= 0 && (size_t)t->recording < t->count;
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
  slot->fade_in = 0;
  slot->fade_out = 0;
  slot->gain = 1.0f;
  t->recording = (long)index;
  return 0;
}

size_t aud_track_record_push(aud_track *t, const float *interleaved, size_t frames)
{
  aud_clip *c;

  /* aud_track_recording() rather than the field, so an index the clip list has
   * outgrown can never be dereferenced; see clip_insert() */
  if (!aud_track_recording(t) || interleaved == NULL || frames == 0)
  {
    return 0;
  }

  c = &t->clips[t->recording];
  /*
   * A block narrower than the lane is a legal thing to hold - aud_track_read()
   * plays one and leaves the rest silent - but it is not a thing to grow: the
   * reserve below counts in the block's frames and the copy in the lane's, so a
   * mismatch writes past the end of what was reserved.
   */
  if (c->audio == NULL || c->audio->channels != t->channels)
  {
    return 0;
  }
  if (aud_samples_reserve(c->audio, frames) != 0)
  {
    return 0;
  }

  memcpy(aud_samples_tail(c->audio), interleaved, frames * t->channels * sizeof(float));
  aud_samples_advance(c->audio, frames);
  /* the clip is the window [offset, end) of the block, which a split moves off
   * zero - so its length is what is past the offset, not the whole block */
  c->frames = c->audio->frames - c->offset;
  return frames;
}

int aud_track_record_continue(aud_track *t, uint64_t at)
{
  aud_clip *last;

  if (t == NULL || t->recording >= 0 || t->count == 0)
  {
    return -1;
  }

  last = &t->clips[t->count - 1u];
  if (last->start + last->frames != at)
  {
    return -1; /* something was edited in the gap; this is not the same tail */
  }

  /*
   * A block anything else is reading cannot be grown: a split or a copy leaves
   * two clips over one block, and appending would change what the other one
   * plays. One reference means this clip is the only owner.
   *
   * Nor one that is not the lane's width. A paste across lanes, or a project
   * file that names a channel count its clips do not have, can leave the tail
   * of a lane holding a block of another shape - and the take about to arrive
   * is the lane's width, which is not what such a block has room for.
   */
  if (last->audio == NULL || last->audio->refs != 1 ||
      last->audio->channels != t->channels)
  {
    return -1;
  }

  /*
   * The tail bucket was indexed when the take stopped and is about to be
   * short again; aud_track_record_end() indexes it a second time, which is
   * what it does after any take.
   */
  t->recording = (long)(t->count - 1u);
  return 0;
}

void aud_track_record_end(aud_track *t)
{
  aud_clip *c;

  if (!aud_track_recording(t))
  {
    if (t != NULL)
    {
      t->recording = -1;
    }
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
