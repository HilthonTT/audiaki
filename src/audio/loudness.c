/* SPDX-License-Identifier: MIT */
#include "audio/loudness.h"

#include "audio/truepeak.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOUD_PI 3.14159265358979323846

/*
 * The blocks the measurement is defined over, in units of the 100 ms sub-block
 * both of them step by. Keeping one history of sub-blocks and summing runs of
 * it is what lets the 400 ms and the 3 s windows share a single pass: their
 * overlaps are 75% and 96.7%, and 100 ms is the step that expresses both.
 */
#define LOUD_SUBS_PER_SEC 10u
#define LOUD_MOMENTARY_SUBS 4u /* 400 ms */
#define LOUD_SHORT_SUBS 30u    /* 3 s */

/* BS.1770-4: loudness is this plus 10 log10 of the summed channel powers. */
#define LOUD_OFFSET (-0.691)

/*
 * The gates. The absolute one drops the silence between the notes, so a take
 * with long gaps is not rated quieter than the same playing without them. The
 * relative one then drops whatever sits far below the take's own average, which
 * is what stops a fade-in or a count-off dragging the figure down.
 */
#define LOUD_ABSOLUTE_GATE (-70.0)
#define LOUD_RELATIVE_GATE (-10.0) /* BS.1770-4, for the integrated loudness */
#define LOUD_RANGE_GATE (-20.0)    /* EBU Tech 3342, for the range */

/* The range is the spread between these two percentiles of what survives. */
#define LOUD_RANGE_LOW 0.10
#define LOUD_RANGE_HIGH 0.95

/* How many blocks the history starts with room for: about a minute of them. */
#define LOUD_BLOCKS_MIN 1024u

/*
 * The true peak interpolator lives in audio/truepeak.h, where the limiter can
 * reach it too - a limiter judged by a different filter from this one would be
 * a take reported over a ceiling it had just been put under. Its width is the
 * only thing this file still needs a name for: the buffer below is kept in
 * groups of it.
 */
#define LOUD_TP_TAPS AUD_TRUEPEAK_TAPS

typedef struct
{
  double b0, b1, b2, a1, a2;
} loud_biquad;

struct aud_loudness
{
  unsigned rate;
  unsigned channels;
  size_t sub_frames; /* frames to a 100 ms sub-block */

  /* the two halves of the K-weighting, and four state values a channel */
  loud_biquad shelf;
  loud_biquad highpass;
  double *state;

  /* the last 3 s of sub-block power, as a ring; both windows read out of it */
  double sub[LOUD_SHORT_SUBS];
  size_t sub_at;
  size_t sub_count; /* how many have ever completed, not how many are held */
  double sub_acc;   /* the one being filled */
  size_t sub_have;

  /* mean square per completed block, which is what the gating works over */
  double *momentary;
  size_t momentary_count;
  size_t momentary_cap;
  double *shortterm;
  size_t shortterm_count;
  size_t shortterm_cap;

  /*
   * The most recent of each, kept beside the histories rather than read off
   * their ends. range_of() reorders the short-term history, and a live meter
   * that took the last element would start reporting whichever block happened
   * to sort last the moment anything called aud_loudness_read().
   */
  double momentary_now;
  double shortterm_now;

  aud_truepeak tp;

  /*
   * Per channel, the previous group of samples followed by the group being
   * filled, so every window the filter reads is contiguous - and so a whole
   * group can be dismissed at once. `tp_have` is how far into the second half
   * the filling has got, and is the same for every channel.
   */
  float *tp_buf; /* channels * 2 * LOUD_TP_TAPS */
  float *tp_prev_max;
  float *tp_cur_max;
  size_t tp_have;
  double true_peak;
};

int aud_loudness_measured(double lufs)
{
  return lufs > AUD_LUFS_NONE;
}

int aud_loudness_supported(unsigned rate, unsigned channels)
{
  return rate >= AUD_LOUDNESS_MIN_RATE && channels > 0 &&
         channels <= AUD_LOUDNESS_MAX_CHANNELS;
}

/* -- the ear, as two filters ----------------------------------------------- */

