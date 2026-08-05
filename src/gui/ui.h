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

/* Rows of an open dropdown drawn before it starts scrolling instead. */
#define AUD_UI_DROPDOWN_MAX_ROWS 8

/*
 * A dropdown over `items`. `*selected` is the chosen index and `*open` the
 * caller's open/closed state, both written through. Returns non-zero on the
 * frame the selection changes.
 *
 * The open list is drawn over whatever is beneath it, so this has to be called
 * after the widgets it covers. It does not block their input: a caller with an
 * open menu is expected to disable them itself, which is one line and beats
 * threading a focus stack through an immediate mode interface.
 */
int aud_ui_dropdown(Rectangle bounds, const char *const *items, int count, int *selected,
                    int *open, int enabled);

/* A panel background with a one pixel border. */
void aud_ui_panel(Rectangle bounds);

/* Text helpers that take a colour and an alignment rather than raw positions. */
void aud_ui_text(float x, float y, int size, Color color, const char *text);
void aud_ui_text_right(float right, float y, int size, Color color, const char *text);
void aud_ui_text_centred(Rectangle bounds, int size, Color color, const char *text);

/* Format `seconds` as mm:ss.t into `dst`, which must hold at least 16 bytes. */
void aud_ui_format_clock(char *dst, size_t size, double seconds);

#endif /* AUDIAKI_GUI_UI_H */
