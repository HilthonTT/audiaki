/* SPDX-License-Identifier: MIT */
#include "edit/mix.h"

#include <stdlib.h>
#include <string.h>

int aud_mix_init(aud_mixer *m, size_t frames)
{
  if (m == NULL || frames == 0)
  {
    return -1;
  }

  memset(m, 0, sizeof(*m));
  m->frames = frames;
  return 0;
}

void aud_mix_free(aud_mixer *m)
{
  if (m == NULL)
  {
    return;
  }

  free(m->scratch);
  m->scratch = NULL;
  m->channels = 0;
}

/* Room for one track's worth of `frames` frames at `channels`. */
static int scratch_for(aud_mixer *m, size_t frames, unsigned channels)
{
  float *grown;

  if (m->scratch != NULL && frames <= m->frames && channels <= m->channels)
  {
    return 0;
  }

  if (frames > m->frames)
  {
    m->frames = frames;
  }
  if (channels > m->channels)
  {
    m->channels = channels;
  }

  grown = realloc(m->scratch, m->frames * m->channels * sizeof(float));
  if (grown == NULL)
  {
    return -1;
  }
  m->scratch = grown;
  return 0;
}

int aud_mix_audible(const aud_doc *d, const aud_track *t)
{
  if (d == NULL || t == NULL || t->muted)
  {
    return 0;
  }

  /*
   * Solo is exclusive rather than additive: the moment one track is soloed,
   * every track that is not disappears. That is what solo means on a desk, and
   * it is the only reading under which pressing it on one track tells you
   * anything about that track.
   */
  for (size_t i = 0; i < d->count; i++)
  {
    if (d->tracks[i].soloed)
    {
      return t->soloed;
    }
  }
  return 1;
}

/*
 * How much of a track reaches each side. Centre is unity on both, and a hard
 * pan silences the far side without making the near one louder - turning a
 * track up is what the gain is for, and a pan that changed the level would
 * make balancing two of them a moving target.
 */
static void pan_gains(float pan, float *left, float *right)
{
  if (pan < -1.0f)
  {
    pan = -1.0f;
  }
  if (pan > 1.0f)
  {
    pan = 1.0f;
  }

  *left = pan > 0.0f ? 1.0f - pan : 1.0f;
  *right = pan < 0.0f ? 1.0f + pan : 1.0f;
}

int aud_mix_read(aud_mixer *m, const aud_doc *d, uint64_t at, float *out, size_t frames,
                 unsigned channels)
{
  if (m == NULL || d == NULL || out == NULL || frames == 0 || channels == 0)
  {
    return -1;
  }

  memset(out, 0, frames * channels * sizeof(float));

  for (size_t i = 0; i < d->count; i++)
  {
    const aud_track *t = &d->tracks[i];
    float left;
    float right;

    if (!aud_mix_audible(d, t) || t->count == 0)
    {
      continue;
    }

    /* nothing of this track is anywhere near the window being asked for */
    if (at >= aud_track_end(t))
    {
      continue;
    }

    if (scratch_for(m, frames, t->channels) != 0)
    {
      memset(out, 0, frames * channels * sizeof(float));
      return -1;
    }

    aud_track_read(t, at, m->scratch, frames);
    pan_gains(t->pan, &left, &right);

    /*
     * A mono mix has nowhere to pan to, so panning it would only turn the
     * track down - which is what applying the left leg alone to the one
     * channel would do. See track.h, which says pan is ignored here.
     */
    if (channels == 1u)
    {
      left = 1.0f;
      right = 1.0f;
    }

    left *= t->gain;
    right *= t->gain;

    for (size_t f = 0; f < frames; f++)
    {
      const float *src = m->scratch + f * t->channels;
      float *dst = out + f * channels;

      for (unsigned ch = 0; ch < channels; ch++)
      {
        /*
         * A track narrower than the mix is heard on every channel of it - a
         * mono take belongs in the middle of a stereo mix, not in its left
         * side. A wider one has its extra channels folded onto the last one
         * rather than dropped, which is crude but is at least audible.
         */
        float v;

        if (t->channels == 1u)
        {
          v = src[0];
        }
        else if (ch < t->channels)
        {
          v = src[ch];
        }
        else
        {
          v = src[t->channels - 1u];
        }

        dst[ch] += v * ((ch % 2u) == 0 ? left : right);
      }
    }
  }

  return 0;
}