/*
 * The K-weighting, derived at `rate` rather than tabulated.
 *
 * BS.1770 prints its coefficients at 48 kHz only, and a take is as likely to be
 * at 44.1. The analogue prototypes behind that table are these frequencies and
 * Q values, and re-deriving them through the bilinear transform at the rate in
 * hand is what makes the measurement rate-independent - which it has to be, or
 * the same performance recorded at two rates would not read the same.
 *
 * The first is a high shelf, +4 dB above about 1.7 kHz, standing in for the way
 * a head in a sound field lifts the top. The second is a high pass at 38 Hz,
 * standing in for how little the ear makes of what is below it. Together they
 * are why this rates a bright take above a boomy one that measures the same in
 * RMS.
 */
static void design(aud_loudness *l)
{
  double f0 = 1681.974450955533;
  double gain = 3.999843853973347;
  double q = 0.7071752369554196;
  double k = tan(LOUD_PI * f0 / (double)l->rate);
  double vh = pow(10.0, gain / 20.0);
  double vb = pow(vh, 0.4996667741545416);
  double a0 = 1.0 + k / q + k * k;

  l->shelf.b0 = (vh + vb * k / q + k * k) / a0;
  l->shelf.b1 = 2.0 * (k * k - vh) / a0;
  l->shelf.b2 = (vh - vb * k / q + k * k) / a0;
  l->shelf.a1 = 2.0 * (k * k - 1.0) / a0;
  l->shelf.a2 = (1.0 - k / q + k * k) / a0;

  f0 = 38.13547087602444;
  q = 0.5003270373238773;
  k = tan(LOUD_PI * f0 / (double)l->rate);
  a0 = 1.0 + k / q + k * k;

  l->highpass.b0 = 1.0;
  l->highpass.b1 = -2.0;
  l->highpass.b2 = 1.0;
  l->highpass.a1 = 2.0 * (k * k - 1.0) / a0;
  l->highpass.a2 = (1.0 - k / q + k * k) / a0;
}

/* -- the meter ------------------------------------------------------------- */

aud_loudness *aud_loudness_create(unsigned rate, unsigned channels)
{
  aud_loudness *l;

  if (!aud_loudness_supported(rate, channels))
  {
    errno = EINVAL;
    return NULL;
  }

  l = calloc(1, sizeof(*l));
  if (l == NULL)
  {
    errno = ENOMEM;
    return NULL;
  }

  l->rate = rate;
  l->channels = channels;
  /*
   * A rate that is not a multiple of ten leaves a few frames over at the end of
   * every second. They land in the part-block that is discarded anyway, and the
   * alternative - sub-blocks that differ in length by a frame - would make the
   * mean of four of them no longer the mean over 400 ms.
   */
  l->sub_frames = rate / LOUD_SUBS_PER_SEC;

  l->state = calloc((size_t)channels * 4u, sizeof(*l->state));
  l->tp_buf = calloc((size_t)channels * 2u * LOUD_TP_TAPS, sizeof(*l->tp_buf));
  l->tp_prev_max = calloc(channels, sizeof(*l->tp_prev_max));
  l->tp_cur_max = calloc(channels, sizeof(*l->tp_cur_max));
  if (l->state == NULL || l->tp_buf == NULL || l->tp_prev_max == NULL ||
      l->tp_cur_max == NULL)
  {
    aud_loudness_destroy(l);
    errno = ENOMEM;
    return NULL;
  }

  design(l);
  aud_truepeak_build(&l->tp);
  return l;
}

void aud_loudness_destroy(aud_loudness *l)
{
  if (l == NULL)
  {
    return;
  }
  free(l->state);
  free(l->tp_buf);
  free(l->tp_prev_max);
  free(l->tp_cur_max);
  free(l->momentary);
  free(l->shortterm);
  free(l);
}

static int push(double **arr, size_t *count, size_t *cap, double value)
{
  if (*count == *cap)
  {
    size_t want = *cap > 0 ? *cap * 2u : LOUD_BLOCKS_MIN;
    double *bigger = realloc(*arr, want * sizeof(**arr));

    if (bigger == NULL)
    {
      errno = ENOMEM;
      return -1;
    }
    *arr = bigger;
    *cap = want;
  }

  (*arr)[(*count)++] = value;
  return 0;
}

