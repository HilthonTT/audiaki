/* SPDX-License-Identifier: MIT */
#include "edit/export.h"

#include "edit/mix.h"
#include "media/ffmpeg.h"
#include "media/wav.h"
#include "util/path.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Frames mixed and written per pass. */
#define EXPORT_CHUNK 8192u

void aud_export_defaults(aud_export_options *opts)
{
  if (opts == NULL)
  {
    return;
  }

  memset(opts, 0, sizeof(*opts));
  /*
   * Every field's default is its zero, `bits` included: zero there means 24,
   * which plan_export() supplies. Filling it in with 24 here would be the same
   * export and a different request - and "a depth was asked for" is a question
   * a lossy target has to be able to answer no to.
   */
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

/* -- which file this is ----------------------------------------------------- */

/*
 * The extensions audiaki writes, and nothing else. A short list on purpose:
 * one that holds the samples as they are, one that holds them losslessly in
 * less room, and two that do not hold them at all but are what the rest of the
 * world plays. Adding another is a row here.
 */
static const struct
{
  const char *ext;
  aud_export_format format;
  const char *name;
} export_formats[] = {
    {".wav", AUD_EXPORT_WAV, "WAV"},
    {".flac", AUD_EXPORT_FLAC, "FLAC"},
    {".opus", AUD_EXPORT_OPUS, "Opus"},
    {".mp3", AUD_EXPORT_MP3, "MP3"},
};

#define EXPORT_FORMAT_COUNT (sizeof(export_formats) / sizeof(export_formats[0]))

/* Whether `ext` is `want`, however it was typed: a name is not shouted. */
static int extension_is(const char *ext, const char *want)
{
  size_t i = 0;

  for (; ext[i] != '\0' && want[i] != '\0'; i++)
  {
    char a = ext[i];

    if (a >= 'A' && a <= 'Z')
    {
      a = (char)(a - 'A' + 'a');
    }
    if (a != want[i])
    {
      return 0;
    }
  }
  return ext[i] == '\0' && want[i] == '\0';
}

/*
 * The extension of `path`'s own last component, or "" when it has none. A dot
 * in a folder name is not an extension, and neither is one that begins the
 * filename - ".wav" is a hidden file called that, not an unnamed WAV.
 */
static const char *extension_of(const char *path)
{
  const char *leaf = aud_path_basename(path);
  const char *dot = strrchr(leaf, '.');

  return (dot != NULL && dot != leaf) ? dot : "";
}

aud_export_format aud_export_format_of(const char *path)
{
  const char *ext;

  if (path == NULL)
  {
    return AUD_EXPORT_UNKNOWN;
  }

  ext = extension_of(path);
  if (*ext == '\0')
  {
    return AUD_EXPORT_WAV; /* a name typed without one means what it always did */
  }

  for (size_t i = 0; i < EXPORT_FORMAT_COUNT; i++)
  {
    if (extension_is(ext, export_formats[i].ext))
    {
      return export_formats[i].format;
    }
  }
  return AUD_EXPORT_UNKNOWN;
}

const char *aud_export_format_name(aud_export_format format)
{
  for (size_t i = 0; i < EXPORT_FORMAT_COUNT; i++)
  {
    if (export_formats[i].format == format)
    {
      return export_formats[i].name;
    }
  }
  return "";
}

int aud_export_format_needs_ffmpeg(aud_export_format format)
{
  return format != AUD_EXPORT_WAV && format != AUD_EXPORT_UNKNOWN;
}

/* Whether the format holds samples at a depth, which only the lossy ones do not. */
static int carries_a_depth(aud_export_format format)
{
  return format == AUD_EXPORT_WAV || format == AUD_EXPORT_FLAC;
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
  aud_export_format format;
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

  p->format = aud_export_format_of(opts->path);
  if (p->format == AUD_EXPORT_UNKNOWN)
  {
    say(why, "that is not a format audiaki writes - try .wav, .flac, .opus or .mp3");
    return -1;
  }

  p->bits = opts->bits != 0 ? opts->bits : 24u;
  if (p->bits != 16u && p->bits != 24u && p->bits != 32u)
  {
    say(why, "that is not a bit depth audiaki writes");
    return -1;
  }
  /*
   * Refused rather than ignored, both ways round. A lossy file holds no samples
   * for a depth to describe, and FLAC holds 24 bits at the most - accepting
   * either request and quietly writing something else is exactly the silence
   * --bits was already fixed for once.
   */
  if (opts->bits != 0 && !carries_a_depth(p->format))
  {
    say(why, "a bit depth is not something that format carries");
    return -1;
  }
  if (p->format == AUD_EXPORT_FLAC && p->bits > 24u)
  {
    say(why, "FLAC holds 24 bits at the most");
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

/* -- where the PCM goes ----------------------------------------------------- */

/*
 * The file being written, which is a WAV writer or a pipe to ffmpeg. The mixing
 * loop below is the same either way and says so by talking to this instead of
 * to one of them: a format is a thing the export writes to, not a second copy
 * of how it mixes.
 */
typedef struct
{
  aud_export_format format;
  wav_writer wav;
  FFMPEG *enc;
} export_sink;

/*
 * Open `path`. `overwrite` is honoured for WAV by the writer; ffmpeg is run
 * with -y and always replaces, so the caller checks first - which both callers
 * here already do, because a set of stems has to be checked as a set anyway.
 *
 * Returns 0, or -1 with `*why` set.
 */
static int sink_open(export_sink *sink, const export_plan *plan, const char *path,
                     unsigned rate, int overwrite, const char **why)
{
  sink->format = plan->format;
  sink->enc = NULL;

  if (plan->format != AUD_EXPORT_WAV)
  {
    if (!overwrite && access(path, F_OK) == 0)
    {
      say(why, "a file of that name is already there");
      return -1;
    }
    sink->enc = ffmpeg_start_encoding(path, rate, plan->channels, plan->bits);
    if (sink->enc == NULL)
    {
      /* ffmpeg.h has said which part of starting it failed */
      say(why, "that file could not be encoded - is ffmpeg installed?");
      return -1;
    }
    return 0;
  }

  /*
   * Large, because a mixdown is the one file that can be longer than anything
   * that went into it: a session of overlaid takes exports as one continuous
   * stretch, and 4 GB is only three and a half hours of it.
   */
  if (wav_open_ex(&sink->wav, path, rate, (uint16_t)plan->channels, (uint16_t)plan->bits,
                  overwrite, NULL, WAV_OPEN_LARGE) != 0)
  {
    say(why, errno == EEXIST ? "a file of that name is already there"
                             : "that file could not be created");
    return -1;
  }
  return 0;
}

static int sink_write(export_sink *sink, const unsigned char *pcm, size_t bytes,
                      const char **why)
{
  if (sink->format != AUD_EXPORT_WAV)
  {
    if (ffmpeg_send_audio(sink->enc, pcm, bytes) != 0)
    {
      say(why, "the export could not be encoded");
      return -1;
    }
    return 0;
  }

  /* only RIFF counts its own size, so only RIFF can run out of room to */
  if (wav_would_overflow(&sink->wav, bytes))
  {
    say(why, "the export reached the 4 GB WAV size limit");
    return -1;
  }
  if (wav_write(&sink->wav, pcm, bytes) != 0)
  {
    say(why, "the export could not be written");
    return -1;
  }
  return 0;
}

static int sink_close(export_sink *sink, const char **why)
{
  if (sink->format != AUD_EXPORT_WAV)
  {
    if (ffmpeg_finish(sink->enc, 0) != 0)
    {
      say(why, "the encoder did not finish - the file may be unusable");
      return -1;
    }
    return 0;
  }

  if (wav_close(&sink->wav) != 0)
  {
    say(why, "the export could not be finished");
    return -1;
  }
  return 0;
}

/* Give up, taking the part-written file with us: half an export looks like a
 * whole one in a directory listing. */
static void sink_discard(export_sink *sink, const char *path)
{
  if (sink->format != AUD_EXPORT_WAV)
  {
    ffmpeg_finish(sink->enc, 1);
    remove(path);
    return;
  }
  wav_discard(&sink->wav);
}

/*
 * Mix `d` to `path` under `plan`. `track` is the one lane to write, or -1 for
 * all of them mixed together - the only difference between a stem and a
 * mixdown, and it is one argument deep rather than a second copy of this.
 */
static int write_mix(const aud_doc *d, const export_plan *plan, long track,
                     const char *path, int overwrite, const char **why)
{
  export_sink sink;
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

  if (sink_open(&sink, plan, path, d->rate, overwrite, why) != 0)
  {
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
      sink_discard(&sink, path);
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
    if (sink_write(&sink, pcm, bytes, why) != 0)
    {
      sink_discard(&sink, path);
      goto out;
    }

    at += want;
  }

  if (sink_close(&sink, why) != 0)
  {
    remove(path);
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
  const char *ext;
  size_t keep;
  int n;

  if (dst == NULL || size == 0 || base == NULL)
  {
    return -1;
  }

  /*
   * The base without its extension, so "mix.wav" gives "mix-01-Rhythm.wav"
   * rather than "mix.wav-01-Rhythm.wav" - and then the extension back on the
   * end, so a set asked for as .flac is a set of FLACs. A base with none gets
   * .wav, which is what a name with no extension means everywhere else here.
   */
  ext = extension_of(base);
  if (*ext == '\0')
  {
    ext = ".wav";
  }
  keep = strlen(base) - strlen(extension_of(base));
  if (keep >= sizeof(stem))
  {
    return -1;
  }
  memcpy(stem, base, keep);
  stem[keep] = '\0';

  tidy_name(tidy, sizeof(tidy), name);

  if (tidy[0] != '\0')
  {
    n = snprintf(full, sizeof(full), "%s-%02zu-%s%s", stem, index + 1u, tidy, ext);
  }
  else
  {
    n = snprintf(full, sizeof(full), "%s-%02zu%s", stem, index + 1u, ext);
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
