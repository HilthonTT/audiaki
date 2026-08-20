/* SPDX-License-Identifier: MIT */
/*
 * track.h - one lane of the timeline, and the clips laid along it.
 *
 * A track is a sorted, non-overlapping list of clips. A clip is a window onto a
 * shared block of audio - see samples.h - placed at some frame of the timeline.
 * The gaps between clips are silence, and they are real: deleting the middle of
 * a take without rippling leaves a hole rather than a shorter track.
 *
 * Everything an edit does is expressed here, and none of it touches a sample.
 * Splitting a clip makes two clips over the same block. Deleting a range moves
 * the windows in. Pasting inserts clips that point at somebody else's audio.
 * That is what makes an undo snapshot affordable, and it is worth keeping: any
 * operation added later should be able to say the same.
 *
 * The invariant the whole file maintains: clips are sorted by `start`, no two
 * overlap, and none is empty. Everything below assumes it and leaves it true.
 */
#ifndef AUDIAKI_EDIT_TRACK_H
#define AUDIAKI_EDIT_TRACK_H

#include "edit/samples.h"

#include <stddef.h>
#include <stdint.h>

#define AUD_TRACK_NAME_MAX 64

/*
 * How far a clip's gain may be turned, linear rather than in decibels.
 *
 * Zero is silence and 16 is +24 dB, which is enough to bring a take recorded
 * far too quietly up to where the rest of the session sits and not enough for
 * a hand-edited project to ask for a number that means nothing. A normalize
 * that needs more than this lands short of its target rather than being
 * refused - see aud_edit_normalize().
 */
#define AUD_CLIP_GAIN_MAX 16.0f

/* Track heights, in pixels. The view reads them; the track owns them, because
 * how tall you have made one lane is a property of that lane. */
#define AUD_TRACK_HEIGHT_MIN 48
#define AUD_TRACK_HEIGHT_DEFAULT 132
#define AUD_TRACK_HEIGHT_MAX 600
#define AUD_TRACK_HEIGHT_COLLAPSED 28

/*
 * A window onto a block of audio, placed on the timeline.
 *
 * `audio` is shared and never written to. `offset` and `frames` say which part
 * of it this clip shows, and `start` says where that part sits. Two clips over
 * the same block, with adjoining windows, are what a split leaves behind.
 *
 * The fades are the one thing here that changes what a sample reads as, and
 * they are still not written into the block: they are lengths, applied on the
 * way out by aud_track_read(). A cut across a note clicks without them, and a
 * fade that had to be baked in would cost the block its immutability and undo
 * its affordability with it.
 *
 * Both are clamped to `frames` and may overlap on a short clip, where the two
 * ramps simply multiply. A split inside a fade truncates it rather than
 * carrying a half-finished ramp into the second half, which the model has no
 * way to say.
 *
 * `gain` is the third of these and the simplest: one number the whole window is
 * multiplied by, applied at the same point and for the same reason. It is what
 * makes an overdub that came in six decibels hot fixable without turning the
 * whole lane down, and what a normalize sets - see aud_edit_normalize(). Held
 * to [0, AUD_CLIP_GAIN_MAX], and 1.0 on every clip that nobody has touched.
 *
 * `muted` is the fourth, and it exists for comping. A gain of zero would say
 * the same thing to the mix, but it would say it by throwing away whatever gain
 * was there - and the whole point of choosing between four passes of the same
 * bar is that you change your mind, put the bar back on pass two, and find pass
 * one exactly as you left it. So it is a flag beside the gain rather than a
 * value of it: nothing is lost, and flipping it back is free.
 *
 * It is deliberately *not* reflected in aud_track_range(), which the fades and
 * the gain are. A comp is a picture of the passes you are choosing between, and
 * a waveform that vanished when you chose the other one would take away the
 * thing you were looking at.
 */
typedef struct
{
  aud_samples *audio;
  size_t offset; /* first frame of `audio` shown */
  size_t frames; /* how many, always more than none */
  uint64_t start;
  size_t fade_in;  /* frames of ramp up from silence at the clip's head */
  size_t fade_out; /* frames of ramp down to silence at its tail */
  float gain;      /* linear, 1.0 for the audio as it was recorded */
  int muted;       /* heard as silence, without forgetting anything above */
} aud_clip;

