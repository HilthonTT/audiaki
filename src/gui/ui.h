/* SPDX-License-Identifier: MIT */
/*
 * ui.h - the handful of widgets the transport bar needs.
 *
 * raylib draws shapes and text and leaves interface toolkits to someone else,
 * and raygui would be another dependency for four controls. These are
 * immediate mode: each call draws the widget and returns what the user did to
 * it this frame, so there is no widget tree and no state to keep in sync with
 * the engine.
 *
 * Hover and press states are derived from the mouse position each frame, which
 * is all the state a control this simple has.
 */
#ifndef AUDIAKI_GUI_UI_H
#define AUDIAKI_GUI_UI_H

#include "raylib.h"

#include <stddef.h>

/* The palette the whole window is drawn from. */
#define AUD_UI_BG   \
  CLITERAL(Color)   \
  {                 \
    12, 12, 14, 255 \
  }
#define AUD_UI_PANEL \
  CLITERAL(Color)    \
  {                  \
    22, 22, 26, 255  \
  }
#define AUD_UI_EDGE \
  CLITERAL(Color)   \
  {                 \
    44, 44, 52, 255 \
  }
#define AUD_UI_TEXT    \
  CLITERAL(Color)      \
  {                    \
    228, 228, 235, 255 \
  }
#define AUD_UI_MUTED   \
  CLITERAL(Color)      \
  {                    \
    128, 128, 142, 255 \
  }
#define AUD_UI_ACCENT \
  CLITERAL(Color)     \
  {                   \
    88, 168, 255, 255 \
  }
#define AUD_UI_RECORD \
  CLITERAL(Color)     \
  {                   \
    236, 68, 76, 255  \
  }
#define AUD_UI_WARN   \
  CLITERAL(Color)     \
  {                   \
    246, 176, 60, 255 \
  }
#define AUD_UI_OK     \
  CLITERAL(Color)     \
  {                   \
    76, 208, 132, 255 \
  }

/*
 * A button. `tint` colours the label and the border when it is active.
 * Returns non-zero on the frame it is clicked. A disabled button dims itself
 * and never reports a click.
 */
int aud_ui_button(Rectangle bounds, const char *label, Color tint, int enabled);

/*
 * A button that stays lit while `on`. Returns non-zero when clicked, leaving
 * the caller to flip the state it passed in.
 */
int aud_ui_toggle(Rectangle bounds, const char *label, int on, Color tint, int enabled);

/*
 * A horizontal slider over [min, max]. Writes through to *value while it is
 * being dragged and returns non-zero on any frame the value changed.
 *
 * The drag is held until the button is let go, wherever the pointer wanders to
 * in the meantime: a control that stops following the moment you leave its few
 * pixels reads as broken rather than as precise. The wheel nudges it while the
 * pointer is over it, for the correction a drag is a clumsy way to make.
 */
int aud_ui_slider(Rectangle bounds, float *value, float min, float max, Color tint,
                  int enabled);

/*
 * A horizontal level meter, 0.0 to 1.0, that runs green to amber to red.
 * `peak_hold` draws a separate marker; pass a negative value to omit it.
 */
void aud_ui_meter(Rectangle bounds, float level, float peak_hold);

/*
 * A segmented control: `count` labels sharing `bounds`, one of them lit.
 * `*selected` is written through. Returns non-zero on the frame it changes.
 *
 * `dim` draws it faintly until the pointer is over it, for a control that sits
 * on top of something worth looking at and should not compete with it.
 */
int aud_ui_tabs(Rectangle bounds, const char *const *labels, int count, int *selected,
                int enabled, int dim);

/* What a text field reports about the frame it was drawn in. */
#define AUD_UI_FIELD_CLICKED 1   /* the pointer went down on it: give it focus */
#define AUD_UI_FIELD_SUBMITTED 2 /* Enter, while it had focus */
#define AUD_UI_FIELD_EDITED 4    /* the text changed */

/*
 * A single line of editable text, held in `text` and never longer than `size`
 * including its terminator. Returns the AUD_UI_FIELD_* bits above.
 *
 * Focus belongs to the caller rather than to the widget: a dialog knows how
 * many fields it has and which of them Tab should move to, and an immediate
 * mode control redrawn from scratch every frame knows neither. So this only
 * says that it was clicked, and edits when it is told it has focus.
 *
 * Text longer than the box shows its end rather than its beginning, because
 * the end is where the caret is and where a path says which file it means.
 */
int aud_ui_field(Rectangle bounds, char *text, size_t size, int focused, int enabled);

/* The height of one row of a list, which is also how it is scrolled. */
#define AUD_UI_LIST_ROW 26.0f

/*
 * A scrolling list of rows filling `bounds`, with `*scroll` the index of the
 * top visible one, written through. Returns the row clicked this frame, or -1.
 *
 * `marked` is drawn as the current one, or -1 for none. Unlike the dropdown
 * this has no selection of its own: it is used to walk through folders, where
 * clicking a row means "go there" rather than "this one is now chosen".
 */
int aud_ui_list(Rectangle bounds, const char *const *items, int count, int marked,
                int *scroll, int enabled);

/* Rows of an open dropdown drawn before it starts scrolling instead. */
#define AUD_UI_DROPDOWN_MAX_ROWS 8

/*
 * A dropdown over `items`. `*selected` is the chosen index, `*open` the
 * caller's open/closed state and `*scroll` the index of the top visible row,
 * all written through. Returns non-zero on the frame the selection changes.
 *
 * A list longer than AUD_UI_DROPDOWN_MAX_ROWS scrolls on the mouse wheel, and
 * opens showing whatever is currently selected rather than the top - otherwise
 * the entries past the first screenful could never be reached at all.
 *
 * The open list is drawn over whatever is beneath it, so this has to be called
 * after the widgets it covers. It does not block their input: a caller with an
 * open menu is expected to disable them itself, which is one line and beats
 * threading a focus stack through an immediate mode interface.
 */
int aud_ui_dropdown(Rectangle bounds, const char *const *items, int count, int *selected,
                    int *open, int *scroll, int enabled);

/*
 * Hover help. A control asks for its own line while the pointer rests on it,
 * and the one pending gets drawn after every widget rather than beside the one
 * that asked - a tooltip underneath the button next to it would be worse than
 * no tooltip at all.
 *
 * Disabled controls are why this exists. A greyed-out button is a question, and
 * the answer to it should not live in the manual.
 */
void aud_ui_tooltip(Rectangle bounds, const char *text);

/* Draw whatever tooltip was asked for this frame. Called last, exactly once. */
void aud_ui_tooltip_draw(void);

/* Text helpers that take a colour and an alignment rather than raw positions. */
void aud_ui_text(float x, float y, int size, Color color, const char *text);
void aud_ui_text_right(float right, float y, int size, Color color, const char *text);
void aud_ui_text_centred(Rectangle bounds, int size, Color color, const char *text);

/* Format `seconds` as mm:ss.t into `dst`, which must hold at least 16 bytes. */
void aud_ui_format_clock(char *dst, size_t size, double seconds);

#endif /* AUDIAKI_GUI_UI_H */
