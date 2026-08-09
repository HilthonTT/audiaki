/* SPDX-License-Identifier: MIT */
/*
 * samples.h - a block of audio, shared and never changed.
 *
 * The one decision the whole editor is built on. Audio arrives once - from a
 * take that was just recorded, or a WAV that was imported - and from then on
 * nobody writes to it. Everything an edit does is expressed as clips pointing
 * at windows of these blocks, so cutting, splitting, pasting and duplicating
 * move a handful of structs and copy no samples at all.
 *
 * What that buys is undo. A snapshot of the whole project is a copy of its clip
 * lists with the reference counts bumped: a few kilobytes for a session holding
 * hundreds of megabytes of audio. Undo that had to copy the audio would be
 * unaffordable at these sizes, and an editor without undo is not an editor.
 *
 * Interleaved float throughout, which is what everything above wants: the
 * meters, the visualiser and the mix all read floats, and a take's own format
 * has already done its job by the time it reaches here.
 *
 * The one exception to "never changed" is a block still being recorded into.
 * It has exactly one owner while that lasts - the clip at the end of the track
 * being recorded - so growing it can move nothing anybody else is holding. Once
 * the take stops it becomes as immutable as the rest, and everything downstream
 * of that point can go on assuming it always was.
 *
 * No audio system and no drawing, so it is unit tested like the rest.
 */
#ifndef AUDIAKI_EDIT_SAMPLES_H
#define AUDIAKI_EDIT_SAMPLES_H

#include <stddef.h>

/*
 * Frames summarised by one bucket of the fine peak index, and fine buckets per
 * coarse one. 256 is about a pixel's worth of a waveform at a comfortable zoom,
 * and two levels is enough: the coarse buckets summarise 65536 frames each, so
 * an hour of audio is sixteen hundred of them - a screenful either way.
 */
#define AUD_PEAK_BUCKET 256u
#define AUD_PEAK_COARSE 256u

/*
 * What one column of a waveform is drawn from: the loudest and quietest sample
 * over the span it covers, and the root mean square of it.
 *
 * Both, because either on its own misleads. The peaks alone make a quiet take
 * with occasional transients look as loud as a compressed one; the RMS alone
 * hides the transient that clipped. Drawn together - the peak envelope, and the
 * RMS as a solid core inside it - the two say how loud it is and how loud it
 * got, which is what anyone looking at a waveform is asking.
 */
typedef struct
{
  float min;
  float max;
  float rms;
} aud_peak;

/*
 * A reference-counted block of interleaved float audio.
 *
 * Created with a reference count of one, held by whoever created it. Every clip
 * that points at it takes another. It goes away when the last one is dropped,
 * which is what makes a cut cheap: the clip that referred to the audio is gone,
 * and the audio itself is still there for the undo stack to bring back.
 */
typedef struct
{
  int refs;
  unsigned channels;
  size_t frames;   /* how many are real */
  size_t capacity; /* how many there is room for; only differs while recording */
  float *data;     /* channels * capacity, interleaved */

  /*
   * The peak index, built once by aud_samples_index(). Two levels, so a
   * waveform zoomed out to a whole session reads a few thousand buckets rather
   * than a hundred million samples. NULL until it is built, which every reader
   * copes with by scanning the samples instead.
   */
  aud_peak *fine;
  aud_peak *coarse;
  size_t fine_count;   /* buckets per channel */
  size_t coarse_count; /* buckets per channel */
  size_t fine_room;
  size_t coarse_room;
  /*
   * Frames the index actually covers. Always `frames` once a block is finished;
   * short of it while one is being recorded into, because a bucket cannot be
   * summarised until all of it has arrived. Readers scan the samples past this
   * point, which is at most one bucket of them.
   */
  size_t indexed;

  /*
   * The file this block was read from, or NULL when it came from nowhere a
   * reader could go back to.
   *
   * A project file is a list of clips, and a clip is a window onto a block -
   * so saving one is only possible if the block can be found again. Every
   * block in practice has a home: an import has the file it came from, and a
   * take has the WAV the recorder wrote beside it. See project.h.
   */
  char *source;
} aud_samples;

/*
 * Allocate `frames` frames of silence. Returns NULL when the allocation fails
 * or the geometry is unusable, which for a long import is a real possibility
 * and not an assertion.
 */
aud_samples *aud_samples_create(unsigned channels, size_t frames);

/* One more owner. Returns `s`, and tolerates NULL, so it reads well inline. */
aud_samples *aud_samples_retain(aud_samples *s);

/* One fewer owner; frees at zero. Safe on NULL. */
void aud_samples_release(aud_samples *s);

/*
 * Build the peak index. Called once, after the samples have been filled in and
 * before the block is shared with anything that draws it. Silently does
 * nothing if there is no memory for it: the index is an optimisation, and a
 * waveform that has to read the samples is slow rather than wrong.
 */
void aud_samples_index(aud_samples *s);

/*
 * Summarise channel `ch` over frames [from, to) into `out`.
 *
 * Reads whichever level of the index covers the span in a sensible number of
 * steps, and the samples themselves for a span too short to have a bucket. An
 * empty or out-of-range span reads as silence rather than as a failure, because
 * a waveform drawn past the end of a take has to draw something.
 */
void aud_samples_range(const aud_samples *s, unsigned ch, size_t from, size_t to,
                       aud_peak *out);

/* Bytes this block occupies, its index included. For the memory warning. */
size_t aud_samples_bytes(const aud_samples *s);

/*
 * Say where this block came from, so a project can refer to it rather than
 * copy it. A NULL or empty path clears it. Returns 0, or -1 out of memory, in
 * which case the block simply goes on having no source.
 */
int aud_samples_set_source(aud_samples *s, const char *path);

/* Where it came from, or "" when nothing said. Never NULL, so it prints. */
const char *aud_samples_source(const aud_samples *s);

/* -- blocks still being recorded into --------------------------------------- */

/*
 * Create a block with room for `capacity` frames but no frames in it yet, for
 * a take whose length is not known until it stops.
 */
aud_samples *aud_samples_create_empty(unsigned channels, size_t capacity);

/*
 * Make room for `extra` more frames past the ones already there, growing
 * geometrically so a long take does not become a long series of copies.
 *
 * Only legal on a block nobody else is holding - the recorder's own - because
 * it can move the samples. Returns 0, or -1 when the memory is not there.
 */
int aud_samples_reserve(aud_samples *s, size_t extra);

/* Where the next frame goes: the end of what has arrived. */
float *aud_samples_tail(aud_samples *s);

/*
 * Take `frames` more frames as real, and index whichever buckets that
 * completed. Called after writing at aud_samples_tail().
 */
void aud_samples_advance(aud_samples *s, size_t frames);

#endif /* AUDIAKI_EDIT_SAMPLES_H */