typedef struct
{
  char name[AUD_TRACK_NAME_MAX];
  unsigned channels;

  int muted;
  int soloed;
  float gain; /* linear, 0 to 2 */
  float pan;  /* -1 hard left to +1 hard right; ignored on a mono output */

  /* how it is shown, which is the track's business and not a parallel table */
  int height;
  int collapsed;
  int selected;

  aud_clip *clips;
  size_t count;
  size_t capacity;

  /* the clip audio is arriving into, or -1. See the recording section below. */
  long recording;
} aud_track;

/* -- lifecycle ------------------------------------------------------------- */

/* Set up an empty track. Returns 0, or -1 if the arguments are unusable. */
int aud_track_init(aud_track *t, const char *name, unsigned channels);

/* Drop every clip and free the list. The track is left usable and empty. */
void aud_track_free(aud_track *t);

/*
 * Copy `src` into `dst`, which must be uninitialised. The clip list is copied
 * and the audio blocks are retained rather than duplicated, so this is cheap
 * whatever the track holds - which is what the undo stack is built on.
 *
 * Returns 0 on success, -1 when the list could not be allocated.
 */
int aud_track_copy(aud_track *dst, const aud_track *src);

/* -- reading --------------------------------------------------------------- */

/* One past the last frame any clip reaches, or 0 for an empty track. */
uint64_t aud_track_end(const aud_track *t);

/* Whether any clip covers `frame`. Gaps and past the end are silence. */
int aud_track_covered(const aud_track *t, uint64_t frame);

/*
 * Whether the clip covering `frame` is silenced. Zero where nothing covers it,
 * because a gap is silence that has not been chosen against - the question this
 * answers is "is this lane the one being heard here", and an empty lane is not
 * an answer to it either way.
 */
int aud_track_muted_at(const aud_track *t, uint64_t frame);

/*
 * The nearest clip boundary strictly after or before `frame`, or `frame` itself
 * when there is none that way.
 *
 * Both ends of every clip count, so a take laid down in one piece has two and a
 * split one has four. These are the places worth landing on: the start of a
 * take, the cut you made in it, the point where it stops. Jumping between them
 * is how a trim gets made without the pointer.
 */
uint64_t aud_track_edge_after(const aud_track *t, uint64_t frame);
uint64_t aud_track_edge_before(const aud_track *t, uint64_t frame);

/*
 * Summarise channel `ch` over timeline frames [from, to) into `out`, across
 * every clip that overlaps it. A span that includes a gap takes in the silence
 * there, which is what makes a hole in a take visible rather than invisible.
 *
 * One call per column is what the waveform is drawn from.
 */
void aud_track_range(const aud_track *t, unsigned ch, uint64_t from, uint64_t to,
                     aud_peak *out);

/*
 * Render `frames` frames of the timeline from `at` into `interleaved`, which
 * must hold frames * t->channels floats. Gaps and the space past the last clip
 * come back as silence. The track's gain and pan are not applied: this is what
 * the track holds, not what the mix makes of it. What each clip says about
 * itself - its fades and its gain - is applied here, because that is part of
 * what the track holds rather than something the mix decides.
 */
void aud_track_read(const aud_track *t, uint64_t at, float *interleaved, size_t frames);

/* -- editing --------------------------------------------------------------- */

/*
 * Place `audio` on the timeline at `start`, as one clip covering all of it.
 * Takes a reference. Returns 0, or -1 when the list could not grow or the
 * placement would overlap a clip already there.
 */
int aud_track_add(aud_track *t, aud_samples *audio, uint64_t start);

/*
 * The general form of the above: place an arbitrary window of `audio`, with
 * fades, rather than all of it. Takes a reference. Used by the project loader,
 * which is rebuilding clips that were saved rather than laying down new ones.
 *
 * Returns 0, or -1 when the window is outside the block, the placement would
 * overlap, or the list could not grow.
 */
int aud_track_place(aud_track *t, aud_samples *audio, size_t offset, size_t frames,
                    uint64_t start, size_t fade_in, size_t fade_out);

