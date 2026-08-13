/* SPDX-License-Identifier: MIT */
#include "edit/export.h"

#include "edit/mix.h"
#include "media/wav.h"
#include "util/path.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frames mixed and written per pass. */
#define EXPORT_CHUNK 8192u

void aud_export_defaults(aud_export_options *opts)
{
  if (opts == NULL)
  {
    return;
  }

  memset(opts, 0, sizeof(*opts));
  opts->bits = 24u; /* what audiaki records at, so an export is not a downgrade */
}

/* One float sample as `bits` of little-endian PCM, written into `dst`. */
static void put_sample(unsigned char *dst, float v, unsigned bits)
{
  long scale;
  long value;

  /*
   * Clamped here and nowhere else. The mix deliberately does not, so that a
   * project pushed past full scale reads as too hot rather than as quietly
   * squared off - but a file has to hold something, and wrapping round to the
   * opposite polarity would turn a loud passage into a burst of noise.
   */
  if (v > 1.0f)
  {
    v = 1.0f;
  }
  if (v < -1.0f)
  {
    v = -1.0f;
  }

  switch (bits)
  {
  case 16u:
    scale = 32767L;
    value = (long)(v * (float)scale);
    dst[0] = (unsigned char)(value & 0xff);
    dst[1] = (unsigned char)((value >> 8) & 0xff);
    return;
  case 32u:
    scale = 2147483647L;
    value = (long)((double)v * (double)scale);
    dst[0] = (unsigned char)(value & 0xff);
    dst[1] = (unsigned char)((value >> 8) & 0xff);
    dst[2] = (unsigned char)((value >> 16) & 0xff);
    dst[3] = (unsigned char)((value >> 24) & 0xff);
    return;
  case 24u:
  default:
    scale = 8388607L;
    value = (long)(v * (float)scale);
    dst[0] = (unsigned char)(value & 0xff);
    dst[1] = (unsigned char)((value >> 8) & 0xff);
    dst[2] = (unsigned char)((value >> 16) & 0xff);
    return;
  }
}

static void say(const char **why, const char *text)
{
  if (why != NULL)
  {
    *why = text;
  }
}

/*
 * The rate, width, depth and range one export runs at, worked out from what was
 * asked for and what the project holds.
 *
 * Its own step because a set of stems settles all four once and then writes
 * every file to them. Stems that disagreed on any of the four would not line up
 * when they were laid back down side by side, which is the only thing anybody
 * wants stems for.
 */
typedef struct
{
  unsigned channels;
  unsigned bits;
  uint64_t from;
  uint64_t to;
} export_plan;

static int plan_export(const aud_doc *d, const aud_export_options *opts, export_plan *p,
                       const char **why)
{
  if (d == NULL || opts == NULL || opts->path == NULL || d->rate == 0)
  {
    say(why, "nothing to export");
    return -1;
  }

  p->channels = opts->channels;
  if (p->channels > 2u)
  {
    /*
     * Checked the way the depth below is, and for the same reason: a width the
     * mixer has no pan law for would be written into the header regardless and
     * come back as a file nothing else knows what to do with.
     */
    say(why, "an export is mono or stereo");
    return -1;
  }
  if (p->channels == 0)
  {
    /*
     * The widest track decides, so a stereo take does not come back mono and a
     * project of nothing but mono takes does not gain a duplicate channel.
     */
    p->channels = 1u;
    for (size_t i = 0; i < d->count; i++)
    {
      if (d->tracks[i].channels > p->channels)
      {
        p->channels = d->tracks[i].channels;
      }
    }
    if (p->channels > 2u)
    {
      p->channels = 2u;
    }
  }

  p->bits = opts->bits != 0 ? opts->bits : 24u;
  if (p->bits != 16u && p->bits != 24u && p->bits != 32u)
  {
    say(why, "that is not a bit depth audiaki writes");
    return -1;
  }

  p->from = opts->from;
  p->to = opts->to != 0 ? opts->to : aud_doc_end(d);
  if (p->to <= p->from)
  {
    say(why, "there is nothing in that range");
    return -1;
  }

  return 0;
}

