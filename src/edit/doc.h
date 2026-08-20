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

/*
 * For the tempo bounds, and for nothing else. The tempo a project is counted
 * on and the tempo a metronome can play are the same number, and two sets of
 * limits for it would be one set too many.
 */
#include "audio/click.h"

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

/* What a project counts on until someone says otherwise. */
#define AUD_DOC_DEFAULT_TEMPO 120.0

/*
 * A place on the ruler worth coming back to, and what it is called.
 *
 * The grid says where the bars are and a marker says where the second chorus
 * is, which is a different question: one is arithmetic from the tempo and the
 * other is something only the person playing knows. Forty-eight characters is
 * enough for "solo - come in late" and short enough to draw on a ruler beside
 * a dozen others.
 *
 * More of them than any session has, and few enough that a hand-edited project
 * asking for thousands is refused rather than believed.
 */
#define AUD_MARKER_NAME_MAX 48
#define AUD_DOC_MAX_MARKERS 512

typedef struct
{
  uint64_t at;
  char name[AUD_MARKER_NAME_MAX];
} aud_marker;

typedef struct
{
  aud_track *tracks;
  size_t count;
  uint64_t cursor;
  uint64_t sel_start;
  uint64_t sel_end;

  /*
   * The markers as they stood, because an edit that ripples moves them - see
   * aud_doc_markers_ripple(). Undoing the edit has to put them back where they
   * were or the ruler ends up describing audio that has been taken away.
   */
  aud_marker *markers;
  size_t marker_count;

  char label[AUD_DOC_LABEL_MAX]; /* the edit this was taken before */
} aud_doc_state;