/*
 * Ramp the head or tail of whatever clip meets `frame` over `frames`, so a cut
 * across a note stops clicking. `frames` of zero takes the fade off again.
 *
 * aud_edit_fade_in() and its pair are how the window reaches these; they split
 * at the selection's edges first, so there is a clip boundary for the ramp to
 * start or end at. Returns 0 when something was faded, -1 when nothing met the
 * frame.
 */
int aud_track_fade_in_at(aud_track *t, uint64_t frame, size_t frames);
int aud_track_fade_out_at(aud_track *t, uint64_t frame, size_t frames);

/* The gain a clip's fades put on one of its frames, counting from its head. */
float aud_clip_fade_gain(const aud_clip *c, size_t frame);

/*
 * Multiply the gain of every clip inside timeline frames [from, to) by `by`,
 * splitting at both edges first so what changes is exactly the range asked for.
 *
 * Multiplied rather than set, so that turning a piece down twice is twice as
 * quiet and normalizing something already normalized changes nothing. Each
 * clip's result is held to [0, AUD_CLIP_GAIN_MAX], so a range asked for more
 * than the model can say lands at the ceiling rather than being refused.
 *
 * Returns 0 when any clip was touched, -1 when the range held no audio or the
 * clip list could not grow for the splits.
 */
int aud_track_gain_scale(aud_track *t, uint64_t from, uint64_t to, float by);

/*
 * Set the gain of the clip that starts exactly at `frame`, the way
 * aud_track_fade_in_at() sets a ramp there. What the project loader rebuilds a
 * saved clip with. Returns 0, or -1 when no clip starts there.
 */
int aud_track_gain_at(aud_track *t, uint64_t frame, float gain);

/*
 * Silence, or unsilence, every clip inside timeline frames [from, to),
 * splitting at both edges first so what changes is exactly the range asked for.
 *
 * Set rather than toggled, because the caller that matters is comping and it
 * knows which way round it wants every lane - see aud_edit_comp(). Returns 0
 * when any clip was touched, -1 when the range held no audio or the clip list
 * could not grow for the splits.
 */
int aud_track_mute_range(aud_track *t, uint64_t from, uint64_t to, int muted);

/*
 * Set the flag on the clip that starts exactly at `frame`, the way
 * aud_track_gain_at() sets a gain there. What the project loader rebuilds a
 * saved clip with. Returns 0, or -1 when no clip starts there.
 */
int aud_track_mute_at(aud_track *t, uint64_t frame, int muted);

/*
 * Cut any clip that straddles `frame` into two adjoining clips over the same
 * audio. A frame that falls in a gap, on a clip boundary, or outside the track
 * changes nothing.
 *
 * Returns 0 on success, -1 when the list could not grow. Every operation that
 * works on a range starts here, which is why it is the only one that needs to
 * think about clip boundaries at all.
 */
int aud_track_split(aud_track *t, uint64_t frame);

/*
 * Remove timeline frames [from, to).
 *
 * With `ripple`, everything after `to` slides back by the length removed, so
 * the track gets shorter - which is what Delete means when you have selected a
 * mistake. Without it the range becomes a gap, and everything else stays where
 * it was, which is what Silence means.
 *
 * Returns 0 on success, -1 when the list could not grow (a delete can need one
 * more clip than it started with, when it splits one in two).
 */
int aud_track_delete(aud_track *t, uint64_t from, uint64_t to, int ripple);

/*
 * Open `frames` frames of silence at `at`, moving everything from there on
 * later. A clip straddling `at` is split first, so the gap really opens rather
 * than landing inside a clip. Returns 0, or -1 if the list could not grow.
 */
int aud_track_insert_gap(aud_track *t, uint64_t at, uint64_t frames);

/*
 * `frame` moved by `by`, which may be negative. The one place a signed offset
 * meets an unsigned frame, so the arithmetic that has to be careful about it is
 * written once. The caller has established that the answer is in range.
 */
uint64_t aud_frame_offset(uint64_t frame, int64_t by);