/*
 * Mix `d` to `path` under `plan`. `track` is the one lane to write, or -1 for
 * all of them mixed together - the only difference between a stem and a
 * mixdown, and it is one argument deep rather than a second copy of this.
 */
static int write_mix(const aud_doc *d, const export_plan *plan, long track,
                     const char *path, int overwrite, const char **why)
{
  wav_writer w;
  aud_mixer mix;
  float *mixed = NULL;
  unsigned char *pcm = NULL;
  uint64_t at;
  size_t frame_bytes;
  int rc = -1;

  if (aud_mix_init(&mix, EXPORT_CHUNK) != 0)
  {
    say(why, "not enough memory to mix");
    return -1;
  }

  frame_bytes = (size_t)plan->channels * (plan->bits / 8u);
  mixed = malloc(EXPORT_CHUNK * plan->channels * sizeof(float));
  pcm = malloc(EXPORT_CHUNK * frame_bytes);
  if (mixed == NULL || pcm == NULL)
  {
    say(why, "not enough memory to export");
    goto out;
  }

  /*
   * Large, because a mixdown is the one file that can be longer than anything
   * that went into it: a session of overlaid takes exports as one continuous
   * stretch, and 4 GB is only three and a half hours of it.
   */
  if (wav_open_ex(&w, path, d->rate, (uint16_t)plan->channels, (uint16_t)plan->bits,
                  overwrite, NULL, WAV_OPEN_LARGE) != 0)
  {
    say(why, errno == EEXIST ? "a file of that name is already there"
                             : "that file could not be created");
    goto out;
  }

  for (at = plan->from; at < plan->to;)
  {
    size_t want = (size_t)(plan->to - at);
    size_t bytes;
    int mixed_ok;

    if (want > EXPORT_CHUNK)
    {
      want = EXPORT_CHUNK;
    }

    if (track < 0)
    {
      mixed_ok = aud_mix_read(&mix, d, at, mixed, want, plan->channels);
    }
    else
    {
      mixed_ok =
          aud_mix_read_track(&mix, d, (size_t)track, at, mixed, want, plan->channels);
    }

    if (mixed_ok != 0)
    {
      say(why, "not enough memory to mix");
      wav_discard(&w);
      goto out;
    }

    for (size_t f = 0; f < want; f++)
    {
      for (unsigned ch = 0; ch < plan->channels; ch++)
      {
        put_sample(pcm + (f * plan->channels + ch) * (plan->bits / 8u),
                   mixed[f * plan->channels + ch], plan->bits);
      }
    }

    bytes = want * frame_bytes;
    if (wav_would_overflow(&w, bytes))
    {
      say(why, "the export reached the 4 GB WAV size limit");
      wav_discard(&w);
      goto out;
    }
    if (wav_write(&w, pcm, bytes) != 0)
    {
      say(why, "the export could not be written");
      wav_discard(&w);
      goto out;
    }

    at += want;
  }

  if (wav_close(&w) != 0)
  {
    say(why, "the export could not be finished");
    goto out;
  }

  rc = 0;

out:
  free(pcm);
  free(mixed);
  aud_mix_free(&mix);
  return rc;
}

int aud_export_wav(const aud_doc *d, const aud_export_options *opts, const char **why)
{
  export_plan plan;

  say(why, NULL);

  if (plan_export(d, opts, &plan, why) != 0)
  {
    return -1;
  }

  return write_mix(d, &plan, -1, opts->path, opts->overwrite, why);
}

/* -- stems ------------------------------------------------------------------ */

/*
 * `name` reduced to something every filesystem will take, and something worth
 * reading in a directory listing: letters, digits, dashes and underscores, with
 * everything else - spaces, slashes, accented letters this cannot case-fold -
 * becoming a dash, runs of them collapsing to one, and the ends trimmed.
 *
 * A name that leaves nothing behind gives the empty string, and the caller
 * falls back to the track's number on its own. Losing the name is a cosmetic
 * failure; a track called "../mix" writing outside the folder is not.
 */