/* The mean of the `subs` most recent sub-blocks, which is the power of the
 * block ending at the one just completed. */
static double window_power(const aud_loudness *l, size_t subs)
{
  double sum = 0.0;

  for (size_t i = 0; i < subs; i++)
  {
    /* sub_at has already moved past the newest, so step back from there */
    size_t at = (l->sub_at + LOUD_SHORT_SUBS - 1u - i) % LOUD_SHORT_SUBS;

    sum += l->sub[at];
  }
  return sum / (double)subs;
}

/* A completed sub-block, and the blocks that ending it may complete. */
static int close_sub_block(aud_loudness *l)
{
  l->sub[l->sub_at] = l->sub_acc / (double)l->sub_frames;
  l->sub_at = (l->sub_at + 1u) % LOUD_SHORT_SUBS;
  l->sub_count++;
  l->sub_acc = 0.0;
  l->sub_have = 0;

  /*
   * The filter states decay towards zero through a silent passage and would
   * reach the denormal range in a few seconds of it, where the arithmetic slows
   * down by orders of magnitude on most hardware. Flushing them here rather
   * than per sample costs one pass every 100 ms and catches it long before it
   * matters, because reaching that range from any audible level takes seconds.
   */
  for (size_t i = 0; i < (size_t)l->channels * 4u; i++)
  {
    if (l->state[i] > -1e-30 && l->state[i] < 1e-30)
    {
      l->state[i] = 0.0;
    }
  }

  if (l->sub_count >= LOUD_MOMENTARY_SUBS)
  {
    l->momentary_now = window_power(l, LOUD_MOMENTARY_SUBS);
    if (push(&l->momentary, &l->momentary_count, &l->momentary_cap, l->momentary_now) !=
        0)
    {
      return -1;
    }
  }
  if (l->sub_count >= LOUD_SHORT_SUBS)
  {
    l->shortterm_now = window_power(l, LOUD_SHORT_SUBS);
    if (push(&l->shortterm, &l->shortterm_count, &l->shortterm_cap, l->shortterm_now) !=
        0)
    {
      return -1;
    }
  }
  return 0;
}

/*
 * Look between the samples of `positions` windows of one channel's buffer, and
 * keep the largest thing found there.
 *
 * Phase 0 is not run: it is the identity, so it would only rediscover the
 * sample the window ends on - and those are taken as they arrive, which is both
 * cheaper and does not depend on the filter having that property.
 */
static void interpolate(aud_loudness *l, const float *buf, size_t positions)
{
  float best = 0.0f;

  for (size_t i = 0; i < positions; i++)
  {
    const float *window = buf + i + 1u;

    float between = aud_truepeak_between(&l->tp, window);

    if (between > best)
    {
      best = between;
    }
  }

  if ((double)best > l->true_peak)
  {
    l->true_peak = (double)best;
  }
}

/*
 * A group of samples is complete: look between them if anything there could
 * beat what has been found, and make it the previous group.
 *
 * The test is what keeps this affordable. A window never spans more than two
 * groups, so the largest sample any window in this one can hold is known, and
 * the filter cannot lift it past the interpolator's bound times itself. Below the peak so
 * far, there is provably nothing here to find and the filtering is skipped outright. On a
 * real take that is nearly every group - the loudest moment is one moment - which turns
 * the true peak from the most expensive thing in the measurement into one of the
 * cheapest, without approximating it anywhere.
 */
static void close_tp_group(aud_loudness *l)
{
  for (unsigned c = 0; c < l->channels; c++)
  {
    float *buf = l->tp_buf + (size_t)c * 2u * LOUD_TP_TAPS;
    float loudest =
        l->tp_prev_max[c] > l->tp_cur_max[c] ? l->tp_prev_max[c] : l->tp_cur_max[c];

    if (l->tp.bound * (double)loudest > l->true_peak)
    {
      interpolate(l, buf, LOUD_TP_TAPS);
    }

    /* memmove rather than memcpy: the two halves of one array cannot overlap
     * at this size, but both pointers are into the same object, which is all a
     * static analyser can see - and for twelve floats the two are the same */
    memmove(buf, buf + LOUD_TP_TAPS, LOUD_TP_TAPS * sizeof(*buf));
    l->tp_prev_max[c] = l->tp_cur_max[c];
    l->tp_cur_max[c] = 0.0f;
  }
  l->tp_have = 0;
}

