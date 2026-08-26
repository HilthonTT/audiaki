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

/*
 * The palette the whole window is drawn from.
 *
 * Four neutrals rather than two, because depth is what tells a control from
 * the thing it sits on: the window is BG, a panel laid on it is PANEL, a
 * control raised off that panel is SURFACE, and SURFACE_HI is the same control
 * with the pointer over it. Nothing here is pure grey - the whole ramp is
 * pulled a little towards blue so that the accent and the level colours read as
 * colour rather than as the only thing on the screen that has any.
 */
#define AUD_UI_BG   \
  CLITERAL(Color)   \
  {                 \
    11, 12, 16, 255 \
  }
#define AUD_UI_PANEL \
  CLITERAL(Color)    \
  {                  \
    20, 22, 29, 255  \
  }
#define AUD_UI_SURFACE \
  CLITERAL(Color)      \
  {                    \
    30, 33, 43, 255    \
  }
#define AUD_UI_SURFACE_HI \
  CLITERAL(Color)         \
  {                       \
    41, 45, 58, 255       \
  }
#define AUD_UI_EDGE \
  CLITERAL(Color)   \
  {                 \
    54, 59, 76, 255 \
  }
/* The line between two things that belong together, as opposed to two that do
 * not: a lane and the next lane, rather than a panel and the window. */
#define AUD_UI_EDGE_SOFT \
  CLITERAL(Color)        \
  {                      \
    34, 37, 48, 255      \
  }
#define AUD_UI_TEXT    \
  CLITERAL(Color)      \
  {                    \
    232, 235, 242, 255 \
  }
#define AUD_UI_MUTED   \
  CLITERAL(Color)      \
  {                    \
    148, 155, 176, 255 \
  }
/* Text that is there to be found rather than to be read: counts, sizes, units. */
#define AUD_UI_FAINT  \
  CLITERAL(Color)     \
  {                   \
    97, 104, 124, 255 \
  }
#define AUD_UI_ACCENT \
  CLITERAL(Color)     \
  {                   \
    91, 157, 255, 255 \
  }
#define AUD_UI_RECORD \
  CLITERAL(Color)     \
  {                   \
    242, 85, 95, 255  \
  }
#define AUD_UI_WARN   \
  CLITERAL(Color)     \
  {                   \
    245, 178, 60, 255 \
  }
#define AUD_UI_OK     \
  CLITERAL(Color)     \
  {                   \
    53, 211, 153, 255 \
  }

/* -- lettering -------------------------------------------------------------- */

/*
 * The three faces the window letters in. They are found on the machine at
 * startup and fall back to the one raylib carries, so this adds no file to
 * install and no dependency to build against - but on any desktop with fonts
 * on it the window is drawn in a real typeface rather than in a bitmap.
 */
typedef enum
{
  AUD_UI_SANS = 0, /* everything that is words */
  AUD_UI_STRONG,   /* what a control is called, and headings */
  AUD_UI_MONO,     /* clocks, decibels, anything whose digits change in place */
  AUD_UI_FACE_COUNT
} aud_ui_face;

/*
 * Let the faces go. Called on the way out and before a hot reload: they are
 * textures on the card, and the library that made them is the one that must
 * free them. The next call to draw anything finds them again.
 */
void aud_ui_fonts_release(void);

/* How wide `text` is in `face` at `size`, which is what a toolbar lays out from. */
float aud_ui_measure(aud_ui_face face, const char *text, int size);

/*
 * The size buttons and toggles letter their labels at from here on, or 0 for
 * the default. A label never overflows its control whatever this says - it is
 * drawn a size down, and then a size down again, and cut short with an ellipsis
 * once there is nothing left to give. This is how a toolbar that has worked out
 * it must pack tighter than usual says so for the whole row at once, so that
 * every button in it is lettered the same.
 */
void aud_ui_label_size(int size);

/*
 * The glyphs a transport button can carry ahead of its label. They are drawn
 * from shapes rather than lettered, because there is no character for "record"
 * in a font that is guaranteed to be on the machine - and because a triangle
 * and a disc are what every transport in the world has used for fifty years,
 * which is worth more here than any word.
 */
typedef enum
{
  AUD_UI_ICON_NONE = 0,
  AUD_UI_ICON_PLAY,
  AUD_UI_ICON_RECORD,
  AUD_UI_ICON_PAUSE,
  AUD_UI_ICON_STOP,
  AUD_UI_ICON_LOOP,
  /* and the four that are not about the transport at all: a drawer that is
   * open or shut, a lane that is folded away, and a thing to be closed */
  AUD_UI_ICON_OPEN,
  AUD_UI_ICON_SHUT,
  AUD_UI_ICON_FOLD,
  AUD_UI_ICON_CLOSE
} aud_ui_icon;

/* The room a glyph and its gap ask for beside a label lettered at `font`. */
float aud_ui_icon_width(int font);

/*
 * A button. `tint` colours the label and the border when it is active.
 * Returns non-zero on the frame it is clicked. A disabled button dims itself
 * and never reports a click.
 */
int aud_ui_button(Rectangle bounds, const char *label, Color tint, int enabled);

/* The same button with a glyph ahead of the label; see aud_ui_icon. */
int aud_ui_button_icon(Rectangle bounds, aud_ui_icon icon, const char *label, Color tint,
                       int enabled);

/* And the same toggle. */
int aud_ui_toggle_icon(Rectangle bounds, aud_ui_icon icon, const char *label, int on,
                       Color tint, int enabled);

/*
 * A button with no plate under it: a label that lights when the pointer is on
 * it and nothing at all when it is not. For the parts of a compound control -
 * the two steps either side of a reading - which are buttons but are not
 * separate things, and would say they were if each had a border of its own.
 */
int aud_ui_ghost(Rectangle bounds, const char *label, Color tint, int enabled);

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

/*
 * Text helpers that take a colour and an alignment rather than raw positions.
 * The aud_ui_write family names the face; aud_ui_text and its two are the same
 * calls in AUD_UI_SANS, which is what most of the window wants.
 */
void aud_ui_write(aud_ui_face face, float x, float y, int size, Color color,
                  const char *text);
void aud_ui_write_right(aud_ui_face face, float right, float y, int size, Color color,
                        const char *text);
void aud_ui_write_centred(aud_ui_face face, Rectangle bounds, int size, Color color,
                          const char *text);

void aud_ui_text(float x, float y, int size, Color color, const char *text);
void aud_ui_text_right(float right, float y, int size, Color color, const char *text);
void aud_ui_text_centred(Rectangle bounds, int size, Color color, const char *text);

/* -- the shapes everything else is built from ------------------------------- */

/*
 * A rounded rectangle with a border, at a radius in pixels rather than the
 * fraction of its own height raylib asks for - which is the wrong unit for a
 * window where a 20 pixel button and a 200 pixel panel should have the same
 * corner. Either colour may be blank to leave that half undrawn.
 */
void aud_ui_panel(Rectangle bounds, float radius, Color fill, Color edge);

/*
 * The soft dark spread under something that floats: a menu, a dialog, a
 * tooltip. Drawn before the thing itself. It is what says the panel is over the
 * window rather than cut out of it, and it is the one bit of depth here that is
 * not a border.
 */
void aud_ui_shadow(Rectangle bounds, float radius, float spread);

/* The thin vertical rule that separates one group of buttons from the next. */
void aud_ui_rule(float x, float y, float height, Color color);

/* Format `seconds` as mm:ss.t into `dst`, which must hold at least 16 bytes. */
void aud_ui_format_clock(char *dst, size_t size, double seconds);

#endif /* AUDIAKI_GUI_UI_H */
