/* SPDX-License-Identifier: MIT */
/*
 * mix.h - what the project sounds like.
 *
 * One function, and everything that needs to hear the timeline goes through it:
 * playback, and exporting. That is the point of it being its own file - a
 * project that played back differently from how it exported would be a project
 * you could not trust, and there is only one way to be sure of that.
 *
 * Mute, solo, gain and pan are applied here rather than by the tracks
 * themselves, because they describe how a track sits in a mix rather than what
 * it holds - see track.h, whose reader deliberately ignores all four.
 */
#ifndef AUDIAKI_EDIT_MIX_H
#define AUDIAKI_EDIT_MIX_H

#include "edit/doc.h"

/*
 * Scratch space for one call. Kept by the caller so a mix in a playback loop
 * allocates nothing: it is handed one buffer at the start and reuses it.
 */
typedef struct
{
  float *scratch;
  size_t frames;
  unsigned channels; /* what `scratch` has room for per frame */
} aud_mixer;

/*
 * Set up a mixer for up to `frames` frames at a time. Returns 0, or -1 when
 * there is no memory for it.
 */
int aud_mix_init(aud_mixer *m, size_t frames);

void aud_mix_free(aud_mixer *m);

/*
 * Mix `frames` frames of the project from `at` into `out`, which holds
 * frames * channels interleaved floats.
 *
 * A mono track reaches both sides of a stereo mix; a stereo track keeps its
 * sides. Nothing is clamped - what comes out is what the tracks add up to, and
 * a mix that is too hot should read as too hot rather than as quietly squared
 * off. Whoever turns it into samples clamps.
 *
 * Returns 0 on success, -1 when the scratch would not stretch to it, having
 * left `out` silent rather than half mixed.
 */
int aud_mix_read(aud_mixer *m, const aud_doc *d, uint64_t at, float *out, size_t frames,
                 unsigned channels);

/*
 * Whether anything would be heard from `t` at all: not muted, and soloed if
 * anything is. Exposed because the view greys out what it would not play.
 */
int aud_mix_audible(const aud_doc *d, const aud_track *t);

#endif /* AUDIAKI_EDIT_MIX_H */