int aud_loudness_feed(aud_loudness *l, const float *interleaved, size_t frames)
{
  unsigned channels;

  if (l == NULL || (interleaved == NULL && frames > 0))
  {
    errno = EINVAL;
    return -1;
  }
  channels = l->channels;

  for (size_t f = 0; f < frames; f++)
  {
    const float *frame = interleaved + f * channels;
    size_t slot = LOUD_TP_TAPS + l->tp_have;

    for (unsigned c = 0; c < channels; c++)
    {
      double x = (double)frame[c];
      double *st = l->state + (size_t)c * 4u;
      double shelved;
      double weighted;
      float magnitude = fabsf(frame[c]);

      /* both stages in transposed direct form II, which needs two states and
       * keeps the rounding on the summing node rather than in a delay line */
      shelved = l->shelf.b0 * x + st[0];
      st[0] = l->shelf.b1 * x - l->shelf.a1 * shelved + st[1];
      st[1] = l->shelf.b2 * x - l->shelf.a2 * shelved;

      weighted = l->highpass.b0 * shelved + st[2];
      st[2] = l->highpass.b1 * shelved - l->highpass.a1 * weighted + st[3];
      st[3] = l->highpass.b2 * shelved - l->highpass.a2 * weighted;

      /* every channel at full weight; see aud_loudness_create */
      l->sub_acc += weighted * weighted;

      /*
       * The true peak runs off the unweighted sample, because it is about what
       * a converter will have to reproduce rather than about what it sounds
       * like. The sample itself counts towards it directly - see interpolate() -
       * and is kept for the filtering that may follow the group it is in.
       */
      l->tp_buf[(size_t)c * 2u * LOUD_TP_TAPS + slot] = frame[c];
      if (magnitude > l->tp_cur_max[c])
      {
        l->tp_cur_max[c] = magnitude;
      }
      if ((double)magnitude > l->true_peak)
      {
        l->true_peak = (double)magnitude;
      }
    }

    if (++l->tp_have == LOUD_TP_TAPS)
    {
      close_tp_group(l);
    }

    if (++l->sub_have == l->sub_frames && close_sub_block(l) != 0)
    {
      return -1;
    }
  }
  return 0;
}

/* -- what it all came to --------------------------------------------------- */

static double to_lufs(double power)
{
  if (!(power > 0.0))
  {
    return AUD_LUFS_NONE;
  }
  return LOUD_OFFSET + 10.0 * log10(power);
}

static double from_lufs(double lufs)
{
  return pow(10.0, (lufs - LOUD_OFFSET) / 10.0);
}

static int compare_double(const void *a, const void *b)
{
  double x = *(const double *)a;
  double y = *(const double *)b;

  if (x < y)
  {
    return -1;
  }
  return x > y ? 1 : 0;
}

/* The mean power of every block at or above `floor_lufs`, or a negative number
 * when none of them was. Means rather than medians throughout: the gating in
 * BS.1770 is defined over the power, not over the decibels. */
static double gated_mean(const double *blocks, size_t count, double floor_lufs)
{
  double floor_power = from_lufs(floor_lufs);
  double sum = 0.0;
  size_t kept = 0;

  for (size_t i = 0; i < count; i++)
  {
    if (blocks[i] > floor_power)
    {
      sum += blocks[i];
      kept++;
    }
  }
  return kept > 0 ? sum / (double)kept : -1.0;
}

/*
 * The two-pass gate both figures use, differing only in how far below the
 * take's own average the second pass cuts.
 */
static double gated_loudness(const double *blocks, size_t count, double relative)
{
  double first = gated_mean(blocks, count, LOUD_ABSOLUTE_GATE);
  double second;

  if (!(first > 0.0))
  {
    return AUD_LUFS_NONE;
  }

  second = gated_mean(blocks, count, to_lufs(first) + relative);
  return second > 0.0 ? to_lufs(second) : AUD_LUFS_NONE;
}

/* The loudest block, ungated - a peak rather than an average, so the silence
 * the gate exists to drop cannot be what is loudest anyway. */
