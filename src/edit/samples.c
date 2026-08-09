/* SPDX-License-Identifier: MIT */
#include "edit/samples.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * A ceiling on one block, so a corrupt header or a mistyped length is refused
 * rather than handed to malloc as a number the size of the machine. Sixteen
 * gigabytes of float is far past what anyone will edit in memory and far short
 * of what would wrap the arithmetic below.
 */
#define SAMPLES_MAX_FRAMES ((size_t)1 << 32)

aud_samples *aud_samples_create(unsigned channels, size_t frames)
{
  aud_samples *s;

  if (channels == 0 || frames == 0 || frames > SAMPLES_MAX_FRAMES)
  {
    return NULL;
  }
  if (frames > SIZE_MAX / (channels * sizeof(float)))
  {
    return NULL;
  }

  s = calloc(1, sizeof(*s));
  if (s == NULL)
  {
    return NULL;
  }

  s->data = calloc(frames * channels, sizeof(float));
  if (s->data == NULL)
  {
    free(s);
    return NULL;
  }

  s->refs = 1;
  s->channels = channels;
  s->frames = frames;
  s->capacity = frames;
  return s;
}

aud_samples *aud_samples_create_empty(unsigned channels, size_t capacity)
{
  aud_samples *s = aud_samples_create(channels, capacity);

  if (s != NULL)
  {
    s->frames = 0;
  }
  return s;
}

int aud_samples_reserve(aud_samples *s, size_t extra)
{
  size_t want;
  size_t room;
  float *grown;

  if (s == NULL)
  {
    return -1;
  }

  want = s->frames + extra;
  if (want <= s->capacity)
  {
    return 0;
  }
  if (want > SAMPLES_MAX_FRAMES || want > SIZE_MAX / (s->channels * sizeof(float)))
  {
    return -1;
  }

  /* geometric, so a take that runs for an hour is not an hour of memcpy */
  room = s->capacity == 0 ? want : s->capacity;
  while (room < want)
  {
    room *= 2u;
    if (room > SAMPLES_MAX_FRAMES)
    {
      room = want;
      break;
    }
  }

  grown = realloc(s->data, room * s->channels * sizeof(float));
  if (grown == NULL)
  {
    return -1;
  }

  s->data = grown;
  s->capacity = room;
  return 0;
}

float *aud_samples_tail(aud_samples *s)
{
  if (s == NULL || s->data == NULL)
  {
    return NULL;
  }
  return s->data + s->frames * s->channels;
}

/* Summarise fine buckets [from, to) into coarse bucket `c`. */
static void build_coarse(aud_samples *s, size_t c, size_t from, size_t to)
{
  for (unsigned ch = 0; ch < s->channels; ch++)
  {
    float lo = s->fine[from * s->channels + ch].min;
    float hi = s->fine[from * s->channels + ch].max;
    double energy = 0.0;

    for (size_t b = from; b < to; b++)
    {
      const aud_peak *p = &s->fine[b * s->channels + ch];

      if (p->min < lo)
      {
        lo = p->min;
      }
      if (p->max > hi)
      {
        hi = p->max;
      }
      energy += (double)p->rms * (double)p->rms;
    }

    s->coarse[c * s->channels + ch].min = lo;
    s->coarse[c * s->channels + ch].max = hi;
    s->coarse[c * s->channels + ch].rms = (float)sqrt(energy / (double)(to - from));
  }
}

/* Summarise the samples of one fine bucket, [from, to) of the block. */
static void build_fine(aud_samples *s, size_t b, size_t from, size_t to)
{
  for (unsigned ch = 0; ch < s->channels; ch++)
  {
    float lo = s->data[from * s->channels + ch];
    float hi = lo;
    double energy = 0.0;

    for (size_t f = from; f < to; f++)
    {
      float v = s->data[f * s->channels + ch];

      if (v < lo)
      {
        lo = v;
      }
      if (v > hi)
      {
        hi = v;
      }
      energy += (double)v * (double)v;
    }

    s->fine[b * s->channels + ch].min = lo;
    s->fine[b * s->channels + ch].max = hi;
    s->fine[b * s->channels + ch].rms = (float)sqrt(energy / (double)(to - from));
  }
}

