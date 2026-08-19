/* SPDX-License-Identifier: MIT */
/*
 * edit.h - what the edit keys and buttons actually do.
 *
 * Every operation here works on the same thing: the selected time range, across
 * the selected tracks. Each takes a checkpoint before it changes anything, so
 * undo is not something a caller has to remember, and each leaves the selection
 * somewhere sensible for whatever is pressed next.
 *
 * None of them copies audio. They are all clip surgery over shared blocks - see
 * samples.h - which is what makes cutting a forty minute take instant and undo
 * affordable. Anything added here should be able to say the same.
 */
#ifndef AUDIAKI_EDIT_EDIT_H
#define AUDIAKI_EDIT_EDIT_H

#include "edit/doc.h"

#include <stdint.h>

/*
 * What was cut or copied: a slice of the project, tracks and all, with each
 * track's audio shared with wherever it came from. It survives the project it
 * came out of, which is why it holds tracks of its own rather than indices.
 */
typedef struct
{
  aud_track *tracks;
  size_t count;
  unsigned rate;   /* what it was cut from, so a paste can refuse a mismatch */
  uint64_t frames; /* its length, which is what a paste opens room for */
} aud_clipboard;

void aud_clipboard_init(aud_clipboard *c);
void aud_clipboard_clear(aud_clipboard *c);
int aud_clipboard_empty(const aud_clipboard *c);

/*
 * Every operation returns 0 when it did something, and -1 when it could not -
 * nothing selected, nothing on the clipboard, or out of memory. A -1 leaves the
 * project exactly as it was, checkpoint included.
 */

/* Remove the selection and close the gap, so the tracks get shorter. */
int aud_edit_delete(aud_doc *d);

/*
 * Remove the selection and leave a hole of the same length, so everything after
 * it stays where it was. What you want when the timing either side matters and
 * one bar of it does not.
 */
int aud_edit_silence(aud_doc *d);

/* Copy the selection to `c`, leaving the project alone. */
int aud_edit_copy(aud_doc *d, aud_clipboard *c);

/* Copy it and then delete it, closing the gap. */
int aud_edit_cut(aud_doc *d, aud_clipboard *c);

/*
 * Insert the clipboard at the cursor. A selection is replaced by it, the way
 * typing over selected text is. The clipboard's tracks go into the selected
 * tracks in order, and any it has spare become new tracks at the bottom.
 */
int aud_edit_paste(aud_doc *d, const aud_clipboard *c);

/* Throw away everything outside the selection, on the selected tracks. */
int aud_edit_trim(aud_doc *d);

/*
 * Cut the clips at the edges of the selection without removing anything, so
 * the piece can be dragged or deleted on its own later. With no range, cuts at
 * the cursor.
 */
int aud_edit_split(aud_doc *d);

/* Copy the selection into new tracks at the bottom, at the same position. */
int aud_edit_duplicate(aud_doc *d);

/*
 * How far the selection could move by, which is `want` held to whatever the
 * least roomy of the selected tracks would run into - a move that travelled
 * further on one lane than another would take an overdub out of time with the
 * take it was played against.
 *
 * Zero when there is nowhere that way to go, which the window reads as the
 * answer to "may I drop it here": clips do not overlap, so a move onto occupied
 * ground stops against it rather than writing over it.
 */
int64_t aud_edit_move_room(const aud_doc *d, int64_t want);

/*
 * Move the selection along the timeline by `by` frames, on every selected
 * track, leaving a gap where it came from. The selection travels with the audio
 * so that a nudge can be repeated and what moved stays picked out.
 *
 * `by` is clamped by aud_edit_move_room() rather than refused for being too far,
 * so dragging hard against a neighbouring take lands against it. Returns 0 when
 * anything moved, -1 when there was no room at all.
 */
int aud_edit_move(aud_doc *d, int64_t by);

/*
 * Ramp the selection up out of silence, or down into it, on every selected
 * track. The selection's length is the length of the fade, and which end it
 * sits at is which of the two this is.
 *
 * A cut across a note clicks, and these are the answer to that. The samples are
 * not touched: the ramp is a length on the clip, applied on the way out - see
 * track.h. Returns 0 when anything was faded, -1 when nothing met the edge.
 */
int aud_edit_fade_in(aud_doc *d);
int aud_edit_fade_out(aud_doc *d);

/*
 * Turn the selection up or down by `db`, on every selected track.
 *
 * Relative, and it multiplies what the clips already say rather than replacing
 * it, so the key that does this can be leant on: three presses of -1 dB is
 * -3 dB. Like a fade, no sample is touched - it is a number on the clip,
 * applied on the way out - and like a fade it is exactly the selection that
 * gets it, the clips at the edges being split first.
 *
 * A lane that would go past AUD_CLIP_GAIN_MAX stops there. Returns 0 when
 * anything was turned, -1 when there was nothing selected to turn.
 */
int aud_edit_gain(aud_doc *d, double db);

/*
 * What a normalize aims at, which are two different questions.
 *
 * PEAK is about headroom: the loudest point in the selection is put at `level`
 * dBTP and everything else follows it up or down. It is the answer to "use the
 * space that is there" and it says nothing about how loud the result sounds.
 *
 * LOUDNESS is about how loud it sounds: the selection's integrated loudness is
 * put at `level` LUFS, by the BS.1770 measurement --info reports - see
 * audio/loudness.h. It is the answer to "make these two takes sit together",
 * which peak normalizing famously does not, and it is allowed to clip: a take
 * brought up to a loudness target can exceed full scale, and nothing here
 * limits it.
 */
typedef enum
{
  AUD_NORMALIZE_PEAK = 0,
  AUD_NORMALIZE_LOUDNESS
} aud_normalize_target;

/*
 * A dB below full scale to put a peak at, and a LUFS to put a loudness at,
 * for a caller with no reason to choose either.
 *
 * -1 dBTP is the ceiling every delivery specification asks for, and the reason
 * is between the samples: a lossy encoder reconstructs a waveform that goes
 * higher than the samples it was given, so a file mastered to 0 clips on
 * playback and one mastered here does not. -18 LUFS is where a track sits in a
 * mix that has somewhere left to go - a mix of lanes each normalized to a
 * streaming target would be far past full scale before it was mixed.
 */
#define AUD_NORMALIZE_PEAK_DEFAULT (-1.0)
#define AUD_NORMALIZE_LOUDNESS_DEFAULT (-18.0)

/*
 * Measure the selection and set the clip gain that lands it on `level`.
 *
 * One measurement and one figure per lane, not one for all of them: what makes
 * two takes comparable is each of them reaching the target, and a single factor
 * across every selected lane would be a master fader wearing this name. Within
 * a lane the relative levels are untouched, because it is one number over the
 * whole selection rather than one per clip.
 *
 * Measured through the same read the mix uses, so the fades and any gain
 * already there are part of what is measured - which is what makes this
 * idempotent: normalizing something already normalized computes a factor of one.
 *
 * A lane with nothing to measure in the selection - silence, or a range under
 * the 400 ms BS.1770 needs for a loudness - is left alone rather than being
 * multiplied by a guess. Returns 0 when any lane was normalized, -1 when none
 * was: nothing selected, or nothing measurable in what is.
 */
int aud_edit_normalize(aud_doc *d, aud_normalize_target to, double level);

/* Remove track `index` outright. */
int aud_edit_remove_track(aud_doc *d, size_t index);

#endif /* AUDIAKI_EDIT_EDIT_H */
