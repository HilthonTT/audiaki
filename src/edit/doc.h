/* SPDX-License-Identifier: MIT */
/*
 * doc.h - the project: the tracks, what is selected, and how to take it back.
 *
 * The model half of the editor. It knows nothing about pixels, raylib or a
 * sound server; the window reads it and asks it for changes, and everything
 * here can be exercised without a window at all.
 *
 * Selection is a time range and a set of tracks, the way it is in every editor
 * that works this way: dragging across two lanes and pressing Delete should
 * take the same seconds out of both. A range of zero length is a cursor, which
 * is what paste and split use.
 *
 * Undo is a stack of whole-project snapshots. That is affordable only because a
 * snapshot shares its audio with the project it came from - see samples.h - so
 * a step costs a copy of the clip lists rather than a copy of the session.
 */
#ifndef AUDIAKI_EDIT_DOC_H
#define AUDIAKI_EDIT_DOC_H

#include "edit/track.h"

#include <stddef.h>
#include <stdint.h>

/*
 * More lanes than anyone is mixing in a window this size, and small enough
 * that a runaway import is caught rather than swallowed.
 */
#define AUD_DOC_MAX_TRACKS 64

/* How far back you can go. Each step is a clip list, not a session's audio. */
#define AUD_DOC_UNDO_DEPTH 64

/* What an undo step is called, for the button that offers to take it back. */
#define AUD_DOC_LABEL_MAX 32

typedef struct
{
  aud_track *tracks;
  size_t count;
  uint64_t cursor;
  uint64_t sel_start;
  uint64_t sel_end;
  char label[AUD_DOC_LABEL_MAX]; /* the edit this was taken before */
} aud_doc_state;

typedef struct
{
  unsigned rate;

  aud_track *tracks;
  size_t count;
  size_t capacity;

  /*
   * The selection. `sel_start == sel_end` is a cursor rather than a range, and
   * `cursor` follows the start so that both readings agree. Which tracks it
   * covers is aud_track.selected, because a track knows whether it is in it.
   */
  uint64_t cursor;
  uint64_t sel_start;
  uint64_t sel_end;

  aud_doc_state undo[AUD_DOC_UNDO_DEPTH];
  aud_doc_state redo[AUD_DOC_UNDO_DEPTH];
  size_t undo_count;
  size_t redo_count;

  /* set whenever something changed that the view has not drawn yet */
  int dirty;
} aud_doc;

/* -- lifecycle ------------------------------------------------------------- */

void aud_doc_init(aud_doc *d, unsigned rate);
void aud_doc_free(aud_doc *d);

/* -- tracks ---------------------------------------------------------------- */

/*
 * Append an empty track and return it, or NULL when there is no room. The
 * pointer is only good until the next add: the list is one array, and it
 * moves. Take the index from aud_doc_index_of() and look it up again.
 */
aud_track *aud_doc_add_track(aud_doc *d, const char *name, unsigned channels);

/* Remove track `index`, freeing what it held. */
void aud_doc_remove_track(aud_doc *d, size_t index);

/* Move track `index` one place up or down the stack. */
void aud_doc_move_track(aud_doc *d, size_t index, int down);

/* One past the last frame any track reaches: the length of the project. */
uint64_t aud_doc_end(const aud_doc *d);

/* Frames of audio held across every track, for the memory readout. */
size_t aud_doc_bytes(const aud_doc *d);

/* -- selection ------------------------------------------------------------- */

/* Put the cursor at `frame` and collapse the selection onto it. */
void aud_doc_set_cursor(aud_doc *d, uint64_t frame);

/* Select [from, to), in either order. A zero-length range is a cursor. */
void aud_doc_select(aud_doc *d, uint64_t from, uint64_t to);

/* Non-zero when the selection has any length to it. */
int aud_doc_has_range(const aud_doc *d);

/* Select every track, or none of them. */
void aud_doc_select_tracks(aud_doc *d, int selected);

/* Non-zero when any track is selected. */
int aud_doc_any_track_selected(const aud_doc *d);

/*
 * Select everything: every track, and from the start to the end of the longest
 * one. What Ctrl+A means, and what several operations fall back to when
 * nothing has been selected at all.
 */
void aud_doc_select_all(aud_doc *d);

/* -- undo ------------------------------------------------------------------ */

/*
 * Remember the project as it is now, under `label`, before changing it. Called
 * by every operation in edit.h before it touches anything; a caller that
 * forgets loses a step of undo rather than corrupting anything.
 *
 * Pushing clears the redo stack, because the future you could have gone back
 * to is not the future any more.
 */
void aud_doc_checkpoint(aud_doc *d, const char *label);

/*
 * Step back, or forward. Returns 0 on success, -1 when there is nothing in
 * that direction.
 */
int aud_doc_undo(aud_doc *d);
int aud_doc_redo(aud_doc *d);

/* What undo or redo would take back, or NULL when there is nothing. */
const char *aud_doc_undo_label(const aud_doc *d);
const char *aud_doc_redo_label(const aud_doc *d);

#endif /* AUDIAKI_EDIT_DOC_H */