/* Room for `want` fine buckets and the coarse ones over them. */
static int index_reserve(aud_samples *s, size_t want)
{
  size_t coarse_want = (want + AUD_PEAK_COARSE - 1u) / AUD_PEAK_COARSE;
  aud_peak *grown;

  if (want > s->fine_room)
  {
    size_t room = s->fine_room == 0 ? 64u : s->fine_room;

    while (room < want)
    {
      room *= 2u;
    }
    grown = realloc(s->fine, room * s->channels * sizeof(aud_peak));
    if (grown == NULL)
    {
      return -1;
    }
    s->fine = grown;
    s->fine_room = room;
  }

  if (coarse_want > s->coarse_room)
  {
    size_t room = s->coarse_room == 0 ? 8u : s->coarse_room;

    while (room < coarse_want)
    {
      room *= 2u;
    }
    grown = realloc(s->coarse, room * s->channels * sizeof(aud_peak));
    if (grown == NULL)
    {
      return -1;
    }
    s->coarse = grown;
    s->coarse_room = room;
  }

  return 0;
}

/*
 * Index whatever has arrived since last time.
 *
 * `last` says whether the frames that do not fill a whole bucket should be
 * summarised anyway. While a take is being recorded they should not - more of
 * that bucket is on its way - and once it has stopped they must, or the tail of
 * every block would be left to be scanned forever.
 */
static void index_upto(aud_samples *s, int last)
{
  while (s->indexed + AUD_PEAK_BUCKET <= s->frames)
  {
    size_t b = s->indexed / AUD_PEAK_BUCKET;
    size_t c = b / AUD_PEAK_COARSE;

    if (index_reserve(s, b + 1u) != 0)
    {
      return; /* no memory for the index; the readers cope by scanning */
    }

    build_fine(s, b, s->indexed, s->indexed + AUD_PEAK_BUCKET);
    s->indexed += AUD_PEAK_BUCKET;
    s->fine_count = b + 1u;

    /* the coarse bucket this landed in, rebuilt from the fine ones under it */
    build_coarse(s, c, c * AUD_PEAK_COARSE, s->fine_count);
    s->coarse_count = c + 1u;
  }

  if (last && s->indexed < s->frames)
  {
    size_t b = s->indexed / AUD_PEAK_BUCKET;
    size_t c = b / AUD_PEAK_COARSE;

    if (index_reserve(s, b + 1u) != 0)
    {
      return;
    }

    build_fine(s, b, s->indexed, s->frames);
    s->indexed = s->frames;
    s->fine_count = b + 1u;
    build_coarse(s, c, c * AUD_PEAK_COARSE, s->fine_count);
    s->coarse_count = c + 1u;
  }
}

void aud_samples_advance(aud_samples *s, size_t frames)
{
  if (s == NULL || frames == 0)
  {
    return;
  }

  s->frames += frames;
  if (s->frames > s->capacity)
  {
    s->frames = s->capacity;
  }

  /*
   * Only whole buckets. The tail that has not filled one yet is left to the
   * readers, which scan it directly - at most 256 frames of that, which is
   * nothing next to what the buckets are saving them.
   */
  index_upto(s, 0);
}

void aud_samples_index(aud_samples *s)
{
  if (s == NULL || s->frames == 0)
  {
    return;
  }
  index_upto(s, 1);
}

aud_samples *aud_samples_retain(aud_samples *s)
{
  if (s != NULL)
  {
    s->refs++;
  }
  return s;
}

void aud_samples_release(aud_samples *s)
{
  if (s == NULL || --s->refs > 0)
  {
    return;
  }

  free(s->coarse);
  free(s->fine);
  free(s->data);
  free(s);
}

size_t aud_samples_bytes(const aud_samples *s)
{
  size_t bytes;

  if (s == NULL)
  {
    return 0;
  }

  bytes = sizeof(*s) + s->capacity * s->channels * sizeof(float);
  bytes += s->fine_room * s->channels * sizeof(aud_peak);
  bytes += s->coarse_room * s->channels * sizeof(aud_peak);
  return bytes;
}

/*
 * A summary being built up out of pieces.
 *
 * The peaks are the widest of what went in; the RMS cannot be, so the energy
 * and the number of frames it came from are carried and divided out at the end.
 * That is what lets a span be answered from a mixture of whole buckets and the
 * loose samples at either end of them without the answer drifting.
 */
typedef struct
{
  float lo;
  float hi;
  double energy;
  size_t frames;
  int any;
} accum;