static void tidy_name(char *dst, size_t size, const char *name)
{
  size_t n = 0;
  int pending_dash = 0; /* held back, so a trailing run trims itself */

  if (dst == NULL || size == 0)
  {
    return;
  }

  dst[0] = '\0';
  if (name == NULL)
  {
    return;
  }

  for (const char *p = name; *p != '\0' && n + 1u < size; p++)
  {
    unsigned char c = (unsigned char)*p;

    if (isalnum(c) || c == '_')
    {
      if (pending_dash && n > 0)
      {
        dst[n++] = '-';
        if (n + 1u >= size)
        {
          break;
        }
      }
      pending_dash = 0;
      dst[n++] = (char)c;
      continue;
    }

    pending_dash = 1;
  }

  dst[n] = '\0';
}

int aud_export_is_stem(const aud_doc *d, size_t index)
{
  if (d == NULL || index >= d->count)
  {
    return 0;
  }

  return aud_mix_audible(d, &d->tracks[index]) && d->tracks[index].count > 0;
}

int aud_export_stem_path(char *dst, size_t size, const char *base, size_t index,
                         const char *name)
{
  char full[AUD_PATH_MAX];
  char stem[AUD_PATH_MAX];
  char tidy[AUD_EXPORT_STEM_NAME_MAX];
  const char *leaf;
  const char *dot;
  size_t keep;
  int n;

  if (dst == NULL || size == 0 || base == NULL)
  {
    return -1;
  }

  /*
   * The base without its extension, so "mix.wav" gives "mix-01-Rhythm.wav"
   * rather than "mix.wav-01-Rhythm.wav". A dot in a folder name is not an
   * extension, and neither is one that begins the filename.
   */
  leaf = aud_path_basename(base);
  dot = strrchr(leaf, '.');
  keep = (dot != NULL && dot != leaf) ? (size_t)(dot - base) : strlen(base);
  if (keep >= sizeof(stem))
  {
    return -1;
  }
  memcpy(stem, base, keep);
  stem[keep] = '\0';

  tidy_name(tidy, sizeof(tidy), name);

  if (tidy[0] != '\0')
  {
    n = snprintf(full, sizeof(full), "%s-%02zu-%s.wav", stem, index + 1u, tidy);
  }
  else
  {
    n = snprintf(full, sizeof(full), "%s-%02zu.wav", stem, index + 1u);
  }

  if (n < 0 || (size_t)n >= sizeof(full) || (size_t)n >= size)
  {
    return -1;
  }

  memcpy(dst, full, (size_t)n + 1u);
  return 0;
}

int aud_export_stems(const aud_doc *d, const aud_export_options *opts, size_t *written,
                     const char **why)
{
  export_plan plan;
  char path[AUD_PATH_MAX];
  size_t count = 0;
  size_t i;

  if (written != NULL)
  {
    *written = 0;
  }
  say(why, NULL);

  if (plan_export(d, opts, &plan, why) != 0)
  {
    return -1;
  }

  for (i = 0; i < d->count; i++)
  {
    if (!aud_export_is_stem(d, i))
    {
      continue;
    }

    if (aud_export_stem_path(path, sizeof(path), opts->path, i, d->tracks[i].name) != 0)
    {
      say(why, "that would make a filename too long to write");
      break;
    }

    if (write_mix(d, &plan, (long)i, path, opts->overwrite, why) != 0)
    {
      break;
    }

    count++;
  }

  if (i < d->count)
  {
    /*
     * Take back the ones already written. The set is what was asked for, and
     * the stems written before the failure are exactly the lanes below `i` that
     * would have been written at all - nothing was skipped on the way past.
     */
    for (size_t j = 0; j < i; j++)
    {
      if (!aud_export_is_stem(d, j))
      {
        continue;
      }
      if (aud_export_stem_path(path, sizeof(path), opts->path, j, d->tracks[j].name) == 0)
      {
        remove(path);
      }
    }
    return -1;
  }

  if (count == 0)
  {
    say(why, "there is no track here that the mix would hear");
    return -1;
  }

  if (written != NULL)
  {
    *written = count;
  }
  return 0;
}