typedef struct
{
  unsigned rate;

  /*
   * The tempo the project is counted on: what the ruler draws its bars from,
   * what the metronome plays, and what the pointer snaps to. A property of the
   * session rather than of the view, so it is saved with it - two people
   * opening the same project should see the same bar lines.
   *
   * Not part of an undo step. Everything on the undo stack is audio moving
   * about, and the tempo moves none: changing it redraws the grid and leaves
   * every sample where it was.
   */
  double tempo;
  unsigned beats_per_bar; /* 0 or 1 for a bare pulse with no bar to it */

  /*
   * How finely the grid is divided, which is what an edit lands on: 0 is whole
   * bars, 1 is beats, and anything above is that many to a beat - 2 for
   * eighths, 3 for triplets, 4 for sixteenths.
   *
   * A property of the session for the same reason the tempo is, and not part of
   * an undo step for the same reason either: changing it moves no audio, it
   * only changes where the next edit will land.
   */
  unsigned grid_div;

  aud_track *tracks;
  size_t count;
  size_t capacity;

  /*
   * Places on the ruler, sorted by where they are and never two at one frame.
   *
   * A property of the session like the tempo is, and saved with it - but unlike
   * the tempo these are part of an undo step, because an edit that shortens the
   * timeline moves them and undoing it has to move them back.
   */
  aud_marker *markers;
  size_t marker_count;
  size_t marker_capacity;

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

/* -- the tempo ------------------------------------------------------------- */

/*
 * Set the tempo, held to what click.h will play. A `bpm` outside those bounds
 * is clamped rather than refused: this is driven by a spinner and by a
 * hand-edited project file, and neither wants an error for an answer.
 */
void aud_doc_set_tempo(aud_doc *d, double bpm, unsigned beats_per_bar);

/*
 * Frames between one beat and the next, as a real number so a tempo that does
 * not divide the sample rate does not drift a frame per bar. Zero when there
 * is no usable tempo, which is the caller's cue that there is no grid.
 */
double aud_doc_beat_frames(const aud_doc *d);

/* The same for a bar. Equal to a beat when the project is a bare pulse. */
double aud_doc_bar_frames(const aud_doc *d);

/*
 * What `grid_div` may be. The ceiling is the metronome's, so the grid can be
 * divided exactly as finely as the click can count it out.
 */
#define AUD_DOC_GRID_BAR 0u
#define AUD_DOC_GRID_BEAT 1u
#define AUD_DOC_GRID_MAX AUD_CLICK_SUBDIV_MAX

/* Set the division, clamped to AUD_DOC_GRID_MAX. */
void aud_doc_set_grid(aud_doc *d, unsigned div);

/*
 * Frames between one grid line and the next, which is a bar, a beat or a
 * fraction of one depending on `grid_div`. Zero when there is no usable tempo,
 * which is the caller's cue that there is no grid.
 */
double aud_doc_grid_frames(const aud_doc *d);

/*
 * How to say the current division out loud, e.g. "bars", "beats" or
 * "1/3 beat". Never NULL, and points at static storage.
 */
const char *aud_doc_grid_label(const aud_doc *d);

/*
 * The nearest grid line to `frame`, or `frame` itself when there is no grid.
 * What "snap to the grid" means, in the one place that decides it.
 */
uint64_t aud_doc_snap(const aud_doc *d, uint64_t frame);

/*
 * The grid line one step before or after `frame`, for a key that walks the
 * grid rather than pointing at it. Always moves: sitting exactly on a line
 * steps off it rather than returning it again. `frame` itself when there is no
 * grid, which the caller should read as "nothing to step to".
 */
uint64_t aud_doc_grid_step(const aud_doc *d, uint64_t frame, int back);

/* -- markers --------------------------------------------------------------- */

/*
 * Put a marker at `at`, called `name` - which may be NULL or empty for one that
 * is only a place. Returns its index, or -1 when there is no room for another.
 *
 * A marker already at that exact frame is renamed rather than joined by a
 * second: two markers at one place would draw on top of each other and there
 * would be no way to pick either.
 */
long aud_doc_mark(aud_doc *d, uint64_t at, const char *name);

/* Take marker `index` away. An index past the end changes nothing. */
void aud_doc_unmark(aud_doc *d, size_t index);

/* Take every marker away. */
void aud_doc_clear_markers(aud_doc *d);

/* The marker at exactly `at`, or -1 when there is none there. */
long aud_doc_marker_at(const aud_doc *d, uint64_t at);

/*
 * The marker nearest `at` and no further from it than `within`, or -1.
 *
 * What a click on the ruler lands on, and what tells a key that drops a marker
 * that it is being pressed on one that is already there. Ties go to the earlier
 * one, so a pointer exactly between two behaves the same way twice.
 */
long aud_doc_marker_near(const aud_doc *d, uint64_t at, uint64_t within);

/*
 * The marker one before or one after `frame`, or `frame` itself when there is
 * none that way - which the caller should read as "nothing to step to", the
 * same answer aud_doc_grid_step() gives.
 *
 * Always moves: sitting exactly on a marker steps off it rather than returning
 * it again, so the key can be leant on.
 */
uint64_t aud_doc_marker_step(const aud_doc *d, uint64_t frame, int back);

/*
 * Move every marker from `at` on by `by` frames, and drop any that lands
 * before `at` - which is what a delete does to the markers inside the range it
 * took away.
 *
 * Called by the edits that change how long the timeline is, and only when they
 * changed it for every lane. A ripple on some of the lanes and not others
 * leaves the project no shorter than it was, and moving the ruler for it would
 * put every marker in the session wrong to fix one of them.
 */
void aud_doc_markers_ripple(aud_doc *d, uint64_t at, int64_t by);

/* -- selection ------------------------------------------------------------- */

/* Put the cursor at `frame` and collapse the selection onto it. */
void aud_doc_set_cursor(aud_doc *d, uint64_t frame);

/* Select [from, to), in either order. A zero-length range is a cursor. */
void aud_doc_select(aud_doc *d, uint64_t from, uint64_t to);

/*
 * The same, said as an anchor and the end being moved. The cursor is left on
 * the anchor rather than on the lower of the two, which is what lets a run of
 * keystrokes go on growing the selection from the end it started at - and lets
 * the next one tell which end that was.
 */
void aud_doc_select_from(aud_doc *d, uint64_t anchor, uint64_t edge);

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