/*
 * How far timeline frames [from, to) could move by, given what else is on the
 * track. `want` is the offset asked for and the answer has the same sign and no
 * greater magnitude - it is `want` clamped by whatever the range would run into,
 * and 0 when there is nowhere that way to go.
 *
 * The obstacles are the parts of clips that lie outside [from, to), because
 * those are what stays put while the range moves. A range over the middle of a
 * take therefore has nowhere to go at all: the rest of the take is against both
 * of its edges.
 *
 * Asked before moving rather than discovered while moving, because a move
 * across several tracks has to travel the same distance on each of them - see
 * aud_edit_move_room().
 */
int64_t aud_track_move_room(const aud_track *t, uint64_t from, uint64_t to, int64_t want);

/*
 * Move timeline frames [from, to) along the track by `by` frames, leaving a gap
 * of the same length behind. The clips at either edge are split first, so what
 * moves is exactly the range asked for.
 *
 * Nothing is overwritten: the whole point of the room check is that the landing
 * ground is clear, and a `by` that aud_track_move_room() would have clamped is
 * refused rather than fudged. Nothing is copied either - the clips that move
 * keep the blocks they were reading.
 *
 * Returns 0 on success - including when the range held no audio, which moves
 * silence about and is a success in the same way deleting an empty range is - or
 * -1 when there is no room for `by`, a take is open on the track, or the clip
 * list could not grow.
 */
int aud_track_move(aud_track *t, uint64_t from, uint64_t to, int64_t by);

/*
 * Copy timeline frames [from, to) of `src` into `dst`, which must be
 * uninitialised, with the extracted range starting at frame 0. The audio is
 * shared, not copied. Returns 0 on success, -1 on failure.
 */
int aud_track_extract(const aud_track *src, uint64_t from, uint64_t to, aud_track *dst);

/*
 * Insert `src`'s clips into `t` at `at`, moving whatever was there along by
 * `src`'s length. Returns 0 on success, -1 on failure.
 */
int aud_track_paste(aud_track *t, uint64_t at, const aud_track *src);

/* -- recording into a track ------------------------------------------------- */

/*
 * Open a clip at `start` that will grow as audio arrives, so a take appears on
 * the timeline while it is being played rather than when it is over.
 *
 * The block behind it has one owner - this clip - for as long as the take
 * lasts, which is what makes growing it safe; see samples.h. Nothing else may
 * be added to the track until aud_track_record_end().
 *
 * Returns 0, or -1 when there is no room at `start`, or no memory.
 */
int aud_track_record_begin(aud_track *t, uint64_t start, size_t capacity_hint);

/*
 * Reopen the last clip and carry on growing it, for a take the capture device
 * was pulled out of and which is being continued into the same file.
 *
 * `at` must be exactly where that clip ends, because what is about to arrive
 * is the rest of the same performance and belongs against it rather than near
 * it.
 *
 * One clip rather than two touching ones, and the reason is not cosmetic: the
 * two halves are one file now, and a project stores a clip as an offset into
 * the file it came from. Two clips would both be stamped with that one file
 * and both claim to start at the beginning of it, so reloading the project
 * would play the first half twice.
 *
 * Refused when the last clip is not where `at` says, when a take is already
 * open, or when anything else shares that clip's audio - a block another clip
 * is reading must not change under it.
 *
 * Returns 0, or -1 when the take cannot be carried on, at which point the
 * caller should open a new clip and a new file the way it always did.
 */
int aud_track_record_continue(aud_track *t, uint64_t at);

/*
 * Append `frames` frames of interleaved audio, at the track's channel count,
 * to the clip being recorded into. Returns the number taken, which is short of
 * `frames` only when memory ran out.
 */
size_t aud_track_record_push(aud_track *t, const float *interleaved, size_t frames);

/*
 * Close the take: finish the peak index and let the block go back to being
 * immutable. A track with nothing recorded into it loses the empty clip.
 */
void aud_track_record_end(aud_track *t);

/* Non-zero while a take is open on this track. */
int aud_track_recording(const aud_track *t);

/*
 * Join clips that meet exactly and read consecutively from the same block, and
 * drop any that ended up empty. Purely housekeeping - it changes nothing about
 * what the track sounds like - but without it a session of edits accumulates
 * clip boundaries that are no longer boundaries of anything.
 */
void aud_track_tidy(aud_track *t);

#endif /* AUDIAKI_EDIT_TRACK_H */