static double loudest(const double *blocks, size_t count)
{
  double most = 0.0;

  for (size_t i = 0; i < count; i++)
  {
    if (blocks[i] > most)
    {
      most = blocks[i];
    }
  }
  return to_lufs(most);
}

/*
 * EBU Tech 3342: the spread between the 10th and 95th percentile of the
 * short-term blocks that survive a gate cut 20 LU below their own average.
 *
 * The top is a percentile rather than the maximum because one loud moment is
 * not a range, and the bottom is not the minimum for the same reason at the
 * other end - a fade-out passes through every level there is.
 */
static double range_of(double *blocks, size_t count)
{
  double first = gated_mean(blocks, count, LOUD_ABSOLUTE_GATE);
  double floor_power;
  size_t from = 0;
  size_t kept;
  double low;
  double high;

  if (!(first > 0.0))
  {
    return AUD_LUFS_NONE;
  }
  floor_power = from_lufs(to_lufs(first) + LOUD_RANGE_GATE);

  /*
   * Sorted first and gated afterwards, rather than the other way round. Once
   * the blocks are in order everything above the gate is the tail of the array,
   * so the survivors are found without moving anything - which is what lets
   * this reorder the history it was handed instead of rewriting it, and what
   * makes reading twice give the same answer twice.
   */
  qsort(blocks, count, sizeof(*blocks), compare_double);
  while (from < count && !(blocks[from] > floor_power))
  {
    from++;
  }

  kept = count - from;
  if (kept == 0)
  {
    return AUD_LUFS_NONE;
  }

  low = blocks[from + (size_t)((double)(kept - 1u) * LOUD_RANGE_LOW + 0.5)];
  high = blocks[from + (size_t)((double)(kept - 1u) * LOUD_RANGE_HIGH + 0.5)];
  return to_lufs(high) - to_lufs(low);
}

void aud_loudness_read(aud_loudness *l, aud_loudness_reading *out)
{
  if (out == NULL)
  {
    return;
  }

  out->integrated = AUD_LUFS_NONE;
  out->range = AUD_LUFS_NONE;
  out->momentary_max = AUD_LUFS_NONE;
  out->short_max = AUD_LUFS_NONE;
  out->true_peak = 0.0;

  if (l == NULL)
  {
    return;
  }

  out->integrated = gated_loudness(l->momentary, l->momentary_count, LOUD_RELATIVE_GATE);
  out->momentary_max = loudest(l->momentary, l->momentary_count);
  out->short_max = loudest(l->shortterm, l->shortterm_count);

  /*
   * The group still being filled has not been looked between yet, and on a take
   * that ends on its loudest moment that is exactly where the peak is. Reading
   * it here costs at most eleven windows and disturbs nothing: the buffer is
   * only read, so feeding carries on unaffected, and if those positions are
   * filtered again when the group does complete they can only find the same
   * value a second time.
   */
  for (unsigned c = 0; c < l->channels; c++)
  {
    interpolate(l, l->tp_buf + (size_t)c * 2u * LOUD_TP_TAPS, l->tp_have);
  }
  out->true_peak = l->true_peak;

  /*
   * Last, because it is the one that reorders what it reads. Nothing above
   * depends on the order - every figure here is a mean or a maximum - and
   * feeding can carry on afterwards for the same reason.
   */
  out->range = range_of(l->shortterm, l->shortterm_count);
}

void aud_loudness_read_live(const aud_loudness *l, aud_loudness_live *out)
{
  if (out == NULL)
  {
    return;
  }

  out->momentary = AUD_LUFS_NONE;
  out->short_term = AUD_LUFS_NONE;
  out->integrated = AUD_LUFS_NONE;

  if (l == NULL)
  {
    return;
  }

  /*
   * A block that has not happened yet reads as nothing rather than as silence,
   * which is what to_lufs() makes of a power of zero anyway - so a meter in its
   * first 400 ms and one over digital silence say the same thing, and both of
   * them are right.
   */
  out->momentary = to_lufs(l->momentary_now);
  out->short_term = to_lufs(l->shortterm_now);

  /* gated over the whole history, which no ordering of it can change */
  out->integrated = gated_loudness(l->momentary, l->momentary_count, LOUD_RELATIVE_GATE);
}