static void accum_init(accum *a)
{
  a->lo = 0.0f;
  a->hi = 0.0f;
  a->energy = 0.0;
  a->frames = 0;
  a->any = 0;
}

static void accum_add(accum *a, float lo, float hi, double energy, size_t frames)
{
  if (frames == 0)
  {
    return;
  }

  if (!a->any)
  {
    a->lo = lo;
    a->hi = hi;
    a->any = 1;
  }
  else
  {
    a->lo = lo < a->lo ? lo : a->lo;
    a->hi = hi > a->hi ? hi : a->hi;
  }

  a->energy += energy;
  a->frames += frames;
}

static void accum_finish(const accum *a, aud_peak *out)
{
  out->min = a->lo;
  out->max = a->hi;
  out->rms = a->frames > 0 ? (float)sqrt(a->energy / (double)a->frames) : 0.0f;
}

/* Take in the samples of `ch` over [from, to) directly. */
static void add_samples(accum *a, const aud_samples *s, unsigned ch, size_t from,
                        size_t to)
{
  float lo;
  float hi;
  double energy = 0.0;

  if (from >= to)
  {
    return;
  }

  lo = s->data[from * s->channels + ch];
  hi = lo;

  for (size_t f = from; f < to; f++)
  {
    float v = s->data[f * s->channels + ch];

    if (v < lo)
    {
      lo = v;
    }
    if (v > hi)
    {
      hi = v;
    }
    energy += (double)v * (double)v;
  }

  accum_add(a, lo, hi, energy, to - from);
}

/* Take in buckets [first, last) of `level`, each summarising `grain` frames. */
static void add_buckets(accum *a, const aud_peak *level, unsigned channels, unsigned ch,
                        size_t first, size_t last, size_t grain)
{
  for (size_t b = first; b < last; b++)
  {
    const aud_peak *p = &level[b * channels + ch];

    accum_add(a, p->min, p->max, (double)p->rms * (double)p->rms * (double)grain, grain);
  }
}

/* The body of aud_samples_range(), which the coarse path re-enters for its
 * partial ends rather than repeating the whole decision. */
static void add_range(accum *a, const aud_samples *s, unsigned ch, size_t from, size_t to)
{
  size_t span;

  if (to > s->frames)
  {
    to = s->frames;
  }
  if (from >= to)
  {
    return;
  }

  /*
   * Past what the index covers there is nothing to read but the samples. That
   * is at most one bucket for a finished block, and the tail of whatever is
   * still being recorded for one that is not.
   */
  if (to > s->indexed)
  {
    size_t tail = from > s->indexed ? from : s->indexed;

    add_samples(a, s, ch, tail, to);
    to = s->indexed;
    if (from >= to)
    {
      return;
    }
  }
  span = to - from;

  if (s->fine == NULL || span < 2u * AUD_PEAK_BUCKET)
  {
    add_samples(a, s, ch, from, to);
    return;
  }

  if (span < 2u * AUD_PEAK_BUCKET * AUD_PEAK_COARSE)
  {
    size_t first = (from + AUD_PEAK_BUCKET - 1u) / AUD_PEAK_BUCKET;
    size_t last = to / AUD_PEAK_BUCKET;

    /* the partial buckets at either end are read as samples, so the answer is
     * exact rather than rounded out to the bucket boundary */
    add_samples(a, s, ch, from, first * AUD_PEAK_BUCKET);
    add_buckets(a, s->fine, s->channels, ch, first, last, AUD_PEAK_BUCKET);
    add_samples(a, s, ch, last * AUD_PEAK_BUCKET, to);
    return;
  }

  {
    size_t grain = (size_t)AUD_PEAK_BUCKET * AUD_PEAK_COARSE;
    size_t first = (from + grain - 1u) / grain;
    size_t last = to / grain;

    /* the ends fall to the level below, which does the same to its own ends */
    add_range(a, s, ch, from, first * grain);
    add_buckets(a, s->coarse, s->channels, ch, first, last, grain);
    add_range(a, s, ch, last * grain, to);
  }
}

void aud_samples_range(const aud_samples *s, unsigned ch, size_t from, size_t to,
                       aud_peak *out)
{
  accum a;

  if (out == NULL)
  {
    return;
  }

  accum_init(&a);
  if (s != NULL && ch < s->channels)
  {
    add_range(&a, s, ch, from, to);
  }
  accum_finish(&a, out);
}
