/* SPDX-License-Identifier: MIT */
#include "edit/export.h"

#include "edit/mix.h"
#include "media/wav.h"

#include <errno.h>
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

int aud_export_wav(const aud_doc *d, const aud_export_options *opts, const char **why)
{
  wav_writer w;
  aud_mixer mix;
  float *mixed = NULL;
  unsigned char *pcm = NULL;
  unsigned channels;
  unsigned bits;
  uint64_t from;
  uint64_t to;
  uint64_t at;
  size_t frame_bytes;
  int rc = -1;

  say(why, NULL);

  if (d == NULL || opts == NULL || opts->path == NULL || d->rate == 0)
  {
    say(why, "nothing to export");
    return -1;
  }

  channels = opts->channels;
  if (channels == 0)
  {
    /*
     * The widest track decides, so a stereo take does not come back mono and a
     * project of nothing but mono takes does not gain a duplicate channel.
     */
    channels = 1u;
    for (size_t i = 0; i < d->count; i++)
    {
      if (d->tracks[i].channels > channels)
      {
        channels = d->tracks[i].channels;
      }
    }
    if (channels > 2u)
    {
      channels = 2u;
    }
  }

  bits = opts->bits != 0 ? opts->bits : 24u;
  if (bits != 16u && bits != 24u && bits != 32u)
  {
    say(why, "that is not a bit depth audiaki writes");
    return -1;
  }

  from = opts->from;
  to = opts->to != 0 ? opts->to : aud_doc_end(d);
  if (to <= from)
  {
    say(why, "there is nothing in that range");
    return -1;
  }

  if (aud_mix_init(&mix, EXPORT_CHUNK) != 0)
  {
    say(why, "not enough memory to mix");
    return -1;
  }

  frame_bytes = (size_t)channels * (bits / 8u);
  mixed = malloc(EXPORT_CHUNK * channels * sizeof(float));
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
  if (wav_open_ex(&w, opts->path, d->rate, (uint16_t)channels, (uint16_t)bits,
                  opts->overwrite, NULL, WAV_OPEN_LARGE) != 0)
  {
    say(why, errno == EEXIST ? "a file of that name is already there"
                             : "that file could not be created");
    goto out;
  }

  for (at = from; at < to;)
  {
    size_t want = (size_t)(to - at);
    size_t bytes;

    if (want > EXPORT_CHUNK)
    {
      want = EXPORT_CHUNK;
    }

    if (aud_mix_read(&mix, d, at, mixed, want, channels) != 0)
    {
      say(why, "not enough memory to mix");
      wav_discard(&w);
      goto out;
    }

    for (size_t f = 0; f < want; f++)
    {
      for (unsigned ch = 0; ch < channels; ch++)
      {
        put_sample(pcm + (f * channels + ch) * (bits / 8u), mixed[f * channels + ch],
                   bits);
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
