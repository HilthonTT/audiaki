/* SPDX-License-Identifier: MIT */
/*
 * repair.h - the spectrum of what was recorded, and drawing on it.
 *
 * The other panel in the drawer. viz.h shows what is arriving from the
 * interface right now and is gone a frame later; this shows what is already on
 * the timeline, holds still, and can be edited.
 *
 * The graph is the edit. There is no separate list of filters to keep in step
 * with what is drawn: the line on screen is aud_spectral's gain curve, drawing
 * on it writes to that curve, and applying multiplies the audio by it - see
 * audio/spectral.h. What that buys is that the obvious gesture is the right
 * one. A hum is a spike standing out of the floor around it, so you drag the
 * spike down to the floor and it is gone.
 *
 * Three ways in, because a spike you can see is not the only kind of noise:
 *
 *   drag        pull any part of the spectrum down to where the pointer is
 *   Find hum    the fundamental of a steady buzz, and its harmonics in one go
 *   the profile a noise floor learned from a quiet stretch, subtracted from
 *               the whole take - for hiss, which is everywhere and has no
 *               spike to point at
 *
 * Nothing here changes the project until Apply is pressed, and Apply is one
 * step of undo like any other edit - see edit/repair.h, which is what it calls.
 * Everything up to that point is a picture and a curve.
 */
#ifndef AUDIAKI_GUI_REPAIR_H
#define AUDIAKI_GUI_REPAIR_H

#include "audio/spectral.h"
#include "edit/doc.h"

#include "raylib.h"

/* What the graph covers. The top of a bass guitar's fundamental range is 100
 * Hz or so, and the hum under it is lower still, so the axis starts low. */
#define AUD_REPAIR_MIN_HZ 20.0
#define AUD_REPAIR_TOP_DB 0.0f
#define AUD_REPAIR_FLOOR_DB (-100.0f)

/* How wide the brush is, as a fraction of an octave, and what it opens at. */
#define AUD_REPAIR_BRUSH_MIN 0.02f
#define AUD_REPAIR_BRUSH_MAX 1.0f
#define AUD_REPAIR_BRUSH_DEFAULT 0.12f

/* Harmonics the Notch button reaches, and what it offers to start with. */
#define AUD_REPAIR_HARMONICS_MAX 12
#define AUD_REPAIR_HARMONICS_DEFAULT 6

/* What the panel says about the last thing that happened in it. */
#define AUD_REPAIR_NOTE_MAX 160

/*
 * Seconds a changed selection has to hold still before the spectrum is read
 * again. Dragging a selection across a take changes it every frame, and
 * re-reading a forty minute range sixty times a second would make the drag
 * that asked for it stutter. A sixth of a second is under the time it takes to
 * let go of a mouse button and long enough that only the range settled on gets
 * read.
 */
#define AUD_REPAIR_SETTLE 0.16

typedef struct
{
  /*
   * The analysis and the curve. Created for the project's sample rate and
   * thrown away when that changes, which is only when a session is opened.
   * NULL until there is something to look at.
   */
  aud_spectral *sp;

  /*
   * What the reading currently on screen was taken from. Kept so the audio is
   * only read again when the answer would differ - see AUD_REPAIR_SETTLE.
   */
  int have;
  size_t track;
  uint64_t from;
  uint64_t to;
  size_t edits; /* undo depth, so an undo is noticed as well as a selection */
  size_t clips;

  /* a re-read waiting for the selection to stop moving */
  int pending;
  double settle_at;

  /* the drag, which has to outlive the frame it started in */
  int painting;
  int lifting;
  double last_hz;

  float brush;
  float hum_hz; /* what Find offered, or 0 */
  float harmonics;

  /* the predicted spectrum, redrawn every frame from the curve */
  float *result;
  size_t result_bins;

  /*
   * Apply has been pressed and nothing has happened yet.
   *
   * The panel asks rather than acts because the answer is not its to give: a
   * repair rewrites audio and leaves a file behind, and whether that is worth
   * doing is a question for the window, which owns the dialog that asks it -
   * see app_confirm_apply(). The window clears this and calls
   * aud_repair_panel_apply() if the answer was yes.
   */
  int apply_wanted;

  char note[AUD_REPAIR_NOTE_MAX];
} aud_repair_panel;

void aud_repair_panel_init(aud_repair_panel *p);
void aud_repair_panel_free(aud_repair_panel *p);

/*
 * Throw the reading away, so the next frame takes it again. Called when
 * something happened to the audio that the panel cannot see from the selection
 * alone - a take landing on the timeline, a project opening.
 */
void aud_repair_panel_reset(aud_repair_panel *p);

/*
 * Draw into `area` and carry out whatever the pointer did. `enabled` is
 * cleared when something is over the top of the window.
 *
 * Pressing Apply sets `apply_wanted` rather than doing anything; nothing here
 * changes the project. The caller answers that and calls the function below.
 */
void aud_repair_panel_draw(aud_repair_panel *p, aud_doc *d, Rectangle area, int enabled);

/*
 * Put the curve through the audio the reading was taken from, writing the
 * result into `dir`. What Apply meant, once it has been agreed to.
 *
 * The range is the one the reading was taken over rather than whatever is
 * selected now, so the audio that gets cleaned up is the audio that was on the
 * graph when the button was pressed.
 *
 * Returns non-zero when the project changed. Either way it says what happened
 * in `note`.
 */
int aud_repair_panel_apply(aud_repair_panel *p, aud_doc *d, const char *dir);

/* Seconds the reading covers, for the question that asks about it. */
double aud_repair_panel_seconds(const aud_repair_panel *p, const aud_doc *d);

/* The lane it was taken from, or "" when there is not one. Never NULL. */
const char *aud_repair_panel_track(const aud_repair_panel *p, const aud_doc *d);

#endif /* AUDIAKI_GUI_REPAIR_H */
