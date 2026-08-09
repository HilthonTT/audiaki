/* SPDX-License-Identifier: MIT */
/*
 * timeline.h - the tracks, drawn and driven.
 *
 * The editor's whole surface: the time ruler, the per-track control column, the
 * waveforms and everything you can do to them with a pointer. It reads and
 * writes an aud_doc and knows nothing else about the app, so the model can be
 * exercised without it and it can be moved without disturbing the model.
 *
 * What lives here rather than in the document is what the document does not
 * care about: how far in you are zoomed, how far along you have scrolled, and
 * which drag is in progress. A project looks the same however you were looking
 * at it.
 */
#ifndef AUDIAKI_GUI_TIMELINE_H
#define AUDIAKI_GUI_TIMELINE_H

#include "edit/doc.h"

#include "raylib.h"

/* The control column beside each track: name, mute, solo, gain, pan. */
#define AUD_TIMELINE_PANEL_W 168.0f

/* The strip of amplitude labels between the panel and the waveform. */
#define AUD_TIMELINE_SCALE_W 34.0f

/* How far in and out the zoom goes, in pixels per second of audio. */
#define AUD_TIMELINE_ZOOM_MIN 0.05
#define AUD_TIMELINE_ZOOM_MAX 12000.0
#define AUD_TIMELINE_ZOOM_DEFAULT 40.0

/* One notch of the wheel, or one press of the zoom buttons. */
#define AUD_TIMELINE_ZOOM_STEP 1.3

typedef struct
{
  double zoom;   /* pixels per second */
  double scroll; /* the second at the left edge of the waveform area */
  float rows;    /* pixels the track stack is scrolled down by */

  /*
   * Which drag is in progress, if any. Immediate mode has no widget to hold
   * this, and a drag is the one thing that has to outlive the frame it started
   * in - a selection that stopped following the pointer the moment it left the
   * lane it began in would read as broken.
   */
  int selecting;
  uint64_t anchor; /* the frame the selection drag started at */
  int resizing;    /* a track's bottom edge is being dragged */
  size_t resize_track;
  int resize_from_h;
  float resize_from_y;
  int scrubbing; /* the ruler is being dragged */

  /*
   * Pixels of lane on screen, as of the last draw. Kept because the keyboard
   * has to scroll a track into view and only the drawing knows how much room
   * the lanes ended up with.
   */
  float rows_h;

  /* what the pointer is over, for the status line to explain */
  char hint[96];
} aud_timeline;

void aud_timeline_init(aud_timeline *tl);

/*
 * Draw the ruler and the tracks, and carry out whatever the pointer did to
 * them. `ruler` and `area` are the two rectangles they occupy, `playhead` is
 * where playback has reached (or the document's cursor when nothing is
 * playing), and `enabled` is cleared when something is over the top of it all.
 */
void aud_timeline_draw(aud_timeline *tl, aud_doc *d, Rectangle ruler, Rectangle area,
                       uint64_t playhead, int playing, int enabled);

/* Zoom about `at_seconds`, keeping that moment under the same pixel. */
void aud_timeline_zoom_at(aud_timeline *tl, double factor, double at_seconds,
                          float width);

/* Zoom so the whole project fits in `width` pixels of waveform. */
void aud_timeline_fit(aud_timeline *tl, const aud_doc *d, float width);

/* Zoom so the selection fills `width`; falls back to fitting the project. */
void aud_timeline_fit_selection(aud_timeline *tl, const aud_doc *d, float width);

/* Scroll so `frame` is visible, nudging as little as possible to get it there. */
void aud_timeline_reveal(aud_timeline *tl, const aud_doc *d, uint64_t frame, float width);

/*
 * The same up the stack: scroll so track `index` is on screen, given `height`
 * pixels of room for the lanes. Selecting a track with the keyboard has to
 * bring it into view, or the selection moves somewhere you cannot see.
 */
void aud_timeline_reveal_track(aud_timeline *tl, const aud_doc *d, size_t index,
                               float height);

/* Seconds to a x offset within the waveform area, and back. */
float aud_timeline_x_of(const aud_timeline *tl, double seconds);
double aud_timeline_seconds_at(const aud_timeline *tl, float x);

#endif /* AUDIAKI_GUI_TIMELINE_H */
